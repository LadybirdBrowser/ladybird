/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NumericLimits.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/PaintingSurface.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
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

DisplayList::DisplayList(u64 compatible_visual_context_tree_version)
    : m_compatible_visual_context_tree_version(compatible_visual_context_tree_version)
    , m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

DisplayList::DisplayList(u64 compatible_visual_context_tree_version, u64 id, ByteBuffer&& command_bytes, Optional<AsyncScrollingMetadata> async_scrolling_metadata)
    : m_compatible_visual_context_tree_version(compatible_visual_context_tree_version)
    , m_id(id)
    , m_command_bytes(move(command_bytes))
    , m_async_scrolling_metadata(move(async_scrolling_metadata))
{
}

bool DisplayList::append_bytes(
    DisplayListCommandType type,
    ReadonlyBytes payload,
    ReadonlyBytes inline_data,
    AccumulatedVisualContextTree const& visual_context_tree,
    VisualContextIndex context_index,
    Optional<Gfx::IntRect> bounding_rect,
    bool is_clip)
{
    VERIFY(visual_context_tree.version() == m_compatible_visual_context_tree_version);
    if (visual_context_tree.has_empty_effective_clip(context_index))
        return false;
    VERIFY(m_command_bytes.size() % DisplayList::command_alignment == 0);
    VERIFY(payload.size() <= NumericLimits<u32>::max());
    VERIFY(inline_data.size() <= NumericLimits<u32>::max() - payload.size());
    auto payload_size = payload.size() + inline_data.size();
    auto record_size = sizeof(DisplayListCommandHeader) + payload_size;
    constexpr auto command_alignment = DisplayList::command_alignment;
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

void DisplayList::append_command_sequence(
    ReadonlyBytes command_sequence,
    AccumulatedVisualContextTree const& visual_context_tree,
    VisualContextIndex context_index)
{
    VERIFY(visual_context_tree.version() == m_compatible_visual_context_tree_version);

    auto command_bytes = MUST(ByteBuffer::copy(command_sequence));

    set_command_sequence_visual_context(command_bytes.span(), context_index);
    VERIFY(m_command_bytes.size() % DisplayList::command_alignment == 0);
    VERIFY(command_bytes.size() % DisplayList::command_alignment == 0);

    if (!command_bytes.is_empty())
        m_command_bytes.append(command_bytes.data(), command_bytes.size());
}

ByteBuffer DisplayList::copy_command_bytes_from(size_t command_start_offset) const
{
    VERIFY(command_start_offset <= m_command_bytes.size());
    return MUST(m_command_bytes.slice(
        command_start_offset,
        m_command_bytes.size() - command_start_offset));
}

void DisplayListPlayer::execute(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    DisplayListResourceStorage const& resource_storage,
    ScrollStateSnapshot const& scroll_state_snapshot,
    RefPtr<Gfx::PaintingSurface> surface,
    CanvasSurfaceRegistry const* canvas_surface_registry)
{
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());
    m_surface = surface;
    m_active_display_list = &display_list;
    m_active_visual_context_tree = &visual_context_tree;
    m_resource_storage = &resource_storage;
    m_canvas_surface_registry = canvas_surface_registry;
    execute_impl(display_list, scroll_state_snapshot);
    m_canvas_surface_registry = nullptr;
    m_resource_storage = nullptr;
    m_active_visual_context_tree = nullptr;
    m_active_display_list = nullptr;
    m_surface = nullptr;
}

void DisplayListPlayer::execute_display_list_into_surface(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree, Gfx::PaintingSurface& target_surface)
{
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());
    TemporaryChange surface_change { m_surface, RefPtr<Gfx::PaintingSurface> { target_surface } };
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    ScrollStateSnapshot scroll_state_snapshot;
    execute_impl(display_list, scroll_state_snapshot);
}

