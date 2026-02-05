/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <LibSyntax/Document.h>
#include <LibSyntax/TextPosition.h>

namespace Syntax {

class HighlighterClient {
public:
    virtual ~HighlighterClient() = default;

    virtual Vector<TextDocumentSpan> const& spans() const = 0;
    virtual void set_span_at_index(size_t index, TextDocumentSpan span) = 0;
    virtual void clear_spans() { do_set_spans({}); }

    virtual Vector<TextDocumentFoldingRegion>& folding_regions() = 0;
    virtual Vector<TextDocumentFoldingRegion> const& folding_regions() const = 0;

    virtual ByteString highlighter_did_request_text() const = 0;
    virtual void highlighter_did_request_update() = 0;
    virtual Document& highlighter_did_request_document() = 0;
    virtual TextPosition highlighter_did_request_cursor() const = 0;
    virtual void highlighter_did_set_spans(Vector<TextDocumentSpan>) = 0;
    virtual void highlighter_did_set_folding_regions(Vector<TextDocumentFoldingRegion>) = 0;

    void do_set_spans(Vector<TextDocumentSpan> spans) { highlighter_did_set_spans(move(spans)); }
    void do_set_folding_regions(Vector<TextDocumentFoldingRegion> folding_regions) { highlighter_did_set_folding_regions(move(folding_regions)); }
    void do_update() { highlighter_did_request_update(); }

    ByteString get_text() const { return highlighter_did_request_text(); }
    Document& get_document() { return highlighter_did_request_document(); }
    TextPosition get_cursor() const { return highlighter_did_request_cursor(); }

    static constexpr auto span_collection_index = 0;
};

}
