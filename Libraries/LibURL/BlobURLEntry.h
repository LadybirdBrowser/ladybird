/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibURL/Origin.h>

namespace URL {

// https://w3c.github.io/FileAPI/#blob-url-entry
struct BlobURLEntry {
    // This represents the raw bytes behind a 'Blob'
    struct Blob {
        String type;
        ByteBuffer data;
    };

    struct MediaSource { };

    using Object = Variant<Blob, MediaSource>;

    // This represents the parts of HTML::Environment that we need for a BlobURL entry.
    struct Environment {
        Origin origin;
    };

    Object object;
    Environment environment;
};

}
