/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/Blob.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::FileAPI {

using BlobPart = FlattenVariant<WebIDL::BufferSourceVariant, Variant<GC::Ref<Blob>, Utf16String>>;
using BlobParts = GC::ConservativeVector<BlobPart>;
using BlobPartsOrByteBuffer = Variant<BlobParts, ByteBuffer>;
using EndingType = Bindings::EndingType;
using BlobPropertyBag = Bindings::BlobPropertyBag;

[[nodiscard]] ErrorOr<Utf16String> convert_line_endings_to_native(Utf16View string);
[[nodiscard]] ErrorOr<ByteBuffer> process_blob_parts(BlobParts const& blob_parts, Optional<BlobPropertyBag> const& options = {});
[[nodiscard]] bool is_basic_latin(Utf16View view);

class WEB_API Blob
    : public Bindings::GCAllocatedWrappable
    , public Bindings::Serializable {
    WEB_WRAPPABLE(Blob, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Blob);

public:
    virtual ~Blob() override;

    [[nodiscard]] static GC::Ref<Blob> create(ByteBuffer, Utf16String type);
    [[nodiscard]] static GC::Ref<Blob> create(ByteBuffer bytes, String type) { return create(move(bytes), Utf16String::from_utf8(type)); }
    [[nodiscard]] static GC::Ref<Blob> create(Optional<BlobPartsOrByteBuffer> const& blob_parts_or_byte_buffer = {}, Optional<BlobPropertyBag> const& options = {});
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<Blob>> construct_impl(Optional<BlobParts> const& blob_parts, Optional<BlobPropertyBag> const& options);

    // https://w3c.github.io/FileAPI/#dfn-size
    u64 size() const { return m_byte_buffer.size(); }
    // https://w3c.github.io/FileAPI/#dfn-type
    Utf16String const& type() const { return m_type; }

    WebIDL::ExceptionOr<GC::Ref<Blob>> slice(Optional<i64> start = {}, Optional<i64> end = {}, Optional<Utf16String> const& content_type = {});
    ErrorOr<GC::Ref<Blob>> slice_blob(Optional<i64> start = {}, Optional<i64> end = {}, Optional<Utf16String> const& content_type = {});

    GC::Ref<Streams::ReadableStream> stream(JS::Object const& relevant_global_object);
    GC::Ref<WebIDL::Promise> text(JS::Object const& relevant_global_object);
    GC::Ref<WebIDL::Promise> array_buffer(JS::Object const& relevant_global_object);
    GC::Ref<WebIDL::Promise> bytes(JS::Object const& relevant_global_object);
    ReadonlyBytes raw_bytes() const LIFETIME_BOUND { return m_byte_buffer.bytes(); }

    GC::Ref<Streams::ReadableStream> get_stream(JS::Realm&);

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::StructuredSerializeWriter&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::StructuredSerializeReader&, HTML::DeserializationMemory&) override;

protected:
    Blob(ByteBuffer, Utf16String type);
    Blob(ByteBuffer);

    ByteBuffer m_byte_buffer {};
    Utf16String m_type {};

private:
    Blob();
};

}
