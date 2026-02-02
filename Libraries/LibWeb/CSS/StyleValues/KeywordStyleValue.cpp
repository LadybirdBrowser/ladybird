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
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RGBColorStyleValue.h>
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

Optional<Color> KeywordStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    if (keyword() == Keyword::Currentcolor) {
        return color_resolution_context.current_color.value_or(Color::Black);
    }

    PreferredColorScheme scheme = color_resolution_context.color_scheme.value_or(PreferredColorScheme::Light);

    // Calculate accent_color_text based on contrast to accent_color
    if (keyword() == Keyword::Accentcolortext) {
        // min_contrast = 10.2 is a magic number which provides the best accessibility trade-off based on:
        // 1. https://webaim.org/resources/contrastchecker/
        // 2. Current implementation of luminosity() and contrast_ratio() methods for Color instances

        // the baseline colors with the least contrast from black and white are #757575 and #767676
        // which score over 4.5 ratio for #fff and #000 accent_color_text values correspondingly
        auto constexpr min_contrast = 10.2;
        auto system_accent_text = SystemColor::accent_color_text(scheme);

        if (color_resolution_context.accent_color.value_or(SystemColor::accent_color(scheme)).contrast_ratio(system_accent_text) < min_contrast)
            return system_accent_text.inverted();

        return system_accent_text;
    }

    // First, handle <system-color>s, since they don't strictly require a node.
    // https://www.w3.org/TR/css-color-4/#css-system-colors
    // https://www.w3.org/TR/css-color-4/#deprecated-system-colors
    switch (keyword()) {
    case Keyword::Accentcolor:
        return color_resolution_context.accent_color.value_or(SystemColor::accent_color(scheme));
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
    default:
        break;
    }

    if (!color_resolution_context.document) {
        // FIXME: Can't resolve palette colors without a document.
        return Color::Black;
    }

    switch (keyword()) {
    case Keyword::LibwebLink:
    case Keyword::Linktext:
        return color_resolution_context.document->normal_link_color().value_or(SystemColor::link_text(scheme));
    case Keyword::Visitedtext:
        return color_resolution_context.document->visited_link_color().value_or(SystemColor::visited_text(scheme));
    case Keyword::Activetext:
        return color_resolution_context.document->active_link_color().value_or(SystemColor::active_text(scheme));
    default:
        break;
    }

    auto palette = color_resolution_context.document->page().palette();
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

ValueComparingNonnullRefPtr<StyleValue const> KeywordStyleValue::absolutized(ComputationContext const& context) const
{
    if (!has_color())
        return *this;

    // The currentcolor keyword computes to itself.
    // https://drafts.csswg.org/css-color-4/#resolving-other-colors
    if (keyword() == Keyword::Currentcolor)
        return *this;

    ColorResolutionContext color_resolution_context;
    if (context.abstract_element.has_value()) {
        color_resolution_context.document = context.abstract_element->document();
        color_resolution_context.calculation_resolution_context = CalculationResolutionContext::from_computation_context(context);
        color_resolution_context.color_scheme = context.color_scheme;
    }

    auto resolved_color = to_color(color_resolution_context);
    if (!resolved_color.has_value())
        return *this;

    return RGBColorStyleValue::create(
        NumberStyleValue::create(resolved_color->red()),
        NumberStyleValue::create(resolved_color->green()),
        NumberStyleValue::create(resolved_color->blue()),
        NumberStyleValue::create(resolved_color->alpha() / 255.0f),
        ColorSyntax::Legacy);
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
