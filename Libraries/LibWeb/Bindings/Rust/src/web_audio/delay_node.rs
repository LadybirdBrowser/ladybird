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
use libweb_rust::web_audio::rendering::render_nodes::create_delay_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#DelayNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DelayNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-delaynode-delaytime
    delay_time: Ref<AudioParam>,
}

impl Inherits<AudioNode> for DelayNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for DelayNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for DelayNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct DelayOptions {
    pub parent: AudioNodeOptions,
    pub delay_time: f64,
    pub max_delay_time: f64,
}

impl DelayNode {
    pub(crate) fn validate_options(options: &DelayOptions) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#dom-delayoptions-maxdelaytime
        // If specified, this value MUST be greater than zero and less than three minutes or a NotSupportedError exception MUST be thrown.
        const MAXIMUM_DELAY_TIME_SECONDS: f64 = 180.;
        if options.max_delay_time <= 0.
            || options.max_delay_time >= MAXIMUM_DELAY_TIME_SECONDS
            || options.max_delay_time.is_nan()
        {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Max delay time must be between 0 and 180 seconds exclusive"),
            ));
        }
        Ok(())
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: DelayOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::validate_options(&options)?;
        Self::create(cx, context, &options)
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &DelayOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            delay_time: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.delay_time as f32,
                0.,
                options.max_delay_time as f32,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
        });
        node.node.set_this(AnyAudioNode::DelayNode(node));
        node.node.event_target().set_owner(node);
        node.delay_time.set_owner(AnyAudioNode::DelayNode(node));

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#DelayNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to yes
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node.queue_render_node_creation(create_delay_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            Arc::clone(node.delay_time.render_param()),
            options.max_delay_time,
        ));
        Ok(node)
    }

    pub(crate) fn delay_time(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.delay_time)
    }
}
