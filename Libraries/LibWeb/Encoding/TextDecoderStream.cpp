/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextDecoderStream.h>
#include <LibWeb/Encoding/TextDecoderStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextDecoderStream);

// Returns the largest prefix length of `bytes` that can be safely decoded as UTF-8 without splitting an in-progress
// multi-byte sequence. The remainder (if any) is held over for the next chunk.
static size_t find_utf8_safe_decode_boundary(ReadonlyBytes bytes)
{
    // A valid UTF-8 sequence is at most 4 bytes long, so we never need to look back more than 3 continuation bytes
    // to find the leading byte of the trailing sequence.
    size_t scan = 0;
    while (scan < bytes.size() && scan < 4) {
        size_t pos = bytes.size() - scan - 1;
        u8 byte = bytes[pos];

        // Continuation byte (10xxxxxx): keep walking back to find the leading byte.
        if ((byte & 0xC0) == 0x80) {
            ++scan;
            continue;
        }

        // ASCII byte (0xxxxxxx): the trailing sequence ends here and is complete.
        if ((byte & 0x80) == 0)
            return pos + 1;

        // Multi-byte leading byte. If it's a recognized leading byte and the buffer doesn't yet hold the full
        // sequence, cut before it so the next chunk can complete it. Otherwise (recognized and complete, or
        // unrecognized so it'll just become a replacement character) include all bytes up to the end.
        size_t expected_length = 0;
        if ((byte & 0xE0) == 0xC0)
            expected_length = 2;
        else if ((byte & 0xF0) == 0xE0)
            expected_length = 3;
        else if ((byte & 0xF8) == 0xF0)
            expected_length = 4;
        else
            return pos + 1;
        if (bytes.size() - pos >= expected_length)
            return bytes.size();
        return pos;
    }

    // No leading byte found within the last 4 bytes. Either the buffer is shorter than that, or it ends with 4
    // continuation bytes (malformed UTF-8). Either way, decode everything; the decoder will produce replacement
    // characters as needed.
    return bytes.size();
}

