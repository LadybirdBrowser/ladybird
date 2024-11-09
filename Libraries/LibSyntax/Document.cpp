/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibSyntax/Document.h>

namespace Syntax {

size_t TextDocumentLine::first_non_whitespace_column() const
{
    for (size_t i = 0; i < length(); ++i) {
        auto code_point = code_points()[i];
        if (!is_ascii_space(code_point))
            return i;
    }
    return length();
}

Optional<size_t> TextDocumentLine::last_non_whitespace_column() const
{
    for (ssize_t i = length() - 1; i >= 0; --i) {
        auto code_point = code_points()[i];
        if (!is_ascii_space(code_point))
            return i;
    }
    return {};
}

bool TextDocumentLine::ends_in_whitespace() const
{
    if (!length())
        return false;
    return is_ascii_space(code_points()[length() - 1]);
}

bool TextDocumentLine::can_select() const
{
    if (is_empty())
        return false;
    for (size_t i = 0; i < length(); ++i) {
        auto code_point = code_points()[i];
        if (code_point != '\n' && code_point != '\r' && code_point != '\f' && code_point != '\v')
            return true;
    }
    return false;
}

size_t TextDocumentLine::leading_spaces() const
{
    size_t count = 0;
    for (; count < m_text.size(); ++count) {
        if (m_text[count] != ' ') {
            break;
        }
    }
    return count;
}

ByteString TextDocumentLine::to_utf8() const
{
    StringBuilder builder;
    builder.append(view());
    return builder.to_byte_string();
}

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
    m_text.clear();
    document.update_views({});
}

void TextDocumentLine::set_text(Document& document, Vector<u32> const text)
{
    m_text = move(text);
    document.update_views({});
}

bool TextDocumentLine::set_text(Document& document, StringView text)
{
    if (text.is_empty()) {
        clear(document);
        return true;
    }
    m_text.clear();
    Utf8View utf8_view(text);
    if (!utf8_view.validate()) {
        return false;
    }
    for (auto code_point : utf8_view)
        m_text.append(code_point);
    document.update_views({});
    return true;
}

void TextDocumentLine::append(Document& document, u32 const* code_points, size_t length)
{
    if (length == 0)
        return;
    m_text.append(code_points, length);
    document.update_views({});
}

void TextDocumentLine::append(Document& document, u32 code_point)
{
    insert(document, length(), code_point);
}

void TextDocumentLine::prepend(Document& document, u32 code_point)
{
    insert(document, 0, code_point);
}

void TextDocumentLine::insert(Document& document, size_t index, u32 code_point)
{
    if (index == length()) {
        m_text.append(code_point);
    } else {
        m_text.insert(index, code_point);
    }
    document.update_views({});
}

void TextDocumentLine::remove(Document& document, size_t index)
{
    if (index == length()) {
        m_text.take_last();
    } else {
        m_text.remove(index);
    }
    document.update_views({});
}

void TextDocumentLine::remove_range(Document& document, size_t start, size_t length)
{
    VERIFY(length <= m_text.size());

    Vector<u32> new_data;
    new_data.ensure_capacity(m_text.size() - length);
    for (size_t i = 0; i < start; ++i)
        new_data.append(m_text[i]);
    for (size_t i = (start + length); i < m_text.size(); ++i)
        new_data.append(m_text[i]);
    m_text = move(new_data);
    document.update_views({});
}

void TextDocumentLine::keep_range(Document& document, size_t start_index, size_t length)
{
    VERIFY(start_index + length < m_text.size());

    Vector<u32> new_data;
    new_data.ensure_capacity(m_text.size());
    for (size_t i = start_index; i <= (start_index + length); i++)
        new_data.append(m_text[i]);

    m_text = move(new_data);
    document.update_views({});
}

void TextDocumentLine::truncate(Document& document, size_t length)
{
    m_text.resize(length);
    document.update_views({});
}

