/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/Engine/GraphExecutor.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/SincResampler.h>
#include <WebAudioWorker/SessionSampler.h>
#include <WebAudioWorker/WebAudioSession.h>

namespace WebAudioWorker {

namespace {

using namespace Web::WebAudio::Render;

void ensure_resampler_initialized(RenderState& scratch, size_t device_channel_count, u32 context_sample_rate_hz, u32 device_sample_rate_hz, double input_frames_per_output_frame)
{
    if (scratch.resampler_initialized
        && scratch.resampler_last_context_sample_rate == context_sample_rate_hz
        && scratch.resampler_last_device_sample_rate == device_sample_rate_hz
        && scratch.resampler_last_channel_count == device_channel_count)
        return;

    sample_rate_converter_init(scratch.resampler, device_channel_count, input_frames_per_output_frame, 4096);
    scratch.resampler_initialized = true;
    scratch.resampler_last_context_sample_rate = context_sample_rate_hz;
    scratch.resampler_last_device_sample_rate = device_sample_rate_hz;
    scratch.resampler_last_channel_count = device_channel_count;

    // Buffers are preallocated during initialize_render_state(); just ensure span vectors
    // match the current device channel count.
    scratch.resample_input_spans.resize(device_channel_count);
    scratch.resample_output_spans.resize(device_channel_count);

    scratch.resample_input_read_index = 0;
    scratch.resample_input_available_frames = 0;
}

void append_context_quantum_to_resampler_input(ResampleRenderContext& ctx)
{
    ctx.executor.begin_new_quantum(ctx.scratch.rendered_frames);
    auto const& destination_bus = ctx.executor.render_destination_for_current_quantum();
    ctx.executor.render_analysers_for_current_quantum();

    ctx.scratch.context_mix_bus->zero();
    AudioBus const* input_buses[] = { &destination_bus };
    mix_inputs_into(*ctx.scratch.context_mix_bus, ReadonlySpan<AudioBus const*> { input_buses, 1 });

    ctx.scratch.rendered_frames += RENDER_QUANTUM_SIZE;

    // Write the context quantum into the per-channel ring buffer.
    size_t const capacity_frames = ctx.scratch.resample_input_channels[0].size();
    if (capacity_frames == 0)
        return;

    // Ensure space. If we somehow over-produce input (for example due to ratio pathology),
    // drop oldest frames to keep the render thread bounded and memory-safe.
    if (ctx.scratch.resample_input_available_frames + RENDER_QUANTUM_SIZE > capacity_frames) {
        size_t const overflow = (ctx.scratch.resample_input_available_frames + RENDER_QUANTUM_SIZE) - capacity_frames;
        ctx.scratch.resample_input_read_index = (ctx.scratch.resample_input_read_index + overflow) % capacity_frames;
        ctx.scratch.resample_input_available_frames -= min(overflow, ctx.scratch.resample_input_available_frames);
    }

    size_t write_index = (ctx.scratch.resample_input_read_index + ctx.scratch.resample_input_available_frames) % capacity_frames;
    size_t const first_part = min(RENDER_QUANTUM_SIZE, capacity_frames - write_index);
    size_t const second_part = RENDER_QUANTUM_SIZE - first_part;

    for (size_t ch = 0; ch < ctx.device_channel_count; ++ch) {
        auto input_channel = ctx.scratch.context_mix_bus->channel(ch);
        __builtin_memcpy(ctx.scratch.resample_input_channels[ch].data() + write_index, input_channel.data(), first_part * sizeof(f32));
        if (second_part > 0)
            __builtin_memcpy(ctx.scratch.resample_input_channels[ch].data(), input_channel.data() + first_part, second_part * sizeof(f32));
    }

    ctx.scratch.resample_input_available_frames += RENDER_QUANTUM_SIZE;
}

void ensure_resampler_input_frames_available(ResampleRenderContext& ctx, size_t required_frames, size_t max_context_quanta_per_output_quantum, size_t& context_quanta_appended)
{
    while (ctx.scratch.resample_input_available_frames < required_frames && context_quanta_appended < max_context_quanta_per_output_quantum) {
        append_context_quantum_to_resampler_input(ctx);
        ++context_quanta_appended;
    }
}

void build_resampler_input_spans(RenderState& scratch, size_t device_channel_count)
{
    size_t const capacity_frames = scratch.resample_input_channels[0].size();
    for (size_t ch = 0; ch < device_channel_count; ++ch) {
        if (scratch.resample_input_available_frames == 0) {
            scratch.resample_input_spans[ch] = ReadonlySpan<f32> {};
            continue;
        }
        size_t const contiguous = capacity_frames - scratch.resample_input_read_index;
        if (scratch.resample_input_available_frames <= contiguous) {
            scratch.resample_input_spans[ch] = ReadonlySpan<f32> {
                scratch.resample_input_channels[ch].data() + scratch.resample_input_read_index,
                scratch.resample_input_available_frames
            };
        } else {
            auto& scratch_channel = scratch.resample_input_scratch_channels[ch];
            VERIFY(scratch_channel.size() >= capacity_frames);
            size_t const first_part = contiguous;
            size_t const second_part = scratch.resample_input_available_frames - first_part;
            __builtin_memcpy(scratch_channel.data(), scratch.resample_input_channels[ch].data() + scratch.resample_input_read_index, first_part * sizeof(f32));
            __builtin_memcpy(scratch_channel.data() + first_part, scratch.resample_input_channels[ch].data(), second_part * sizeof(f32));
            scratch.resample_input_spans[ch] = ReadonlySpan<f32> { scratch_channel.data(), scratch.resample_input_available_frames };
        }
    }
}

}

void render_at_device_sample_rate(ResampleRenderContext& ctx)
{
    ctx.executor.begin_new_quantum(ctx.scratch.rendered_frames);
    auto const& destination_bus = ctx.executor.render_destination_for_current_quantum();
    ctx.executor.render_analysers_for_current_quantum();

    ctx.scratch.mix_bus->zero();
    AudioBus const* input_buses[] = { &destination_bus };
    mix_inputs_into(*ctx.scratch.mix_bus, ReadonlySpan<AudioBus const*> { input_buses, 1 });

    if (ctx.scratch.planar_spans.size() != ctx.device_channel_count)
        ctx.scratch.planar_spans.resize(ctx.device_channel_count);
    for (size_t ch = 0; ch < ctx.device_channel_count; ++ch)
        ctx.scratch.planar_spans[ch] = ctx.scratch.mix_bus->channel(ch);

    copy_planar_to_interleaved(ctx.scratch.planar_spans.span(), ctx.scratch.interleaved.span(), RENDER_QUANTUM_SIZE);

    ctx.scratch.rendered_frames += RENDER_QUANTUM_SIZE;
    ctx.scratch.frames_written += RENDER_QUANTUM_SIZE;
}

void render_with_resampler(ResampleRenderContext& ctx)
{
    double const input_frames_per_output_frame = static_cast<double>(ctx.context_sample_rate_hz) / static_cast<double>(ctx.device_sample_rate_hz);

    ensure_resampler_initialized(ctx.scratch, ctx.device_channel_count, ctx.context_sample_rate_hz, ctx.device_sample_rate_hz, input_frames_per_output_frame);

    if (!ctx.scratch.context_mix_bus || ctx.scratch.context_mix_bus->channel_capacity() != ctx.device_channel_count || ctx.scratch.context_mix_bus->frame_count() != RENDER_QUANTUM_SIZE)
        ctx.scratch.context_mix_bus = make<AudioBus>(ctx.device_channel_count, RENDER_QUANTUM_SIZE, ctx.device_channel_count);

    ctx.scratch.mix_bus->zero();

    // Ensure we have at least one context quantum of input before attempting to resample.
    if (ctx.scratch.resample_input_available_frames < RENDER_QUANTUM_SIZE)
        append_context_quantum_to_resampler_input(ctx);

    // Bound the amount of context-rate rendering we can do per output quantum.
    // This keeps the render thread deterministic even when the SRC behaves unexpectedly.
    size_t const max_context_quanta_per_output_quantum = 8;
    size_t context_quanta_appended = 0;

    size_t const taps = SincResamplerKernel::tap_count;
    size_t const estimated_input = static_cast<size_t>(ceil(static_cast<double>(RENDER_QUANTUM_SIZE) * input_frames_per_output_frame)) + taps;
    ensure_resampler_input_frames_available(ctx, estimated_input, max_context_quanta_per_output_quantum, context_quanta_appended);

    // Build resampler input spans. If the ring wraps, copy into a preallocated scratch buffer.
    build_resampler_input_spans(ctx.scratch, ctx.device_channel_count);

    for (size_t ch = 0; ch < ctx.device_channel_count; ++ch)
        ctx.scratch.resample_output_spans[ch] = ctx.scratch.mix_bus->channel(ch);

    auto resample_result = sample_rate_converter_process(
        ctx.scratch.resampler,
        ctx.scratch.resample_input_spans.span(),
        ctx.scratch.resample_output_spans.span(),
        false);

    // If the converter produced nothing, try to append one more context quantum (bounded)
    // and re-run once.
    if (resample_result.output_frames_produced == 0 && context_quanta_appended < max_context_quanta_per_output_quantum) {
        append_context_quantum_to_resampler_input(ctx);
        ++context_quanta_appended;
        ensure_resampler_input_frames_available(ctx, estimated_input, max_context_quanta_per_output_quantum, context_quanta_appended);
        build_resampler_input_spans(ctx.scratch, ctx.device_channel_count);

        resample_result = sample_rate_converter_process(
            ctx.scratch.resampler,
            ctx.scratch.resample_input_spans.span(),
            ctx.scratch.resample_output_spans.span(),
            false);
    }

    size_t const capacity_frames = ctx.scratch.resample_input_channels[0].size();
    if (resample_result.input_frames_consumed > 0 && capacity_frames > 0) {
        ctx.scratch.resample_input_read_index = (ctx.scratch.resample_input_read_index + resample_result.input_frames_consumed) % capacity_frames;
        ctx.scratch.resample_input_available_frames = ctx.scratch.resample_input_available_frames >= resample_result.input_frames_consumed
            ? (ctx.scratch.resample_input_available_frames - resample_result.input_frames_consumed)
            : 0;
    }

    if (resample_result.output_frames_produced < RENDER_QUANTUM_SIZE) {
        ctx.scratch.underrun_frames.fetch_add(RENDER_QUANTUM_SIZE - resample_result.output_frames_produced, AK::MemoryOrder::memory_order_relaxed);
    }

    if (ctx.scratch.planar_spans.size() != ctx.device_channel_count)
        ctx.scratch.planar_spans.resize(ctx.device_channel_count);
    for (size_t ch = 0; ch < ctx.device_channel_count; ++ch)
        ctx.scratch.planar_spans[ch] = ctx.scratch.mix_bus->channel(ch);

    copy_planar_to_interleaved(ctx.scratch.planar_spans.span(), ctx.scratch.interleaved.span(), RENDER_QUANTUM_SIZE);
    ctx.scratch.frames_written += RENDER_QUANTUM_SIZE;
}

}
