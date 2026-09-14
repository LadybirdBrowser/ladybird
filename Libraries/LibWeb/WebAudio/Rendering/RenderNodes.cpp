/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <LibGfx/Vector3.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Sinks/AudioPullSink.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio::Rendering {

// Additive synthesis of the built-in oscillator waveforms is capped to keep the per-sample cost bounded for very low
// fundamental frequencies.
static constexpr size_t MAX_OSCILLATOR_HARMONICS = 5000;

struct PanGains {
    float left { 0 };
    float right { 0 };
};

// The equal-power panning curve maps a position in [0, 1] to a left/right gain pair whose squared sum is 1, shared by
// the StereoPannerNode and the equalpower PannerNode.
// https://webaudio.github.io/web-audio-api/#stereopanner-algorithm
static PanGains equal_power_pan_gains(double position)
{
    return {
        static_cast<float>(AK::cos(position * AK::Pi<double> / 2)),
        static_cast<float>(AK::sin(position * AK::Pi<double> / 2)),
    };
}

DestinationRenderNode::DestinationRenderNode(NodeID node_id, size_t channel_count, size_t quantum_size)
    : RenderNode(node_id, 1, 1, quantum_size, channel_count)
{
    set_channel_count(channel_count);
    set_channel_count_mode(Bindings::ChannelCountMode::Explicit);
}

void DestinationRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    auto& destination_output = output(0);
    destination_output.set_channel_count(channel_count());
    destination_output.zero();
    destination_output.sum_from(input, channel_interpretation());
}

MediaElementAudioSourceRenderNode::MediaElementAudioSourceRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<Media::AudioPullSink> audio_pull_sink, bool output_must_be_silenced)
    : RenderNode(node_id, 0, 1, quantum_size)
    , m_audio_pull_sink(move(audio_pull_sink))
    , m_output_must_be_silenced(output_must_be_silenced)
{
}

void MediaElementAudioSourceRenderNode::handle_message(NodeMessage const& message)
{
    message.visit(
        [&](SetMediaElementSourceOutputSilenced const& set_silenced) { m_output_must_be_silenced = set_silenced.output_must_be_silenced; },
        [](auto const&) {});
}

// https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
void MediaElementAudioSourceRenderNode::process(RenderGraph&, RenderContext const& context)
{
    // The number of channels of the single output equals the number of channels of the audio referenced by the
    // HTMLMediaElement passed in as the argument to createMediaElementSource(), or is 1 if the HTMLMediaElement has
    // no audio.
    auto channel_count = max(1u, static_cast<unsigned>(m_audio_pull_sink->channel_count()));
    auto& source_output = output(0);
    source_output.set_channel_count(channel_count);
    source_output.zero();

    if (m_audio_pull_sink->channel_count() == 0)
        return;

    VERIFY(channel_count <= Audio::ChannelMap::capacity());
    Array<Span<float>, Audio::ChannelMap::capacity()> output_channels;
    for (size_t channel = 0; channel < channel_count; ++channel)
        output_channels[channel] = source_output.channel(channel);

    m_audio_pull_sink->render(output_channels.span().trim(channel_count), static_cast<i64>(context.output_latency_in_frames));

    // https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceOptions-security
    // To prevent this, a MediaElementAudioSourceNode MUST output silence instead of the normal output of the
    // HTMLMediaElement if it has been created using an HTMLMediaElement for which the execution of the fetch
    // algorithm labeled the resource as CORS-cross-origin.
    if (m_output_must_be_silenced)
        source_output.zero();
}

GainRenderNode::GainRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> gain)
    : RenderNode(node_id, 1, 1, quantum_size)
    , m_gain(move(gain))
{
    m_gain_values.resize(quantum_size);
}

void GainRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    auto& gain_output = output(0);
    gain_output.set_channel_count(input.channel_count());
    if (input.is_silent()) {
        gain_output.zero();
        return;
    }

    m_gain->compute(graph, context, m_gain_values);

    for (size_t channel_index = 0; channel_index < input.channel_count(); ++channel_index) {
        auto input_samples = input.channel(channel_index);
        auto output_samples = gain_output.channel(channel_index);
        for (size_t frame = 0; frame < context.quantum_size; ++frame)
            output_samples[frame] = input_samples[frame] * m_gain_values[frame];
    }
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-start
void ScheduledSourceRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    // If 0 is passed in for this value or if the value is less than currentTime, then the sound will start playing
    // immediately.
    // NB: The start time is clamped to the first render quantum processed after the start message arrived, since the
    //     control thread's currentTime may already have advanced past the scheduled time by then.
    if (m_start_time_needs_clamping) {
        m_start_time = max(*m_start_time, context.quantum_start_time());
        m_start_time_needs_clamping = false;
    }
    process_source(graph, context);
}

