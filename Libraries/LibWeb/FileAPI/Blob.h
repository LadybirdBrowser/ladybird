/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/BlobPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::FileAPI {

using BlobPart = Variant<GC::Root<WebIDL::BufferSource>, GC::Root<Blob>, String>;
using BlobParts = Vector<BlobPart>;
using BlobPartsOrByteBuffer = Variant<BlobParts, ByteBuffer>;

struct BlobPropertyBag {
    String type = String {};
    Bindings::EndingType endings;
};

[[nodiscard]] ErrorOr<String> convert_line_endings_to_native(StringView string);
[[nodiscard]] ErrorOr<ByteBuffer> process_blob_parts(BlobParts const& blob_parts, Optional<BlobPropertyBag> const& options = {});
[[nodiscard]] bool is_basic_latin(StringView view);

class WEB_API Blob
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(Blob, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Blob);

public:
    virtual ~Blob() override;

    [[nodiscard]] static GC::Ref<Blob> create(JS::Realm&, ByteBuffer, String type);
    [[nodiscard]] static GC::Ref<Blob> create(JS::Realm&, Optional<BlobPartsOrByteBuffer> const& blob_parts_or_byte_buffer = {}, Optional<BlobPropertyBag> const& options = {});
    static WebIDL::ExceptionOr<GC::Ref<Blob>> construct_impl(JS::Realm&, Optional<BlobParts> const& blob_parts = {}, Optional<BlobPropertyBag> const& options = {});

    // https://w3c.github.io/FileAPI/#dfn-size
    u64 size() const { return m_byte_buffer.size(); }
    // https://w3c.github.io/FileAPI/#dfn-type
    String const& type() const { return m_type; }

    WebIDL::ExceptionOr<GC::Ref<Blob>> slice(Optional<i64> start = {}, Optional<i64> end = {}, Optional<String> const& content_type = {});

    GC::Ref<Streams::ReadableStream> stream();
    GC::Ref<WebIDL::Promise> text();
    GC::Ref<WebIDL::Promise> array_buffer();
    GC::Ref<WebIDL::Promise> bytes();

    ReadonlyBytes raw_bytes() const LIFETIME_BOUND { return m_byte_buffer.bytes(); }

    GC::Ref<Streams::ReadableStream> get_stream();

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

protected:
    Blob(JS::Realm&, ByteBuffer, String type);
    Blob(JS::Realm&, ByteBuffer);

    virtual void initialize(JS::Realm&) override;

    WebIDL::ExceptionOr<GC::Ref<Blob>> slice_blob(Optional<i64> start = {}, Optional<i64> end = {}, Optional<String> const& content_type = {});

    ByteBuffer m_byte_buffer {};
    String m_type {};

private:
    explicit Blob(JS::Realm&);
};

}
