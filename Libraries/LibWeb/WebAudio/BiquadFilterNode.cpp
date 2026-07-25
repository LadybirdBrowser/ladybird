/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Bindings/BiquadFilterNode.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioArray.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/Rendering/BiquadCoefficients.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(BiquadFilterNode);

BiquadFilterNode::BiquadFilterNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::BiquadFilterOptions const& options)
    : AudioNode(realm, context)
    , m_type(options.type)
    , m_frequency(AudioParam::create(realm, context, this, options.frequency, 0, context->nyquist_frequency(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, context, this, options.detune, -1200 * AK::log2(NumericLimits<float>::max()), 1200 * AK::log2(NumericLimits<float>::max()), Bindings::AutomationRate::ARate))
    , m_q(AudioParam::create(realm, context, this, options.q, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_gain(AudioParam::create(realm, context, this, options.gain, NumericLimits<float>::lowest(), 40 * AK::log10(NumericLimits<float>::max()), Bindings::AutomationRate::ARate))
{
}

BiquadFilterNode::~BiquadFilterNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-type
void BiquadFilterNode::set_type(Bindings::BiquadFilterType type)
{
    m_type = type;
    context()->queue_control_message(NodeMessage { SetBiquadFilterType { node_id(), type } });
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-type
Bindings::BiquadFilterType BiquadFilterNode::type() const
{
    return m_type;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-frequency
GC::Ref<AudioParam> BiquadFilterNode::frequency() const
{
    return m_frequency;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-detune
GC::Ref<AudioParam> BiquadFilterNode::detune() const
{
    return m_detune;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-q
GC::Ref<AudioParam> BiquadFilterNode::q() const
{
    return m_q;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-gain
GC::Ref<AudioParam> BiquadFilterNode::gain() const
{
    return m_gain;
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-getfrequencyresponse
WebIDL::ExceptionOr<void> BiquadFilterNode::get_frequency_response(GC::Ref<JS::Float32Array> frequency_hz, GC::Ref<JS::Float32Array> mag_response, GC::Ref<JS::Float32Array> phase_response)
{
    auto length = float32_array_length(*frequency_hz);

    // If these arrays do not have the same length, an InvalidAccessError MUST be thrown.
    if (float32_array_length(*mag_response) != length || float32_array_length(*phase_response) != length)
        return WebIDL::InvalidAccessError::create(realm(), "Arrays must have the same length"_utf16);

    auto frequencies = copy_float32_array(*frequency_hz);
    Vector<float> magnitudes;
    magnitudes.resize(length);
    Vector<float> phases;
    phases.resize(length);

    // The filter response is evaluated with the current parameter values at the context's current time.
    auto current_time = context()->current_time();
    auto nyquist_frequency = context()->nyquist_frequency();
    auto frequency = m_frequency->intrinsic_value_at_time(current_time);
    auto detune = m_detune->intrinsic_value_at_time(current_time);
    auto computed_frequency = frequency * AK::exp2(detune / 1200.);
    auto normalized_frequency = clamp(computed_frequency / nyquist_frequency, 0., 1.);
    auto coefficients = Rendering::compute_biquad_coefficients(m_type, normalized_frequency, m_q->intrinsic_value_at_time(current_time), m_gain->intrinsic_value_at_time(current_time));

    for (size_t index = 0; index < length; ++index) {
        auto response_frequency = frequencies[index];

        // Frequencies outside the nominal range produce NaN in both output arrays.
        if (isnan(response_frequency) || response_frequency < 0 || response_frequency > nyquist_frequency) {
            magnitudes[index] = NAN;
            phases[index] = NAN;
            continue;
        }
        Rendering::biquad_frequency_response(coefficients, response_frequency / nyquist_frequency, magnitudes[index], phases[index]);
    }

    overwrite_float32_array(*mag_response, magnitudes);
    overwrite_float32_array(*phase_response, phases);
    return {};
}

WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> BiquadFilterNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::BiquadFilterOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-biquadfilternode
WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> BiquadFilterNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::BiquadFilterOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.
    auto node = realm.create<BiquadFilterNode>(realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#BiquadFilterNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to yes

    TRY(node->initialize_audio_node_options(options, default_options));

    node->queue_render_node_creation(make<Rendering::BiquadFilterRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size(), node->m_frequency->render_param(), node->m_detune->render_param(), node->m_q->render_param(), node->m_gain->render_param()));
    if (options.type != Bindings::BiquadFilterType::Lowpass)
        context->queue_control_message(NodeMessage { SetBiquadFilterType { node->node_id(), options.type } });

    return node;
}

void BiquadFilterNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BiquadFilterNode);
    Base::initialize(realm);
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
