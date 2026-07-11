/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Rect.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListDamage.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

struct DisplayListCommandReference {
    DisplayListCommandHeader header;
    ReadonlyBytes payload;
};

static ReadonlyBytes display_list_data_span_bytes(ReadonlyBytes payload, DisplayListDataSpan span)
{
    VERIFY(span.offset <= payload.size());
    VERIFY(span.size <= payload.size() - span.offset);
    return payload.slice(span.offset, span.size);
}

static Vector<DisplayListCommandReference> collect_display_list_command_references(ReadonlyBytes command_bytes)
{
    Vector<DisplayListCommandReference> commands;
    DisplayList::for_each_command_header(command_bytes, [&](auto const& header, auto payload) {
        commands.append({ header, payload });
    });
    return commands;
}

static bool display_list_commands_are_equal(DisplayListCommandReference const& a, DisplayListCommandReference const& b)
{
    if (a.header.type != b.header.type
        || a.header.has_bounding_rect != b.header.has_bounding_rect
        || a.header.is_clip != b.header.is_clip
        || a.header.bounding_rect != b.header.bounding_rect)
        return false;

    if (a.header.type == DisplayListCommandType::DrawScaledDecodedImageFrame) {
        auto first = read_display_list_object<DrawScaledDecodedImageFrame>(a.payload);
        auto second = read_display_list_object<DrawScaledDecodedImageFrame>(b.payload);
        return first.dst_rect == second.dst_rect
            && first.src_rect == second.src_rect
            && first.frame_id == second.frame_id
            && first.scaling_mode == second.scaling_mode
            && first.compositing_and_blending_operator == second.compositing_and_blending_operator
            && first.isolated_backdrop_color == second.isolated_backdrop_color;
    }

    if (a.header.type == DisplayListCommandType::DrawGlyphRun) {
        auto first = read_display_list_object<DrawGlyphRun>(a.payload);
        auto second = read_display_list_object<DrawGlyphRun>(b.payload);
        return first.font_id == second.font_id
            && first.glyphs.size == second.glyphs.size
            && display_list_data_span_bytes(a.payload, first.glyphs) == display_list_data_span_bytes(b.payload, second.glyphs)
            && first.rect == second.rect
            && first.glyph_bounding_rect == second.glyph_bounding_rect
            && first.translation == second.translation
            && first.scale == second.scale
            && first.color == second.color
            && first.orientation == second.orientation;
    }

    if (a.header.type == DisplayListCommandType::PaintTextShadow) {
        auto first = read_display_list_object<PaintTextShadow>(a.payload);
        auto second = read_display_list_object<PaintTextShadow>(b.payload);
        return first.font_id == second.font_id
            && first.glyphs.size == second.glyphs.size
            && display_list_data_span_bytes(a.payload, first.glyphs) == display_list_data_span_bytes(b.payload, second.glyphs)
            && first.shadow_bounding_rect == second.shadow_bounding_rect
            && first.text_rect == second.text_rect
            && first.draw_location == second.draw_location
            && first.scale == second.scale
            && first.blur_radius == second.blur_radius
            && first.color == second.color;
    }

    return a.header.payload_size == b.header.payload_size && a.payload == b.payload;
}

static bool corner_radii_are_equal(Gfx::CornerRadii const& a, Gfx::CornerRadii const& b)
{
    auto corner_radius_is_equal = [](auto const& first, auto const& second) {
        return first.horizontal_radius == second.horizontal_radius
            && first.vertical_radius == second.vertical_radius;
    };
    return corner_radius_is_equal(a.top_left, b.top_left)
        && corner_radius_is_equal(a.top_right, b.top_right)
        && corner_radius_is_equal(a.bottom_right, b.bottom_right)
        && corner_radius_is_equal(a.bottom_left, b.bottom_left);
}

static bool matrices_are_equal(Gfx::FloatMatrix4x4 const& a, Gfx::FloatMatrix4x4 const& b)
{
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            if (a[row, column] != b[row, column])
                return false;
        }
    }
    return true;
}

