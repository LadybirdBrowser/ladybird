/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, SetOscillatorWaveform};
use libweb_rust::web_audio::rendering::render_nodes::create_oscillator_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::audio_scheduled_source_node::AudioScheduledSourceNode;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use super::periodic_wave::PeriodicWave;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) use libweb_rust::web_audio::rendering::node_message::OscillatorType;

// https://webaudio.github.io/web-audio-api/#OscillatorNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct OscillatorNode {
    node: AudioScheduledSourceNode,

    // https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
    #[gc(untraced)]
    r#type: Cell<OscillatorType>,

    // https://webaudio.github.io/web-audio-api/#dom-oscillatornode-frequency
    frequency: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-oscillatornode-detune
    detune: Ref<AudioParam>,
    periodic_wave: Cell<Option<Ref<PeriodicWave>>>,
}

impl Inherits<AudioScheduledSourceNode> for OscillatorNode {
    fn parent(&self) -> &AudioScheduledSourceNode {
        &self.node
    }
}

impl Wrappable for OscillatorNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.parent().finalize_node();
    }
}

impl GetTheParent for OscillatorNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct OscillatorOptions {
    pub parent: AudioNodeOptions,
    pub r#type: OscillatorType,
    pub frequency: f32,
    pub detune: f32,
    pub periodic_wave: Option<Ref<PeriodicWave>>,
}

impl OscillatorNode {
    fn queue_waveform_update(&self) {
        let periodic_wave = self.periodic_wave.get().map(|wave| wave.render_data());
        self.node
            .parent()
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetOscillatorWaveform(
                SetOscillatorWaveform {
                    node_id: self.node.parent().node_id(),
                    r#type: self.r#type.get(),
                    periodic_wave,
                },
            )));
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &OscillatorOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioScheduledSourceNode::new(defer_gc, context),
            r#type: Cell::new(options.r#type),
            frequency: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.frequency,
                -context.nyquist_frequency(),
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
            periodic_wave: Cell::new(None),
        });
        node.node.parent().set_this(AnyAudioNode::OscillatorNode(node));
        node.node.parent().event_target().set_owner(node);
        node.frequency.set_owner(AnyAudioNode::OscillatorNode(node));
        node.detune.set_owner(AnyAudioNode::OscillatorNode(node));
        if options.r#type == OscillatorType::Custom {
            node.set_periodic_wave_value(options.periodic_wave);
        }

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#OscillatorNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to no
        node.node
            .parent()
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node
            .parent()
            .queue_render_node_creation(create_oscillator_render_node(
                node.node.parent().node_id(),
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                Arc::clone(node.frequency.render_param()),
                Arc::clone(node.detune.render_param()),
            ));
        node.queue_waveform_update();
        Ok(node)
    }

    pub(crate) fn validate_options(options: &OscillatorOptions) -> WebIdlResult<()> {
        if options.r#type == OscillatorType::Custom && options.periodic_wave.is_none() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Oscillator node type 'custom' requires PeriodicWave to be provided"),
            ));
        }
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-oscillatornode-oscillatornode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: OscillatorOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::validate_options(&options)?;
        Self::create(cx, context, &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<OscillatorType> {
        Ok(self.r#type.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
    pub(crate) fn set_type(&self, _: &mut CallContext, r#type: OscillatorType) -> WebIdlResult<()> {
        if r#type == OscillatorType::Custom && self.r#type.get() != OscillatorType::Custom {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Oscillator node type cannot be changed to 'custom'"),
            ));
        }

        // FIXME: An appropriate PeriodicWave should be set here based on the given type.
        self.set_periodic_wave_value(None);
        self.r#type.set(r#type);
        self.queue_waveform_update();
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-oscillatornode-setperiodicwave
    pub(crate) fn set_periodic_wave(&self, _: &mut CallContext, wave: Ref<PeriodicWave>) -> WebIdlResult<()> {
        self.set_periodic_wave_value(Some(wave));
        Ok(())
    }

    fn set_periodic_wave_value(&self, wave: Option<Ref<PeriodicWave>>) {
        self.periodic_wave.set(wave);
        self.r#type.set(OscillatorType::Custom);
        self.queue_waveform_update();
    }

    pub(crate) fn frequency(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.frequency)
    }

    pub(crate) fn detune(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.detune)
    }
}
