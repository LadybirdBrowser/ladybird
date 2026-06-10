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

class File : public Blob {
    WEB_WRAPPABLE(File, Blob);
    GC_DECLARE_ALLOCATOR(File);

public:
    static GC::Ref<File> create();
    static ErrorOr<GC::Ref<File>> create(BlobParts const& file_bits, String const& file_name, Optional<FilePropertyBag> const& options = {});
    static WebIDL::ExceptionOr<GC::Ref<File>> construct_impl(BlobParts const& file_bits, String const& file_name, Optional<FilePropertyBag> const& options);

    virtual ~File() override;

    // https://w3c.github.io/FileAPI/#dfn-name
    String const& name() const { return m_name; }
    // https://w3c.github.io/FileAPI/#dfn-lastModified
    i64 last_modified() const { return m_last_modified; }

    virtual WebIDL::ExceptionOr<void> serialization_steps(JS::Realm&, HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

private:
    File(ByteBuffer, String file_name, String type, i64 last_modified);
    File();

    String m_name;
    i64 m_last_modified { 0 };
};

}
