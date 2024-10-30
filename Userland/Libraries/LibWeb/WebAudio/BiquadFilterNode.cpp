/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioParamPrototype.h>
#include <LibWeb/Bindings/BiquadFilterNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(BiquadFilterNode);

BiquadFilterNode::BiquadFilterNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, BiquadFilterOptions const& options)
    : AudioNode(realm, context)
    , m_type(options.type)
    , m_frequency(AudioParam::create(realm, options.frequency, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, options.detune, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_q(AudioParam::create(realm, options.q, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_gain(AudioParam::create(realm, options.gain, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
{
}

BiquadFilterNode::~BiquadFilterNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-type
void BiquadFilterNode::set_type(Bindings::BiquadFilterType type)
{
    m_type = type;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-type
Bindings::BiquadFilterType BiquadFilterNode::type() const
{
    return m_type;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-frequency
JS::NonnullGCPtr<AudioParam> BiquadFilterNode::frequency() const
{
    return m_frequency;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-detune
JS::NonnullGCPtr<AudioParam> BiquadFilterNode::detune() const
{
    return m_detune;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-q
JS::NonnullGCPtr<AudioParam> BiquadFilterNode::q() const
{
    return m_q;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-gain
JS::NonnullGCPtr<AudioParam> BiquadFilterNode::gain() const
{
    return m_gain;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-getfrequencyresponse
WebIDL::ExceptionOr<void> BiquadFilterNode::get_frequency_response(JS::Handle<WebIDL::BufferSource> const& frequency_hz, JS::Handle<WebIDL::BufferSource> const& mag_response, JS::Handle<WebIDL::BufferSource> const& phase_response)
{
    (void)frequency_hz;
    (void)mag_response;
    (void)phase_response;
    dbgln("FIXME: Implement BiquadFilterNode::get_frequency_response(Float32Array, Float32Array, Float32Array)");
    return {};
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> BiquadFilterNode::create(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, BiquadFilterOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-biquadfilternode
WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> BiquadFilterNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, BiquadFilterOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = realm.vm().heap().allocate<BiquadFilterNode>(realm, realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#BiquadFilterNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to yes

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

void BiquadFilterNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BiquadFilterNode);
}

void BiquadFilterNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_frequency);
    visitor.visit(m_detune);
    visitor.visit(m_q);
    visitor.visit(m_gain);
}

}
