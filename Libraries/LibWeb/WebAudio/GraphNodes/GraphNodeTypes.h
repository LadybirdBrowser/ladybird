/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

#define ENUMERATE_GRAPH_NODE_TYPES(X)                     \
    X(AudioListener, "AudioListener")                     \
    X(Destination, "Destination")                         \
    X(Oscillator, "Oscillator")                           \
    X(AudioBufferSource, "AudioBufferSource")             \
    X(MediaElementAudioSource, "MediaElementAudioSource") \
    X(MediaStreamAudioSource, "MediaStreamAudioSource")   \
    X(ConstantSource, "ConstantSource")                   \
    X(Convolver, "Convolver")                             \
    X(BiquadFilter, "BiquadFilter")                       \
    X(IIRFilter, "IIRFilter")                             \
    X(WaveShaper, "WaveShaper")                           \
    X(DynamicsCompressor, "DynamicsCompressor")           \
    X(Gain, "Gain")                                       \
    X(Delay, "Delay")                                     \
    X(Panner, "Panner")                                   \
    X(StereoPanner, "StereoPanner")                       \
    X(ChannelSplitter, "ChannelSplitter")                 \
    X(ChannelMerger, "ChannelMerger")                     \
    X(Analyser, "Analyser")                               \
    X(AudioWorklet, "AudioWorklet")                       \
    X(ScriptProcessor, "ScriptProcessor")                 \
    X(OhNoes, "OhNoes")

enum class GraphNodeType : u8 {
#define __ENUMERATE_GRAPH_NODE_TYPE(name, _debug_name) name,
    ENUMERATE_GRAPH_NODE_TYPES(__ENUMERATE_GRAPH_NODE_TYPE)
#undef __ENUMERATE_GRAPH_NODE_TYPE
        Unknown,
};

constexpr StringView graph_node_type_name(GraphNodeType type)
{
    switch (type) {
#define __ENUMERATE_GRAPH_NODE_TYPE(name, debug_name) \
    case GraphNodeType::name:                         \
        return debug_name##sv;
        ENUMERATE_GRAPH_NODE_TYPES(__ENUMERATE_GRAPH_NODE_TYPE)
#undef __ENUMERATE_GRAPH_NODE_TYPE
    case GraphNodeType::Unknown:
        return "Unknown"sv;
    }
    VERIFY_NOT_REACHED();
}

using MediaElementAudioSourceProviderID = u64;
using MediaStreamAudioSourceProviderID = u64;

enum class GraphUpdateKind : u8 {
    None,
    Parameter,
    Topology,
    RebuildRequired,
};

enum class OscillatorType : u8 {
    Sine = 0,
    Square = 1,
    Sawtooth = 2,
    Triangle = 3,
    Custom = 4,
};

enum class BiquadFilterType : u8 {
    Lowpass = 0,
    Highpass = 1,
    Bandpass = 2,
    Lowshelf = 3,
    Highshelf = 4,
    Peaking = 5,
    Notch = 6,
    Allpass = 7,
};

enum class OverSampleType : u8 {
    None = 0,
    X2 = 1,
    X4 = 2,
};

enum class ChannelCountMode : u8 {
    Max = 0,
    ClampedMax = 1,
    Explicit = 2,
};

enum class ChannelInterpretation : u8 {
    Speakers = 0,
    Discrete = 1,
};

enum class AutomationRate : u8 {
    ARate = 0,
    KRate = 1,
};

enum class PanningModelType : u8 {
    EqualPower = 0,
    HRTF = 1,
};

enum class DistanceModelType : u8 {
    Linear = 0,
    Inverse = 1,
    Exponential = 2,
};

// AudioParam slots for each GraphNodeType.
struct RenderParamLayout {
    static constexpr size_t gain_param_count = 1;
    static constexpr size_t oscillator_param_count = 2;
    static constexpr size_t buffer_source_param_count = 2;
    static constexpr size_t constant_source_param_count = 1;
    static constexpr size_t biquad_filter_param_count = 4;
    static constexpr size_t dynamics_compressor_param_count = 5;
    static constexpr size_t delay_param_count = 1;
    static constexpr size_t panner_param_count = 6;
    static constexpr size_t stereo_panner_param_count = 1;
    static constexpr size_t audio_listener_param_count = 9;

