/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Bindings/CanvasRenderingContext2DPrototype.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/HTML/CanvasPattern.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasstate
class CanvasState {
public:
    virtual ~CanvasState() = default;

    virtual Gfx::Painter* painter_for_canvas_state() = 0;
    virtual Gfx::Path& path_for_canvas_state() = 0;

    void save();
    void restore();
    void reset();
    bool is_context_lost();

    using FillOrStrokeVariant = Variant<Gfx::Color, GC::Root<CanvasGradient>, GC::Root<CanvasPattern>>;

    struct FillOrStrokeStyle {
        FillOrStrokeStyle(Gfx::Color color)
            : m_fill_or_stroke_style(color)
        {
        }

        FillOrStrokeStyle(GC::Root<CanvasGradient> gradient)
            : m_fill_or_stroke_style(gradient)
        {
        }

        FillOrStrokeStyle(GC::Root<CanvasPattern> pattern)
            : m_fill_or_stroke_style(pattern)
        {
        }

        NonnullRefPtr<Gfx::PaintStyle> to_gfx_paint_style();

        Optional<Gfx::Color> as_color() const;
        Gfx::Color to_color_but_fixme_should_accept_any_paint_style() const;

        using JsFillOrStrokeStyle = Variant<String, GC::Root<CanvasGradient>, GC::Root<CanvasPattern>>;

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
        Vector<Gfx::Filter> filters;
        Optional<String> filters_string;
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
        RefPtr<CSS::CSSStyleValue> font_style_value { nullptr };
        RefPtr<Gfx::Font const> current_font { nullptr };
        Bindings::CanvasTextAlign text_align { Bindings::CanvasTextAlign::Start };
        Bindings::CanvasTextBaseline text_baseline { Bindings::CanvasTextBaseline::Alphabetic };
    };
    DrawingState& drawing_state() { return m_drawing_state; }
    DrawingState const& drawing_state() const { return m_drawing_state; }

    void clear_drawing_state_stack() { m_drawing_state_stack.clear(); }
    void reset_drawing_state() { m_drawing_state = {}; }

    virtual void reset_to_default_state() = 0;

protected:
    CanvasState() = default;

private:
    DrawingState m_drawing_state;
    Vector<DrawingState> m_drawing_state_stack;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-context-lost
    bool m_context_lost { false };
};

}
