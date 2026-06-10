/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/FileAPI/FileReader.h>
#include <LibWeb/Forward.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#FileReaderSync
class FileReaderSync : public Bindings::Wrappable {
    WEB_WRAPPABLE(FileReaderSync, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(FileReaderSync);

public:
    virtual ~FileReaderSync() override;

    [[nodiscard]] static GC::Ref<FileReaderSync> create();

    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> read_as_array_buffer(JS::Realm&, Blob&);
    WebIDL::ExceptionOr<String> read_as_binary_string(Blob&);
    WebIDL::ExceptionOr<String> read_as_text(Blob&, Optional<String> const& encoding = {});
    WebIDL::ExceptionOr<String> read_as_data_url(Blob&);

private:
    explicit FileReaderSync();

    WebIDL::ExceptionOr<ByteBuffer> read_as_array_buffer_impl(Blob&);

    template<typename Result>
    WebIDL::ExceptionOr<Result> read_as(Blob&, FileReader::Type, Optional<String> const& encoding = {});
};

}
