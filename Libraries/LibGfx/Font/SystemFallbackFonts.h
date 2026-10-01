/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>

namespace Gfx {

struct SystemFallbackFontKey {
    u32 code_point { 0 };
    u16 weight { 0 };
    u16 width { 0 };
    u8 slope { 0 };
    bool prefer_color_emoji { false };

    [[nodiscard]] bool operator==(SystemFallbackFontKey const&) const = default;
};

// The installed font that covers a code point at one style. The answer depends on the key and the
// font set alone, never on a document, so one memo serves every document in the process and the
// answer for a key never changes while the font set stands. The memo keeps one typeface per code
// point and style until the font set changes; the point size only picks a font from that typeface.
RefPtr<Font const> system_fallback_font(SystemFallbackFontKey const&, float point_size);

// Drops every memoized answer. Call this whenever the installed font set changes.
void clear_system_fallback_font_cache();

[[nodiscard]] size_t system_fallback_font_cache_size();

}
