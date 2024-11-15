/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Deflate.h>
#include <LibCompress/Gzip.h>
#include <LibCompress/Zlib.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Compression/CompressionStream.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Compression {

GC_DEFINE_ALLOCATOR(CompressionStream);

// https://compression.spec.whatwg.org/#dom-compressionstream-compressionstream
WebIDL::ExceptionOr<GC::Ref<CompressionStream>> CompressionStream::construct_impl(JS::Realm& realm, Bindings::CompressionFormat format)
{
    // 1. If format is unsupported in CompressionStream, then throw a TypeError.
    // 2. Set this's format to format.
    auto input_stream = make<AllocatingMemoryStream>();

    auto compressor = [&, input_stream = MaybeOwned<Stream> { *input_stream }]() mutable -> ErrorOr<Compressor> {
        switch (format) {
        case Bindings::CompressionFormat::Deflate:
            return TRY(Compress::ZlibCompressor::construct(move(input_stream)));
        case Bindings::CompressionFormat::DeflateRaw:
            return TRY(Compress::DeflateCompressor::construct(make<LittleEndianInputBitStream>(move(input_stream))));
        case Bindings::CompressionFormat::Gzip:
            return TRY(Compress::GzipCompressor::create(move(input_stream)));
        }

        VERIFY_NOT_REACHED();
    }();

    if (compressor.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to create compressor: {}", compressor.error())) };

    // 5. Set this's transform to a new TransformStream.
    // NOTE: We do this first so that we may store it as nonnull in the GenericTransformStream.
    auto stream = realm.create<CompressionStream>(realm, realm.create<Streams::TransformStream>(realm), compressor.release_value(), move(input_stream));

    // 3. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the compress and enqueue a chunk
    //    algorithm with this and chunk.
    auto transform_algorithm = GC::create_function(realm.heap(), [stream](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->compress_and_enqueue_chunk(chunk); result.is_error()) {
            auto throw_completion = Bindings::dom_exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, *throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let flushAlgorithm be an algorithm which takes no argument and runs the compress flush and enqueue algorithm with this.
    auto flush_algorithm = GC::create_function(realm.heap(), [stream]() -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->compress_flush_and_enqueue(); result.is_error()) {
            auto throw_completion = Bindings::dom_exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, *throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 6. Set up this's transform with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to flushAlgorithm.
    Streams::transform_stream_set_up(stream->m_transform, transform_algorithm, flush_algorithm);

    return stream;
}

CompressionStream::CompressionStream(JS::Realm& realm, GC::Ref<Streams::TransformStream> transform, Compressor compressor, NonnullOwnPtr<AllocatingMemoryStream> input_stream)
    : Bindings::PlatformObject(realm)
    , Streams::GenericTransformStreamMixin(transform)
    , m_compressor(move(compressor))
    , m_output_stream(move(input_stream))
{
}

CompressionStream::~CompressionStream() = default;

void CompressionStream::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CompressionStream);
}

void CompressionStream::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Streams::GenericTransformStreamMixin::visit_edges(visitor);
}

// https://compression.spec.whatwg.org/#compress-and-enqueue-a-chunk
WebIDL::ExceptionOr<void> CompressionStream::compress_and_enqueue_chunk(JS::Value chunk)
{
    auto& realm = this->realm();

    // 1. If chunk is not a BufferSource type, then throw a TypeError.
    if (!WebIDL::is_buffer_source_type(chunk))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Chunk is not a BufferSource type"sv };

    // 2. Let buffer be the result of compressing chunk with cs's format and context.
    auto buffer = [&]() -> ErrorOr<ByteBuffer> {
        if (auto buffer = WebIDL::underlying_buffer_source(chunk.as_object()))
            return compress(buffer->buffer(), Finish::No);
        return ByteBuffer {};
    }();

    if (buffer.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to compress chunk: {}", buffer.error())) };

    // 3. If buffer is empty, return.
    if (buffer.value().is_empty())
        return {};

    // 4. Split buffer into one or more non-empty pieces and convert them into Uint8Arrays.
    auto array_buffer = JS::ArrayBuffer::create(realm, buffer.release_value());
    auto array = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

    // 5. For each Uint8Array array, enqueue array in cs's transform.
    TRY(Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), array));
    return {};
}

// https://compression.spec.whatwg.org/#compress-flush-and-enqueue
WebIDL::ExceptionOr<void> CompressionStream::compress_flush_and_enqueue()
{
    auto& realm = this->realm();

    // 1. Let buffer be the result of compressing an empty input with cs's format and context, with the finish flag.
    auto buffer = compress({}, Finish::Yes);

    if (buffer.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to compress flush: {}", buffer.error())) };

    // 2. If buffer is empty, return.
    if (buffer.value().is_empty())
        return {};

    // 3. Split buffer into one or more non-empty pieces and convert them into Uint8Arrays.
    auto array_buffer = JS::ArrayBuffer::create(realm, buffer.release_value());
    auto array = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

    // 4. For each Uint8Array array, enqueue array in cs's transform.
    TRY(Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), array));
    return {};
}

ErrorOr<ByteBuffer> CompressionStream::compress(ReadonlyBytes bytes, Finish finish)
{
    TRY(m_compressor.visit([&](auto const& compressor) {
        return compressor->write_until_depleted(bytes);
    }));

    if (finish == Finish::Yes) {
        TRY(m_compressor.visit(
            [&](NonnullOwnPtr<Compress::ZlibCompressor> const& compressor) {
                return compressor->finish();
            },
            [&](NonnullOwnPtr<Compress::DeflateCompressor> const& compressor) {
                return compressor->final_flush();
            },
            [&](NonnullOwnPtr<Compress::GzipCompressor> const& compressor) {
                return compressor->finish();
            }));
    }

    auto buffer = TRY(ByteBuffer::create_uninitialized(m_output_stream->used_buffer_size()));
    TRY(m_output_stream->read_until_filled(buffer.bytes()));

    return buffer;
}

}