void ScheduledSourceRenderNode::handle_message(NodeMessage const& message)
{
    message.visit(
        [&](StartSource const& start_source) {
            m_start_time = start_source.when;
            m_start_time_needs_clamping = true;
        },
        [&](StopSource const& stop_source) { m_stop_time = stop_source.when; },
        [](auto const&) {});
}

// Marks this node as ended once the quantum has advanced past the stop time or the content was exhausted.
void ScheduledSourceRenderNode::update_ended_state(RenderContext const& context)
{
    if (!m_start_time.has_value() || m_ended)
        return;
    auto quantum_end_time = context.frame_time(context.quantum_size);
    if (quantum_end_time >= m_stop_time || m_content_ended) {
        m_ended = true;
        m_ended_pending = true;
    }
}

ConstantSourceRenderNode::ConstantSourceRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> offset)
    : ScheduledSourceRenderNode(node_id, 0, 1, quantum_size)
    , m_offset(move(offset))
{
    m_offset_values.resize(quantum_size);
}

void ConstantSourceRenderNode::process_source(RenderGraph& graph, RenderContext const& context)
{
    m_offset->compute(graph, context, m_offset_values);
    auto output_samples = output(0).channel(0);
    for (size_t frame = 0; frame < context.quantum_size; ++frame)
        output_samples[frame] = is_active_at(context.frame_time(frame)) ? m_offset_values[frame] : 0.f;
    update_ended_state(context);
}

OscillatorRenderNode::OscillatorRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune)
    : ScheduledSourceRenderNode(node_id, 0, 1, quantum_size)
    , m_frequency(move(frequency))
    , m_detune(move(detune))
{
    m_frequency_values.resize(quantum_size);
    m_detune_values.resize(quantum_size);
}

void OscillatorRenderNode::handle_message(NodeMessage const& message)
{
    message.visit(
        [&](SetOscillatorWaveform const& set_waveform) {
            m_type = set_waveform.type;
            m_periodic_wave = set_waveform.periodic_wave;

            // https://webaudio.github.io/web-audio-api/#waveform-normalization
            // If the internal slot [[normalize]] of this PeriodicWave is true (the default), the waveform defined in
            // the previous section is normalized so that the maximum value is 1.
            m_periodic_wave_scale = 1.f;
            if (m_periodic_wave && m_periodic_wave->normalize) {
                float maximum_amplitude = 0.f;
                constexpr size_t normalization_sample_count = 4096;
                for (size_t sample_index = 0; sample_index < normalization_sample_count; ++sample_index) {
                    auto phase = 2 * AK::Pi<double> * sample_index / normalization_sample_count;
                    float amplitude = 0.f;
                    for (size_t harmonic = 1; harmonic < m_periodic_wave->real.size(); ++harmonic)
                        amplitude += m_periodic_wave->real[harmonic] * static_cast<float>(AK::cos(harmonic * phase))
                            + m_periodic_wave->imag[harmonic] * static_cast<float>(AK::sin(harmonic * phase));
                    maximum_amplitude = max(maximum_amplitude, AK::abs(amplitude));
                }
                if (maximum_amplitude > 0.f)
                    m_periodic_wave_scale = 1.f / maximum_amplitude;
            }
        },
        [&](auto const& other_message) { ScheduledSourceRenderNode::handle_message(other_message); });
}

