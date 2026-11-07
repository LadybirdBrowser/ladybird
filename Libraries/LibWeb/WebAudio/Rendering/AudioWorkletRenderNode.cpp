/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/AudioWorkletRenderNode.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

static void* create_render_node(NodeID node_id, size_t input_count, size_t output_count, size_t quantum_size, Vector<u32> const& output_channel_capacities, size_t channel_count, Bindings::ChannelCountMode channel_count_mode, Bindings::ChannelInterpretation channel_interpretation, AudioWorkletPipe& pipe, Vector<NonnullRefPtr<RenderAudioParam>> const& params)
{
    Vector<void const*> native_params;
    for (auto const& param : params)
        native_params.append(param->rust_param());
    return RustFFI::web_audio_node_create_audio_worklet(node_id.value(), input_count, output_count, quantum_size, output_channel_capacities.span(), channel_count, static_cast<RustFFI::ChannelCountMode>(channel_count_mode), static_cast<RustFFI::ChannelInterpretation>(channel_interpretation), pipe.rust_pipe(), native_params.span());
}

AudioWorkletRenderNode::AudioWorkletRenderNode(NodeID node_id, size_t input_count, size_t output_count, size_t quantum_size,
    Vector<u32> output_channel_capacities,
    size_t channel_count, Bindings::ChannelCountMode channel_count_mode, Bindings::ChannelInterpretation channel_interpretation,
    NonnullRefPtr<AudioWorkletPipe> pipe,
    Vector<NonnullRefPtr<RenderAudioParam>> params)
    : RenderNode(create_render_node(node_id, input_count, output_count, quantum_size, output_channel_capacities, channel_count, channel_count_mode, channel_interpretation, pipe, params))
{
}

}
