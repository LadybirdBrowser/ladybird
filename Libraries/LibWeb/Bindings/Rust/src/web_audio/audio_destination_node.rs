/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::collapsible_if, reason = "preserve the C++ branch structure")]

use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::rendering::render_nodes::create_destination_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#AudioDestinationNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioDestinationNode {
    node: AudioNode,
}

impl Inherits<AudioNode> for AudioDestinationNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for AudioDestinationNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for AudioDestinationNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl AudioDestinationNode {
    // https://webaudio.github.io/web-audio-api/#dom-audiodestinationnode-maxchannelcount
    pub(crate) fn max_channel_count(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.max_channel_count_value())
    }

    fn max_channel_count_value(&self) -> u32 {
        // The maximum number of channels that the channelCount attribute can be set to. An AudioDestinationNode
        // representing the audio hardware end-point (the normal case) can potentially output more than 2 channels of
        // audio if the audio hardware is multi-channel. maxChannelCount is the maximum number of channels that this
        // hardware is capable of supporting.
        // NB: For an OfflineAudioContext, channelCount may not be changed, so the maximum equals the channel count the
        //     context was constructed with. For an AudioContext, we follow the recommendation from the spec's privacy
        //     considerations that maxChannelCount be set to two (stereo) instead of exposing the audio hardware's
        //     actual capabilities.
        if matches!(self.node.context_value(), AnyBaseAudioContext::OfflineAudioContext(_)) {
            return self.node.channel_count_value();
        }
        2
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        channel_count: u32,
    ) -> WebIdlResult<Ref<Self>> {
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, channel_count),
        });
        node.node.set_this(AnyAudioNode::AudioDestinationNode(node));
        node.node.event_target().set_owner(node);

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#AudioDestinationNode
        let default_options = AudioNodeDefaultOptions {
            channel_count,
            channel_count_mode: ChannelCountMode::Explicit,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to no
        node.node
            .initialize_audio_node_options(cx, &AudioNodeOptions::default(), &default_options)?;
        node.node.queue_render_node_creation(create_destination_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            channel_count as usize,
        ));
        Ok(node)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, channel_count: u32) -> WebIdlResult<()> {
        if channel_count == self.node.channel_count_value() {
            return Ok(());
        }

        // The behavior depends on whether the destination node is the destination of an AudioContext
        // or OfflineAudioContext:

        // AudioContext: The channel count MUST be between 1 and maxChannelCount. An IndexSizeError
        // exception MUST be thrown for any attempt to set the count outside this range.
        // NB: Setting the count to zero throws NotSupportedError through AudioNode's channelCount validation below,
        //     taking precedence over the range check here.
        if matches!(self.node.context_value(), AnyBaseAudioContext::AudioContext(_)) {
            if channel_count > self.max_channel_count_value() {
                return Err(Exception::Dom(
                    "IndexSizeError",
                    Utf16String::from_ascii(b"Channel index is out of range"),
                ));
            }
        }

        // OfflineAudioContext: This value may not be changed; a NotSupportedError exception MUST be thrown if
        // channelCount is changed to a different value.
        if matches!(self.node.context_value(), AnyBaseAudioContext::OfflineAudioContext(_)) {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Cannot change channel count in an OfflineAudioContext"),
            ));
        }
        self.node.set_channel_count_impl(channel_count)
    }
}