// https://webaudio.github.io/web-audio-api/#oscillator-coefficients
float OscillatorRenderNode::sample_waveform(double phase, double frequency, double nyquist_frequency) const
{
    switch (m_type) {
    case Bindings::OscillatorType::Sine:
        return static_cast<float>(AK::sin(phase));
    case Bindings::OscillatorType::Square: {
        // b[n] = 2/(nπ) * (1 - cos(nπ))
        // NB: This is 4/(nπ) for odd n and 0 for even n.
        auto harmonic_count = frequency != 0 ? static_cast<size_t>(nyquist_frequency / AK::abs(frequency)) : MAX_OSCILLATOR_HARMONICS;
        harmonic_count = min(harmonic_count, MAX_OSCILLATOR_HARMONICS);
        double value = 0;
        for (size_t harmonic = 1; harmonic <= harmonic_count; harmonic += 2) {
            auto coefficient = 4 / (harmonic * AK::Pi<double>);
            value += coefficient * AK::sin(harmonic * phase);
        }
        return static_cast<float>(value);
    }
    case Bindings::OscillatorType::Sawtooth: {
        // b[n] = (-1)^(n+1) * 2/(nπ)
        auto harmonic_count = frequency != 0 ? static_cast<size_t>(nyquist_frequency / AK::abs(frequency)) : MAX_OSCILLATOR_HARMONICS;
        harmonic_count = min(harmonic_count, MAX_OSCILLATOR_HARMONICS);
        double value = 0;
        for (size_t harmonic = 1; harmonic <= harmonic_count; ++harmonic) {
            auto coefficient = 2 / (harmonic * AK::Pi<double>);
            if (harmonic % 2 == 0)
                coefficient = -coefficient;
            value += coefficient * AK::sin(harmonic * phase);
        }
        return static_cast<float>(value);
    }
    case Bindings::OscillatorType::Triangle: {
        // b[n] = 8 * sin(nπ/2) / (nπ)²
        // NB: This alternates sign for odd n and is 0 for even n.
        auto harmonic_count = frequency != 0 ? static_cast<size_t>(nyquist_frequency / AK::abs(frequency)) : MAX_OSCILLATOR_HARMONICS;
        harmonic_count = min(harmonic_count, MAX_OSCILLATOR_HARMONICS);
        double value = 0;
        auto sign = 1.;
        for (size_t harmonic = 1; harmonic <= harmonic_count; harmonic += 2) {
            auto coefficient = sign * 8 / (harmonic * harmonic * AK::Pi<double> * AK::Pi<double>);
            value += coefficient * AK::sin(harmonic * phase);
            sign = -sign;
        }
        return static_cast<float>(value);
    }
    case Bindings::OscillatorType::Custom: {
        if (!m_periodic_wave)
            return 0.f;
        auto harmonic_limit = m_periodic_wave->real.size();
        double value = 0;
        for (size_t harmonic_index = 1; harmonic_index < harmonic_limit; ++harmonic_index) {
            value += m_periodic_wave->real[harmonic_index] * AK::cos(harmonic_index * phase)
                + m_periodic_wave->imag[harmonic_index] * AK::sin(harmonic_index * phase);
        }
        return static_cast<float>(value) * m_periodic_wave_scale;
    }
    }
    VERIFY_NOT_REACHED();
}

void OscillatorRenderNode::process_source(RenderGraph& graph, RenderContext const& context)
{
    m_frequency->compute(graph, context, m_frequency_values);
    m_detune->compute(graph, context, m_detune_values);

    auto nyquist_frequency = context.sample_rate / 2;
    auto output_samples = output(0).channel(0);
    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        if (!is_active_at(context.frame_time(frame))) {
            output_samples[frame] = 0.f;
            continue;
        }

        // computedOscFrequency(t) = frequency(t) * pow(2, detune(t) / 1200)
        // NB: The computed frequency is clamped to the Nyquist frequency.
        auto computed_frequency = m_frequency_values[frame] * AK::exp2(m_detune_values[frame] / 1200.);
        computed_frequency = clamp(computed_frequency, -nyquist_frequency, nyquist_frequency);

        output_samples[frame] = sample_waveform(m_phase, computed_frequency, nyquist_frequency);

        m_phase += 2 * AK::Pi<double> * computed_frequency / context.sample_rate;
        m_phase = AK::fmod(m_phase, 2 * AK::Pi<double>);
        if (m_phase < 0)
            m_phase += 2 * AK::Pi<double>;
    }
    update_ended_state(context);
}

AudioBufferSourceRenderNode::AudioBufferSourceRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> playback_rate, NonnullRefPtr<RenderAudioParam> detune)
    : ScheduledSourceRenderNode(node_id, 0, 1, quantum_size)
    , m_playback_rate(move(playback_rate))
    , m_detune(move(detune))
{
    m_playback_rate_values.resize(quantum_size);
    m_detune_values.resize(quantum_size);
}

void AudioBufferSourceRenderNode::handle_message(NodeMessage const& message)
{
    message.visit(
        [&](StartBufferSource const& start_buffer_source) {
            ScheduledSourceRenderNode::handle_message(NodeMessage { StartSource { start_buffer_source.node_id, start_buffer_source.when } });
            m_offset = start_buffer_source.offset;
            m_duration = start_buffer_source.duration;
            if (start_buffer_source.buffer)
                m_buffer = start_buffer_source.buffer;
        },
        [&](SetBufferSourceParameters const& set_parameters) {
            m_loop = set_parameters.loop;
            m_loop_start = set_parameters.loop_start;
            m_loop_end = set_parameters.loop_end;
            if (set_parameters.update_buffer)
                m_buffer = set_parameters.buffer;
        },
        [&](auto const& other_message) { ScheduledSourceRenderNode::handle_message(other_message); });
}

