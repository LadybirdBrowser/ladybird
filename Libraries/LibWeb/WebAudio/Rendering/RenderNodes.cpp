/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio::Rendering {

DestinationRenderNode::DestinationRenderNode(NodeID node_id, size_t channel_count, size_t quantum_size)
    : RenderNode(RustFFI::web_audio_node_create_destination(node_id.value(), quantum_size, channel_count))
{
}
GainRenderNode::GainRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> gain)
    : RenderNode(RustFFI::web_audio_node_create_gain(node_id.value(), quantum_size, gain->rust_param()))
{
}
ConstantSourceRenderNode::ConstantSourceRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> offset)
    : RenderNode(RustFFI::web_audio_node_create_constant_source(node_id.value(), quantum_size, offset->rust_param()))
{
}
OscillatorRenderNode::OscillatorRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune)
    : RenderNode(RustFFI::web_audio_node_create_oscillator(node_id.value(), quantum_size, frequency->rust_param(), detune->rust_param()))
{
}
AudioBufferSourceRenderNode::AudioBufferSourceRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> playback_rate, NonnullRefPtr<RenderAudioParam> detune)
    : RenderNode(RustFFI::web_audio_node_create_audio_buffer_source(node_id.value(), quantum_size, playback_rate->rust_param(), detune->rust_param()))
{
}
DelayRenderNode::DelayRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> delay_time, double max_delay_time)
    : RenderNode(RustFFI::web_audio_node_create_delay(node_id.value(), quantum_size, delay_time->rust_param(), max_delay_time))
{
}
StereoPannerRenderNode::StereoPannerRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> pan)
    : RenderNode(RustFFI::web_audio_node_create_stereo_panner(node_id.value(), quantum_size, pan->rust_param()))
{
}
ChannelSplitterRenderNode::ChannelSplitterRenderNode(NodeID node_id, size_t quantum_size, size_t number_of_outputs)
    : RenderNode(RustFFI::web_audio_node_create_channel_splitter(node_id.value(), quantum_size, number_of_outputs))
{
}
ChannelMergerRenderNode::ChannelMergerRenderNode(NodeID node_id, size_t quantum_size, size_t number_of_inputs)
    : RenderNode(RustFFI::web_audio_node_create_channel_merger(node_id.value(), quantum_size, number_of_inputs))
{
}
BiquadFilterRenderNode::BiquadFilterRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune, NonnullRefPtr<RenderAudioParam> q, NonnullRefPtr<RenderAudioParam> gain)
    : RenderNode(RustFFI::web_audio_node_create_biquad_filter(node_id.value(), quantum_size, frequency->rust_param(), detune->rust_param(), q->rust_param(), gain->rust_param()))
{
}
IIRFilterRenderNode::IIRFilterRenderNode(NodeID node_id, size_t quantum_size, Vector<double> feedforward, Vector<double> feedback)
    : RenderNode(RustFFI::web_audio_node_create_iir_filter(node_id.value(), quantum_size, feedforward.span(), feedback.span()))
{
}
WaveShaperRenderNode::WaveShaperRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(RustFFI::web_audio_node_create_wave_shaper(node_id.value(), quantum_size))
{
}
ConvolverRenderNode::ConvolverRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(RustFFI::web_audio_node_create_convolver(node_id.value(), quantum_size))
{
}
PassthroughRenderNode::PassthroughRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(RustFFI::web_audio_node_create_passthrough(node_id.value(), quantum_size))
{
}
PannerRenderNode::PannerRenderNode(NodeID node_id, size_t quantum_size, Params params, ListenerParams listener_params)
    : RenderNode([&] {
        Array<void const*, 15> values { params.position_x->rust_param(), params.position_y->rust_param(), params.position_z->rust_param(),
            params.orientation_x->rust_param(), params.orientation_y->rust_param(), params.orientation_z->rust_param(),
            listener_params.position_x->rust_param(), listener_params.position_y->rust_param(), listener_params.position_z->rust_param(),
            listener_params.forward_x->rust_param(), listener_params.forward_y->rust_param(), listener_params.forward_z->rust_param(),
            listener_params.up_x->rust_param(), listener_params.up_y->rust_param(), listener_params.up_z->rust_param() };
        return RustFFI::web_audio_node_create_panner(node_id.value(), quantum_size, values.span());
    }())
{
}

}
