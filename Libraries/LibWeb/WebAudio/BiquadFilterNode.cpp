/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/AudioParamPrototype.h>
#include <LibWeb/Bindings/BiquadFilterNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/RenderNodes/BiquadFilterRenderNode.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(BiquadFilterNode);

BiquadFilterNode::BiquadFilterNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, BiquadFilterOptions const& options)
    : AudioNode(realm, context)
    , m_type(options.type)
    , m_frequency(AudioParam::create(realm, context, options.frequency, 0, context->nyquist_frequency(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, context, options.detune, -1200 * AK::log2(NumericLimits<float>::max()), 1200 * AK::log2(NumericLimits<float>::max()), Bindings::AutomationRate::ARate))
    , m_q(AudioParam::create(realm, context, options.q, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_gain(AudioParam::create(realm, context, options.gain, NumericLimits<float>::lowest(), 40 * AK::log10(NumericLimits<float>::max()), Bindings::AutomationRate::ARate))
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
WebIDL::ExceptionOr<void> BiquadFilterNode::get_frequency_response(GC::Root<WebIDL::BufferSource> const& frequency_hz, GC::Root<WebIDL::BufferSource> const& mag_response, GC::Root<WebIDL::BufferSource> const& phase_response)
{
    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-getfrequencyresponse

    if (!is<JS::Float32Array>(*frequency_hz->raw_object())
        || !is<JS::Float32Array>(*mag_response->raw_object())
        || !is<JS::Float32Array>(*phase_response->raw_object())) {
        return WebIDL::InvalidAccessError::create(realm(), "Arguments must be Float32Array"_utf16);
    }

    auto const& frequency_data = static_cast<JS::Float32Array const&>(*frequency_hz->raw_object()).data();
    auto mag_data = static_cast<JS::Float32Array&>(*mag_response->raw_object()).data();
    auto phase_data = static_cast<JS::Float32Array&>(*phase_response->raw_object()).data();

    if (mag_data.size() != frequency_data.size() || phase_data.size() != frequency_data.size())
        return WebIDL::InvalidAccessError::create(realm(), "All arrays must have the same length"_utf16);

    size_t const length = frequency_data.size();
    if (length == 0)
        return {};

    double const sample_rate = context()->sample_rate();
    double const nyquist = sample_rate * 0.5;

    Render::BiquadFilterType const render_type = static_cast<Render::BiquadFilterType>(to_underlying(m_type));
    float const computed_frequency = Render::compute_biquad_computed_frequency(sample_rate, m_frequency->value(), m_detune->value());
    Render::BiquadCoefficients const c = Render::compute_biquad_normalized_coefficients(render_type, sample_rate, computed_frequency, m_q->value(), m_gain->value());

    for (size_t i = 0; i < length; ++i) {
        float const f = frequency_data[i];

        if (!__builtin_isfinite(f) || __builtin_isnan(f) || f < 0.0f || static_cast<double>(f) > nyquist) {
            mag_data[i] = AK::NaN<float>;
            phase_data[i] = AK::NaN<float>;
            continue;
        }

        double const omega = 2.0 * AK::Pi<double> * (static_cast<double>(f) / sample_rate);
        double const cos_omega = AK::cos(omega);
        double const sin_omega = AK::sin(omega);
        double const cos_2omega = AK::cos(2.0 * omega);
        double const sin_2omega = AK::sin(2.0 * omega);

        double const num_re = static_cast<double>(c.b0) + (static_cast<double>(c.b1) * cos_omega) + (static_cast<double>(c.b2) * cos_2omega);
        double const num_im = -((static_cast<double>(c.b1) * sin_omega) + (static_cast<double>(c.b2) * sin_2omega));

        double const den_re = 1.0 + (static_cast<double>(c.a1) * cos_omega) + (static_cast<double>(c.a2) * cos_2omega);
        double const den_im = -((static_cast<double>(c.a1) * sin_omega) + (static_cast<double>(c.a2) * sin_2omega));

        double const den_mag2 = (den_re * den_re) + (den_im * den_im);
        if (den_mag2 == 0.0 || !__builtin_isfinite(den_mag2) || __builtin_isnan(den_mag2)) {
            mag_data[i] = AK::NaN<float>;
            phase_data[i] = AK::NaN<float>;
            continue;
        }

        double const h_re = (num_re * den_re + num_im * den_im) / den_mag2;
        double const h_im = (num_im * den_re - num_re * den_im) / den_mag2;

        double const mag = AK::sqrt((h_re * h_re) + (h_im * h_im));
        double const phase = AK::atan2(h_im, h_re);

        mag_data[i] = static_cast<float>(mag);
        phase_data[i] = static_cast<float>(phase);
    }

    return {};
}

WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> BiquadFilterNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, BiquadFilterOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-biquadfilternode
WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> BiquadFilterNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, BiquadFilterOptions const& options)
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
