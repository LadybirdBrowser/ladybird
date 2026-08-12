/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <AK/Math.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/AudioWorkletPipe.h>

namespace Web::WebAudio::Rendering {

// The renderer produces a device buffer's worth of quanta in one burst, so lookahead must cover a full burst plus
// scheduling margin. At the 100 ms, 48 kHz defaults this is 38 quanta, a prime level of 48, and capacity 64.
AudioWorkletPipe::Config::RingSizing AudioWorkletPipe::Config::ring_sizing_for_device_latency(u32 device_latency_ms, float sample_rate, size_t quantum_size)
{
    VERIFY(sample_rate > 0);
    VERIFY(quantum_size > 0);
    auto burst_quanta = static_cast<size_t>(AK::ceil(device_latency_ms / 1000.0 * sample_rate / quantum_size));
    auto prime_level = ceil_div(burst_quanta * 5, static_cast<size_t>(4));
    size_t ring_capacity = 1;
    while (ring_capacity < prime_level + max(burst_quanta / 4, static_cast<size_t>(1)))
        ring_capacity *= 2;
    return { .ring_capacity = ring_capacity, .prime_level = prime_level };
}

NonnullRefPtr<AudioWorkletPipe> AudioWorkletPipe::create(Config config, Core::EventLoop& main_thread_event_loop)
{
    VERIFY(config.quantum_size > 0);
    VERIFY(config.input_channel_capacity.size() == config.input_count);
    VERIFY(config.output_channel_capacity.size() == config.output_count);
    VERIFY(config.param_is_a_rate.size() == config.param_count);
    VERIFY(config.ring_capacity > 0 && popcount(config.ring_capacity) == 1);
    VERIFY(config.prime_level <= config.ring_capacity);
    return adopt_ref(*new AudioWorkletPipe(move(config), main_thread_event_loop));
}

AudioWorkletPipe::AudioWorkletPipe(Config config, Core::EventLoop& main_thread_event_loop)
    : m_config(move(config))
    , m_ring_index_mask(m_config.ring_capacity - 1)
    , m_main_thread_event_loop(main_thread_event_loop)
{
    auto channel_bytes = m_config.quantum_size * sizeof(float);

    size_t offset = slot_channel_counts_offset + m_config.input_count * sizeof(u32);
    m_input_samples_offset.ensure_capacity(m_config.input_count);
    for (u32 input = 0; input < m_config.input_count; ++input) {
        m_input_samples_offset.unchecked_append(offset);
        offset += m_config.input_channel_capacity[input] * channel_bytes;
    }
    m_param_block_offset.ensure_capacity(m_config.param_count);
    m_param_block_length.ensure_capacity(m_config.param_count);
    for (u32 param = 0; param < m_config.param_count; ++param) {
        m_param_block_offset.unchecked_append(offset);
        size_t block_length = m_config.param_is_a_rate[param] ? m_config.quantum_size : 1;
        m_param_block_length.unchecked_append(block_length);
        offset += block_length * sizeof(float);
    }
    m_input_slot_stride = round_up_to_power_of_two(offset, slot_alignment);

    offset = slot_channel_counts_offset + m_config.output_count * sizeof(u32);
    m_output_samples_offset.ensure_capacity(m_config.output_count);
    for (u32 output = 0; output < m_config.output_count; ++output) {
        m_output_samples_offset.unchecked_append(offset);
        offset += m_config.output_channel_capacity[output] * channel_bytes;
    }
    m_output_slot_stride = round_up_to_power_of_two(offset, slot_alignment);

    m_output_region_offset = m_config.ring_capacity * m_input_slot_stride;
    m_arena = MUST(FixedArray<u8>::create(m_output_region_offset + m_config.ring_capacity * m_output_slot_stride));
}

bool AudioWorkletPipe::try_push_input(Function<void(InputSlotWriter&)> const& fill)
{
    auto tail = m_input_ring.tail.load(AK::MemoryOrder::memory_order_relaxed);
    if (tail - m_input_ring.head.load(AK::MemoryOrder::memory_order_acquire) == m_config.ring_capacity) {
        m_dropped_input_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        return false;
    }
    auto* slot = input_slot(tail);
    *reinterpret_cast<u32*>(slot + slot_flags_offset) = 0;
    InputSlotWriter writer { *this, slot };
    fill(writer);
    m_input_ring.tail.store(tail + 1, AK::MemoryOrder::memory_order_release);
    return true;
}

bool AudioWorkletPipe::try_pop_input(Function<void(InputSlotReader const&)> const& read)
{
    auto head = m_input_ring.head.load(AK::MemoryOrder::memory_order_relaxed);
    if (head == m_input_ring.tail.load(AK::MemoryOrder::memory_order_acquire))
        return false;
    InputSlotReader reader { *this, input_slot(head) };
    read(reader);
    m_input_ring.head.store(head + 1, AK::MemoryOrder::memory_order_release);
    return true;
}

bool AudioWorkletPipe::try_push_output(Function<void(OutputSlotWriter&)> const& fill)
{
    auto tail = m_output_ring.tail.load(AK::MemoryOrder::memory_order_relaxed);
    if (tail - m_output_ring.head.load(AK::MemoryOrder::memory_order_acquire) == m_config.ring_capacity)
        return false;
    auto* slot = output_slot(tail);
    *reinterpret_cast<u32*>(slot + slot_flags_offset) = 0;
    OutputSlotWriter writer { *this, slot };
    fill(writer);
    m_output_ring.tail.store(tail + 1, AK::MemoryOrder::memory_order_release);
    return true;
}

bool AudioWorkletPipe::try_pop_output(Function<void(OutputSlotReader const&)> const& read)
{
    auto head = m_output_ring.head.load(AK::MemoryOrder::memory_order_relaxed);
    if (head == m_output_ring.tail.load(AK::MemoryOrder::memory_order_acquire)) {
        m_underrun_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        return false;
    }
    OutputSlotReader reader { *this, output_slot(head) };
    read(reader);
    m_output_ring.head.store(head + 1, AK::MemoryOrder::memory_order_release);
    return true;
}

size_t AudioWorkletPipe::output_occupancy() const
{
    return m_output_ring.tail.load(AK::MemoryOrder::memory_order_acquire) - m_output_ring.head.load(AK::MemoryOrder::memory_order_relaxed);
}

void AudioWorkletPipe::discard_outputs_down_to(size_t target_occupancy)
{
    // A consumer-side pop-without-read: advancing the head releases the discarded slots back to the producer. New
    // pushes can only move the tail further along, so the snapshot below never overshoots.
    auto head = m_output_ring.head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_output_ring.tail.load(AK::MemoryOrder::memory_order_acquire);
    if (tail - head <= target_occupancy)
        return;
    m_output_ring.head.store(tail - target_occupancy, AK::MemoryOrder::memory_order_release);
}

void AudioWorkletPipe::prime_outputs_with_silence(size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        bool pushed = try_push_output([&](OutputSlotWriter& slot) {
            slot.start_frame() = 0;
            slot.set_processor_active(true);
            for (u32 output = 0; output < m_config.output_count; ++output) {
                auto channel_count = m_config.output_channel_count_matches_input ? 1u : m_config.output_channel_capacity[output];
                slot.actual_channel_count(output) = channel_count;
                for (u32 channel = 0; channel < channel_count; ++channel)
                    slot.output_channel(output, channel).fill(0);
            }
        });
        VERIFY(pushed);
    }
}

void AudioWorkletPipe::request_wakeup()
{
    // Coalesce a device burst into one main-thread pump instead of posting once per quantum.
    if (m_pump_scheduled.exchange(true, AK::MemoryOrder::memory_order_acq_rel))
        return;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this)] {
        if (self->state() == State::ShutDown)
            return;
        if (self->m_pump_callback)
            self->m_pump_callback();
    });
}

void AudioWorkletPipe::clear_wakeup_flag()
{
    // A read-modify-write rather than a plain store: it synchronizes with the audio thread's exchange in
    // request_wakeup(), so whenever that exchange elides a wakeup (returns true), everything the audio thread
    // pushed beforehand is visible to this pump run's drain.
    m_pump_scheduled.exchange(false, AK::MemoryOrder::memory_order_acq_rel);
}

void AudioWorkletPipe::set_pump_callback(Function<void()> pump_callback)
{
    m_pump_callback = move(pump_callback);
}

void AudioWorkletPipe::clear_pump_callback()
{
    m_pump_callback = nullptr;
}

}
