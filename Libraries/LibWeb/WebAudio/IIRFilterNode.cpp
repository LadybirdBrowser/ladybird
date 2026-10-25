/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebAudio/AudioArray.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/IIRFilterNode.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(IIRFilterNode);

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createiirfilter
static constexpr size_t max_coefficient_count = 20;

IIRFilterNode::IIRFilterNode(GC::Ref<BaseAudioContext> context, Vector<double> feedforward, Vector<double> feedback)
    : AudioNode(context)
    , m_feedforward(move(feedforward))
    , m_feedback(move(feedback))
{
}

IIRFilterNode::~IIRFilterNode() = default;

WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> IIRFilterNode::create(GC::Ref<BaseAudioContext> context, IIRFilterOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createiirfilter
    // feedforward: An array of the feedforward (numerator) coefficients for the transfer function of the IIR filter.
    // The maximum length of this array is 20. If all of the values are zero, an InvalidStateError MUST be thrown. A
    // NotSupportedError MUST be thrown if the array length is 0 or greater than 20.
    // feedback: An array of the feedback (denominator) coefficients for the transfer function of the IIR filter. The
    // maximum length of this array is 20. If the first element of the array is 0, an InvalidStateError MUST be thrown.
    // A NotSupportedError MUST be thrown if the array length is 0 or greater than 20.
    auto const& feedforward = options.feedforward;
    auto const& feedback = options.feedback;
    if (feedforward.is_empty() || feedforward.size() > max_coefficient_count)
        return WebIDL::NotSupportedError::create("The number of feedforward coefficients must be between 1 and 20"_utf16);
    if (feedback.is_empty() || feedback.size() > max_coefficient_count)
        return WebIDL::NotSupportedError::create("The number of feedback coefficients must be between 1 and 20"_utf16);
    if (all_of(feedforward, [](double coefficient) { return coefficient == 0; }))
        return WebIDL::InvalidStateError::create("At least one feedforward coefficient must not be zero"_utf16);
    if (feedback.first() == 0)
        return WebIDL::InvalidStateError::create("The first feedback coefficient must not be zero"_utf16);

    // https://webaudio.github.io/web-audio-api/#IIRFilterNode-filter-definition
    // NB: Dividing every coefficient by a_0 gives the same transfer function with a_0 = 1, which the filter uses.
    auto a0 = feedback.first();
    Vector<double> normalized_feedforward;
    normalized_feedforward.ensure_capacity(feedforward.size());
    for (auto coefficient : feedforward)
        normalized_feedforward.unchecked_append(coefficient / a0);
    Vector<double> normalized_feedback;
    normalized_feedback.ensure_capacity(feedback.size());
    for (auto coefficient : feedback)
        normalized_feedback.unchecked_append(coefficient / a0);

    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent MUST
    // initialize the AudioNode this, with context and options as arguments.
    auto node = GC::Heap::the().allocate<IIRFilterNode>(context, normalized_feedforward, normalized_feedback);

    // https://webaudio.github.io/web-audio-api/#IIRFilterNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to yes

    TRY(node->initialize_audio_node_options(options, default_options));

    node->queue_render_node_creation(make<Rendering::IIRFilterRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size(), move(normalized_feedforward), move(normalized_feedback)));

    return node;
}

// https://webaudio.github.io/web-audio-api/#dom-iirfilternode-iirfilternode
WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> IIRFilterNode::create_for_constructor(GC::Ref<BaseAudioContext> context, IIRFilterOptions const& options)
{
    return create(context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-iirfilternode-getfrequencyresponse
WebIDL::ExceptionOr<void> IIRFilterNode::get_frequency_response(GC::Ref<JS::Float32Array> frequency_hz, GC::Ref<JS::Float32Array> mag_response, GC::Ref<JS::Float32Array> phase_response)
{
    // Given the current filter parameter settings, synchronously calculates the frequency response for the specified
    // frequencies. The three parameters MUST be Float32Arrays of the same length, or an InvalidAccessError MUST be
    // thrown.
    auto length = float32_array_length(*frequency_hz);
    if (float32_array_length(*mag_response) != length || float32_array_length(*phase_response) != length)
        return WebIDL::InvalidAccessError::create(context()->relevant_settings_object().realm(), "Arrays must have the same length"_utf16);

    auto frequencies = copy_float32_array(*frequency_hz);
    Vector<float> magnitudes;
    magnitudes.resize(length);
    Vector<float> phases;
    phases.resize(length);

    auto nyquist_frequency = context()->nyquist_frequency();

    // https://webaudio.github.io/web-audio-api/#IIRFilterNode-filter-definition
    // NB: H(z) = (sum_{m=0}^{M} b_m z^{-m}) / (sum_{n=0}^{N} a_n z^{-n}), evaluated at z = e^{j omega}. A polynomial in
    //     z^-1 is evaluated with Horner's method, from the coefficient of its highest power down.
    struct ComplexNumber {
        double real { 0 };
        double imaginary { 0 };
    };
    auto evaluate = [](ReadonlySpan<double> coefficients, ComplexNumber z_inverse) {
        ComplexNumber result;
        for (size_t index = coefficients.size(); index-- > 0;) {
            result = {
                result.real * z_inverse.real - result.imaginary * z_inverse.imaginary + coefficients[index],
                result.real * z_inverse.imaginary + result.imaginary * z_inverse.real,
            };
        }
        return result;
    };

    for (size_t index = 0; index < length; ++index) {
        double frequency = frequencies[index];

        // If a value in the frequencyHz parameter is not within [0, sampleRate/2], where sampleRate is the value of
        // the sampleRate property of the AudioContext, the corresponding value at the same index of the magResponse
        // and phaseResponse arrays MUST be NaN.
        if (isnan(frequency) || frequency < 0 || frequency > nyquist_frequency) {
            magnitudes[index] = NAN;
            phases[index] = NAN;
            continue;
        }

        double omega = AK::Pi<double> * frequency / nyquist_frequency;
        ComplexNumber z_inverse { AK::cos(omega), -AK::sin(omega) };
        auto numerator = evaluate(m_feedforward, z_inverse);
        auto denominator = evaluate(m_feedback, z_inverse);
        auto denominator_magnitude_squared = denominator.real * denominator.real + denominator.imaginary * denominator.imaginary;
        ComplexNumber response {
            (numerator.real * denominator.real + numerator.imaginary * denominator.imaginary) / denominator_magnitude_squared,
            (numerator.imaginary * denominator.real - numerator.real * denominator.imaginary) / denominator_magnitude_squared,
        };
        magnitudes[index] = static_cast<float>(AK::hypot(response.real, response.imaginary));
        phases[index] = static_cast<float>(AK::atan2(response.imaginary, response.real));
    }

    overwrite_float32_array(*mag_response, magnitudes);
    overwrite_float32_array(*phase_response, phases);
    return {};
}

}
