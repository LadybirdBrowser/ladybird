/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/File.h>
#include <LibWeb/FileAPI/Blob.h>

namespace Web::FileAPI {

using FilePropertyBag = Bindings::FilePropertyBag;

class WEB_API File : public Blob {
    WEB_WRAPPABLE(File, Blob);
    GC_DECLARE_ALLOCATOR(File);

public:
    static GC::Ref<File> create();
    static ErrorOr<GC::Ref<File>> create(BlobParts const& file_bits, Utf16String const& file_name, Optional<FilePropertyBag> const& options = {});
    static ErrorOr<GC::Ref<File>> create(BlobParts const& file_bits, String const& file_name, Optional<FilePropertyBag> const& options = {}) { return create(file_bits, Utf16String::from_utf8(file_name), options); }
    static WebIDL::ExceptionOr<GC::Ref<File>> construct_impl(BlobParts const& file_bits, Utf16String const& file_name, Optional<FilePropertyBag> const& options);

    virtual ~File() override;

    // https://w3c.github.io/FileAPI/#dfn-name
    Utf16String const& name() const { return m_name; }
    // https://w3c.github.io/FileAPI/#dfn-lastModified
    i64 last_modified() const { return m_last_modified; }

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::StructuredSerializeWriter&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::StructuredSerializeReader&, HTML::DeserializationMemory&) override;

private:
    File(ByteBuffer, Utf16String file_name, Utf16String type, i64 last_modified);
    File();

    Utf16String m_name;
    i64 m_last_modified { 0 };
};

}