    static constexpr size_t param_count(Render::GraphNodeType type)
    {
        switch (type) {
        case Render::GraphNodeType::Gain:
            return gain_param_count;
        case Render::GraphNodeType::Oscillator:
            return oscillator_param_count;
        case Render::GraphNodeType::AudioBufferSource:
            return buffer_source_param_count;
        case Render::GraphNodeType::ConstantSource:
            return constant_source_param_count;
        case Render::GraphNodeType::Convolver:
            return 0;
        case Render::GraphNodeType::BiquadFilter:
            return biquad_filter_param_count;
        case Render::GraphNodeType::WaveShaper:
            return 0;
        case Render::GraphNodeType::DynamicsCompressor:
            return dynamics_compressor_param_count;
        case Render::GraphNodeType::Delay:
            return delay_param_count;
        case Render::GraphNodeType::Panner:
            return panner_param_count;
        case Render::GraphNodeType::StereoPanner:
            return stereo_panner_param_count;
        case Render::GraphNodeType::AudioListener:
            return audio_listener_param_count;
        default:
            return 0;
        }
    }
};

// Per-node param indices.
struct GainParamIndex {
    static constexpr size_t gain = 0;
};

struct OscillatorParamIndex {
    static constexpr size_t frequency = 0;
    static constexpr size_t detune = 1;
};

struct AudioBufferSourceParamIndex {
    static constexpr size_t playback_rate = 0;
    static constexpr size_t detune = 1;
};

struct ConstantSourceParamIndex {
    static constexpr size_t offset = 0;
};

struct AudioListenerParamIndex {
    static constexpr size_t position_x = 0;
    static constexpr size_t position_y = 1;
    static constexpr size_t position_z = 2;
    static constexpr size_t forward_x = 3;
    static constexpr size_t forward_y = 4;
    static constexpr size_t forward_z = 5;
    static constexpr size_t up_x = 6;
    static constexpr size_t up_y = 7;
    static constexpr size_t up_z = 8;
};

struct BiquadFilterParamIndex {
    static constexpr size_t frequency = 0;
    static constexpr size_t detune = 1;
    static constexpr size_t q = 2;
    static constexpr size_t gain = 3;
};

struct DynamicsCompressorParamIndex {
    static constexpr size_t threshold = 0;
    static constexpr size_t knee = 1;
    static constexpr size_t ratio = 2;
    static constexpr size_t attack = 3;
    static constexpr size_t release = 4;
};

struct DelayParamIndex {
    static constexpr size_t delay_time = 0;
};

struct PannerParamIndex {
    static constexpr size_t position_x = 0;
    static constexpr size_t position_y = 1;
    static constexpr size_t position_z = 2;
    static constexpr size_t orientation_x = 3;
    static constexpr size_t orientation_y = 4;
    static constexpr size_t orientation_z = 5;
};

struct StereoPannerParamIndex {
    static constexpr size_t pan = 0;
};

// Render-thread snapshot description of an audio graph.
// https://webaudio.github.io/web-audio-api/#rendering-thread

struct GraphParamConnection {

    NodeID source;
    NodeID destination;
    size_t source_output_index { 0 };
    // Index into the destination node's AudioParam list.
    // This is intentionally a per-node namespace to avoid global enums that grow holes.
    size_t destination_param_index { 0 };
};

// Minimal render-thread representation of AudioParam automation.
enum class GraphAutomationSegmentType : u8 {

    Constant,
    LinearRamp,
    ExponentialRamp,
    Target,
    ValueCurve,
};

struct GraphAutomationSegment {

    GraphAutomationSegmentType type { GraphAutomationSegmentType::Constant };

    // Segment boundaries in the AudioContext timeline, in seconds.
    // These preserve sub-sample scheduling precision (event times can fall between sample frames).
    f64 start_time { 0.0 }; // inclusive
    f64 end_time { 0.0 };   // exclusive

    // Original value-curve timing, retained even if the segment is truncated by a later event.
    f64 curve_start_time { 0.0 };
    f64 curve_duration { 0.0 };

    size_t start_frame { 0 }; // inclusive
    size_t end_frame { 0 };   // exclusive

    f32 start_value { 0.0f };
    f32 end_value { 0.0f };

    f32 time_constant { 0.0f };
    f32 target { 0.0f };

    Vector<f32> curve;
};

struct GraphParamAutomation {

    NodeID destination;
    size_t destination_param_index { 0 };

    // Base value at time 0 for this AudioParam.
    f32 initial_value { 0.0f };

    // Used for NaN -> defaultValue in computedValue.
    f32 default_value { 0.0f };

    // Used for clamping when applying computedValue to the DSP parameter.
    f32 min_value { 0.0f };
    f32 max_value { 0.0f };

    AutomationRate automation_rate { AutomationRate::ARate };
    Vector<GraphAutomationSegment> segments;
};

struct GraphConnection {

    NodeID source;
    NodeID destination;
    size_t source_output_index { 0 };
    size_t destination_input_index { 0 };
};

}
