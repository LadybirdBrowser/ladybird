/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/FileAPI/SerializedBlobURLEntry.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::FileAPI::SerializedBlobURLEntry const& entry)
{
    auto const* blob = entry.object.get_pointer<Web::FileAPI::SerializedBlobURLEntry::Blob>();
    TRY(encoder.encode(blob != nullptr));
    if (blob) {
        TRY(encoder.encode(blob->type));
        TRY(encoder.encode(blob->data));
    }
    TRY(encoder.encode(entry.origin));
    return {};
}

template<>
ErrorOr<Web::FileAPI::SerializedBlobURLEntry> decode(Decoder& decoder)
{
    Web::FileAPI::SerializedBlobURLEntry::Object object = Web::FileAPI::SerializedBlobURLEntry::MediaSource {};
    if (TRY(decoder.decode<bool>())) {
        auto type = TRY(decoder.decode<String>());
        auto data = TRY(decoder.decode<Core::AnonymousBuffer>());
        object = Web::FileAPI::SerializedBlobURLEntry::Blob { .type = move(type), .data = move(data) };
    }
    auto origin = TRY(decoder.decode<URL::Origin>());
    return Web::FileAPI::SerializedBlobURLEntry { .object = move(object), .origin = move(origin) };
}

}
