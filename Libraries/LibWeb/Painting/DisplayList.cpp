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

static void set_command_sequence_visual_context(Bytes command_bytes, VisualContextIndex context_index)
{
    for (size_t offset = 0; offset < command_bytes.size();) {
        VERIFY(offset + sizeof(DisplayListCommandHeader) <= command_bytes.size());
        auto* header_data = command_bytes.data() + offset;
        auto header = read_display_list_object<DisplayListCommandHeader>({ header_data, command_bytes.size() - offset });
        header.context_index = context_index;
        write_display_list_object(Bytes { header_data, sizeof(header) }, header);
        offset += sizeof(header) + header.payload_size;
        VERIFY(offset <= command_bytes.size());
    }
}

DisplayListCommandSequence::DisplayListCommandSequence() = default;

DisplayList::DisplayList(
    NonnullRefPtr<AccumulatedVisualContextTree const> visual_context_tree,
    DisplayListResourceStorage resource_storage)
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
    auto header_bytes = display_list_object_bytes(header);
    m_command_bytes.append(header_bytes.data(), header_bytes.size());
    m_command_bytes.append(payload.data(), payload.size());
    if (!inline_data.is_empty())
        m_command_bytes.append(inline_data.data(), inline_data.size());
    m_command_bytes.resize(m_command_bytes.size() + trailing_padding, ByteBuffer::ZeroFillNewElements::Yes);
    return true;
}

void DisplayList::append_command_sequence(DisplayListCommandSequence const& sequence, VisualContextIndex context_index)
{
    auto command_bytes = MUST(ByteBuffer::copy(sequence.m_command_bytes.span()));

    set_command_sequence_visual_context(command_bytes.span(), context_index);
    m_resource_storage.append_referenced_resources_from(sequence.m_resource_storage, command_bytes.span());
    VERIFY(m_command_bytes.size() % DisplayListCommandSequence::command_alignment == 0);
    VERIFY(command_bytes.size() % DisplayListCommandSequence::command_alignment == 0);

    if (!command_bytes.is_empty())
        m_command_bytes.append(command_bytes.data(), command_bytes.size());
}

DisplayListCommandSequence DisplayList::copy_command_sequence_from(size_t command_start_offset) const
{
    VERIFY(command_start_offset <= m_command_bytes.size());
    DisplayListCommandSequence sequence;
    sequence.m_command_bytes = MUST(m_command_bytes.slice(command_start_offset, m_command_bytes.size() - command_start_offset));
    sequence.m_resource_storage.append_referenced_resources_from(m_resource_storage, sequence.m_command_bytes.span());
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
                                      .filter_data = {},
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

        TemporaryChange current_command_payload_change { m_current_command_payload, payload };
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
