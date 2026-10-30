/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/FileList.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#filelist-section
class WEB_API FileList : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(FileList, file_list)

public:
    [[nodiscard]] static GC::Ref<FileList> create();

    void add_file(GC::Ref<File>);

    // https://w3c.github.io/FileAPI/#dfn-length
    WebIDL::UnsignedLong length() const;

    // https://w3c.github.io/FileAPI/#dfn-item
    GC::Ptr<File> item(size_t index) const;
};

}
