/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/FixedArray.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Platform.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio::Rendering {

// A lock-free bridge between the realtime render node and the main-thread processor. Two SPSC rings share a
// preallocated arena; underruns render silence and a full input ring drops quanta. Its acquire/release head-tail
// discipline follows AK/SingleProducerCircularQueue.h, but the slot stride is configured at run time.
//
// Per-slot layout; strides are rounded up to 64 bytes so slots start cache-line-aligned relative to the arena:
//
//   Input slot                                            Output slot
//     u64 start_frame                                       u64 start_frame
//     u32 flags (bit 0: gap before this quantum)            u32 flags (bit 0: processor active; bit 1: reserved
//     u32 actual_channel_count[input_count]                     for a per-quantum processing-error signal)
//     f32 samples[input_channel_capacity[0]][quantum_size]  u32 actual_channel_count[output_count]
//       ... one sample block per input ...                  f32 samples[output_channel_capacity[0]][quantum_size]
//     f32 param_block[param_count]: quantum_size samples      ... one sample block per output ...
//         each for a-rate parameters, 1 for k-rate
//
// Slots are accessed in place through the writer/reader views below, which are only valid inside the callback they
// are passed to. A slot's unwritten regions keep stale data from the previous trip around the ring, so a writer
// must set the channel counts it wants a reader to trust; the flags are cleared on every push.
class WEB_API AudioWorkletPipe final : public AtomicRefCounted<AudioWorkletPipe> {
public:
    struct Config {
        size_t quantum_size { 0 };
        float sample_rate { 0 };
        u32 input_count { 0 };
        u32 output_count { 0 };
        Vector<u32> input_channel_capacity;  // Per input: the largest channel count a slot can carry.
        Vector<u32> output_channel_capacity; // Per output: the largest channel count a slot can carry.
        bool output_channel_count_matches_input { false };
        u32 param_count { 0 };
        Vector<bool> param_is_a_rate; // Per parameter: a-rate blocks carry quantum_size samples, k-rate blocks one.
        size_t ring_capacity { 0 };   // Slots per ring; must be a power of two.
        size_t prime_level { 0 };     // Silent output quanta pushed before rendering starts; at most ring_capacity.

        struct RingSizing {
            size_t ring_capacity { 0 };
            size_t prime_level { 0 };
        };
        static WEB_API RingSizing ring_sizing_for_device_latency(u32 device_latency_ms, float sample_rate, size_t quantum_size);
    };

    enum class State : u8 {
        Running,
        Failed,
        ShutDown,
    };

    class InputSlotWriter {
    public:
        u64& start_frame();
        void set_had_gap(bool);
        u32& actual_channel_count(size_t input);
        Span<float> input_channel(size_t input, size_t channel);
        Span<float> param_block(size_t param);

    private:
        friend class AudioWorkletPipe;
        InputSlotWriter(AudioWorkletPipe const& pipe, u8* slot)
            : m_pipe(pipe)
            , m_slot(slot)
        {
        }

        AudioWorkletPipe const& m_pipe;
        u8* m_slot { nullptr };
    };

    class InputSlotReader {
    public:
        u64 start_frame() const;
        bool had_gap() const;
        u32 actual_channel_count(size_t input) const;
        ReadonlySpan<float> input_channel(size_t input, size_t channel) const;
        ReadonlySpan<float> param_block(size_t param) const;

    private:
        friend class AudioWorkletPipe;
        InputSlotReader(AudioWorkletPipe const& pipe, u8 const* slot)
            : m_pipe(pipe)
            , m_slot(slot)
        {
        }

        AudioWorkletPipe const& m_pipe;
        u8 const* m_slot { nullptr };
    };

    class OutputSlotWriter {
    public:
        u64& start_frame();
        void set_processor_active(bool);
        u32& actual_channel_count(size_t output);
        Span<float> output_channel(size_t output, size_t channel);

    private:
        friend class AudioWorkletPipe;
        OutputSlotWriter(AudioWorkletPipe const& pipe, u8* slot)
            : m_pipe(pipe)
            , m_slot(slot)
        {
        }

        AudioWorkletPipe const& m_pipe;
        u8* m_slot { nullptr };
    };

    class OutputSlotReader {
    public:
        u64 start_frame() const;
        bool processor_active() const;
        u32 actual_channel_count(size_t output) const;
        ReadonlySpan<float> output_channel(size_t output, size_t channel) const;

    private:
        friend class AudioWorkletPipe;
        OutputSlotReader(AudioWorkletPipe const& pipe, u8 const* slot)
            : m_pipe(pipe)
            , m_slot(slot)
        {
        }

        AudioWorkletPipe const& m_pipe;
        u8 const* m_slot { nullptr };
    };

    static NonnullRefPtr<AudioWorkletPipe> create(Config, Core::EventLoop& main_thread_event_loop);

    Config const& config() const { return m_config; }

    // Audio-thread side (input producer, output consumer):

    // Fills the next input slot in place. Returns false, and counts a dropped quantum, when the ring is full; the
    // caller should then mark the next successful push with set_had_gap() so the processor can tell that quanta
    // went missing.
    bool try_push_input(Function<void(InputSlotWriter&)> const& fill);

    // Reads the oldest processed quantum. Returns false, and counts an underrun, when none is ready; the caller
    // then outputs silence, never blocking the audio callback.
    bool try_pop_output(Function<void(OutputSlotReader const&)> const& read);

    size_t output_occupancy() const;

    // Drops the oldest processed quanta until at most target_occupancy remain. After a main-thread stall the pump
    // catches up and refills the output ring past the prime level; clamping back down keeps the pipe's added
    // latency from ratcheting up permanently.
    void discard_outputs_down_to(size_t target_occupancy);

    // Schedules the pump on the main thread's event loop. Coalesced: only the first request after
    // clear_wakeup_flag() posts an event, so a device burst of several dozen quanta causes one pump run.
    void request_wakeup();

    // Main-thread side (input consumer, output producer):

    bool try_pop_input(Function<void(InputSlotReader const&)> const& read);
    bool try_push_output(Function<void(OutputSlotWriter&)> const& fill);

    // Pushes `count` silent, processor-active output quanta. Called on the control thread before the pipe is handed
    // to the render node, so the audio thread has a burst worth of lookahead before the first processed quantum.
    void prime_outputs_with_silence(size_t count);

    // The pump calls this before draining the input ring, so a burst that lands while the pump is running schedules
    // another run instead of being lost.
    void clear_wakeup_flag();

    // Accessed only on the control thread, including by deferred wakeups.
    void set_pump_callback(Function<void()>);
    void clear_pump_callback();

    // Either thread:

    State state() const { return static_cast<State>(m_state.load(AK::MemoryOrder::memory_order_acquire)); }
    void set_state(State state) { m_state.store(to_underlying(state), AK::MemoryOrder::memory_order_release); }

    // A single atomic store, so this is safe to call from a GC finalizer on either thread.
    void request_shutdown() { set_state(State::ShutDown); }

    u64 underrun_count() const { return m_underrun_count.load(AK::MemoryOrder::memory_order_relaxed); }
    u64 dropped_input_count() const { return m_dropped_input_count.load(AK::MemoryOrder::memory_order_relaxed); }

private:
    AudioWorkletPipe(Config, Core::EventLoop& main_thread_event_loop);

    static constexpr size_t slot_flags_offset = sizeof(u64);
    static constexpr size_t slot_channel_counts_offset = sizeof(u64) + sizeof(u32);
    static constexpr size_t slot_alignment = 64;
    static constexpr u32 input_slot_flag_had_gap = 1u << 0;
    static constexpr u32 output_slot_flag_processor_active = 1u << 0;

    u8* input_slot(size_t counter) { return m_arena.data() + (counter & m_ring_index_mask) * m_input_slot_stride; }
    u8* output_slot(size_t counter) { return m_arena.data() + m_output_region_offset + (counter & m_ring_index_mask) * m_output_slot_stride; }

    struct Ring {
        AK_CACHE_ALIGNED Atomic<size_t> tail { 0 }; // Producer-owned: the next slot to write.
        AK_CACHE_ALIGNED Atomic<size_t> head { 0 }; // Consumer-owned: the next slot to read.
    };

    Config m_config;
    size_t m_ring_index_mask { 0 };

    size_t m_input_slot_stride { 0 };
    size_t m_output_slot_stride { 0 };
    size_t m_output_region_offset { 0 };
    Vector<size_t> m_input_samples_offset;
    Vector<size_t> m_output_samples_offset;
    Vector<size_t> m_param_block_offset;
    Vector<size_t> m_param_block_length;
    FixedArray<u8> m_arena;

    Ring m_input_ring;
    Ring m_output_ring;

    Core::EventLoop& m_main_thread_event_loop;
    Function<void()> m_pump_callback;
    Atomic<bool> m_pump_scheduled { false };

    Atomic<u8> m_state { to_underlying(State::Running) };
    Atomic<u64> m_underrun_count { 0 };
    Atomic<u64> m_dropped_input_count { 0 };
};

inline u64& AudioWorkletPipe::InputSlotWriter::start_frame()
{
    return *reinterpret_cast<u64*>(m_slot);
}

inline void AudioWorkletPipe::InputSlotWriter::set_had_gap(bool had_gap)
{
    auto& flags = *reinterpret_cast<u32*>(m_slot + slot_flags_offset);
    if (had_gap)
        flags |= input_slot_flag_had_gap;
    else
        flags &= ~input_slot_flag_had_gap;
}

inline u32& AudioWorkletPipe::InputSlotWriter::actual_channel_count(size_t input)
{
    VERIFY(input < m_pipe.m_config.input_count);
    return *reinterpret_cast<u32*>(m_slot + slot_channel_counts_offset + input * sizeof(u32));
}

inline Span<float> AudioWorkletPipe::InputSlotWriter::input_channel(size_t input, size_t channel)
{
    VERIFY(input < m_pipe.m_config.input_count);
    VERIFY(channel < m_pipe.m_config.input_channel_capacity[input]);
    auto* samples = reinterpret_cast<float*>(m_slot + m_pipe.m_input_samples_offset[input]) + channel * m_pipe.m_config.quantum_size;
    return { samples, m_pipe.m_config.quantum_size };
}

inline Span<float> AudioWorkletPipe::InputSlotWriter::param_block(size_t param)
{
    VERIFY(param < m_pipe.m_config.param_count);
    return { reinterpret_cast<float*>(m_slot + m_pipe.m_param_block_offset[param]), m_pipe.m_param_block_length[param] };
}

inline u64 AudioWorkletPipe::InputSlotReader::start_frame() const
{
    return *reinterpret_cast<u64 const*>(m_slot);
}

inline bool AudioWorkletPipe::InputSlotReader::had_gap() const
{
    return (*reinterpret_cast<u32 const*>(m_slot + slot_flags_offset) & input_slot_flag_had_gap) != 0;
}

inline u32 AudioWorkletPipe::InputSlotReader::actual_channel_count(size_t input) const
{
    VERIFY(input < m_pipe.m_config.input_count);
    return *reinterpret_cast<u32 const*>(m_slot + slot_channel_counts_offset + input * sizeof(u32));
}

inline ReadonlySpan<float> AudioWorkletPipe::InputSlotReader::input_channel(size_t input, size_t channel) const
{
    VERIFY(input < m_pipe.m_config.input_count);
    VERIFY(channel < m_pipe.m_config.input_channel_capacity[input]);
    auto const* samples = reinterpret_cast<float const*>(m_slot + m_pipe.m_input_samples_offset[input]) + channel * m_pipe.m_config.quantum_size;
    return { samples, m_pipe.m_config.quantum_size };
}

inline ReadonlySpan<float> AudioWorkletPipe::InputSlotReader::param_block(size_t param) const
{
    VERIFY(param < m_pipe.m_config.param_count);
    return { reinterpret_cast<float const*>(m_slot + m_pipe.m_param_block_offset[param]), m_pipe.m_param_block_length[param] };
}

inline u64& AudioWorkletPipe::OutputSlotWriter::start_frame()
{
    return *reinterpret_cast<u64*>(m_slot);
}

inline void AudioWorkletPipe::OutputSlotWriter::set_processor_active(bool processor_active)
{
    auto& flags = *reinterpret_cast<u32*>(m_slot + slot_flags_offset);
    if (processor_active)
        flags |= output_slot_flag_processor_active;
    else
        flags &= ~output_slot_flag_processor_active;
}

inline u32& AudioWorkletPipe::OutputSlotWriter::actual_channel_count(size_t output)
{
    VERIFY(output < m_pipe.m_config.output_count);
    return *reinterpret_cast<u32*>(m_slot + slot_channel_counts_offset + output * sizeof(u32));
}

inline Span<float> AudioWorkletPipe::OutputSlotWriter::output_channel(size_t output, size_t channel)
{
    VERIFY(output < m_pipe.m_config.output_count);
    VERIFY(channel < m_pipe.m_config.output_channel_capacity[output]);
    auto* samples = reinterpret_cast<float*>(m_slot + m_pipe.m_output_samples_offset[output]) + channel * m_pipe.m_config.quantum_size;
    return { samples, m_pipe.m_config.quantum_size };
}

inline u64 AudioWorkletPipe::OutputSlotReader::start_frame() const
{
    return *reinterpret_cast<u64 const*>(m_slot);
}

inline bool AudioWorkletPipe::OutputSlotReader::processor_active() const
{
    return (*reinterpret_cast<u32 const*>(m_slot + slot_flags_offset) & output_slot_flag_processor_active) != 0;
}

inline u32 AudioWorkletPipe::OutputSlotReader::actual_channel_count(size_t output) const
{
    VERIFY(output < m_pipe.m_config.output_count);
    return *reinterpret_cast<u32 const*>(m_slot + slot_channel_counts_offset + output * sizeof(u32));
}

inline ReadonlySpan<float> AudioWorkletPipe::OutputSlotReader::output_channel(size_t output, size_t channel) const
{
    VERIFY(output < m_pipe.m_config.output_count);
    VERIFY(channel < m_pipe.m_config.output_channel_capacity[output]);
    auto const* samples = reinterpret_cast<float const*>(m_slot + m_pipe.m_output_samples_offset[output]) + channel * m_pipe.m_config.quantum_size;
    return { samples, m_pipe.m_config.quantum_size };
}

}
