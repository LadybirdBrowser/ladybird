/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/FileAPI/FileReader.h>
#include <LibWeb/Forward.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#FileReaderSync
class FileReaderSync : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(FileReaderSync, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(FileReaderSync);

public:
    virtual ~FileReaderSync() override;

    [[nodiscard]] static GC::Ref<FileReaderSync> create(JS::Realm&);
    static GC::Ref<FileReaderSync> construct_impl(JS::Realm&);

    WebIDL::ExceptionOr<GC::Root<JS::ArrayBuffer>> read_as_array_buffer(Blob&);
    WebIDL::ExceptionOr<String> read_as_binary_string(Blob&);
    WebIDL::ExceptionOr<String> read_as_text(Blob&, Optional<String> const& encoding = {});
    WebIDL::ExceptionOr<String> read_as_data_url(Blob&);

private:
    explicit FileReaderSync(JS::Realm&);

    template<typename Result>
    WebIDL::ExceptionOr<Result> read_as(Blob&, FileReader::Type, Optional<String> const& encoding = {});

    virtual void initialize(JS::Realm&) override;
};

}
