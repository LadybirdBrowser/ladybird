/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Checked.h>
#include <AK/NumericLimits.h>
#include <AK/TemporaryChange.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Path.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

static Atomic<u64> s_next_id { 1 };

DisplayList::DisplayList(u64 compatible_visual_context_tree_structural_epoch)
    : m_compatible_visual_context_tree_structural_epoch(compatible_visual_context_tree_structural_epoch)
    , m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

DisplayList::DisplayList(u64 compatible_visual_context_tree_structural_epoch, u64 id, ByteBuffer&& command_bytes, Vector<DisplayListCommandRun>&& command_runs, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata> async_scrolling_metadata)
    : m_compatible_visual_context_tree_structural_epoch(compatible_visual_context_tree_structural_epoch)
    , m_id(id)
    , m_command_bytes(move(command_bytes))
    , m_command_runs(move(command_runs))
    , m_surface_clear_color(surface_clear_color)
    , m_async_scrolling_metadata(move(async_scrolling_metadata))
{
}

DisplayList::~DisplayList()
{
    Compositing::RustFFI::display_list_destroy_effect_clip_plan(m_replay_effect_clip_plan.load());
    Compositing::RustFFI::display_list_release_command_storage(m_rust_command_storage);
}

void const* DisplayList::replay_effect_clip_plan(AccumulatedVisualContextTree const& tree) const
{
    VERIFY(m_compatible_visual_context_tree_structural_epoch == tree.structural_epoch());
    if (auto const* plan = m_replay_effect_clip_plan.load())
        return plan;
    auto runs = command_runs();
    auto const* plan = Compositing::RustFFI::display_list_create_effect_clip_plan(tree.rust_handle(), runs.data(), runs.size());
    VERIFY(plan);
    void const* existing = nullptr;
    if (!m_replay_effect_clip_plan.compare_exchange_strong(existing, plan)) {
        Compositing::RustFFI::display_list_destroy_effect_clip_plan(plan);
        return existing;
    }
    return plan;
}

NonnullRefPtr<DisplayList> DisplayList::share_rust_command_storage(AccumulatedVisualContextTree const& visual_context_tree, void const* storage)
{
    VERIFY(storage);
    return adopt_rust_command_storage(visual_context_tree, Compositing::RustFFI::display_list_retain_command_storage(storage));
}

NonnullRefPtr<DisplayList> DisplayList::adopt_rust_command_storage(AccumulatedVisualContextTree const& visual_context_tree, void const* storage)
{
    VERIFY(storage);
    auto recorded = Compositing::RustFFI::display_list_command_storage_view(storage);
    auto display_list = create(visual_context_tree);
    display_list->m_rust_command_storage = storage;
    display_list->m_borrowed_command_bytes = { recorded.bytes, recorded.byte_count };
    display_list->m_borrowed_command_runs = { recorded.command_runs, recorded.command_run_count };
    MUST(validate_display_list_command_runs(display_list->command_bytes(), display_list->command_runs()));
    return display_list;
}

Optional<DisplayList::SharedBufferLayout> DisplayList::shared_buffer_layout(u64 tape_size, u64 run_count)
{
    if (tape_size % command_alignment != 0)
        return {};
    Checked<u64> runs_bytes = run_count;
    runs_bytes *= sizeof(DisplayListCommandRun);
    Checked<u64> total_size = tape_size;
    total_size += runs_bytes;
    if (runs_bytes.has_overflow() || total_size.has_overflow())
        return {};
    return SharedBufferLayout { .runs_offset = tape_size, .total_size = total_size.value() };
}

ErrorOr<Core::AnonymousBuffer> DisplayList::copy_to_shared_buffer() const
{
    auto tape = command_bytes();
    auto runs = command_runs();
    auto layout = shared_buffer_layout(tape.size(), runs.size());
    if (!layout.has_value())
        return Error::from_string_literal("Display list is too large for a shared buffer");
    if (layout->total_size == 0)
        return Core::AnonymousBuffer {};
    // Sealed where the platform supports it, so the receiver cannot be faulted by the file shrinking.
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(layout->total_size, Core::AnonymousBuffer::Sealability::Sealable));
    auto* destination = buffer.data<u8>();
    tape.copy_to({ destination, tape.size() });
    if (!runs.is_empty())
        __builtin_memcpy(destination + layout->runs_offset, runs.data(), runs.size() * sizeof(DisplayListCommandRun));
    return buffer;
}

