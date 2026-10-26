"""Durable partial-write recovery and scope isolation through native Memory."""

import sqlite3

import pytest

from powercontext.builtin.artifacts.memory import MemoryEntryInput
from powercontext.builtin.persistence.sqlite import SQLiteConfig
from powercontext.builtin.runtime import BuiltinConfig, CaptureSource, RememberMemoryRequest, open_builtin_runtime
from powercontext.builtin.runtime.source_lifecycle import SourceLifecycleEvent
from powercontext.builtin.scope import ScopeDraft


@pytest.mark.asyncio
async def test_native_failure_can_resume_after_restart_without_rewriting_successful_entries(tmp_path):
    path = tmp_path / "recovery.db"
    config = BuiltinConfig(database=SQLiteConfig(url=f"sqlite+aiosqlite:///{path}"))
    withdrawal = SourceLifecycleEvent(node_id="origin", revision=1, sequence=1, available=False, reason="withdrawn")
    async with open_builtin_runtime(config) as runtime:
        assert runtime.scopes is not None and runtime.lifecycle is not None
        scope = await runtime.scopes.create(
            ScopeDraft(title="Recovery", summary="Evidence recovery", idempotency_key="recovery")
        )
        memory = runtime.memory.for_scope(scope.scope_id)
        lifecycle = runtime.lifecycle.for_scope(scope.scope_id)
        receipt = await runtime.sources.for_scope(scope.scope_id).capture(
            CaptureSource(source_id="origin", content="fact", metadata={})
        )
        await lifecycle.register_source("origin", receipt.source_ref, revision=1)
        citations = []
        for index in range(2):
            written = await memory.remember(
                RememberMemoryRequest(entries=(MemoryEntryInput(kind="fact", text=f"fact {index}"),))
            )
            assert written.entry is not None
            citations.append(written.entry.citation)
            await lifecycle.register_memory(
                f"derived-{index}", written.entry.citation, parents=("origin",), policy="all"
            )
        blocked_id = citations[1].entry_id.replace("'", "''")
        with sqlite3.connect(path) as database:
            database.execute(
                "CREATE TRIGGER fail_second BEFORE DELETE ON pc_memory_entry_heads "
                f"WHEN OLD.entry_id = '{blocked_id}' BEGIN SELECT RAISE(ABORT, 'injected storage failure'); END"
            )
        with pytest.raises(Exception, match="injected storage failure"):
            await lifecycle.apply(withdrawal)
        states = [entry.state for entry in (await memory.list(include_inactive=True)).entries]
        assert sorted(states) == ["active", "inactive"]
        with sqlite3.connect(path) as database:
            database.execute("DROP TRIGGER fail_second")
    async with open_builtin_runtime(config) as runtime:
        assert runtime.scopes is not None and runtime.lifecycle is not None
        lifecycle = runtime.lifecycle.for_scope(scope.scope_id)
        assert await lifecycle.apply(withdrawal) == []
        memory = runtime.memory.for_scope(scope.scope_id)
        assert not (await memory.list()).entries
        before = (await memory.list()).memory_ref
        assert await lifecycle.apply(withdrawal) == []
        assert (await memory.list()).memory_ref == before
        restoration = withdrawal.model_copy(update={"sequence": 2, "available": True})
        assert await lifecycle.apply(restoration) == ["derived-0", "derived-1", "origin"]
        for index, citation in enumerate(citations):
            assert await lifecycle.resolve(f"derived-{index}", purpose="generation") == citation


@pytest.mark.asyncio
async def test_same_node_identity_in_different_scopes_is_independent():
    async with open_builtin_runtime(BuiltinConfig(database=SQLiteConfig())) as runtime:
        assert runtime.scopes is not None and runtime.lifecycle is not None
        services = []
        for index in range(2):
            scope = await runtime.scopes.create(
                ScopeDraft(title="Isolated", summary="Evidence scope", idempotency_key=f"scope-{index}")
            )
            source = await runtime.sources.for_scope(scope.scope_id).capture(
                CaptureSource(source_id="origin", content="fact", metadata={})
            )
            lifecycle = runtime.lifecycle.for_scope(scope.scope_id)
            await lifecycle.register_source("origin", source.source_ref, revision=1)
            services.append(lifecycle)
        await services[0].apply(
            SourceLifecycleEvent(node_id="origin", revision=1, sequence=1, available=False, reason="withdrawn")
        )
        assert await services[1].resolve("origin", purpose="generation")
