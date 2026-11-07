/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::TypedArrayBase;
use libjs_rust::api::{Gc, Trace};
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::rendering::biquad_coefficients::{biquad_frequency_response, compute_biquad_coefficients};
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, SetBiquadFilterType};
use libweb_rust::web_audio::rendering::render_nodes::create_biquad_filter_render_node;

use super::audio_array::{copy_float32_array, float32_array_length, overwrite_float32_array};
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) use libweb_rust::web_audio::rendering::biquad_coefficients::BiquadFilterType;

// https://webaudio.github.io/web-audio-api/#BiquadFilterNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct BiquadFilterNode {
    node: AudioNode,
    #[gc(untraced)]
    r#type: Cell<BiquadFilterType>,

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-frequency
    frequency: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-detune
    detune: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-q
    q: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-gain
    gain: Ref<AudioParam>,
}

impl Inherits<AudioNode> for BiquadFilterNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for BiquadFilterNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for BiquadFilterNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct BiquadFilterOptions {
    pub parent: AudioNodeOptions,
    pub r#type: BiquadFilterType,
    pub frequency: f32,
    pub detune: f32,
    pub q: f32,
    pub gain: f32,
}

impl BiquadFilterNode {
    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &BiquadFilterOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
        // MUST initialize the AudioNode this, with context and options as arguments.
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            r#type: Cell::new(options.r#type),
            frequency: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.frequency,
                0.,
                context.nyquist_frequency(),
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            detune: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.detune,
                -1200. * ak::math::log2(f32::MAX),
                1200. * ak::math::log2(f32::MAX),
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            q: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.q,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            gain: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.gain,
                f32::MIN,
                40. * ak::math::log10(f32::MAX),
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
        });
        node.node.set_this(AnyAudioNode::BiquadFilterNode(node));
        node.node.event_target().set_owner(node);
        node.frequency.set_owner(AnyAudioNode::BiquadFilterNode(node));
        node.detune.set_owner(AnyAudioNode::BiquadFilterNode(node));
        node.q.set_owner(AnyAudioNode::BiquadFilterNode(node));
        node.gain.set_owner(AnyAudioNode::BiquadFilterNode(node));

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#BiquadFilterNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to yes
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node.queue_render_node_creation(create_biquad_filter_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            Arc::clone(node.frequency.render_param()),
            Arc::clone(node.detune.render_param()),
            Arc::clone(node.q.render_param()),
            Arc::clone(node.gain.render_param()),
        ));
        if options.r#type != BiquadFilterType::Lowpass {
            context.queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetBiquadFilterType(
                SetBiquadFilterType {
                    node_id: node.node.node_id(),
                    r#type: options.r#type,
                },
            )));
        }
        Ok(node)
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-biquadfilternode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: BiquadFilterOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-type
    pub(crate) fn set_type(&self, _: &mut CallContext, r#type: BiquadFilterType) -> WebIdlResult<()> {
        self.r#type.set(r#type);
        self.node
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetBiquadFilterType(
                SetBiquadFilterType {
                    node_id: self.node.node_id(),
                    r#type,
                },
            )));
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<BiquadFilterType> {
        Ok(self.r#type.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-frequency
    pub(crate) fn frequency(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.frequency)
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-detune
    pub(crate) fn detune(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.detune)
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-q
    pub(crate) fn q(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.q)
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-gain
    pub(crate) fn gain(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.gain)
    }

    // https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-getfrequencyresponse
    pub(crate) fn get_frequency_response(
        &self,
        cx: &mut CallContext,
        frequency_hz: Gc<TypedArrayBase>,
        mag_response: Gc<TypedArrayBase>,
        phase_response: Gc<TypedArrayBase>,
    ) -> WebIdlResult<()> {
        let length = float32_array_length(&frequency_hz);

        // If these arrays do not have the same length, an InvalidAccessError MUST be thrown.
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

        // The filter response is evaluated with the current parameter values at the context's current time.
        let current_time = self.node.context_value().current_time_value();
        let nyquist_frequency = self.node.context_value().nyquist_frequency();
        let frequency = self.frequency.intrinsic_value_at_time(current_time);
        let detune = self.detune.intrinsic_value_at_time(current_time);
        let computed_frequency = f64::from(frequency) * ak::math::exp2(f64::from(detune) / 1200.);
        let normalized_frequency = computed_frequency / f64::from(nyquist_frequency);
        let normalized_frequency = normalized_frequency.clamp(0., 1.);
        let coefficients = compute_biquad_coefficients(
            self.r#type.get(),
            normalized_frequency,
            f64::from(self.q.intrinsic_value_at_time(current_time)),
            f64::from(self.gain.intrinsic_value_at_time(current_time)),
        );
        for index in 0..length {
            let response_frequency = frequencies[index];

            // Frequencies outside the nominal range produce NaN in both output arrays.
            if response_frequency.is_nan() || response_frequency < 0. || response_frequency > nyquist_frequency {
                magnitudes[index] = f32::NAN;
                phases[index] = f32::NAN;
                continue;
            }
            (magnitudes[index], phases[index]) =
                biquad_frequency_response(&coefficients, f64::from(response_frequency / nyquist_frequency));
        }
        overwrite_float32_array(&mag_response, &magnitudes);
        overwrite_float32_array(&phase_response, &phases);
        Ok(())
    }
}
