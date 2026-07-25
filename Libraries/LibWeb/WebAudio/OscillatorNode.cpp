/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OscillatorNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OscillatorNode.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OscillatorNode);

OscillatorNode::~OscillatorNode() = default;

WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> OscillatorNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::OscillatorOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-oscillatornode
WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> OscillatorNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::OscillatorOptions const& options)
{
    if (options.type == Bindings::OscillatorType::Custom && !options.periodic_wave)
        return WebIDL::InvalidStateError::create(realm, "Oscillator node type 'custom' requires PeriodicWave to be provided"_utf16);

    auto node = realm.create<OscillatorNode>(realm, context, options);

    if (options.type == Bindings::OscillatorType::Custom)
        node->set_periodic_wave(options.periodic_wave);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#OscillatorNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    node->queue_render_node_creation(make<Rendering::OscillatorRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size(), node->m_frequency->render_param(), node->m_detune->render_param()));
    node->queue_waveform_update();

    return node;
}

void OscillatorNode::queue_waveform_update()
{
    RefPtr<Rendering::PeriodicWaveData> wave_data;
    if (m_type == Bindings::OscillatorType::Custom && m_periodic_wave)
        wave_data = m_periodic_wave->render_data();
    context()->queue_control_message(NodeMessage { SetOscillatorWaveform { node_id(), m_type, move(wave_data) } });
}

OscillatorNode::OscillatorNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::OscillatorOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_type(options.type)
    , m_frequency(AudioParam::create(realm, context, this, options.frequency, -context->nyquist_frequency(), context->nyquist_frequency(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, context, this, options.detune, -1200 * AK::log2(NumericLimits<float>::max()), 1200 * AK::log2(NumericLimits<float>::max()), Bindings::AutomationRate::ARate))
{
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
Bindings::OscillatorType OscillatorNode::type() const
{
    return m_type;
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
WebIDL::ExceptionOr<void> OscillatorNode::set_type(Bindings::OscillatorType type)
{
    if (type == Bindings::OscillatorType::Custom && m_type != Bindings::OscillatorType::Custom)
        return WebIDL::InvalidStateError::create(realm(), "Oscillator node type cannot be changed to 'custom'"_utf16);

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
    m_type = Bindings::OscillatorType::Custom;
    queue_waveform_update();
}

void OscillatorNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OscillatorNode);
    Base::initialize(realm);
}

void OscillatorNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_frequency);
    visitor.visit(m_detune);
    visitor.visit(m_periodic_wave);
}

}
