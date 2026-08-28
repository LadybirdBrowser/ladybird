/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Debug.h>
#include <AK/NumericLimits.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Path.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

static Atomic<u64> s_next_id { 1 };

DisplayList::DisplayList(u64 compatible_visual_context_tree_structural_epoch)
    : m_compatible_visual_context_tree_structural_epoch(compatible_visual_context_tree_structural_epoch)
    , m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

DisplayList::DisplayList(u64 compatible_visual_context_tree_structural_epoch, u64 id, ByteBuffer&& command_bytes, Vector<DisplayListCommandRun>&& command_runs, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata> async_scrolling_metadata, HashMap<FrameNodeIndex, DisplayListResourceId>&& mask_display_lists)
    : m_compatible_visual_context_tree_structural_epoch(compatible_visual_context_tree_structural_epoch)
    , m_id(id)
    , m_command_bytes(move(command_bytes))
    , m_command_runs(move(command_runs))
    , m_surface_clear_color(surface_clear_color)
    , m_async_scrolling_metadata(move(async_scrolling_metadata))
    , m_mask_display_lists(move(mask_display_lists))
{
}

NonnullRefPtr<DisplayList> DisplayList::create_from_command_bytes(AccumulatedVisualContextTree const& visual_context_tree, ByteBuffer&& command_bytes, Vector<DisplayListCommandRun>&& command_runs)
{
    MUST(validate_display_list_command_runs(command_bytes, command_runs));
    auto display_list = create(visual_context_tree);
    display_list->m_command_bytes = move(command_bytes);
    display_list->m_command_runs = move(command_runs);
    return display_list;
}

Vector<DisplayListCommandRun> compute_display_list_command_runs(ReadonlyBytes command_bytes)
{
    Vector<DisplayListCommandRun> runs;
    u32 offset = 0;
    DisplayList::for_each_command_header(command_bytes, [&](DisplayListCommandHeader const& header, ReadonlyBytes) {
        auto record_size = static_cast<u32>(sizeof(DisplayListCommandHeader) + header.payload_size);
        if (runs.is_empty() || runs.last().context != header.context) {
            DisplayListCommandRun new_run {};
            new_run.offset = offset;
            new_run.context = header.context;
            runs.append(new_run);
        }
        auto& run = runs.last();
        run.size += record_size;
        offset += record_size;
        if (display_list_command_is_compositor_metadata(header.command_type))
            run.has_compositor_metadata = true;
        else if (header.has_bounding_rect)
            run.ink_bounds.unite(header.bounding_rect);
        else
            run.has_unbounded_draw = true;
    });
    return runs;
}

ErrorOr<void> validate_display_list_references_live_visual_context_nodes(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree)
{
    Vector<FrameNodeIndex> mask_frames;
    mask_frames.ensure_capacity(display_list.mask_display_lists().size());
    for (auto const& mask_display_list : display_list.mask_display_lists())
        mask_frames.unchecked_append(mask_display_list.key);
    auto command_runs = display_list.command_runs();
    if (!Layout::RustFFI::display_list_references_only_live_visual_context_nodes(visual_context_tree.rust_handle(), command_runs.data(), command_runs.size(), mask_frames.data(), mask_frames.size()))
        return Error::from_string_literal("Display list references a visual context node that is not live");
    return {};
}

ErrorOr<void> validate_display_list_command_runs(ReadonlyBytes command_bytes, ReadonlySpan<DisplayListCommandRun> runs)
{
    size_t next_offset = 0;
    for (auto const& run : runs) {
        if (run.offset != next_offset || run.size == 0 || run.size % DisplayList::command_alignment != 0)
            return Error::from_string_literal("Display list command runs do not cover the command bytes");
        next_offset += run.size;
    }
    if (next_offset != command_bytes.size())
        return Error::from_string_literal("Display list command runs do not cover the command bytes");
    if constexpr (DISPLAY_LIST_RUNS_DEBUG) {
        if (runs != compute_display_list_command_runs(command_bytes).span())
            return Error::from_string_literal("Display list command runs disagree with the command bytes");
    }
    return {};
}

