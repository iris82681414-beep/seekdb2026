# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""HTTP score-based reranking with global ranks across bounded batches."""

from __future__ import annotations

import math

import httpx

from powercontext.builtin.artifacts.memory.models import MemoryHit
from powercontext.builtin.artifacts.memory.reranking import MemoryRerankDecision
from powercontext.builtin.inference import InferenceUsage


def _validated_scores(payload: object, expected: set[int]) -> dict[int, float]:
    if not isinstance(payload, dict) or set(payload) != {"scores"} or not isinstance(payload["scores"], list):
        raise ValueError("rerank response must contain only a scores array")  # noqa: TRY003
    scores: dict[int, float] = {}
    for item in payload["scores"]:
        if not isinstance(item, dict) or set(item) != {"rank", "score"}:
            raise ValueError("rerank score must contain exactly rank and score")  # noqa: TRY003
        rank, score = item["rank"], item["score"]
        if type(rank) is not int or rank not in expected or rank in scores:
            raise ValueError("rerank response has an invalid or duplicate rank")  # noqa: TRY003
        if type(score) not in (int, float):
            raise ValueError("rerank score must be a finite number")  # noqa: TRY003
        try:
            finite_score = float(score)
        except OverflowError as error:
            raise ValueError("rerank score must be a finite number") from error  # noqa: TRY003
        if not math.isfinite(finite_score):
            raise ValueError("rerank score must be a finite number")  # noqa: TRY003
        scores[rank] = finite_score
    if set(scores) != expected:
        raise ValueError("rerank response must cover every rank in its batch exactly once")  # noqa: TRY003
    return scores


class HTTPMemoryReranker:
    """Select original candidates using validated scores from a caller-owned client."""

    def __init__(
        self,
        endpoint: str,
        *,
        client: httpx.AsyncClient,
        batch_size: int = 32,
        policy_id: str = "http-cross-encoder-v1",
    ) -> None:
        if type(batch_size) is not int or batch_size < 1:
            raise ValueError("batch_size must be a positive integer")  # noqa: TRY003
        self.policy_id = policy_id
        self._endpoint = endpoint
        self._client = client
        self._batch_size = batch_size

    async def rerank(self, query: str, candidates: tuple[MemoryHit, ...], limit: int, /) -> MemoryRerankDecision:
        if not candidates or type(limit) is not int or not 1 <= limit <= len(candidates):
            raise ValueError("limit must be between one and the nonempty candidate count")  # noqa: TRY003
        scores: dict[int, float] = {}
        requests = 0
        for start in range(0, len(candidates), self._batch_size):
            batch = candidates[start : start + self._batch_size]
            response = await self._client.post(
                self._endpoint,
                json={
                    "query": query,
                    "candidates": [
                        {"rank": start + offset, "text": hit.text} for offset, hit in enumerate(batch, start=1)
                    ],
                },
            )
            requests += 1
            response.raise_for_status()
            expected = set(range(start + 1, start + len(batch) + 1))
            scores.update(_validated_scores(response.json(), expected))
        selected = tuple(sorted(scores, key=lambda rank: (-scores[rank], rank))[:limit])
        return MemoryRerankDecision(selected_ranks=selected, usage=InferenceUsage(requests=requests))


__all__ = ["HTTPMemoryReranker"]
