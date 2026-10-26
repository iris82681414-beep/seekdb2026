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

"""Share in-flight embedding work and retain a bounded cache of successful vectors."""

from __future__ import annotations

import asyncio
import math
from collections import OrderedDict
from dataclasses import dataclass

from powercontext.builtin.inference.models import EmbeddingResult, EmbeddingVector, InferenceUsage
from powercontext.builtin.inference.protocols import EmbeddingModel


@dataclass(frozen=True, slots=True, eq=False)
class _BatchUsage:
    usage: InferenceUsage


@dataclass(frozen=True, slots=True)
class _EmbeddedVector:
    vector: EmbeddingVector
    batch: _BatchUsage | None = None


def _observe_future(future: asyncio.Future[_EmbeddedVector]) -> None:
    # All callers can cancel while the shared work still finishes or fails.
    if not future.cancelled():
        future.exception()


class CoalescingEmbeddingModel:
    """Coordinate batches in one event loop without transferring cancellation ownership."""

    def __init__(
        self,
        model: EmbeddingModel,
        *,
        max_batch_size: int = 64,
        coalesce_ms: float = 5.0,
        cache_size: int = 1024,
    ) -> None:
        if type(max_batch_size) is not int or max_batch_size < 1:
            raise ValueError("max_batch_size must be a positive integer")  # noqa: TRY003
        if type(cache_size) is not int or cache_size < 0:
            raise ValueError("cache_size must be a nonnegative integer")  # noqa: TRY003
        if isinstance(coalesce_ms, bool) or not isinstance(coalesce_ms, (int, float)):
            raise TypeError("coalesce_ms must be a nonnegative finite number")  # noqa: TRY003
        if not math.isfinite(coalesce_ms) or coalesce_ms < 0:
            raise ValueError("coalesce_ms must be a nonnegative finite number")  # noqa: TRY003
        self.profile = model.profile
        self._model = model
        self._max_batch_size = max_batch_size
        self._coalesce_seconds = coalesce_ms / 1000
        self._cache_size = cache_size
        self._cache: OrderedDict[str, EmbeddingVector] = OrderedDict()
        self._inflight: dict[str, asyncio.Future[_EmbeddedVector]] = {}
        self._pending: dict[str, asyncio.Future[_EmbeddedVector]] = {}
        self._flush_handle: asyncio.TimerHandle | None = None
        self._tasks: set[asyncio.Task[None]] = set()

    async def embed(self, texts: tuple[str, ...], /) -> EmbeddingResult:
        if not texts:
            return EmbeddingResult(vectors=(), usage=InferenceUsage(requests=0))
        loop = asyncio.get_running_loop()
        available: dict[str, _EmbeddedVector] = {}
        waiting: dict[str, asyncio.Future[_EmbeddedVector]] = {}
        for text in texts:
            if text in available or text in waiting:
                continue
            if text in self._cache:
                self._cache.move_to_end(text)
                available[text] = _EmbeddedVector(self._cache[text])
                continue
            future = self._inflight.get(text)
            if future is None:
                future = loop.create_future()
                future.add_done_callback(_observe_future)
                self._inflight[text] = future
                self._pending[text] = future
            waiting[text] = future
        if self._pending and self._flush_handle is None:
            self._flush_handle = loop.call_later(self._coalesce_seconds, self._flush)
        if waiting:
            values = await asyncio.gather(*(asyncio.shield(future) for future in waiting.values()))
            available.update(zip(waiting, values, strict=True))
        batches = {value.batch for value in available.values() if value.batch is not None}
        input_tokens = [batch.usage.input_tokens for batch in batches if batch.usage.input_tokens is not None]
        output_tokens = [batch.usage.output_tokens for batch in batches if batch.usage.output_tokens is not None]
        return EmbeddingResult(
            vectors=tuple(available[text].vector for text in texts),
            usage=InferenceUsage(
                requests=sum(batch.usage.requests for batch in batches),
                input_tokens=sum(input_tokens) if input_tokens else None,
                output_tokens=sum(output_tokens) if output_tokens else None,
            ),
        )

    def _flush(self) -> None:
        self._flush_handle = None
        pending = tuple(self._pending.items())
        self._pending.clear()
        for start in range(0, len(pending), self._max_batch_size):
            task = asyncio.create_task(self._run_batch(pending[start : start + self._max_batch_size]))
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)

    async def _run_batch(self, pending: tuple[tuple[str, asyncio.Future[_EmbeddedVector]], ...]) -> None:
        try:
            result = await self._model.embed(tuple(text for text, _future in pending))
            if len(result.vectors) != len(pending):
                # Handle provider contract failures through the same shared failure path.
                raise ValueError("embedding vector count does not match the requested batch")  # noqa: TRY003, TRY301
            batch = _BatchUsage(result.usage)
            for (text, future), vector in zip(pending, result.vectors, strict=True):
                if self._cache_size:
                    self._cache[text] = vector
                    self._cache.move_to_end(text)
                    while len(self._cache) > self._cache_size:
                        self._cache.popitem(last=False)
                self._inflight.pop(text, None)
                future.set_result(_EmbeddedVector(vector, batch))
        except BaseException as error:
            for text, future in pending:
                self._inflight.pop(text, None)
                if not future.done():
                    future.set_exception(error)
            if isinstance(error, (KeyboardInterrupt, SystemExit)):
                raise


__all__ = ["CoalescingEmbeddingModel"]
