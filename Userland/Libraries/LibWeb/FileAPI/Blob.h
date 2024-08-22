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
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::FileAPI {

using BlobPart = Variant<JS::Handle<WebIDL::BufferSource>, JS::Handle<Blob>, String>;

struct BlobPropertyBag {
    String type = String {};
    Bindings::EndingType endings;
};

[[nodiscard]] ErrorOr<String> convert_line_endings_to_native(StringView string);
[[nodiscard]] ErrorOr<ByteBuffer> process_blob_parts(Vector<BlobPart> const& blob_parts, Optional<BlobPropertyBag> const& options = {});
[[nodiscard]] bool is_basic_latin(StringView view);

class Blob
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(Blob, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Blob);

public:
    virtual ~Blob() override;

    [[nodiscard]] static JS::NonnullGCPtr<Blob> create(JS::Realm&, ByteBuffer, String type);
    [[nodiscard]] static JS::NonnullGCPtr<Blob> create(JS::Realm&, Optional<Vector<BlobPart>> const& blob_parts = {}, Optional<BlobPropertyBag> const& options = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Blob>> construct_impl(JS::Realm&, Optional<Vector<BlobPart>> const& blob_parts = {}, Optional<BlobPropertyBag> const& options = {});

    // https://w3c.github.io/FileAPI/#dfn-size
    u64 size() const { return m_byte_buffer.size(); }
    // https://w3c.github.io/FileAPI/#dfn-type
    String const& type() const { return m_type; }

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Blob>> slice(Optional<i64> start = {}, Optional<i64> end = {}, Optional<String> const& content_type = {});

    JS::NonnullGCPtr<Streams::ReadableStream> stream();
    JS::NonnullGCPtr<JS::Promise> text();
    JS::NonnullGCPtr<JS::Promise> array_buffer();
    JS::NonnullGCPtr<JS::Promise> bytes();

    ReadonlyBytes raw_bytes() const { return m_byte_buffer.bytes(); }

    JS::NonnullGCPtr<Streams::ReadableStream> get_stream();

    virtual StringView interface_name() const override { return "Blob"sv; }

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::SerializationRecord& record, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(ReadonlySpan<u32> const& record, size_t& position, HTML::DeserializationMemory&) override;

protected:
    Blob(JS::Realm&, ByteBuffer, String type);
    Blob(JS::Realm&, ByteBuffer);

    virtual void initialize(JS::Realm&) override;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Blob>> slice_blob(Optional<i64> start = {}, Optional<i64> end = {}, Optional<String> const& content_type = {});

    ByteBuffer m_byte_buffer {};
    String m_type {};

private:
    explicit Blob(JS::Realm&);
};

}
