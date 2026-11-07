/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/AudioWorkletPipe.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

struct AudioWorkletPipeCallbacks : AtomicRefCounted<AudioWorkletPipeCallbacks> {
    explicit AudioWorkletPipeCallbacks(Core::EventLoop& loop)
        : event_loop(loop)
    {
    }
    Core::EventLoop& event_loop;
    Function<void()> pump_callback;
};

NonnullRefPtr<AudioWorkletPipe> AudioWorkletPipe::create(Config config, Core::EventLoop& event_loop)
{
    return adopt_ref(*new AudioWorkletPipe(move(config), event_loop));
}

AudioWorkletPipe::AudioWorkletPipe(Config config, Core::EventLoop& event_loop)
    : m_config(move(config))
    , m_callbacks(adopt_ref(*new AudioWorkletPipeCallbacks(event_loop)))
{
    RustFFI::PipeCallbacks callbacks {
        { m_callbacks.ptr(), [](void* service) { static_cast<AudioWorkletPipeCallbacks*>(service)->ref(); }, [](void* service) { static_cast<AudioWorkletPipeCallbacks*>(service)->unref(); } },
        [](void* service, void* task, void (*invoke)(void*)) {
            static_cast<AudioWorkletPipeCallbacks*>(service)->event_loop.deferred_invoke([task, invoke] { invoke(task); });
        },
        [](void* service) {
            auto& callbacks = *static_cast<AudioWorkletPipeCallbacks*>(service);
            if (callbacks.pump_callback)
                callbacks.pump_callback();
        },
        [](void*, size_t, u64) { return false; },
    };
    RustFFI::WorkletPipeConfig native_config { m_config.quantum_size, m_config.input_count, m_config.output_count, m_config.input_channel_capacity.span(), m_config.output_channel_capacity.span(), m_config.output_channel_count_matches_input, m_config.param_count, m_config.param_is_a_rate.span(), m_config.ring_capacity, m_config.prime_level };
    m_pipe = RustFFI::web_audio_worklet_pipe_create(native_config, callbacks);
}

AudioWorkletPipe::~AudioWorkletPipe() { RustFFI::web_audio_worklet_pipe_destroy(m_pipe); }
AudioWorkletPipe::Config::RingSizing AudioWorkletPipe::Config::ring_sizing_for_device_latency(u32 latency, float sample_rate, size_t quantum_size)
{
    RingSizing sizing;
    RustFFI::web_audio_worklet_pipe_ring_sizing(latency, sample_rate, quantum_size, &sizing.ring_capacity, &sizing.prime_level);
    return sizing;
}

bool AudioWorkletPipe::try_push_input(Function<void(InputSlotWriter&)> const& callback)
{
    struct Context {
        AudioWorkletPipe& pipe;
        Function<void(InputSlotWriter&)> const& callback;
    } context { *this, callback };
    return RustFFI::web_audio_worklet_pipe_transfer(m_pipe, false, true, &context, [](void* pointer, u8* slot) {
        auto& context = *static_cast<Context*>(pointer);
        InputSlotWriter view { context.pipe, slot };
        context.callback(view);
    });
}

bool AudioWorkletPipe::try_pop_input(Function<void(InputSlotReader const&)> const& callback)
{
    struct Context {
        AudioWorkletPipe& pipe;
        Function<void(InputSlotReader const&)> const& callback;
    } context { *this, callback };
    return RustFFI::web_audio_worklet_pipe_transfer(m_pipe, false, false, &context, [](void* pointer, u8* slot) {
        auto& context = *static_cast<Context*>(pointer);
        InputSlotReader view { context.pipe, slot };
        context.callback(view);
    });
}

bool AudioWorkletPipe::try_push_output(Function<void(OutputSlotWriter&)> const& callback)
{
    struct Context {
        AudioWorkletPipe& pipe;
        Function<void(OutputSlotWriter&)> const& callback;
    } context { *this, callback };
    return RustFFI::web_audio_worklet_pipe_transfer(m_pipe, true, true, &context, [](void* pointer, u8* slot) {
        auto& context = *static_cast<Context*>(pointer);
        OutputSlotWriter view { context.pipe, slot };
        context.callback(view);
    });
}

bool AudioWorkletPipe::try_pop_output(Function<void(OutputSlotReader const&)> const& callback)
{
    struct Context {
        AudioWorkletPipe& pipe;
        Function<void(OutputSlotReader const&)> const& callback;
    } context { *this, callback };
    return RustFFI::web_audio_worklet_pipe_transfer(m_pipe, true, false, &context, [](void* pointer, u8* slot) {
        auto& context = *static_cast<Context*>(pointer);
        OutputSlotReader view { context.pipe, slot };
        context.callback(view);
    });
}