ErrorOr<NonnullRefPtr<DisplayList>> DisplayList::create_from_shared_buffer(Properties properties, Core::AnonymousBuffer shared_tape_buffer, u64 tape_size, u64 run_count)
{
    auto layout = shared_buffer_layout(tape_size, run_count);
    if (!layout.has_value())
        return Error::from_string_literal("Display list sizes do not describe a shared buffer");
    ByteBuffer tape;
    Vector<DisplayListCommandRun> command_runs;
    if (layout->total_size > 0) {
        if (!shared_tape_buffer.is_valid())
            return Error::from_string_literal("Display list arrived without its shared buffer");
        TRY(shared_tape_buffer.validate_backing_size());
        if (layout->total_size > shared_tape_buffer.size())
            return Error::from_string_literal("Display list sizes exceed its shared buffer");
        // Every run holds at least one command, so more runs than headers cannot describe this tape.
        if (run_count > tape_size / sizeof(DisplayListCommandHeader))
            return Error::from_string_literal("Display list run table is larger than its tape allows");
        // The sender can still write to the buffer, so the tape and its runs are copied out before they are checked.
        TRY(command_runs.try_resize(run_count));
        if (run_count > 0)
            __builtin_memcpy(command_runs.data(), shared_tape_buffer.data<u8>() + layout->runs_offset, run_count * sizeof(DisplayListCommandRun));
        tape = TRY(ByteBuffer::copy(shared_tape_buffer.bytes().slice(0, tape_size)));
    }
    TRY(validate_received_display_list_tape(tape, command_runs));
    return adopt_ref(*new DisplayList(properties.compatible_visual_context_tree_structural_epoch, properties.id, move(tape), move(command_runs), properties.surface_clear_color, move(properties.async_scrolling_metadata)));
}

DisplayList::Properties DisplayList::properties() const
{
    return {
        .id = m_id,
        .compatible_visual_context_tree_structural_epoch = m_compatible_visual_context_tree_structural_epoch,
        .surface_clear_color = m_surface_clear_color,
        .async_scrolling_metadata = m_async_scrolling_metadata,
    };
}

ErrorOr<void> validate_display_list_references_live_visual_context_nodes(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree)
{
    auto command_runs = display_list.command_runs();
    if (!Compositing::RustFFI::display_list_references_only_live_visual_context_nodes(visual_context_tree.rust_handle(), command_runs.data(), command_runs.size()))
        return Error::from_string_literal("Display list references a visual context node that is not live");
    return {};
}

ErrorOr<void> validate_display_list_command_runs(ReadonlyBytes command_bytes, ReadonlySpan<DisplayListCommandRun> runs)
{
    size_t next_offset = 0;
    for (auto const& run : runs) {
        if (run.offset != next_offset || run.size == 0 || run.size % DisplayList::command_alignment != 0)
            return Error::from_string_literal("Display list command runs do not cover the command bytes");
        if (run.size > command_bytes.size() - next_offset)
            return Error::from_string_literal("Display list command runs exceed the command bytes");
        next_offset += run.size;
    }
    if (next_offset != command_bytes.size())
        return Error::from_string_literal("Display list command runs do not cover the command bytes");
    return {};
}

