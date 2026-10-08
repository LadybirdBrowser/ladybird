/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Checked.h>
#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/RustFFI.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

static Atomic<u64> s_next_id { 1 };

static u64 next_id()
{
    return s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

DisplayList::DisplayList(u64 compatible_visual_context_tree_structural_epoch, u64 id, void const* storage, Optional<Gfx::Color> surface_clear_color, Optional<AsyncScrollingMetadata> async_scrolling_metadata)
    : m_compatible_visual_context_tree_structural_epoch(compatible_visual_context_tree_structural_epoch)
    , m_id(id)
    , m_storage(storage)
    , m_surface_clear_color(surface_clear_color)
    , m_async_scrolling_metadata(move(async_scrolling_metadata))
{
    VERIFY(m_storage);
    auto view = RustFFI::display_list_storage_view(m_storage);
    m_command_bytes = { view.bytes, view.byte_count };
    m_command_runs = { view.command_runs, view.command_run_count };
}

DisplayList::~DisplayList()
{
    RustFFI::display_list_storage_release(m_storage);
}

NonnullRefPtr<DisplayList> DisplayList::create(AccumulatedVisualContextTree const& visual_context_tree)
{
    return adopt_ref(*new DisplayList(visual_context_tree.structural_epoch(), next_id(), RustFFI::display_list_storage_create_empty(), {}, {}));
}

NonnullRefPtr<DisplayList> DisplayList::adopt_rust_command_storage(AccumulatedVisualContextTree const& visual_context_tree, void const* recorded)
{
    VERIFY(recorded);
    return adopt_ref(*new DisplayList(visual_context_tree.structural_epoch(), next_id(), RustFFI::display_list_storage_adopt_recorded(recorded), {}, {}));
}

NonnullRefPtr<DisplayList> DisplayList::share_rust_command_storage(AccumulatedVisualContextTree const& visual_context_tree, void const* recorded)
{
    VERIFY(recorded);
    return adopt_rust_command_storage(visual_context_tree, RustFFI::display_list_retain_command_storage(recorded));
}

NonnullRefPtr<DisplayList> DisplayList::adopt_received_storage(Properties properties, void const* storage)
{
    return adopt_ref(*new DisplayList(properties.compatible_visual_context_tree_structural_epoch, properties.id, storage, properties.surface_clear_color, move(properties.async_scrolling_metadata)));
}

// Copies a tape and run table that came from another process into Rust memory through `fill`, then checks them and
// returns the storage that holds them. Every later read of the tape is then sound.
static ErrorOr<void const*> receive_display_list_tape(u64 tape_size, u64 run_count, Function<ErrorOr<void>(Bytes tape, Bytes run_bytes)> const& fill)
{
    auto pending = RustFFI::display_list_storage_begin(tape_size, run_count);
    if (!pending.pending)
        return Error::from_string_literal("Display list is too large to receive");
    ArmedScopeGuard abandon_pending = [&] { RustFFI::display_list_storage_abandon(pending.pending); };
    TRY(fill(Bytes { pending.tape, tape_size }, Bytes { pending.run_bytes, run_count * sizeof(DisplayListCommandRun) }));
    abandon_pending.disarm();
    u8 const* error = nullptr;
    size_t error_size = 0;
    auto const* storage = RustFFI::display_list_storage_finish(pending.pending, &error, &error_size);
    if (!storage)
        return Error::from_string_view({ error, error_size });
    return storage;
}

DisplayList::ReferencedResourceIds DisplayList::referenced_resource_ids() const
{
    auto ids = RustFFI::display_list_storage_referenced_resource_ids(m_storage);
    return {
        .fonts = { ids.font_ids, ids.font_id_count },
        .image_frames = { ids.image_frame_ids, ids.image_frame_id_count },
        .video_sinks = { ids.video_sink_ids, ids.video_sink_id_count },
        .display_lists = { ids.display_list_ids, ids.display_list_id_count },
    };
}

bool DisplayList::requires_direct_replay_without_nested_lists(AccumulatedVisualContextTree const& visual_context_tree) const
{
    VERIFY(m_compatible_visual_context_tree_structural_epoch == visual_context_tree.structural_epoch());
    return RustFFI::display_list_storage_requires_direct_replay_without_nested_lists(m_storage, visual_context_tree.rust_handle());
}

void DisplayList::for_each_compositor_metadata(Function<void(ContextRef, DisplayListCommandType, ReadonlyBytes)> const& callback) const
{
    RustFFI::display_list_storage_for_each_compositor_metadata(m_storage, const_cast<void*>(static_cast<void const*>(&callback)), [](void* context, ContextRef run_context, DisplayListCommandType command_type, u8 const* payload, size_t payload_size) {
        (*static_cast<Function<void(ContextRef, DisplayListCommandType, ReadonlyBytes)> const*>(context))(run_context, command_type, { payload, payload_size });
    });
}

template<typename Command>
static void for_each_indexed_record(void const* storage, RustFFI::FfiIndexedRecordKind kind, Function<void(ContextRef, Optional<Gfx::IntRect>, Command const&)> const& callback)
{
    RustFFI::display_list_storage_for_each_indexed_record(storage, kind, const_cast<void*>(static_cast<void const*>(&callback)), [](void* context, ContextRef run_context, bool has_bounding_rect, Gfx::IntRect bounding_rect, u8 const* payload, size_t payload_size) {
        auto command = read_display_list_object<Command>({ payload, payload_size });
        auto rect = has_bounding_rect ? Optional<Gfx::IntRect> { bounding_rect } : Optional<Gfx::IntRect> {};
        (*static_cast<Function<void(ContextRef, Optional<Gfx::IntRect>, Command const&)> const*>(context))(run_context, rect, command);
    });
}

void DisplayList::for_each_drawn_canvas(Function<void(ContextRef, Optional<Gfx::IntRect>, DrawCanvas const&)> const& callback) const
{
    for_each_indexed_record<DrawCanvas>(m_storage, RustFFI::FfiIndexedRecordKind::DrawnCanvas, callback);
}

void DisplayList::for_each_caret(Function<void(ContextRef, Optional<Gfx::IntRect>, PaintCaret const&)> const& callback) const
{
    for_each_indexed_record<PaintCaret>(m_storage, RustFFI::FfiIndexedRecordKind::Caret, callback);
}

void DisplayList::replay(AccumulatedVisualContextTree const& visual_context_tree, ScrollStateSnapshot const& scroll_state, RustFFI::FfiDisplayListReplayCallbacks const& callbacks) const
{
    VERIFY(m_compatible_visual_context_tree_structural_epoch == visual_context_tree.structural_epoch());
    auto scroll_offsets = scroll_state.device_offsets();
    RustFFI::display_list_replay(visual_context_tree.rust_handle(), m_storage, scroll_offsets.data(), scroll_offsets.size(), &callbacks);
}

void DisplayList::replay_records(ReadonlyBytes records, ScrollStateSnapshot const& scroll_state, RustFFI::FfiDisplayListReplayCallbacks const& callbacks)
{
    auto scroll_offsets = scroll_state.device_offsets();
    RustFFI::display_list_replay_records(records.data(), records.size(), scroll_offsets.data(), scroll_offsets.size(), &callbacks);
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
    if (layout->total_size > 0) {
        if (!shared_tape_buffer.is_valid())
            return Error::from_string_literal("Display list arrived without its shared buffer");
        TRY(shared_tape_buffer.validate_backing_size());
        if (layout->total_size > shared_tape_buffer.size())
            return Error::from_string_literal("Display list sizes exceed its shared buffer");
        // Every run holds at least one command, so more runs than headers cannot describe this tape.
        if (run_count > tape_size / sizeof(DisplayListCommandHeader))
            return Error::from_string_literal("Display list run table is larger than its tape allows");
    }
    // The sender can still write to the buffer, so the tape and its runs are copied out before they are checked.
    auto const* storage = TRY(receive_display_list_tape(tape_size, run_count, [&](Bytes tape, Bytes run_bytes) -> ErrorOr<void> {
        if (layout->total_size == 0)
            return {};
        auto source = shared_tape_buffer.bytes();
        source.slice(0, tape_size).copy_to(tape);
        source.slice(layout->runs_offset, run_bytes.size()).copy_to(run_bytes);
        return {};
    }));
    return adopt_received_storage(move(properties), storage);
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
    if (!RustFFI::display_list_references_only_live_visual_context_nodes(visual_context_tree.rust_handle(), display_list.rust_handle()))
        return Error::from_string_literal("Display list references a visual context node that is not live");
    return {};
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
    TRY(encoder.encode(display_list.properties()));
    auto command_bytes = display_list.command_bytes();
    auto command_runs = display_list.command_runs();
    TRY(encoder.encode_size(command_bytes.size()));
    TRY(encoder.encode_size(command_runs.size()));
    TRY(encoder.append(command_bytes.data(), command_bytes.size()));
    // Trivially copyable records, so they travel as raw bytes like the command tape does.
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
    auto properties = TRY(decoder.decode<Compositing::DisplayList::Properties>());
    auto tape_size = TRY(decoder.decode_size());
    auto run_count = TRY(decoder.decode_size());
    auto const* storage = TRY(Compositing::receive_display_list_tape(tape_size, run_count, [&](Bytes tape, Bytes run_bytes) -> ErrorOr<void> {
        TRY(decoder.decode_into(tape));
        TRY(decoder.decode_into(run_bytes));
        return {};
    }));
    return Compositing::DisplayList::adopt_received_storage(move(properties), storage);
}

}
