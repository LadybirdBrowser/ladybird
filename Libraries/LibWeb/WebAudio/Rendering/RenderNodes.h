/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibMedia/Forward.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>
#include <LibWeb/WebAudio/Rendering/BiquadCoefficients.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

// Captures the mixed input signal so the renderer can copy it into the rendered buffer.
// https://webaudio.github.io/web-audio-api/#AudioDestinationNode
class DestinationRenderNode final : public RenderNode {
public:
    DestinationRenderNode(NodeID, size_t channel_count, size_t quantum_size);

    virtual void process(RenderGraph&, RenderContext const&) override;
};

// https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
class MediaElementAudioSourceRenderNode final : public RenderNode {
public:
    MediaElementAudioSourceRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<Media::AudioPullSink>, bool output_must_be_silenced);

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void handle_message(NodeMessage const&) override;

private:
    NonnullRefPtr<Media::AudioPullSink> m_audio_pull_sink;
    bool m_output_must_be_silenced { false };
};

// https://webaudio.github.io/web-audio-api/#GainNode
class GainRenderNode final : public RenderNode {
public:
    GainRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> gain);

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override { callback(*m_gain); }

private:
    NonnullRefPtr<RenderAudioParam> m_gain;
    Vector<float> m_gain_values;
};

// Base class for source nodes that are scheduled with start() and stop().
// https://webaudio.github.io/web-audio-api/#AudioScheduledSourceNode
class ScheduledSourceRenderNode : public RenderNode {
public:
    using RenderNode::RenderNode;

    virtual void process(RenderGraph&, RenderContext const&) override final;
    virtual void handle_message(NodeMessage const&) override;
    virtual bool take_ended_notification() override { return exchange(m_ended_pending, false); }

protected:
    virtual void process_source(RenderGraph&, RenderContext const&) = 0;

    bool has_started() const { return m_start_time.has_value(); }
    double start_time() const { return m_start_time.value(); }
    double stop_time() const { return m_stop_time; }

    bool is_active_at(double time) const
    {
        return m_start_time.has_value() && time >= *m_start_time && time < m_stop_time && !m_content_ended;
    }

    // Playback naturally ran out of content before the scheduled stop time, e.g. a non-looping buffer finished.
    void set_content_ended() { m_content_ended = true; }
    bool content_ended() const { return m_content_ended; }

    // Marks this node as ended once the quantum has advanced past the stop time or the content was exhausted.
    void update_ended_state(RenderContext const&);

private:
    Optional<double> m_start_time;
    double m_stop_time { AK::Infinity<double> };
    bool m_start_time_needs_clamping { false };
    bool m_content_ended { false };
    bool m_ended { false };
    bool m_ended_pending { false };
};

// https://webaudio.github.io/web-audio-api/#ConstantSourceNode
class ConstantSourceRenderNode final : public ScheduledSourceRenderNode {
public:
    ConstantSourceRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> offset);

    virtual void process_source(RenderGraph&, RenderContext const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override { callback(*m_offset); }

private:
    NonnullRefPtr<RenderAudioParam> m_offset;
    Vector<float> m_offset_values;
};

// https://webaudio.github.io/web-audio-api/#OscillatorNode
class OscillatorRenderNode final : public ScheduledSourceRenderNode {
public:
    OscillatorRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune);

    virtual void process_source(RenderGraph&, RenderContext const&) override;
    virtual void handle_message(NodeMessage const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override
    {
        callback(*m_frequency);
        callback(*m_detune);
    }

private:
    float sample_waveform(double phase, double frequency, double nyquist_frequency) const;

    NonnullRefPtr<RenderAudioParam> m_frequency;
    NonnullRefPtr<RenderAudioParam> m_detune;
    Vector<float> m_frequency_values;
    Vector<float> m_detune_values;
    Bindings::OscillatorType m_type { Bindings::OscillatorType::Sine };
    RefPtr<PeriodicWaveData> m_periodic_wave;
    float m_periodic_wave_scale { 1.f };
    double m_phase { 0 };
};

// https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
class AudioBufferSourceRenderNode final : public ScheduledSourceRenderNode {
public:
    AudioBufferSourceRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> playback_rate, NonnullRefPtr<RenderAudioParam> detune);

    virtual void process_source(RenderGraph&, RenderContext const&) override;
    virtual void handle_message(NodeMessage const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override
    {
        callback(*m_playback_rate);
        callback(*m_detune);
    }

private:
    float sample_buffer(size_t channel, double playhead_frames, double loop_start_frame, double loop_end_frame) const;

    NonnullRefPtr<RenderAudioParam> m_playback_rate;
    NonnullRefPtr<RenderAudioParam> m_detune;
    Vector<float> m_playback_rate_values;
    Vector<float> m_detune_values;

    RefPtr<AudioBufferContents> m_buffer;
    bool m_loop { false };
    double m_loop_start { 0 };
    double m_loop_end { 0 };

    double m_offset { 0 };
    Optional<double> m_duration;

    // Playback position within the buffer contents, in buffer sample frames.
    double m_playhead { 0 };
    bool m_playhead_valid { false };
    double m_played_time { 0 };
};