ErrorOr<void> validate_received_display_list_tape(ReadonlyBytes command_bytes, ReadonlySpan<DisplayListCommandRun> runs)
{
    size_t error_size = 0;
    auto const* error = Compositing::RustFFI::display_list_validate_tape(command_bytes.data(), command_bytes.size(), reinterpret_cast<u8 const*>(runs.data()), runs.size() * sizeof(DisplayListCommandRun), &error_size);
    if (error)
        return Error::from_string_view({ error, error_size });
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

void DisplayListPlayer::execute_command_bytes_into_surface(ReadonlyBytes command_bytes, Gfx::PaintingSurface& target_surface)
{
    TemporaryChange surface_change { m_surface, RefPtr<Gfx::PaintingSurface> { target_surface } };
    ScrollStateSnapshot scroll_state_snapshot;
    execute_command_bytes(command_bytes, scroll_state_snapshot);
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

// Builds the callbacks the Rust replay drives a player through.
struct DisplayListPlayer::ReplayCallbacks {
    static Compositing::RustFFI::FfiDisplayListReplayCallbacks for_player(DisplayListPlayer& player)
    {
        return {
            .context = &player,
            .canvas_matrix = [](void* context) -> Gfx::FloatMatrix4x4 { return static_cast<DisplayListPlayer*>(context)->canvas_matrix(); },
            .set_matrix = [](void* context, Gfx::FloatMatrix4x4 const* matrix) { static_cast<DisplayListPlayer*>(context)->set_matrix(*matrix); },
            .would_be_fully_clipped_by_painter = [](void* context, Gfx::IntRect rect) -> bool {
                return static_cast<DisplayListPlayer*>(context)->would_be_fully_clipped_by_painter(rect);
            },
            .push_clip = [](void* context, ReplayClip const* clip) { static_cast<DisplayListPlayer*>(context)->push_clip(*clip); },
            .push_clip_path = [](void* context, void const* path, Gfx::WindingRule winding_rule) { static_cast<DisplayListPlayer*>(context)->push_clip_path(*static_cast<Gfx::Path const*>(path), winding_rule); },
            .push_layer = [](void* context, ReplayLayer const* layer) { static_cast<DisplayListPlayer*>(context)->push_layer(*layer); },
            .push_mask = [](void* context, ReplayMask const* mask) { static_cast<DisplayListPlayer*>(context)->push_mask(*mask); },
            .pop_mask = [](void* context, ReplayMask const* mask, EffectNodeIndex effect) { static_cast<DisplayListPlayer*>(context)->pop_mask(*mask, effect); },
            .pop = [](void* context) { static_cast<DisplayListPlayer*>(context)->pop(); },
            .push_device_space_plane_clip = [](void* context, Gfx::FloatVector3 const* vertices, size_t vertex_count) {
                Gfx::Path path;
                path.move_to({ vertices[0].x(), vertices[0].y() });
                for (size_t i = 1; i < vertex_count; ++i)
                    path.line_to({ vertices[i].x(), vertices[i].y() });
                path.close();
                static_cast<DisplayListPlayer*>(context)->push_device_space_plane_clip(path); },
            .push_transform = [](void* context, Gfx::AffineTransform const* transform) { static_cast<DisplayListPlayer*>(context)->push_transform(*transform); },
            .push_clip_path_bytes = [](void* context, u8 const* path_bytes, size_t path_bytes_size, Gfx::WindingRule winding_rule) { static_cast<DisplayListPlayer*>(context)->push_clip_path(Gfx::Path::from_serialized_bytes({ path_bytes, path_bytes_size }), winding_rule); },
            .play_command = [](void* context, DisplayListCommandType command_type, u8 const* command, u8 const* payload, size_t payload_size) { static_cast<DisplayListPlayer*>(context)->play_command_bytes(command_type, command, { payload, payload_size }); },
        };
    }
};

void DisplayListPlayer::play_command_bytes(DisplayListCommandType command_type, u8 const* command, ReadonlyBytes payload)
{
    TemporaryChange current_command_payload_change { m_current_command_payload, payload };
    switch (command_type) {
#define PLAY_DISPLAY_LIST_COMMAND(command_type)                                                  \
    case DisplayListCommandType::command_type:                                                   \
        play_command(read_display_list_object<command_type>({ command, sizeof(command_type) })); \
        break;
        ENUMERATE_DISPLAY_LIST_COMMANDS(PLAY_DISPLAY_LIST_COMMAND)
#undef PLAY_DISPLAY_LIST_COMMAND
    }
}

void DisplayListPlayer::execute_command_bytes(ReadonlyBytes command_bytes, ScrollStateSnapshot const& scroll_state)
{
    auto callbacks = ReplayCallbacks::for_player(*this);
    auto scroll_offsets = scroll_state.device_offsets();
    Compositing::RustFFI::display_list_replay_records(command_bytes.data(), command_bytes.size(), scroll_offsets.data(), scroll_offsets.size(), &callbacks);
}

void DisplayListPlayer::declare_mask_content(EffectNodeIndex effect, ReadonlyBytes content)
{
    m_declared_mask_contents.set(effect.value(), content);
}

Optional<ReadonlyBytes> DisplayListPlayer::declared_mask_content(EffectNodeIndex effect) const
{
    return m_declared_mask_contents.get(effect.value());
}

void DisplayListPlayer::execute_impl(DisplayList const& display_list, ScrollStateSnapshot const& scroll_state)
{
    TemporaryChange active_scroll_state_change { m_active_scroll_state, &scroll_state };
    TemporaryChange declared_mask_contents_change { m_declared_mask_contents, HashMap<u32, ReadonlyBytes> {} };
    auto const& visual_context_tree = active_visual_context_tree();
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    VERIFY(m_surface);

    auto callbacks = ReplayCallbacks::for_player(*this);
    auto command_bytes = display_list.command_bytes();
    auto command_runs = display_list.command_runs();
    auto scroll_offsets = scroll_state.device_offsets();
    Compositing::RustFFI::display_list_replay(visual_context_tree.rust_handle(), display_list.replay_effect_clip_plan(visual_context_tree), command_bytes.data(), command_bytes.size(), command_runs.data(), command_runs.size(), scroll_offsets.data(), scroll_offsets.size(), &callbacks);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayList::AsyncScrollingMetadata const& metadata)
{
    TRY(encoder.encode(metadata.viewport_rect));
    TRY(encoder.encode(metadata.wheel_event_listener_state_generation));
    TRY(encoder.encode(metadata.has_blocking_wheel_event_listeners));
    TRY(encoder.encode(metadata.has_blocking_wheel_event_region_covering_viewport));
    TRY(encoder.encode(metadata.device_pixels_per_css_pixel));
    TRY(encoder.encode(metadata.keyboard_scroll_state));
    return {};
}

template<>
ErrorOr<Compositing::DisplayList::AsyncScrollingMetadata> decode(Decoder& decoder)
{
    return Compositing::DisplayList::AsyncScrollingMetadata {
        .viewport_rect = TRY(decoder.decode<Gfx::IntRect>()),
        .wheel_event_listener_state_generation = TRY(decoder.decode<u64>()),
        .has_blocking_wheel_event_listeners = TRY(decoder.decode<bool>()),
        .has_blocking_wheel_event_region_covering_viewport = TRY(decoder.decode<bool>()),
        .device_pixels_per_css_pixel = TRY(decoder.decode<double>()),
        .keyboard_scroll_state = TRY(decoder.decode<Compositing::KeyboardScrollState>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayList::Properties const& properties)
{
    TRY(encoder.encode(properties.id));
    TRY(encoder.encode(properties.compatible_visual_context_tree_structural_epoch));
    TRY(encoder.encode(properties.surface_clear_color));
    TRY(encoder.encode(properties.async_scrolling_metadata));
    return {};
}

template<>
ErrorOr<Compositing::DisplayList::Properties> decode(Decoder& decoder)
{
    return Compositing::DisplayList::Properties {
        .id = TRY(decoder.decode<u64>()),
        .compatible_visual_context_tree_structural_epoch = TRY(decoder.decode<u64>()),
        .surface_clear_color = TRY(decoder.decode<Optional<Gfx::Color>>()),
        .async_scrolling_metadata = TRY(decoder.decode<Optional<Compositing::DisplayList::AsyncScrollingMetadata>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayList const& display_list)
{
    TRY(encoder.encode(display_list.m_id));
    auto command_bytes = display_list.command_bytes();
    TRY(encoder.encode_size(command_bytes.size()));
    TRY(encoder.append(command_bytes.data(), command_bytes.size()));
    TRY(encoder.encode(display_list.m_compatible_visual_context_tree_structural_epoch));
    TRY(encoder.encode(display_list.m_surface_clear_color));
    TRY(encoder.encode(display_list.m_async_scrolling_metadata));
    // Trivially copyable records, so they travel as raw bytes like the command tape does.
    auto command_runs = display_list.command_runs();
    TRY(encoder.encode_size(command_runs.size()));
    if (!command_runs.is_empty())
        TRY(encoder.append(reinterpret_cast<u8 const*>(command_runs.data()), command_runs.size() * sizeof(Compositing::DisplayListCommandRun)));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, NonnullRefPtr<Compositing::DisplayList> const& display_list)
{
    return encoder.encode(*display_list);
}

template<>
ErrorOr<NonnullRefPtr<Compositing::DisplayList>> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<u64>());
    auto command_bytes = TRY(decoder.decode<ByteBuffer>());
    auto compatible_visual_context_tree_structural_epoch = TRY(decoder.decode<u64>());
    auto surface_clear_color = TRY(decoder.decode<Optional<Gfx::Color>>());
    auto async_scrolling_metadata = TRY(decoder.decode<Optional<Compositing::DisplayList::AsyncScrollingMetadata>>());
    auto command_run_count = TRY(decoder.decode_size());
    Vector<Compositing::DisplayListCommandRun> command_runs;
    TRY(command_runs.try_resize(command_run_count));
    if (!command_runs.is_empty())
        TRY(decoder.decode_into(Bytes { reinterpret_cast<u8*>(command_runs.data()), command_runs.size() * sizeof(Compositing::DisplayListCommandRun) }));
    TRY(Compositing::validate_received_display_list_tape(command_bytes, command_runs));
    return adopt_ref(*new Compositing::DisplayList(compatible_visual_context_tree_structural_epoch, id, move(command_bytes), move(command_runs), surface_clear_color, move(async_scrolling_metadata)));
}

}
