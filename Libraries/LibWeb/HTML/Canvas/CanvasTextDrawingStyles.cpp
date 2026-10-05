/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2023, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CanvasTextDrawingStyles.h"
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontResolution.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

template<typename CanvasType>
Utf16String CanvasTextDrawingStyles<CanvasType>::font() const
{
    // When font style value is empty return default string
    if (!drawing_state().font_style_value) {
        return "10px sans-serif"_utf16;
    }

    // On getting, the font attribute must return the serialized form of the current font of the context (with no 'line-height' component).
    return drawing_state().font_style_value->to_utf16_string(CSS::SerializationMode::ResolvedValue);
}

template<typename CanvasType>
void CanvasTextDrawingStyles<CanvasType>::set_font(Utf16View font)
{
    // The font IDL attribute, on setting, must be parsed as a CSS <'font'> value (but without supporting property-independent style sheet syntax like 'inherit'),
    // and the resulting font must be assigned to the context, with the 'line-height' component forced to 'normal', with the 'font-size' component converted to CSS pixels,
    // and with system fonts being computed to explicit values.

    auto const parsing_context = [&]() {
        if constexpr (SameAs<CanvasType, HTML::HTMLCanvasElement>)
            return CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::OnScreenCanvasContextFontValue };
        return CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue };
    }();

    auto font_style_value_result = parse_css_value(parsing_context, font, CSS::PropertyID::Font);

    // If the new value is syntactically incorrect (including using property-independent style sheet syntax like 'inherit' or 'initial'), then it must be ignored, without assigning a new font value.
    // NOTE: ShorthandStyleValue should be the only valid option here. We implicitly VERIFY this below.
    if (!font_style_value_result || !font_style_value_result->is_shorthand()) {
        return;
    }

    // Load font with font style value properties
    auto const& font_style_value = font_style_value_result->as_shorthand();
    auto& canvas_element = static_cast<CanvasType&>(this->canvas_host());

    auto computed_math_depth = CSS::InitialValues::math_depth();

    // FIXME: We will need to absolutize this once we support ident() functions
    auto font_family = font_style_value.longhand(CSS::PropertyID::FontFamily);

    Optional<DOM::AbstractElement> inheritance_parent;

    if constexpr (SameAs<CanvasType, HTML::HTMLCanvasElement>) {
        // NB: Once pending style work is settled, the canvas's installed style is current, unless it is below
        //     display:none, where style updates leave it stale. Only then is its style computed again.
        auto& document = canvas_element.document();
        Layout::ForcedReadScope read { document };
        document.update_style_for_element(DOM::AbstractElement { canvas_element }, DOM::Document::StyleUpdateMode::OnlyIfNeeded);
        auto style_record = canvas_element.style_record_identity();
        if (!style_record || has_flag(document.style_computer().style_engine().style_record_dependency_flags(read, style_record), CSS::StyleRecordDependencyFlag::InDisplayNoneSubtree))
            document.update_style_for_element(DOM::AbstractElement { canvas_element });

        if (canvas_element.navigable() && canvas_element.is_connected()) {
            // NOTE: Since we can't set a math depth directly here we always use the inherited value for the computed value
            computed_math_depth = canvas_element.template style_group<CSS::ComputedValues::FontValues>()->math_depth;

            // NOTE: The canvas itself is considered the inheritance parent
            inheritance_parent = canvas_element;
        }
    }

    auto computation_context = canvas_element.canvas_font_computation_context();

    auto const& computed_font_size = CSS::StyleComputer::compute_font_size(font_style_value.longhand(CSS::PropertyID::FontSize)->absolutized(computation_context), computed_math_depth, inheritance_parent, CSSPixels { 10 });
    auto const& computed_font_style = CSS::StyleComputer::compute_font_style(font_style_value.longhand(CSS::PropertyID::FontStyle)->absolutized(computation_context));
    auto const& computed_font_weight = CSS::StyleComputer::compute_font_weight(font_style_value.longhand(CSS::PropertyID::FontWeight)->absolutized(computation_context), inheritance_parent);
    auto const& computed_font_width = CSS::StyleComputer::compute_font_width(font_style_value.longhand(CSS::PropertyID::FontWidth)->absolutized(computation_context));
    // NB: This doesn't require absolutization since only the font-variant-caps longhand can be set and that can only be
    //     a keyword value
    auto const& computed_font_variant = font_style_value.longhand(CSS::PropertyID::FontVariant).release_nonnull();

    drawing_state().font_style_value = CSS::ShorthandStyleValue::create(
        CSS::PropertyID::Font,
        {
            // Set explicitly https://drafts.csswg.org/css-fonts/#set-explicitly
            CSS::PropertyID::FontFamily,
            CSS::PropertyID::FontSize,
            CSS::PropertyID::FontWidth,
            CSS::PropertyID::FontStyle,
            CSS::PropertyID::FontVariant,
            CSS::PropertyID::FontWeight,
            CSS::PropertyID::LineHeight,

            // Reset implicitly https://drafts.csswg.org/css-fonts/#reset-implicitly
            CSS::PropertyID::FontFeatureSettings,
            CSS::PropertyID::FontKerning,
            CSS::PropertyID::FontLanguageOverride,
            CSS::PropertyID::FontOpticalSizing,
            // FIXME: PropertyID::FontSizeAdjust,
            CSS::PropertyID::FontVariationSettings,
        },
        {
            // Set explicitly
            *font_family,
            computed_font_size,
            computed_font_width,
            computed_font_style,
            computed_font_variant,
            computed_font_weight,
            property_initial_value(CSS::PropertyID::LineHeight), // NB: line-height is forced to normal (i.e. the initial value)

            // Reset implicitly
            property_initial_value(CSS::PropertyID::FontFeatureSettings),   // font-feature-settings
            property_initial_value(CSS::PropertyID::FontKerning),           // font-kerning,
            property_initial_value(CSS::PropertyID::FontLanguageOverride),  // font-language-override
            property_initial_value(CSS::PropertyID::FontOpticalSizing),     // font-optical-sizing,
                                                                            // FIXME: font-size-adjust,
            property_initial_value(CSS::PropertyID::FontVariationSettings), // font-variation-settings
        });

    CSS::FontFeatureData font_feature_data;

    if (keyword_to_font_variant_caps(computed_font_variant->as_shorthand().longhand(CSS::PropertyID::FontVariantCaps)->to_keyword()) == CSS::FontVariantCaps::SmallCaps)
        font_feature_data.font_variant_caps = CSS::FontVariantCaps::SmallCaps;

    // https://drafts.csswg.org/css-font-loading/#font-source
    auto& font_computer = canvas_element.canvas_font_computer();

    drawing_state().font_environment_generation = font_computer.environment_generation();
    drawing_state().current_font_cascade_list = CSS::resolve_font_for_style_values(font_computer,
        {
            .font_families = CSS::computed_font_families_from_style_value(*font_family),
            .font_optical_sizing = CSS::FontOpticalSizing::Auto,
            .font_size = computed_font_size->as_length().length().absolute_length_to_px(),
            .font_slope = computed_font_style->as_font_style().to_font_slope(),
            .font_weight = computed_font_weight->as_number().number(),
            .font_width = computed_font_width->as_percentage().percentage(),
            .font_variation_settings = {},
            .font_feature_data = font_feature_data,
            .font_feature_values_scope = {},
        });
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-letterspacing
template<typename CanvasType>
Utf16String CanvasTextDrawingStyles<CanvasType>::letter_spacing() const
{
    // The letterSpacing getter steps are to return the serialized form of this's letter spacing.
    Utf16StringBuilder builder;
    drawing_state().letter_spacing->serialize(builder, CSS::SerializationMode::Normal);
    return builder.to_string();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-letterspacing
template<typename CanvasType>
void CanvasTextDrawingStyles<CanvasType>::set_letter_spacing(Utf16View letter_spacing)
{
    // 1. Let parsed be the result of parsing the given value as a CSS <length>.
    auto parsed = parse_css_type(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, letter_spacing, CSS::ValueType::Length);

    // 2. If parsed is failure, then return.
    if (!parsed)
        return;

    // 3. Set this's letter spacing to parsed.
    drawing_state().letter_spacing = parsed.release_nonnull();
}

template<typename CanvasType>
float CanvasTextDrawingStyles<CanvasType>::resolved_letter_spacing() const
{
    auto absolutized_length = CSS::Length::from_style_value(drawing_state().letter_spacing->absolutized(computation_context_for_drawing_state()), {});

    return static_cast<float>(absolutized_length.absolute_length_to_px().to_double());
}

template class CanvasTextDrawingStyles<HTMLCanvasElement>;
template class CanvasTextDrawingStyles<HTML::OffscreenCanvas>;

}
