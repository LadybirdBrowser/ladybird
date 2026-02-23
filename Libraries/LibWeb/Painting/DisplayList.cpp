/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>

namespace Web::Painting {

void DisplayList::append(DisplayListCommand&& command, RefPtr<AccumulatedVisualContext const> context)
{
    m_commands.append({ move(context), move(command) });
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

static bool command_is_clip(DisplayListCommand const& command)
{
    return command.visit(
        [&](auto const& command) -> bool {
            if constexpr (requires { command.is_clip(); })
                return command.is_clip();
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
    m_surface = surface;
    auto scroll_state_snapshot = m_scroll_state_snapshots_by_display_list.get(display_list).value_or({});
    execute_impl(display_list, scroll_state_snapshot);
    if (surface)
        flush();
    m_surface = nullptr;
    if (surface) {
        surface->unlock_context();
    }
}

static RefPtr<AccumulatedVisualContext const> find_common_ancestor(RefPtr<AccumulatedVisualContext const> a, RefPtr<AccumulatedVisualContext const> b)
{
    if (!a || !b)
        return {};

    while (a->depth() > b->depth())
        a = a->parent();
    while (b->depth() > a->depth())
        b = b->parent();

    while (a != b) {
        a = a->parent();
        b = b->parent();
    }
    return a;
}

// Converts a CSS-pixel-space 4x4 matrix to device-pixel-space.
// - Translation column (column 3, rows 0-2) is scaled up by DPR
// - Perspective row (row 3, columns 0-2) is scaled down by DPR
// - All other elements are unaffected (the scale factors cancel out)
static FloatMatrix4x4 scale_matrix_for_device_pixels(FloatMatrix4x4 matrix, float scale)
{
    matrix[0, 3] *= scale;
    matrix[1, 3] *= scale;
    matrix[2, 3] *= scale;
    matrix[3, 0] /= scale;
    matrix[3, 1] /= scale;
    matrix[3, 2] /= scale;
    return matrix;
}

void DisplayListPlayer::execute_impl(DisplayList& display_list, ScrollStateSnapshot const& scroll_state)
{
    auto const& commands = display_list.commands();
    auto device_pixels_per_css_pixel = display_list.device_pixels_per_css_pixel();

    DevicePixelConverter device_pixel_converter { device_pixels_per_css_pixel };

    VERIFY(m_surface);

    auto for_each_node_from_common_ancestor_to_target = [](this auto const& self, RefPtr<AccumulatedVisualContext const> common_ancestor, RefPtr<AccumulatedVisualContext const> node, auto&& callback) -> void {
        if (!node || node == common_ancestor)
            return;
        self(common_ancestor, node->parent(), callback);
        callback(*node);
    };

    auto apply_accumulated_visual_context = [&](AccumulatedVisualContext const& node) {
        node.data().visit(
            [&](EffectsData const& effects) {
                Optional<Gfx::Filter> gfx_filter;
                if (effects.filter.has_filters())
                    gfx_filter = to_gfx_filter(effects.filter, device_pixels_per_css_pixel);
                apply_effects({ .opacity = effects.opacity, .compositing_and_blending_operator = effects.blend_mode, .filter = gfx_filter });
            },
            [&](PerspectiveData const& perspective) {
                save({});
                auto matrix = scale_matrix_for_device_pixels(perspective.matrix, static_cast<float>(device_pixels_per_css_pixel));
                apply_transform({ 0, 0 }, matrix);
            },
            [&](ScrollData const& scroll) {
                save({});
                auto own_offset = scroll_state.own_offset_for_frame_with_id(scroll.scroll_frame_id);
                if (!own_offset.is_zero()) {
                    auto scroll_offset = own_offset.to_type<double>().scaled(device_pixels_per_css_pixel).to_type<int>();
                    translate({ .delta = scroll_offset });
                }
            },
            [&](TransformData const& transform) {
                save({});
                auto origin = transform.origin.to_type<double>().scaled(device_pixels_per_css_pixel).to_type<float>();
                auto matrix = scale_matrix_for_device_pixels(transform.matrix, static_cast<float>(device_pixels_per_css_pixel));
                apply_transform(origin, matrix);
            },
            [&](ClipData const& clip) {
                save({});
                auto device_rect = device_pixel_converter.rounded_device_rect(clip.rect).to_type<int>();
                auto corner_radii = clip.corner_radii.as_corners(device_pixel_converter);
                if (corner_radii.has_any_radius())
                    add_rounded_rect_clip({ .corner_radii = corner_radii, .border_rect = device_rect, .corner_clip = CornerClip::Outside });
                else
                    add_clip_rect({ .rect = device_rect });
            },
            [&](ClipPathData const& clip_path) {
                save({});
                auto transformed_path = clip_path.path.copy_transformed(Gfx::AffineTransform {}.set_scale(static_cast<float>(device_pixels_per_css_pixel), static_cast<float>(device_pixels_per_css_pixel)));
                add_clip_path(transformed_path);
            });
    };

    RefPtr<AccumulatedVisualContext const> applied_context;
    size_t applied_depth = 0;

    auto switch_to_context = [&](RefPtr<AccumulatedVisualContext const> const& target_context) {
        if (applied_context == target_context)
            return;

        auto common_ancestor = find_common_ancestor(applied_context, target_context);
        auto common_ancestor_depth = common_ancestor ? common_ancestor->depth() : 0;

        while (applied_depth > common_ancestor_depth) {
            restore({});
            applied_depth--;
        }

        for_each_node_from_common_ancestor_to_target(common_ancestor, target_context, [&](AccumulatedVisualContext const& node) {
            apply_accumulated_visual_context(node);
            applied_depth++;
        });

        applied_context = target_context;
    };

    for (size_t command_index = 0; command_index < commands.size(); command_index++) {
        auto const& [context, command] = commands[command_index];

        auto bounding_rect = command_bounding_rectangle(command);

        // OPTIMIZATION: If the leaf context is an effect and we're switching to a new context,
        //               check culling before applying it. Effects (opacity, filters, blend modes) don't affect
        //               clip state, so would_be_fully_clipped_by_painter() returns the same result before and after
        //               applying effects.
        //               This avoids expensive saveLayer/restore cycles for off-screen elements with effects like blur.
        // NOTE: We must not do this for consecutive commands with the same context, as that would incorrectly restore
        //       and re-apply the effect layer, breaking blend mode compositing.
        if (context && applied_context != context && context->is_effect() && bounding_rect.has_value()) {
            switch_to_context(context->parent());
            if (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect))
                continue;
        }

        switch_to_context(context);

        if (command.has<PaintScrollBar>()) {
            auto translated_command = command;
            auto& paint_scroll_bar = translated_command.get<PaintScrollBar>();
            auto scroll_offset = scroll_state.own_offset_for_frame_with_id(paint_scroll_bar.scroll_frame_id);
            if (paint_scroll_bar.vertical) {
                auto offset = scroll_offset.y() * paint_scroll_bar.scroll_size;
                paint_scroll_bar.thumb_rect.translate_by(0, -offset.to_int() * device_pixels_per_css_pixel);
            } else {
                auto offset = scroll_offset.x() * paint_scroll_bar.scroll_size;
                paint_scroll_bar.thumb_rect.translate_by(-offset.to_int() * device_pixels_per_css_pixel, 0);
            }
            paint_scrollbar(paint_scroll_bar);
            continue;
        }

        if (bounding_rect.has_value() && (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect))) {
            // Any clip that's located outside of the visible region is equivalent to a simple clip-rect,
            // so replace it with one to avoid doing unnecessary work.
            if (command_is_clip(command)) {
                if (command.has<AddClipRect>()) {
                    add_clip_rect(command.get<AddClipRect>());
                } else {
                    add_clip_rect({ bounding_rect.release_value() });
                }
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
        else HANDLE_COMMAND(DrawScaledImmutableBitmap, draw_scaled_immutable_bitmap)
        else HANDLE_COMMAND(DrawRepeatedImmutableBitmap, draw_repeated_immutable_bitmap)
        else HANDLE_COMMAND(DrawExternalContent, draw_external_content)
        else HANDLE_COMMAND(AddClipRect, add_clip_rect)
        else HANDLE_COMMAND(Save, save)
        else HANDLE_COMMAND(SaveLayer, save_layer)
        else HANDLE_COMMAND(Restore, restore)
        else HANDLE_COMMAND(Translate, translate)
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
        else HANDLE_COMMAND(PaintNestedDisplayList, paint_nested_display_list)
        else HANDLE_COMMAND(ApplyEffects, apply_effects)
        else VERIFY_NOT_REACHED();
        // clang-format on
    }

    while (applied_depth > 0) {
        restore({});
        applied_depth--;
    }

    flush();
}

}
