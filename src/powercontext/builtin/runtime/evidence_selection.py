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

"""Select query-relevant literal spans while measuring their complete cited envelope."""

from __future__ import annotations

import json
import math
import re
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from itertools import pairwise
from typing import TYPE_CHECKING

from powercontext.artifacts import ArtifactAddress
from powercontext.builtin.artifacts.search import analyze_fts_query, analyze_text
from powercontext.builtin.runtime.errors import PreparedContextInvariantError
from powercontext.builtin.runtime.models import PrepareContextRequest, PreparedContext
from powercontext.builtin.runtime.prepared_text import TRUST_POLICY

if TYPE_CHECKING:
    from powercontext.builtin.runtime.prepared_context import (
        MemoryEntryAddress,
        PreparedContextBuild,
        PreparedMemoryCandidates,
    )

_BOUNDARY = re.compile(r"(?<=[.!?])\s+|\n+")
_BEGIN = "BEGIN_POWERCONTEXT_PREPARED_CONTEXT_V1"
_END = "END_POWERCONTEXT_PREPARED_CONTEXT_V1"
_RESULT_TERMS = frozenset({"found", "showed", "observed", "demonstrated", "associated", "significantly", "results"})


@dataclass(frozen=True, slots=True)
class MemoryEvidenceFragment:
    """One literal half-open character interval at an exact scoped Memory version."""

    address: MemoryEntryAddress
    start: int
    end: int
    text: str


@dataclass(frozen=True, slots=True)
class _Document:
    address: MemoryEntryAddress
    text: str


@dataclass(frozen=True, slots=True)
class _Span:
    document: int
    start: int
    end: int
    terms: frozenset[str]
    score: float


def _terms(text: str, *, query: bool = False) -> tuple[str, ...]:
    normalized = analyze_fts_query(text) if query else analyze_text(text)
    return tuple(
        term[:-1] if len(term) > 4 and term.endswith("s") and not term.endswith("ss") else term
        for term in normalized.split()
    )


def _sentences(text: str) -> tuple[tuple[int, int], ...]:
    boundaries = [0, *(match.end() for match in _BOUNDARY.finditer(text)), len(text)]
    sentences = []
    for start, end in pairwise(boundaries):
        while start < end and text[start].isspace():
            start += 1
        while end > start and text[end - 1].isspace():
            end -= 1
        if start < end:
            sentences.append((start, end))
    return tuple(sentences)


def _intervals(text: str) -> tuple[tuple[int, int], ...]:
    sentences = _sentences(text)
    intervals = set(sentences)
    for index, (start, end) in enumerate(sentences):
        for width in (2, 3):
            if index + width <= len(sentences):
                expanded_end = sentences[index + width - 1][1]
                if expanded_end - start <= 1200:
                    intervals.add((start, expanded_end))
        # Long unpunctuated text still needs query-dependent windows near its tail.
        if end - start > 256:
            for offset in range(start, end, 96):
                intervals.add((offset, min(offset + 192, end)))
    return tuple(sorted(intervals))


def _documents(groups: Sequence[PreparedMemoryCandidates], current_scope_id: str | None) -> tuple[_Document, ...]:
    from powercontext.builtin.runtime.prepared_context import MemoryEntryAddress

    documents = []
    seen: set[tuple[str, str, int, str, str]] = set()
    for group in groups:
        scope_id = group.scope_id or current_scope_id
        if not scope_id and group.hits:
            raise PreparedContextInvariantError("memory-scope-missing")
        for hit in group.hits:
            if hit.memory_ref != group.memory_ref:
                raise PreparedContextInvariantError("memory-ref-mismatch")
            if not hit.entry_id.strip() or not hit.entry_version_id.strip() or not hit.text.strip():
                continue
            key = (
                scope_id or "",
                hit.memory_ref.artifact_id,
                hit.memory_ref.revision,
                hit.entry_id,
                hit.entry_version_id,
            )
            if key in seen:
                continue
            seen.add(key)
            address = MemoryEntryAddress(
                memory=ArtifactAddress(scope_id=scope_id or "", artifact=hit.memory_ref),
                entry_id=hit.entry_id,
                entry_version_id=hit.entry_version_id,
            )
            documents.append(_Document(address, hit.text))
    return tuple(documents)


