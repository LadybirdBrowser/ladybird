/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvastextdrawingstyles
template<typename IncludingClass, typename CanvasType>
class CanvasTextDrawingStyles {
public:
    ~CanvasTextDrawingStyles() = default;

    ByteString font() const
    {
        // When font style value is empty return default string
        if (!my_drawing_state().font_style_value) {
            return "10px sans-serif";
        }

        // On getting, the font attribute must return the serialized form of the current font of the context (with no 'line-height' component).
        auto const& font_style_value = my_drawing_state().font_style_value->as_shorthand();
        auto font_style = font_style_value.longhand(CSS::PropertyID::FontStyle);
        auto font_weight = font_style_value.longhand(CSS::PropertyID::FontWeight);
        auto font_size = font_style_value.longhand(CSS::PropertyID::FontSize);
        auto font_family = font_style_value.longhand(CSS::PropertyID::FontFamily);
        return ByteString::formatted("{} {} {} {}",
            font_style->to_string(CSS::SerializationMode::Normal),
            font_weight->to_string(CSS::SerializationMode::Normal),
            font_size->to_string(CSS::SerializationMode::Normal),
            font_family->to_string(CSS::SerializationMode::Normal));
    }

    // https://html.spec.whatwg.org/multipage/canvas.html#font-style-source-object
    Variant<DOM::Document*, HTML::WorkerGlobalScope*> get_font_source_for_font_style_source_object(CanvasType& font_style_source_object)
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

    void set_font(StringView font)
    {
        // The font IDL attribute, on setting, must be parsed as a CSS <'font'> value (but without supporting property-independent style sheet syntax like 'inherit'),
        // and the resulting font must be assigned to the context, with the 'line-height' component forced to 'normal', with the 'font-size' component converted to CSS pixels,
        // and with system fonts being computed to explicit values.
        // FIXME: with the 'line-height' component forced to 'normal'
        // FIXME: with the 'font-size' component converted to CSS pixels
        auto font_style_value_result = parse_css_value(CSS::Parser::ParsingParams {}, font, CSS::PropertyID::Font);

        // If the new value is syntactically incorrect (including using property-independent style sheet syntax like 'inherit' or 'initial'), then it must be ignored, without assigning a new font value.
        // NOTE: ShorthandStyleValue should be the only valid option here. We implicitly VERIFY this below.
        if (!font_style_value_result || !font_style_value_result->is_shorthand()) {
            return;
        }
        my_drawing_state().font_style_value = font_style_value_result.release_nonnull();

        // Load font with font style value properties
        auto const& font_style_value = my_drawing_state().font_style_value->as_shorthand();
        auto& canvas_element = reinterpret_cast<IncludingClass&>(*this).canvas_element();

        auto& font_style = *font_style_value.longhand(CSS::PropertyID::FontStyle);
        auto& font_weight = *font_style_value.longhand(CSS::PropertyID::FontWeight);
        auto& font_width = *font_style_value.longhand(CSS::PropertyID::FontWidth);
        auto& font_size = *font_style_value.longhand(CSS::PropertyID::FontSize);
        auto& font_family = *font_style_value.longhand(CSS::PropertyID::FontFamily);

        // https://drafts.csswg.org/css-font-loading/#font-source
        auto font_source = get_font_source_for_font_style_source_object(canvas_element);

        auto font_list = font_source.visit(
            [&](DOM::Document* document) -> RefPtr<Gfx::FontCascadeList const> {
                auto computed_math_depth = CSS::InitialValues::math_depth();
                auto inherited_math_depth = CSS::InitialValues::math_depth();

                // NOTE: The initial value here is non-standard as the default font is "10px sans-serif"
                auto inherited_font_size = CSSPixels { 10 };
                auto inherited_font_weight = CSS::InitialValues::font_weight();
                auto length_resolution_context = CSS::Length::ResolutionContext::for_window(*document->window());

                if constexpr (SameAs<CanvasType, HTML::HTMLCanvasElement>) {
                    // NOTE: The canvas itself is considered the inheritance parent
                    if (canvas_element.computed_properties()) {
                        // NOTE: Since we can't set a math depth directly here we always use the inherited value for the computed value
                        computed_math_depth = canvas_element.computed_properties()->math_depth();
                        inherited_math_depth = canvas_element.computed_properties()->math_depth();
                        inherited_font_size = canvas_element.computed_properties()->font_size();
                        inherited_font_weight = canvas_element.computed_properties()->font_weight();
                        length_resolution_context = CSS::Length::ResolutionContext::for_element(DOM::AbstractElement { canvas_element });
                    }
                }

                auto const& computed_font_size = CSS::StyleComputer::compute_font_size(font_size, computed_math_depth, inherited_font_size, inherited_math_depth, length_resolution_context);
                auto const& computed_font_weight = CSS::StyleComputer::compute_font_weight(font_weight, inherited_font_weight, length_resolution_context);

                return document->style_computer().compute_font_for_style_values(
                    font_family,
                    computed_font_size->as_length().length().absolute_length_to_px(),
                    font_style,
                    computed_font_weight->as_number().number(),
                    font_width);
            },
            [](HTML::WorkerGlobalScope*) -> RefPtr<Gfx::FontCascadeList const> {
                // FIXME: implement computing the font for HTML::WorkerGlobalScope
                return {};
            });

        if (!font_list)
            return;

        my_drawing_state().current_font_cascade_list = font_list;
    }

    Bindings::CanvasTextAlign text_align() const { return my_drawing_state().text_align; }
    void set_text_align(Bindings::CanvasTextAlign text_align) { my_drawing_state().text_align = text_align; }

    Bindings::CanvasTextBaseline text_baseline() const { return my_drawing_state().text_baseline; }
    void set_text_baseline(Bindings::CanvasTextBaseline text_baseline) { my_drawing_state().text_baseline = text_baseline; }

protected:
    CanvasTextDrawingStyles() = default;

private:
    CanvasState::DrawingState& my_drawing_state() { return reinterpret_cast<IncludingClass&>(*this).drawing_state(); }
    CanvasState::DrawingState const& my_drawing_state() const { return reinterpret_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
