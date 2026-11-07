/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::rendering::render_nodes::create_stereo_panner_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#stereopannernode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StereoPannerNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-stereopannernode-pan
    pan: Ref<AudioParam>,
}

impl Inherits<AudioNode> for StereoPannerNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for StereoPannerNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for StereoPannerNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct StereoPannerOptions {
    pub parent: AudioNodeOptions,
    pub pan: f32,
}

impl StereoPannerNode {
    // https://webaudio.github.io/web-audio-api/#dom-stereopannernode-stereopannernode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: StereoPannerOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &StereoPannerOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // Create the node and allocate memory
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            pan: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.pan,
                -1.,
                1.,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
        });
        node.node.set_this(AnyAudioNode::StereoPannerNode(node));
        node.node.event_target().set_owner(node);
        node.pan.set_owner(AnyAudioNode::StereoPannerNode(node));

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#stereopannernode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::ClampedMax,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to no
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node.queue_render_node_creation(create_stereo_panner_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            Arc::clone(node.pan.render_param()),
        ));
        Ok(node)
    }

    pub(crate) fn pan(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.pan)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, channel_count: u32) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
        // The channel count cannot be greater than two, and a NotSupportedError exception MUST be thrown for any attempt to change it to a value greater than two.
        if channel_count > 2 {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"StereoPannerNode does not support channel count greater than 2"),
            ));
        }

        self.node.set_channel_count_impl(channel_count)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
    pub(crate) fn set_channel_count_mode_override(
        &self,
        _: &mut CallContext,
        mode: ChannelCountMode,
    ) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
        // The channel count mode cannot be set to "max", and a NotSupportedError exception MUST be thrown for any attempt to set it to "max".
        if mode == ChannelCountMode::Max {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"StereoPannerNode does not support 'max' as channelCountMode."),
            ));
        }

        // If the mode is valid, call the base class implementation
        self.node.set_channel_count_mode_impl(mode)
    }
}
