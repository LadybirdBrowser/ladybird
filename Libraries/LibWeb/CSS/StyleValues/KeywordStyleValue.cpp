/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "KeywordStyleValue.h"
#include <LibWeb/CSS/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>

namespace Web::CSS {

bool KeywordStyleValue::is_color(Keyword keyword)
{
    switch (keyword) {
    case Keyword::Accentcolor:
    case Keyword::Accentcolortext:
    case Keyword::Activeborder:
    case Keyword::Activecaption:
    case Keyword::Activetext:
    case Keyword::Appworkspace:
    case Keyword::Background:
    case Keyword::Buttonborder:
    case Keyword::Buttonface:
    case Keyword::Buttonhighlight:
    case Keyword::Buttonshadow:
    case Keyword::Buttontext:
    case Keyword::Canvas:
    case Keyword::Canvastext:
    case Keyword::Captiontext:
    case Keyword::Currentcolor:
    case Keyword::Field:
    case Keyword::Fieldtext:
    case Keyword::Graytext:
    case Keyword::Highlight:
    case Keyword::Highlighttext:
    case Keyword::Inactiveborder:
    case Keyword::Inactivecaption:
    case Keyword::Inactivecaptiontext:
    case Keyword::Infobackground:
    case Keyword::Infotext:
    case Keyword::LibwebButtonfacedisabled:
    case Keyword::LibwebButtonfacehover:
    case Keyword::Linktext:
    case Keyword::Mark:
    case Keyword::Marktext:
    case Keyword::Menu:
    case Keyword::Menutext:
    case Keyword::Scrollbar:
    case Keyword::Selecteditem:
    case Keyword::Selecteditemtext:
    case Keyword::Threeddarkshadow:
    case Keyword::Threedface:
    case Keyword::Threedhighlight:
    case Keyword::Threedlightshadow:
    case Keyword::Threedshadow:
    case Keyword::Visitedtext:
    case Keyword::Window:
    case Keyword::Windowframe:
    case Keyword::Windowtext:
        return true;
    default:
        return false;
    }
}

bool KeywordStyleValue::has_color() const
{
    return is_color(keyword());
}

Optional<Color> KeywordStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_storage;
    auto input = make_rust_color_resolution_input(color_resolution_context, length_storage);
    auto resolved = StyleValueFFI::rust_style_value_to_color(m_value.operator->(), &input);
    if (!resolved.resolved)
        return {};
    return Color(resolved.rgba[0], resolved.rgba[1], resolved.rgba[2], resolved.rgba[3]);
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
GC::Ref<CSSStyleValue> KeywordStyleValue::reify(Utf16FlyString const&) const
{
    // 1. Return a new CSSKeywordValue with its value internal slot set to the serialization of ident.
    return CSSKeywordValue::create(utf16_fly_string_from_keyword(keyword()));
}

}
