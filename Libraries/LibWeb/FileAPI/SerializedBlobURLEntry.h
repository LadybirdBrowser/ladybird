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
#include <LibURL/BlobURLEntry.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#blob-url-entry
struct SerializedBlobURLEntry {
    struct Blob {
        URL::BlobURLEntry::Token token { 0 };
        String type;
        Core::AnonymousBuffer data;
    };

    struct MediaSource { };

    using Object = Variant<Blob, MediaSource>;

    Object object;
    URL::Origin origin;
};

class WEB_API BlobURLObject final : public URL::BlobURLEntry::OpaqueObject {
public:
    static NonnullRefPtr<BlobURLObject> create(String type, Core::AnonymousBuffer data)
    {
        return adopt_ref(*new BlobURLObject(move(type), move(data)));
    }

    String const& type() const { return m_type; }
    Core::AnonymousBuffer const& data() const { return m_data; }

private:
    BlobURLObject(String type, Core::AnonymousBuffer data)
        : m_type(move(type))
        , m_data(move(data))
    {
    }

    String m_type;
    Core::AnonymousBuffer m_data;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::FileAPI::SerializedBlobURLEntry const&);

template<>
WEB_API ErrorOr<Web::FileAPI::SerializedBlobURLEntry> decode(Decoder&);

}
