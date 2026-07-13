/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/File.h>
#include <LibWeb/FileAPI/Blob.h>

namespace Web::FileAPI {

class WEB_API File : public Blob {
    WEB_PLATFORM_OBJECT(File, Blob);
    GC_DECLARE_ALLOCATOR(File);

public:
    static GC::Ref<File> create(JS::Realm& realm);
    static WebIDL::ExceptionOr<GC::Ref<File>> create(JS::Realm&, BlobParts const& file_bits, Utf16String file_name, Optional<Bindings::FilePropertyBag> const& options = {});
    static WebIDL::ExceptionOr<GC::Ref<File>> construct_impl(JS::Realm&, BlobParts const& file_bits, Utf16String file_name, Optional<Bindings::FilePropertyBag> const& options = {});

    virtual ~File() override;

    // https://w3c.github.io/FileAPI/#dfn-name
    Utf16String const& name() const { return m_name; }
    // https://w3c.github.io/FileAPI/#dfn-lastModified
    i64 last_modified() const { return m_last_modified; }

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::StructuredSerializeWriter&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::StructuredSerializeReader&, HTML::DeserializationMemory&) override;

private:
    File(JS::Realm&, ByteBuffer, Utf16String file_name, Utf16String type, i64 last_modified);
    explicit File(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_name;
    i64 m_last_modified { 0 };
};

}
