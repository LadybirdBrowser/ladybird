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
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Compression/CompressionStream.h>
#include <LibWeb/Streams/GenericTransformStream.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Compression {

using Decompressor = Variant<
    NonnullOwnPtr<Compress::ZlibDecompressor>,
    NonnullOwnPtr<Compress::DeflateDecompressor>,
    NonnullOwnPtr<Compress::GzipDecompressor>>;

// https://compression.spec.whatwg.org/#decompressionstream
class DecompressionStream final
    : public Bindings::PlatformObject
    , public Streams::GenericTransformStreamMixin {
    WEB_PLATFORM_OBJECT(DecompressionStream, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DecompressionStream);

public:
    static WebIDL::ExceptionOr<GC::Ref<DecompressionStream>> construct_impl(JS::Realm&, Bindings::CompressionFormat);
    virtual ~DecompressionStream() override;

private:
    DecompressionStream(JS::Realm&, GC::Ref<Streams::TransformStream>, Decompressor, NonnullOwnPtr<AllocatingMemoryStream>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> decompress_and_enqueue_chunk(JS::Value);
    WebIDL::ExceptionOr<void> decompress_flush_and_enqueue();

    ErrorOr<ByteBuffer> decompress(ReadonlyBytes);

    Decompressor m_decompressor;
    NonnullOwnPtr<AllocatingMemoryStream> m_input_stream;
};

}