size_t AudioWorkletPipe::output_occupancy() const { return RustFFI::web_audio_worklet_pipe_output_occupancy(m_pipe); }
void AudioWorkletPipe::discard_outputs_down_to(size_t target) { RustFFI::web_audio_worklet_pipe_discard(m_pipe, target); }
void AudioWorkletPipe::prime_outputs_with_silence(size_t count) { RustFFI::web_audio_worklet_pipe_prime(m_pipe, count); }
void AudioWorkletPipe::request_wakeup() { RustFFI::web_audio_worklet_pipe_request_wakeup(m_pipe); }
void AudioWorkletPipe::clear_wakeup_flag() { RustFFI::web_audio_worklet_pipe_clear_wakeup(m_pipe); }
void AudioWorkletPipe::set_pump_callback(Function<void()> callback) { m_callbacks->pump_callback = move(callback); }
void AudioWorkletPipe::clear_pump_callback() { m_callbacks->pump_callback = nullptr; }
AudioWorkletPipe::State AudioWorkletPipe::state() const { return static_cast<State>(RustFFI::web_audio_worklet_pipe_state(m_pipe)); }
void AudioWorkletPipe::set_state(State state) { RustFFI::web_audio_worklet_pipe_set_state(m_pipe, static_cast<RustFFI::WorkletPipeState>(state)); }
u64 AudioWorkletPipe::underrun_count() const { return RustFFI::web_audio_worklet_pipe_counter(m_pipe, false); }
u64 AudioWorkletPipe::dropped_input_count() const { return RustFFI::web_audio_worklet_pipe_counter(m_pipe, true); }

u64& AudioWorkletPipe::InputSlotWriter::start_frame() { return *RustFFI::web_audio_worklet_slot_frame(m_slot); }
void AudioWorkletPipe::InputSlotWriter::set_had_gap(bool value) { RustFFI::web_audio_worklet_slot_set_flag(m_slot, value); }
u32& AudioWorkletPipe::InputSlotWriter::actual_channel_count(size_t index) { return *RustFFI::web_audio_worklet_slot_count(m_pipe.m_pipe, m_slot, false, index); }
Span<float> AudioWorkletPipe::InputSlotWriter::input_channel(size_t index, size_t channel)
{
    Span<float> samples;
    RustFFI::web_audio_worklet_slot_channel(m_pipe.m_pipe, m_slot, false, index, channel, &samples);
    return samples;
}
Span<float> AudioWorkletPipe::InputSlotWriter::param_block(size_t param)
{
    Span<float> samples;
    RustFFI::web_audio_worklet_slot_param(m_pipe.m_pipe, m_slot, param, &samples);
    return samples;
}

u64 AudioWorkletPipe::InputSlotReader::start_frame() const { return *RustFFI::web_audio_worklet_slot_frame(const_cast<u8*>(m_slot)); }
bool AudioWorkletPipe::InputSlotReader::had_gap() const { return RustFFI::web_audio_worklet_slot_flag(m_slot); }
u32 AudioWorkletPipe::InputSlotReader::actual_channel_count(size_t index) const { return *RustFFI::web_audio_worklet_slot_count(m_pipe.m_pipe, const_cast<u8*>(m_slot), false, index); }
ReadonlySpan<float> AudioWorkletPipe::InputSlotReader::input_channel(size_t index, size_t channel) const
{
    Span<float> samples;
    RustFFI::web_audio_worklet_slot_channel(m_pipe.m_pipe, const_cast<u8*>(m_slot), false, index, channel, &samples);
    return samples;
}
ReadonlySpan<float> AudioWorkletPipe::InputSlotReader::param_block(size_t param) const
{
    Span<float> samples;
    RustFFI::web_audio_worklet_slot_param(m_pipe.m_pipe, const_cast<u8*>(m_slot), param, &samples);
    return samples;
}

u64& AudioWorkletPipe::OutputSlotWriter::start_frame() { return *RustFFI::web_audio_worklet_slot_frame(m_slot); }
void AudioWorkletPipe::OutputSlotWriter::set_processor_active(bool value) { RustFFI::web_audio_worklet_slot_set_flag(m_slot, value); }
u32& AudioWorkletPipe::OutputSlotWriter::actual_channel_count(size_t index) { return *RustFFI::web_audio_worklet_slot_count(m_pipe.m_pipe, m_slot, true, index); }
Span<float> AudioWorkletPipe::OutputSlotWriter::output_channel(size_t index, size_t channel)
{
    Span<float> samples;
    RustFFI::web_audio_worklet_slot_channel(m_pipe.m_pipe, m_slot, true, index, channel, &samples);
    return samples;
}

u64 AudioWorkletPipe::OutputSlotReader::start_frame() const { return *RustFFI::web_audio_worklet_slot_frame(const_cast<u8*>(m_slot)); }
bool AudioWorkletPipe::OutputSlotReader::processor_active() const { return RustFFI::web_audio_worklet_slot_flag(m_slot); }
u32 AudioWorkletPipe::OutputSlotReader::actual_channel_count(size_t index) const { return *RustFFI::web_audio_worklet_slot_count(m_pipe.m_pipe, const_cast<u8*>(m_slot), true, index); }
ReadonlySpan<float> AudioWorkletPipe::OutputSlotReader::output_channel(size_t index, size_t channel) const
{
    Span<float> samples;
    RustFFI::web_audio_worklet_slot_channel(m_pipe.m_pipe, const_cast<u8*>(m_slot), true, index, channel, &samples);
    return samples;
}

}