// https://webaudio.github.io/web-audio-api/#DelayNode
class DelayRenderNode final : public RenderNode {
public:
    DelayRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> delay_time, double max_delay_time);

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override { callback(*m_delay_time); }

private:
    NonnullRefPtr<RenderAudioParam> m_delay_time;
    Vector<float> m_delay_values;
    double m_max_delay_time { 0 };
    Vector<Vector<float>> m_history;
    size_t m_write_index { 0 };
};

// https://webaudio.github.io/web-audio-api/#StereoPannerNode
class StereoPannerRenderNode final : public RenderNode {
public:
    StereoPannerRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> pan);

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override { callback(*m_pan); }

private:
    NonnullRefPtr<RenderAudioParam> m_pan;
    Vector<float> m_pan_values;
};

// https://webaudio.github.io/web-audio-api/#ChannelSplitterNode
class ChannelSplitterRenderNode final : public RenderNode {
public:
    ChannelSplitterRenderNode(NodeID, size_t quantum_size, size_t number_of_outputs);

    virtual void process(RenderGraph&, RenderContext const&) override;
};

// https://webaudio.github.io/web-audio-api/#ChannelMergerNode
class ChannelMergerRenderNode final : public RenderNode {
public:
    ChannelMergerRenderNode(NodeID, size_t quantum_size, size_t number_of_inputs);

    virtual void process(RenderGraph&, RenderContext const&) override;
};

// https://webaudio.github.io/web-audio-api/#BiquadFilterNode
class BiquadFilterRenderNode final : public RenderNode {
public:
    BiquadFilterRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune, NonnullRefPtr<RenderAudioParam> q, NonnullRefPtr<RenderAudioParam> gain);

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void handle_message(NodeMessage const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override
    {
        callback(*m_frequency);
        callback(*m_detune);
        callback(*m_q);
        callback(*m_gain);
    }

private:
    struct FilterState {
        double x1 { 0 };
        double x2 { 0 };
        double y1 { 0 };
        double y2 { 0 };
    };

    NonnullRefPtr<RenderAudioParam> m_frequency;
    NonnullRefPtr<RenderAudioParam> m_detune;
    NonnullRefPtr<RenderAudioParam> m_q;
    NonnullRefPtr<RenderAudioParam> m_gain;
    Vector<float> m_frequency_values;
    Vector<float> m_detune_values;
    Vector<float> m_q_values;
    Vector<float> m_gain_values;
    Bindings::BiquadFilterType m_type { Bindings::BiquadFilterType::Lowpass };
    Vector<FilterState> m_filter_states;
};

// https://webaudio.github.io/web-audio-api/#PannerNode
class PannerRenderNode final : public RenderNode {
public:
    struct Params {
        NonnullRefPtr<RenderAudioParam> position_x;
        NonnullRefPtr<RenderAudioParam> position_y;
        NonnullRefPtr<RenderAudioParam> position_z;
        NonnullRefPtr<RenderAudioParam> orientation_x;
        NonnullRefPtr<RenderAudioParam> orientation_y;
        NonnullRefPtr<RenderAudioParam> orientation_z;
    };
    struct ListenerParams {
        NonnullRefPtr<RenderAudioParam> position_x;
        NonnullRefPtr<RenderAudioParam> position_y;
        NonnullRefPtr<RenderAudioParam> position_z;
        NonnullRefPtr<RenderAudioParam> forward_x;
        NonnullRefPtr<RenderAudioParam> forward_y;
        NonnullRefPtr<RenderAudioParam> forward_z;
        NonnullRefPtr<RenderAudioParam> up_x;
        NonnullRefPtr<RenderAudioParam> up_y;
        NonnullRefPtr<RenderAudioParam> up_z;
    };

    PannerRenderNode(NodeID, size_t quantum_size, Params, ListenerParams);

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void handle_message(NodeMessage const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const& callback) override;

private:
    Params m_params;
    ListenerParams m_listener_params;
    SetPannerParameters m_config;

    // One buffer of computed values per panner and listener param, in the order of the Params and ListenerParams
    // struct members.
    Vector<Vector<float>> m_param_values;
};

// Passes the input through unchanged; used for nodes whose processing is not implemented yet but which should not
// silence the signal path, like AnalyserNode.
class PassthroughRenderNode final : public RenderNode {
public:
    PassthroughRenderNode(NodeID, size_t quantum_size);

    virtual void process(RenderGraph&, RenderContext const&) override;
};

}
