/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSKeywordValue.h"
#include <LibGfx/Palette.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

String CSSKeywordValue::to_string() const
{
    return MUST(String::from_utf8(string_from_keyword(keyword())));
}

bool CSSKeywordValue::is_color(Keyword keyword)
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

bool CSSKeywordValue::has_color() const
{
    return is_color(keyword());
}

Color CSSKeywordValue::to_color(Optional<Layout::NodeWithStyle const&> node) const
{
    if (keyword() == Keyword::Currentcolor) {
        if (!node.has_value() || !node->has_style())
            return Color::Black;
        return node->computed_values().color();
    }

    // First, handle <system-color>s, since they don't require a node.
    // https://www.w3.org/TR/css-color-4/#css-system-colors
    // https://www.w3.org/TR/css-color-4/#deprecated-system-colors
    switch (keyword()) {
    case Keyword::Accentcolor:
        return SystemColor::accent_color();
    case Keyword::Accentcolortext:
        return SystemColor::accent_color_text();
    case Keyword::Activetext:
        return SystemColor::active_text();
    case Keyword::Buttonborder:
    case Keyword::Activeborder:
    case Keyword::Inactiveborder:
    case Keyword::Threeddarkshadow:
    case Keyword::Threedhighlight:
    case Keyword::Threedlightshadow:
    case Keyword::Threedshadow:
    case Keyword::Windowframe:
        return SystemColor::button_border();
    case Keyword::Buttonface:
    case Keyword::Buttonhighlight:
    case Keyword::Buttonshadow:
    case Keyword::Threedface:
        return SystemColor::button_face();
    case Keyword::Buttontext:
        return SystemColor::button_text();
    case Keyword::Canvas:
    case Keyword::Appworkspace:
    case Keyword::Background:
    case Keyword::Inactivecaption:
    case Keyword::Infobackground:
    case Keyword::Menu:
    case Keyword::Scrollbar:
    case Keyword::Window:
        return SystemColor::canvas();
    case Keyword::Canvastext:
    case Keyword::Activecaption:
    case Keyword::Captiontext:
    case Keyword::Infotext:
    case Keyword::Menutext:
    case Keyword::Windowtext:
        return SystemColor::canvas_text();
    case Keyword::Field:
        return SystemColor::field();
    case Keyword::Fieldtext:
        return SystemColor::field_text();
    case Keyword::Graytext:
    case Keyword::Inactivecaptiontext:
        return SystemColor::gray_text();
    case Keyword::Highlight:
        return SystemColor::highlight();
    case Keyword::Highlighttext:
        return SystemColor::highlight_text();
    case Keyword::Mark:
        return SystemColor::mark();
    case Keyword::Marktext:
        return SystemColor::mark_text();
    case Keyword::Selecteditem:
        return SystemColor::selected_item();
    case Keyword::Selecteditemtext:
        return SystemColor::selected_item_text();
    case Keyword::Visitedtext:
        return SystemColor::visited_text();
    default:
        break;
    }

    if (!node.has_value()) {
        // FIXME: Can't resolve palette colors without layout node.
        return Color::Black;
    }

    auto& document = node->document();
    if (keyword() == Keyword::LibwebLink || keyword() == Keyword::Linktext)
        return document.normal_link_color();

    auto palette = document.page().palette();
    switch (keyword()) {
    case Keyword::LibwebPaletteDesktopBackground:
        return palette.color(ColorRole::DesktopBackground);
    case Keyword::LibwebPaletteActiveWindowBorder1:
        return palette.color(ColorRole::ActiveWindowBorder1);
    case Keyword::LibwebPaletteActiveWindowBorder2:
        return palette.color(ColorRole::ActiveWindowBorder2);
    case Keyword::LibwebPaletteActiveWindowTitle:
        return palette.color(ColorRole::ActiveWindowTitle);
    case Keyword::LibwebPaletteInactiveWindowBorder1:
        return palette.color(ColorRole::InactiveWindowBorder1);
    case Keyword::LibwebPaletteInactiveWindowBorder2:
        return palette.color(ColorRole::InactiveWindowBorder2);
    case Keyword::LibwebPaletteInactiveWindowTitle:
        return palette.color(ColorRole::InactiveWindowTitle);
    case Keyword::LibwebPaletteMovingWindowBorder1:
        return palette.color(ColorRole::MovingWindowBorder1);
    case Keyword::LibwebPaletteMovingWindowBorder2:
        return palette.color(ColorRole::MovingWindowBorder2);
    case Keyword::LibwebPaletteMovingWindowTitle:
        return palette.color(ColorRole::MovingWindowTitle);
    case Keyword::LibwebPaletteHighlightWindowBorder1:
        return palette.color(ColorRole::HighlightWindowBorder1);
    case Keyword::LibwebPaletteHighlightWindowBorder2:
        return palette.color(ColorRole::HighlightWindowBorder2);
    case Keyword::LibwebPaletteHighlightWindowTitle:
        return palette.color(ColorRole::HighlightWindowTitle);
    case Keyword::LibwebPaletteMenuStripe:
        return palette.color(ColorRole::MenuStripe);
    case Keyword::LibwebPaletteMenuBase:
        return palette.color(ColorRole::MenuBase);
    case Keyword::LibwebPaletteMenuBaseText:
        return palette.color(ColorRole::MenuBaseText);
    case Keyword::LibwebPaletteMenuSelection:
        return palette.color(ColorRole::MenuSelection);
    case Keyword::LibwebPaletteMenuSelectionText:
        return palette.color(ColorRole::MenuSelectionText);
    case Keyword::LibwebPaletteWindow:
        return palette.color(ColorRole::Window);
    case Keyword::LibwebPaletteWindowText:
        return palette.color(ColorRole::WindowText);
    case Keyword::LibwebPaletteButton:
        return palette.color(ColorRole::Button);
    case Keyword::LibwebPaletteButtonText:
        return palette.color(ColorRole::ButtonText);
    case Keyword::LibwebPaletteBase:
        return palette.color(ColorRole::Base);
    case Keyword::LibwebPaletteBaseText:
        return palette.color(ColorRole::BaseText);
    case Keyword::LibwebPaletteThreedHighlight:
        return palette.color(ColorRole::ThreedHighlight);
    case Keyword::LibwebPaletteThreedShadow1:
        return palette.color(ColorRole::ThreedShadow1);
    case Keyword::LibwebPaletteThreedShadow2:
        return palette.color(ColorRole::ThreedShadow2);
    case Keyword::LibwebPaletteHoverHighlight:
        return palette.color(ColorRole::HoverHighlight);
    case Keyword::LibwebPaletteSelection:
        return palette.color(ColorRole::Selection);
    case Keyword::LibwebPaletteSelectionText:
        return palette.color(ColorRole::SelectionText);
    case Keyword::LibwebPaletteInactiveSelection:
        return palette.color(ColorRole::InactiveSelection);
    case Keyword::LibwebPaletteInactiveSelectionText:
        return palette.color(ColorRole::InactiveSelectionText);
    case Keyword::LibwebPaletteRubberBandFill:
        return palette.color(ColorRole::RubberBandFill);
    case Keyword::LibwebPaletteRubberBandBorder:
        return palette.color(ColorRole::RubberBandBorder);
    case Keyword::LibwebPaletteLink:
        return palette.color(ColorRole::Link);
    case Keyword::LibwebPaletteActiveLink:
        return palette.color(ColorRole::ActiveLink);
    case Keyword::LibwebPaletteVisitedLink:
        return palette.color(ColorRole::VisitedLink);
    case Keyword::LibwebPaletteRuler:
        return palette.color(ColorRole::Ruler);
    case Keyword::LibwebPaletteRulerBorder:
        return palette.color(ColorRole::RulerBorder);
    case Keyword::LibwebPaletteRulerActiveText:
        return palette.color(ColorRole::RulerActiveText);
    case Keyword::LibwebPaletteRulerInactiveText:
        return palette.color(ColorRole::RulerInactiveText);
    case Keyword::LibwebPaletteTextCursor:
        return palette.color(ColorRole::TextCursor);
    case Keyword::LibwebPaletteFocusOutline:
        return palette.color(ColorRole::FocusOutline);
    case Keyword::LibwebPaletteSyntaxComment:
        return palette.color(ColorRole::SyntaxComment);
    case Keyword::LibwebPaletteSyntaxNumber:
        return palette.color(ColorRole::SyntaxNumber);
    case Keyword::LibwebPaletteSyntaxString:
        return palette.color(ColorRole::SyntaxString);
    case Keyword::LibwebPaletteSyntaxType:
        return palette.color(ColorRole::SyntaxType);
    case Keyword::LibwebPaletteSyntaxPunctuation:
        return palette.color(ColorRole::SyntaxPunctuation);
    case Keyword::LibwebPaletteSyntaxOperator:
        return palette.color(ColorRole::SyntaxOperator);
    case Keyword::LibwebPaletteSyntaxKeyword:
        return palette.color(ColorRole::SyntaxKeyword);
    case Keyword::LibwebPaletteSyntaxControlKeyword:
        return palette.color(ColorRole::SyntaxControlKeyword);
    case Keyword::LibwebPaletteSyntaxIdentifier:
        return palette.color(ColorRole::SyntaxIdentifier);
    case Keyword::LibwebPaletteSyntaxPreprocessorStatement:
        return palette.color(ColorRole::SyntaxPreprocessorStatement);
    case Keyword::LibwebPaletteSyntaxPreprocessorValue:
        return palette.color(ColorRole::SyntaxPreprocessorValue);
    default:
        return {};
    }
}

}
