# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
from __future__ import annotations

import asyncio
import hashlib
import importlib.util
from pathlib import Path
from typing import Literal

import pytest

from powercontext.builtin.artifacts.memory import MemoryCandidateRequest, MemoryEntryInput
from powercontext.builtin.persistence.seekdb import SeekDBConfig
from powercontext.builtin.persistence.sqlite import SQLiteConfig
from powercontext.builtin.runtime import (
    BuiltinConfig,
    GetMemoryEntryRequest,
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
from powercontext.sources.pdf_pages import PDF_PAGE_SOURCE_DEFINITION, PDFPageInput, search_pdf_pages
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence


class _LiteralPages:
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


@pytest.mark.parametrize("compact_headings", [False, True])
@pytest.mark.parametrize("backend", ["sqlite", "seekdb"])
def test_financial_concepts_and_requested_report_period_retrieve_exact_page_citations(
    tmp_path: Path, compact_headings: bool, backend: Literal["sqlite", "seekdb"]
) -> None:
    if backend == "seekdb" and importlib.util.find_spec("pylibseekdb") is None:
        pytest.skip("native seekdb runtime is unavailable")

    async def scenario() -> None:
        config = BuiltinConfig(
            database=(
                SQLiteConfig(url=f"sqlite+aiosqlite:///{tmp_path / 'reports.db'}")
                if backend == "sqlite"
                else SeekDBConfig(path=tmp_path / "seekdb")
            )
        )
        registry = SourceDefinitionRegistry((PDF_PAGE_SOURCE_DEFINITION,))
        bodies = [
            ("REPORT_2018_10K", 1, "CONSOLIDATED BALANCE SHEETS\nProperty, plant and equipment, net 125\nAssets 400"),
            (
                "REPORT_2023Q2_10Q",
                1,
                "CONDENSED CONSOLIDATED BALANCE SHEETS\nCurrent assets 200\nCurrent liabilities 100",
            ),
            (
                "REPORT_2022_10K",
                1,
                "CONSOLIDATED STATEMENTS OF INCOME\nRevenues 500\nCostofsales 300\nOperatingincome 100\nNetincome 75",
            ),
            (
                "REPORT_2022_10K",
                2,
                "CONSOLIDATED STATEMENTS OF CASH FLOWS\nOperating cash flow 200\nDepreciation and amortization 25",
            ),
            (
                "REPORT_2022_10K",
                99,
                "Index to consolidated financial statements\n"
                "CONSOLIDATED STATEMENTS OF INCOME - For the years ended 2022 and 2021 .... 1\n"
                "CONSOLIDATED STATEMENTS OF CASH FLOWS - For the years ended 2022 and 2021 .... 2\n"
                "EBITDA operating income depreciation amortization cash flow statement",
            ),
            (
                "REPORT_2022_10K",
                100,
                "Notes to financial statements\nINCOME STATEMENT\n"
                "Operating margin, revenue, sales, cost, profit, income, earnings, depreciation and amortization",
            ),
        ]
        if compact_headings:
            bodies = [
                (document, number, heading.replace(" ", "") + "\n" + rows)
                for document, number, text in bodies
                for heading, _separator, rows in (text.partition("\n"),)
            ]
        bodies.extend(
            (
                "REPORT_2024_10K",
                page + 1,
                "Notes on property, plant and equipment and operating margin.\n"
                "Current assets, current liabilities and liquidity ratios.\n"
                "Revenue, sales, cost, profit, income and operations.\n"
                "A financial analyst using the details shown in the report compares 2018, 2022 and 2023.",
            )
            for page in range(32)
        )
        expected = {}
        source_by_page = {}
        async with open_builtin_runtime(config, candidate_pipeline=_LiteralPages()) as runtime:
            assert runtime.scopes is not None
            scope = await runtime.scopes.create(
                ScopeDraft(title="Reports", summary="Report period and accounting evidence", idempotency_key="reports")
            )
            await runtime.ingestion.register(manifest_for_definition(PDF_PAGE_SOURCE_DEFINITION))
            for document, number, text in bodies:
                page = PDFPageInput(
                    document_id=document,
                    document_sha256=hashlib.sha256(document.encode()).hexdigest(),
                    page_number=number,
                    text=text,
                )
                observation = project_source_for_transport(registry, await registry.resolve(page))
                expected[observation.name] = f"{document}\n{text}"
                source_by_page[document, number] = observation.name
                await runtime.ingestion.submit(
                    SubmitSourceObservation(scope_id=scope.scope_id, observation=observation)
                )
            memory = runtime.memory.for_scope(scope.scope_id)
            while (await memory.flush()).processed:
                pass

        cases = (
            ("What is year end FY2018 net PPNE? Use the details shown by a financial analyst.", "REPORT_2018_10K", 1),
            ("Does the Q2 of FY2023 quick ratio indicate a healthy liquidity profile?", "REPORT_2023Q2_10Q", 1),
            ("How did operating margin change in FY2022?", "REPORT_2022_10K", 1),
            ("How much revenue on the income statement?", "REPORT_2022_10K", 1),
        )
        async with open_builtin_runtime(config) as runtime:
            memory = runtime.memory.for_scope(scope.scope_id)
            for question, document, number in cases:
                citations = await search_pdf_pages(memory, question, limit=1)
                assert len(citations) == 1
                record = await memory.get(GetMemoryEntryRequest(citation=citations[0]))
                assert len(record.entry.sources) == 1
                assert record.entry.text == expected[record.entry.sources[0].source_id]
                assert record.entry.text == expected[source_by_page[document, number]]
            citations = await search_pdf_pages(
                memory,
                "Compute FY2022 unadjusted EBITDA using operating income and depreciation from the cash flow statement.",
                limit=2,
            )
            records = [await memory.get(GetMemoryEntryRequest(citation=citation)) for citation in citations]
            assert {record.entry.text for record in records} == {
                expected[source_by_page["REPORT_2022_10K", number]] for number in (1, 2)
            }

    asyncio.run(scenario())
