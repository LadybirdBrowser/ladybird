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
    bool context_geometry_only,
    Optional<Gfx::IntRect> bounding_rect,
    bool is_clip)
{
    VERIFY(visual_context_tree.version() == m_compatible_visual_context_tree_version);
    // Geometry-only commands ignore the chain's clips, so an empty effective clip doesn't make them invisible.
    if (!context_geometry_only && visual_context_tree.has_empty_effective_clip(context_index))
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
        .context_geometry_only = context_geometry_only,
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

u32 DisplayList::append_command_range_from(
    DisplayList const& source_display_list,
    DisplayListCommandRange source_range,
    AccumulatedVisualContextTree const& visual_context_tree,
    VisualContextIndex recorded_context_index,
    VisualContextIndex current_context_index)
{
    VERIFY(&source_display_list != this);
    VERIFY(visual_context_tree.version() == m_compatible_visual_context_tree_version);
    VERIFY(m_command_bytes.size() % DisplayList::command_alignment == 0);
    VERIFY(source_range.size % DisplayList::command_alignment == 0);
    VERIFY(static_cast<size_t>(source_range.offset) + source_range.size <= source_display_list.m_command_bytes.size());

    auto destination_offset = m_command_bytes.size();
    VERIFY(destination_offset + source_range.size <= NumericLimits<u32>::max());
    if (source_range.is_empty())
        return static_cast<u32>(destination_offset);

    m_command_bytes.append(source_display_list.m_command_bytes.data() + source_range.offset, source_range.size);
    // The copied headers already carry the index the range was recorded under, so they only need rewriting
    // when the paintable's context was assigned a different index since then.
    if (recorded_context_index != current_context_index)
        set_command_sequence_visual_context(m_command_bytes.span().slice(destination_offset, source_range.size), current_context_index);
    return static_cast<u32>(destination_offset);
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

    // Cumulative to-root matrices for every visual context node, resolved against the live scroll
    // offsets and folded onto the canvas matrix at replay entry, so any node's space can be entered
    // absolutely with a single set_matrix(). Coordinate-affecting nodes therefore never touch the
    // canvas save stack; only clips and effects do. Clip and effect nodes inherit their parent's
    // matrix, so the palette is defined for every node index; nearest_spatial_node canonicalizes
    // indices whose spaces coincide onto one identity for the current-matrix cache, and
    // nearest_frame_node links every node to its innermost clip-or-effect ancestor-or-self (root
    // index 0 doubling as none), letting frame chains hop between frames without visiting the
    // coordinate-affecting nodes in between. The storage lives on the player so steady-state
    // replays reuse warm capacity; moving it out for the duration of the replay keeps re-entrant
    // nested replays from clobbering the outer palette.
    auto palette_storage = move(m_replay_palette_storage);
    auto& transform_palette = palette_storage.to_root_matrices;
    auto& nearest_spatial_node = palette_storage.nearest_spatial_nodes;
    auto& nearest_frame_node = palette_storage.nearest_frame_nodes;
    auto const& nodes = visual_context_tree.nodes();
    transform_palette.clear_with_capacity();
    transform_palette.ensure_capacity(nodes.size());
    nearest_spatial_node.clear_with_capacity();
    nearest_spatial_node.ensure_capacity(nodes.size());
    nearest_frame_node.clear_with_capacity();
    nearest_frame_node.ensure_capacity(nodes.size());
    auto const replay_base_matrix = canvas_matrix();
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto const& node = nodes[i];
        auto append_spatial = [&](Gfx::FloatMatrix4x4 const& local_matrix) {
            auto const& parent_matrix = i == 0 ? replay_base_matrix : transform_palette[node.parent_index.value()];
            transform_palette.unchecked_append(parent_matrix * local_matrix);
            nearest_spatial_node.unchecked_append(VisualContextIndex { i });
            nearest_frame_node.unchecked_append(i == 0 ? VISUAL_VIEWPORT_NODE_INDEX : nearest_frame_node[node.parent_index.value()]);
        };
        auto append_spatial_translation = [&](Gfx::IntPoint offset) {
            // Whole device pixels, matching the Translate ops the per-node application replayed.
            append_spatial(Gfx::translation_matrix(Vector3<float>(static_cast<float>(offset.x()), static_cast<float>(offset.y()), 0)));
        };
        auto append_non_spatial = [&] {
            VERIFY(i != 0);
            transform_palette.unchecked_append(transform_palette[node.parent_index.value()]);
            nearest_spatial_node.unchecked_append(nearest_spatial_node[node.parent_index.value()]);
            nearest_frame_node.unchecked_append(VisualContextIndex { i });
        };
        node.data.visit(
            [&](TransformData const& transform) {
                auto matrix = Gfx::translation_matrix(Vector3<float>(transform.origin.x(), transform.origin.y(), 0));
                matrix = matrix * transform.matrix;
                matrix = matrix * Gfx::translation_matrix(Vector3<float>(-transform.origin.x(), -transform.origin.y(), 0));
                append_spatial(matrix);
            },
            [&](PerspectiveData const& perspective) {
                append_spatial(perspective.matrix);
            },
            [&](ScrollData const&) {
                append_spatial_translation(scroll_state.device_offset_for_index(VisualContextIndex { i }).to_type<int>());
            },
            [&](ScrollCompensation const& compensation) {
                append_spatial_translation((-scroll_state.device_offset_for_index(compensation.scroll_node_index)).to_type<int>());
            },
            [&](AnchorScrollShift const& shift) {
                append_spatial_translation(shift.masked_offset(scroll_state).to_type<int>());
            },
            [&](ClipData const&) { append_non_spatial(); },
            [&](ClipPathData const&) { append_non_spatial(); },
            [&](EffectsData const&) { append_non_spatial(); });
    }

    // The palette entry the canvas matrix currently equals, if known; every Restore resets the
    // matrix to its save point, so unwinding applied frames invalidates it. Recorded streams
    // contain no matrix-mutating commands, so playing commands never invalidates the cache.
    Optional<VisualContextIndex> current_ctm_space;
    auto ensure_ctm_space = [&](VisualContextIndex context_index) {
        auto space_node_index = nearest_spatial_node[context_index.value()];
        if (current_ctm_space == space_node_index)
            return;
        set_matrix(transform_palette[space_node_index.value()]);
        current_ctm_space = space_node_index;
    };

    // One entry of the applied/target context: a clip or effect node on the target's root chain,
    // each holding exactly one canvas save (a Save for clips, the layer pushed by ApplyEffects for
    // effects), so unwinding is one Restore per popped frame. Geometry-only commands target an
    // empty frame list: their coordinates come entirely from the palette.
    Vector<VisualContextIndex, 16> applied_frames;
    Vector<VisualContextIndex, 16> target_frames;
    Optional<VisualContextIndex> applied_context_index;
    bool applied_context_geometry_only { false };

    auto build_target_frames = [&](VisualContextIndex target_index, bool geometry_only) {
        target_frames.clear_with_capacity();
        if (geometry_only)
            return;
        for (auto index = nearest_frame_node[target_index.value()];
            index != VISUAL_VIEWPORT_NODE_INDEX;
            index = nearest_frame_node[visual_context_tree.node_at(index).parent_index.value()]) {
            target_frames.append(index);
        }
        target_frames.reverse();
    };

    auto restore_to_length = [&](size_t length) {
        if (applied_frames.size() > length)
            current_ctm_space = {};
        while (applied_frames.size() > length) {
            play_command(Restore {});
            applied_frames.take_last();
        }
    };

    // OPTIMIZATION: When walking down to apply effects (opacity, filters, blend modes), check culling before applying
    //               each effect. Effects don't affect clip state, so the culling check is valid before applying them.
    //               This avoids expensive saveLayer/restore cycles for off-screen elements with effects like blur.
    enum class SwitchResult : u8 {
        Switched,
        CulledByEffect,
    };
    auto switch_to_context = [&](VisualContextIndex target_index, bool geometry_only, Optional<Gfx::IntRect> bounding_rect = {}) -> SwitchResult {
        if (applied_context_index == target_index && applied_context_geometry_only == geometry_only)
            return SwitchResult::Switched;

        build_target_frames(target_index, geometry_only);

        auto common_prefix_length = applied_frames.span().matching_prefix_length(target_frames);

        restore_to_length(common_prefix_length);

        for (size_t i = common_prefix_length; i < target_frames.size(); ++i) {
            auto frame_node_index = target_frames[i];
            auto const& frame_node = visual_context_tree.node_at(frame_node_index);
            if (auto const* effects = frame_node.data.get_pointer<EffectsData>()) {
                if (bounding_rect.has_value()) {
                    bool culled_by_effect = bounding_rect->is_empty();
                    if (!culled_by_effect) {
                        ensure_ctm_space(target_index);
                        culled_by_effect = would_be_fully_clipped_by_painter(*bounding_rect);
                    }
                    if (culled_by_effect) {
                        restore_to_length(common_prefix_length);
                        // The canvas is unwound to the shared prefix; clearing the applied index
                        // keeps the fast path from reusing the pre-cull context while the frame
                        // vector still enables prefix reuse on the next switch.
                        applied_context_index = {};
                        return SwitchResult::CulledByEffect;
                    }
                }
                ensure_ctm_space(frame_node_index);
                play_command(ApplyEffects {
                                 .opacity = effects->opacity,
                                 .compositing_and_blending_operator = effects->blend_mode,
                                 .has_filter = effects->gfx_filter.has_value(),
                                 .filter_data = {},
                             },
                    effects->gfx_filter.has_value() ? &effects->gfx_filter.value() : nullptr);
            } else {
                play_command(Save {});
                ensure_ctm_space(frame_node_index);
                frame_node.data.visit(
                    [&](ClipData const& clip) {
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
                        add_clip_path(clip_path.path, clip_path.fill_rule);
                    },
                    [&](auto const&) { VERIFY_NOT_REACHED(); });
            }
            applied_frames.append(frame_node_index);
        }

        applied_context_index = target_index;
        applied_context_geometry_only = geometry_only;
        return SwitchResult::Switched;
    };

    DisplayList::for_each_command_header(commands, [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (display_list_command_is_compositor_metadata(header.type))
            return;

        auto bounding_rect = header.has_bounding_rect
            ? Optional<Gfx::IntRect>(header.bounding_rect)
            : Optional<Gfx::IntRect> {};

        if (switch_to_context(header.context_index, header.context_geometry_only, bounding_rect) == SwitchResult::CulledByEffect)
            return;

        ensure_ctm_space(header.context_index);

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
                auto device_offset = scroll_state.device_offset_for_index(command.scroll_node_index);
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

    restore_to_length(0);
    // Node spaces were entered by setting the canvas matrix absolutely, outside any save, so the
    // matrix the replay entered with must be handed back explicitly.
    set_matrix(replay_base_matrix);

    m_replay_palette_storage = move(palette_storage);
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
