/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StringView.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Value.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/IncrementalDocumentParser.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(IncrementalDocumentParser);

static Utf16String utf16_string_from_standardized_encoding_label(StringView label)
{
    return Utf16String::from_ascii_without_validation(label.bytes());
}

GC::Ref<IncrementalDocumentParser> IncrementalDocumentParser::create(GC::Ref<DOM::Document> document, GC::Ref<Fetch::Infrastructure::Body> body, URL::URL url, Optional<MimeSniff::MimeType> mime_type)
{
    return GC::Heap::the().allocate<IncrementalDocumentParser>(document, body, move(url), move(mime_type));
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
    m_body->wait_for_sniff_bytes(GC::create_function(GC::Heap::the(), [parser](ReadonlyBytes sniff_bytes) {
        parser->initialize_parser(sniff_bytes);
    }));
}

void IncrementalDocumentParser::set_allow_declarative_shadow_roots(HTMLParser::AllowDeclarativeShadowRoots allow_declarative_shadow_roots)
{
    m_allow_declarative_shadow_roots = allow_declarative_shadow_roots;
    if (m_parser)
        m_parser->set_allow_declarative_shadow_roots(allow_declarative_shadow_roots);
}

void IncrementalDocumentParser::initialize_parser(ReadonlyBytes sniff_bytes)
{
    if (m_parser)
        return;

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-with-a-known-character-encoding
    // https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding
    Optional<StringView> standardized_encoding;
    auto encoding_confidence = EncodingConfidence::Certain;
    if (m_document->has_encoding()) {
        standardized_encoding = TextCodec::get_standardized_encoding(m_document->encoding().value());
    } else {
        auto [encoding, confidence] = run_encoding_sniffing_algorithm(m_document, sniff_bytes, m_mime_type);
        standardized_encoding = TextCodec::get_standardized_encoding(encoding);
        encoding_confidence = confidence;
    }
    VERIFY(standardized_encoding.has_value());
    dbgln_if(HTML_PARSER_DEBUG, "The incremental HTML parser selected encoding '{}'", standardized_encoding.value());

    m_decoder = make<TextCodec::StreamingDecoder>(standardized_encoding.value(), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement);

    // https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding
    // The document's character encoding must immediately be set to the value returned from this
    // algorithm, at the same time as the user agent uses the returned value to select the decoder
    // to use for the input byte stream.
    m_document->set_encoding(utf16_string_from_standardized_encoding_label(standardized_encoding.value()));

    m_document->set_url(m_url);
    m_parser = HTMLParser::create_with_open_input_stream(m_document, encoding_confidence);
    m_parser->set_change_encoding_callback(GC::create_function(GC::Heap::the(), [this](StringView encoding) {
        auto result = change_encoding(encoding);
        if (result.is_error()) {
            release_encoding_change_buffers();
            return false;
        }
        return result.release_value();
    }));
    m_parser->set_parsing_complete_callback(GC::create_function(GC::Heap::the(), [this]() {
        release_encoding_change_buffers();
    }));
    m_parser->set_ready_for_more_input_callback(GC::create_function(GC::Heap::the(), [this]() {
        feed_parser();
    }));
    m_parser->set_allow_declarative_shadow_roots(m_allow_declarative_shadow_roots);

    start_incremental_read();
}

void IncrementalDocumentParser::start_incremental_read()
{
    auto parser = GC::Ref { *this };
    m_body->incrementally_read(
        m_document->relevant_settings_object().realm(),
        GC::create_function(GC::Heap::the(), [parser](ByteBuffer bytes) mutable {
            parser->process_body_chunk(move(bytes));
        }),
        GC::create_function(GC::Heap::the(), [parser] {
            parser->process_end_of_body();
        }),
        GC::create_function(GC::Heap::the(), [parser](JS::Value error) {
            parser->process_body_error(error);
        }),
        GC::Ref { m_document->relevant_settings_object().realm().global_object() });
}

bool IncrementalDocumentParser::should_continue() const
{
    // NOTE: document.open() replaces m_document->parser() without aborting the old parser, so we have to stop feeding
    //       bytes once we're no longer the document's active parser.
    return m_parser && !m_parser->aborted() && m_document->parser() == m_parser;
}

