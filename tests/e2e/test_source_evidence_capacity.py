# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
from __future__ import annotations

import asyncio
import hashlib
import importlib.util
from pathlib import Path
from typing import Literal

import pytest

from powercontext.builtin.artifacts.memory import InvalidMemoryCandidateError, MemoryCandidateRequest, MemoryEntryInput
from powercontext.builtin.persistence.seekdb import SeekDBConfig
from powercontext.builtin.persistence.sqlite import SQLiteConfig
from powercontext.builtin.runtime import (
    BuiltinConfig,
    GetMemoryEntryRequest,
    RememberMemoryRequest,
    ReviseMemoryEntryRequest,
    SearchMemoryRequest,
    SubmitSourceObservation,
    open_builtin_runtime,
)
from powercontext.builtin.scope import ScopeDraft
from powercontext.sources import (
    SourceDefinitionRegistry,
    SourceObservation,
    manifest_for_definition,
    project_source_for_transport,
)
from powercontext.sources.pdf_pages import PDF_PAGE_SOURCE_DEFINITION, PDFPageInput
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence


class _LiteralEvidence:
    async def extract(self, request: MemoryCandidateRequest, /) -> tuple[MemoryEntryInput, ...]:
        return tuple(
            MemoryEntryInput(
                kind="source-evidence",
                text=TextEvidence.model_validate(source.projection(TEXT_EVIDENCE_PROJECTION_KEY)).content,
                sources=(source,),
            )
            for source in request.sources
            if isinstance(source, SourceObservation)
        )


@pytest.mark.parametrize("backend", ["sqlite", "seekdb"])
def test_long_page_persists_searches_and_revises_without_losing_historical_citations(
    tmp_path: Path, backend: Literal["sqlite", "seekdb"]
) -> None:
    if backend == "seekdb" and importlib.util.find_spec("pylibseekdb") is None:
        pytest.skip("native seekdb runtime is unavailable")

    async def scenario() -> None:
        config = BuiltinConfig(
            database=(
                SQLiteConfig(url=f"sqlite+aiosqlite:///{tmp_path / 'evidence.db'}")
                if backend == "sqlite"
                else SeekDBConfig(path=tmp_path / "seekdb")
            )
        )
        text = "Quarterly report.\n" + "Financial ledger balances.\n" * 500 + "violetmarker"
        assert 8192 < len(text.encode()) < 65536
        page = PDFPageInput(
            document_id="report",
            document_sha256=hashlib.sha256(b"constructed page evidence").hexdigest(),
            page_number=1,
            text=text,
        )
        registry = SourceDefinitionRegistry((PDF_PAGE_SOURCE_DEFINITION,))
        observation = project_source_for_transport(registry, await registry.resolve(page))
        async with open_builtin_runtime(config, candidate_pipeline=_LiteralEvidence()) as runtime:
            assert runtime.scopes is not None
            scope = await runtime.scopes.create(
                ScopeDraft(title="Complete evidence", summary="Long captured page", idempotency_key="evidence")
            )
            await runtime.ingestion.register(manifest_for_definition(PDF_PAGE_SOURCE_DEFINITION))
            request = SubmitSourceObservation(scope_id=scope.scope_id, observation=observation)
            await runtime.ingestion.submit(request)
            await runtime.ingestion.submit(request)
            memory = runtime.memory.for_scope(scope.scope_id)
            while (await memory.flush()).processed:
                pass
            entries = await memory.list()
            assert len(entries.entries) == 1
            original = entries.entries[0]
            assert original.entry.text == f"report\n{text}"
            assert len(original.entry.sources) == 1

        async with open_builtin_runtime(config) as runtime:
            memory = runtime.memory.for_scope(scope.scope_id)
            assert await memory.get(GetMemoryEntryRequest(citation=original.citation)) == original
            hits = await memory.search(SearchMemoryRequest(query="violetmarker", mode="fts", limit=1))
            assert len(hits.hits) == 1 and hits.hits[0].entry_version_id == original.entry.entry_version_id
            updated_text = original.entry.text.replace("violetmarker", "ambermarker")
            revised = await memory.revise(
                ReviseMemoryEntryRequest(citation=original.citation, kind="source-evidence", text=updated_text)
            )
            updated = revised.entry
            assert updated is not None
            assert updated.entry.text == updated_text
            assert updated.entry.sources == original.entry.sources
            assert await memory.get(GetMemoryEntryRequest(citation=original.citation)) == original
            before = (await memory.list()).memory_ref
            with pytest.raises(InvalidMemoryCandidateError, match="8192 UTF-8 bytes"):
                await memory.remember(RememberMemoryRequest(entries=(MemoryEntryInput(kind="note", text="界" * 2731),)))
            assert (await memory.list()).memory_ref == before

        async with open_builtin_runtime(config) as runtime:
            memory = runtime.memory.for_scope(scope.scope_id)
            assert await memory.get(GetMemoryEntryRequest(citation=original.citation)) == original
            assert await memory.get(GetMemoryEntryRequest(citation=updated.citation)) == updated
            hits = await memory.search(SearchMemoryRequest(query="ambermarker", mode="fts", limit=1))
            assert len(hits.hits) == 1 and hits.hits[0].entry_version_id == updated.entry.entry_version_id

    asyncio.run(scenario())