float AudioBufferSourceRenderNode::sample_buffer(size_t channel, double playhead_frames, double loop_start_frame, double loop_end_frame) const
{
    auto const& samples = m_buffer->channels[channel];
    auto index = static_cast<i64>(AK::floor(playhead_frames));
    auto fraction = static_cast<float>(playhead_frames - index);

    auto sample_at = [&](i64 sample_index) -> float {
        // When looping, interpolation across the loop end wraps around to the loop start.
        if (m_loop && sample_index >= static_cast<i64>(loop_end_frame) && loop_end_frame > loop_start_frame)
            sample_index -= static_cast<i64>(loop_end_frame - loop_start_frame);
        if (sample_index < 0 || sample_index >= static_cast<i64>(samples.size()))
            return 0.f;
        return samples[sample_index];
    };
    if (fraction == 0.f)
        return sample_at(index);
    return sample_at(index) + fraction * (sample_at(index + 1) - sample_at(index));
}

// https://webaudio.github.io/web-audio-api/#playback-AudioBufferSourceNode
void AudioBufferSourceRenderNode::process_source(RenderGraph& graph, RenderContext const& context)
{
    m_playback_rate->compute(graph, context, m_playback_rate_values);
    m_detune->compute(graph, context, m_detune_values);

    auto& source_output = output(0);
    source_output.set_channel_count(m_buffer ? m_buffer->channels.size() : 1);
    source_output.zero();

    if (!m_buffer || !has_started()) {
        update_ended_state(context);
        return;
    }

    // NB: The playback rate is k-rate, so a single computed value is used for the entire quantum. The playhead is
    //     tracked in buffer sample frames so that unity-rate playback of a buffer at the context's sample rate
    //     advances by exactly one frame per rendered frame and reproduces the buffer bit-exactly.
    auto computed_playback_rate = m_playback_rate_values[0] * AK::exp2(m_detune_values[0] / 1200.);
    auto buffer_sample_rate = static_cast<double>(m_buffer->sample_rate);
    auto playhead_increment = computed_playback_rate * buffer_sample_rate / context.sample_rate;
    auto buffer_length = static_cast<double>(m_buffer->length());

    // NB: Determine the effective loop region, in buffer sample frames.
    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
    auto actual_loop_start = 0.;
    auto actual_loop_end = buffer_length;
    if (m_loop && m_loop_start >= 0 && m_loop_end > 0 && m_loop_start < m_loop_end) {
        actual_loop_start = min(m_loop_start * buffer_sample_rate, buffer_length);
        actual_loop_end = min(m_loop_end * buffer_sample_rate, buffer_length);
    }

    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        auto frame_time = context.frame_time(frame);
        if (!is_active_at(frame_time))
            continue;

        if (!m_playhead_valid) {
            // NB: Playback begins at the given offset, corrected for the source starting between two sample frames.
            m_playhead = m_offset * buffer_sample_rate;
            if (m_loop)
                m_playhead = min(m_playhead, actual_loop_end);
            else
                m_playhead = min(m_playhead, buffer_length);
            m_playhead += (frame_time - start_time()) * computed_playback_rate * buffer_sample_rate;
            m_playhead_valid = true;
        }

        // NB: Compare using a half-frame tolerance since m_played_time accumulates one frame's worth of content at
        //     a time and would otherwise overshoot the duration by up to one frame due to floating-point rounding.
        auto frame_consumption = AK::abs(computed_playback_rate) / context.sample_rate;
        if (m_duration.has_value() && m_played_time + frame_consumption / 2 >= *m_duration) {
            set_content_ended();
            continue;
        }

        if (m_loop) {
            auto loop_duration = actual_loop_end - actual_loop_start;
            if (loop_duration > 0) {
                while (m_playhead >= actual_loop_end)
                    m_playhead -= loop_duration;
                while (m_playhead < actual_loop_start && playhead_increment < 0)
                    m_playhead += loop_duration;
            }
        } else if (m_playhead >= buffer_length || (playhead_increment < 0 && m_playhead < 0)) {
            set_content_ended();
            continue;
        }

        for (size_t channel_index = 0; channel_index < m_buffer->channels.size(); ++channel_index)
            source_output.channel(channel_index)[frame] = sample_buffer(channel_index, m_playhead, actual_loop_start, actual_loop_end);

        m_playhead += playhead_increment;
        m_played_time += AK::abs(computed_playback_rate) / context.sample_rate;
    }
    update_ended_state(context);
}

