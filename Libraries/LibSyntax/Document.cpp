/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibSyntax/Document.h>

namespace Syntax {

TextDocumentLine::TextDocumentLine(Document& document)
{
    clear(document);
}

TextDocumentLine::TextDocumentLine(Document& document, StringView text)
{
    set_text(document, text);
}

void TextDocumentLine::clear(Document& document)
{
    m_text = {};
    m_length = 0;
    document.update_views({});
}

bool TextDocumentLine::set_text(Document& document, StringView text)
{
    if (text.is_empty()) {
        clear(document);
        return true;
    }

    m_text = {};
    m_length = 0;

    Utf8View utf8_view(text);
    if (!utf8_view.validate())
        return false;

    m_text = String::from_utf8_without_validation(text.bytes());
    m_length = utf8_view.length();
    document.update_views({});
    return true;
}

void Document::set_spans(Vector<TextDocumentSpan> spans)
{
    quick_sort(spans, [](TextDocumentSpan const& a, TextDocumentSpan const& b) {
        return a.range.start() < b.range.start();
    });

    // The end of the TextRanges of spans are non-inclusive, i.e span range = [X,y).
    // This transforms the span's range to be inclusive, i.e [X,Y].
    auto adjust_end = [](TextDocumentSpan span) -> TextDocumentSpan {
        span.range.set_end({ span.range.end().line(), span.range.end().column() == 0 ? 0 : span.range.end().column() - 1 });
        return span;
    };

    Vector<TextDocumentSpan> merged_spans;
    for (auto& span : spans) {
        if (merged_spans.is_empty()) {
            merged_spans.append(span);
            continue;
        }

        auto last_span = merged_spans.last();

        if (adjust_end(span).range.start() > adjust_end(last_span).range.end()) {
            // Current span does not intersect with previous one, can simply append to merged list.
            merged_spans.append(span);
            continue;
        }
        merged_spans.take_last();

        if (span.range.start() > last_span.range.start()) {
            auto first_part = last_span;
            first_part.range.set_end(span.range.start());
            merged_spans.append(move(first_part));
        }

        TextDocumentSpan merged_span;
        merged_span.range = { span.range.start(), min(span.range.end(), last_span.range.end()) };
        merged_span.is_skippable = span.is_skippable | last_span.is_skippable;
        merged_span.data = span.data ? span.data : last_span.data;
        merged_span.attributes.color = last_span.attributes.color;
        merged_span.attributes.bold = span.attributes.bold | last_span.attributes.bold;
        merged_span.attributes.background_color = span.attributes.background_color.has_value() ? span.attributes.background_color.value() : last_span.attributes.background_color;
        merged_span.attributes.underline_color = span.attributes.underline_color.has_value() ? span.attributes.underline_color.value() : last_span.attributes.underline_color;
        merged_span.attributes.underline_style = span.attributes.underline_style.has_value() ? span.attributes.underline_style : last_span.attributes.underline_style;
        merged_spans.append(move(merged_span));

        if (span.range.end() == last_span.range.end())
            continue;

        if (span.range.end() > last_span.range.end()) {
            auto last_part = span;
            last_part.range.set_start(last_span.range.end());
            merged_spans.append(move(last_part));
            continue;
        }

        auto last_part = last_span;
        last_part.range.set_start(span.range.end());
        merged_spans.append(move(last_part));
    }

    m_spans.clear();
    TextDocumentSpan previous_span { .range = { TextPosition(0, 0), TextPosition(0, 0) }, .attributes = {} };
    for (auto span : merged_spans) {
        // Validate spans
        if (!span.range.is_valid()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Invalid span {} => ignoring", span.range);
            continue;
        }
        if (span.range.end() < span.range.start()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Span {} has negative length => ignoring", span.range);
            continue;
        }
        if (span.range.end() < previous_span.range.start()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Spans not sorted (Span {} ends before previous span {}) => ignoring", span.range, previous_span.range);
            continue;
        }
        if (span.range.start() < previous_span.range.end()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Span {} overlaps previous span {} => ignoring", span.range, previous_span.range);
            continue;
        }

        previous_span = span;
        m_spans.append(move(span));
    }
}

}