void DisplayListPlayer::execute_nested_display_list(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    ScrollStateSnapshot const& scroll_state_snapshot,
    ReadonlyBytes command_bytes)
{
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
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
    auto const& visual_context_tree = active_visual_context_tree();
    VERIFY(display_list.compatible_visual_context_tree_version() == visual_context_tree.version());

    VERIFY(m_surface);

    auto for_each_node_from_common_ancestor_to_target =
        [&](this auto const& self,
            Optional<VisualContextIndex> common_ancestor_index,
            VisualContextIndex target_index,
            auto&& callback) -> IterationDecision {
        if (common_ancestor_index.has_value() && target_index == common_ancestor_index.value())
            return IterationDecision::Continue;
        if (target_index != VISUAL_VIEWPORT_NODE_INDEX
            && self(common_ancestor_index, visual_context_tree.node_at(target_index).parent_index, callback)
                == IterationDecision::Break) {
            return IterationDecision::Break;
        }
        return callback(target_index, visual_context_tree.node_at(target_index));
    };

    auto apply_accumulated_visual_context =
        [&](VisualContextIndex, AccumulatedVisualContextNode const& node) {
            node.data.visit(
                [&](EffectsData const& effects) {
                    play_command(ApplyEffects {
                                     .opacity = effects.opacity,
                                     .compositing_and_blending_operator = effects.blend_mode,
                                     .has_filter = effects.gfx_filter.has_value(),
                                     .filter_data = {},
                                 },
                        effects.gfx_filter.has_value() ? &effects.gfx_filter.value() : nullptr);
                },
                [&](PerspectiveData const& perspective) {
                    play_command(Save {});
                    apply_transform({ 0, 0 }, perspective.matrix);
                },
                [&](ScrollData const& scroll) {
                    play_command(Save {});
                    auto offset = scroll_state.device_offset_for_index(scroll.scroll_frame_index);
                    if (!offset.is_zero())
                        play_command(Translate { .delta = offset.to_type<int>() });
                },
                [&](ScrollCompensation const& compensation) {
                    play_command(Save {});
                    auto offset = scroll_state.device_offset_for_index(compensation.scroll_frame_index);
                    if (!offset.is_zero())
                        play_command(Translate { .delta = (-offset).to_type<int>() });
                },
                [&](AnchorScrollShift const& shift) {
                    play_command(Save {});
                    auto offset = shift.masked_offset(scroll_state);
                    if (!offset.is_zero())
                        play_command(Translate { .delta = offset.to_type<int>() });
                },
                [&](TransformData const& transform) {
                    play_command(Save {});
                    apply_transform(transform.origin, transform.matrix);
                },
                [&](ClipData const& clip) {
                    play_command(Save {});
                    if (clip.corner_radii.has_any_radius()) {
                        play_command(AddRoundedRectClip {
                            .corner_radii = clip.corner_radii,
                            .border_rect = clip.rect.to_type<int>(),
                            .corner_clip = Gfx::CornerClip::Outside,
                        });
                    } else {
                        play_command(AddClipRect { .rect = clip.rect.to_type<int>() });
                    }
                },
                [&](ClipPathData const& clip_path) {
                    play_command(Save {});
                    add_clip_path(clip_path.path, clip_path.fill_rule);
                });
        };

    VisualContextIndex applied_context_index;
    bool has_applied_context { false };
    size_t applied_depth = 0;

    // OPTIMIZATION: When walking down to apply effects (opacity, filters, blend modes), check culling before applying
    //               each effect. Effects don't affect clip state, so the culling check is valid before applying them.
    //               This avoids expensive saveLayer/restore cycles for off-screen elements with effects like blur.
    enum class SwitchResult : u8 {
        Switched,
        CulledByEffect,
    };
    auto switch_to_context = [&](VisualContextIndex target_index, Optional<Gfx::IntRect> bounding_rect = {}) -> SwitchResult {
        if (has_applied_context && applied_context_index == target_index)
            return SwitchResult::Switched;

        Optional<VisualContextIndex> common_ancestor_index;
        if (has_applied_context)
            common_ancestor_index = visual_context_tree.find_common_ancestor(applied_context_index, target_index);
        size_t const common_ancestor_depth = common_ancestor_index.has_value() ? visual_context_tree.node_at(common_ancestor_index.value()).depth + 1 : 0;

        auto has_coordinate_changing_descendant = [&](Optional<VisualContextIndex> ancestor_index) {
            for (auto index = target_index;; index = visual_context_tree.node_at(index).parent_index) {
                if (ancestor_index.has_value() && index == ancestor_index.value())
                    break;
                auto const& node = visual_context_tree.node_at(index);
                if (node.data.has<TransformData>()
                    || node.data.has<PerspectiveData>()
                    || node.data.has<ScrollData>()
                    || node.data.has<ScrollCompensation>()
                    || node.data.has<AnchorScrollShift>())
                    return true;
                if (index == VISUAL_VIEWPORT_NODE_INDEX)
                    break;
            }
            return false;
        };

        auto restore_to_depth = [&](size_t target_depth) {
            while (applied_depth > target_depth) {
                play_command(Restore {});
                --applied_depth;
            }
        };
        restore_to_depth(common_ancestor_depth);

        auto result = SwitchResult::Switched;
        for_each_node_from_common_ancestor_to_target(
            common_ancestor_index,
            target_index,
            [&](VisualContextIndex node_index, AccumulatedVisualContextNode const& node) {
                if (bounding_rect.has_value() && node.data.has<EffectsData>()) {
                    auto can_cull_before_effect = !has_coordinate_changing_descendant(Optional<VisualContextIndex> { node_index });
                    if (bounding_rect->is_empty() || (can_cull_before_effect && would_be_fully_clipped_by_painter(*bounding_rect))) {
                        result = SwitchResult::CulledByEffect;
                        return IterationDecision::Break;
                    }
                }
                apply_accumulated_visual_context(node_index, node);
                applied_depth++;
                return IterationDecision::Continue;
            });

        if (result == SwitchResult::Switched) {
            applied_context_index = target_index;
            has_applied_context = true;
        } else {
            restore_to_depth(common_ancestor_depth);
        }
        return result;
    };

    DisplayList::for_each_command_header(commands, [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (display_list_command_is_compositor_metadata(header.type))
            return;

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
                    play_command(read_display_list_command_payload<AddClipRect>(payload));
                else
                    play_command(AddClipRect { bounding_rect.release_value() });
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
            play_command(command);                                                    \
        });                                                                           \
        break;
            ENUMERATE_DISPLAY_LIST_COMMANDS(DISPATCH_DISPLAY_LIST_COMMAND)
#undef DISPATCH_DISPLAY_LIST_COMMAND
        }
    });

    while (applied_depth > 0) {
        play_command(Restore {});
        applied_depth--;
    }
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayList::AsyncScrollingMetadata const& metadata)
{
    TRY(encoder.encode(metadata.viewport_rect));
    TRY(encoder.encode(metadata.wheel_event_listener_state_generation));
    TRY(encoder.encode(metadata.has_blocking_wheel_event_listeners));
    TRY(encoder.encode(metadata.has_blocking_wheel_event_region_covering_viewport));
    return {};
}

