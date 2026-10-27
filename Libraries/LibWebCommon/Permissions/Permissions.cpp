/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Permissions/Permissions.h>

namespace Web::Permissions {

Optional<PermissionName> permission_name_from_string(StringView name)
{
    if (name == "geolocation"sv)
        return PermissionName::Geolocation;
    if (name == "microphone"sv)
        return PermissionName::Microphone;
    return {};
}

StringView to_string(PermissionName name)
{
    switch (name) {
    case PermissionName::Geolocation:
        return "geolocation"sv;
    case PermissionName::Microphone:
        return "microphone"sv;
    }
    VERIFY_NOT_REACHED();
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Permissions::PermissionSnapshotEntry const& entry)
{
    TRY(encoder.encode(entry.top_level_origin));
    TRY(encoder.encode(entry.name));
    TRY(encoder.encode(entry.state));
    return {};
}

template<>
ErrorOr<Web::Permissions::PermissionSnapshotEntry> IPC::decode(Decoder& decoder)
{
    auto top_level_origin = TRY(decoder.decode<URL::Origin>());
    auto name = TRY(decoder.decode<Web::Permissions::PermissionName>());
    auto state = TRY(decoder.decode<Web::Permissions::EffectiveState>());
    return Web::Permissions::PermissionSnapshotEntry { move(top_level_origin), name, state };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Permissions::PermissionSnapshot const& snapshot)
{
    TRY(encoder.encode(snapshot.revision));
    TRY(encoder.encode(snapshot.origins));
    TRY(encoder.encode(snapshot.entries));
    TRY(encoder.encode(snapshot.geolocation_default));
    TRY(encoder.encode(snapshot.microphone_default));
    return {};
}

template<>
ErrorOr<Web::Permissions::PermissionSnapshot> IPC::decode(Decoder& decoder)
{
    auto revision = TRY(decoder.decode<u64>());
    auto origins = TRY(decoder.decode<Vector<URL::Origin>>());
    auto entries = TRY(decoder.decode<Vector<Web::Permissions::PermissionSnapshotEntry>>());
    auto geolocation_default = TRY(decoder.decode<Web::Permissions::EffectiveState>());
    auto microphone_default = TRY(decoder.decode<Web::Permissions::EffectiveState>());
    return Web::Permissions::PermissionSnapshot { revision, move(origins), move(entries), geolocation_default, microphone_default };
}
