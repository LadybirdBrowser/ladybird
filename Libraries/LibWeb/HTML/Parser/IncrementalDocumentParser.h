/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16View.h>
#include <LibJS/Heap/Cell.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::HTML {

class WEB_API IncrementalDocumentParser final : public JS::Cell {
    GC_CELL(IncrementalDocumentParser, JS::Cell);
    GC_DECLARE_ALLOCATOR(IncrementalDocumentParser);

public:
    static GC::Ref<IncrementalDocumentParser> create(GC::Ref<DOM::Document>, GC::Ref<Fetch::Infrastructure::Body>, URL::URL, Optional<MimeSniff::MimeType>);

    void start();
    void set_allow_declarative_shadow_roots(HTMLParser::AllowDeclarativeShadowRoots);

private:
    IncrementalDocumentParser(GC::Ref<DOM::Document>, GC::Ref<Fetch::Infrastructure::Body>, URL::URL, Optional<MimeSniff::MimeType>);

    virtual void visit_edges(Cell::Visitor&) override;

    void initialize_parser(ReadonlyBytes sniff_bytes);
    void start_incremental_read();
    void process_body_chunk(ByteBuffer);
    void process_end_of_body();
    void process_body_error(JS::Value);

    void append_decoded(Utf16View);
    void pump();
    void register_deferred_start();
    bool should_continue() const;

    GC::Ref<DOM::Document> m_document;
    GC::Ref<Fetch::Infrastructure::Body> m_body;
    URL::URL m_url;
    Optional<MimeSniff::MimeType> m_mime_type;
    HTMLParser::AllowDeclarativeShadowRoots m_allow_declarative_shadow_roots { HTMLParser::AllowDeclarativeShadowRoots::Yes };

    GC::Ptr<HTMLParser> m_parser;
    OwnPtr<TextCodec::StreamingDecoder> m_decoder;

    Utf16StringBuilder m_source;
};

}
