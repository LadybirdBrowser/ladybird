/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibMedia/Audio/SignalProcessing.h>
#include <LibWeb/Bindings/AudioParamPrototype.h>
#include <LibWeb/Bindings/BiquadFilterNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(BiquadFilterNode);

BiquadFilterNode::BiquadFilterNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, BiquadFilterOptions const& options)
    : AudioNode(realm, context)
    , m_frequency(AudioParam::create(realm, options.frequency, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, options.detune, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_q(AudioParam::create(realm, options.q, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_gain(AudioParam::create(realm, options.gain, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
{
}

BiquadFilterNode::~BiquadFilterNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-type
WebIDL::ExceptionOr<void> BiquadFilterNode::set_type(Bindings::BiquadFilterType type)
{
    m_type = type;
    return {};
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

    auto F_s = context()->sample_rate();
    // https://webaudio.github.io/web-audio-api/#computedfrequency
    auto f0 = frequency()->value() * pow(2.0, detune()->value() / 1200.0);
    auto G = gain()->value();
    auto Q = q()->value();

    auto A = pow(10, (f64)G / 40.0);
    auto omega_0 = 2 * AK::Pi<f64> * f0 / F_s;
    auto alpha_Q = sin(omega_0) / (2.0 * Q);
    auto alpha_Q_dB = 0.5 * sin(omega_0) / (2.0 * pow(10, Q / 20.0));
    auto S = 1.0;
    auto alpha_S = 0.5 * sin(omega_0) * sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2);

    auto get_coefficients = [&]() {
        switch (type()) {
        case Bindings::BiquadFilterType::Lowpass:
            return Audio::biquad_filter_lowpass_coefficients(omega_0, alpha_Q_dB);
        case Bindings::BiquadFilterType::Highpass:
            return Audio::biquad_filter_highpass_coefficients(omega_0, alpha_Q_dB);
        case Bindings::BiquadFilterType::Bandpass:
            return Audio::biquad_filter_bandpass_coefficients(omega_0, alpha_Q_dB);
        case Bindings::BiquadFilterType::Notch:
            return Audio::biquad_filter_notch_coefficients(omega_0, alpha_Q_dB);
        case Bindings::BiquadFilterType::Allpass:
            return Audio::biquad_filter_allpass_coefficients(omega_0, alpha_Q, A);
        case Bindings::BiquadFilterType::Peaking:
            return Audio::biquad_filter_peaking_coefficients(omega_0, alpha_Q, A);
        case Bindings::BiquadFilterType::Lowshelf:
            return Audio::biquad_filter_lowshelf_coefficients(omega_0, alpha_S, A);
        case Bindings::BiquadFilterType::Highshelf:
            return Audio::biquad_filter_highshelf_coefficients(omega_0, alpha_S, A);
        }

        return Audio::biquad_filter_lowpass_coefficients(omega_0, alpha_Q_dB);
    };

    Array<f64, 6> coefficients = get_coefficients();
    (void)coefficients;

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