DelayRenderNode::DelayRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> delay_time, double max_delay_time)
    : RenderNode(node_id, 1, 1, quantum_size)
    , m_delay_time(move(delay_time))
    , m_max_delay_time(max_delay_time)
{
    m_delay_values.resize(quantum_size);
}

void DelayRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    m_delay_time->compute(graph, context, m_delay_values);

    auto history_length = static_cast<size_t>(AK::ceil(m_max_delay_time * context.sample_rate)) + 2;
    if (m_history.size() != input.channel_count() || (!m_history.is_empty() && m_history.first().size() != history_length)) {
        m_history.resize_and_keep_capacity(input.channel_count());
        for (auto& channel_history : m_history) {
            channel_history.clear_with_capacity();
            channel_history.resize(history_length);
        }
        m_write_index = 0;
    }

    auto& delay_output = output(0);
    delay_output.set_channel_count(input.channel_count());

    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        auto delay_seconds = clamp(static_cast<double>(m_delay_values[frame]), 0., m_max_delay_time);
        auto delay_frames = delay_seconds * context.sample_rate;

        for (size_t channel_index = 0; channel_index < input.channel_count(); ++channel_index) {
            auto& channel_history = m_history[channel_index];
            channel_history[m_write_index] = input.channel(channel_index)[frame];

            // Read with linear interpolation to support fractional delays.
            auto read_position = static_cast<double>(m_write_index) - delay_frames;
            while (read_position < 0)
                read_position += history_length;
            auto read_index = static_cast<size_t>(read_position);
            auto fraction = static_cast<float>(read_position - read_index);
            auto sample_a = channel_history[read_index % history_length];
            auto sample_b = channel_history[(read_index + 1) % history_length];
            delay_output.channel(channel_index)[frame] = sample_a + fraction * (sample_b - sample_a);
        }
        m_write_index = (m_write_index + 1) % history_length;
    }
}

StereoPannerRenderNode::StereoPannerRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> pan)
    : RenderNode(node_id, 1, 1, quantum_size, 2)
    , m_pan(move(pan))
{
    set_channel_count_mode(Bindings::ChannelCountMode::ClampedMax);
    m_pan_values.resize(quantum_size);
}

// https://webaudio.github.io/web-audio-api/#stereopanner-algorithm
void StereoPannerRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    m_pan->compute(graph, context, m_pan_values);

    auto& panner_output = output(0);
    panner_output.set_channel_count(2);
    auto output_left = panner_output.channel(0);
    auto output_right = panner_output.channel(1);

    auto input_left = input.channel(0);
    auto input_right = input.channel_count() > 1 ? input.channel(1) : input.channel(0);

    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        // Let pan be the computedValue of the pan AudioParam of this StereoPannerNode.
        // Clamp pan to [-1, 1].
        auto pan = clamp(m_pan_values[frame], -1.f, 1.f);

        // Calculate x by normalizing pan value to [0, 1].
        double x;
        if (input.channel_count() == 1)
            x = (pan + 1) / 2.;
        else
            x = pan <= 0 ? pan + 1. : pan;

        // Left and right gain values are calculated as: gainL = cos(x * π / 2); gainR = sin(x * π / 2);
        auto [gain_left, gain_right] = equal_power_pan_gains(x);

        // For mono input, the stereo output is calculated as: outputL = input * gainL; outputR = input * gainR;
        // NB: For stereo input, the attenuated channel is mixed into the opposite output channel instead.
        if (input.channel_count() == 1) {
            output_left[frame] = input_left[frame] * gain_left;
            output_right[frame] = input_left[frame] * gain_right;
        } else if (pan <= 0) {
            output_left[frame] = input_left[frame] + input_right[frame] * gain_left;
            output_right[frame] = input_right[frame] * gain_right;
        } else {
            output_left[frame] = input_left[frame] * gain_left;
            output_right[frame] = input_right[frame] + input_left[frame] * gain_right;
        }
    }
}

ChannelSplitterRenderNode::ChannelSplitterRenderNode(NodeID node_id, size_t quantum_size, size_t number_of_outputs)
    : RenderNode(node_id, 1, number_of_outputs, quantum_size)
{
    set_channel_count(number_of_outputs);
    set_channel_count_mode(Bindings::ChannelCountMode::Explicit);
    set_channel_interpretation(Bindings::ChannelInterpretation::Discrete);
}

void ChannelSplitterRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    for (size_t output_index = 0; output_index < output_count(); ++output_index) {
        auto output_samples = output(output_index).channel(0);
        if (output_index < input.channel_count()) {
            auto input_samples = input.channel(output_index);
            for (size_t frame = 0; frame < context.quantum_size; ++frame)
                output_samples[frame] = input_samples[frame];
        } else {
            output_samples.fill(0.f);
        }
    }
}

