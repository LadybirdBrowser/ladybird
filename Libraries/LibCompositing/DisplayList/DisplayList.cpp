/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Checked.h>
#include <AK/NumericLimits.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/RustFFI.h>
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

void DisplayList::replay(AccumulatedVisualContextTree const& visual_context_tree, ScrollStateSnapshot const& scroll_state, RustFFI::FfiDisplayListReplayCallbacks const& callbacks) const
{
    VERIFY(m_compatible_visual_context_tree_structural_epoch == visual_context_tree.structural_epoch());
    auto bytes = command_bytes();
    auto runs = command_runs();
    auto scroll_offsets = scroll_state.device_offsets();
    RustFFI::display_list_replay(visual_context_tree.rust_handle(), replay_effect_clip_plan(visual_context_tree), bytes.data(), bytes.size(), runs.data(), runs.size(), scroll_offsets.data(), scroll_offsets.size(), &callbacks);
}

void DisplayList::replay_records(ReadonlyBytes records, ScrollStateSnapshot const& scroll_state, RustFFI::FfiDisplayListReplayCallbacks const& callbacks)
{
    auto scroll_offsets = scroll_state.device_offsets();
    RustFFI::display_list_replay_records(records.data(), records.size(), scroll_offsets.data(), scroll_offsets.size(), &callbacks);
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