void DisplayListPlayer::execute(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    DisplayListResourceStorage const& resource_storage,
    ScrollStateSnapshot const& scroll_state_snapshot,
    RefPtr<Gfx::PaintingSurface> surface,
    CanvasSurfaceRegistry const* canvas_surface_registry)
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    if (m_layer_filter_cache_tree_structural_epoch != visual_context_tree.structural_epoch()) {
        m_layer_filters_by_tree_structural_epoch_and_frame.clear();
        m_layer_filter_cache_tree_structural_epoch = visual_context_tree.structural_epoch();
    }
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
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
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
    ScrollStateSnapshot const& scroll_state_snapshot)
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    execute_impl(display_list, scroll_state_snapshot);
}

Gfx::Filter const& DisplayListPlayer::layer_filter(ReplayLayer const& layer)
{
    ReadonlyBytes filter_bytes { layer.filter_bytes, layer.filter_bytes_size };
    auto& filters_by_frame = m_layer_filters_by_tree_structural_epoch_and_frame.ensure(active_visual_context_tree().structural_epoch());
    if (auto cached = filters_by_frame.get(layer.frame.value()); cached.has_value() && cached->filter_bytes.bytes() == filter_bytes)
        return cached->filter;
    auto filter = Gfx::deserialize_filter(filter_bytes, [&](u64 image_id) {
        return resource_storage().image_frame(ImageFrameResourceId { image_id });
    });
    filters_by_frame.set(layer.frame.value(), CachedLayerFilter { MUST(ByteBuffer::copy(filter_bytes)), move(filter) });
    return filters_by_frame.get(layer.frame.value())->filter;
}

void DisplayListPlayer::execute_run_commands(DisplayListCommandRun const& run, ScrollStateSnapshot const& scroll_state)
{
    DisplayList::for_each_command_header(active_display_list().command_bytes_of_run(run), [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (display_list_command_is_compositor_metadata(header.command_type))
            return;

        auto bounding_rect = header.has_bounding_rect
            ? Optional<Gfx::IntRect>(header.bounding_rect)
            : Optional<Gfx::IntRect> {};

        if (bounding_rect.has_value() && (bounding_rect->is_empty() || would_be_fully_clipped_by_painter(*bounding_rect)))
            return;

        TemporaryChange current_command_payload_change { m_current_command_payload, payload };
        if (header.clips_to_bounding_rect)
            push_clip(ReplayClip { .rect = header.bounding_rect.to_type<float>(), .corner_radii = {}, .mode = ClipMode::Intersect });
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

        switch (header.command_type) {
#define DISPATCH_DISPLAY_LIST_COMMAND(command_type, player_method)                    \
    case DisplayListCommandType::command_type:                                        \
        dispatch_command.template operator()<command_type>([&](auto const& command) { \
            play_command(command);                                                    \
        });                                                                           \
        break;
            ENUMERATE_DISPLAY_LIST_COMMANDS(DISPATCH_DISPLAY_LIST_COMMAND)
#undef DISPATCH_DISPLAY_LIST_COMMAND
        }
        if (header.clips_to_bounding_rect)
            pop();
    });
}