ReadonlyBytes IncrementalDocumentParser::decoded_input_bytes() const
{
    return m_input_bytes.span().slice(0, m_decoded_byte_count);
}

ReadonlyBytes IncrementalDocumentParser::undecoded_input_bytes() const
{
    return m_input_bytes.span().slice(m_decoded_byte_count);
}

// pump() runs the tokenizer to the end of its input, so the only reasons decoded characters can be left unconsumed
// are the parser waiting for scripts to be allowed to run and the parser being paused on a parser-blocking script.
bool IncrementalDocumentParser::tokenizer_will_consume_input_immediately() const
{
    return m_document->ready_to_run_scripts() && !m_parser->is_paused();
}

void IncrementalDocumentParser::append_decoded(Utf16View decoded)
{
    m_source.append(decoded);
    m_parser->tokenizer().append_to_input_stream(decoded);
}

void IncrementalDocumentParser::process_body_chunk(ByteBuffer bytes)
{
    if (!should_continue()) {
        discard_input_bytes();
        return;
    }

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // Each task that the networking task source places on the task queue while fetching runs must
    // fill the parser's input byte stream with the fetched bytes and cause the HTML parser to
    // perform the appropriate processing of the input stream.
    if (m_input_bytes.is_empty())
        m_input_bytes = move(bytes);
    else
        m_input_bytes.append(bytes);

    feed_parser();
}

static size_t encoding_independent_prefix_length(ReadonlyBytes bytes)
{
    size_t length = 0;
    while (length < bytes.size()) {
        // Non-ASCII bytes may have different Unicode interpretations in different encodings. ESC is also
        // encoding-sensitive because it can change the state of an ISO-2022-JP decoder, affecting how
        // subsequent ASCII-range bytes are interpreted.
        auto byte = bytes[length];
        if (!is_ascii(byte) || byte == 0x1B)
            break;

        ++length;
    }
    return length;
}

void IncrementalDocumentParser::feed_parser()
{
    while (!undecoded_input_bytes().is_empty()) {
        if (!should_continue()) {
            discard_input_bytes();
            return;
        }

        auto undecoded_bytes = undecoded_input_bytes();
        auto length_to_decode = undecoded_bytes.size();

        if (m_parser->encoding_confidence() == EncodingConfidence::Tentative) {
            auto current_encoding = TextCodec::get_standardized_encoding(m_document->encoding().value());
            VERIFY(current_encoding.has_value());

            // Every byte of a UTF-16 stream is encoding-sensitive, and step 1 of the change-the-encoding algorithm
            // refuses to move away from UTF-16BE/LE anyway, so there is no declaration left to hold bytes for.
            if (!current_encoding->is_one_of_ignoring_ascii_case("UTF-16BE"sv, "UTF-16LE"sv)) {
                auto prefix_length = encoding_independent_prefix_length(undecoded_bytes);

                // Step 5 of the change-the-encoding algorithm only swaps the decoder for bytes that read the same
                // in both encodings, so converting an encoding-sensitive byte commits any declaration behind it to
                // the step 6 restart we do not implement. Holding them until the tokenizer catches up rules one out.
                if (prefix_length == 0 && !tokenizer_will_consume_input_immediately()) {
                    // Nothing is decoded this round, so pump() cannot arm the wake-up that brings us back. A parser
                    // paused on a script has the resume hook; one that cannot run scripts yet needs this.
                    if (!m_document->ready_to_run_scripts())
                        register_deferred_start();
                    return;
                }

                if (prefix_length > 0)
                    length_to_decode = prefix_length;
            }
        }

        decode_and_process(length_to_decode);
    }

    if (!should_continue() || m_parser->encoding_confidence() != EncodingConfidence::Tentative)
        release_encoding_change_buffers();

    if (m_body_is_exhausted)
        finish_body();
}

void IncrementalDocumentParser::process_end_of_body()
{
    // The body's stream lives in the realm of the document that started this navigation, and this parser stays attached
    // to the new document until its load event. So, drop the body as soon as it's exhausted — rather than keeping the
    // previous document alive until the new one has finished loading. Blink, WebKit, and Gecko all do this too.
    m_body = nullptr;
    m_body_is_exhausted = true;

    if (!should_continue()) {
        discard_input_bytes();
        return;
    }

    // Bytes held back for a still-possible encoding declaration are fed first; the implied EOF character waits for
    // them in finish_body().
    feed_parser();
}

