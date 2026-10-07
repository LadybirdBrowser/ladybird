/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/SystemFontMatcher.h>

// A platform without a font matcher matches no installed fonts.
namespace Gfx::SystemFontMatcher {

Optional<SystemFontMatch> match_family_style(StringView, u16, u16, u8)
{
    return {};
}

Optional<SystemFontMatch> match_code_point(u32, u16, u16, u8, bool)
{
    return {};
}

Optional<FlyString> resolve_generic_family(StringView, u16, u8)
{
    return {};
}

}
