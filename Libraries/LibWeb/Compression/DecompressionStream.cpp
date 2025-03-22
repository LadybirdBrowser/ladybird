/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Deflate.h>
#include <LibCompress/Gzip.h>
#include <LibCompress/Zlib.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/DecompressionStreamPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Compression/DecompressionStream.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Compression {

GC_DEFINE_ALLOCATOR(DecompressionStream);

// https://compression.spec.whatwg.org/#dom-decompressionstream-decompressionstream
WebIDL::ExceptionOr<GC::Ref<DecompressionStream>> DecompressionStream::construct_impl(JS::Realm& realm, Bindings::CompressionFormat format)
{
    // 1. If format is unsupported in DecompressionStream, then throw a TypeError.
    // 2. Set this's format to format.
    auto input_stream = make<AllocatingMemoryStream>();

    auto decompressor = [&, input_stream = MaybeOwned<Stream> { *input_stream }]() mutable -> ErrorOr<Decompressor> {
        switch (format) {
        case Bindings::CompressionFormat::Deflate:
            return TRY(Compress::ZlibDecompressor::create(move(input_stream)));
        case Bindings::CompressionFormat::DeflateRaw:
            return TRY(Compress::DeflateDecompressor::create(move(input_stream)));
        case Bindings::CompressionFormat::Gzip:
            return TRY(Compress::GzipDecompressor::create((move(input_stream))));
        }

        VERIFY_NOT_REACHED();
    }();

    if (decompressor.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to create decompressor: {}", decompressor.error())) };

    // 5. Set this's transform to a new TransformStream.
    // NOTE: We do this first so that we may store it as nonnull in the GenericTransformStream.
    auto stream = realm.create<DecompressionStream>(realm, realm.create<Streams::TransformStream>(realm), decompressor.release_value(), move(input_stream));

    // 3. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the decompress and enqueue a chunk
    //    algorithm with this and chunk.
    auto transform_algorithm = GC::create_function(realm.heap(), [stream](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->decompress_and_enqueue_chunk(chunk); result.is_error()) {
            auto throw_completion = Bindings::exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, *throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let flushAlgorithm be an algorithm which takes no argument and runs the decompress flush and enqueue algorithm with this.
    auto flush_algorithm = GC::create_function(realm.heap(), [stream]() -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->decompress_flush_and_enqueue(); result.is_error()) {
            auto throw_completion = Bindings::exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, *throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 6. Set up this's transform with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to flushAlgorithm.
    stream->m_transform->set_up(transform_algorithm, flush_algorithm);

    return stream;
}

DecompressionStream::DecompressionStream(JS::Realm& realm, GC::Ref<Streams::TransformStream> transform, Decompressor decompressor, NonnullOwnPtr<AllocatingMemoryStream> input_stream)
    : Bindings::PlatformObject(realm)
    , Streams::GenericTransformStreamMixin(transform)
    , m_decompressor(move(decompressor))
    , m_input_stream(move(input_stream))
{
}

DecompressionStream::~DecompressionStream() = default;

void DecompressionStream::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DecompressionStream);
}

void DecompressionStream::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Streams::GenericTransformStreamMixin::visit_edges(visitor);
}

// https://compression.spec.whatwg.org/#decompress-and-enqueue-a-chunk
WebIDL::ExceptionOr<void> DecompressionStream::decompress_and_enqueue_chunk(JS::Value chunk)
{
    auto& realm = this->realm();

    // 1. If chunk is not a BufferSource type, then throw a TypeError.
    if (!WebIDL::is_buffer_source_type(chunk))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Chunk is not a BufferSource type"sv };

    // 2. Let buffer be the result of decompressing chunk with ds's format and context. If this results in an error,
    //    then throw a TypeError.
    auto maybe_buffer = [&]() -> ErrorOr<ByteBuffer> {
        auto chunk_buffer = TRY(WebIDL::get_buffer_source_copy(chunk.as_object()));
        TRY(m_input_stream->write_until_depleted(move(chunk_buffer)));

        auto decompressed = TRY(ByteBuffer::create_uninitialized(4096));
        auto size = TRY(m_decompressor.visit([&](auto const& decompressor) -> ErrorOr<size_t> {
            return TRY(decompressor->read_some(decompressed.bytes())).size();
        }));
        return decompressed.slice(0, size);
    }();
    if (maybe_buffer.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to decompress chunk: {}", maybe_buffer.error())) };

    auto buffer = maybe_buffer.release_value();

    // 3. If buffer is empty, return.
    if (buffer.is_empty())
        return {};

    // 4. Split buffer into one or more non-empty pieces and convert them into Uint8Arrays.
    auto array_buffer = JS::ArrayBuffer::create(realm, move(buffer));
    auto array = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

    // 5. For each Uint8Array array, enqueue array in ds's transform.
    m_transform->enqueue(array);
    return {};
}

// https://compression.spec.whatwg.org/#decompress-flush-and-enqueue
WebIDL::ExceptionOr<void> DecompressionStream::decompress_flush_and_enqueue()
{
    auto& realm = this->realm();

    // 1. Let buffer be the result of decompressing an empty input with ds's format and context, with the finish flag.
    auto maybe_buffer = m_decompressor.visit([&](auto const& decompressor) -> ErrorOr<ByteBuffer> {
        return TRY(decompressor->read_until_eof());
    });
    if (maybe_buffer.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to compress flush: {}", maybe_buffer.error())) };

    auto buffer = maybe_buffer.release_value();

    // 2. If the end of the compressed input has not been reached, then throw a TypeError.
    if (m_decompressor.visit([](auto const& decompressor) { return !decompressor->is_eof(); }))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "End of compressed input has not been reached"sv };

    // 3. If buffer is empty, return.
    if (buffer.is_empty())
        return {};

    // 4. Split buffer into one or more non-empty pieces and convert them into Uint8Arrays.
    auto array_buffer = JS::ArrayBuffer::create(realm, move(buffer));
    auto array = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

    // 5. For each Uint8Array array, enqueue array in ds's transform.
    m_transform->enqueue(array);
    return {};
}

}
