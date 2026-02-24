/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/IIRFilterNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/IIRFilterNode.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(IIRFilterNode);

struct NormalizedIIRCoefficients {
    Vector<double> feedforward;
    Vector<double> feedback;
};

static WebIDL::ExceptionOr<NormalizedIIRCoefficients> normalize_iir_coefficients(JS::Realm& realm, Vector<double> const& feedforward, Vector<double> const& feedback)
{
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createiirfilter
    // A NotSupportedError must be thrown if the array length is 0 or greater than 20.
    if (feedforward.is_empty() || feedforward.size() > 20)
        return WebIDL::NotSupportedError::create(realm, "Feedforward array length must be between 1 and 20"_utf16);
    if (feedback.is_empty() || feedback.size() > 20)
        return WebIDL::NotSupportedError::create(realm, "Feedback array length must be between 1 and 20"_utf16);

    // An InvalidStateError must be thrown if all of the feedforward values are zero.
    bool any_feedforward_nonzero = false;
    for (auto value : feedforward) {
        if (value != 0.0) {
            any_feedforward_nonzero = true;
            break;
        }
    }
    if (!any_feedforward_nonzero)
        return WebIDL::InvalidStateError::create(realm, "Feedforward coefficients must not all be zero"_utf16);

    // An InvalidStateError must be thrown if the first element of feedback is 0.
    if (feedback[0] == 0.0)
        return WebIDL::InvalidStateError::create(realm, "Feedback[0] must not be zero"_utf16);

    double const a0 = feedback[0];
    double const inv_a0 = 1.0 / a0;

    NormalizedIIRCoefficients normalized;
    normalized.feedforward.ensure_capacity(feedforward.size());
    for (auto value : feedforward)
        normalized.feedforward.append(value * inv_a0);

    normalized.feedback.ensure_capacity(feedback.size());
    for (auto value : feedback)
        normalized.feedback.append(value * inv_a0);

    // Normalize so feedback[0] is 1.0.
    normalized.feedback[0] = 1.0;

    return normalized;
}

IIRFilterNode::IIRFilterNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, IIRFilterOptions const& options)
    : AudioNode(realm, context)
    , m_feedforward(options.feedforward)
    , m_feedback(options.feedback)
{
}

IIRFilterNode::~IIRFilterNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-iirfilternode-getfrequencyresponse
WebIDL::ExceptionOr<void> IIRFilterNode::get_frequency_response(GC::Root<WebIDL::BufferSource> const& frequency_hz, GC::Root<WebIDL::BufferSource> const& mag_response, GC::Root<WebIDL::BufferSource> const& phase_response)
{
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

    for (size_t i = 0; i < length; ++i) {
        float const f = frequency_data[i];

        if (!__builtin_isfinite(f) || __builtin_isnan(f) || f < 0.0f || static_cast<double>(f) > nyquist) {
            mag_data[i] = AK::NaN<float>;
            phase_data[i] = AK::NaN<float>;
            continue;
        }

        double const omega = 2.0 * AK::Pi<double> * (static_cast<double>(f) / sample_rate);

        double num_re = 0.0;
        double num_im = 0.0;
        for (size_t k = 0; k < m_feedforward.size(); ++k) {
            double const phase = omega * static_cast<double>(k);
            double const cos_phase = AK::cos(phase);
            double const sin_phase = AK::sin(phase);
            double const b = m_feedforward[k];
            num_re += b * cos_phase;
            num_im -= b * sin_phase;
        }

        double den_re = 0.0;
        double den_im = 0.0;
        for (size_t k = 0; k < m_feedback.size(); ++k) {
            double const phase = omega * static_cast<double>(k);
            double const cos_phase = AK::cos(phase);
            double const sin_phase = AK::sin(phase);
            double const a = m_feedback[k];
            den_re += a * cos_phase;
            den_im -= a * sin_phase;
        }

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

WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> IIRFilterNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, IIRFilterOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-iirfilternode-iirfilternode
WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> IIRFilterNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, IIRFilterOptions const& options)
{
    auto normalized = TRY(normalize_iir_coefficients(realm, options.feedforward, options.feedback));

    IIRFilterOptions normalized_options = options;
    normalized_options.feedforward = move(normalized.feedforward);
    normalized_options.feedback = move(normalized.feedback);

    auto node = realm.create<IIRFilterNode>(realm, context, normalized_options);

    // Default options for channel count and interpretation.
    // https://webaudio.github.io/web-audio-api/#IIRFilterNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to yes

    TRY(node->initialize_audio_node_options(normalized_options, default_options));

    return node;
}

void IIRFilterNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IIRFilterNode);
    Base::initialize(realm);
}

}
