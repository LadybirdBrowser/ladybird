/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "KeywordStyleValue.h"
#include <LibGfx/Palette.h>
#include <LibWeb/CSS/CSSKeywordValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>

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
    {
        Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_storage;
        auto input = make_rust_color_resolution_input(color_resolution_context, length_storage);
        auto resolved = StyleValueFFI::rust_style_value_to_color(m_value.operator->(), &input);
        if (resolved.resolved)
            return Color(resolved.rgba[0], resolved.rgba[1], resolved.rgba[2], resolved.rgba[3]);
    }
    if (keyword() == Keyword::Currentcolor) {
        return color_resolution_context.current_color.value_or(Color::Black);
    }

    PreferredColorScheme scheme = color_resolution_context.color_scheme.value_or(PreferredColorScheme::Light);

    // https://www.w3.org/TR/css-color-4/#css-system-colors
    // https://www.w3.org/TR/css-color-4/#deprecated-system-colors
    switch (keyword()) {
    // AD-HOC: The spec says that 'accentcolor' and 'accentcolortext' should be resolved based on the value of the
    //         'accent-color' property, but this isn't implemented by any other browsers and isn't fully specified
    //          (https://github.com/w3c/csswg-drafts/issues/10971).
    case Keyword::Accentcolor:
        return SystemColor::accent_color(scheme);
    case Keyword::Accentcolortext:
        return SystemColor::accent_color_text(scheme);
    case Keyword::Activetext:
        return SystemColor::active_text(scheme);
    case Keyword::Buttonborder:
    case Keyword::Activeborder:
    case Keyword::Inactiveborder:
    case Keyword::Threeddarkshadow:
    case Keyword::Threedhighlight:
    case Keyword::Threedlightshadow:
    case Keyword::Threedshadow:
    case Keyword::Windowframe:
        return SystemColor::button_border(scheme);
    case Keyword::Buttonface:
    case Keyword::Buttonhighlight:
    case Keyword::Buttonshadow:
    case Keyword::Threedface:
        return SystemColor::button_face(scheme);
    case Keyword::Buttontext:
        return SystemColor::button_text(scheme);
    case Keyword::Canvas:
    case Keyword::Appworkspace:
    case Keyword::Background:
    case Keyword::Inactivecaption:
    case Keyword::Infobackground:
    case Keyword::Menu:
    case Keyword::Scrollbar:
    case Keyword::Window:
        return SystemColor::canvas(scheme);
    case Keyword::Canvastext:
    case Keyword::Activecaption:
    case Keyword::Captiontext:
    case Keyword::Infotext:
    case Keyword::Menutext:
    case Keyword::Windowtext:
        return SystemColor::canvas_text(scheme);
    case Keyword::Field:
        return SystemColor::field(scheme);
    case Keyword::Fieldtext:
        return SystemColor::field_text(scheme);
    case Keyword::Graytext:
    case Keyword::Inactivecaptiontext:
        return SystemColor::gray_text(scheme);
    case Keyword::Highlight:
        return SystemColor::highlight(scheme);
    case Keyword::Highlighttext:
        return SystemColor::highlight_text(scheme);
    case Keyword::Mark:
        return SystemColor::mark(scheme);
    case Keyword::Marktext:
        return SystemColor::mark_text(scheme);
    case Keyword::Selecteditem:
        return SystemColor::selected_item(scheme);
    case Keyword::Selecteditemtext:
        return SystemColor::selected_item_text(scheme);
    case Keyword::LibwebButtonfacedisabled:
        return SystemColor::button_face(scheme).with_alpha(128);
    case Keyword::LibwebButtonfacehover:
        return SystemColor::button_face(scheme).darkened(0.8f);
    case Keyword::Linktext:
        return SystemColor::link_text(scheme);
    case Keyword::Visitedtext:
        return SystemColor::visited_text(scheme);
    default:
        break;
    }

    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
GC::Ref<CSSStyleValue> KeywordStyleValue::reify(Utf16FlyString const&) const
{
    // 1. Return a new CSSKeywordValue with its value internal slot set to the serialization of ident.
    return CSSKeywordValue::create(utf16_fly_string_from_keyword(keyword()));
}

}
