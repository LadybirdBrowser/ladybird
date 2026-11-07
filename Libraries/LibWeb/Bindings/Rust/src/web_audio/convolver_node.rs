/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::collapsible_if, reason = "preserve the C++ branch structure")]

use std::cell::Cell;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::rendering::convolver_kernel::{AudioBufferContents, ConvolverDelayLine, ConvolverKernel};
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, SetConvolverKernel};
use libweb_rust::web_audio::rendering::render_nodes::create_convolver_render_node;

use super::audio_buffer::AudioBuffer;
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#ConvolverNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ConvolverNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-convolvernode-buffer
    buffer: Cell<Option<Ref<AudioBuffer>>>,

    // https://webaudio.github.io/web-audio-api/#dom-convolvernode-normalize
    normalize: Cell<bool>,
}

impl Inherits<AudioNode> for ConvolverNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for ConvolverNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for ConvolverNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct ConvolverOptions {
    pub parent: AudioNodeOptions,
    pub buffer: Option<Option<Ref<AudioBuffer>>>,
    pub disable_normalization: bool,
}

impl ConvolverNode {
    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &ConvolverOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // NB: The node and its render node are created up front, so that step 2 has something to hand the impulse
        //     response to.
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            buffer: Cell::new(None),
            normalize: Cell::new(!options.disable_normalization),
        });
        node.node.set_this(AnyAudioNode::ConvolverNode(node));
        node.node.event_target().set_owner(node);
        node.node.queue_render_node_creation(create_convolver_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
        ));

        // 1. Set the attributes normalize to the inverse of the value of disableNormalization.
        // NB: Done by the constructor above, so that step 2 already sees the final value.
        // 2. If buffer exists, set the buffer attribute to its value.
        if let Some(buffer) = options.buffer {
            node.set_buffer(cx, buffer)?;
        }

        // 3. Let o be new AudioNodeOptions dictionary.
        // 4. If channelCount exists in options, set channelCount on o with the same value.
        // 5. If channelCountMode exists in options, set channelCountMode on o with the same value.
        // 6. If channelInterpretation exists in options, set channelInterpretation on o with the same value.
        // 7. Initialize the AudioNode this, with c and o as argument.
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::ClampedMax,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to yes
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        Ok(node)
    }

    // https://webaudio.github.io/web-audio-api/#dom-convolvernode-convolvernode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: ConvolverOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-convolvernode-buffer
    pub(crate) fn set_buffer(&self, cx: &mut CallContext, buffer: Option<Ref<AudioBuffer>>) -> WebIdlResult<()> {
        // 1. If the buffer number of channels is not 1, 2, 4, or if the sample-rate of the buffer is not the same as the
        //    sample-rate of its associated BaseAudioContext, a NotSupportedError MUST be thrown.
        // AD-HOC: The steps do not cover a null buffer, which clears the impulse response and leaves the node silent.
        if let Some(buffer) = buffer {
            let number_of_channels = buffer.number_of_channels(cx)?;
            if number_of_channels != 1 && number_of_channels != 2 && number_of_channels != 4 {
                return Err(Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_ascii(b"ConvolverNode buffer must have 1, 2 or 4 channels"),
                ));
            }
            if buffer.sample_rate(cx)? != self.node.context_value().sample_rate_value() {
                return Err(Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_ascii(b"ConvolverNode buffer must have the same sample rate as its context"),
                ));
            }
        }
        self.buffer.set(buffer);

        // 2. Acquire the content of the AudioBuffer.
        // NB: The acquired content is cut into partitions and transformed here, and the delay line that the rendering
        //     thread pairs those partitions with is sized and allocated alongside it, so that the rendering thread
        //     receives both ready to use and never has to build either during a render quantum.
        let mut kernel = None;
        let mut delay_line = None;
        if let Some(buffer) = buffer {
            if let Some(contents) = AudioBuffer::acquire_contents(buffer, cx.vm()) {
                let channels: Vec<_> = contents.channels.iter().map(|channel| ak::Span::new(channel)).collect();
                kernel = ConvolverKernel::create(
                    &AudioBufferContents {
                        channels: &channels,
                        sample_rate: contents.sample_rate,
                    },
                    BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                    self.normalize.get(),
                );
            }
            if let Some(kernel) = &kernel {
                delay_line = Some(Arc::new(ConvolverDelayLine::create(kernel)));
            }
        }
        self.node
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetConvolverKernel(
                SetConvolverKernel {
                    node_id: self.node.node_id(),
                    kernel,
                    delay_line,
                },
            )));
        Ok(())
    }

    pub(crate) fn buffer(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<AudioBuffer>>> {
        Ok(self.buffer.get())
    }

    pub(crate) fn normalize(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.normalize.get())
    }

    pub(crate) fn set_normalize(&self, _: &mut CallContext, value: bool) -> WebIdlResult<()> {
        self.normalize.set(value);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, channel_count: u32) -> WebIdlResult<()> {
        // The channel count cannot be greater than two, and a NotSupportedError exception MUST be thrown for any attempt
        // to change it to a value greater than two.
        if channel_count > 2 {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"ConvolverNode does not support channel count greater than 2"),
            ));
        }

        self.node.set_channel_count_impl(channel_count)
    }

    // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
    pub(crate) fn set_channel_count_mode_override(
        &self,
        _: &mut CallContext,
        mode: ChannelCountMode,
    ) -> WebIdlResult<()> {
        // The channel count mode cannot be set to "max", and a NotSupportedError exception MUST be thrown for any attempt
        // to set it to "max".
        if mode == ChannelCountMode::Max {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"ConvolverNode does not support 'max' as channelCountMode."),
            ));
        }

        self.node.set_channel_count_mode_impl(mode)
    }
}
