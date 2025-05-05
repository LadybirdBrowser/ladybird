/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

void DisplayList::append(Command&& command, Optional<i32> scroll_frame_id)
{
    m_commands.append({ scroll_frame_id, move(command) });
}

static Optional<Gfx::IntRect> command_bounding_rectangle(Command const& command)
{
    return command.visit(
        [&](auto const& command) -> Optional<Gfx::IntRect> {
            if constexpr (requires { command.bounding_rect(); })
                return command.bounding_rect();
            else
                return {};
        });
}

static bool command_is_clip_or_mask(Command const& command)
{
    return command.visit(
        [&](auto const& command) -> bool {
            if constexpr (requires { command.is_clip_or_mask(); })
                return command.is_clip_or_mask();
            else
                return false;
        });
}

static bool command_performs_save(Command const& command)
{
    return command.visit(
        [&](auto const& command) -> bool {
            if constexpr (requires { command.performs_save(); })
                return command.performs_save();
            else
                return false;
        });
}

void DisplayListPlayer::execute(DisplayList& display_list, ScrollStateSnapshot const& scroll_state, RefPtr<Gfx::PaintingSurface> surface)
{
    if (surface) {
        surface->lock_context();
    }
    m_containment_stack.clear();
    if (surface)
        m_containment_stack.append({ surface->rect(), 0 });
    execute_impl(display_list, scroll_state, surface, {});
    if (surface) {
        surface->unlock_context();
    }
}

