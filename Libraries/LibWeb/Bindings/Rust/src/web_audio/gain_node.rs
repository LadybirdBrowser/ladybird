/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use libjs_rust::api::Trace;
use libweb_rust::web_audio::rendering::render_nodes::create_gain_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#GainNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct GainNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-gainnode-gain
    gain: Ref<AudioParam>,
}

impl Inherits<AudioNode> for GainNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for GainNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for GainNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct GainOptions {
    pub parent: AudioNodeOptions,
    pub gain: f32,
}

impl GainNode {
    // https://webaudio.github.io/web-audio-api/#dom-gainnode-gainnode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: GainOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &GainOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // Create the node and allocate memory
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            gain: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.gain,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
        });
        node.node.set_this(AnyAudioNode::GainNode(node));
        node.node.event_target().set_owner(node);
        node.gain.set_owner(AnyAudioNode::GainNode(node));

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#GainNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to no
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node.queue_render_node_creation(create_gain_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            Arc::clone(node.gain.render_param()),
        ));
        Ok(node)
    }

    pub(crate) fn gain(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.gain)
    }
}
