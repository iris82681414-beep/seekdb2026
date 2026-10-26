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

"""Self-contained, canonical context packs and validated prefix deltas."""

from __future__ import annotations

import base64
import binascii
import hashlib
import json
import re
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

_PACK_SCHEMA = "powercontext.context-pack.v1"
_DELTA_SCHEMA = "powercontext.context-pack-delta.v1"
_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")


class ContextPackError(ValueError):
    """Invalid context wire data, integrity metadata, or chunk boundaries."""


def _canonical(value: Mapping[str, object]) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _encoded(items: tuple[bytes, ...]) -> list[str]:
    if not isinstance(items, tuple) or any(not isinstance(item, bytes) for item in items):
        raise ContextPackError("context items must be a tuple of bytes")  # noqa: TRY003
    return [base64.b64encode(item).decode("ascii") for item in items]


def _decoded(items: object) -> tuple[bytes, ...]:
    if not isinstance(items, list):
        raise ContextPackError("encoded context items must be an array")  # noqa: TRY003
    result = []
    for item in items:
        if not isinstance(item, str):
            raise ContextPackError("encoded context items must be Base64 strings")  # noqa: TRY003
        try:
            decoded = base64.b64decode(item, validate=True)
        except (ValueError, binascii.Error) as error:
            raise ContextPackError("invalid context Base64") from error  # noqa: TRY003
        if base64.b64encode(decoded).decode("ascii") != item:
            raise ContextPackError("context Base64 must use canonical encoding")  # noqa: TRY003
        result.append(decoded)
    return tuple(result)


def _load(data: bytes, fields: set[str], schema: str) -> dict[str, Any]:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ContextPackError("duplicate context wire field")  # noqa: TRY003
            result[key] = value
        return result

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=unique_object)
    except (UnicodeError, ValueError, AttributeError) as error:
        raise ContextPackError("invalid context JSON") from error  # noqa: TRY003
    if not isinstance(value, dict) or set(value) != fields or value["schema"] != schema:
        raise ContextPackError("invalid context schema or fields")  # noqa: TRY003
    return value


def _validate_digest(value: object) -> None:
    if not isinstance(value, str) or _DIGEST.fullmatch(value) is None:
        raise ContextPackError("invalid context digest")  # noqa: TRY003


@dataclass(frozen=True, slots=True)
class ContextPack:
    """A complete byte sequence with a digest over its canonical encoded chunks."""

    items: tuple[bytes, ...]
    digest: str
    byte_count: int

    @classmethod
    def build(cls, items: tuple[bytes, ...]) -> ContextPack:
        encoded = _encoded(items)
        unsigned = {"schema": _PACK_SCHEMA, "items": encoded}
        digest = "sha256:" + hashlib.sha256(_canonical(unsigned)).hexdigest()
        byte_count = 0
        while True:
            actual = len(_canonical({**unsigned, "digest": digest, "byte_count": byte_count}))
            if actual == byte_count:
                return cls(items=items, digest=digest, byte_count=byte_count)
            byte_count = actual

    @classmethod
    def from_content(cls, content: str | None, *, chunk_bytes: int = 512) -> ContextPack:
        if type(chunk_bytes) is not int or chunk_bytes < 1:
            raise ContextPackError("chunk_bytes must be a positive integer")  # noqa: TRY003
        if content is not None and not isinstance(content, str):
            raise ContextPackError("context content must be text or None")  # noqa: TRY003
        try:
            data = (content or "").encode("utf-8")
        except UnicodeError as error:
            raise ContextPackError("context must be valid UTF-8") from error  # noqa: TRY003
        chunks: list[bytes] = []
        # Line boundaries let appended evidence reuse the preceding complete lines.
        for line in data.splitlines(keepends=True):
            start = 0
            while start < len(line):
                end = min(start + chunk_bytes, len(line))
                chunk = line[start:end].decode("utf-8", errors="ignore").encode("utf-8")
                if not chunk:
                    raise ContextPackError("chunk limit cannot hold the next UTF-8 character")  # noqa: TRY003
                chunks.append(chunk)
                start += len(chunk)
        return cls.build(tuple(chunks))

    def to_bytes(self) -> bytes:
        return _canonical({
            "schema": _PACK_SCHEMA,
            "items": _encoded(self.items),
            "digest": self.digest,
            "byte_count": self.byte_count,
        })

    @classmethod
    def from_bytes(cls, data: bytes) -> ContextPack:
        value = _load(data, {"schema", "items", "digest", "byte_count"}, _PACK_SCHEMA)
        pack = cls.build(_decoded(value["items"]))
        if type(value["byte_count"]) is not int or value["byte_count"] != pack.byte_count:
            raise ContextPackError("context byte count does not match its canonical encoding")  # noqa: TRY003
        if value["digest"] != pack.digest:
            raise ContextPackError("context digest mismatch")  # noqa: TRY003
        return pack


@dataclass(frozen=True, slots=True)
class ContextPackDelta:
    """Retain a base prefix and append a complete, independently encoded suffix."""

    base_digest: str
    target_digest: str
    prefix_items: int
    appended: tuple[bytes, ...]

    @classmethod
    def between(cls, base: ContextPack, target: ContextPack) -> ContextPackDelta:
        _validate_pack(base)
        _validate_pack(target)
        prefix = 0
        for old, new in zip(base.items, target.items, strict=False):
            if old != new:
                break
            prefix += 1
        return cls(base.digest, target.digest, prefix, target.items[prefix:])

    def to_bytes(self) -> bytes:
        _validate_digest(self.base_digest)
        _validate_digest(self.target_digest)
        if type(self.prefix_items) is not int or self.prefix_items < 0:
            raise ContextPackError("prefix_items must be a nonnegative integer")  # noqa: TRY003
        return _canonical({
            "schema": _DELTA_SCHEMA,
            "base_digest": self.base_digest,
            "target_digest": self.target_digest,
            "prefix_items": self.prefix_items,
            "appended": _encoded(self.appended),
        })

    @property
    def byte_count(self) -> int:
        return len(self.to_bytes())

    @classmethod
    def from_bytes(cls, data: bytes) -> ContextPackDelta:
        value = _load(data, {"schema", "base_digest", "target_digest", "prefix_items", "appended"}, _DELTA_SCHEMA)
        delta = cls(value["base_digest"], value["target_digest"], value["prefix_items"], _decoded(value["appended"]))
        delta.to_bytes()
        return delta


def _validate_pack(pack: ContextPack) -> None:
    if not isinstance(pack, ContextPack) or pack != ContextPack.build(pack.items):
        raise ContextPackError("invalid base or target context pack")  # noqa: TRY003


def apply_context_pack_delta(base: ContextPack, delta: ContextPackDelta) -> ContextPack:
    """Reconstruct solely from the supplied base and validated delta data."""
    _validate_pack(base)
    delta.to_bytes()
    if delta.base_digest != base.digest or delta.prefix_items > len(base.items):
        raise ContextPackError("delta does not match its base pack")  # noqa: TRY003
    target = ContextPack.build(base.items[: delta.prefix_items] + delta.appended)
    if target.digest != delta.target_digest:
        raise ContextPackError("delta target digest mismatch")  # noqa: TRY003
    return target


__all__ = ["ContextPack", "ContextPackDelta", "ContextPackError", "apply_context_pack_delta"]
