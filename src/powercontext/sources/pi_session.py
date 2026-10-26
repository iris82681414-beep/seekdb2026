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

"""Capture complete visible Pi history while retaining branch and checkpoint identity."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any
from urllib.parse import quote

from pydantic import BaseModel, JsonValue

from powercontext.limits import MAX_SOURCE_ID_LENGTH
from powercontext.sources.connectors import ConnectorRunCompletion, ConnectorRunSession, ConnectorRunStatus
from powercontext.sources.definitions import AdapterSourceDefinition
from powercontext.sources.models import Source, SourceMaterialization
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence

_Record = dict[str, Any]


class PiSessionInput(BaseModel):
    content: bytes
    leaf_id: str | None = None


class PiEventInput(BaseModel):
    session_id: str
    event_id: str
    parent_id: str | None
    event_type: str
    role: str
    content: str
    record: dict[str, JsonValue]


class PiEventSource(Source):
    session_id: str
    event_id: str
    parent_id: str | None
    event_type: str
    role: str
    content: str
    record: dict[str, JsonValue]


def _identity(session_id: str, event_id: str) -> str:
    return f"pi:{quote(session_id, safe='')}:{quote(event_id, safe='')}"


def _archive(value: PiSessionInput) -> tuple[str, dict[str, _Record]]:
    try:
        records = [json.loads(line) for line in value.content.decode("utf-8-sig").splitlines() if line.strip()]
    except (UnicodeError, ValueError) as error:
        raise ValueError("invalid Pi JSONL archive") from error  # noqa: TRY003
    if not records or not isinstance(records[0], dict):
        raise ValueError("Pi archive requires a session header")  # noqa: TRY003
    header = records[0]
    if (
        header.get("type") != "session"
        or type(header.get("version")) is not int
        or header["version"] != 3
        or not isinstance(header.get("id"), str)
        or not header["id"].strip()
    ):
        raise ValueError("Pi archive requires a version 3 session header with an ID")  # noqa: TRY003
    nodes: dict[str, _Record] = {}
    for record in records[1:]:
        if not isinstance(record, dict) or not isinstance(record.get("id"), str) or not record["id"].strip():
            raise ValueError("Pi nodes require nonempty string IDs")  # noqa: TRY003
        node_id = record["id"]
        if node_id in nodes or "parentId" not in record:
            raise ValueError("Pi node IDs must be unique and declare their parent")  # noqa: TRY003
        parent = record["parentId"]
        if parent is not None and (not isinstance(parent, str) or parent not in nodes):
            raise ValueError("Pi parent must refer to an earlier node")  # noqa: TRY003
        nodes[node_id] = record
    return header["id"], nodes


def _text_blocks(content: object, *, tool_calls: bool = False) -> str:
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""
    parts: list[str] = []
    for block in content:
        if not isinstance(block, dict):
            continue
        text = block.get("text")
        if block.get("type") == "text" and isinstance(text, str):
            parts.append(text)
        elif tool_calls and block.get("type") == "toolCall":
            arguments = json.dumps(
                block.get("arguments", {}), ensure_ascii=False, sort_keys=True, separators=(",", ":")
            )
            parts.append(f"[tool_call {block.get('name', '')} {block.get('id', '')}] {arguments}")
    return "\n".join(parts)


def _visible_event(session_id: str, record: _Record) -> PiEventInput | None:
    event_type = record.get("type")
    if not isinstance(event_type, str):
        raise ValueError("Pi event requires a string type")  # noqa: TRY003, TRY004
    if event_type == "message":
        message = record.get("message")
        if not isinstance(message, dict) or not isinstance(message.get("role"), str):
            raise ValueError("Pi message requires an object with a role")  # noqa: TRY003
        role = message["role"]
        body = _message_body(message)
    elif event_type in {"compaction", "branch_summary"}:
        role = event_type
        body = record.get("summary", "")
    elif event_type == "custom_message":
        role = "custom"
        body = _text_blocks(record.get("content"))
    else:
        return None
    if not isinstance(body, str):
        raise ValueError("Pi visible content must be text")  # noqa: TRY003, TRY004
    if not body:
        return None
    if len(_identity(session_id, record["id"])) > MAX_SOURCE_ID_LENGTH:
        raise ValueError("Pi event identity exceeds the source ID limit")  # noqa: TRY003
    return PiEventInput(
        session_id=session_id,
        event_id=record["id"],
        parent_id=record["parentId"],
        event_type=event_type,
        role=role,
        content=f"{role}: {body}",
        record=record,
    )


def _message_body(message: _Record) -> str:
    role = message["role"]
    if role == "bashExecution":
        command, output = message.get("command", ""), message.get("output", "")
        if not isinstance(command, str) or not isinstance(output, str):
            raise ValueError("Pi bash command and output must be strings")  # noqa: TRY003
        return f"$ {command}\n{output}"
    body = _text_blocks(message.get("content"), tool_calls=True)
    if role == "toolResult" and body:
        return f"[tool_result {message.get('toolName', '')} {message.get('toolCallId', '')}] {body}"
    return body


def _branch_events(session_id: str, nodes: dict[str, _Record], leaf_id: str | None) -> tuple[PiEventInput, ...]:
    if leaf_id is None:
        leaf_id = next(reversed(nodes), None)
    elif leaf_id not in nodes:
        raise ValueError("Pi leaf does not exist")  # noqa: TRY003
    branch = []
    while leaf_id is not None:
        record = nodes[leaf_id]
        branch.append(record)
        leaf_id = record["parentId"]
    events = (_visible_event(session_id, record) for record in reversed(branch))
    return tuple(event for event in events if event is not None)


def parse_pi_session(value: PiSessionInput) -> tuple[PiEventInput, ...]:
    """Validate the entire archive and then render the selected ancestor path."""
    session_id, nodes = _archive(value)
    return _branch_events(session_id, nodes, value.leaf_id)


class _PiEventAdapter:
    input_class: type[PiEventInput] = PiEventInput
    name = "pi-event"
    source_class: type[PiEventSource] = PiEventSource

    async def resolve(self, value: PiEventInput, /) -> PiEventSource:
        identity = _identity(value.session_id, value.event_id)
        if len(identity) > MAX_SOURCE_ID_LENGTH:
            raise ValueError("Pi event identity exceeds the source ID limit")  # noqa: TRY003
        return PiEventSource(name=identity, materialization=SourceMaterialization.CAPTURED, **value.model_dump())

    async def read(self, source: PiEventSource, /) -> PiEventInput:
        return PiEventInput.model_validate(source.model_dump())


class _PiEventProjection:
    name = TEXT_EVIDENCE_PROJECTION_KEY.name
    version = TEXT_EVIDENCE_PROJECTION_KEY.version
    source_class: type[PiEventSource] = PiEventSource
    output_class: type[BaseModel] = TextEvidence

    def project(self, source: PiEventSource, /) -> TextEvidence:
        return TextEvidence(
            source_type="pi-event",
            source_id=source.name,
            content=source.content,
            metadata={
                "session_id": source.session_id,
                "event_id": source.event_id,
                "parent_id": source.parent_id,
                "event_type": source.event_type,
                "role": source.role,
            },
        )


PI_EVENT_SOURCE_DEFINITION = AdapterSourceDefinition[PiEventInput, PiEventSource, PiEventInput](
    _PiEventAdapter(), projections=(_PiEventProjection(),)
)


def _record_digest(record: _Record) -> str:
    canonical = json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


class PiSessionConnector:
    """Import selected events with durable, immutable history across branch switches."""

    name = "pi-session-history"
    version = "1"
    source_definitions = frozenset({"pi-event"})

    def __init__(self, path: str | Path, leaf_id: str | None = None) -> None:
        self._path = Path(path)
        self._leaf_id = leaf_id

    async def run(self, session: ConnectorRunSession, /) -> ConnectorRunCompletion:
        session_id, nodes = _archive(PiSessionInput(content=self._path.read_bytes(), leaf_id=self._leaf_id))
        events = _branch_events(session_id, nodes, self._leaf_id)
        checkpoint = session.checkpoint
        imported: dict[str, str] = {}
        if checkpoint is not None:
            if not isinstance(checkpoint, dict) or checkpoint.get("version") != 1:
                raise ValueError("invalid Pi connector checkpoint")  # noqa: TRY003
            previous = checkpoint.get("events")
            if not isinstance(previous, dict) or any(not isinstance(value, str) for value in previous.values()):
                raise ValueError("invalid Pi checkpoint event checksums")  # noqa: TRY003
            imported = {key: value for key, value in previous.items() if isinstance(value, str)}
        # Check all previously imported records present in the input, including off-branch history.
        for node_id, record in nodes.items():
            expected = imported.get(_identity(session_id, node_id))
            if expected is not None and expected != _record_digest(record):
                raise ValueError("previously imported Pi history was rewritten")  # noqa: TRY003
        for event in events:
            identity = _identity(session_id, event.event_id)
            if identity not in imported:
                await session.submit(identity, "pi-event", event)
                imported[identity] = _record_digest(event.record)
        return ConnectorRunCompletion(
            status=ConnectorRunStatus.COMPLETE,
            checkpoint={"version": 1, "events": imported},
        )


__all__ = [
    "PI_EVENT_SOURCE_DEFINITION",
    "PiEventInput",
    "PiEventSource",
    "PiSessionConnector",
    "PiSessionInput",
    "parse_pi_session",
]
