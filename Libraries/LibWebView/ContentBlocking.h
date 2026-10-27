/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>

namespace WebView {

enum class ContentBlockingState : u8 {
    Unavailable,
    Disabled,
    SiteDisabled,
    Enabled,
    NoRules,
};

// A resource the content blocker kept the page from loading, named by its host and what kind of resource it was, such as
// a script or an image.
struct BlockedResource {
    String host;
    StringView kind;
};

struct ContentBlockingSnapshot {
    Optional<String> site;
    ContentBlockingState state { ContentBlockingState::Unavailable };
    Vector<BlockedResource> blocked_resources;

    bool enabled() const { return state == ContentBlockingState::Enabled || state == ContentBlockingState::NoRules; }
    bool can_toggle() const { return site.has_value() && (enabled() || state == ContentBlockingState::SiteDisabled); }
};

}
