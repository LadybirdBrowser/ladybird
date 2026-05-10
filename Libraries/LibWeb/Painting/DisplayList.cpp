/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NumericLimits.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

static Atomic<u64> s_next_id { 1 };

template<DisplayListCommand Command, typename Callback>
static void rewrite_command_payload(Bytes payload, Callback callback)
{
    VERIFY(payload.size() >= sizeof(Command));
    auto command = read_display_list_command_payload<Command>(payload);
    callback(command);
    __builtin_memcpy(payload.data(), &command, sizeof(Command));
}

static void set_command_sequence_visual_context(Bytes command_bytes, VisualContextIndex context_index)
{
    for (size_t offset = 0; offset < command_bytes.size();) {
        VERIFY(offset + sizeof(DisplayListCommandHeader) <= command_bytes.size());
        auto* header_data = command_bytes.data() + offset;
        DisplayListCommandHeader header;
        __builtin_memcpy(&header, header_data, sizeof(header));
        header.context_index = context_index;
        __builtin_memcpy(header_data, &header, sizeof(header));
        offset += sizeof(header) + header.payload_size;
        VERIFY(offset <= command_bytes.size());
    }
}

static void adjust_display_list_data_span(DisplayListDataSpan& span, i64 offset_delta)
{
    auto offset = static_cast<i64>(span.offset) + offset_delta;
    VERIFY(offset >= 0);
    VERIFY(offset <= NumericLimits<u32>::max());
    span.offset = static_cast<u32>(offset);
}

static void adjust_gradient_color_stops(DisplayListGradientColorStops& color_stops, i64 offset_delta)
{
    adjust_display_list_data_span(color_stops.colors, offset_delta);
    adjust_display_list_data_span(color_stops.positions, offset_delta);
}

static void adjust_paint_style(DisplayListPaintStyle& paint_style, i64 offset_delta)
{
    switch (paint_style.type) {
    case DisplayListPaintStyleType::LinearGradient:
    case DisplayListPaintStyleType::RadialGradient:
        adjust_gradient_color_stops(paint_style.gradient.color_stops, offset_delta);
        break;
    case DisplayListPaintStyleType::None:
    case DisplayListPaintStyleType::Pattern:
        break;
    }
}

template<DisplayListCommand Command>
static constexpr bool command_has_inline_data()
{
    return (requires(Command command) { command.glyphs; })
        || (requires(Command command) { command.path_data; })
        || (requires(Command command) { command.dash_array; })
        || (requires(Command command) { command.paint_style; })
        || (requires(Command command) { command.command_bytes; })
        || (requires(Command command) { command.color_stops; });
}

template<DisplayListCommand Command>
static void adjust_command_inline_data_offsets(Bytes payload, i64 offset_delta)
{
    rewrite_command_payload<Command>(payload, [&](Command& command) {
        if constexpr (requires { command.glyphs; })
            adjust_display_list_data_span(command.glyphs, offset_delta);
        if constexpr (requires { command.path_data; })
            adjust_display_list_data_span(command.path_data, offset_delta);
        if constexpr (requires { command.dash_array; })
            adjust_display_list_data_span(command.dash_array, offset_delta);
        if constexpr (requires { command.paint_style; command.paint_kind; }) {
            if (command.paint_kind == decltype(command.paint_kind)::PaintStyle)
                adjust_paint_style(command.paint_style, offset_delta);
        }
        if constexpr (requires { command.command_bytes; })
            adjust_display_list_data_span(command.command_bytes, offset_delta);
        if constexpr (requires { command.color_stops; })
            adjust_gradient_color_stops(command.color_stops, offset_delta);
    });
}

static void adjust_command_sequence_inline_data_offsets(Bytes command_bytes, i64 offset_delta)
{
    DisplayListCommandSequence::for_each_command_header(command_bytes, [&](DisplayListCommandHeader const& header, Bytes payload) {
        visit_display_list_command_type(header.type, [&]<DisplayListCommand Command>() {
            if constexpr (command_has_inline_data<Command>())
                adjust_command_inline_data_offsets<Command>(payload, offset_delta);
        });
    });
}

DisplayListCommandSequence::DisplayListCommandSequence()
    : m_resource_storage(DisplayListResourceStorage::create())
{
}

DisplayList::DisplayList(
    NonnullRefPtr<AccumulatedVisualContextTree const> visual_context_tree,
    NonnullRefPtr<DisplayListResourceStorage> resource_storage)
    : m_visual_context_tree(move(visual_context_tree))
    , m_resource_storage(move(resource_storage))
    , m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

