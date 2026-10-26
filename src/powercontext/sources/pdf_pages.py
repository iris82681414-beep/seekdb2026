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

"""Complete PDF page evidence with lexical retrieval through scoped Memory APIs."""

from __future__ import annotations

import hashlib
import io
import math
import re
from collections import Counter
from typing import TYPE_CHECKING
from urllib.parse import quote

from pydantic import BaseModel, Field, field_validator
from pypdf import PageObject, PdfReader
from typing_extensions import override

from powercontext.builtin.artifacts.memory.models import MemoryCitation, MemoryHit
from powercontext.builtin.runtime.models import GetMemoryEntryRequest, SearchMemoryRequest
from powercontext.limits import MAX_SOURCE_ID_LENGTH
from powercontext.sources.definitions import AdapterSourceDefinition
from powercontext.sources.models import Source, SourceMaterialization
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence

if TYPE_CHECKING:
    from pypdf._text_extraction._layout_mode import Font

    from powercontext.builtin.runtime.application import ScopedMemoryApplication

_TOKEN = re.compile(r"[a-z]+|\d+")
_STOP_WORDS = frozenset([
    "a",
    "an",
    "the",
    "and",
    "or",
    "for",
    "of",
    "to",
    "in",
    "on",
    "at",
    "by",
    "from",
    "with",
    "as",
    "is",
    "are",
    "was",
    "were",
    "be",
    "been",
    "being",
    "what",
    "which",
    "who",
    "how",
    "why",
    "when",
    "where",
    "do",
    "does",
    "did",
    "can",
    "could",
    "should",
    "would",
    "has",
    "have",
    "had",
    "its",
    "their",
    "that",
    "this",
    "these",
    "those",
    "any",
    "all",
    "not",
    "only",
    "if",
    "then",
    "than",
    "into",
    "using",
    "use",
    "used",
    "answer",
    "response",
    "respond",
    "question",
    "give",
    "calculate",
    "assuming",
    "perspective",
    "analyst",
    "details",
    "shown",
    "based",
    "relying",
    "directly",
    "outlined",
    "explicitly",
    "primarily",
    "important",
    "reported",
    "define",
    "state",
    "quantity",
    "fy",
    "usd",
    "million",
    "millions",
    "percent",
    "percentage",
    "please",
    "following",
    "information",
    "provided",
    "within",
    "round",
    "decimal",
    "places",
    "place",
    "units",
    "two",
    "one",
    "billions",
    "billion",
    "investment",
    "banking",
    "banker",
    "financial",
    "useful",
    "relevant",
    "metric",
    "company",
    "like",
    "explain",
    "your",
    "you",
    "we",
    "our",
    "it",
    "s",
    "year",
    "end",
    "data",
    "profile",
    "improving",
])
# General accounting vocabulary expands terminology, never document-specific answers.
_FINANCIAL_TERMS = (
    frozenset({
        "geography",
        "geographies",
        "geographic",
        "geographical",
        "region",
        "regional",
        "regions",
        "segment",
        "segments",
        "countries",
    }),
    frozenset({"legal", "litigation", "lawsuit", "lawsuits", "proceedings", "battles"}),
    frozenset({"capex", "expenditures", "purchases", "property", "equipment"}),
    frozenset({"ebitda", "depreciation", "amortization", "operating", "income"}),
    frozenset({"profit", "profits", "earnings", "income"}),
    frozenset({"restructuring", "charges", "costs"}),
    frozenset({"revenue", "revenues", "sales"}),
    frozenset({"cash", "flow", "flows", "activities"}),
)
_ACCOUNTING_CONCEPTS = (
    (r"\b(?:ppne|ppe|pp\s*&\s*e|fixed\s+assets?)\b", "property plant equipment assets balance sheet"),
    (r"\b(?:ar|accounts?\s+receivables?)\b", "accounts trade receivables balance sheet"),
    (r"\b(?:working\s+capital|quick\s+ratio|liquidity)\b", "balance sheet current assets liabilities cash receivables"),
    (r"\binventory\s+turnover\b", "inventory inventories cost goods sales balance sheet income operations"),
    (
        r"\b(?:liquidat(?:e[ds]?|ion|ing)|book\s+value)\b",
        "tangible book value tbvps bvps common equity shareholders shares",
    ),
    (r"\b(?:gross|operating)\s+margin\b", "income operations revenue revenues sales cost profit operating"),
    (r"\b(?:cogs|cost\s+of\s+goods)\b", "cost goods sales revenue"),
    (r"\bcapital[ -]intensive\b", "property plant equipment assets revenue revenues capital expenditures cash flows"),
    (r"\b(?:ebit|ebitda|ebitdar)\b", "ebit ebitda ebitdar income operating cash flow depreciation amortization rent"),
    (r"\bcash\s*flow\s+conversion\b", "income operations cash flow depreciation amortization"),
    (r"\btop[ -]?line\b", "revenue revenues sales growth"),
    (r"\b(?:ceo|chief\s+executive)\b", "chief executive officer president appointment succession experience"),
    (
        r"\b(?:acquired|acquisitions?|business\s+combinations?)\b",
        "acquired acquisition acquisitions business combinations",
    ),
    (r"\b(?:products?|services?)\s+categor(?:y|ies)\b", "business segment segments products services revenue"),
    (r"\b(?:customers?|clients?)\b", "customers customer clients client concentration contracts revenue sales"),
    (r"\bcyclic(?:al|ality)?\b", "cyclical cyclicality cycle demand economic cycles"),
)
_STATEMENT_INTENTS = (
    (
        r"\b(?:balance\s+sheet|financial\s+position|quick\s+ratio|working\s+capital|ppne|ppe|pp\s*&\s*e|"
        r"net\s+ar|asset\s+turnover|inventory\s+turnover|capital[ -]intensive)\b",
        r"balance\s+sheets?|statements?\s+of\s+financial\s+position",
        "balance sheet financial position",
    ),
    (
        r"\b(?:income\s+statement|statement\s+of\s+(?:income|operations)|p\s*&\s*l|"
        r"(?:gross|operating)\s+margin|net\s+income|effective\s+tax\s+rate|ebitdar?|asset\s+turnover|"
        r"inventory\s+turnover|capital[ -]intensive)\b",
        r"(?:statements?\s+of\s+(?:(?:net|comprehensive)\s+)*(?:income|earnings|operations)|income\s+statements?)",
        "income revenues earnings operations",
    ),
    (
        r"\b(?:cash\s*flows?|capex|capital\s+expenditures?|ebitdar?|cash\s+dividends?)\b",
        r"statements?\s+of\s+cash\s+flows?",
        "cash flow activities",
    ),
    (
        r"\b(?:products?|services?)\s+categor(?:y|ies)\b",
        r"(?:summary\s+of\s+)?(?:business\s+)?segment\s+(?:data|information)",
        "segment revenues sales",
    ),
)


