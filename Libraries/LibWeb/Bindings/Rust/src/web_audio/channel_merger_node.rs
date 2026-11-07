/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::rendering::render_nodes::create_channel_merger_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#ChannelMergerNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ChannelMergerNode {
    node: AudioNode,
    number_of_inputs: u32,
}

impl Inherits<AudioNode> for ChannelMergerNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for ChannelMergerNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for ChannelMergerNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct ChannelMergerOptions {
    pub parent: AudioNodeOptions,
    pub number_of_inputs: u32,
}

impl ChannelMergerNode {
    pub(crate) fn validate_options(options: &ChannelMergerOptions) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelmerger
        // An IndexSizeError exception MUST be thrown if numberOfInputs is less than 1 or is greater
        // than the number of supported channels.
        if options.number_of_inputs < 1 || options.number_of_inputs > BaseAudioContext::MAX_NUMBER_OF_CHANNELS {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Invalid number of inputs"),
            ));
        }
        Ok(())
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: ChannelMergerOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::validate_options(&options)?;
        Self::create(cx, context, &options)
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &ChannelMergerOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            number_of_inputs: options.number_of_inputs,
        });
        node.node.set_this(AnyAudioNode::ChannelMergerNode(node));
        node.node.event_target().set_owner(node);

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#ChannelMergerNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 1,
            channel_count_mode: ChannelCountMode::Explicit,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to no
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node.queue_render_node_creation(create_channel_merger_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            options.number_of_inputs as usize,
        ));
        Ok(node)
    }

    pub(crate) fn number_of_inputs_value(&self) -> u32 {
        self.number_of_inputs
    }

    pub(crate) fn number_of_outputs_value(&self) -> u32 {
        1
    }

    // https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, channel_count: u32) -> WebIdlResult<()> {
        // The channel count cannot be changed, and an InvalidStateError exception MUST be thrown for
        // any attempt to change the value.
        if channel_count != 1 {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Channel count cannot be changed"),
            ));
        }

        self.node.set_channel_count_impl(channel_count)
    }

    pub(crate) fn set_channel_count_mode_override(
        &self,
        _: &mut CallContext,
        channel_count_mode: ChannelCountMode,
    ) -> WebIdlResult<()> {
        // The channel count mode cannot be changed from "explicit" and an InvalidStateError exception
        // MUST be thrown for any attempt to change the value.
        if channel_count_mode != ChannelCountMode::Explicit {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Channel count mode cannot be changed"),
            ));
        }

        self.node.set_channel_count_mode_impl(channel_count_mode)
    }
}
