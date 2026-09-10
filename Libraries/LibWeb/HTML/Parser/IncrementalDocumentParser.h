/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
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
    ErrorOr<bool> change_encoding(StringView);

    void feed_parser();
    void finish_body();
    void decode_and_process(size_t length);
    void release_encoding_change_buffers();
    void discard_input_bytes();
    void append_decoded(Utf16View);
    void pump();
    void register_deferred_start();
    bool should_continue() const;
    bool tokenizer_will_consume_input_immediately() const;
    ReadonlyBytes decoded_input_bytes() const;
    ReadonlyBytes undecoded_input_bytes() const;

    GC::Ref<DOM::Document> m_document;
    GC::Ptr<Fetch::Infrastructure::Body> m_body;
    URL::URL m_url;
    Optional<MimeSniff::MimeType> m_mime_type;
    HTMLParser::AllowDeclarativeShadowRoots m_allow_declarative_shadow_roots { HTMLParser::AllowDeclarativeShadowRoots::Yes };

    GC::Ptr<HTMLParser> m_parser;
    OwnPtr<TextCodec::StreamingDecoder> m_decoder;

    // Everything received while a declaration could still change the encoding, and how much of it the decoder has
    // converted. The bytes past that point are the ones held back from a tokenizer that has not caught up.
    ByteBuffer m_input_bytes;
    size_t m_decoded_byte_count { 0 };
    Utf16StringBuilder m_source;
    bool m_body_is_exhausted { false };
    bool m_processed_implied_eof { false };
};

}
