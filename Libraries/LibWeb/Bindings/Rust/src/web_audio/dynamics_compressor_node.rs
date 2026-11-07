/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::rendering::render_nodes::create_passthrough_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#DynamicsCompressorNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DynamicsCompressorNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-threshold
    threshold: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-knee
    knee: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-ratio
    ratio: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-attack
    attack: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-release
    release: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-internal-reduction-slot
    reduction: f32, // [[internal reduction]]
}

impl Inherits<AudioNode> for DynamicsCompressorNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for DynamicsCompressorNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for DynamicsCompressorNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct DynamicsCompressorOptions {
    pub parent: AudioNodeOptions,
    pub threshold: f32,
    pub knee: f32,
    pub ratio: f32,
    pub attack: f32,
    pub release: f32,
}

impl DynamicsCompressorNode {
    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-dynamicscompressornode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: DynamicsCompressorOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &DynamicsCompressorOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // Create the node and allocate memory
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            threshold: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.threshold,
                -100.,
                0.,
                AutomationRate::KRate,
                FixedAutomationRate::Yes,
            ),
            knee: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.knee,
                0.,
                40.,
                AutomationRate::KRate,
                FixedAutomationRate::Yes,
            ),
            ratio: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.ratio,
                1.,
                20.,
                AutomationRate::KRate,
                FixedAutomationRate::Yes,
            ),
            attack: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.attack,
                0.,
                1.,
                AutomationRate::KRate,
                FixedAutomationRate::Yes,
            ),
            release: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.release,
                0.,
                1.,
                AutomationRate::KRate,
                FixedAutomationRate::Yes,
            ),
            reduction: 0.,
        });
        node.node.set_this(AnyAudioNode::DynamicsCompressorNode(node));
        node.node.event_target().set_owner(node);
        node.threshold.set_owner(AnyAudioNode::DynamicsCompressorNode(node));
        node.knee.set_owner(AnyAudioNode::DynamicsCompressorNode(node));
        node.ratio.set_owner(AnyAudioNode::DynamicsCompressorNode(node));
        node.attack.set_owner(AnyAudioNode::DynamicsCompressorNode(node));
        node.release.set_owner(AnyAudioNode::DynamicsCompressorNode(node));

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#DynamicsCompressorNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::ClampedMax,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to yes
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;

        // FIXME: Implement actual dynamics compression; for now the signal passes through unchanged.
        node.node.queue_render_node_creation(create_passthrough_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
        ));
        Ok(node)
    }

    pub(crate) fn threshold(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.threshold)
    }

    pub(crate) fn knee(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.knee)
    }

    pub(crate) fn ratio(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.ratio)
    }

    pub(crate) fn attack(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.attack)
    }

    pub(crate) fn release(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.release)
    }

    pub(crate) fn reduction(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.reduction)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, channel_count: u32) -> WebIdlResult<()> {
        if channel_count > 2 {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"DynamicsCompressorNode does not support channel count greater than 2"),
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
        if mode == ChannelCountMode::Max {
            // Return a NotSupportedError if 'max' is used
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"DynamicsCompressorNode does not support 'max' as channelCountMode."),
            ));
        }

        // If the mode is valid, call the base class implementation
        self.node.set_channel_count_mode_impl(mode)
    }
}