static bool visual_context_data_is_equal(VisualContextData const& a, VisualContextData const& b)
{
    return a.visit(
        [&](ScrollData const& data) {
            auto const* other = b.get_pointer<ScrollData>();
            return other && data.scroll_frame_index == other->scroll_frame_index && data.is_sticky == other->is_sticky;
        },
        [&](ClipData const& data) {
            auto const* other = b.get_pointer<ClipData>();
            return other && data.rect == other->rect && corner_radii_are_equal(data.corner_radii, other->corner_radii);
        },
        [&](TransformData const& data) {
            auto const* other = b.get_pointer<TransformData>();
            return other && matrices_are_equal(data.matrix, other->matrix) && data.origin == other->origin;
        },
        [&](PerspectiveData const& data) {
            auto const* other = b.get_pointer<PerspectiveData>();
            return other && matrices_are_equal(data.matrix, other->matrix);
        },
        [&](ClipPathData const& data) {
            auto const* other = b.get_pointer<ClipPathData>();
            return other
                && data.bounding_rect == other->bounding_rect
                && data.fill_rule == other->fill_rule
                && data.path.serialize_to_bytes() == other->path.serialize_to_bytes();
        },
        [&](EffectsData const& data) {
            auto const* other = b.get_pointer<EffectsData>();
            if (!other || data.gfx_filter.has_value() != other->gfx_filter.has_value())
                return false;
            if (data.gfx_filter.has_value()) {
                Function<u64(Gfx::DecodedImageFrame const&)> encode_image = [](Gfx::DecodedImageFrame const& image) -> u64 {
                    return reinterpret_cast<FlatPtr>(&image);
                };
                if (Gfx::serialize_filter(*data.gfx_filter, encode_image) != Gfx::serialize_filter(*other->gfx_filter, encode_image))
                    return false;
            }
            return other
                && data.opacity == other->opacity
                && data.blend_mode == other->blend_mode;
        },
        [&](ScrollCompensation const& data) {
            auto const* other = b.get_pointer<ScrollCompensation>();
            return other && data.scroll_frame_index == other->scroll_frame_index;
        },
        [&](AnchorScrollShift const& data) {
            auto const* other = b.get_pointer<AnchorScrollShift>();
            return other
                && data.scroll_frame_index == other->scroll_frame_index
                && data.negate == other->negate
                && data.compensate_x == other->compensate_x
                && data.compensate_y == other->compensate_y;
        });
}

static bool visual_context_chains_are_compatible(VisualContextIndex old_index, AccumulatedVisualContextTree const& old_tree, VisualContextIndex new_index, AccumulatedVisualContextTree const& new_tree)
{
    for (;;) {
        auto const& old_node = old_tree.node_at(old_index);
        auto const& new_node = new_tree.node_at(new_index);
        if (old_node.depth != new_node.depth)
            return false;
        if (!old_node.data.visit([&](auto const& data) {
                using DataType = RemoveCVReference<decltype(data)>;
                return new_node.data.has<DataType>();
            }))
            return false;
        if (old_index == VISUAL_VIEWPORT_NODE_INDEX)
            return new_index == VISUAL_VIEWPORT_NODE_INDEX;
        if (new_index == VISUAL_VIEWPORT_NODE_INDEX)
            return false;
        old_index = old_node.parent_index;
        new_index = new_node.parent_index;
    }
}

static bool visual_context_chains_are_equal(VisualContextIndex old_index, AccumulatedVisualContextTree const& old_tree, VisualContextIndex new_index, AccumulatedVisualContextTree const& new_tree)
{
    for (;;) {
        auto const& old_node = old_tree.node_at(old_index);
        auto const& new_node = new_tree.node_at(new_index);
        if (old_node.has_empty_effective_clip != new_node.has_empty_effective_clip
            || !visual_context_data_is_equal(old_node.data, new_node.data))
            return false;
        if (old_index == VISUAL_VIEWPORT_NODE_INDEX)
            return true;
        old_index = old_node.parent_index;
        new_index = new_node.parent_index;
    }
}

