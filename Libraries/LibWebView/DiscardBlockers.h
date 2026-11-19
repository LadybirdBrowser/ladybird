/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Types.h>

namespace WebView {

// What keeps a tab from being discarded, which lets it stay loaded while the user doesn't see it.
enum class DiscardBlockers : u32 {
    None = 0,
    NotATab = 1 << 0,
    NotRestorable = 1 << 1,
    Visible = 1 << 2,
    Pinned = 1 << 3,
    CurrentTabOfWindow = 1 << 4,
    Loading = 1 << 5,
    Crashed = 1 << 6,
    PlayingAudio = 1 << 7,
    RecentlyPlayedAudio = 1 << 8,
    CapturingAudio = 1 << 9,
    UsingLocation = 1 << 10,
    OwnsAView = 1 << 11,
    DevToolsConnected = 1 << 12,
    UserEdits = 1 << 13,
    Downloading = 1 << 14,
    AwaitingPermissionPrompt = 1 << 15,
    // Only a tab hidden for long is kept for these, as they're work the tab does for the user in the background.
    ChangedTitleWhileHidden = 1 << 16,
    ChangedFaviconWhileHidden = 1 << 17,
    MayShowNotifications = 1 << 18,
};

AK_ENUM_BITWISE_OPERATORS(DiscardBlockers);

}
