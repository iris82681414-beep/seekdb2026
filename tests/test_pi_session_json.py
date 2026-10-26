import json

import pytest

from powercontext.sources.pi_session import PiSessionInput, parse_pi_session


@pytest.mark.parametrize("constant", ["NaN", "Infinity", "-Infinity"])
@pytest.mark.parametrize("location", ["header", "other_branch"])
def test_pi_rejects_non_json_constants_in_the_entire_archive(constant: str, location: str) -> None:
    header = '{"type":"session","version":3,"id":"session"}'
    selected = '{"id":"selected","parentId":null,"type":"message","message":{"role":"user","content":"ok"}}'
    other = '{"id":"other","parentId":null,"type":"control","metadata":{"value":' + constant + "}}"
    if location == "header":
        header = header[:-1] + ',"metadata":{"value":' + constant + "}}"
        other = '{"id":"other","parentId":null,"type":"control"}'
    content = "\n".join((header, selected, other)).encode()

    with pytest.raises(ValueError):
        parse_pi_session(PiSessionInput(content=content, leaf_id="selected"))


@pytest.mark.parametrize("text", ["NaN", "Infinity", "-Infinity"])
def test_pi_preserves_strings_that_resemble_json_constants(text: str) -> None:
    header = {"type": "session", "version": 3, "id": "session"}
    record = {
        "id": "event",
        "parentId": None,
        "type": "message",
        "message": {"role": "user", "content": text},
        "metadata": {"value": text, "number": -1.5e2},
    }
    content = "\n".join(json.dumps(value) for value in (header, record)).encode()

    events = parse_pi_session(PiSessionInput(content=content))

    assert len(events) == 1
    assert events[0].content == f"user: {text}"
    assert events[0].record == record