ChannelMergerRenderNode::ChannelMergerRenderNode(NodeID node_id, size_t quantum_size, size_t number_of_inputs)
    : RenderNode(node_id, number_of_inputs, 1, quantum_size, number_of_inputs)
{
    set_channel_count(1);
    set_channel_count_mode(Bindings::ChannelCountMode::Explicit);
}

void ChannelMergerRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto& merger_output = output(0);
    merger_output.set_channel_count(input_count());
    for (size_t input_index = 0; input_index < input_count(); ++input_index) {
        auto const& input = pull_input(graph, context, input_index);
        auto input_samples = input.channel(0);
        auto output_samples = merger_output.channel(input_index);
        for (size_t frame = 0; frame < context.quantum_size; ++frame)
            output_samples[frame] = input_samples[frame];
    }
}

BiquadFilterRenderNode::BiquadFilterRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune, NonnullRefPtr<RenderAudioParam> q, NonnullRefPtr<RenderAudioParam> gain)
    : RenderNode(node_id, 1, 1, quantum_size)
    , m_frequency(move(frequency))
    , m_detune(move(detune))
    , m_q(move(q))
    , m_gain(move(gain))
{
    m_frequency_values.resize(quantum_size);
    m_detune_values.resize(quantum_size);
    m_q_values.resize(quantum_size);
    m_gain_values.resize(quantum_size);
}

void BiquadFilterRenderNode::handle_message(NodeMessage const& message)
{
    message.visit(
        [&](SetBiquadFilterType const& set_type) { m_type = set_type.type; },
        [](auto const&) {});
}

void BiquadFilterRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    m_frequency->compute(graph, context, m_frequency_values);
    m_detune->compute(graph, context, m_detune_values);
    m_q->compute(graph, context, m_q_values);
    m_gain->compute(graph, context, m_gain_values);

    if (m_filter_states.size() != input.channel_count())
        m_filter_states.resize(input.channel_count());

    auto nyquist_frequency = context.sample_rate / 2;
    auto compute_coefficients_for_frame = [&](size_t frame) {
        auto computed_frequency = m_frequency_values[frame] * AK::exp2(m_detune_values[frame] / 1200.);
        auto normalized_frequency = clamp(computed_frequency / nyquist_frequency, 0., 1.);
        return compute_biquad_coefficients(m_type, normalized_frequency, m_q_values[frame], m_gain_values[frame]);
    };
    auto parameters_changed = [&](size_t frame) {
        return m_frequency_values[frame] != m_frequency_values[frame - 1]
            || m_detune_values[frame] != m_detune_values[frame - 1]
            || m_q_values[frame] != m_q_values[frame - 1]
            || m_gain_values[frame] != m_gain_values[frame - 1];
    };

    auto& filter_output = output(0);
    filter_output.set_channel_count(input.channel_count());

    // AD-HOC: The coefficients are recomputed whenever the computed parameter values change, so a-rate automation is
    //         applied with per-frame resolution while k-rate parameters use a single set of coefficients for the
    //         entire quantum.
    auto coefficients = compute_coefficients_for_frame(0);
    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        if (frame > 0 && parameters_changed(frame))
            coefficients = compute_coefficients_for_frame(frame);
        for (size_t channel_index = 0; channel_index < input.channel_count(); ++channel_index) {
            auto& state = m_filter_states[channel_index];
            double x = input.channel(channel_index)[frame];
            auto y = coefficients.b0 * x + coefficients.b1 * state.x1 + coefficients.b2 * state.x2
                - coefficients.a1 * state.y1 - coefficients.a2 * state.y2;
            state.x2 = state.x1;
            state.x1 = x;
            state.y2 = state.y1;
            state.y1 = y;
            filter_output.channel(channel_index)[frame] = static_cast<float>(y);
        }
    }
}

