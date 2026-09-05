/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibWeb/Bindings/BiquadFilterNode.h>
#include <LibWeb/Bindings/OscillatorNode.h>
#include <LibWeb/Bindings/PannerNode.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

struct StartSource {
    NodeID node_id { 0 };
    double when { 0.0 };
};

struct StopSource {
    NodeID node_id { 0 };
    double when { 0.0 };
};

// Starts an AudioBufferSourceNode, transferring the acquired buffer contents along with the playback range.
struct StartBufferSource {
    NodeID node_id { 0 };
    double when { 0.0 };
    double offset { 0.0 };
    Optional<double> duration;
    RefPtr<Rendering::AudioBufferContents> buffer;
};

// Updates an AudioBufferSourceNode's loop parameters, and its buffer if it was assigned after the node started.
struct SetBufferSourceParameters {
    NodeID node_id { 0 };
    bool loop { false };
    double loop_start { 0.0 };
    double loop_end { 0.0 };
    RefPtr<Rendering::AudioBufferContents> buffer;
    bool update_buffer { false };
};

struct SetOscillatorWaveform {
    NodeID node_id { 0 };
    Bindings::OscillatorType type { Bindings::OscillatorType::Sine };
    RefPtr<Rendering::PeriodicWaveData> periodic_wave;
};

struct SetBiquadFilterType {
    NodeID node_id { 0 };
    Bindings::BiquadFilterType type { Bindings::BiquadFilterType::Lowpass };
};

struct SetPannerParameters {
    NodeID node_id { 0 };
    Bindings::PanningModelType panning_model { Bindings::PanningModelType::Equalpower };
    Bindings::DistanceModelType distance_model { Bindings::DistanceModelType::Inverse };
    double ref_distance { 1 };
    double max_distance { 10000 };
    double rolloff_factor { 1 };
    double cone_inner_angle { 360 };
    double cone_outer_angle { 360 };
    double cone_outer_gain { 0 };
};

// Attaches the ring buffer that carries a MediaStreamTrack's audio into a
// MediaStreamAudioSourceNode's render node. Shipped as a message rather than a constructor
// argument so the ring can be replaced without rebuilding the render node.
struct SetMediaStreamSourceRing {
    NodeID node_id { 0 };
    RefPtr<Media::SpscAudioFrameRing> ring;
    u32 channel_count { 0 };
};

// A control message that updates the state of a single render node.
using NodeMessage = Variant<StartSource, StopSource, StartBufferSource, SetBufferSourceParameters, SetOscillatorWaveform, SetBiquadFilterType, SetPannerParameters, SetMediaStreamSourceRing>;

inline NodeID node_message_target(NodeMessage const& message)
{
    return message.visit([](auto const& node_message) { return node_message.node_id; });
}

}