TextDocumentSpan const* Document::span_at(TextPosition const& position) const
{
    for (auto& span : m_spans) {
        if (span.range.contains(position))
            return &span;
    }
    return nullptr;
}

void Document::set_spans(u32 span_collection_index, Vector<TextDocumentSpan> spans)
{
    m_span_collections.set(span_collection_index, move(spans));
    merge_span_collections();
}

struct SpanAndCollectionIndex {
    TextDocumentSpan span;
    u32 collection_index { 0 };
};

void Document::merge_span_collections()
{
    Vector<SpanAndCollectionIndex> sorted_spans;
    auto collection_indices = m_span_collections.keys();
    quick_sort(collection_indices);

    for (auto collection_index : collection_indices) {
        auto spans = m_span_collections.get(collection_index).value();
        for (auto span : spans) {
            sorted_spans.append({ move(span), collection_index });
        }
    }

    quick_sort(sorted_spans, [](SpanAndCollectionIndex const& a, SpanAndCollectionIndex const& b) {
        if (a.span.range.start() == b.span.range.start()) {
            return a.collection_index < b.collection_index;
        }
        return a.span.range.start() < b.span.range.start();
    });

    // The end of the TextRanges of spans are non-inclusive, i.e span range = [X,y).
    // This transforms the span's range to be inclusive, i.e [X,Y].
    auto adjust_end = [](TextDocumentSpan span) -> TextDocumentSpan {
        span.range.set_end({ span.range.end().line(), span.range.end().column() == 0 ? 0 : span.range.end().column() - 1 });
        return span;
    };

    Vector<SpanAndCollectionIndex> merged_spans;
    for (auto& span_and_collection_index : sorted_spans) {
        if (merged_spans.is_empty()) {
            merged_spans.append(span_and_collection_index);
            continue;
        }

        auto const& span = span_and_collection_index.span;
        auto last_span_and_collection_index = merged_spans.last();
        auto const& last_span = last_span_and_collection_index.span;

        if (adjust_end(span).range.start() > adjust_end(last_span).range.end()) {
            // Current span does not intersect with previous one, can simply append to merged list.
            merged_spans.append(span_and_collection_index);
            continue;
        }
        merged_spans.take_last();

        if (span.range.start() > last_span.range.start()) {
            SpanAndCollectionIndex first_part = last_span_and_collection_index;
            first_part.span.range.set_end(span.range.start());
            merged_spans.append(move(first_part));
        }

        SpanAndCollectionIndex merged_span;
        merged_span.collection_index = span_and_collection_index.collection_index;
        merged_span.span.range = { span.range.start(), min(span.range.end(), last_span.range.end()) };
        merged_span.span.is_skippable = span.is_skippable | last_span.is_skippable;
        merged_span.span.data = span.data ? span.data : last_span.data;
        merged_span.span.attributes.color = span_and_collection_index.collection_index > last_span_and_collection_index.collection_index ? span.attributes.color : last_span.attributes.color;
        merged_span.span.attributes.bold = span.attributes.bold | last_span.attributes.bold;
        merged_span.span.attributes.background_color = span.attributes.background_color.has_value() ? span.attributes.background_color.value() : last_span.attributes.background_color;
        merged_span.span.attributes.underline_color = span.attributes.underline_color.has_value() ? span.attributes.underline_color.value() : last_span.attributes.underline_color;
        merged_span.span.attributes.underline_style = span.attributes.underline_style.has_value() ? span.attributes.underline_style : last_span.attributes.underline_style;
        merged_spans.append(move(merged_span));

        if (span.range.end() == last_span.range.end())
            continue;

        if (span.range.end() > last_span.range.end()) {
            SpanAndCollectionIndex last_part = span_and_collection_index;
            last_part.span.range.set_start(last_span.range.end());
            merged_spans.append(move(last_part));
            continue;
        }

        SpanAndCollectionIndex last_part = last_span_and_collection_index;
        last_part.span.range.set_start(span.range.end());
        merged_spans.append(move(last_part));
    }

    m_spans.clear();
    TextDocumentSpan previous_span { .range = { TextPosition(0, 0), TextPosition(0, 0) }, .attributes = {} };
    for (auto span : merged_spans) {
        // Validate spans
        if (!span.span.range.is_valid()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Invalid span {} => ignoring", span.span.range);
            continue;
        }
        if (span.span.range.end() < span.span.range.start()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Span {} has negative length => ignoring", span.span.range);
            continue;
        }
        if (span.span.range.end() < previous_span.range.start()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Spans not sorted (Span {} ends before previous span {}) => ignoring", span.span.range, previous_span.range);
            continue;
        }
        if (span.span.range.start() < previous_span.range.end()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Span {} overlaps previous span {} => ignoring", span.span.range, previous_span.range);
            continue;
        }

        previous_span = span.span;
        m_spans.append(move(span.span));
    }
}

