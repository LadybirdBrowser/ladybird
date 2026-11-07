/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::rendering::render_nodes::create_channel_splitter_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#ChannelSplitterNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ChannelSplitterNode {
    node: AudioNode,
    number_of_outputs: u32,
}

impl Inherits<AudioNode> for ChannelSplitterNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for ChannelSplitterNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for ChannelSplitterNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct ChannelSplitterOptions {
    pub parent: AudioNodeOptions,
    pub number_of_outputs: u32,
}

impl ChannelSplitterNode {
    pub(crate) fn validate_options(options: &ChannelSplitterOptions) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelsplitter
        // An IndexSizeError exception MUST be thrown if numberOfOutputs is less than 1 or is greater than the number of supported channels.
        if options.number_of_outputs < 1 || options.number_of_outputs > BaseAudioContext::MAX_NUMBER_OF_CHANNELS {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Invalid number of outputs"),
            ));
        }
        Ok(())
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: ChannelSplitterOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::validate_options(&options)?;
        Self::create(cx, context, &options)
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &ChannelSplitterOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            number_of_outputs: options.number_of_outputs,
        });
        node.node.set_this(AnyAudioNode::ChannelSplitterNode(node));
        node.node.event_target().set_owner(node);

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#ChannelSplitterNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: node.number_of_outputs,
            channel_count_mode: ChannelCountMode::Explicit,
            channel_interpretation: ChannelInterpretation::Discrete,
        };

        // FIXME: Set tail-time to no
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node
            .queue_render_node_creation(create_channel_splitter_render_node(
                node.node.node_id(),
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                options.number_of_outputs as usize,
            ));
        Ok(node)
    }

    pub(crate) fn number_of_inputs_value(&self) -> u32 {
        1
    }

    pub(crate) fn number_of_outputs_value(&self) -> u32 {
        self.number_of_outputs
    }

    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, channel_count: u32) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
        // The channel count cannot be changed, and an InvalidStateError exception MUST be thrown for any attempt to change the value.
        if channel_count != self.number_of_outputs {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Channel count must be equal to number of outputs"),
            ));
        }

        self.node.set_channel_count_impl(channel_count)
    }

    pub(crate) fn set_channel_count_mode_override(
        &self,
        _: &mut CallContext,
        channel_count_mode: ChannelCountMode,
    ) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
        // The channel count mode cannot be changed from "explicit" and an InvalidStateError exception MUST be thrown for any attempt to change the value.
        if channel_count_mode != ChannelCountMode::Explicit {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Channel count mode must be 'explicit'"),
            ));
        }

        self.node.set_channel_count_mode_impl(channel_count_mode)
    }

    pub(crate) fn set_channel_interpretation_override(
        &self,
        _: &mut CallContext,
        channel_interpretation: ChannelInterpretation,
    ) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#audionode-channelinterpretation-constraints
        // The channel interpretation can not be changed from "discrete" and a InvalidStateError exception MUST be thrown for any attempt to change the value.
        if channel_interpretation != ChannelInterpretation::Discrete {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Channel interpretation must be 'discrete'"),
            ));
        }

        self.node.set_channel_interpretation_impl(channel_interpretation)
    }
}