void IncrementalDocumentParser::finish_body()
{
    if (m_processed_implied_eof)
        return;
    if (!should_continue())
        return;

    VERIFY(m_body_is_exhausted);
    VERIFY(undecoded_input_bytes().is_empty());

    auto decoded = m_decoder->finish_to_utf16().release_value_but_fixme_should_propagate_errors();
    append_decoded(decoded);

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // When no more bytes are available, have the parser process the implied EOF character.
    m_document->set_source(m_source.to_string());
    m_processed_implied_eof = true;
    m_parser->tokenizer().close_input_stream();
    pump();

    if (!should_continue())
        release_encoding_change_buffers();
}

ErrorOr<bool> IncrementalDocumentParser::change_encoding(StringView new_encoding)
{
    // If all bytes converted so far have the same interpretation in the new encoding, retain the replacement
    // streaming decoder after feeding it those bytes. Feeding the prefix both performs the required comparison and
    // restores any decoder state needed to continue at the current byte position.
    auto decoder = make<TextCodec::StreamingDecoder>(new_encoding, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement);

    Utf16StringBuilder builder;
    builder.append(TRY(decoder->to_utf16(decoded_input_bytes())));
    if (m_processed_implied_eof)
        builder.append(TRY(decoder->finish_to_utf16()));
    auto redecoded_source = builder.to_string();

    auto current_source = m_processed_implied_eof ? m_document->source().utf16_view() : m_source.view();
    if (!redecoded_source.starts_with(current_source)
        || (m_processed_implied_eof && redecoded_source.length_in_code_units() != current_source.length_in_code_units())) {
        // FIXME: A mismatch requires restarting the navigation with the new encoding, as described by step 6 of the
        //        change-the-encoding algorithm.
        release_encoding_change_buffers();
        return false;
    }

    auto additionally_decoded_source = redecoded_source.utf16_view().substring_view(current_source.length_in_code_units());
    if (!additionally_decoded_source.is_empty()) {
        VERIFY(!m_processed_implied_eof);
        append_decoded(additionally_decoded_source);
    }

    m_decoder = move(decoder);
    release_encoding_change_buffers();
    return true;
}

void IncrementalDocumentParser::decode_and_process(size_t length)
{
    auto undecoded_bytes = undecoded_input_bytes();
    VERIFY(length <= undecoded_bytes.size());

    auto decoded = m_decoder->to_utf16(undecoded_bytes.trim(length)).release_value_but_fixme_should_propagate_errors();

    // Count the bytes as converted before running the parser: a script it runs can deliver another body chunk, and
    // feed_parser() would otherwise convert these a second time.
    m_decoded_byte_count += length;

    append_decoded(decoded);
    pump();
}

void IncrementalDocumentParser::release_encoding_change_buffers()
{
    // Only bytes the decoder has yet to convert are still needed; the rest were kept solely to compare decoders.
    auto undecoded_bytes = undecoded_input_bytes();
    auto undecoded_size = undecoded_bytes.size();
    undecoded_bytes.copy_to(m_input_bytes.span());
    m_input_bytes.resize(undecoded_size);
    m_decoded_byte_count = 0;
}

void IncrementalDocumentParser::discard_input_bytes()
{
    m_input_bytes.clear();
    m_decoded_byte_count = 0;
}

void IncrementalDocumentParser::process_body_error(JS::Value)
{
    m_body = nullptr;

    // Bytes withheld while the tokenizer was behind are still part of the document. This ends the parse, so no
    // declaration can still change how they decode: feed them to the tokenizer with the encoding as it stands.
    if (should_continue() && !undecoded_input_bytes().is_empty())
        decode_and_process(undecoded_input_bytes().size());

    discard_input_bytes();
    dbgln("FIXME: Load html page with an error if incremental read of body failed.");
    HTMLParser::the_end(m_document, m_parser);
}

void IncrementalDocumentParser::register_deferred_start()
{
    if (m_document->has_deferred_parser_start())
        return;

    auto parser = GC::Ref { *this };
    m_document->set_deferred_parser_start(GC::create_function(GC::Heap::the(), [parser] {
        parser->pump();

        // The tokenizer can consume input again, so hand over any bytes withheld while it could not.
        parser->feed_parser();
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
