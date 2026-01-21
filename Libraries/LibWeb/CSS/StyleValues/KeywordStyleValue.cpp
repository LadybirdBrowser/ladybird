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
#include <LibWeb/CSS/Color.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

void KeywordStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    builder.append(string_from_keyword(keyword()));
}

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
    case Keyword::LibwebLink:
    case Keyword::LibwebPaletteActiveLink:
    case Keyword::LibwebPaletteActiveWindowBorder1:
    case Keyword::LibwebPaletteActiveWindowBorder2:
    case Keyword::LibwebPaletteActiveWindowTitle:
    case Keyword::LibwebPaletteBase:
    case Keyword::LibwebPaletteBaseText:
    case Keyword::LibwebPaletteButton:
    case Keyword::LibwebPaletteButtonText:
    case Keyword::LibwebPaletteDesktopBackground:
    case Keyword::LibwebPaletteFocusOutline:
    case Keyword::LibwebPaletteHighlightWindowBorder1:
    case Keyword::LibwebPaletteHighlightWindowBorder2:
    case Keyword::LibwebPaletteHighlightWindowTitle:
    case Keyword::LibwebPaletteHoverHighlight:
    case Keyword::LibwebPaletteInactiveSelection:
    case Keyword::LibwebPaletteInactiveSelectionText:
    case Keyword::LibwebPaletteInactiveWindowBorder1:
    case Keyword::LibwebPaletteInactiveWindowBorder2:
    case Keyword::LibwebPaletteInactiveWindowTitle:
    case Keyword::LibwebPaletteLink:
    case Keyword::LibwebPaletteMenuBase:
    case Keyword::LibwebPaletteMenuBaseText:
    case Keyword::LibwebPaletteMenuSelection:
    case Keyword::LibwebPaletteMenuSelectionText:
    case Keyword::LibwebPaletteMenuStripe:
    case Keyword::LibwebPaletteMovingWindowBorder1:
    case Keyword::LibwebPaletteMovingWindowBorder2:
    case Keyword::LibwebPaletteMovingWindowTitle:
    case Keyword::LibwebPaletteRubberBandBorder:
    case Keyword::LibwebPaletteRubberBandFill:
    case Keyword::LibwebPaletteRuler:
    case Keyword::LibwebPaletteRulerActiveText:
    case Keyword::LibwebPaletteRulerBorder:
    case Keyword::LibwebPaletteRulerInactiveText:
    case Keyword::LibwebPaletteSelection:
    case Keyword::LibwebPaletteSelectionText:
    case Keyword::LibwebPaletteSyntaxComment:
    case Keyword::LibwebPaletteSyntaxControlKeyword:
    case Keyword::LibwebPaletteSyntaxIdentifier:
    case Keyword::LibwebPaletteSyntaxKeyword:
    case Keyword::LibwebPaletteSyntaxNumber:
    case Keyword::LibwebPaletteSyntaxOperator:
    case Keyword::LibwebPaletteSyntaxPreprocessorStatement:
    case Keyword::LibwebPaletteSyntaxPreprocessorValue:
    case Keyword::LibwebPaletteSyntaxPunctuation:
    case Keyword::LibwebPaletteSyntaxString:
    case Keyword::LibwebPaletteSyntaxType:
    case Keyword::LibwebPaletteTextCursor:
    case Keyword::LibwebPaletteThreedHighlight:
    case Keyword::LibwebPaletteThreedShadow1:
    case Keyword::LibwebPaletteThreedShadow2:
    case Keyword::LibwebPaletteVisitedLink:
    case Keyword::LibwebPaletteWindow:
    case Keyword::LibwebPaletteWindowText:
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