class PDFDocumentInput(BaseModel):
    document_id: str
    content: bytes

    @field_validator("document_id")
    @classmethod
    def require_document_id(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("PDF document ID must not be blank")  # noqa: TRY003
        return value


class PDFPageInput(BaseModel):
    document_id: str
    document_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    page_number: int = Field(strict=True, ge=1)
    text: str

    @field_validator("document_id")
    @classmethod
    def require_document_id(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("PDF document ID must not be blank")  # noqa: TRY003
        return value


class PDFPageSource(Source):
    document_id: str
    document_sha256: str
    page_number: int
    text: str


class _LayoutPage(PageObject):
    @override
    def _layout_mode_fonts(self) -> dict[str, Font]:
        fonts = super()._layout_mode_fonts()
        # Decode spacer glyphs before grouping positioned text runs. A font may
        # map a spacer to LF or NBSP even inside one displayed table label.
        for font in fonts.values():
            for key, value in tuple(font.character_map.items()):
                if isinstance(value, str):
                    font.character_map[key] = value.translate({10: None, 13: None, 160: None})
        return fonts


def _page_text(page: PageObject) -> str:
    plain = page.extract_text() or ""
    text = plain
    try:
        layout_page = _LayoutPage(page.pdf, page.indirect_reference)
        layout_page.update(page)
        layout = layout_page.extract_text(extraction_mode="layout", layout_mode_strip_rotated=False) or ""
        # Layout mode does not handle every font or transformation. Keep the
        # original extraction whenever its non-whitespace characters differ.
        if Counter(char for char in plain if not char.isspace()) == Counter(
            char for char in layout if not char.isspace()
        ):
            text = layout
    except Exception:
        text = plain
    return "\n".join(" ".join(line.split()) for line in text.replace("\u00a0", "").splitlines() if line.strip())


def parse_pdf_document(value: PDFDocumentInput) -> tuple[PDFPageInput, ...]:
    """Extract every physical page atomically, including legitimate blank pages."""
    if not value.content:
        raise ValueError("PDF document must not be empty")  # noqa: TRY003
    digest = hashlib.sha256(value.content).hexdigest()
    try:
        reader = PdfReader(io.BytesIO(value.content), strict=True)
        if reader.is_encrypted and not reader.decrypt(""):
            raise ValueError("PDF requires a nonempty password")  # noqa: TRY003, TRY301
        return tuple(
            PDFPageInput(
                document_id=value.document_id,
                document_sha256=digest,
                page_number=number,
                text=_page_text(page),
            )
            for number, page in enumerate(reader.pages, start=1)
        )
    except Exception as error:
        raise ValueError("PDF could not be completely parsed") from error  # noqa: TRY003


class _PDFPageAdapter:
    input_class: type[PDFPageInput] = PDFPageInput
    name = "pdf-page"
    source_class: type[PDFPageSource] = PDFPageSource

    async def resolve(self, value: PDFPageInput, /) -> PDFPageSource:
        identity = f"pdf:{quote(value.document_id, safe='')}:{value.document_sha256}:{value.page_number}"
        if len(identity) > MAX_SOURCE_ID_LENGTH:
            raise ValueError("PDF page identity exceeds the source ID limit")  # noqa: TRY003
        return PDFPageSource(name=identity, materialization=SourceMaterialization.CAPTURED, **value.model_dump())

    async def read(self, source: PDFPageSource, /) -> PDFPageInput:
        return PDFPageInput.model_validate(source.model_dump())


class _PDFPageProjection:
    name = TEXT_EVIDENCE_PROJECTION_KEY.name
    version = TEXT_EVIDENCE_PROJECTION_KEY.version
    source_class: type[PDFPageSource] = PDFPageSource
    output_class: type[BaseModel] = TextEvidence

    def project(self, source: PDFPageSource, /) -> TextEvidence:
        return TextEvidence(
            source_type="pdf-page",
            source_id=source.name,
            content=f"{source.document_id}\n{source.text}",
            metadata={
                "document_id": source.document_id,
                "document_sha256": source.document_sha256,
                "page_number": source.page_number,
            },
        )


PDF_PAGE_SOURCE_DEFINITION = AdapterSourceDefinition[PDFPageInput, PDFPageSource, PDFPageInput](
    _PDFPageAdapter(), projections=(_PDFPageProjection(),)
)


def _stem(term: str) -> str:
    if len(term) > 4 and term.endswith("ies"):
        return term[:-3] + "y"
    if len(term) > 3 and term.endswith("s") and not term.endswith("ss"):
        return term[:-1]
    return term


def _query_variants(question: str) -> tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]:
    normalized = re.sub(r"\bcashflow\b", "cash flow", question.lower())
    terms = tuple(dict.fromkeys(term for term in _TOKEN.findall(normalized) if term not in _STOP_WORDS))
    expanded = set(terms)
    queries = [question, " ".join(terms)]
    topics = []
    for group in _FINANCIAL_TERMS:
        if group.intersection(terms):
            expanded.update(group)
            topics.append(tuple(sorted(group)))
    for pattern, vocabulary in _ACCOUNTING_CONCEPTS:
        if re.search(pattern, normalized):
            expanded.update(vocabulary.split())
            topics.append(tuple(vocabulary.split()))
    for intent, _heading, vocabulary in _STATEMENT_INTENTS:
        if re.search(intent, normalized):
            topics.append(tuple(vocabulary.split()))
    query_stems = {_stem(term) for term in terms}
    topics = [
        tuple(
            sorted(
                topic,
                key=lambda term: (
                    term not in terms,
                    _stem(term) not in query_stems,
                    not any(term.startswith(stem) for stem in query_stems),
                ),
            )
        )
        for topic in topics
    ]
    # Short alternative queries admit either synonym under Memory's lexical
    # coverage policy. Round-robin topics retain several accounting facets.
    for start in range(0, max((len(topic) for topic in topics), default=0), 2):
        queries.extend(" ".join(topic[start : start + 2]) for topic in topics if start < len(topic))
    queries.extend(f"{term} {_stem(term)}" for term in terms if len(term) > 4 and _stem(term) != term)
    if len(terms) > 8:
        queries.extend(" ".join(terms[start : start + 6]) for start in range(0, len(terms), 6))
    return terms, tuple(sorted(expanded - set(terms))), tuple(dict.fromkeys(query for query in queries if query))[:10]


def _statement_heading(text: str, heading: str) -> re.Match[str] | None:
    header = text.split("\n", 1)[-1][:1200]
    matches = re.finditer(
        r"(?im)^\s*(?:(?:consolidated|combined|condensed|unaudited|and)\s*){0,5}(?:"
        + heading.replace(r"\s+", r"\s*")
        + r")\b",
        header,
    )
    for match in matches:
        tail = header[match.end() :].split("\n", 1)[0]
        if not re.search(r"(?<!\d)\d{1,3}\s*$", tail):
            return match
    return None


def _report_period(text: str) -> re.Match[str] | None:
    return re.search(r"(?<!\d)((?:19|20)\d{2})(?:[_ -]?q([1-4]))?", text.split("\n", 1)[0].lower())


def _cover_statements(
    ranked: tuple[MemoryHit, ...],
    headings: list[str],
    requested_years: set[int],
    requested_quarter: str | None,
    question: str,
) -> tuple[MemoryHit, ...]:
    question_words = re.findall(r"[a-z0-9]+", question.lower())
    named_labels = {
        "".join(question_words[start:end])
        for start in range(len(question_words))
        for end in range(start + 1, min(start + 5, len(question_words)) + 1)
    }
    selected = []
    selected_ranks = set()
    for heading in headings:
        matches = []
        for rank, hit in enumerate(ranked):
            match = _statement_heading(hit.text, heading)
            if match is None or rank in selected_ranks:
                continue
            period = _report_period(hit.text)
            document = hit.text.split("\n", 1)[0].lower()
            label = re.sub(r"[^a-z0-9]", "", document[: period.start()] if period else document)
            named_document = label in named_labels
            latest_requested = bool(period and requested_years and int(period.group(1)) == max(requested_years))
            quarter_match = bool(period and requested_quarter and period.group(2) == requested_quarter)
            primary_income = "comprehensive" not in match.group(0).lower()
            consolidated = bool(re.search(r"consolidated|combined|condensed", match.group(0), re.IGNORECASE))
            matches.append((
                named_document,
                latest_requested,
                quarter_match,
                primary_income,
                consolidated,
                -match.start(),
                -rank,
                hit,
            ))
        if matches:
            best = max(matches, key=lambda item: item[:7])
            selected.append(best[-1])
            selected_ranks.add(-best[-2])
    # A ratio can require several different statements. Keep one primary page
    # per requested statement before filling the remaining lexical ranking.
    return tuple(selected) + tuple(hit for rank, hit in enumerate(ranked) if rank not in selected_ranks)


def _rerank_pages(
    hits: tuple[MemoryHit, ...], terms: tuple[str, ...], expanded: tuple[str, ...], *, question: str = ""
) -> tuple[MemoryHit, ...]:
    documents = [Counter(_stem(term) for term in _TOKEN.findall(hit.text.lower())) for hit in hits]
    frequencies: Counter[str] = Counter()
    for document in documents:
        frequencies.update(document.keys())
    weights = dict.fromkeys((_stem(term) for term in expanded), 0.4)
    weights.update({_stem(term): 0.15 if term.isdigit() else 1.0 for term in terms})
    average_length = sum(sum(document.values()) for document in documents) / max(len(documents), 1)
    requested_years = {int(year) for year in re.findall(r"(?<!\d)((?:19|20)\d{2})(?!\d)", question)}
    quarter = re.search(r"(?:\b|(?<=\d))q([1-4])\b|\bq([1-4])(?=(?:19|20)\d{2})", question.lower())
    requested_quarter = next((value for value in quarter.groups() if value), None) if quarter else None
    headings = [heading for intent, heading, _vocabulary in _STATEMENT_INTENTS if re.search(intent, question.lower())]
    adjusted_metric = bool(re.search(r"\b(?:adjusted|non[ -]?gaap|ebitdar)\b", question, re.IGNORECASE))
    scores = []
    for position, (hit, document) in enumerate(zip(hits, documents, strict=True)):
        length = sum(document.values())
        score = 0.0
        for term, weight in weights.items():
            count = document[term]
            if count:
                inverse_frequency = math.log1p((len(documents) - frequencies[term] + 0.5) / (frequencies[term] + 0.5))
                saturation = count * 2.2 / (count + 1.2 * (0.25 + 0.75 * length / max(average_length, 1)))
                score += weight * inverse_frequency * saturation
        if any(_statement_heading(hit.text, heading) for heading in headings):
            score *= 2.0
        # The projection's first line is the caller's document ID. When it
        # explicitly names a report period, use it as a soft preference; body
        # years often describe historical comparisons or future obligations.
        period = _report_period(hit.text)
        if requested_years and period and int(period.group(1)) in requested_years:
            score *= 1.4
            if requested_quarter and period.group(2) == requested_quarter:
                score *= 1.4
        # Earnings releases commonly carry the reported non-GAAP results,
        # while annual reports also contain definitions and risk discussion.
        # Treat an explicit report label as a soft preference, like its period.
        if adjusted_metric and re.search(r"(?:^|[_ -])earnings(?:[_ -]|$)", hit.text.split("\n", 1)[0], re.IGNORECASE):
            score *= 1.5
        scores.append((score, hit.score, -position, hit))
    ranked = tuple(item[-1] for item in sorted(scores, key=lambda item: item[:3], reverse=True))
    return _cover_statements(ranked, headings, requested_years, requested_quarter, question)


async def search_pdf_pages(
    memory: ScopedMemoryApplication, question: str, limit: int = 10
) -> tuple[MemoryCitation, ...]:
    """Retrieve and rerank real Memory hits, returning one exact citation per page."""
    if type(limit) is not int or limit < 1:
        raise ValueError("PDF search limit must be a positive integer")  # noqa: TRY003
    if not question.strip():
        return ()
    terms, expanded, queries = _query_variants(question)
    candidates: dict[tuple[str, str], MemoryHit] = {}
    for query in queries:
        result = await memory.search(SearchMemoryRequest(query=query, mode="fts", limit=min(max(limit * 6, 60), 200)))
        for hit in result.hits:
            candidates.setdefault((hit.entry_id, hit.entry_version_id), hit)
    citations: list[MemoryCitation] = []
    pages: set[str] = set()
    for hit in _rerank_pages(tuple(candidates.values()), terms, expanded, question=question):
        citation = MemoryCitation(
            memory_ref=hit.memory_ref, entry_id=hit.entry_id, entry_version_id=hit.entry_version_id
        )
        record = await memory.get(GetMemoryEntryRequest(citation=citation))
        source_pages = {source.source_id for source in record.entry.sources if source.source_type == "pdf-page"}
        if len(source_pages) != 1 or source_pages.intersection(pages):
            continue
        pages.update(source_pages)
        citations.append(citation)
        if len(citations) == limit:
            break
    return tuple(citations)


__all__ = [
    "PDF_PAGE_SOURCE_DEFINITION",
    "PDFDocumentInput",
    "PDFPageInput",
    "PDFPageSource",
    "parse_pdf_document",
    "search_pdf_pages",
]