bool DisplayList::append_bytes(
    DisplayListCommandType type,
    ReadonlyBytes payload,
    ReadonlyBytes inline_data,
    VisualContextIndex context_index,
    Optional<Gfx::IntRect> bounding_rect,
    bool is_clip)
{
    if (context_index.value() && m_visual_context_tree->has_empty_effective_clip(context_index))
        return false;
    VERIFY(m_command_bytes.size() % DisplayListCommandSequence::command_alignment == 0);
    VERIFY(payload.size() <= NumericLimits<u32>::max());
    VERIFY(inline_data.size() <= NumericLimits<u32>::max() - payload.size());
    auto payload_size = payload.size() + inline_data.size();
    auto record_size = sizeof(DisplayListCommandHeader) + payload_size;
    constexpr auto command_alignment = DisplayListCommandSequence::command_alignment;
    auto trailing_padding = align_up_to(record_size, command_alignment) - record_size;
    VERIFY(trailing_padding <= NumericLimits<u32>::max() - payload_size);
    DisplayListCommandHeader header {
        .type = type,
        .payload_size = static_cast<u32>(payload_size + trailing_padding),
        .context_index = context_index,
        .has_bounding_rect = bounding_rect.has_value(),
        .is_clip = is_clip,
        .bounding_rect = bounding_rect.value_or({}),
    };
    m_command_bytes.append(reinterpret_cast<u8 const*>(&header), sizeof(header));
    m_command_bytes.append(payload.data(), payload.size());
    if (!inline_data.is_empty())
        m_command_bytes.append(inline_data.data(), inline_data.size());
    m_command_bytes.resize(m_command_bytes.size() + trailing_padding);
    return true;
}

void DisplayList::append_command_sequence(DisplayListCommandSequence const& sequence, VisualContextIndex context_index)
{
    Vector<u8> command_bytes;
    if (!sequence.m_command_bytes.is_empty())
        command_bytes.append(sequence.m_command_bytes.data(), sequence.m_command_bytes.size());

    set_command_sequence_visual_context(command_bytes.span(), context_index);
    m_resource_storage->append_referenced_resources_from(*sequence.m_resource_storage, command_bytes.span());
    VERIFY(m_command_bytes.size() % DisplayListCommandSequence::command_alignment == 0);
    VERIFY(command_bytes.size() % DisplayListCommandSequence::command_alignment == 0);
    adjust_command_sequence_inline_data_offsets(command_bytes.span(), m_command_bytes.size());

    if (!command_bytes.is_empty())
        m_command_bytes.append(command_bytes.data(), command_bytes.size());
}

DisplayListCommandSequence DisplayList::copy_command_sequence_from(size_t command_start_offset) const
{
    VERIFY(command_start_offset <= m_command_bytes.size());
    DisplayListCommandSequence sequence;
    if (command_start_offset < m_command_bytes.size())
        sequence.m_command_bytes.append(
            m_command_bytes.data() + command_start_offset,
            m_command_bytes.size() - command_start_offset);
    adjust_command_sequence_inline_data_offsets(
        sequence.m_command_bytes.span(),
        -static_cast<i64>(command_start_offset));
    sequence.m_resource_storage->append_referenced_resources_from(*m_resource_storage, sequence.m_command_bytes.span());
    return sequence;
}

void DisplayListPlayer::execute(DisplayList const& display_list, ScrollStateSnapshot const& scroll_state_snapshot, RefPtr<Gfx::PaintingSurface> surface)
{
    m_surface = surface;
    m_active_display_list = &display_list;
    execute_impl(display_list, scroll_state_snapshot);
    if (surface)
        flush();
    m_active_display_list = nullptr;
    m_surface = nullptr;
}

void DisplayListPlayer::execute_display_list_into_surface(DisplayList const& display_list, Gfx::PaintingSurface& target_surface)
{
    TemporaryChange surface_change { m_surface, RefPtr<Gfx::PaintingSurface> { target_surface } };
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    ScrollStateSnapshot scroll_state_snapshot;
    execute_impl(display_list, scroll_state_snapshot);
}

void DisplayListPlayer::execute_nested_display_list(
    DisplayList const& display_list,
    ScrollStateSnapshot const& scroll_state_snapshot,
    ReadonlyBytes command_bytes)
{
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    execute_impl(display_list, scroll_state_snapshot, command_bytes);
}

void DisplayListPlayer::execute_impl(DisplayList const& display_list, ScrollStateSnapshot const& scroll_state)
{
    execute_impl(display_list, scroll_state, display_list.command_bytes());
}

