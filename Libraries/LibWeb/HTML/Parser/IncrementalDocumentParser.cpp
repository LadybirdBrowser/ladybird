/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/Value.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/IncrementalDocumentParser.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(IncrementalDocumentParser);

GC::Ref<IncrementalDocumentParser> IncrementalDocumentParser::create(GC::Ref<DOM::Document> document, GC::Ref<Fetch::Infrastructure::Body> body, URL::URL url, Optional<MimeSniff::MimeType> mime_type)
{
    return document->realm().create<IncrementalDocumentParser>(document, body, move(url), move(mime_type));
}

IncrementalDocumentParser::IncrementalDocumentParser(GC::Ref<DOM::Document> document, GC::Ref<Fetch::Infrastructure::Body> body, URL::URL url, Optional<MimeSniff::MimeType> mime_type)
    : m_document(document)
    , m_body(body)
    , m_url(move(url))
    , m_mime_type(move(mime_type))
{
}

void IncrementalDocumentParser::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_body);
    visitor.visit(m_parser);
}

void IncrementalDocumentParser::start()
{
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // The user agent may wait for more bytes of the resource to be available while determining the
    // encoding. Body::wait_for_sniff_bytes waits until its sniff-byte threshold is available, or
    // until the stream closes.
    //
    // FIXME: The spec allows starting the parse after 500 ms or 1024 bytes, whichever comes first.
    // We only honor the byte threshold.
    auto parser = GC::Ref { *this };
    m_body->wait_for_sniff_bytes(GC::create_function(heap(), [parser](ReadonlyBytes sniff_bytes) {
        parser->initialize_parser(sniff_bytes);
    }));
}

void IncrementalDocumentParser::initialize_parser(ReadonlyBytes sniff_bytes)
{
    if (m_parser)
        return;

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-with-a-known-character-encoding
    // https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding
    auto encoding = m_document->has_encoding()
        ? m_document->encoding().value().to_byte_string()
        : run_encoding_sniffing_algorithm(m_document, sniff_bytes, m_mime_type);
    dbgln_if(HTML_PARSER_DEBUG, "The incremental HTML parser selected encoding '{}'", encoding);

    auto decoder = TextCodec::decoder_for(encoding);
    VERIFY(decoder.has_value());

    auto standardized_encoding = TextCodec::get_standardized_encoding(encoding);
    VERIFY(standardized_encoding.has_value());
    m_decoder = make<TextCodec::StreamingDecoder>(decoder.value());

    // https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding
    // The document's character encoding must immediately be set to the value returned from this
    // algorithm, at the same time as the user agent uses the returned value to select the decoder
    // to use for the input byte stream.
    m_document->set_encoding(MUST(String::from_utf8(standardized_encoding.value())));

    // FIXME: Implement the spec's "change the encoding while parsing" algorithm.
    m_document->set_url(m_url);
    m_parser = HTMLParser::create_with_open_input_stream(m_document);

    start_incremental_read();
}

void IncrementalDocumentParser::start_incremental_read()
{
    auto parser = GC::Ref { *this };
    m_body->incrementally_read(
        GC::create_function(heap(), [parser](ByteBuffer bytes) mutable {
            parser->process_body_chunk(move(bytes));
        }),
        GC::create_function(heap(), [parser] {
            parser->process_end_of_body();
        }),
        GC::create_function(heap(), [parser](JS::Value error) {
            parser->process_body_error(error);
        }),
        GC::Ref { m_document->realm().global_object() });
}

bool IncrementalDocumentParser::should_continue() const
{
    // NOTE: document.open() replaces m_document->parser() without aborting the old parser, so we have to stop feeding
    //       bytes once we're no longer the document's active parser.
    return m_parser && !m_parser->aborted() && m_document->parser() == m_parser;
}

void IncrementalDocumentParser::append_decoded(StringView decoded)
{
    m_source.append(decoded);
    m_parser->tokenizer().append_to_input_stream(decoded);
}

void IncrementalDocumentParser::process_body_chunk(ByteBuffer bytes)
{
    if (!should_continue())
        return;

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // Each task that the networking task source places on the task queue while fetching runs must
    // fill the parser's input byte stream with the fetched bytes and cause the HTML parser to
    // perform the appropriate processing of the input stream.
    auto decoded = m_decoder->to_utf8(bytes.bytes()).release_value_but_fixme_should_propagate_errors();
    append_decoded(decoded.bytes_as_string_view());
    pump();
}

void IncrementalDocumentParser::process_end_of_body()
{
    if (!should_continue())
        return;

    auto decoded = m_decoder->finish().release_value_but_fixme_should_propagate_errors();
    append_decoded(decoded.bytes_as_string_view());

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // When no more bytes are available, have the parser process the implied EOF character.
    m_document->set_source(m_source.to_string_without_validation());
    m_parser->tokenizer().close_input_stream();
    pump();
}

void IncrementalDocumentParser::process_body_error(JS::Value)
{
    dbgln("FIXME: Load html page with an error if incremental read of body failed.");
    HTMLParser::the_end(m_document, m_parser);
}

void IncrementalDocumentParser::register_deferred_start()
{
    if (m_document->has_deferred_parser_start())
        return;

    auto parser = GC::Ref { *this };
    m_document->set_deferred_parser_start(GC::create_function(heap(), [parser] {
        parser->pump();
    }));
}

void IncrementalDocumentParser::pump()
{
    if (!should_continue())
        return;

    if (!m_document->ready_to_run_scripts()) {
        register_deferred_start();
        return;
    }

    if (m_parser->stopped())
        return;

    // FIXME: Process link headers (read-html step 3, third paragraph) after the first parser pass.
    if (m_parser->tokenizer().is_input_stream_closed()) {
        m_parser->run_until_completion();
        return;
    }

    if (m_parser->is_paused())
        return;

    m_parser->run();
}

}
