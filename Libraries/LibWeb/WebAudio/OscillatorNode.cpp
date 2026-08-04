/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OscillatorNode.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OscillatorNode);

OscillatorNode::~OscillatorNode() = default;

void OscillatorNode::queue_waveform_update()
{
    RefPtr<Rendering::PeriodicWaveData> periodic_wave;
    if (m_periodic_wave)
        periodic_wave = m_periodic_wave->render_data();

    context()->queue_control_message(NodeMessage { SetOscillatorWaveform {
        .node_id = node_id(),
        .type = m_type,
        .periodic_wave = move(periodic_wave),
    } });
}

WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> OscillatorNode::create(GC::Ref<BaseAudioContext> context, OscillatorOptions const& options)
{
    auto node = GC::Heap::the().allocate<OscillatorNode>(context, options);

    if (options.type == OscillatorType::Custom)
        node->set_periodic_wave(options.periodic_wave);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#OscillatorNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    node->queue_render_node_creation(make<Rendering::OscillatorRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size(), node->m_frequency->render_param(), node->m_detune->render_param()));
    node->queue_waveform_update();

    return node;
}

WebIDL::ExceptionOr<void> OscillatorNode::validate_options(OscillatorOptions const& options)
{
    if (options.type == OscillatorType::Custom && !options.periodic_wave)
        return WebIDL::InvalidStateError::create("Oscillator node type 'custom' requires PeriodicWave to be provided"_utf16);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-oscillatornode
WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> OscillatorNode::create_for_constructor(GC::Ref<BaseAudioContext> context, OscillatorOptions const& options)
{
    TRY(validate_options(options));
    return create(context, options);
}

OscillatorNode::OscillatorNode(GC::Ref<BaseAudioContext> context, OscillatorOptions const& options)
    : AudioScheduledSourceNode(context)
    , m_type(options.type)
    , m_frequency(AudioParam::create(context, this, options.frequency, -context->nyquist_frequency(), context->nyquist_frequency(), AutomationRate::ARate))
    , m_detune(AudioParam::create(context, this, options.detune, -1200 * AK::log2(NumericLimits<float>::max()), 1200 * AK::log2(NumericLimits<float>::max()), AutomationRate::ARate))
{
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
OscillatorType OscillatorNode::type() const
{
    return m_type;
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
WebIDL::ExceptionOr<void> OscillatorNode::set_type(OscillatorType type)
{
    if (type == OscillatorType::Custom && m_type != OscillatorType::Custom)
        return WebIDL::InvalidStateError::create("Oscillator node type cannot be changed to 'custom'"_utf16);

    // FIXME: An appropriate PeriodicWave should be set here based on the given type.
    set_periodic_wave(nullptr);

    m_type = type;
    queue_waveform_update();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-setperiodicwave
void OscillatorNode::set_periodic_wave(GC::Ptr<PeriodicWave> periodic_wave)
{
    m_periodic_wave = periodic_wave;
    m_type = OscillatorType::Custom;
}

void OscillatorNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_frequency);
    visitor.visit(m_detune);
    visitor.visit(m_periodic_wave);
}

}
