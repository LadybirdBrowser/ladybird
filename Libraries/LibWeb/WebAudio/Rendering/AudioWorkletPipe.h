/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
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
// preallocated arena; underruns render silence and a full input ring drops quanta. Each ring uses acquire/release
// head-tail discipline, and the slot stride is configured at run time.
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
struct AudioWorkletPipeCallbacks;

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

    ~AudioWorkletPipe();
    void const* rust_pipe() const { return m_pipe; }

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

    State state() const;
    void set_state(State);

    // A single atomic store, so this is safe to call from a GC finalizer on either thread.
    void request_shutdown() { set_state(State::ShutDown); }

    u64 underrun_count() const;
    u64 dropped_input_count() const;

private:
    AudioWorkletPipe(Config, Core::EventLoop& main_thread_event_loop);

    Config m_config;
    NonnullRefPtr<AudioWorkletPipeCallbacks> m_callbacks;
    void const* m_pipe { nullptr };
};

}
