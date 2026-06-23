/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Variant.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextDecoder.h>
#include <LibWeb/Bindings/TextDecoderStream.h>
#include <LibWeb/Encoding/TextDecoderStream.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextDecoderStream);

// https://encoding.spec.whatwg.org/#dom-textdecoderstream
WebIDL::ExceptionOr<GC::Ref<TextDecoderStream>> TextDecoderStream::construct_impl(JS::Realm& realm, FlyString label, Bindings::TextDecoderOptions const& options)
{
    // 1. Let encoding be the result of getting an encoding from label.
    auto encoding = TextCodec::get_standardized_encoding(label);

    // 2. If encoding is failure or replacement, then throw a RangeError.
    if (!encoding.has_value() || encoding->equals_ignoring_ascii_case("replacement"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, MUST(String::formatted("Invalid encoding {}", label)) };

    // 3. Set this’s encoding to encoding.
    auto lowercase_encoding_name = encoding.value().to_ascii_lowercase_string();

    // 4. If options["fatal"] is true, then set this’s error mode to "fatal".
    auto error_mode = options.fatal ? TextCodec::ErrorMode::Fatal : TextCodec::ErrorMode::Replacement;

    // 5. Set this’s ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.ignore_bom;

    // NB: Steps 7-11 — we create the TransformStream and the TextDecoderStream first so that we can refer to the
    //     stream from the transform/flush algorithms.

    // 9. Let transformStream be a new TransformStream.
    auto transform_stream = realm.create<Streams::TransformStream>(realm);

    // 6. Set this’s decoder to a new instance of this’s encoding’s decoder, and set this’s I/O queue to a new I/O queue.
    auto stream = realm.create<TextDecoderStream>(realm, transform_stream, lowercase_encoding_name, error_mode, ignore_bom);

    // 7. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the decode and enqueue a chunk
    //    algorithm with this and chunk.
    auto transform_algorithm = GC::create_function(realm.heap(), [stream](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        HTML::TemporaryExecutionContext execution_context { realm };
        if (auto result = stream->decode_and_enqueue_chunk(chunk); result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 8. Let flushAlgorithm be an algorithm which takes no arguments and runs the flush and enqueue algorithm with this.
    auto flush_algorithm = GC::create_function(realm.heap(), [stream]() -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        HTML::TemporaryExecutionContext execution_context { realm };
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

TextDecoderStream::TextDecoderStream(JS::Realm& realm, GC::Ref<Streams::TransformStream> transform, FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom)
    : Bindings::PlatformObject(realm)
    , Streams::GenericTransformStreamMixin(transform)
    , TextDecoderCommonMixin(move(encoding), error_mode, ignore_bom)
{
    set_decoder_to_new_instance_of_encoding_decoder();
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
    // WARNING: See the warning for SharedArrayBuffer objects above.
    auto buffer_or_error = WebIDL::get_buffer_source_copy(chunk.as_object());
    if (buffer_or_error.is_error())
        return WebIDL::OperationError::create(realm, "Failed to copy bytes from BufferSource"_utf16);

    auto const input_copy = buffer_or_error.release_value();

    // 3. Let output be the I/O queue of scalar values « end-of-queue ».
    TextDecoderOutputQueue output;

    // 4. While true:
    bool did_read_bytes = false;
    auto read_item = [&]() -> Variant<ReadonlyBytes, EndOfQueue> {
        if (did_read_bytes)
            return EndOfQueue {};
        did_read_bytes = true;
        return input_copy.bytes();
    };

    while (true) {
        // 1. Let item be the result of reading from decoder’s I/O queue.
        auto item = read_item();

        // 2. If item is end-of-queue:
        if (item.has<EndOfQueue>()) {
            // 1. Let outputChunk be the result of running serialize I/O queue with decoder and output.
            auto output_chunk = TRY(serialize_io_queue(vm, output));

            // 2. If outputChunk is not the empty string, then enqueue outputChunk in decoder’s transform.
            TRY(enqueue_decoded_output(output_chunk));

            // 3. Return.
            return {};
        }

        // 3. Let result be the result of processing an item with item, decoder’s decoder, decoder’s I/O queue, output, and decoder’s
        //    error mode.
        auto result = process_an_item(vm, item.get<ReadonlyBytes>(), output);

        // 4. If result is error, then throw a TypeError.
        if (result.is_error())
            return result.release_error();
    }
}

// https://encoding.spec.whatwg.org/#flush-and-enqueue
WebIDL::ExceptionOr<void> TextDecoderStream::flush_and_enqueue()
{
    // 1. Let output be the I/O queue of scalar values « end-of-queue ».
    TextDecoderOutputQueue output;

    // 2. While true:
    // NB: TextCodec::StreamingDecoder keeps any cross-chunk decoder state internally. Since flushing reads only
    //     end-of-queue from the decoder's I/O queue, we write out that single read instead of materializing a queue and
    //     looping over it.
    // 1. Let item be the result of reading from this’s I/O queue.
    auto item = EndOfQueue {};

    // 2. Let result be the result of processing an item with item, this’s decoder, decoder’s I/O queue, output, and decoder’s
    //    error mode.
    auto result = process_an_item(vm(), item, output);

    // 3. If result is finished:
    //     1. Let outputChunk be the result of running serialize I/O queue with decoder and output.
    //     2. If outputChunk is not the empty string, then enqueue outputChunk in decoder’s transform.
    //     3. Return.
    if (!result.is_error()) {
        auto output_chunk = TRY(serialize_io_queue(vm(), output));
        TRY(enqueue_decoded_output(output_chunk));
        return {};
    }

    // 4. Otherwise, if result is error, throw a TypeError.
    return result.release_error();
}

WebIDL::ExceptionOr<void> TextDecoderStream::enqueue_decoded_output(String const& decoded)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    if (decoded.is_empty())
        return {};

    auto js_string = JS::PrimitiveString::create(vm, Utf16String::from_utf8(decoded));
    return Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), js_string);
}

}
