/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/FileReaderSync.h>
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
    static GC::Ref<FileReaderSync> construct_impl();

    WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> read_as_array_buffer(JS::Realm&, Blob&);
    WebIDL::ExceptionOr<String> read_as_binary_string(JS::Realm&, Blob&);
    WebIDL::ExceptionOr<String> read_as_text(JS::Realm&, Blob&, Optional<String> const& encoding = {});
    WebIDL::ExceptionOr<String> read_as_data_url(JS::Realm&, Blob&);

private:
    explicit FileReaderSync();

    template<typename Result>
    WebIDL::ExceptionOr<Result> read_as(JS::Realm&, Blob&, FileReader::Type, Optional<String> const& encoding = {});
};

}
