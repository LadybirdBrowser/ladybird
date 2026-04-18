/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CanvasRenderingContext2D.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/HTML/CanvasPattern.h>

namespace Web::HTML {

using FillOrStrokeVariant = Variant<Gfx::Color, GC::Ref<CanvasGradient>, GC::Ref<CanvasPattern>>;

struct FillOrStrokeStyle {
    FillOrStrokeStyle(Gfx::Color color)
        : m_fill_or_stroke_style(color)
    {
    }

    FillOrStrokeStyle(GC::Ref<CanvasGradient> gradient)
        : m_fill_or_stroke_style(gradient)
    {
    }

    FillOrStrokeStyle(GC::Ref<CanvasPattern> pattern)
        : m_fill_or_stroke_style(pattern)
    {
    }

    NonnullRefPtr<Gfx::PaintStyle> to_gfx_paint_style();

    Optional<Gfx::Color> as_color() const;
    Gfx::Color to_color_but_fixme_should_accept_any_paint_style() const;

    using JsFillOrStrokeStyle = Variant<String, GC::Ref<CanvasGradient>, GC::Ref<CanvasPattern>>;

    JsFillOrStrokeStyle to_js_fill_or_stroke_style() const
    {
        return m_fill_or_stroke_style.visit(
            [&](Gfx::Color color) -> JsFillOrStrokeStyle {
                return color.to_string(Gfx::Color::HTMLCompatibleSerialization::Yes);
            },
            [&](auto handle) -> JsFillOrStrokeStyle {
                return handle;
            });
    }

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        visitor.visit(m_fill_or_stroke_style);
    }

private:
    FillOrStrokeVariant m_fill_or_stroke_style;
    RefPtr<Gfx::PaintStyle> m_color_paint_style { nullptr };
};

// https://html.spec.whatwg.org/multipage/canvas.html#drawing-state
struct DrawingState {
    Gfx::AffineTransform transform;
    FillOrStrokeStyle fill_style { Gfx::Color::Black };
    FillOrStrokeStyle stroke_style { Gfx::Color::Black };
    float shadow_offset_x { 0.0f };
    float shadow_offset_y { 0.0f };
    float shadow_blur { 0.0f };
    Gfx::Color shadow_color { Gfx::Color::Transparent };
    Optional<Gfx::Filter> filter;
    Optional<String> filter_string;
    float line_width { 1 };
    Bindings::CanvasLineCap line_cap { Bindings::CanvasLineCap::Butt };
    Bindings::CanvasLineJoin line_join { Bindings::CanvasLineJoin::Miter };
    float miter_limit { 10 };
    Vector<double> dash_list;
    float line_dash_offset { 0 };
    bool image_smoothing_enabled { true };
    Bindings::ImageSmoothingQuality image_smoothing_quality { Bindings::ImageSmoothingQuality::Low };
    float global_alpha = { 1 };
    Gfx::CompositingAndBlendingOperator current_compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::SourceOver;
    RefPtr<CSS::StyleValue const> font_style_value { nullptr };
    RefPtr<Gfx::FontCascadeList const> current_font_cascade_list { nullptr };
    Bindings::CanvasTextAlign text_align { Bindings::CanvasTextAlign::Start };
    Bindings::CanvasTextBaseline text_baseline { Bindings::CanvasTextBaseline::Alphabetic };
    Bindings::CanvasDirection direction { Bindings::CanvasDirection::Inherit };
    NonnullRefPtr<CSS::StyleValue const> letter_spacing { CSS::LengthStyleValue::create(CSS::Length::make_px(0)) };

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        fill_style.visit_edges(visitor);
        stroke_style.visit_edges(visitor);
    }
};

}
