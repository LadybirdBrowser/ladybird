/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/SystemFontMatcher.h>
#include <LibGfx/Font/TypefaceSkia.h>

namespace Gfx::SystemFontMatcher {

static Optional<SystemFontMatch> match_of(ErrorOr<RefPtr<TypefaceSkia>> typeface)
{
    if (typeface.is_error() || !typeface.value())
        return {};
    return SystemFontMatch { NonnullRefPtr<Typeface> { typeface.release_value().release_nonnull() } };
}

Optional<SystemFontMatch> match_family_style(StringView family, u16 weight, u16 width, u8 slope)
{
    return match_of(TypefaceSkia::match_family_style(family, weight, width, slope));
}

Optional<SystemFontMatch> match_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    return match_of(TypefaceSkia::find_typeface_for_code_point(code_point, weight, width, slope, prefer_color_emoji));
}

Optional<FlyString> resolve_generic_family(StringView family, u16 weight, u8 slope)
{
    return TypefaceSkia::resolve_generic_family(family, weight, slope);
}

}
