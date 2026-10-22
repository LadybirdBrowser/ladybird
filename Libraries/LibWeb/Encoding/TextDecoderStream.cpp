/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Encoding/TextDecoderStream.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamOperations.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextDecoderStream);

WebIDL::ExceptionOr<GC::Ref<TextDecoderStream>> TextDecoderStream::create_for_constructor(JS::Object& relevant_global_object, Utf16String const& label, TextDecoderOptions const& options)
{
    auto& realm = HTML::relevant_realm(relevant_global_object);
    auto label_fly_string = Utf16FlyString::from_utf16(label.utf16_view());

    // 1. Let encoding be the result of getting an encoding from label.
    auto encoding = TextCodec::get_standardized_encoding(label_fly_string.view());

    // 2. If encoding is failure or replacement, then throw a RangeError.
    if (!encoding.has_value() || encoding->equals_ignoring_ascii_case("replacement"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, Utf16String::formatted("Invalid encoding {}", label_fly_string.to_utf16_string()) };

    // 3. Set this’s encoding to encoding.
    auto lowercase_encoding_name = encoding.value().to_ascii_lowercase_string();

    // 4. If options["fatal"] is true, then set this’s error mode to "fatal".
    auto error_mode = options.fatal ? TextCodec::ErrorMode::Fatal : TextCodec::ErrorMode::Replacement;

    // 5. Set this’s ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.ignore_bom;

    // NB: Steps 7-11 — we create the TransformStream and the TextDecoderStream first so that we can refer to the
    //     stream from the transform/flush algorithms.

    // 9. Let transformStream be a new TransformStream.
    auto transform_stream = GC::Heap::the().allocate<Streams::TransformStream>();

    auto stream = GC::Heap::the().allocate<TextDecoderStream>(transform_stream, lowercase_encoding_name, error_mode, ignore_bom);

    // 7. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the decode and enqueue a chunk
    //    algorithm with this and chunk.
    auto transform_algorithm = GC::create_function(GC::Heap::the(), [stream, realm = GC::Ref(realm)](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        if (auto result = stream->decode_and_enqueue_chunk(realm->vm(), chunk); result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 8. Let flushAlgorithm be an algorithm which takes no arguments and runs the flush and enqueue algorithm with this.
    auto flush_algorithm = GC::create_function(GC::Heap::the(), [stream, realm = GC::Ref(realm)]() -> GC::Ref<WebIDL::Promise> {
        if (auto result = stream->flush_and_enqueue(realm->vm()); result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 10. Set up transformStream with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to flushAlgorithm.
    transform_stream->set_up(realm, transform_algorithm, flush_algorithm);

    // 11. Set this’s transform to transformStream.
    // NB: Done via the GenericTransformStreamMixin constructor above.

    return stream;
}

TextDecoderStream::TextDecoderStream(GC::Ref<Streams::TransformStream> transform, FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom)
    : Streams::GenericTransformStreamMixin(transform)
    , TextDecoderCommonMixin(move(encoding), error_mode, ignore_bom)
{
    set_decoder_to_new_instance_of_encoding_decoder();
}

TextDecoderStream::~TextDecoderStream() = default;

void TextDecoderStream::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Streams::GenericTransformStreamMixin::visit_edges(visitor);
}

// https://encoding.spec.whatwg.org/#decode-and-enqueue-a-chunk
WebIDL::ExceptionOr<void> TextDecoderStream::decode_and_enqueue_chunk(JS::VM& vm, JS::Value chunk)
{
    // 1. Let bufferSource be the result of converting chunk to an AllowSharedBufferSource.
    if (!WebIDL::is_buffer_source_type(chunk))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Chunk is not a BufferSource"_utf16 };

    // 2. Push a copy of bufferSource to decoder's I/O queue.
    // WARNING: See the warning for SharedArrayBuffer objects above.
    auto buffer_or_error = WebIDL::get_buffer_source_copy(chunk.as_object());
    if (buffer_or_error.is_error())
        return WebIDL::OperationError::create("Failed to copy bytes from BufferSource"_utf16);
    auto input_copy = buffer_or_error.release_value();

    // Decode this chunk while preserving the streaming decoder's pending state.
    TextDecoderOutputQueue output;
    TRY(process_an_item(vm, input_copy.bytes(), output));
    auto decoded = TRY(serialize_io_queue(vm, output));
    return enqueue_decoded_output(vm, decoded);
}

// https://encoding.spec.whatwg.org/#flush-and-enqueue
WebIDL::ExceptionOr<void> TextDecoderStream::flush_and_enqueue(JS::VM& vm)
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
    auto result = process_an_item(vm, item, output);

    // 3. If result is finished:
    //     1. Let outputChunk be the result of running serialize I/O queue with decoder and output.
    //     2. If outputChunk is not the empty string, then enqueue outputChunk in decoder’s transform.
    //     3. Return.
    if (!result.is_error()) {
        auto output_chunk = TRY(serialize_io_queue(vm, output));
        TRY(enqueue_decoded_output(vm, output_chunk));
        return {};
    }

    return result.release_error();
}

WebIDL::ExceptionOr<void> TextDecoderStream::enqueue_decoded_output(JS::VM& vm, Utf16String const& decoded)
{
    if (decoded.is_empty())
        return {};

    auto js_string = JS::PrimitiveString::create(vm, decoded);
    return Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), js_string);
}

}
