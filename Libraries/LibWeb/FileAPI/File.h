/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/FileAPI/Blob.h>

namespace Web::FileAPI {

struct FilePropertyBag : BlobPropertyBag {
    Optional<i64> last_modified;
};

class File : public Blob {
    WEB_PLATFORM_OBJECT(File, Blob);
    JS_DECLARE_ALLOCATOR(File);

public:
    static JS::NonnullGCPtr<File> create(JS::Realm& realm);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<File>> create(JS::Realm&, Vector<BlobPart> const& file_bits, String const& file_name, Optional<FilePropertyBag> const& options = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<File>> construct_impl(JS::Realm&, Vector<BlobPart> const& file_bits, String const& file_name, Optional<FilePropertyBag> const& options = {});

    virtual ~File() override;

    // https://w3c.github.io/FileAPI/#dfn-name
    String const& name() const { return m_name; }
    // https://w3c.github.io/FileAPI/#dfn-lastModified
    i64 last_modified() const { return m_last_modified; }

    virtual StringView interface_name() const override { return "File"sv; }

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::SerializationRecord& record, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(ReadonlySpan<u32> const&, size_t& position, HTML::DeserializationMemory&) override;

private:
    File(JS::Realm&, ByteBuffer, String file_name, String type, i64 last_modified);
    explicit File(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    String m_name;
    i64 m_last_modified { 0 };
};

}