void Document::set_folding_regions(Vector<TextDocumentFoldingRegion> folding_regions)
{
    // Remove any regions that don't span at least 3 lines.
    // Currently, we can't do anything useful with them, and our implementation gets very confused by
    // single-line regions, so drop them.
    folding_regions.remove_all_matching([](TextDocumentFoldingRegion const& region) {
        return region.range.line_count() < 3;
    });

    quick_sort(folding_regions, [](TextDocumentFoldingRegion const& a, TextDocumentFoldingRegion const& b) {
        return a.range.start() < b.range.start();
    });

    for (auto& folding_region : folding_regions) {
        folding_region.line_ptr = &line(folding_region.range.start().line());

        // Map the new folding region to an old one, to preserve which regions were folded.
        // FIXME: This is O(n*n).
        for (auto const& existing_folding_region : m_folding_regions) {
            // We treat two folding regions as the same if they start on the same TextDocumentLine,
            // and have the same line count. The actual line *numbers* might change, but the pointer
            // and count should not.
            if (existing_folding_region.line_ptr
                && existing_folding_region.line_ptr == folding_region.line_ptr
                && existing_folding_region.range.line_count() == folding_region.range.line_count()) {
                folding_region.is_folded = existing_folding_region.is_folded;
                break;
            }
        }
    }

    // FIXME: Remove any regions that partially overlap another region, since these are invalid.

    m_folding_regions = move(folding_regions);

    if constexpr (TEXTEDITOR_DEBUG) {
        dbgln("Document got {} fold regions:", m_folding_regions.size());
        for (auto const& item : m_folding_regions) {
            dbgln("- {} (ptr: {:p}, folded: {})", item.range, item.line_ptr, item.is_folded);
        }
    }
}

Optional<TextDocumentFoldingRegion&> Document::folding_region_starting_on_line(size_t line)
{
    return m_folding_regions.first_matching([line](auto& region) {
        return region.range.start().line() == line;
    });
}

bool Document::line_is_visible(size_t line) const
{
    // FIXME: line_is_visible() gets called a lot.
    //        We could avoid a lot of repeated work if we saved this state on the TextDocumentLine.
    return !any_of(m_folding_regions, [line](auto& region) {
        return region.is_folded
            && line > region.range.start().line()
            && line < region.range.end().line();
    });
}

Vector<TextDocumentFoldingRegion const&> Document::currently_folded_regions() const
{
    Vector<TextDocumentFoldingRegion const&> folded_regions;

    for (auto& region : m_folding_regions) {
        if (region.is_folded) {
            // Only add this region if it's not contained within a previous folded region.
            // Because regions are sorted by their start position, and regions cannot partially overlap,
            // we can just see if it starts inside the last region we appended.
            if (!folded_regions.is_empty() && folded_regions.last().range.contains(region.range.start()))
                continue;

            folded_regions.append(region);
        }
    }

    return folded_regions;
}

}