def _candidates(documents: tuple[_Document, ...], query: str) -> tuple[_Span, ...]:
    query_terms = frozenset(_terms(query, query=True))
    frequencies: Counter[str] = Counter()
    for document in documents:
        frequencies.update(set(_terms(document.text)))
    weights = {term: math.log1p((len(documents) + 1) / (frequencies[term] + 0.5)) for term in query_terms}
    spans = []
    for index, document in enumerate(documents):
        for start, end in _intervals(document.text):
            tokens = _terms(document.text[start:end])
            counts = Counter(tokens)
            matched = query_terms.intersection(counts)
            if not matched:
                continue
            relevance = sum(weights[term] * (1 + math.log(counts[term])) for term in matched)
            coverage = len(matched) / max(len(query_terms), 1)
            relevance *= 1 + coverage
            if _RESULT_TERMS.intersection(counts):
                relevance *= 1.12
            relevance /= max(len(tokens), 8) ** 0.2
            spans.append(_Span(index, start, end, frozenset(matched), relevance))
    return tuple(spans)


def _add_interval(
    intervals: tuple[tuple[int, int], ...], start: int, end: int, text: str
) -> tuple[tuple[int, int], ...]:
    merged: list[tuple[int, int]] = []
    for left, right in sorted((*intervals, (start, end))):
        if merged and (left <= merged[-1][1] or not text[merged[-1][1] : left].strip()):
            previous_start, previous_end = merged[-1]
            merged[-1] = previous_start, max(previous_end, right)
        else:
            merged.append((left, right))
    return tuple(merged)


def _fragments(
    documents: tuple[_Document, ...], selection: dict[int, tuple[tuple[int, int], ...]]
) -> tuple[MemoryEvidenceFragment, ...]:
    return tuple(
        MemoryEvidenceFragment(documents[index].address, start, end, documents[index].text[start:end])
        for index, intervals in selection.items()
        for start, end in intervals
    )


def _render(fragments: tuple[MemoryEvidenceFragment, ...]) -> str:
    items = [
        {
            "citation": {
                "scope_id": fragment.address.memory.scope_id,
                "memory_ref": fragment.address.memory.artifact.model_dump(mode="json"),
                "entry_id": fragment.address.entry_id,
                "entry_version_id": fragment.address.entry_version_id,
            },
            "content": fragment.text,
            "start": fragment.start,
            "end": fragment.end,
        }
        for fragment in fragments
    ]
    payload = json.dumps({"trust": "untrusted_history", "items": items}, ensure_ascii=False, separators=(",", ":"))
    return f"{TRUST_POLICY}\n\n{_BEGIN}\n{payload}\n{_END}"


def build_memory_evidence(
    request: PrepareContextRequest,
    groups: Sequence[PreparedMemoryCandidates],
    *,
    current_scope_id: str | None,
) -> PreparedContextBuild:
    """Greedily add literal evidence using the marginal complete serialized cost."""
    from powercontext.builtin.runtime.prepared_context import PreparedContextBuild

    documents = _documents(groups, current_scope_id)
    candidates = _candidates(documents, request.query)
    selection: dict[int, tuple[tuple[int, int], ...]] = {}
    covered: set[str] = set()
    content_bytes = len(_render(()).encode("utf-8"))
    while candidates:
        best: tuple[float, _Span, dict[int, tuple[tuple[int, int], ...]], int] | None = None
        for candidate in candidates:
            intervals = selection.get(candidate.document, ())
            if any(start <= candidate.start and candidate.end <= end for start, end in intervals):
                continue
            trial = {
                **selection,
                candidate.document: _add_interval(
                    intervals, candidate.start, candidate.end, documents[candidate.document].text
                ),
            }
            size = len(_render(_fragments(documents, trial)).encode("utf-8"))
            if size > request.max_bytes:
                continue
            novelty = len(candidate.terms - covered) / max(len(candidate.terms), 1)
            utility = candidate.score * (0.45 + 0.55 * novelty) / max(size - content_bytes, 1) ** 0.35
            if best is None or utility > best[0]:
                best = utility, candidate, trial, size
        if best is None:
            break
        _utility, selected, selection, content_bytes = best
        covered.update(selected.terms)
        candidates = tuple(candidate for candidate in candidates if candidate != selected)
    fragments = _fragments(documents, selection)
    if not fragments:
        return PreparedContextBuild(context=PreparedContext(status="empty", content=None, content_bytes=0), origins=())
    content = _render(fragments)
    return PreparedContextBuild(
        context=PreparedContext(status="ready", content=content, content_bytes=len(content.encode("utf-8"))),
        origins=tuple(fragment.address for fragment in fragments),
        fragments=fragments,
    )
