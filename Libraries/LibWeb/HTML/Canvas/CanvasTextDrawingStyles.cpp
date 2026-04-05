/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2023, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CanvasTextDrawingStyles.h"
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

template<typename IncludingClass, typename CanvasType>
ByteString CanvasTextDrawingStyles<IncludingClass, CanvasType>::font() const
{
    // When font style value is empty return default string
    if (!my_drawing_state().font_style_value) {
        return "10px sans-serif";
    }

    // On getting, the font attribute must return the serialized form of the current font of the context (with no 'line-height' component).
    return my_drawing_state().font_style_value->to_string(CSS::SerializationMode::ResolvedValue).to_byte_string();
}

// https://html.spec.whatwg.org/multipage/canvas.html#font-style-source-object
template<typename IncludingClass, typename CanvasType>
Variant<DOM::Document*, HTML::WorkerGlobalScope*> CanvasTextDrawingStyles<IncludingClass, CanvasType>::get_font_source_for_font_style_source_object(CanvasType& font_style_source_object)
{
    // Font resolution for the font style source object requires a font source. This is determined for a given object implementing CanvasTextDrawingStyles by the following steps: [CSSFONTLOAD]

    if constexpr (SameAs<CanvasType, HTML::HTMLCanvasElement>) {
        // 1. If object's font style source object is a canvas element, return the element's node document.
        return &font_style_source_object.document();
    } else {
        // 2. Otherwise, object's font style source object is an OffscreenCanvas object:

        // 1. Let global be object's relevant global object.
        auto& global_object = HTML::relevant_global_object(font_style_source_object);

        // 2. If global is a Window object, then return global's associated Document.
        if (is<HTML::Window>(global_object)) {
            auto& window = as<HTML::Window>(global_object);
            return &(window.associated_document());
        }

        // 3. Assert: global implements WorkerGlobalScope.
        VERIFY(is<HTML::WorkerGlobalScope>(global_object));

        // 4. Return global.
        return &(as<HTML::WorkerGlobalScope>(global_object));
    };
}
template<typename IncludingClass, typename CanvasType>
void CanvasTextDrawingStyles<IncludingClass, CanvasType>::set_font(StringView font)
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
    auto& canvas_element = static_cast<IncludingClass&>(*this).canvas_element();

    auto computed_math_depth = CSS::InitialValues::math_depth();

    // FIXME: We will need to absolutize this once we support ident() functions
    auto& font_family = *font_style_value.longhand(CSS::PropertyID::FontFamily);

    Optional<DOM::AbstractElement> inheritance_parent;

    if constexpr (SameAs<CanvasType, HTML::HTMLCanvasElement>) {
        canvas_element.document().update_style_if_needed_for_element(DOM::AbstractElement { canvas_element });

        if (canvas_element.navigable() && canvas_element.is_connected()) {
            VERIFY(canvas_element.computed_properties());

            // NOTE: Since we can't set a math depth directly here we always use the inherited value for the computed value
            computed_math_depth = canvas_element.computed_properties()->math_depth();

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

    my_drawing_state().font_style_value = CSS::ShorthandStyleValue::create(
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
            font_family,
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

    // https://drafts.csswg.org/css-font-loading/#font-source
    auto font_source = get_font_source_for_font_style_source_object(canvas_element);

    CSS::FontFeatureData font_feature_data;

    if (keyword_to_font_variant_caps(computed_font_variant->as_shorthand().longhand(CSS::PropertyID::FontVariantCaps)->to_keyword()) == CSS::FontVariantCaps::SmallCaps)
        font_feature_data.font_variant_caps = CSS::FontVariantCaps::SmallCaps;

    auto font_list = font_source.visit(
        [&](DOM::Document* document) -> RefPtr<Gfx::FontCascadeList const> {
            return document->font_computer().compute_font_for_style_values(
                font_family,
                computed_font_size->as_length().length().absolute_length_to_px(),
                computed_font_style->as_font_style().to_font_slope(),
                computed_font_weight->as_number().number(),
                computed_font_width->as_percentage().percentage(),
                CSS::FontOpticalSizing::Auto,
                {},
                font_feature_data);
        },
        [](HTML::WorkerGlobalScope*) -> RefPtr<Gfx::FontCascadeList const> {
            // FIXME: implement computing the font for HTML::WorkerGlobalScope
            return {};
        });

    if (!font_list)
        return;

    my_drawing_state().current_font_cascade_list = font_list;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-letterspacing
template<typename IncludingClass, typename CanvasType>
String CanvasTextDrawingStyles<IncludingClass, CanvasType>::letter_spacing() const
{
    // The letterSpacing getter steps are to return the serialized form of this's letter spacing.
    StringBuilder builder;
    my_drawing_state().letter_spacing.serialize(builder);
    return MUST(builder.to_string());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-letterspacing
template<typename IncludingClass, typename CanvasType>
void CanvasTextDrawingStyles<IncludingClass, CanvasType>::set_letter_spacing(StringView letter_spacing)
{
    // 1. Let parsed be the result of parsing the given value as a CSS <length>.
    auto parsed = parse_css_type(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, letter_spacing, CSS::ValueType::Length);

    // 2. If parsed is failure, then return.
    if (!parsed || !parsed->is_length())
        return;

    // 3. Set this's letter spacing to parsed.
    my_drawing_state().letter_spacing = parsed->as_length().length();
}

template class CanvasTextDrawingStyles<CanvasRenderingContext2D, HTMLCanvasElement>;
template class CanvasTextDrawingStyles<OffscreenCanvasRenderingContext2D, HTML::OffscreenCanvas>;

}