Optional<CSS::Color> KeywordStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    if (keyword() == Keyword::Currentcolor)
        return color_resolution_context.current_color.value_or(InitialValues::color());

    PreferredColorScheme scheme = color_resolution_context.color_scheme.value_or(PreferredColorScheme::Light);

    // First, handle <system-color>s, since they don't strictly require a node.
    // https://www.w3.org/TR/css-color-4/#css-system-colors
    // https://www.w3.org/TR/css-color-4/#deprecated-system-colors
    switch (keyword()) {
    case Keyword::Accentcolor:
        return CSS::Color { SystemColor::accent_color(scheme), *this };
    case Keyword::Accentcolortext:
        return CSS::Color { SystemColor::accent_color_text(scheme), *this };
    case Keyword::Buttonborder:
    case Keyword::Activeborder:
    case Keyword::Inactiveborder:
    case Keyword::Threeddarkshadow:
    case Keyword::Threedhighlight:
    case Keyword::Threedlightshadow:
    case Keyword::Threedshadow:
    case Keyword::Windowframe:
        return CSS::Color { SystemColor::button_border(scheme), *this };
    case Keyword::Buttonface:
    case Keyword::Buttonhighlight:
    case Keyword::Buttonshadow:
    case Keyword::Threedface:
        return CSS::Color { SystemColor::button_face(scheme), *this };
    case Keyword::Buttontext:
        return CSS::Color { SystemColor::button_text(scheme), *this };
    case Keyword::Canvas:
    case Keyword::Appworkspace:
    case Keyword::Background:
    case Keyword::Inactivecaption:
    case Keyword::Infobackground:
    case Keyword::Menu:
    case Keyword::Scrollbar:
    case Keyword::Window:
        return CSS::Color { SystemColor::canvas(scheme), *this };
    case Keyword::Canvastext:
    case Keyword::Activecaption:
    case Keyword::Captiontext:
    case Keyword::Infotext:
    case Keyword::Menutext:
    case Keyword::Windowtext:
        return CSS::Color { SystemColor::canvas_text(scheme), *this };
    case Keyword::Field:
        return CSS::Color { SystemColor::field(scheme), *this };
    case Keyword::Fieldtext:
        return CSS::Color { SystemColor::field_text(scheme), *this };
    case Keyword::Graytext:
    case Keyword::Inactivecaptiontext:
        return CSS::Color { SystemColor::gray_text(scheme), *this };
    case Keyword::Highlight:
        return CSS::Color { SystemColor::highlight(scheme), *this };
    case Keyword::Highlighttext:
        return CSS::Color { SystemColor::highlight_text(scheme), *this };
    case Keyword::Mark:
        return CSS::Color { SystemColor::mark(scheme), *this };
    case Keyword::Marktext:
        return CSS::Color { SystemColor::mark_text(scheme), *this };
    case Keyword::Selecteditem:
        return CSS::Color { SystemColor::selected_item(scheme), *this };
    case Keyword::Selecteditemtext:
        return CSS::Color { SystemColor::selected_item_text(scheme), *this };
    case Keyword::LibwebButtonfacedisabled:
        return CSS::Color { SystemColor::button_face(scheme).with_alpha(128), *this };
    case Keyword::LibwebButtonfacehover:
        return CSS::Color { SystemColor::button_face(scheme).darkened(0.8f), *this };
    default:
        break;
    }

    if (!color_resolution_context.document) {
        // FIXME: Can't resolve palette colors without a document.
        return CSS::Color { Gfx::Color::Black, *this };
    }

    switch (keyword()) {
    case Keyword::LibwebLink:
    case Keyword::Linktext:
        return CSS::Color { color_resolution_context.document->normal_link_color().value_or(SystemColor::link_text(scheme)), *this };
    case Keyword::Visitedtext:
        return CSS::Color { color_resolution_context.document->visited_link_color().value_or(SystemColor::visited_text(scheme)), *this };
    case Keyword::Activetext:
        return CSS::Color { color_resolution_context.document->active_link_color().value_or(SystemColor::active_text(scheme)), *this };
    default:
        break;
    }

    auto palette = color_resolution_context.document->page().palette();
    switch (keyword()) {
    case Keyword::LibwebPaletteDesktopBackground:
        return CSS::Color { palette.color(ColorRole::DesktopBackground), *this };
    case Keyword::LibwebPaletteActiveWindowBorder1:
        return CSS::Color { palette.color(ColorRole::ActiveWindowBorder1), *this };
    case Keyword::LibwebPaletteActiveWindowBorder2:
        return CSS::Color { palette.color(ColorRole::ActiveWindowBorder2), *this };
    case Keyword::LibwebPaletteActiveWindowTitle:
        return CSS::Color { palette.color(ColorRole::ActiveWindowTitle), *this };
    case Keyword::LibwebPaletteInactiveWindowBorder1:
        return CSS::Color { palette.color(ColorRole::InactiveWindowBorder1), *this };
    case Keyword::LibwebPaletteInactiveWindowBorder2:
        return CSS::Color { palette.color(ColorRole::InactiveWindowBorder2), *this };
    case Keyword::LibwebPaletteInactiveWindowTitle:
        return CSS::Color { palette.color(ColorRole::InactiveWindowTitle), *this };
    case Keyword::LibwebPaletteMovingWindowBorder1:
        return CSS::Color { palette.color(ColorRole::MovingWindowBorder1), *this };
    case Keyword::LibwebPaletteMovingWindowBorder2:
        return CSS::Color { palette.color(ColorRole::MovingWindowBorder2), *this };
    case Keyword::LibwebPaletteMovingWindowTitle:
        return CSS::Color { palette.color(ColorRole::MovingWindowTitle), *this };
    case Keyword::LibwebPaletteHighlightWindowBorder1:
        return CSS::Color { palette.color(ColorRole::HighlightWindowBorder1), *this };
    case Keyword::LibwebPaletteHighlightWindowBorder2:
        return CSS::Color { palette.color(ColorRole::HighlightWindowBorder2), *this };
    case Keyword::LibwebPaletteHighlightWindowTitle:
        return CSS::Color { palette.color(ColorRole::HighlightWindowTitle), *this };
    case Keyword::LibwebPaletteMenuStripe:
        return CSS::Color { palette.color(ColorRole::MenuStripe), *this };
    case Keyword::LibwebPaletteMenuBase:
        return CSS::Color { palette.color(ColorRole::MenuBase), *this };
    case Keyword::LibwebPaletteMenuBaseText:
        return CSS::Color { palette.color(ColorRole::MenuBaseText), *this };
    case Keyword::LibwebPaletteMenuSelection:
        return CSS::Color { palette.color(ColorRole::MenuSelection), *this };
    case Keyword::LibwebPaletteMenuSelectionText:
        return CSS::Color { palette.color(ColorRole::MenuSelectionText), *this };
    case Keyword::LibwebPaletteWindow:
        return CSS::Color { palette.color(ColorRole::Window), *this };
    case Keyword::LibwebPaletteWindowText:
        return CSS::Color { palette.color(ColorRole::WindowText), *this };
    case Keyword::LibwebPaletteButton:
        return CSS::Color { palette.color(ColorRole::Button), *this };
    case Keyword::LibwebPaletteButtonText:
        return CSS::Color { palette.color(ColorRole::ButtonText), *this };
    case Keyword::LibwebPaletteBase:
        return CSS::Color { palette.color(ColorRole::Base), *this };
    case Keyword::LibwebPaletteBaseText:
        return CSS::Color { palette.color(ColorRole::BaseText), *this };
    case Keyword::LibwebPaletteThreedHighlight:
        return CSS::Color { palette.color(ColorRole::ThreedHighlight), *this };
    case Keyword::LibwebPaletteThreedShadow1:
        return CSS::Color { palette.color(ColorRole::ThreedShadow1), *this };
    case Keyword::LibwebPaletteThreedShadow2:
        return CSS::Color { palette.color(ColorRole::ThreedShadow2), *this };
    case Keyword::LibwebPaletteHoverHighlight:
        return CSS::Color { palette.color(ColorRole::HoverHighlight), *this };
    case Keyword::LibwebPaletteSelection:
        return CSS::Color { palette.color(ColorRole::Selection), *this };
    case Keyword::LibwebPaletteSelectionText:
        return CSS::Color { palette.color(ColorRole::SelectionText), *this };
    case Keyword::LibwebPaletteInactiveSelection:
        return CSS::Color { palette.color(ColorRole::InactiveSelection), *this };
    case Keyword::LibwebPaletteInactiveSelectionText:
        return CSS::Color { palette.color(ColorRole::InactiveSelectionText), *this };
    case Keyword::LibwebPaletteRubberBandFill:
        return CSS::Color { palette.color(ColorRole::RubberBandFill), *this };
    case Keyword::LibwebPaletteRubberBandBorder:
        return CSS::Color { palette.color(ColorRole::RubberBandBorder), *this };
    case Keyword::LibwebPaletteLink:
        return CSS::Color { palette.color(ColorRole::Link), *this };
    case Keyword::LibwebPaletteActiveLink:
        return CSS::Color { palette.color(ColorRole::ActiveLink), *this };
    case Keyword::LibwebPaletteVisitedLink:
        return CSS::Color { palette.color(ColorRole::VisitedLink), *this };
    case Keyword::LibwebPaletteRuler:
        return CSS::Color { palette.color(ColorRole::Ruler), *this };
    case Keyword::LibwebPaletteRulerBorder:
        return CSS::Color { palette.color(ColorRole::RulerBorder), *this };
    case Keyword::LibwebPaletteRulerActiveText:
        return CSS::Color { palette.color(ColorRole::RulerActiveText), *this };
    case Keyword::LibwebPaletteRulerInactiveText:
        return CSS::Color { palette.color(ColorRole::RulerInactiveText), *this };
    case Keyword::LibwebPaletteTextCursor:
        return CSS::Color { palette.color(ColorRole::TextCursor), *this };
    case Keyword::LibwebPaletteFocusOutline:
        return CSS::Color { palette.color(ColorRole::FocusOutline), *this };
    case Keyword::LibwebPaletteSyntaxComment:
        return CSS::Color { palette.color(ColorRole::SyntaxComment), *this };
    case Keyword::LibwebPaletteSyntaxNumber:
        return CSS::Color { palette.color(ColorRole::SyntaxNumber), *this };
    case Keyword::LibwebPaletteSyntaxString:
        return CSS::Color { palette.color(ColorRole::SyntaxString), *this };
    case Keyword::LibwebPaletteSyntaxType:
        return CSS::Color { palette.color(ColorRole::SyntaxType), *this };
    case Keyword::LibwebPaletteSyntaxPunctuation:
        return CSS::Color { palette.color(ColorRole::SyntaxPunctuation), *this };
    case Keyword::LibwebPaletteSyntaxOperator:
        return CSS::Color { palette.color(ColorRole::SyntaxOperator), *this };
    case Keyword::LibwebPaletteSyntaxKeyword:
        return CSS::Color { palette.color(ColorRole::SyntaxKeyword), *this };
    case Keyword::LibwebPaletteSyntaxControlKeyword:
        return CSS::Color { palette.color(ColorRole::SyntaxControlKeyword), *this };
    case Keyword::LibwebPaletteSyntaxIdentifier:
        return CSS::Color { palette.color(ColorRole::SyntaxIdentifier), *this };
    case Keyword::LibwebPaletteSyntaxPreprocessorStatement:
        return CSS::Color { palette.color(ColorRole::SyntaxPreprocessorStatement), *this };
    case Keyword::LibwebPaletteSyntaxPreprocessorValue:
        return CSS::Color { palette.color(ColorRole::SyntaxPreprocessorValue), *this };
    default:
        return {};
    }
}

Vector<Parser::ComponentValue> KeywordStyleValue::tokenize() const
{
    return { Parser::Token::create_ident(FlyString::from_utf8_without_validation(string_from_keyword(m_keyword).bytes())) };
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
GC::Ref<CSSStyleValue> KeywordStyleValue::reify(JS::Realm& realm, FlyString const&) const
{
    // 1. Return a new CSSKeywordValue with its value internal slot set to the serialization of ident.
    return CSSKeywordValue::create(realm, FlyString::from_utf8_without_validation(string_from_keyword(m_keyword).bytes()));
}

}