namespace {

// Gfx::DoubleVector3::normalize() divides by the length unconditionally, which would produce NaN components for a zero
// vector; the spatialization math needs a zero vector to normalize back to a zero vector instead.
Gfx::DoubleVector3 safe_normalized(Gfx::DoubleVector3 const& vector)
{
    if (vector.length() == 0)
        return {};
    return vector.normalized();
}

// https://webaudio.github.io/web-audio-api/#azimuth-elevation
double compute_azimuth(Gfx::DoubleVector3 const& source_position, Gfx::DoubleVector3 const& listener_position, Gfx::DoubleVector3 const& listener_forward, Gfx::DoubleVector3 const& listener_up)
{
    // Calculate the source-listener vector.
    auto source_listener = source_position - listener_position;

    // Handle degenerate case if source and listener are at the same point.
    if (source_listener.length() == 0)
        return 0;
    source_listener = source_listener.normalized();

    // Align axes.
    auto listener_right = listener_forward.cross(listener_up);

    // Handle the case where listener's 'up' and 'forward' vectors are linearly dependent, in which case 'right'
    // cannot be determined.
    if (listener_right.length() == 0)
        return 0;

    // Determine a unit vector orthogonal to listener's right, forward.
    auto listener_right_norm = listener_right.normalized();
    auto listener_forward_norm = safe_normalized(listener_forward);
    auto up = listener_right_norm.cross(listener_forward_norm);

    auto up_projection = source_listener.dot(up);
    auto projected_source = source_listener - up * up_projection;
    projected_source = safe_normalized(projected_source);

    auto azimuth = 180 * AK::acos(clamp(projected_source.dot(listener_right_norm), -1., 1.)) / AK::Pi<double>;

    // Source in front or behind the listener.
    auto front_back = projected_source.dot(listener_forward_norm);
    if (front_back < 0)
        azimuth = 360 - azimuth;

    // Make azimuth relative to "forward" and not "right" listener vector.
    // AD-HOC: This is equivalent to the spec's mapping, but lands directly in the range [-180, 180].
    if (azimuth <= 90)
        azimuth = 90 - azimuth;
    else
        azimuth = 450 - azimuth;
    if (azimuth > 180)
        azimuth -= 360;
    return azimuth;
}

// https://webaudio.github.io/web-audio-api/#Spatialization-distance-effects
double compute_distance_gain(double distance, SetPannerParameters const& config)
{
    auto ref_distance = config.ref_distance;
    auto max_distance = config.max_distance;
    auto rolloff_factor = config.rolloff_factor;

    // AD-HOC: The distance is clamped to the interval spanned by refDistance and maxDistance for every distance
    //         model, matching other engines.
    auto clamped_distance = clamp(distance, min(ref_distance, max_distance), max(ref_distance, max_distance));

    switch (config.distance_model) {
    case Bindings::DistanceModelType::Linear: {
        if (ref_distance == max_distance)
            return 1 - clamp(rolloff_factor, 0., 1.);
        return 1 - clamp(rolloff_factor, 0., 1.) * (clamped_distance - ref_distance) / (max_distance - ref_distance);
    }
    case Bindings::DistanceModelType::Inverse:
        if (ref_distance + rolloff_factor * (clamped_distance - ref_distance) == 0)
            return 1;
        return ref_distance / (ref_distance + rolloff_factor * (clamped_distance - ref_distance));
    case Bindings::DistanceModelType::Exponential:
        if (ref_distance == 0)
            return 1;
        return AK::pow(clamped_distance / ref_distance, -rolloff_factor);
    }
    VERIFY_NOT_REACHED();
}

// https://webaudio.github.io/web-audio-api/#Spatialization-sound-cones
double compute_cone_gain(Gfx::DoubleVector3 const& source_position, Gfx::DoubleVector3 const& source_orientation, Gfx::DoubleVector3 const& listener_position, SetPannerParameters const& config)
{
    if (source_orientation.length() == 0 || (config.cone_inner_angle == 360 && config.cone_outer_angle == 360))
        return 1;

    // Normalized source-listener vector.
    auto source_to_listener = safe_normalized(listener_position - source_position);
    auto normalized_source_orientation = source_orientation.normalized();

    // Angle between the source orientation vector and the source-listener vector.
    auto angle = 180 * AK::acos(clamp(source_to_listener.dot(normalized_source_orientation), -1., 1.)) / AK::Pi<double>;
    auto absolute_angle = AK::abs(angle);

    // Divide by 2 here since API is entire angle (not half-angle).
    auto absolute_inner_angle = AK::abs(config.cone_inner_angle) / 2;
    auto absolute_outer_angle = AK::abs(config.cone_outer_angle) / 2;

    if (absolute_angle <= absolute_inner_angle) {
        // No attenuation.
        return 1;
    }
    if (absolute_angle >= absolute_outer_angle) {
        // Max attenuation.
        return config.cone_outer_gain;
    }

    // Between inner and outer cones: inner -> outer, x goes from 0 -> 1.
    auto x = (absolute_angle - absolute_inner_angle) / (absolute_outer_angle - absolute_inner_angle);
    return 1 + (config.cone_outer_gain - 1) * x;
}

}

