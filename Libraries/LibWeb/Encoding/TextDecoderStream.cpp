/*
 * Copyright (c) 2025, Saksham Mittal <hi@gotlou.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextDecoderStreamPrototype.h>
#include <LibWeb/Encoding/TextDecoderStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextDecoderStream);

// https://encoding.spec.whatwg.org/#dom-textdecoderstream
WebIDL::ExceptionOr<GC::Ref<TextDecoderStream>> TextDecoderStream::construct_impl(JS::Realm& realm, FlyString encoding_label, TextDecoderOptions const& options)
{
    auto& vm = realm.vm();

    // 1. Let encoding be the result of getting an encoding from label.
    // If label is not given, let encoding be UTF-8.
    auto encoding = TextCodec::get_standardized_encoding(encoding_label);

    // 2. If encoding is failure or replacement, then throw a RangeError.
    if (!encoding.has_value() || encoding->equals_ignoring_ascii_case("replacement"sv))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, TRY_OR_THROW_OOM(vm, String::formatted("Invalid encoding {}", encoding_label)) };

    // 3. Set this's encoding to encoding.
    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    // The encoding getter steps are to return this's encoding's name, ASCII lowercased.
    auto lowercase_encoding_name = encoding.value().to_ascii_lowercase_string();

    // 4. If options["fatal"] is true, then set this's error mode to "fatal".
    auto fatal = options.fatal;

    // 5. Set this's ignore BOM to options["ignoreBOM"].
    auto ignore_bom = options.ignore_bom;

    // 6. Set this's decoder to a new decoder for this's encoding, and set this's I/O queue to a new I/O queue.
    auto decoder = TextCodec::decoder_for_exact_name(encoding.value());
    VERIFY(decoder.has_value());

    // NOTE: We do these steps first so that we may store it as nonnull in the GenericTransformStream.
    // 9. Let transformStream be a new TransformStream.
    auto transform_stream = realm.create<Streams::TransformStream>(realm);

    // 11. Set this's transform to transformStream.
    auto stream = realm.create<TextDecoderStream>(realm, *decoder, lowercase_encoding_name, fatal, ignore_bom, transform_stream);

    // 7. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the decode and enqueue a chunk
    //    algorithm with this and chunk.
    auto transform_algorithm = GC::create_function(realm.heap(), [stream](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->decode_and_enqueue_chunk(chunk); result.is_error()) {
            auto throw_completion = Bindings::exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 8. Let flushAlgorithm be an algorithm which runs the flush and enqueue algorithm with this.
    auto flush_algorithm = GC::create_function(realm.heap(), [stream]() -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->flush_and_enqueue(); result.is_error()) {
            auto throw_completion = Bindings::exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 10. Set up transformStream with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to flushAlgorithm.
    transform_stream->set_up(transform_algorithm, flush_algorithm);

    return stream;
}

TextDecoderStream::TextDecoderStream(JS::Realm& realm, TextCodec::Decoder& decoder, FlyString encoding, bool fatal, bool ignore_bom, GC::Ref<Streams::TransformStream> transform)
    : Bindings::PlatformObject(realm)
    , Streams::GenericTransformStreamMixin(transform)
    , Encoding::TextDecoderCommonMixin(move(encoding), fatal, ignore_bom)
    , m_decoder(decoder)
{
}

TextDecoderStream::~TextDecoderStream() = default;

void TextDecoderStream::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextDecoderStream);
    Base::initialize(realm);
}

void TextDecoderStream::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Streams::GenericTransformStreamMixin::visit_edges(visitor);
}

// https://encoding.spec.whatwg.org/#decode-and-enqueue-a-chunk
WebIDL::ExceptionOr<void> TextDecoderStream::decode_and_enqueue_chunk(JS::Value chunk)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. Let bufferSource be the result of converting chunk to an AllowSharedBufferSource.
    // Note: We convert to a BufferSource since we need to copy the bytes anyway
    if (!chunk.is_object())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Chunk is not an object"sv };

    auto& chunk_object = chunk.as_object();
    if (!WebIDL::is_buffer_source_type(chunk))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Chunk is not a BufferSource"sv };

    // 2. Push a copy of bufferSource to decoder's I/O queue.
    // Note: Implementations are strongly encouraged to use an implementation strategy that avoids this copy.
    //       When doing so they will have to make sure that changes to bufferSource do not affect future
    //       iterations of the decode-and-enqueue-a-chunk and flush-and-enqueue algorithms.
    auto data_buffer_or_error = WebIDL::get_buffer_source_copy(chunk_object);
    if (data_buffer_or_error.is_error())
        return WebIDL::OperationError::create(realm, "Failed to copy bytes from ArrayBuffer"_utf16);
    auto& data_buffer = data_buffer_or_error.value();
    m_io_queue.append(move(data_buffer));

    // 3. Let output be the I/O queue of scalar values « end-of-queue ».
    Vector<u32> output;

    // 4. While true:
    while (true) {
        // 1. Let item be the result of reading from decoder's I/O queue.
        // 2. If item is end-of-queue, then:
        if (m_io_queue.is_empty()) {
            // 1. Let outputChunk be the result of running serialize I/O queue with decoder and output.
            auto output_chunk = TRY(serialize_io_queue(output));

            // 2. If outputChunk is not the empty string, then enqueue outputChunk in decoder's transform.
            if (!output_chunk.is_empty()) {
                TRY(Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), JS::PrimitiveString::create(vm, output_chunk)));
            }

            // 3. Return.
            return {};
        }

        auto bytes_to_process = m_io_queue.take_first();

        // 3. Otherwise:
        // 1. Let result be the result of processing an item with item, decoder's decoder, decoder's I/O queue,
        //    output, and decoder's error mode.
        auto result = TRY_OR_THROW_OOM(vm, m_decoder.to_utf8({ bytes_to_process.data(), bytes_to_process.size() }));

        // 2. If result is finished, then:
        // Note: In our implementation, we process the entire chunk at once, so we check if there are errors
        if (m_fatal && result.contains(0xfffd)) {
            // If decoder's error mode is "fatal", then throw a TypeError.
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"sv };
        }

        // Add decoded characters to output
        for (auto code_point : result.code_points()) {
            output.append(code_point);
        }

        // Since we processed the entire chunk, we're done with this iteration
        // Break to check if there are more chunks in the queue
        if (m_io_queue.is_empty()) {
            // Serialize and enqueue what we have
            auto output_chunk = TRY(serialize_io_queue(output));
            if (!output_chunk.is_empty()) {
                TRY(Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), JS::PrimitiveString::create(vm, output_chunk)));
            }
            return {};
        }
    }
}

// https://encoding.spec.whatwg.org/#flush-and-enqueue
WebIDL::ExceptionOr<void> TextDecoderStream::flush_and_enqueue()
{
    auto& vm = this->vm();

    // 1. Let output be the I/O queue of scalar values « end-of-queue ».
    Vector<u32> output;

    // 2. While true:
    while (!m_io_queue.is_empty()) {
        // 1. Let item be the result of reading from decoder's I/O queue.
        auto bytes_to_process = m_io_queue.take_first();

        // 2. Let result be the result of processing an item with item, decoder's decoder, decoder's I/O queue,
        //    output, and decoder's error mode.
        auto result = TRY_OR_THROW_OOM(vm, m_decoder.to_utf8({ bytes_to_process.data(), bytes_to_process.size() }));

        // 4. Otherwise, if result is error, throw a TypeError.
        if (m_fatal && result.contains(0xfffd)) {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Decoding failed"sv };
        }

        // Add decoded characters to output
        for (auto code_point : result.code_points()) {
            output.append(code_point);
        }
    }

    // 3. If result is finished, then:
    // 1. Let outputChunk be the result of running serialize I/O queue with decoder and output.
    auto output_chunk = TRY(serialize_io_queue(output));

    // 2. If outputChunk is not the empty string, then enqueue outputChunk in decoder's transform.
    if (!output_chunk.is_empty()) {
        TRY(Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), JS::PrimitiveString::create(vm, output_chunk)));
    }

    // 3. Return.
    return {};
}

// https://encoding.spec.whatwg.org/#concept-td-serialize
WebIDL::ExceptionOr<String> TextDecoderStream::serialize_io_queue(Vector<u32> const& ioQueue)
{
    auto& vm = this->vm();

    // 1. Let output be the empty string.
    StringBuilder output;

    // 2. While true:
    for (size_t i = 0; i < ioQueue.size(); ++i) {
        // 1. Let item be the result of reading from ioQueue
        auto item = ioQueue[i];

        // 3. If decoder’s encoding is UTF-8 or UTF-16BE/LE,
        // and decoder’s ignore BOM and BOM seen are false:
        if (!m_ignore_bom && !m_bom_seen && (m_encoding == "utf-8"_fly_string || m_encoding == "utf-16be"_fly_string || m_encoding == "utf-16le"_fly_string)) {
            // 1. Set decoder's BOM seen to true
            m_bom_seen = true;
            // 2. If item is U+FEFF, then continue.
            if (item == 0xFEFF) {
                continue;
            }
        }

        // 3. Otherwise, append item to output.
        TRY_OR_THROW_OOM(vm, output.try_append_code_point(item));
    }

    // If item is end-of-queue, then return output.
    return TRY_OR_THROW_OOM(vm, output.to_string());
}

}
