/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

void DisplayList::append(DisplayListCommand&& command, Optional<i32> scroll_frame_id, RefPtr<ClipFrame const> clip_frame)
{
    m_commands.append({ scroll_frame_id, move(clip_frame), move(command) });
}

String DisplayList::dump() const
{
    StringBuilder builder;
    int indentation = 0;
    for (auto const& command_list_item : m_commands) {
        auto const& command = command_list_item.command;

        command.visit([&indentation](auto const& command) {
            if constexpr (requires { command.nesting_level_change; }) {
                if (command.nesting_level_change < 0 && indentation >= -command.nesting_level_change)
                    indentation += command.nesting_level_change;
            }
        });

        if (indentation > 0)
            builder.append(MUST(String::repeated("  "_string, indentation)));
        command.visit([&builder](auto const& cmd) { cmd.dump(builder); });
        builder.append('\n');

        command.visit([&indentation](auto const& command) {
            if constexpr (requires { command.nesting_level_change; }) {
                if (command.nesting_level_change > 0)
                    indentation += command.nesting_level_change;
            }
        });
    }
    return builder.to_string_without_validation();
}

static Optional<Gfx::IntRect> command_bounding_rectangle(DisplayListCommand const& command)
{
    return command.visit(
        [&](auto const& command) -> Optional<Gfx::IntRect> {
            if constexpr (requires { command.bounding_rect(); })
                return command.bounding_rect();
            else
                return {};
        });
}

static bool command_is_clip_or_mask(DisplayListCommand const& command)
{
    return command.visit(
        [&](auto const& command) -> bool {
            if constexpr (requires { command.is_clip_or_mask(); })
                return command.is_clip_or_mask();
            else
                return false;
        });
}

void DisplayListPlayer::execute(DisplayList& display_list, ScrollStateSnapshotByDisplayList&& scroll_state_snapshot_by_display_list, RefPtr<Gfx::PaintingSurface> surface)
{
    TemporaryChange change { m_scroll_state_snapshots_by_display_list, move(scroll_state_snapshot_by_display_list) };
    if (surface) {
        surface->lock_context();
    }
    auto scroll_state_snapshot = m_scroll_state_snapshots_by_display_list.get(display_list).value_or({});
    execute_impl(display_list, scroll_state_snapshot, surface);
    if (surface) {
        surface->unlock_context();
    }
}

void DisplayListPlayer::apply_clip_frame(ClipFrame const& clip_frame, ScrollStateSnapshot const& scroll_state, DevicePixelConverter const& device_pixel_converter)
{
    auto const& clip_rects = clip_frame.clip_rects();
    if (clip_rects.is_empty())
        return;

    save({});
    for (auto const& clip_rect : clip_rects) {
        auto css_rect = clip_rect.rect;
        if (auto enclosing_scroll_frame_id = clip_rect.enclosing_scroll_frame_id; enclosing_scroll_frame_id.has_value()) {
            auto cumulative_offset = scroll_state.cumulative_offset_for_frame_with_id(enclosing_scroll_frame_id.value());
            css_rect.translate_by(cumulative_offset);
        }
        auto device_rect = device_pixel_converter.rounded_device_rect(css_rect).to_type<int>();
        auto corner_radii = clip_rect.corner_radii.as_corners(device_pixel_converter);
        if (corner_radii.has_any_radius()) {
            add_rounded_rect_clip({ .corner_radii = corner_radii, .border_rect = device_rect, .corner_clip = CornerClip::Outside });
        } else {
            add_clip_rect({ .rect = device_rect });
        }
    }
}

void DisplayListPlayer::remove_clip_frame(ClipFrame const& clip_frame)
{
    if (clip_frame.clip_rects().is_empty())
        return;
    restore({});
}

