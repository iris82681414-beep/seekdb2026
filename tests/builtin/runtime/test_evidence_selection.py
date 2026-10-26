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

from __future__ import annotations

import json

from powercontext.artifacts import ArtifactRef
from powercontext.builtin.artifacts.memory import MemoryHit
from powercontext.builtin.runtime import PrepareContextRequest
from powercontext.builtin.runtime.prepared_context import PreparedContextBuilder, PreparedMemoryCandidates


def _group(scope_id: str, text: str) -> PreparedMemoryCandidates:
    memory = ArtifactRef(family="memory", artifact_id="memory", revision=1)
    return PreparedMemoryCandidates(
        scope_id=scope_id,
        memory_ref=memory,
        hits=(
            MemoryHit(
                memory_ref=memory,
                entry_id="entry",
                entry_version_id="entry-v1",
                text=text,
                score=1.0,
                matched_by=("fts",),
            ),
        ),
    )


def test_identical_text_in_distinct_scopes_keeps_each_exact_address() -> None:
    text = "The cobalt sensor is calibrated at 37 degrees."
    result = PreparedContextBuilder().build_scopes_result(
        request=PrepareContextRequest(query="cobalt calibrated", evidence_selection=True, max_bytes=2400),
        current_scope_id="left",
        memory_candidates=(_group("left", text), _group("right", text)),
    )
    assert {fragment.address.memory.scope_id for fragment in result.fragments} == {"left", "right"}
    assert result.origins == tuple(fragment.address for fragment in result.fragments)
    assert result.context.content is not None
    items = json.loads(result.context.content.splitlines()[-2])["items"]
    assert {item["citation"]["scope_id"] for item in items} == {"left", "right"}
    assert all(fragment.text == text[fragment.start : fragment.end] for fragment in result.fragments)


def test_evidence_budget_probe_measures_the_selected_complete_envelope() -> None:
    text = ("Unrelated historical administrative notes. " * 70) + "海洋观测 station recorded 37 degrees."
    request = PrepareContextRequest(query="海洋观测 station", evidence_selection=True, max_bytes=1200)
    groups = (_group("project", text),)
    builder = PreparedContextBuilder()
    result = builder.build_scopes_result(request=request, current_scope_id="project", memory_candidates=groups)
    probe = builder.probe_budget(request=request, current_scope_id="project", memory_candidates=groups)
    assert result.fragments and any("37" in fragment.text for fragment in result.fragments)
    assert result.context.content is not None
    assert result.context.content_bytes == len(result.context.content.encode("utf-8")) <= request.max_bytes
    assert probe.delivered_items == len(result.origins)
    assert probe.unused_bytes == request.max_bytes - result.context.content_bytes
    assert all(fragment.text == text[fragment.start : fragment.end] for fragment in result.fragments)
