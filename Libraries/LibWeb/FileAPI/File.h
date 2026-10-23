/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/File.h>
#include <LibWeb/FileAPI/Blob.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#file-section
// The C++ side of a File, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/file_api): what C++ creates
// and reads.
class WEB_API File : public Blob {
    WEB_RUST_INTERFACE(File, file)

public:
    // https://w3c.github.io/FileAPI/#ref-for-dom-file-file, given the bytes as the one part of fileBits, and options
    // with the type and, if it has a value, the last modified date.
    static GC::Ref<File> create(ReadonlyBytes, Utf16String file_name, Utf16String type, Optional<i64> last_modified = {});

    // https://w3c.github.io/FileAPI/#dfn-name
    Utf16String name() const;
    // https://w3c.github.io/FileAPI/#dfn-lastModified
    i64 last_modified() const;
};

}
