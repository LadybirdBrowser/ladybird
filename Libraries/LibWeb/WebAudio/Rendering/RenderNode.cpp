/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio::Rendering {

RenderAudioParam::RenderAudioParam(NonnullRefPtr<AudioParamTimeline> timeline, float min_value, float max_value, Bindings::AutomationRate automation_rate)
    : m_param(RustFFI::web_audio_render_param_create(timeline->rust_timeline(), min_value, max_value, static_cast<RustFFI::AutomationRate>(automation_rate)))
{
    static_assert(to_underlying(Bindings::AutomationRate::ARate) == to_underlying(RustFFI::AutomationRate::ARate));
    static_assert(to_underlying(Bindings::AutomationRate::KRate) == to_underlying(RustFFI::AutomationRate::KRate));
}

RenderAudioParam::~RenderAudioParam()
{
    RustFFI::web_audio_render_param_destroy(m_param);
}

void RenderAudioParam::set_automation_rate(Bindings::AutomationRate rate)
{
    RustFFI::web_audio_render_param_set_automation_rate(m_param, static_cast<RustFFI::AutomationRate>(rate));
}

Bindings::AutomationRate RenderAudioParam::automation_rate() const
{
    return static_cast<Bindings::AutomationRate>(RustFFI::web_audio_render_param_automation_rate(m_param));
}

void RenderAudioParam::clear_inputs()
{
    RustFFI::web_audio_render_param_clear_inputs(m_param);
}

void RenderAudioParam::add_input(NodeID source, size_t output_index)
{
    RustFFI::web_audio_render_param_add_input(m_param, source.value(), output_index);
}

void RenderAudioParam::compute(RenderGraph& graph, RenderContext const& context, Span<float> output)
{
    RustFFI::RenderContext rust_context { context.sample_rate, context.quantum_size, context.quantum_start_frame };
    RustFFI::web_audio_render_param_compute(m_param, &graph, [](void* graph, u64 source, size_t output_index, RustFFI::RenderContext const* context) -> void const* {
        RenderContext render_context { context->sample_rate, context->quantum_size, context->quantum_start_frame };
        auto bus = static_cast<RenderGraph*>(graph)->pull_output(source, output_index, render_context);
        return bus.has_value() ? bus->rust_bus() : nullptr; }, &rust_context, output);
}

RenderNode::RenderNode(NodeID node_id, size_t input_count, size_t output_count, size_t quantum_size, size_t output_channel_count)
    : m_external(true)
{
    m_node = RustFFI::web_audio_node_create_external(node_id.value(), input_count, output_count, quantum_size, output_channel_count, this, {
                                                                                                                                               .process = [](void* node, void const* graph, RustFFI::RenderContext const* context) {
            RenderGraph borrowed_graph(const_cast<void*>(graph), false);
            RenderContext render_context { context->sample_rate, context->quantum_size, context->quantum_start_frame };
            static_cast<RenderNode*>(node)->process(borrowed_graph, render_context); },
                                                                                                                                               .set_ring = [](void* node, void* ring, u32 channel_count) { static_cast<RenderNode*>(node)->handle_message(NodeMessage { SetMediaStreamSourceRing { {}, static_cast<Audio::SpscAudioFrameRing*>(ring), channel_count } }); },
                                                                                                                                               .for_each_param = [](void* node, void* context, void (*callback)(void*, void const*)) { static_cast<RenderNode*>(node)->for_each_param([&](RenderAudioParam& param) { callback(context, param.rust_param()); }); },
                                                                                                                                               .ended = [](void* node) { return static_cast<RenderNode*>(node)->take_ended_notification(); },
                                                                                                                                               .destroy = [](void* node) { if (static_cast<RenderNode*>(node)->m_transferred) delete static_cast<RenderNode*>(node); },
                                                                                                                                           });
}
RenderNode::RenderNode(void* node)
    : m_node(node)
{
}
RenderNode::~RenderNode()
{
    if (m_owned)
        RustFFI::web_audio_node_destroy(m_node);
}
void* RenderNode::release_rust_node()
{
    m_owned = false;
    m_transferred = true;
    return m_node;
}
NodeID RenderNode::node_id() const { return RustFFI::web_audio_node_id(m_node); }
size_t RenderNode::input_count() const { return RustFFI::web_audio_node_input_count(m_node); }
size_t RenderNode::output_count() const { return RustFFI::web_audio_node_output_count(m_node); }
AudioBus RenderNode::output(size_t index) const { return AudioBus::borrow(RustFFI::web_audio_node_output(m_node, index)); }
size_t RenderNode::channel_count() const { return RustFFI::web_audio_node_channel_count(m_node); }
Bindings::ChannelCountMode RenderNode::channel_count_mode() const { return static_cast<Bindings::ChannelCountMode>(RustFFI::web_audio_node_channel_count_mode(m_node)); }
Bindings::ChannelInterpretation RenderNode::channel_interpretation() const { return static_cast<Bindings::ChannelInterpretation>(RustFFI::web_audio_node_channel_interpretation(m_node)); }
void RenderNode::set_channel_count(size_t count) { RustFFI::web_audio_node_set_channel_count(m_node, count); }
void RenderNode::set_channel_count_mode(Bindings::ChannelCountMode mode) { RustFFI::web_audio_node_set_channel_count_mode(m_node, static_cast<RustFFI::ChannelCountMode>(mode)); }
void RenderNode::set_channel_interpretation(Bindings::ChannelInterpretation interpretation) { RustFFI::web_audio_node_set_channel_interpretation(m_node, static_cast<RustFFI::ChannelInterpretation>(interpretation)); }
size_t RenderNode::outgoing_node_connection_count() const { return RustFFI::web_audio_node_outgoing_node_connection_count(m_node); }
size_t RenderNode::outgoing_param_connection_count() const { return RustFFI::web_audio_node_outgoing_param_connection_count(m_node); }
bool RenderNode::has_input_connections(size_t input) const { return RustFFI::web_audio_node_has_input_connections(m_node, input); }
AudioBus RenderNode::pull_input(RenderGraph& graph, RenderContext const& context, size_t input_index)
{
    RustFFI::RenderContext render_context { context.sample_rate, context.quantum_size, context.quantum_start_frame };
    return AudioBus::borrow(const_cast<void*>(RustFFI::web_audio_node_pull_input(m_node, graph.rust_graph(), &render_context, input_index)));
}

}
