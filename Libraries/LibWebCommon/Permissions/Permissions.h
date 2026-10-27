/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Export.h>

namespace Web::Permissions {

// The powerful features whose permission the UI process asks the user for.
enum class PermissionName : u8 {
    Geolocation,
    Microphone,
};

WEBCOMMON_API Optional<PermissionName> permission_name_from_string(StringView);
WEBCOMMON_API StringView to_string(PermissionName);

// What a site gets for a permission once every input is applied: the stored decision, an embargo, the global setting,
// and the operating system's own authorization.
enum class EffectiveState : u8 {
    Granted,
    Denied,
    Prompt,
};

// How the user answered a permission prompt. Dismissed and Ignored leave the permission undecided; Cancelled means the
// prompt went away without the user seeing it through, and counts for nothing.
enum class PromptOutcome : u8 {
    AllowAlways,
    AllowOnce,
    Block,
    Dismissed,
    Ignored,
    Cancelled,
};

struct WEBCOMMON_API PermissionSnapshotEntry {
    URL::Origin top_level_origin;
    PermissionName name;
    EffectiveState state;
};

// The effective states of some of a browsing session's sites, as the UI process sends them to a process that reads them:
// only the sites that process shows, listed in origins. Of those, a site with no entry gets the default for the
// permission's name.
struct WEBCOMMON_API PermissionSnapshot {
    u64 revision { 0 };
    Vector<URL::Origin> origins;
    Vector<PermissionSnapshotEntry> entries;
    EffectiveState geolocation_default { EffectiveState::Prompt };
    EffectiveState microphone_default { EffectiveState::Prompt };
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Permissions::PermissionSnapshotEntry const&);

template<>
WEBCOMMON_API ErrorOr<Web::Permissions::PermissionSnapshotEntry> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Permissions::PermissionSnapshot const&);

template<>
WEBCOMMON_API ErrorOr<Web::Permissions::PermissionSnapshot> decode(Decoder&);

}
