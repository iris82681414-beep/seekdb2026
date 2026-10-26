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
            ("REPORT_2021Q1_10Q", 1, "Selected capital-related metrics\nTBVPS 42"),
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
                3,
                "CONSOLIDATED BALANCE SHEETS\nInventories 80\nCurrent assets 200\nCurrent liabilities 100",
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
            ("SUPPLEMENT_2022_10K", 1, "Segment summary\nRegional sales\nNorth 10\nSouth 20"),
            (
                "CUSTOMER_NOTE_2022",
                1,
                "Distribution summary\nCustomer concentration by geography\nNorth America 60\nEurope 40",
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
            turnover = await search_pdf_pages(memory, "Calculate the FY2022 inventory turnover ratio.", limit=2)
            turnover_records = [await memory.get(GetMemoryEntryRequest(citation=citation)) for citation in turnover]
            assert {record.entry.text for record in turnover_records} == {
                expected[source_by_page["REPORT_2022_10K", number]] for number in (1, 3)
            }
            liquidation = await search_pdf_pages(
                memory,
                "How much could shareholders receive per share if assets were liquidated in Q1 of FY2021?",
                limit=1,
            )
            assert len(liquidation) == 1
            liquidation_record = await memory.get(GetMemoryEntryRequest(citation=liquidation[0]))
            assert liquidation_record.entry.text == expected[source_by_page["REPORT_2021Q1_10Q", 1]]
            regional = await search_pdf_pages(
                memory, "Compare topline, EBITDA and capital expenditures for each region.", limit=5
            )
            regional_records = [await memory.get(GetMemoryEntryRequest(citation=citation)) for citation in regional]
            assert expected[source_by_page["SUPPLEMENT_2022_10K", 1]] in {
                record.entry.text for record in regional_records
            }
            customers = await search_pdf_pages(memory, "Where are customers concentrated?", limit=1)
            assert len(customers) == 1
            customer_record = await memory.get(GetMemoryEntryRequest(citation=customers[0]))
            assert customer_record.entry.text == expected[source_by_page["CUSTOMER_NOTE_2022", 1]]

    asyncio.run(scenario())


@pytest.mark.parametrize("backend", ["sqlite", "seekdb"])
def test_statement_coverage_uses_the_named_company_in_a_mixed_scope(
    tmp_path: Path, backend: Literal["sqlite", "seekdb"]
) -> None:
    if backend == "seekdb" and importlib.util.find_spec("pylibseekdb") is None:
        pytest.skip("native seekdb runtime is unavailable")

    async def scenario() -> None:
        config = BuiltinConfig(
            database=(
                SQLiteConfig(url=f"sqlite+aiosqlite:///{tmp_path / 'mixed.db'}")
                if backend == "sqlite"
                else SeekDBConfig(path=tmp_path / "seekdb")
            )
        )
        registry = SourceDefinitionRegistry((PDF_PAGE_SOURCE_DEFINITION,))
        pages = (
            (
                "ACME_2022_10K",
                "ACME Corporation\nCONSOLIDATED STATEMENTS OF INCOME\n2022 2021\nRevenues 500\nOperating income 100",
            ),
            (
                "OTHER_2022_10K",
                "CONSOLIDATED STATEMENTS OF INCOME\n2022 2021\n"
                "Revenues 900\nOperating income 800\nIncome, sales, cost and profit",
            ),
            (
                "ACME_2023Q2_10Q",
                "ACME Corporation\nCONDENSED CONSOLIDATED BALANCE SHEETS\n2023\n"
                "Current assets 200\nCurrent liabilities 100",
            ),
            (
                "OTHER_2023Q2_10Q",
                "CONDENSED CONSOLIDATED BALANCE SHEETS\n2023\nCurrent assets 900\nCurrent liabilities 800",
            ),
        )
        async with open_builtin_runtime(config, candidate_pipeline=_LiteralPages()) as runtime:
            assert runtime.scopes is not None
            scope = await runtime.scopes.create(
                ScopeDraft(title="Mixed reports", summary="Different companies in one scope", idempotency_key="mixed")
            )
            await runtime.ingestion.register(manifest_for_definition(PDF_PAGE_SOURCE_DEFINITION))
            for document, text in pages:
                page = PDFPageInput(
                    document_id=document,
                    document_sha256=hashlib.sha256(document.encode()).hexdigest(),
                    page_number=1,
                    text=text,
                )
                observation = project_source_for_transport(registry, await registry.resolve(page))
                await runtime.ingestion.submit(
                    SubmitSourceObservation(scope_id=scope.scope_id, observation=observation)
                )
            memory = runtime.memory.for_scope(scope.scope_id)
            while (await memory.flush()).processed:
                pass

        async with open_builtin_runtime(config) as runtime:
            memory = runtime.memory.for_scope(scope.scope_id)
            for question, document, body in (
                ("How did ACME's operating margin change in FY2022?", *pages[0]),
                ("Does ACME have a healthy quick ratio in Q2 of FY2023?", *pages[2]),
            ):
                citations = await search_pdf_pages(memory, question, limit=1)
                assert len(citations) == 1
                record = await memory.get(GetMemoryEntryRequest(citation=citations[0]))
                assert record.entry.text == f"{document}\n{body}"
                assert len(record.entry.sources) == 1

    asyncio.run(scenario())


@pytest.mark.parametrize("backend", ["sqlite", "seekdb"])
def test_adjusted_metric_prefers_reported_results_over_annual_discussion(
    tmp_path: Path, backend: Literal["sqlite", "seekdb"]
) -> None:
    if backend == "seekdb" and importlib.util.find_spec("pylibseekdb") is None:
        pytest.skip("native seekdb runtime is unavailable")

    async def scenario() -> None:
        config = BuiltinConfig(
            database=(
                SQLiteConfig(url=f"sqlite+aiosqlite:///{tmp_path / 'results.db'}")
                if backend == "sqlite"
                else SeekDBConfig(path=tmp_path / "seekdb")
            )
        )
        registry = SourceDefinitionRegistry((PDF_PAGE_SOURCE_DEFINITION,))
        pages = (
            (
                "ACME_2022_10K",
                "Discussion of results\nAdjusted EBITDAR is used to assess performance. "
                "Adjusted EBITDAR excludes selected items and may differ from other companies' definitions.",
            ),
            (
                "ACME_2022Q4_EARNINGS",
                "Full year results\nAdjusted EBITDAR\n2022 2021\nNorth 120 100\nSouth 80 70\nTotal 200 170",
            ),
        )
        async with open_builtin_runtime(config, candidate_pipeline=_LiteralPages()) as runtime:
            assert runtime.scopes is not None
            scope = await runtime.scopes.create(
                ScopeDraft(title="Results", summary="Reported metrics and annual discussion", idempotency_key="results")
            )
            await runtime.ingestion.register(manifest_for_definition(PDF_PAGE_SOURCE_DEFINITION))
            for document, text in pages:
                page = PDFPageInput(
                    document_id=document,
                    document_sha256=hashlib.sha256(document.encode()).hexdigest(),
                    page_number=1,
                    text=text,
                )
                observation = project_source_for_transport(registry, await registry.resolve(page))
                await runtime.ingestion.submit(
                    SubmitSourceObservation(scope_id=scope.scope_id, observation=observation)
                )
            memory = runtime.memory.for_scope(scope.scope_id)
            while (await memory.flush()).processed:
                pass

        async with open_builtin_runtime(config) as runtime:
            memory = runtime.memory.for_scope(scope.scope_id)
            citations = await search_pdf_pages(memory, "What was ACME's adjusted EBITDAR for FY2022?", limit=1)
            assert len(citations) == 1
            record = await memory.get(GetMemoryEntryRequest(citation=citations[0]))
            assert record.entry.text == "\n".join(pages[1])
            assert len(record.entry.sources) == 1

    asyncio.run(scenario())


@pytest.mark.parametrize("backend", ["sqlite", "seekdb"])
def test_business_categories_and_customer_aliases_retrieve_the_reported_evidence(
    tmp_path: Path, backend: Literal["sqlite", "seekdb"]
) -> None:
    if backend == "seekdb" and importlib.util.find_spec("pylibseekdb") is None:
        pytest.skip("native seekdb runtime is unavailable")

    async def scenario() -> None:
        config = BuiltinConfig(
            database=(
                SQLiteConfig(url=f"sqlite+aiosqlite:///{tmp_path / 'business.db'}")
                if backend == "sqlite"
                else SeekDBConfig(path=tmp_path / "seekdb")
            )
        )
        registry = SourceDefinitionRegistry((PDF_PAGE_SOURCE_DEFINITION,))
        bodies = (
            "Summary of Business Segment Data\nRevenues 2022 2021\nIndustrial 70 60\nTransport 30 25\nTotal 100 85",
            "Business discussion\nProduct categories and service categories can change. "
            "The company reviews the revenue contribution of each product category during 2022. "
            "Primary customers influence product categories.",
            "Sales concentration\nMajor clients include North Rail and West Air under long-term contracts. "
            "These contracts account for most sales revenue.",
        )
        async with open_builtin_runtime(config, candidate_pipeline=_LiteralPages()) as runtime:
            assert runtime.scopes is not None
            scope = await runtime.scopes.create(
                ScopeDraft(
                    title="Business", summary="Business categories and counterparties", idempotency_key="business"
                )
            )
            await runtime.ingestion.register(manifest_for_definition(PDF_PAGE_SOURCE_DEFINITION))
            for number, body in enumerate(bodies, 1):
                page = PDFPageInput(
                    document_id="ACME_2022_10K",
                    document_sha256=hashlib.sha256(b"business report").hexdigest(),
                    page_number=number,
                    text=body,
                )
                observation = project_source_for_transport(registry, await registry.resolve(page))
                await runtime.ingestion.submit(
                    SubmitSourceObservation(scope_id=scope.scope_id, observation=observation)
                )
            memory = runtime.memory.for_scope(scope.scope_id)
            while (await memory.flush()).processed:
                pass

        async with open_builtin_runtime(config) as runtime:
            memory = runtime.memory.for_scope(scope.scope_id)
            for question, body, limit in (
                ("Do any product categories exceed 20% of ACME's FY2022 revenue?", bodies[0], 1),
                ("Who are the primary customers?", bodies[2], 2),
            ):
                citations = await search_pdf_pages(memory, question, limit=limit)
                records = [await memory.get(GetMemoryEntryRequest(citation=citation)) for citation in citations]
                assert "ACME_2022_10K\n" + body in {record.entry.text for record in records}
                assert all(len(record.entry.sources) == 1 for record in records)

    asyncio.run(scenario())