void DisplayListPlayer::execute_impl(DisplayList& display_list, ScrollStateSnapshot const& scroll_state, RefPtr<Gfx::PaintingSurface> surface)
{
    if (surface)
        m_surfaces.append(*surface);
    ScopeGuard guard = [&surfaces = m_surfaces, pop_surface_from_stack = !!surface] {
        if (pop_surface_from_stack)
            (void)surfaces.take_last();
    };

    auto const& commands = display_list.commands();
    auto device_pixels_per_css_pixel = display_list.device_pixels_per_css_pixel();

    DevicePixelConverter device_pixel_converter { device_pixels_per_css_pixel };

    VERIFY(!m_surfaces.is_empty());

    auto translate_command_by_scroll = [&](auto& command, int scroll_frame_id) {
        auto cumulative_offset = scroll_state.cumulative_offset_for_frame_with_id(scroll_frame_id);
        auto scroll_offset = cumulative_offset.to_type<double>().scaled(device_pixels_per_css_pixel).to_type<int>();
        command.visit(
            [scroll_offset](auto& command) {
                if constexpr (requires { command.translate_by(scroll_offset); }) {
                    command.translate_by(scroll_offset);
                }
            });
    };

    auto compute_stacking_context_bounds = [&](PushStackingContext const& push_stacking_context, size_t push_stacking_context_index) {
        Gfx::IntRect bounding_rect;
        display_list.for_each_command_in_range(push_stacking_context_index + 1, push_stacking_context.matching_pop_index, [&](auto command, auto scroll_frame_id) {
            if (scroll_frame_id.has_value())
                translate_command_by_scroll(command, scroll_frame_id.value());
            bounding_rect.unite(*command_bounding_rectangle(command));
            return IterationDecision::Continue;
        });
        return bounding_rect;
    };

    Vector<RefPtr<ClipFrame const>> clip_frames_stack;
    clip_frames_stack.append({});
    for (size_t command_index = 0; command_index < commands.size(); command_index++) {
        auto [scroll_frame_id, clip_frame, command] = commands[command_index];

        if (clip_frames_stack.last() != clip_frame) {
            if (auto clip_frame = clip_frames_stack.take_last()) {
                remove_clip_frame(*clip_frame);
            }
            clip_frames_stack.append(clip_frame);
            if (clip_frame) {
                apply_clip_frame(*clip_frame, scroll_state, device_pixel_converter);
            }
        }

        // After entering a new stacking context, we keep the outer clip frame applied.
        // This is necessary when the stacking context has a CSS transform, and all
        // nested ClipFrames aggregate clip rectangles only up to the stacking context
        // node.
        if (command.has<PushStackingContext>()) {
            clip_frames_stack.append({});
        } else if (command.has<PopStackingContext>()) {
            if (auto clip_frame = clip_frames_stack.take_last()) {
                remove_clip_frame(*clip_frame);
            }
        }

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

        if (scroll_frame_id.has_value())
            translate_command_by_scroll(command, scroll_frame_id.value());

        auto bounding_rect = command_bounding_rectangle(command);

        if (command.has<PushStackingContext>()) {
            auto& push_stacking_context = command.get<PushStackingContext>();
            if (push_stacking_context.can_aggregate_children_bounds && !push_stacking_context.bounding_rect.has_value()) {
                bounding_rect = compute_stacking_context_bounds(push_stacking_context, command_index);
                push_stacking_context.bounding_rect = bounding_rect;
            }
        }

        if (bounding_rect.has_value() && (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect))) {
            // Any clip or mask that's located outside of the visible region is equivalent to a simple clip-rect,
            // so replace it with one to avoid doing unnecessary work.
            if (command_is_clip_or_mask(command)) {
                if (command.has<AddClipRect>()) {
                    add_clip_rect(command.get<AddClipRect>());
                } else {
                    add_clip_rect({ bounding_rect.release_value() });
                }
            }
            if (command.has<PushStackingContext>()) {
                auto pop_stacking_context = command.get<PushStackingContext>().matching_pop_index;
                command_index = pop_stacking_context;
                (void)clip_frames_stack.take_last();
            }
            continue;
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
        else HANDLE_COMMAND(FillPath, fill_path)
        else HANDLE_COMMAND(StrokePath, stroke_path)
        else HANDLE_COMMAND(DrawEllipse, draw_ellipse)
        else HANDLE_COMMAND(FillEllipse, fill_ellipse)
        else HANDLE_COMMAND(DrawLine, draw_line)
        else HANDLE_COMMAND(ApplyBackdropFilter, apply_backdrop_filter)
        else HANDLE_COMMAND(DrawRect, draw_rect)
        else HANDLE_COMMAND(AddRoundedRectClip, add_rounded_rect_clip)
        else HANDLE_COMMAND(AddMask, add_mask)
        else HANDLE_COMMAND(PaintScrollBar, paint_scrollbar)
        else HANDLE_COMMAND(PaintNestedDisplayList, paint_nested_display_list)
        else HANDLE_COMMAND(ApplyOpacity, apply_opacity)
        else HANDLE_COMMAND(ApplyCompositeAndBlendingOperator, apply_composite_and_blending_operator)
        else HANDLE_COMMAND(ApplyFilter, apply_filters)
        else HANDLE_COMMAND(ApplyTransform, apply_transform)
        else HANDLE_COMMAND(ApplyMaskBitmap, apply_mask_bitmap)
        else VERIFY_NOT_REACHED();
        // clang-format on
    }

    while (!clip_frames_stack.is_empty()) {
        if (auto clip_frame = clip_frames_stack.take_last()) {
            remove_clip_frame(*clip_frame);
        }
    }

    if (surface)
        flush();
}

}
