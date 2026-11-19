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
    NotRestorable = 1 << 16,
    Visible = 1 << 1,
    Pinned = 1 << 2,
    CurrentTabOfWindow = 1 << 3,
    Loading = 1 << 4,
    Crashed = 1 << 5,
    PlayingAudio = 1 << 6,
    RecentlyPlayedAudio = 1 << 7,
    CapturingAudio = 1 << 8,
    UsingLocation = 1 << 9,
    OwnsAView = 1 << 10,
    DevToolsConnected = 1 << 11,
    UserEdits = 1 << 12,
    // Only a tab hidden for long is kept for these, as they're work the tab does for the user in the background.
    ChangedTitleOrFaviconWhileHidden = 1 << 13,
    MayShowNotifications = 1 << 14,
};

AK_ENUM_BITWISE_OPERATORS(DiscardBlockers);

}
