/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::TypedArrayBase;
use libjs_rust::api::{Gc, Trace};
use libweb_rust::web_audio::rendering::render_nodes::create_iir_filter_render_node;

use super::audio_array::{copy_float32_array, float32_array_length, overwrite_float32_array};
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#IIRFilterNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IIRFilterNode {
    node: AudioNode,

    // The coefficients, divided by the first feedback coefficient.
    #[gc(untraced)]
    feedforward: Vec<f64>,
    #[gc(untraced)]
    feedback: Vec<f64>,
}

impl Inherits<AudioNode> for IIRFilterNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for IIRFilterNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for IIRFilterNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct IIRFilterOptions {
    pub parent: AudioNodeOptions,
    pub feedforward: Vec<f64>,
    pub feedback: Vec<f64>,
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createiirfilter
const MAX_COEFFICIENT_COUNT: usize = 20;

impl IIRFilterNode {
    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &IIRFilterOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createiirfilter
        // feedforward: An array of the feedforward (numerator) coefficients for the transfer function of the IIR filter.
        // The maximum length of this array is 20. If all of the values are zero, an InvalidStateError MUST be thrown. A
        // NotSupportedError MUST be thrown if the array length is 0 or greater than 20.
        // feedback: An array of the feedback (denominator) coefficients for the transfer function of the IIR filter. The
        // maximum length of this array is 20. If the first element of the array is 0, an InvalidStateError MUST be thrown.
        // A NotSupportedError MUST be thrown if the array length is 0 or greater than 20.
        let feedforward = &options.feedforward;
        let feedback = &options.feedback;
        if feedforward.is_empty() || feedforward.len() > MAX_COEFFICIENT_COUNT {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"The number of feedforward coefficients must be between 1 and 20"),
            ));
        }
        if feedback.is_empty() || feedback.len() > MAX_COEFFICIENT_COUNT {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"The number of feedback coefficients must be between 1 and 20"),
            ));
        }
        if feedforward.iter().all(|coefficient| *coefficient == 0.) {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"At least one feedforward coefficient must not be zero"),
            ));
        }
        if feedback[0] == 0. {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"The first feedback coefficient must not be zero"),
            ));
        }

        // https://webaudio.github.io/web-audio-api/#IIRFilterNode-filter-definition
        // NB: Dividing every coefficient by a_0 gives the same transfer function with a_0 = 1, which the filter uses.
        let a0 = feedback[0];
        let mut normalized_feedforward = Vec::with_capacity(feedforward.len());
        for coefficient in feedforward {
            normalized_feedforward.push(coefficient / a0);
        }
        let mut normalized_feedback = Vec::with_capacity(feedback.len());
        for coefficient in feedback {
            normalized_feedback.push(coefficient / a0);
        }

        // When the constructor is called with a BaseAudioContext c and an option object option, the user agent MUST
        // initialize the AudioNode this, with context and options as arguments.
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            feedforward: normalized_feedforward.clone(),
            feedback: normalized_feedback.clone(),
        });
        node.node.set_this(AnyAudioNode::IIRFilterNode(node));
        node.node.event_target().set_owner(node);

        // https://webaudio.github.io/web-audio-api/#IIRFilterNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to yes
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node.queue_render_node_creation(create_iir_filter_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            &normalized_feedforward,
            &normalized_feedback,
        ));
        Ok(node)
    }

    // https://webaudio.github.io/web-audio-api/#dom-iirfilternode-iirfilternode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: IIRFilterOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-iirfilternode-getfrequencyresponse
    pub(crate) fn get_frequency_response(
        &self,
        cx: &mut CallContext,
        frequency_hz: Gc<TypedArrayBase>,
        mag_response: Gc<TypedArrayBase>,
        phase_response: Gc<TypedArrayBase>,
    ) -> WebIdlResult<()> {
        // Given the current filter parameter settings, synchronously calculates the frequency response for the specified
        // frequencies. The three parameters MUST be Float32Arrays of the same length, or an InvalidAccessError MUST be
        // thrown.
        let length = float32_array_length(&frequency_hz);
        if float32_array_length(&mag_response) != length || float32_array_length(&phase_response) != length {
            return Err(Exception::Thrown(
                Exception::Dom(
                    "InvalidAccessError",
                    Utf16String::from_ascii(b"Arrays must have the same length"),
                )
                .throw(&CallContext::new(cx.vm(), self.node.context_value().realm())),
            ));
        }
        let frequencies = copy_float32_array(&frequency_hz);
        let mut magnitudes = vec![0.; length];
        let mut phases = vec![0.; length];
        let nyquist_frequency = self.node.context_value().nyquist_frequency();

        // https://webaudio.github.io/web-audio-api/#IIRFilterNode-filter-definition
        // NB: H(z) = (sum_{m=0}^{M} b_m z^{-m}) / (sum_{n=0}^{N} a_n z^{-n}), evaluated at z = e^{j omega}. A polynomial in
        //     z^-1 is evaluated with Horner's method, from the coefficient of its highest power down.
        #[derive(Clone, Copy)]
        struct ComplexNumber {
            real: f64,
            imaginary: f64,
        }
        let evaluate = |coefficients: &[f64], z_inverse: ComplexNumber| {
            let mut result = ComplexNumber {
                real: 0.,
                imaginary: 0.,
            };
            for index in (0..coefficients.len()).rev() {
                result = ComplexNumber {
                    real: result.real * z_inverse.real - result.imaginary * z_inverse.imaginary + coefficients[index],
                    imaginary: result.real * z_inverse.imaginary + result.imaginary * z_inverse.real,
                };
            }
            result
        };
        for index in 0..length {
            let frequency = f64::from(frequencies[index]);

            // If a value in the frequencyHz parameter is not within [0, sampleRate/2], where sampleRate is the value of
            // the sampleRate property of the AudioContext, the corresponding value at the same index of the magResponse
            // and phaseResponse arrays MUST be NaN.
            if frequency.is_nan() || frequency < 0. || frequency > f64::from(nyquist_frequency) {
                magnitudes[index] = f32::NAN;
                phases[index] = f32::NAN;
                continue;
            }
            let omega = std::f64::consts::PI * frequency / f64::from(nyquist_frequency);
            let z_inverse = ComplexNumber {
                real: ak::math::cos(omega),
                imaginary: -ak::math::sin(omega),
            };
            let numerator = evaluate(&self.feedforward, z_inverse);
            let denominator = evaluate(&self.feedback, z_inverse);
            let denominator_magnitude_squared =
                denominator.real * denominator.real + denominator.imaginary * denominator.imaginary;
            let response = ComplexNumber {
                real: (numerator.real * denominator.real + numerator.imaginary * denominator.imaginary)
                    / denominator_magnitude_squared,
                imaginary: (numerator.imaginary * denominator.real - numerator.real * denominator.imaginary)
                    / denominator_magnitude_squared,
            };
            magnitudes[index] = ak::math::hypot(response.real, response.imaginary) as f32;
            phases[index] = ak::math::atan2(response.imaginary, response.real) as f32;
        }
        overwrite_float32_array(&mag_response, &magnitudes);
        overwrite_float32_array(&phase_response, &phases);
        Ok(())
    }
}