PannerRenderNode::PannerRenderNode(NodeID node_id, size_t quantum_size, Params params, ListenerParams listener_params)
    : RenderNode(node_id, 1, 1, quantum_size, 2)
    , m_params(move(params))
    , m_listener_params(move(listener_params))
{
    set_channel_count_mode(Bindings::ChannelCountMode::ClampedMax);
    m_param_values.resize(15);
    for (auto& values : m_param_values)
        values.resize(quantum_size);
}

void PannerRenderNode::handle_message(NodeMessage const& message)
{
    message.visit(
        [&](SetPannerParameters const& parameters) { m_config = parameters; },
        [](auto const&) {});
}

void PannerRenderNode::for_each_param(Function<void(RenderAudioParam&)> const& callback)
{
    callback(*m_params.position_x);
    callback(*m_params.position_y);
    callback(*m_params.position_z);
    callback(*m_params.orientation_x);
    callback(*m_params.orientation_y);
    callback(*m_params.orientation_z);
}

// https://webaudio.github.io/web-audio-api/#Spatialization
void PannerRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    auto& panner_output = output(0);
    panner_output.set_channel_count(2);
    if (input.is_silent()) {
        panner_output.zero();
        return;
    }

    Array params {
        m_params.position_x.ptr(), m_params.position_y.ptr(), m_params.position_z.ptr(),
        m_params.orientation_x.ptr(), m_params.orientation_y.ptr(), m_params.orientation_z.ptr(),
        m_listener_params.position_x.ptr(), m_listener_params.position_y.ptr(), m_listener_params.position_z.ptr(),
        m_listener_params.forward_x.ptr(), m_listener_params.forward_y.ptr(), m_listener_params.forward_z.ptr(),
        m_listener_params.up_x.ptr(), m_listener_params.up_y.ptr(), m_listener_params.up_z.ptr()
    };
    for (size_t param_index = 0; param_index < params.size(); ++param_index)
        params[param_index]->compute(graph, context, m_param_values[param_index]);

    auto output_left = panner_output.channel(0);
    auto output_right = panner_output.channel(1);
    auto input_left = input.channel(0);
    auto input_right = input.channel_count() > 1 ? input.channel(1) : input.channel(0);

    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        Gfx::DoubleVector3 source_position { m_param_values[0][frame], m_param_values[1][frame], m_param_values[2][frame] };
        Gfx::DoubleVector3 source_orientation { m_param_values[3][frame], m_param_values[4][frame], m_param_values[5][frame] };
        Gfx::DoubleVector3 listener_position { m_param_values[6][frame], m_param_values[7][frame], m_param_values[8][frame] };
        Gfx::DoubleVector3 listener_forward { m_param_values[9][frame], m_param_values[10][frame], m_param_values[11][frame] };
        Gfx::DoubleVector3 listener_up { m_param_values[12][frame], m_param_values[13][frame], m_param_values[14][frame] };

        auto azimuth = compute_azimuth(source_position, listener_position, listener_forward, listener_up);
        auto distance = (source_position - listener_position).length();
        auto gain = static_cast<float>(compute_distance_gain(distance, m_config) * compute_cone_gain(source_position, source_orientation, listener_position, m_config));

        // https://webaudio.github.io/web-audio-api/#Spatialization-panning-algorithm
        // The azimuth value is first contained to be within the range [-90, 90].
        // NB: compute_azimuth() already produces a value in [-180, 180], so only the wrapping step remains.
        if (azimuth < -90)
            azimuth = -180 - azimuth;
        else if (azimuth > 90)
            azimuth = 180 - azimuth;

        double x;
        if (input.channel_count() == 1)
            x = (azimuth + 90) / 180;
        else
            x = azimuth <= 0 ? (azimuth + 90) / 90 : azimuth / 90;

        auto [gain_left, gain_right] = equal_power_pan_gains(x);

        if (input.channel_count() == 1) {
            output_left[frame] = input_left[frame] * gain_left * gain;
            output_right[frame] = input_left[frame] * gain_right * gain;
        } else if (azimuth <= 0) {
            output_left[frame] = (input_left[frame] + input_right[frame] * gain_left) * gain;
            output_right[frame] = input_right[frame] * gain_right * gain;
        } else {
            output_left[frame] = input_left[frame] * gain_left * gain;
            output_right[frame] = (input_right[frame] + input_left[frame] * gain_right) * gain;
        }
    }
}

PassthroughRenderNode::PassthroughRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(node_id, 1, 1, quantum_size)
{
}

void PassthroughRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);
    output(0).copy_from(input);
}

}
