/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>

namespace WebView {

enum class ContentBlockingState : u8 {
    Unavailable,
    Disabled,
    SiteDisabled,
    Enabled,
    NoRules,
};

struct ContentBlockingSnapshot {
    Optional<String> site;
    ContentBlockingState state { ContentBlockingState::Unavailable };
    u64 blocked_request_count { 0 };

    bool enabled() const { return state == ContentBlockingState::Enabled || state == ContentBlockingState::NoRules; }
    bool can_toggle() const { return site.has_value() && (enabled() || state == ContentBlockingState::SiteDisabled); }
};

}
