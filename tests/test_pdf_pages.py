# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
from __future__ import annotations

import io

from pypdf import PdfWriter
from pypdf.generic import DecodedStreamObject, DictionaryObject, NameObject

from powercontext.sources.pdf_pages import PDFDocumentInput, parse_pdf_document


def test_positioned_table_labels_keep_font_mapped_spacers_out_of_words() -> None:
    writer = PdfWriter()
    page = writer.add_blank_page(width=400, height=800)
    mapping = DecodedStreamObject()
    mapping.set_data(
        b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap "
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def "
        b"/CMapName /Test def /CMapType 2 def "
        b"1 begincodespacerange <00> <ff> endcodespacerange "
        b"2 beginbfchar <01> <000a> <02> <00a0> endbfchar "
        b"endcmap CMapName currentdict /CMap defineresource pop end end"
    )
    font = DictionaryObject({
        NameObject("/Type"): NameObject("/Font"),
        NameObject("/Subtype"): NameObject("/Type1"),
        NameObject("/BaseFont"): NameObject("/Helvetica"),
        NameObject("/Encoding"): NameObject("/WinAnsiEncoding"),
        NameObject("/ToUnicode"): writer._add_object(mapping),
    })
    page[NameObject("/Resources")] = DictionaryObject({
        NameObject("/Font"): DictionaryObject({NameObject("/F1"): writer._add_object(font)}),
    })
    content = DecodedStreamObject()
    content.set_data(
        b"BT /F1 12 Tf 1 0 0 1 30 700 Tm (Income\\001before\\001taxes) Tj "
        b"1 0 0 1 280 700 Tm (125) Tj "
        b"1 0 0 1 30 680 Tm (Cash\\002flow) Tj 1 0 0 1 280 680 Tm (75) Tj ET"
    )
    page[NameObject("/Contents")] = writer._add_object(content)
    data = io.BytesIO()
    writer.write(data)

    pages = parse_pdf_document(PDFDocumentInput(document_id="table", content=data.getvalue()))
    lines = pages[0].text.splitlines()
    assert any("Incomebeforetaxes" in line and "125" in line for line in lines)
    assert any("Cashflow" in line and "75" in line for line in lines)


def test_rotated_text_remains_on_its_physical_page() -> None:
    writer = PdfWriter()
    page = writer.add_blank_page(width=400, height=800)
    font = DictionaryObject({
        NameObject("/Type"): NameObject("/Font"),
        NameObject("/Subtype"): NameObject("/Type1"),
        NameObject("/BaseFont"): NameObject("/Helvetica"),
    })
    page[NameObject("/Resources")] = DictionaryObject({
        NameObject("/Font"): DictionaryObject({NameObject("/F1"): writer._add_object(font)}),
    })
    content = DecodedStreamObject()
    content.set_data(b"BT /F1 12 Tf 0 1 -1 0 200 100 Tm (Rotated balance 123) Tj ET")
    page[NameObject("/Contents")] = writer._add_object(content)
    writer.add_blank_page(width=400, height=800)
    data = io.BytesIO()
    writer.write(data)
    pages = parse_pdf_document(PDFDocumentInput(document_id="rotation", content=data.getvalue()))
    assert len(pages) == 2 and pages[0].page_number == 1 and pages[1].page_number == 2
    assert "Rotated balance 123" in pages[0].text
    assert pages[1].text == ""
