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
use libjs_rust::api::Trace;
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::rendering::audio_data::AudioBufferContents;
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, SetBufferSourceParameters, StartBufferSource};
use libweb_rust::web_audio::rendering::render_nodes::create_audio_buffer_source_render_node;

use super::audio_buffer::AudioBuffer;
use super::audio_node::{
    AnyAudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::audio_scheduled_source_node::AudioScheduledSourceNode;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioBufferSourceNode {
    node: AudioScheduledSourceNode,
    buffer: Cell<Option<Ref<AudioBuffer>>>,
    playback_rate: Ref<AudioParam>,
    detune: Ref<AudioParam>,
    r#loop: Cell<bool>,
    loop_start: Cell<f64>,
    loop_end: Cell<f64>,

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer-set-slot
    buffer_set: Cell<bool>,
}

impl Inherits<AudioScheduledSourceNode> for AudioBufferSourceNode {
    fn parent(&self) -> &AudioScheduledSourceNode {
        &self.node
    }
}

impl Wrappable for AudioBufferSourceNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.parent().finalize_node();
    }
}

impl GetTheParent for AudioBufferSourceNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct AudioBufferSourceOptions {
    pub buffer: Option<Option<Ref<AudioBuffer>>>,
    pub playback_rate: f32,
    pub detune: f32,
    pub r#loop: bool,
    pub loop_start: f64,
    pub loop_end: f64,
}

impl AudioBufferSourceNode {
    fn queue_parameters_update(&self, buffer: Option<Arc<AudioBufferContents>>, update_buffer: bool) {
        self.node
            .parent()
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetBufferSourceParameters(
                SetBufferSourceParameters {
                    node_id: self.node.parent().node_id(),
                    r#loop: self.r#loop.get(),
                    loop_start: self.loop_start.get(),
                    loop_end: self.loop_end.get(),
                    buffer,
                    update_buffer,
                },
            )));
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
    pub(crate) fn set_buffer(&self, cx: &mut CallContext, buffer: Option<Ref<AudioBuffer>>) -> WebIdlResult<()> {
        // 1. Let new buffer be the AudioBuffer or null value to be assigned to buffer.
        let new_buffer = buffer;

        // 2. If new buffer is not null and [[buffer set]] is true, throw an InvalidStateError and abort these steps.
        if new_buffer.is_some() && self.buffer_set.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Buffer has already been set"),
            ));
        }

        // 3. If new buffer is not null, set [[buffer set]] to true.
        if new_buffer.is_some() {
            self.buffer_set.set(true);
        }

        // 4. Assign new buffer to the buffer attribute.
        self.buffer.set(new_buffer);

        // 5. If start() has previously been called on this node, perform the operation acquire the content on buffer.
        if self.node.source_started() {
            self.queue_parameters_update(
                self.buffer
                    .get()
                    .and_then(|buffer| AudioBuffer::acquire_contents(buffer, cx.vm())),
                true,
            );
        }
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
    pub(crate) fn buffer(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<AudioBuffer>>> {
        Ok(self.buffer.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-playbackrate
    pub(crate) fn playback_rate(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.playback_rate)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-detune
    pub(crate) fn detune(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.detune)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loop
    pub(crate) fn r#loop(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.r#loop.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loop
    pub(crate) fn set_loop(&self, _: &mut CallContext, value: bool) -> WebIdlResult<()> {
        self.r#loop.set(value);
        self.queue_parameters_update(None, false);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
    pub(crate) fn loop_start(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.loop_start.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
    pub(crate) fn set_loop_start(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.loop_start.set(value);
        self.queue_parameters_update(None, false);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopend
    pub(crate) fn loop_end(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.loop_end.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopend
    pub(crate) fn set_loop_end(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.loop_end.set(value);
        self.queue_parameters_update(None, false);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-start
    pub(crate) fn start(
        &self,
        cx: &mut CallContext,
        when: f64,
        offset: Option<f64>,
        duration: Option<f64>,
    ) -> WebIdlResult<()> {
        // 1. If this AudioBufferSourceNode internal slot [[source started]] is true, an InvalidStateError exception MUST be thrown.
        if self.node.source_started() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"AudioBufferSourceNode has already been started"),
            ));
        }

        // 2. Check for any errors that must be thrown due to parameter constraints described below. If any exception is thrown during this step, abort those steps.
        // A RangeError exception MUST be thrown if when is negative.
        if when < 0. {
            return Err(Exception::range_error("when must not be negative"));
        }

        // A RangeError exception MUST be thrown if offset is negative
        if offset.is_some_and(|offset| offset < 0.) {
            return Err(Exception::range_error("offset must not be negative"));
        }

        // A RangeError exception MUST be thrown if duration is negative.
        if duration.is_some_and(|duration| duration < 0.) {
            return Err(Exception::range_error("duration must not be negative"));
        }

        // 3. Set the internal slot [[source started]] on this AudioBufferSourceNode to true.
        self.node.set_source_started(true);

        // 4. Queue a control message to start the AudioBufferSourceNode, including the parameter values in the message.
        // 5. Acquire the contents of the buffer if the buffer has been set.
        let mut contents = None;
        if let Some(buffer) = self.buffer.get() {
            contents = AudioBuffer::acquire_contents(buffer, cx.vm());
        }
        self.node
            .parent()
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::StartBufferSource(
                StartBufferSource {
                    node_id: self.node.parent().node_id(),
                    when,
                    offset: offset.unwrap_or(0.),
                    duration,
                    buffer: contents,
                },
            )));
        self.node
            .parent()
            .context_value()
            .add_playing_source(cx.vm(), self.node.parent().this());

        // FIXME: 6. Send a control message to the associated AudioContext to start running its rendering thread only when all the following conditions are met:
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-audiobuffersourcenode
    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &AudioBufferSourceOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
        // MUST initialize the AudioNode this, with context and options as arguments.

        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioScheduledSourceNode::new(defer_gc, context),
            buffer: Cell::new(None),
            playback_rate: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.playback_rate,
                f32::MIN,
                f32::MAX,
                AutomationRate::KRate,
                FixedAutomationRate::Yes,
            ),
            detune: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.detune,
                f32::MIN,
                f32::MAX,
                AutomationRate::KRate,
                FixedAutomationRate::Yes,
            ),
            r#loop: Cell::new(options.r#loop),
            loop_start: Cell::new(options.loop_start),
            loop_end: Cell::new(options.loop_end),
            buffer_set: Cell::new(false),
        });
        node.node.parent().set_this(AnyAudioNode::AudioBufferSourceNode(node));
        node.node.parent().event_target().set_owner(node);
        node.playback_rate.set_owner(AnyAudioNode::AudioBufferSourceNode(node));
        node.detune.set_owner(AnyAudioNode::AudioBufferSourceNode(node));

        node.set_buffer(cx, options.buffer.flatten())?;

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to no
        node.node
            .parent()
            .initialize_audio_node_options(cx, &AudioNodeOptions::default(), &default_options)?;
        node.node
            .parent()
            .queue_render_node_creation(create_audio_buffer_source_render_node(
                node.node.parent().node_id(),
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                Arc::clone(node.playback_rate.render_param()),
                Arc::clone(node.detune.render_param()),
            ));
        node.queue_parameters_update(None, false);
        Ok(node)
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: AudioBufferSourceOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }
}
