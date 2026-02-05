/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Variant.h>
#include <LibCompress/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CompressionStreamPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Streams/GenericTransformStream.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Compression {

using Compressor = Variant<
    NonnullOwnPtr<Compress::ZlibCompressor>,
    NonnullOwnPtr<Compress::DeflateCompressor>,
    NonnullOwnPtr<Compress::GzipCompressor>>;

// https://compression.spec.whatwg.org/#compressionstream
class CompressionStream final
    : public Bindings::PlatformObject
    , public Streams::GenericTransformStreamMixin {
    WEB_PLATFORM_OBJECT(CompressionStream, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CompressionStream);

public:
    static WebIDL::ExceptionOr<GC::Ref<CompressionStream>> construct_impl(JS::Realm&, Bindings::CompressionFormat);
    virtual ~CompressionStream() override;

private:
    CompressionStream(JS::Realm&, GC::Ref<Streams::TransformStream>, Compressor, NonnullOwnPtr<AllocatingMemoryStream>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> compress_and_enqueue_chunk(JS::Value);
    WebIDL::ExceptionOr<void> compress_flush_and_enqueue();

    enum class Finish {
        No,
        Yes,
    };
    ErrorOr<ByteBuffer> compress(ReadonlyBytes, Finish);

    Compressor m_compressor;
    NonnullOwnPtr<AllocatingMemoryStream> m_output_stream;
};

}