Optional<Gfx::IntRect> compute_display_list_damage(
    ReadonlyBytes old_display_list_commands,
    AccumulatedVisualContextTree const& old_visual_context_tree,
    ScrollStateSnapshot const& old_scroll_state,
    ReadonlyBytes new_display_list_commands,
    AccumulatedVisualContextTree const& new_visual_context_tree,
    ScrollStateSnapshot const& new_scroll_state,
    Gfx::IntRect viewport_rect)
{
    auto old_commands = collect_display_list_command_references(old_display_list_commands);
    auto new_commands = collect_display_list_command_references(new_display_list_commands);

    auto commands_are_equal = [&](DisplayListCommandReference const& old_command, DisplayListCommandReference const& new_command) {
        return display_list_commands_are_equal(old_command, new_command)
            && visual_context_chains_are_compatible(old_command.header.context_index, old_visual_context_tree, new_command.header.context_index, new_visual_context_tree);
    };
    size_t common_prefix_length = 0;
    auto common_length = min(old_commands.size(), new_commands.size());
    while (common_prefix_length < common_length && commands_are_equal(old_commands[common_prefix_length], new_commands[common_prefix_length]))
        ++common_prefix_length;

    size_t common_suffix_length = 0;
    while (common_suffix_length < common_length - common_prefix_length
        && commands_are_equal(old_commands[old_commands.size() - common_suffix_length - 1], new_commands[new_commands.size() - common_suffix_length - 1]))
        ++common_suffix_length;
    Optional<Gfx::IntRect> damage_rect;
    bool changed_unbounded_command = false;
    auto add_command_damage = [&](DisplayListCommandReference const& command, auto const& visual_context_tree, auto const& scroll_state) {
        if (!command.header.has_bounding_rect) {
            if (display_list_command_is_compositor_metadata(command.header.type)
                || command.header.type == DisplayListCommandType::Save
                || command.header.type == DisplayListCommandType::SaveLayer
                || command.header.type == DisplayListCommandType::Restore)
                return;
            changed_unbounded_command = true;
            return;
        }
        auto transformed_rect = visual_context_tree.transform_rect_to_viewport(command.header.context_index, command.header.bounding_rect.to_type<float>(), scroll_state);
        auto command_damage = Gfx::enclosing_int_rect(transformed_rect);
        if (damage_rect.has_value())
            damage_rect->unite(command_damage);
        else
            damage_rect = command_damage;
    };

    auto add_visual_context_damage = [&](DisplayListCommandReference const& old_command, DisplayListCommandReference const& new_command) {
        if (visual_context_chains_are_equal(old_command.header.context_index, old_visual_context_tree, new_command.header.context_index, new_visual_context_tree))
            return;
        if (!old_command.header.has_bounding_rect || !new_command.header.has_bounding_rect) {
            if (old_command.header.type == DisplayListCommandType::CompositorViewportScrollbar || old_command.header.type == DisplayListCommandType::PaintScrollBar)
                changed_unbounded_command = true;
            return;
        }
        add_command_damage(old_command, old_visual_context_tree, old_scroll_state);
        add_command_damage(new_command, new_visual_context_tree, new_scroll_state);
    };

    for (size_t i = 0; i < common_prefix_length; ++i)
        add_visual_context_damage(old_commands[i], new_commands[i]);

    auto old_index = common_prefix_length;
    auto new_index = common_prefix_length;
    auto old_end = old_commands.size() - common_suffix_length;
    auto new_end = new_commands.size() - common_suffix_length;
    // Realign short inserted or removed sequences without making damage computation quadratic in the display list size.
    static constexpr size_t maximum_realignment_distance = 8;
    auto is_realignment_anchor = [&](size_t candidate_old_index, size_t candidate_new_index) {
        if (!commands_are_equal(old_commands[candidate_old_index], new_commands[candidate_new_index]))
            return false;
        if (candidate_old_index + 1 == old_end || candidate_new_index + 1 == new_end)
            return true;
        return commands_are_equal(old_commands[candidate_old_index + 1], new_commands[candidate_new_index + 1]);
    };
    while (old_index < old_end && new_index < new_end) {
        if (commands_are_equal(old_commands[old_index], new_commands[new_index])) {
            add_visual_context_damage(old_commands[old_index], new_commands[new_index]);
            ++old_index;
            ++new_index;
            continue;
        }

        Optional<size_t> skipped_old_commands;
        Optional<size_t> skipped_new_commands;
        for (size_t distance = 1; distance <= maximum_realignment_distance; ++distance) {
            if (!skipped_old_commands.has_value() && old_index + distance < old_end && is_realignment_anchor(old_index + distance, new_index))
                skipped_old_commands = distance;
            if (!skipped_new_commands.has_value() && new_index + distance < new_end && is_realignment_anchor(old_index, new_index + distance))
                skipped_new_commands = distance;
        }

        if (skipped_old_commands.has_value() && (!skipped_new_commands.has_value() || *skipped_old_commands <= *skipped_new_commands)) {
            for (size_t i = 0; i < *skipped_old_commands; ++i)
                add_command_damage(old_commands[old_index++], old_visual_context_tree, old_scroll_state);
            continue;
        }
        if (skipped_new_commands.has_value()) {
            for (size_t i = 0; i < *skipped_new_commands; ++i)
                add_command_damage(new_commands[new_index++], new_visual_context_tree, new_scroll_state);
            continue;
        }

        add_command_damage(old_commands[old_index++], old_visual_context_tree, old_scroll_state);
        add_command_damage(new_commands[new_index++], new_visual_context_tree, new_scroll_state);
    }
    while (old_index < old_end)
        add_command_damage(old_commands[old_index++], old_visual_context_tree, old_scroll_state);
    while (new_index < new_end)
        add_command_damage(new_commands[new_index++], new_visual_context_tree, new_scroll_state);

    for (size_t i = 0; i < common_suffix_length; ++i)
        add_visual_context_damage(old_commands[old_commands.size() - common_suffix_length + i], new_commands[new_commands.size() - common_suffix_length + i]);

    if (changed_unbounded_command)
        return {};
    if (!damage_rect.has_value())
        return Gfx::IntRect {};

    damage_rect->inflate(1, 1, 1, 1);
    damage_rect->intersect(viewport_rect);
    return damage_rect;
}

}
