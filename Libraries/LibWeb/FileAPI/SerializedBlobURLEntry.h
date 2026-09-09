/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#blob-url-entry
struct SerializedBlobURLEntry {
    struct Blob {
        String type;
        Core::AnonymousBuffer data;
    };

    struct MediaSource { };

    using Object = Variant<Blob, MediaSource>;

    Object object;
    URL::Origin origin;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::FileAPI::SerializedBlobURLEntry const&);

template<>
WEB_API ErrorOr<Web::FileAPI::SerializedBlobURLEntry> decode(Decoder&);

}