void DisplayListPlayer::execute_impl(DisplayList const& display_list, ScrollStateSnapshot const& scroll_state)
{
    auto const& visual_context_tree = active_visual_context_tree();
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    VERIFY(m_surface);

    struct ReplayContext {
        DisplayListPlayer& player;
        ScrollStateSnapshot const& scroll_state;
    } replay_context { *this, scroll_state };

    Layout::RustFFI::FfiDisplayListReplayCallbacks callbacks {
        .context = &replay_context,
        .canvas_matrix = [](void* context) -> Gfx::FloatMatrix4x4 { return static_cast<ReplayContext*>(context)->player.canvas_matrix(); },
        .set_matrix = [](void* context, Gfx::FloatMatrix4x4 const* matrix) { static_cast<ReplayContext*>(context)->player.set_matrix(*matrix); },
        .would_be_fully_clipped_by_painter = [](void* context, Gfx::IntRect rect) -> bool {
            return static_cast<ReplayContext*>(context)->player.would_be_fully_clipped_by_painter(rect);
        },
        .push_clip = [](void* context, ReplayClip const* clip) { static_cast<ReplayContext*>(context)->player.push_clip(*clip); },
        .push_clip_path = [](void* context, void const* path, Gfx::WindingRule winding_rule) { static_cast<ReplayContext*>(context)->player.push_clip_path(*static_cast<Gfx::Path const*>(path), winding_rule); },
        .push_layer = [](void* context, ReplayLayer const* layer) { static_cast<ReplayContext*>(context)->player.push_layer(*layer); },
        .push_mask = [](void* context, ReplayMask const* mask) { static_cast<ReplayContext*>(context)->player.push_mask(*mask); },
        .pop_mask = [](void* context, ReplayMask const* mask, FrameNodeIndex frame) {
            auto& replay = *static_cast<ReplayContext*>(context);
            Optional<DisplayListResourceId> mask_content;
            if (auto display_list_id = replay.player.active_display_list().mask_display_list_id(frame);
                display_list_id.has_value() && replay.player.resource_storage().has_display_list(*display_list_id))
                mask_content = *display_list_id;
            replay.player.pop_mask(*mask, mask_content); },
        .pop = [](void* context) { static_cast<ReplayContext*>(context)->player.pop(); },
        .push_device_space_plane_clip = [](void* context, Gfx::FloatVector3 const* vertices, size_t vertex_count) {
            Gfx::Path path;
            path.move_to({ vertices[0].x(), vertices[0].y() });
            for (size_t i = 1; i < vertex_count; ++i)
                path.line_to({ vertices[i].x(), vertices[i].y() });
            path.close();
            static_cast<ReplayContext*>(context)->player.push_device_space_plane_clip(path); },
        .execute_run = [](void* context, size_t run_index) {
            auto& replay = *static_cast<ReplayContext*>(context);
            replay.player.execute_run_commands(replay.player.active_display_list().command_runs()[run_index], replay.scroll_state); },
    };

    auto command_runs = display_list.command_runs();
    auto scroll_offsets = scroll_state.device_offsets();
    Layout::RustFFI::display_list_replay(visual_context_tree.rust_handle(), command_runs.data(), command_runs.size(), scroll_offsets.data(), scroll_offsets.size(), &callbacks);
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
    TRY(encoder.encode(display_list.m_compatible_visual_context_tree_structural_epoch));
    TRY(encoder.encode(display_list.m_surface_clear_color));
    TRY(encoder.encode(display_list.m_async_scrolling_metadata));
    TRY(encoder.encode(display_list.m_mask_display_lists));
    // Trivially copyable records, so they travel as raw bytes like the command tape does.
    auto const& command_runs = display_list.m_command_runs;
    TRY(encoder.encode_size(command_runs.size()));
    if (!command_runs.is_empty())
        TRY(encoder.append(reinterpret_cast<u8 const*>(command_runs.data()), command_runs.size() * sizeof(Web::Painting::DisplayListCommandRun)));
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
    auto compatible_visual_context_tree_structural_epoch = TRY(decoder.decode<u64>());
    auto surface_clear_color = TRY(decoder.decode<Optional<Gfx::Color>>());
    auto async_scrolling_metadata = TRY(decoder.decode<Optional<Web::Painting::DisplayList::AsyncScrollingMetadata>>());
    auto mask_display_lists = TRY(decoder.decode<HashMap<Web::Painting::FrameNodeIndex, Web::Painting::DisplayListResourceId>>());
    auto command_run_count = TRY(decoder.decode_size());
    Vector<Web::Painting::DisplayListCommandRun> command_runs;
    TRY(command_runs.try_resize(command_run_count));
    if (!command_runs.is_empty())
        TRY(decoder.decode_into(Bytes { reinterpret_cast<u8*>(command_runs.data()), command_runs.size() * sizeof(Web::Painting::DisplayListCommandRun) }));
    TRY(Web::Painting::validate_display_list_command_runs(command_bytes, command_runs));
    return adopt_ref(*new Web::Painting::DisplayList(compatible_visual_context_tree_structural_epoch, id, move(command_bytes), move(command_runs), surface_clear_color, move(async_scrolling_metadata), move(mask_display_lists)));
}

}