template<>
ErrorOr<Web::Painting::DisplayList::AsyncScrollingMetadata> decode(Decoder& decoder)
{
    return Web::Painting::DisplayList::AsyncScrollingMetadata {
        .viewport_rect = TRY(decoder.decode<Gfx::IntRect>()),
        .wheel_event_listener_state_generation = TRY(decoder.decode<u64>()),
        .has_blocking_wheel_event_listeners = TRY(decoder.decode<bool>()),
        .has_blocking_wheel_event_region_covering_viewport = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayList const& display_list)
{
    TRY(encoder.encode(display_list.m_id));
    TRY(encoder.encode(display_list.m_command_bytes));
    TRY(encoder.encode(display_list.m_compatible_visual_context_tree_version));
    TRY(encoder.encode(display_list.m_async_scrolling_metadata));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, NonnullRefPtr<Web::Painting::DisplayList> const& display_list)
{
    return encoder.encode(*display_list);
}

template<>
ErrorOr<NonnullRefPtr<Web::Painting::DisplayList>> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<u64>());
    auto command_bytes = TRY(decoder.decode<ByteBuffer>());
    auto compatible_visual_context_tree_version = TRY(decoder.decode<u64>());
    auto async_scrolling_metadata = TRY(decoder.decode<Optional<Web::Painting::DisplayList::AsyncScrollingMetadata>>());
    return adopt_ref(*new Web::Painting::DisplayList(compatible_visual_context_tree_version, id, move(command_bytes), move(async_scrolling_metadata)));
}

}
