/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <LibURL/Origin.h>

namespace URL {

// https://w3c.github.io/FileAPI/#blob-url-entry
struct BlobURLEntry {
    // An entry's object, which a URL record keeps alive without looking inside.
    class OpaqueObject : public RefCounted<OpaqueObject> {
    public:
        virtual ~OpaqueObject() = default;
    };

    // Names an entry in the user agent's blob URL store. Only a URL record parsed while the entry existed has one, and
    // the store keeps a revoked entry for as long as a token still names it. So a blob URL parsed before its entry was
    // revoked still resolves, and one parsed afterwards does not.
    using Token = u64;

    struct Blob {
        Token token { 0 };

        // NB: Null once the URL has crossed a process boundary, as an object is only usable in the process holding
        //     it. The store is asked for the object by token then.
        RefPtr<OpaqueObject> object;
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