void DisplayListPlayer::execute_impl(
    DisplayList const& display_list,
    ScrollStateSnapshot const& scroll_state,
    ReadonlyBytes commands)
{
    auto const& visual_context_tree = display_list.visual_context_tree();

    VERIFY(m_surface);

    auto for_each_node_from_common_ancestor_to_target =
        [&](this auto const& self,
            VisualContextIndex common_ancestor_index,
            VisualContextIndex target_index,
            auto&& callback) -> IterationDecision {
        if (!target_index.value() || target_index == common_ancestor_index)
            return IterationDecision::Continue;
        if (self(common_ancestor_index, visual_context_tree.node_at(target_index).parent_index, callback)
            == IterationDecision::Break) {
            return IterationDecision::Break;
        }
        return callback(target_index, visual_context_tree.node_at(target_index));
    };

    auto apply_accumulated_visual_context =
        [&](VisualContextIndex, AccumulatedVisualContextNode const& node) {
            node.data.visit(
                [&](EffectsData const& effects) {
                    apply_effects({
                                      .opacity = effects.opacity,
                                      .compositing_and_blending_operator = effects.blend_mode,
                                      .has_filter = effects.gfx_filter.has_value(),
                                      .filter_id = {},
                                  },
                        effects.gfx_filter.has_value() ? &effects.gfx_filter.value() : nullptr);
                },
                [&](PerspectiveData const& perspective) {
                    save({});
                    apply_transform({ 0, 0 }, perspective.matrix);
                },
                [&](ScrollData const& scroll) {
                    save({});
                    auto offset = scroll_state.device_offset_for_index(scroll.scroll_frame_index);
                    if (!offset.is_zero())
                        translate({ .delta = offset.to_type<int>() });
                },
                [&](ScrollCompensation const& compensation) {
                    save({});
                    auto offset = scroll_state.device_offset_for_index(compensation.scroll_frame_index);
                    if (!offset.is_zero())
                        translate({ .delta = (-offset).to_type<int>() });
                },
                [&](TransformData const& transform) {
                    save({});
                    apply_transform(transform.origin, transform.matrix);
                },
                [&](ClipData const& clip) {
                    save({});
                    if (clip.corner_radii.has_any_radius()) {
                        add_rounded_rect_clip({
                            .corner_radii = clip.corner_radii,
                            .border_rect = clip.rect.to_type<int>(),
                            .corner_clip = CornerClip::Outside,
                        });
                    } else {
                        add_clip_rect({ .rect = clip.rect.to_type<int>() });
                    }
                },
                [&](ClipPathData const& clip_path) {
                    save({});
                    add_clip_path(clip_path.path);
                });
        };

    VisualContextIndex applied_context_index;
    size_t applied_depth = 0;

    // OPTIMIZATION: When walking down to apply effects (opacity, filters, blend modes), check culling before applying
    //               each effect. Effects don't affect clip state, so the culling check is valid before applying them.
    //               This avoids expensive saveLayer/restore cycles for off-screen elements with effects like blur.
    enum class SwitchResult : u8 {
        Switched,
        CulledByEffect,
    };
    auto switch_to_context = [&](VisualContextIndex target_index, Optional<Gfx::IntRect> bounding_rect = {}) -> SwitchResult {
        if (applied_context_index == target_index)
            return SwitchResult::Switched;

        auto common_ancestor_index = visual_context_tree.find_common_ancestor(applied_context_index, target_index);
        size_t const common_ancestor_depth = common_ancestor_index.value() ? visual_context_tree.node_at(common_ancestor_index).depth : 0;

        while (applied_depth > common_ancestor_depth) {
            restore({});
            applied_depth--;
        }

        auto result = SwitchResult::Switched;
        for_each_node_from_common_ancestor_to_target(
            common_ancestor_index,
            target_index,
            [&](VisualContextIndex node_index, AccumulatedVisualContextNode const& node) {
                if (bounding_rect.has_value() && node.data.has<EffectsData>()) {
                    if (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect)) {
                        result = SwitchResult::CulledByEffect;
                        return IterationDecision::Break;
                    }
                }
                apply_accumulated_visual_context(node_index, node);
                applied_depth++;
                return IterationDecision::Continue;
            });

        if (result == SwitchResult::Switched)
            applied_context_index = target_index;
        return result;
    };

    DisplayList::for_each_command_header(commands, [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        auto bounding_rect = header.has_bounding_rect
            ? Optional<Gfx::IntRect>(header.bounding_rect)
            : Optional<Gfx::IntRect> {};

        if (switch_to_context(header.context_index, bounding_rect) == SwitchResult::CulledByEffect)
            return;

        if (bounding_rect.has_value() && (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect))) {
            // Any clip that's located outside of the visible region is equivalent to a simple clip-rect,
            // so replace it with one to avoid doing unnecessary work.
            if (header.is_clip) {
                if (header.type == DisplayListCommandType::AddClipRect)
                    add_clip_rect(read_display_list_command_payload<AddClipRect>(payload));
                else
                    add_clip_rect({ bounding_rect.release_value() });
            }
            return;
        }

        auto dispatch_command = [&]<DisplayListCommand Command>(auto&& callback) {
            auto command = read_display_list_command_payload<Command>(payload);
            if constexpr (IsSame<Command, PaintScrollBar>) {
                auto device_offset = scroll_state.device_offset_for_index(command.scroll_frame_index);
                if (command.vertical)
                    command.thumb_rect.translate_by(0, static_cast<int>(-device_offset.y() * command.scroll_size));
                else
                    command.thumb_rect.translate_by(static_cast<int>(-device_offset.x() * command.scroll_size), 0);
            }
            callback(command);
        };

        switch (header.type) {
#define DISPATCH_DISPLAY_LIST_COMMAND(command_type, player_method)                    \
    case DisplayListCommandType::command_type:                                        \
        dispatch_command.template operator()<command_type>([&](auto const& command) { \
            player_method(command);                                                   \
        });                                                                           \
        break;
            ENUMERATE_DISPLAY_LIST_COMMANDS(DISPATCH_DISPLAY_LIST_COMMAND)
#undef DISPATCH_DISPLAY_LIST_COMMAND
        }
    });

    while (applied_depth > 0) {
        restore({});
        applied_depth--;
    }
}

}
