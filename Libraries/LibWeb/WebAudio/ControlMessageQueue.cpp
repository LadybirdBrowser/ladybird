/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio {

static_assert(to_underlying(Bindings::ChannelCountMode::Max) == to_underlying(RustFFI::ChannelCountMode::Max));
static_assert(to_underlying(Bindings::ChannelCountMode::ClampedMax) == to_underlying(RustFFI::ChannelCountMode::ClampedMax));
static_assert(to_underlying(Bindings::ChannelCountMode::Explicit) == to_underlying(RustFFI::ChannelCountMode::Explicit));
static_assert(to_underlying(Bindings::ChannelInterpretation::Speakers) == to_underlying(RustFFI::ChannelInterpretation::Speakers));
static_assert(to_underlying(Bindings::ChannelInterpretation::Discrete) == to_underlying(RustFFI::ChannelInterpretation::Discrete));
static_assert(to_underlying(Bindings::OscillatorType::Sine) == to_underlying(RustFFI::OscillatorType::Sine));
static_assert(to_underlying(Bindings::OscillatorType::Square) == to_underlying(RustFFI::OscillatorType::Square));
static_assert(to_underlying(Bindings::OscillatorType::Sawtooth) == to_underlying(RustFFI::OscillatorType::Sawtooth));
static_assert(to_underlying(Bindings::OscillatorType::Triangle) == to_underlying(RustFFI::OscillatorType::Triangle));
static_assert(to_underlying(Bindings::OscillatorType::Custom) == to_underlying(RustFFI::OscillatorType::Custom));
static_assert(to_underlying(Bindings::PanningModelType::Equalpower) == to_underlying(RustFFI::PanningModelType::Equalpower));
static_assert(to_underlying(Bindings::PanningModelType::HRTF) == to_underlying(RustFFI::PanningModelType::HRTF));
static_assert(to_underlying(Bindings::DistanceModelType::Linear) == to_underlying(RustFFI::DistanceModelType::Linear));
static_assert(to_underlying(Bindings::DistanceModelType::Inverse) == to_underlying(RustFFI::DistanceModelType::Inverse));
static_assert(to_underlying(Bindings::DistanceModelType::Exponential) == to_underlying(RustFFI::DistanceModelType::Exponential));
static_assert(to_underlying(Bindings::OverSampleType::None) == to_underlying(RustFFI::OverSampleType::None));
static_assert(to_underlying(Bindings::OverSampleType::_2x) == to_underlying(RustFFI::OverSampleType::_2x));
static_assert(to_underlying(Bindings::OverSampleType::_4x) == to_underlying(RustFFI::OverSampleType::_4x));

ControlMessageQueue::ControlMessageQueue()
    : m_queue(RustFFI::web_audio_control_queue_create())
{
}
ControlMessageQueue::~ControlMessageQueue() { RustFFI::web_audio_control_queue_destroy(m_queue); }
void* ControlMessageQueue::drain() { return RustFFI::web_audio_control_queue_drain(m_queue); }

void ControlMessageQueue::enqueue(ControlMessage message)
{
    auto shared_object = []<typename T>(T* pointer) {
        return RustFFI::FfiSharedCppObject { pointer,
            [](void* pointer) { static_cast<T*>(pointer)->ref(); },
            [](void* pointer) { static_cast<T*>(pointer)->unref(); } };
    };
    void* rust_message = message.visit(
        [](AddNode& add_node) -> void* {
            auto* node = add_node.node.leak_ptr();
            auto* rust_node = node->release_rust_node();
            delete node;
            return RustFFI::web_audio_message_add_node(rust_node);
        },
        [](RemoveNode const& value) -> void* { return RustFFI::web_audio_message_remove_node(value.node_id.value()); },
        [](ReplaceConnections const& value) -> void* {
            Vector<RustFFI::OutgoingNodeConnection> nodes;
            for (auto const& connection : value.node_connections)
                nodes.append({ connection.destination.value(), connection.output_index, connection.input_index });
            Vector<RustFFI::FfiOutgoingParamConnection> params;
            for (auto const& connection : value.param_connections)
                params.append({ connection.destination->rust_param(), connection.output_index });
            return RustFFI::web_audio_message_replace_connections(value.source.value(), nodes.span(), params.span());
        },
        [](SetChannelConfig const& value) -> void* { return RustFFI::web_audio_message_set_channel_config(value.node_id.value(), value.channel_count, static_cast<RustFFI::ChannelCountMode>(value.channel_count_mode), static_cast<RustFFI::ChannelInterpretation>(value.channel_interpretation)); },
        [&](NodeMessage const& node_message) -> void* {
            return node_message.visit(
                [](StartSource const& value) -> void* { return RustFFI::web_audio_message_start_source(value.node_id.value(), value.when); },
                [](StopSource const& value) -> void* { return RustFFI::web_audio_message_stop_source(value.node_id.value(), value.when); },
                [](StartBufferSource const& value) -> void* { return RustFFI::web_audio_message_start_buffer_source(value.node_id.value(), value.when, value.offset, value.duration.has_value(), value.duration.value_or(0.), value.buffer ? value.buffer->rust_data() : nullptr); },
                [](SetBufferSourceParameters const& value) -> void* { return RustFFI::web_audio_message_set_buffer_source_parameters(value.node_id.value(), value.loop, value.loop_start, value.loop_end, value.buffer ? value.buffer->rust_data() : nullptr, value.update_buffer); },
                [](SetOscillatorWaveform const& value) -> void* { return RustFFI::web_audio_message_set_oscillator_waveform(value.node_id.value(), static_cast<RustFFI::OscillatorType>(value.type), value.periodic_wave ? value.periodic_wave->rust_data() : nullptr); },
                [](SetBiquadFilterType const& value) -> void* { return RustFFI::web_audio_message_set_biquad_filter_type(value.node_id.value(), static_cast<RustFFI::BiquadFilterType>(value.type)); },
                [&](SetConvolverKernel const& value) -> void* { return RustFFI::web_audio_message_set_convolver_kernel(value.node_id.value(), value.kernel ? value.kernel->rust_kernel() : nullptr, value.delay_line ? value.delay_line->rust_delay_line() : nullptr); },
                [](SetPannerParameters const& value) -> void* { return RustFFI::web_audio_message_set_panner_parameters(value.node_id.value(), static_cast<RustFFI::PanningModelType>(value.panning_model), static_cast<RustFFI::DistanceModelType>(value.distance_model), value.ref_distance, value.max_distance, value.rolloff_factor, value.cone_inner_angle, value.cone_outer_angle, value.cone_outer_gain); },
                [](SetWaveShaperParameters const& value) -> void* { return RustFFI::web_audio_message_set_wave_shaper_parameters(value.node_id.value(), value.curve ? value.curve->rust_data() : nullptr, static_cast<RustFFI::OverSampleType>(value.oversample)); },
                [&](SetMediaStreamSourceRing const& value) -> void* { return RustFFI::web_audio_message_set_media_stream_source_ring(value.node_id.value(), shared_object(value.ring.ptr()), value.channel_count); });
        });
    RustFFI::web_audio_control_queue_enqueue(m_queue, rust_message);
}

}
