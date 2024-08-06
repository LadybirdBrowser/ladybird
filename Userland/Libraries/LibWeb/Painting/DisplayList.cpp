/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void DisplayList::apply_scroll_offsets(Vector<Gfx::IntPoint> const& offsets_by_frame_id)
{
    for (auto& command_with_scroll_id : m_commands) {
        if (command_with_scroll_id.scroll_frame_id.has_value()) {
            auto const& scroll_frame_id = command_with_scroll_id.scroll_frame_id.value();
            auto const& scroll_offset = offsets_by_frame_id[scroll_frame_id];
            command_with_scroll_id.command.visit(
                [&](auto& command) {
                    if constexpr (requires { command.translate_by(scroll_offset); })
                        command.translate_by(scroll_offset);
                });
        }
    }
}

void DisplayListPlayer::execute(DisplayList& display_list)
{
    auto const& commands = display_list.commands();

    size_t next_command_index = 0;
    while (next_command_index < commands.size()) {
        auto const& command = commands[next_command_index++].command;
        auto bounding_rect = command_bounding_rectangle(command);
        if (bounding_rect.has_value() && (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect))) {
            continue;
        }

#define HANDLE_COMMAND(command_type, executor_method) \
    if (command.has<command_type>()) {                \
        executor_method(command.get<command_type>()); \
    }

        // clang-format off
        HANDLE_COMMAND(DrawGlyphRun, draw_glyph_run)
        else HANDLE_COMMAND(FillRect, fill_rect)
        else HANDLE_COMMAND(DrawScaledBitmap, draw_scaled_bitmap)
        else HANDLE_COMMAND(DrawScaledImmutableBitmap, draw_scaled_immutable_bitmap)
        else HANDLE_COMMAND(DrawRepeatedImmutableBitmap, draw_repeated_immutable_bitmap)
        else HANDLE_COMMAND(AddClipRect, add_clip_rect)
        else HANDLE_COMMAND(Save, save)
        else HANDLE_COMMAND(Restore, restore)
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
        else VERIFY_NOT_REACHED();
        // clang-format on
    }
}

}
