/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Utf16String.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibWeb/Bindings/Blob.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#blob-section
// The C++ side of a Blob, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/file_api): what C++ creates
// and reads. A Blob that C++ holds can be a File.
class WEB_API Blob : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Blob, blob)

public:
    [[nodiscard]] static GC::Ref<Blob> create(ByteBuffer, Utf16String type);
    [[nodiscard]] static GC::Ref<Blob> create(ByteBuffer bytes, String const& type) { return create(move(bytes), Utf16String::from_utf8(type)); }
    [[nodiscard]] static GC::Ref<Blob> create(Core::AnonymousBuffer, Utf16String type);

    // https://w3c.github.io/FileAPI/#dfn-size
    u64 size() const { return raw_bytes().size(); }
    // https://w3c.github.io/FileAPI/#dfn-type
    Utf16String type() const;

    ReadonlyBytes raw_bytes() const LIFETIME_BOUND;
    ErrorOr<Core::AnonymousBuffer> shared_bytes();

    // https://w3c.github.io/FileAPI/#slice-blob
    GC::Ref<Blob> slice_blob(Optional<i64> start, Optional<i64> end, Optional<Utf16String> content_type) const;

    GC::Ref<Streams::ReadableStream> get_stream(JS::Realm&) const;

    template<typename T>
    bool fast_is() const
    requires(IsSame<T, File>)
    {
        return is_file();
    }

private:
    bool is_file() const;
};

}
