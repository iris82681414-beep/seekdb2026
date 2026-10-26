# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at https://www.apache.org/licenses/LICENSE-2.0
"""Durable evidence dependencies and replayable native availability changes."""
# ruff: noqa: TRY003 - lifecycle validation messages belong to this public boundary.

from __future__ import annotations

from typing import TYPE_CHECKING, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator
from sqlalchemy import insert, select, update

from powercontext.artifacts import ArtifactRef, MemoryCitation
from powercontext.builtin.persistence.database import AsyncDatabase
from powercontext.builtin.persistence.sources import SourceRepository
from powercontext.builtin.persistence.tables import ARTIFACT_HEADS_TABLE, SOURCE_LIFECYCLE_GRAPHS_TABLE
from powercontext.builtin.runtime.models import GetMemoryEntryRequest
from powercontext.errors import PowerContextError
from powercontext.sources import SourceDefinitionRegistry, SourceRef

if TYPE_CHECKING:
    from powercontext.builtin.runtime.application import BuiltinRuntime


class SourceLifecycleError(ValueError):
    """A registration, event or use is inconsistent with its evidence graph."""


class SourceLifecycleEvent(BaseModel):
    """One strictly typed state transition of a registered Source version."""

    model_config = ConfigDict(extra="forbid", frozen=True, strict=True)
    node_id: str = Field(min_length=1)
    revision: int = Field(gt=0)
    sequence: int = Field(gt=0)
    available: bool
    reason: str = Field(min_length=1)

    @field_validator("node_id", "reason")
    @classmethod
    def nonblank(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("must contain non-whitespace text")
        return value


class _Node(BaseModel):
    kind: Literal["source", "memory", "topic"]
    reference: SourceRef | MemoryCitation | ArtifactRef
    revision: int = 1
    parents: tuple[str, ...] = ()
    policy: Literal["all", "any"] = "all"
    available: bool = True


class _Graph(BaseModel):
    format_version: Literal[1] = 1
    nodes: dict[str, _Node] = Field(default_factory=dict)
    events: dict[str, SourceLifecycleEvent] = Field(default_factory=dict)
    settled: bool = True


def _identity(node_id: str) -> None:
    if not isinstance(node_id, str) or not node_id.strip():
        raise SourceLifecycleError("node_id must be a nonblank string")


def _propagate(graph: _Graph) -> None:
    visited: set[str] = set()

    def visit(node_id: str) -> bool:
        node = graph.nodes[node_id]
        if node_id not in visited:
            if node.kind != "source":
                states = [visit(parent) for parent in node.parents]
                node.available = all(states) if node.policy == "all" else any(states)
            visited.add(node_id)
        return node.available

    for node_id in graph.nodes:
        visit(node_id)


class ScopedSourceLifecycle:
    """One scope's single-writer DAG; immutable references remain audit evidence."""

    def __init__(
        self, runtime: BuiltinRuntime, database: AsyncDatabase, sources: SourceRepository, scope_id: str
    ) -> None:
        self._runtime = runtime
        self._database = database
        self._sources = sources
        self.scope_id = scope_id

    async def register_source(self, node_id: str, source_ref: SourceRef, *, revision: int) -> None:
        if type(revision) is not int or revision <= 0 or not isinstance(source_ref, SourceRef):
            raise SourceLifecycleError("invalid Source registration")
        await self._register(node_id, _Node(kind="source", reference=source_ref, revision=revision))

    async def register_memory(
        self, node_id: str, citation: MemoryCitation, *, parents: tuple[str, ...], policy: Literal["all", "any"]
    ) -> None:
        if not isinstance(citation, MemoryCitation):
            raise SourceLifecycleError("invalid Memory citation")
        await self._register(node_id, self._derived("memory", citation, parents, policy))

    async def register_topic(
        self, node_id: str, artifact_ref: ArtifactRef, *, parents: tuple[str, ...], policy: Literal["all", "any"]
    ) -> None:
        if not isinstance(artifact_ref, ArtifactRef) or artifact_ref.family != "topic-memory":
            raise SourceLifecycleError("invalid Topic reference")
        await self._register(node_id, self._derived("topic", artifact_ref, parents, policy))

    @staticmethod
    def _derived(
        kind: Literal["memory", "topic"],
        reference: MemoryCitation | ArtifactRef,
        parents: tuple[str, ...],
        policy: Literal["all", "any"],
    ) -> _Node:
        if policy not in {"all", "any"} or not isinstance(parents, tuple) or not parents:
            raise SourceLifecycleError("derived nodes need parents and an all/any policy")
        for parent in parents:
            _identity(parent)
        if len(set(parents)) != len(parents):
            raise SourceLifecycleError("duplicate parent")
        return _Node(kind=kind, reference=reference, parents=parents, policy=policy)

    async def _register(self, node_id: str, node: _Node) -> None:
        _identity(node_id)
        async with self._runtime._scope_operation(self.scope_id), self._runtime._locked(self.scope_id):
            graph = await self._load()
            existing = graph.nodes.get(node_id)
            if existing is not None:
                if existing.model_copy(update={"available": True}) != node:
                    raise SourceLifecycleError("node registration conflicts with its existing binding")
                if not graph.settled:
                    await self._finish(graph)
                return
            if node_id in node.parents or any(parent not in graph.nodes for parent in node.parents):
                raise SourceLifecycleError("parents must already exist and cannot include this node")
            await self._validate_reference(node)
            graph.nodes[node_id] = node
            _propagate(graph)
            graph.settled = False
            await self._store(graph)
            await self._finish(graph)

    async def _validate_reference(self, node: _Node) -> None:
        try:
            ref = node.reference
            if isinstance(ref, SourceRef):
                async with self._database.transaction() as connection:
                    await self._sources.get(connection, self.scope_id, ref)
            elif isinstance(ref, MemoryCitation):
                await self._runtime.memory.for_scope(self.scope_id).get(GetMemoryEntryRequest(citation=ref))
            else:
                await self._runtime.records.for_scope(self.scope_id).get_artifact_revision(
                    ref.family, ref.artifact_id, ref.revision
                )
        except (PowerContextError, LookupError, ValueError, RuntimeError) as error:
            raise SourceLifecycleError("registered reference must exist and be readable") from error

    async def apply(self, event: SourceLifecycleEvent) -> list[str]:
        if not isinstance(event, SourceLifecycleEvent):
            raise SourceLifecycleError("expected a SourceLifecycleEvent")
        async with self._runtime._scope_operation(self.scope_id), self._runtime._locked(self.scope_id):
            graph = await self._load()
            node = graph.nodes.get(event.node_id)
            if node is None or node.kind != "source" or node.revision != event.revision:
                raise SourceLifecycleError("unknown Source or mismatched lifecycle revision")
            previous = graph.events.get(event.node_id)
            if previous is not None and event.sequence <= previous.sequence:
                if event != previous:
                    raise SourceLifecycleError("event sequence must increase; exact replay is allowed")
            else:
                node.available = event.available
                graph.events[event.node_id] = event
                _propagate(graph)
                graph.settled = False
                # Accept the event before native writes. On an interrupted write,
                # exact replay resumes reconciliation against the actual heads.
                await self._store(graph)
            if not graph.settled:
                await self._finish(graph)
            return sorted(key for key, value in graph.nodes.items() if value.available)

    async def resolve(
        self, node_id: str, *, purpose: Literal["generation", "audit"]
    ) -> SourceRef | MemoryCitation | ArtifactRef:
        _identity(node_id)
        if purpose not in {"generation", "audit"}:
            raise SourceLifecycleError("unknown reference purpose")
        async with self._runtime._scope_operation(self.scope_id):
            graph = await self._load()
            node = graph.nodes.get(node_id)
            if node is None or (purpose == "generation" and (not node.available or not graph.settled)):
                raise SourceLifecycleError("node is unavailable for this purpose")
            return node.reference

    async def _finish(self, graph: _Graph) -> None:
        memories = [node for node in graph.nodes.values() if node.kind == "memory"]
        if memories:
            async with self._runtime._context(self.scope_id) as context:
                service = context.artifacts.memory
                for node in memories:
                    ref = node.reference
                    if not isinstance(ref, MemoryCitation):
                        raise SourceLifecycleError("corrupt Memory binding")
                    current, entries = await service.head_entries(ref.memory_ref.artifact_id)
                    entry = next((entry for entry in entries if entry.entry_id == ref.entry_id), None)
                    if entry is None or entry.entry_version_id != ref.entry_version_id:
                        raise SourceLifecycleError("registered Memory body version changed")
                    state = next(
                        item.state for item in current.content.manifest.entries if item.entry_id == ref.entry_id
                    )
                    if (state == "active") != node.available:
                        change = service.reactivate if node.available else service.forget
                        await change(current, entries=(entry,), reason="Source dependency availability")
        async with self._database.transaction() as connection:
            heads = ARTIFACT_HEADS_TABLE
            for node in graph.nodes.values():
                ref = node.reference
                if node.kind == "topic" and isinstance(ref, ArtifactRef):
                    target = "active" if node.available else "deprecated"
                    await connection.execute(
                        update(heads)
                        .where(
                            heads.c.scope_id == self.scope_id,
                            heads.c.family == ref.family,
                            heads.c.artifact_id == ref.artifact_id,
                            heads.c.lifecycle_state != target,
                        )
                        .values(lifecycle_state=target, governance_generation=heads.c.governance_generation + 1)
                    )
        graph.settled = True
        await self._store(graph)

    async def _load(self) -> _Graph:
        table = SOURCE_LIFECYCLE_GRAPHS_TABLE
        async with self._database.transaction() as connection:
            payload = await connection.scalar(select(table.c.payload).where(table.c.scope_id == self.scope_id))
        return _Graph() if payload is None else _Graph.model_validate_json(payload)

    async def _store(self, graph: _Graph) -> None:
        table = SOURCE_LIFECYCLE_GRAPHS_TABLE
        payload = graph.model_dump_json().encode()
        async with self._database.transaction() as connection:
            exists = await connection.scalar(select(table.c.scope_id).where(table.c.scope_id == self.scope_id))
            if exists is None:
                await connection.execute(insert(table).values(scope_id=self.scope_id, payload=payload))
            else:
                await connection.execute(update(table).where(table.c.scope_id == self.scope_id).values(payload=payload))


class SourceLifecycleApplication:
    """Select the durable lifecycle graph owned by one Runtime scope."""

    def __init__(
        self, runtime: BuiltinRuntime, database: AsyncDatabase, source_registry: SourceDefinitionRegistry
    ) -> None:
        self._runtime = runtime
        self._database = database
        self._sources = SourceRepository(source_registry)

    def for_scope(self, scope_id: str) -> ScopedSourceLifecycle:
        return ScopedSourceLifecycle(self._runtime, self._database, self._sources, scope_id)


__all__ = ["SourceLifecycleError", "SourceLifecycleEvent"]
