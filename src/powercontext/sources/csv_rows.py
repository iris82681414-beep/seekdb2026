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

"""Captured CSV rows with stable primary-key identities and lossless evidence."""

from __future__ import annotations

import csv
import io
import json
from urllib.parse import quote

from pydantic import BaseModel, model_validator

from powercontext.limits import MAX_SOURCE_ID_LENGTH
from powercontext.sources.definitions import AdapterSourceDefinition
from powercontext.sources.models import Source, SourceMaterialization
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence


def csv_row_source_id(document_id: str, primary_key_value: str) -> str:
    """Encode identity components independently without normalizing their values."""
    if not document_id or document_id != document_id.strip():
        raise ValueError("document_id must be nonempty and have no surrounding whitespace")  # noqa: TRY003
    if not primary_key_value.strip():
        raise ValueError("primary key value must not be blank")  # noqa: TRY003
    identity = f"csv:{quote(document_id, safe='')}:{quote(primary_key_value, safe='')}"
    if len(identity) > MAX_SOURCE_ID_LENGTH:
        raise ValueError("CSV row identity exceeds the source ID limit")  # noqa: TRY003
    return identity


class CSVDocumentInput(BaseModel):
    """One complete UTF-8 CSV document and its exact primary-key column name."""

    document_id: str
    primary_key: str
    content: bytes


class CSVRowInput(BaseModel):
    """An original record, preserving column order and every string value."""

    document_id: str
    primary_key: str
    primary_key_value: str
    columns: tuple[str, ...]
    values: tuple[str, ...]

    @model_validator(mode="after")
    def validate_record(self) -> CSVRowInput:
        csv_row_source_id(self.document_id, self.primary_key_value)
        _validate_columns(self.columns, self.primary_key)
        if len(self.values) != len(self.columns):
            raise ValueError("CSV record width does not match the header")  # noqa: TRY003
        if self.values[self.columns.index(self.primary_key)] != self.primary_key_value:
            raise ValueError("CSV primary key does not match the original record")  # noqa: TRY003
        return self


class CSVRowSource(Source):
    """A complete captured row that can be read without reopening its document."""

    source_id: str
    document_id: str
    primary_key: str
    primary_key_value: str
    columns: tuple[str, ...]
    values: tuple[str, ...]


def _validate_columns(columns: tuple[str, ...], primary_key: str) -> None:
    if not columns or any(not column.strip() for column in columns):
        raise ValueError("CSV headers must not be blank")  # noqa: TRY003
    if len(set(columns)) != len(columns):
        raise ValueError("CSV headers must be unique")  # noqa: TRY003
    if not primary_key or primary_key not in columns:
        raise ValueError("CSV primary key must exactly match a header")  # noqa: TRY003


def parse_csv_document(value: CSVDocumentInput) -> tuple[CSVRowInput, ...]:
    """Validate the entire document before returning any captured records."""
    if not value.document_id or value.document_id != value.document_id.strip():
        raise ValueError("document_id must be nonempty and have no surrounding whitespace")  # noqa: TRY003
    try:
        text = value.content.decode("utf-8-sig", errors="strict")
        reader = csv.reader(io.StringIO(text, newline=""), strict=True)
        columns = tuple(next(reader, ()))
        _validate_columns(columns, value.primary_key)
        key_index = columns.index(value.primary_key)
        rows: list[CSVRowInput] = []
        keys: set[str] = set()
        for record in reader:
            if not record:
                continue
            if len(record) != len(columns):
                raise ValueError("CSV record width does not match the header")  # noqa: TRY003
            key = record[key_index]
            if key in keys:
                raise ValueError("CSV primary keys must be unique within a document")  # noqa: TRY003
            rows.append(
                CSVRowInput(
                    document_id=value.document_id,
                    primary_key=value.primary_key,
                    primary_key_value=key,
                    columns=columns,
                    values=tuple(record),
                )
            )
            keys.add(key)
    except (UnicodeError, csv.Error) as error:
        raise ValueError("invalid UTF-8 CSV document") from error  # noqa: TRY003
    return tuple(rows)


class _CSVRowAdapter:
    input_class: type[CSVRowInput] = CSVRowInput
    name = "csv-row"
    source_class: type[CSVRowSource] = CSVRowSource

    async def resolve(self, value: CSVRowInput, /) -> CSVRowSource:
        identity = csv_row_source_id(value.document_id, value.primary_key_value)
        return CSVRowSource(
            name=identity,
            source_id=identity,
            materialization=SourceMaterialization.CAPTURED,
            **value.model_dump(),
        )

    async def read(self, source: CSVRowSource, /) -> CSVRowInput:
        return CSVRowInput.model_validate(source.model_dump())


class _CSVRowTextProjection:
    name = TEXT_EVIDENCE_PROJECTION_KEY.name
    version = TEXT_EVIDENCE_PROJECTION_KEY.version
    source_class: type[CSVRowSource] = CSVRowSource
    output_class: type[BaseModel] = TextEvidence

    def project(self, source: CSVRowSource, /) -> TextEvidence:
        row = dict(zip(source.columns, source.values, strict=True))
        return TextEvidence(
            source_type="csv-row",
            source_id=source.source_id,
            content=json.dumps(row, ensure_ascii=False, separators=(",", ":")),
            metadata={
                "document_id": source.document_id,
                "primary_key": source.primary_key,
                "primary_key_value": source.primary_key_value,
                "columns": list(source.columns),
                "row": row,
            },
        )


CSV_ROW_SOURCE_DEFINITION = AdapterSourceDefinition[CSVRowInput, CSVRowSource, CSVRowInput](
    _CSVRowAdapter(), projections=(_CSVRowTextProjection(),)
)

__all__ = [
    "CSV_ROW_SOURCE_DEFINITION",
    "CSVDocumentInput",
    "CSVRowInput",
    "CSVRowSource",
    "csv_row_source_id",
    "parse_csv_document",
]
