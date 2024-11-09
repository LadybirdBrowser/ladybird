/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf32View.h>
#include <LibGfx/TextAttributes.h>
#include <LibSyntax/Forward.h>
#include <LibSyntax/TextRange.h>

namespace Syntax {

struct TextDocumentSpan {
    TextRange range;
    Gfx::TextAttributes attributes;
    u64 data { 0 };
    bool is_skippable { false };
};

struct TextDocumentFoldingRegion {
    TextRange range;
    bool is_folded { false };
    // This pointer is only used to identify that two TDFRs are the same.
    RawPtr<class TextDocumentLine> line_ptr;
};

class TextDocumentLine {
public:
    explicit TextDocumentLine(Document&);
    explicit TextDocumentLine(Document&, StringView);

    ByteString to_utf8() const;

    Utf32View view() const { return { code_points(), length() }; }
    u32 const* code_points() const { return m_text.data(); }
    size_t length() const { return m_text.size(); }
    bool set_text(Document&, StringView);
    void set_text(Document&, Vector<u32>);
    void append(Document&, u32);
    void prepend(Document&, u32);
    void insert(Document&, size_t index, u32);
    void remove(Document&, size_t index);
    void append(Document&, u32 const*, size_t);
    void truncate(Document&, size_t length);
    void clear(Document&);
    void remove_range(Document&, size_t start, size_t length);
    void keep_range(Document&, size_t start_index, size_t end_index);

    size_t first_non_whitespace_column() const;
    Optional<size_t> last_non_whitespace_column() const;
    bool ends_in_whitespace() const;
    bool can_select() const;
    bool is_empty() const { return length() == 0; }
    size_t leading_spaces() const;

private:
    // NOTE: This vector is null terminated.
    Vector<u32> m_text;
};

class Document : public RefCounted<Document> {
public:
    Document() = default;
    virtual ~Document() = default;

    void set_spans(u32 span_collection_index, Vector<TextDocumentSpan> spans);
    bool has_spans() const { return !m_spans.is_empty(); }
    Vector<TextDocumentSpan>& spans() { return m_spans; }
    Vector<TextDocumentSpan> const& spans() const { return m_spans; }
    void set_span_at_index(size_t index, TextDocumentSpan span) { m_spans[index] = move(span); }
    TextDocumentSpan const* span_at(TextPosition const&) const;

    void set_folding_regions(Vector<TextDocumentFoldingRegion>);
    bool has_folding_regions() const { return !m_folding_regions.is_empty(); }
    Vector<TextDocumentFoldingRegion>& folding_regions() { return m_folding_regions; }
    Vector<TextDocumentFoldingRegion> const& folding_regions() const { return m_folding_regions; }
    Optional<TextDocumentFoldingRegion&> folding_region_starting_on_line(size_t line);
    // Returns all folded FoldingRegions that are not contained inside another folded region.
    Vector<TextDocumentFoldingRegion const&> currently_folded_regions() const;
    // Returns true if any part of the line is currently visible. (Not inside a folded FoldingRegion.)
    bool line_is_visible(size_t line) const;

    virtual TextDocumentLine const& line(size_t line_index) const = 0;
    virtual TextDocumentLine& line(size_t line_index) = 0;

    virtual void update_views(Badge<TextDocumentLine>) = 0;

protected:
    HashMap<u32, Vector<TextDocumentSpan>> m_span_collections;
    Vector<TextDocumentSpan> m_spans;
    Vector<TextDocumentFoldingRegion> m_folding_regions;

private:
    void merge_span_collections();
};

}
