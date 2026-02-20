/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/StringView.h>
#include <LibWeb/HTML/AbstractCanvasRenderingContext2D.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

[[maybe_unused]] static Gfx::Path::CapStyle to_gfx_cap(Bindings::CanvasLineCap const& cap_style)
{
    switch (cap_style) {
    case Bindings::CanvasLineCap::Butt:
        return Gfx::Path::CapStyle::Butt;
    case Bindings::CanvasLineCap::Round:
        return Gfx::Path::CapStyle::Round;
    case Bindings::CanvasLineCap::Square:
        return Gfx::Path::CapStyle::Square;
    }
    VERIFY_NOT_REACHED();
}

[[maybe_unused]] static Gfx::Path::JoinStyle to_gfx_join(Bindings::CanvasLineJoin const& join_style)
{
    switch (join_style) {
    case Bindings::CanvasLineJoin::Round:
        return Gfx::Path::JoinStyle::Round;
    case Bindings::CanvasLineJoin::Bevel:
        return Gfx::Path::JoinStyle::Bevel;
    case Bindings::CanvasLineJoin::Miter:
        return Gfx::Path::JoinStyle::Miter;
    }

    VERIFY_NOT_REACHED();
}

[[maybe_unused]] static Gfx::WindingRule parse_fill_rule(StringView fill_rule)
{
    if (fill_rule == "evenodd"sv)
        return Gfx::WindingRule::EvenOdd;
    if (fill_rule == "nonzero"sv)
        return Gfx::WindingRule::Nonzero;
    dbgln("Unrecognized fillRule for CRC2D.fill() - this problem goes away once we pass an enum instead of a string");
    return Gfx::WindingRule::Nonzero;
}

template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::paint_shadow_for_fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = this->drawing_state();
    if (state.shadow_blur == 0.0f && state.shadow_offset_x == 0.0f && state.shadow_offset_y == 0.0f)
        return;

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    auto alpha = state.global_alpha * (state.shadow_color.alpha() / 255.0f);
    auto fill_style_color = state.fill_style.as_color();
    if (fill_style_color.has_value() && fill_style_color->alpha() > 0)
        alpha = (fill_style_color->alpha() / 255.0f) * state.global_alpha;
    if (alpha == 0.0f)
        return;

    painter->save();

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    transform.multiply(state.transform);
    painter->set_transform(transform);
    painter->fill_path(path, state.shadow_color.with_opacity(alpha), winding_rule, state.shadow_blur, state.current_compositing_and_blending_operator);

    painter->restore();

    did_draw(path.bounding_box());
}

template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::paint_shadow_for_stroke_internal(Gfx::Path const& path, Gfx::Path::CapStyle line_cap, Gfx::Path::JoinStyle line_join, Vector<float> const& dash_array)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = drawing_state();

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    if (state.shadow_blur == 0.0f && state.shadow_offset_x == 0.0f && state.shadow_offset_y == 0.0f)
        return;

    auto alpha = state.global_alpha * (state.shadow_color.alpha() / 255.0f);
    auto fill_style_color = state.fill_style.as_color();
    if (fill_style_color.has_value() && fill_style_color->alpha() > 0)
        alpha = (fill_style_color->alpha() / 255.0f) * state.global_alpha;
    if (alpha == 0.0f)
        return;

    painter->save();

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    transform.multiply(state.transform);
    painter->set_transform(transform);
    painter->stroke_path(path, state.shadow_color.with_opacity(alpha), state.line_width, state.shadow_blur, state.current_compositing_and_blending_operator, line_cap, line_join, state.miter_limit, dash_array, state.line_dash_offset);

    painter->restore();

    did_draw(path.bounding_box());
}

template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::stroke_internal(Gfx::Path const& path)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = drawing_state();
    auto paint_style = state.stroke_style.to_gfx_paint_style();
    if (!paint_style->is_visible())
        return;

    auto line_cap = to_gfx_cap(state.line_cap);
    auto line_join = to_gfx_join(state.line_join);
    // FIXME: Need a Vector<float> for rendering dash_array, but state.dash_list is Vector<double>.
    // Maybe possible to avoid creating copies?
    auto dash_array = Vector<float> {};
    dash_array.ensure_capacity(state.dash_list.size());
    for (auto const& dash : state.dash_list) {
        dash_array.append(static_cast<float>(dash));
    }
    paint_shadow_for_stroke_internal(path, line_cap, line_join, dash_array);
    painter->stroke_path(path, paint_style, state.filter, state.line_width, state.global_alpha, state.current_compositing_and_blending_operator, line_cap, line_join, state.miter_limit, dash_array, state.line_dash_offset);

    did_draw(path.bounding_box());
}

template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = this->drawing_state();
    auto paint_style = state.fill_style.to_gfx_paint_style();
    if (!paint_style->is_visible())
        return;

    paint_shadow_for_fill_internal(path, winding_rule);

    painter->fill_path(path, paint_style, state.filter, state.global_alpha, state.current_compositing_and_blending_operator, winding_rule);

    did_draw(path.bounding_box());
}

template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::clip_internal(Gfx::Path& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    painter->clip(path, winding_rule);
}

template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::set_font(StringView font)
{
    this->CanvasTextDrawingStyles<FinalContext, FinalElement>::set_font(font);
}

template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::set_size(Gfx::IntSize const& size)
{
    if (m_size == size)
        return;
    m_size = size;
    m_surface = nullptr;
    m_painter = nullptr;
}

// https://html.spec.whatwg.org/multipage/canvas.html#reset-the-rendering-context-to-its-default-state
template<typename FinalContext, typename FinalElement>
void AbstractCanvasRenderingContext2D<FinalContext, FinalElement>::reset_to_default_state()
{
    auto surface = this->surface();

    // 1. Clear canvas's bitmap to transparent black.
    if (surface) {
        painter()->clear_rect(surface->rect().template to_type<float>(), clear_color());
    }

    // 2. Empty the list of subpaths in context's current default path.
    path().clear();

    // 3. Clear the context's drawing state stack.
    clear_drawing_state_stack();

    // 4. Reset everything that drawing state consists of to their initial values.
    reset_drawing_state();

    if (surface) {
        painter()->reset();
        did_draw(surface->rect().template to_type<float>());
    }
}

template class AbstractCanvasRenderingContext2D<CanvasRenderingContext2D, HTMLCanvasElement>;
template class AbstractCanvasRenderingContext2D<OffscreenCanvasRenderingContext2D, OffscreenCanvas>;

}