void DisplayListPlayer::execute_impl(DisplayList& display_list, ScrollStateSnapshot const& scroll_state, RefPtr<Gfx::PaintingSurface> surface, Gfx::Point<int> surface_offset)
{
    if (surface)
        m_surfaces.append(*surface);
    ScopeGuard guard = [&surfaces = m_surfaces, pop_surface_from_stack = !!surface] {
        if (pop_surface_from_stack)
            (void)surfaces.take_last();
    };

    auto const& commands = display_list.commands();
    auto device_pixels_per_css_pixel = display_list.device_pixels_per_css_pixel();

    VERIFY(!m_surfaces.is_empty());

    m_containment_stack.append({ m_containment_stack.last() });
    m_containment_stack.last().visible_area.translate_by(-surface_offset);

    for (size_t command_index = 0; command_index < commands.size(); command_index++) {
        auto scroll_frame_id = commands[command_index].scroll_frame_id;
        auto command = commands[command_index].command;

        if (command.has<PaintScrollBar>()) {
            auto& paint_scroll_bar = command.get<PaintScrollBar>();
            auto scroll_offset = scroll_state.own_offset_for_frame_with_id(paint_scroll_bar.scroll_frame_id);
            if (paint_scroll_bar.vertical) {
                auto offset = scroll_offset.y() * paint_scroll_bar.scroll_size;
                paint_scroll_bar.thumb_rect.translate_by(0, -offset.to_int() * device_pixels_per_css_pixel);
            } else {
                auto offset = scroll_offset.x() * paint_scroll_bar.scroll_size;
                paint_scroll_bar.thumb_rect.translate_by(-offset.to_int() * device_pixels_per_css_pixel, 0);
            }
        }

        if (scroll_frame_id.has_value()) {
            auto cumulative_offset = scroll_state.cumulative_offset_for_frame_with_id(scroll_frame_id.value());
            auto scroll_offset = cumulative_offset.to_type<double>().scaled(device_pixels_per_css_pixel).to_type<int>();
            command.visit(
                [&](auto& command) {
                    if constexpr (requires { command.translate_by(scroll_offset); }) {
                        command.translate_by(scroll_offset);
                    }
                });
        }

        auto bounding_rect = command_bounding_rectangle(command);
        if (command_is_clip_or_mask(command)) {
            if (bounding_rect.has_value()) {
                // Non-local effects prevent us from just intersecting with the previous visible area because we are
                // potentially discarding parts of the clipped region that the non-local effect could bring into view.
                // In this case, we scope to the clipped region and render all of it as if there were no non-local effects,
                // since things inside the clipped element, but beyond the clipped area will not be affected by the
                // existing non-local effect. (It will only affect the element in its entirety once drawn)
                if (m_containment_stack.last().non_local_effects > 0) {
                    m_containment_stack.last().visible_area = { *bounding_rect };
                    m_containment_stack.last().non_local_effects = 0;
                } else {
                    m_containment_stack.last().visible_area.intersect(*bounding_rect);
                }

                if (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect)) {
                    // Any clip or mask that's located outside of the drawn region is equivalent to a simple clip-rect,
                    // so replace it with one to avoid doing unnecessary work.
                    if (command.has<AddClipRect>()) {
                        add_clip_rect(command.get<AddClipRect>());
                    } else {
                        add_clip_rect({ bounding_rect.release_value() });
                    }
                    continue;
                }
            }
        } else if (command.has<Translate>()) {
            m_containment_stack.last().visible_area.translate_by(-command.get<Translate>().delta);
        } else if (command.has<ApplyTransform>()) {
            auto affine_transform = Gfx::extract_2d_affine_transform(command.get<ApplyTransform>().matrix);
            auto final_transform = Gfx::AffineTransform {}
                                       .translate(command.get<ApplyTransform>().origin)
                                       .multiply(affine_transform)
                                       .translate(-command.get<ApplyTransform>().origin);
            m_containment_stack.last().visible_area = final_transform.map(m_containment_stack.last().visible_area);
        } else if (command.has<Restore>()) {
            m_containment_stack.take_last();
        } else if (command_performs_save(command)) {
            m_containment_stack.append({ m_containment_stack.last() });
        } else if (command.has<StartNonLocalEffect>()) {
            m_containment_stack.last().non_local_effects++;
            continue;
        } else if (command.has<EndNonLocalEffect>()) {
            m_containment_stack.last().non_local_effects--;
            continue;
        } else {
            if (bounding_rect.has_value() && !bounding_rect.value().intersects(m_containment_stack.last().visible_area)) {
                continue;
            }
        }

#define HANDLE_COMMAND(command_type, executor_method) \
    if (command.has<command_type>()) {                \
        executor_method(command.get<command_type>()); \
    }

        // clang-format off
        HANDLE_COMMAND(DrawGlyphRun, draw_glyph_run)
        else HANDLE_COMMAND(FillRect, fill_rect)
        else HANDLE_COMMAND(DrawPaintingSurface, draw_painting_surface)
        else HANDLE_COMMAND(DrawScaledImmutableBitmap, draw_scaled_immutable_bitmap)
        else HANDLE_COMMAND(DrawRepeatedImmutableBitmap, draw_repeated_immutable_bitmap)
        else HANDLE_COMMAND(AddClipRect, add_clip_rect)
        else HANDLE_COMMAND(Save, save)
        else HANDLE_COMMAND(SaveLayer, save_layer)
        else HANDLE_COMMAND(Restore, restore)
        else HANDLE_COMMAND(Translate, translate)
        else HANDLE_COMMAND(PushStackingContext, push_stacking_context)
        else HANDLE_COMMAND(PopStackingContext, pop_stacking_context)
        else HANDLE_COMMAND(PaintLinearGradient, paint_linear_gradient)
        else HANDLE_COMMAND(PaintRadialGradient, paint_radial_gradient)
        else HANDLE_COMMAND(PaintConicGradient, paint_conic_gradient)
        else HANDLE_COMMAND(PaintOuterBoxShadow, paint_outer_box_shadow)
        else HANDLE_COMMAND(PaintInnerBoxShadow, paint_inner_box_shadow)
        else HANDLE_COMMAND(PaintTextShadow, paint_text_shadow)
        else HANDLE_COMMAND(FillRectWithRoundedCorners, fill_rect_with_rounded_corners)
        else HANDLE_COMMAND(FillPathUsingColor, fill_path_using_color)
        else HANDLE_COMMAND(FillPathUsingPaintStyle, fill_path_using_paint_style)
        else HANDLE_COMMAND(StrokePathUsingColor, stroke_path_using_color)
        else HANDLE_COMMAND(StrokePathUsingPaintStyle, stroke_path_using_paint_style)
        else HANDLE_COMMAND(DrawEllipse, draw_ellipse)
        else HANDLE_COMMAND(FillEllipse, fill_ellipse)
        else HANDLE_COMMAND(DrawLine, draw_line)
        else HANDLE_COMMAND(ApplyBackdropFilter, apply_backdrop_filter)
        else HANDLE_COMMAND(DrawRect, draw_rect)
        else HANDLE_COMMAND(DrawTriangleWave, draw_triangle_wave)
        else HANDLE_COMMAND(AddRoundedRectClip, add_rounded_rect_clip)
        else HANDLE_COMMAND(AddMask, add_mask)
        else HANDLE_COMMAND(PaintScrollBar, paint_scrollbar)
        else HANDLE_COMMAND(PaintNestedDisplayList, paint_nested_display_list)
        else HANDLE_COMMAND(ApplyOpacity, apply_opacity)
        else HANDLE_COMMAND(ApplyCompositeAndBlendingOperator, apply_composite_and_blending_operator)
        else HANDLE_COMMAND(ApplyFilters, apply_filters)
        else HANDLE_COMMAND(ApplyTransform, apply_transform)
        else HANDLE_COMMAND(ApplyMaskBitmap, apply_mask_bitmap)
        else VERIFY_NOT_REACHED();
        // clang-format on

        if (command.has<PaintNestedDisplayList>()) {
            m_containment_stack.last().visible_area.translate_by(command.get<PaintNestedDisplayList>().rect.location());
        }
    }

    // This also takes care of un-translating the visible area back to where it
    // was before the call to this function
    m_containment_stack.take_last();

    if (surface)
        flush();
}

}