// https://encoding.spec.whatwg.org/#dom-textdecoderstream
WebIDL::ExceptionOr<GC::Ref<TextDecoderStream>> TextDecoderStream::construct_impl(JS::Realm& realm, FlyString label, TextDecoderOptions const& options)
{
    // 1. Let encoding be the result of getting an encoding from label.
    auto encoding = TextCodec::get_standardized_encoding(label);

    // 2. If encoding is failure or replacement, then throw a RangeError.
    if (!encoding.has_value() || encoding->equals_ignoring_ascii_case("replacement"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, MUST(String::formatted("Invalid encoding {}", label)) };

    // 3. Set this’s encoding to encoding.
    auto lowercase_encoding_name = encoding.value().to_ascii_lowercase_string();

    // 4. If options["fatal"] is true, then set this’s error mode to "fatal".
    auto error_mode = options.fatal ? ErrorMode::Fatal : ErrorMode::Replacement;

    // 5. Set this’s ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.ignore_bom;

    // 6. Set this’s decoder to a new instance of this’s encoding’s decoder, and set this’s I/O queue to a new I/O queue.
    auto decoder = TextCodec::decoder_for_exact_name(encoding.value());
    VERIFY(decoder.has_value());

    // NB: Steps 7-11 — we create the TransformStream and the TextDecoderStream first so that we can refer to the
    //     stream from the transform/flush algorithms.

    // 9. Let transformStream be a new TransformStream.
    auto transform_stream = realm.create<Streams::TransformStream>(realm);

    auto stream = realm.create<TextDecoderStream>(realm, transform_stream, *decoder, lowercase_encoding_name, error_mode, ignore_bom);

    // 7. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the decode and enqueue a chunk
    //    algorithm with this and chunk.
    auto transform_algorithm = GC::create_function(realm.heap(), [stream](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        if (auto result = stream->decode_and_enqueue_chunk(chunk); result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 8. Let flushAlgorithm be an algorithm which takes no arguments and runs the flush and enqueue algorithm with this.
    auto flush_algorithm = GC::create_function(realm.heap(), [stream]() -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        if (auto result = stream->flush_and_enqueue(); result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 10. Set up transformStream with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to flushAlgorithm.
    transform_stream->set_up(transform_algorithm, flush_algorithm);

    // 11. Set this’s transform to transformStream.
    // NB: Done via the GenericTransformStreamMixin constructor above.

    return stream;
}

TextDecoderStream::TextDecoderStream(JS::Realm& realm, GC::Ref<Streams::TransformStream> transform, TextCodec::Decoder& decoder, FlyString encoding, ErrorMode error_mode, bool ignore_bom)
    : Bindings::PlatformObject(realm)
    , Streams::GenericTransformStreamMixin(transform)
    , TextDecoderCommonMixin(decoder, move(encoding), error_mode, ignore_bom)
{
}

TextDecoderStream::~TextDecoderStream() = default;

void TextDecoderStream::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextDecoderStream);
    Base::initialize(realm);
}

void TextDecoderStream::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Streams::GenericTransformStreamMixin::visit_edges(visitor);
}

// https://encoding.spec.whatwg.org/#decode-and-enqueue-a-chunk
WebIDL::ExceptionOr<void> TextDecoderStream::decode_and_enqueue_chunk(JS::Value chunk)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let bufferSource be the result of converting chunk to an AllowSharedBufferSource.
    if (!WebIDL::is_buffer_source_type(chunk))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Chunk is not a BufferSource"sv };

    // 2. Push a copy of bufferSource to decoder's I/O queue.
    auto buffer_or_error = WebIDL::get_buffer_source_copy(chunk.as_object());
    if (buffer_or_error.is_error())
        return WebIDL::OperationError::create(realm, "Failed to copy bytes from BufferSource"_utf16);
    auto buffer = buffer_or_error.release_value();
    m_io_queue.append(buffer.bytes());

    // NB: Only decode the prefix of m_io_queue that doesn't end mid-multi-byte-sequence; the remainder is held over
    //     for the next chunk so we don't emit spurious replacement characters at chunk boundaries. We currently only
    //     do this boundary search for UTF-8; the underlying decoders for other encodings are stateless across calls
    //     so for those we just decode whatever's in the queue and don't carry anything over.
    auto safe_length = (m_encoding == "utf-8"_fly_string)
        ? find_utf8_safe_decode_boundary(m_io_queue.bytes())
        : m_io_queue.size();
    if (safe_length == 0)
        return {};

    auto decoded = TRY_OR_THROW_OOM(vm, m_decoder.to_utf8(StringView { m_io_queue.data(), safe_length }));

    auto remaining = m_io_queue.size() - safe_length;
    if (remaining > 0)
        memmove(m_io_queue.data(), m_io_queue.data() + safe_length, remaining);
    m_io_queue.resize(remaining);

    // 3-4. Run "processing an item" until the input is exhausted, accumulating the output, then enqueue any non-empty
    //      result. If processing returns error, throw a TypeError.
    return enqueue_decoded_output(decoded);
}

// https://encoding.spec.whatwg.org/#flush-and-enqueue
WebIDL::ExceptionOr<void> TextDecoderStream::flush_and_enqueue()
{
    // 1-3. Drain decoder's I/O queue and run "processing an item" to completion.

    // NB: For UTF-8, anything still in the I/O queue here is exactly the trailing partial sequence that
    //     decode_and_enqueue_chunk held back at the safe boundary. The WHATWG UTF-8 decoder emits a single replacement
    //     character for the whole incomplete sequence, so emit exactly one rather than letting the underlying decoder
    //     produce one per stray byte.
    String decoded;
    if (!m_io_queue.is_empty()) {
        decoded = "\xEF\xBF\xBD"_string;
        m_io_queue.clear();
    }

    return enqueue_decoded_output(decoded);
}

WebIDL::ExceptionOr<void> TextDecoderStream::enqueue_decoded_output(String const& decoded)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // https://encoding.spec.whatwg.org/#concept-td-serialize
    // FIXME: The underlying TextCodec decoders currently strip leading BOMs unconditionally for UTF-8 and UTF-16BE/LE,
    //        so the "ignore BOM" flag is effectively ignored here. Once the decoders accept a "preserve BOM" mode,
    //        plumb m_ignore_bom through and strip the BOM from `decoded` only when m_ignore_bom is false.
    if (!m_bom_seen && !decoded.is_empty())
        m_bom_seen = true;

    if (decoded.is_empty())
        return {};

    // If decoder's error mode is "fatal" and processing produced any error, throw a TypeError.
    // NB: We can only detect this approximately by looking for U+FFFD in the decoded output, which the underlying
    //     decoder substitutes for invalid sequences. This matches the existing TextDecoder.decode() behavior.
    if (fatal() && decoded.contains(0xFFFD))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"sv };

    auto js_string = JS::PrimitiveString::create(vm, decoded);
    return Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), js_string);
}

}
