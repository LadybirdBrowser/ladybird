/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use libjs_rust::api::Trace;
use libweb_rust::web_audio::rendering::render_nodes::create_constant_source_render_node;

use super::audio_node::AnyAudioNode;
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::audio_scheduled_source_node::AudioScheduledSourceNode;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#ConstantSourceNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ConstantSourceNode {
    node: AudioScheduledSourceNode,

    // https://webaudio.github.io/web-audio-api/#dom-constantsourcenode-offset
    offset: Ref<AudioParam>,
}

impl Inherits<AudioScheduledSourceNode> for ConstantSourceNode {
    fn parent(&self) -> &AudioScheduledSourceNode {
        &self.node
    }
}

impl Wrappable for ConstantSourceNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.parent().finalize_node();
    }
}

impl GetTheParent for ConstantSourceNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct ConstantSourceOptions {
    pub offset: f32,
}

impl ConstantSourceNode {
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: ConstantSourceOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &ConstantSourceOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioScheduledSourceNode::new(defer_gc, context),
            offset: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.offset,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
        });
        node.node.parent().set_this(AnyAudioNode::ConstantSourceNode(node));
        node.node.parent().event_target().set_owner(node);
        node.offset.set_owner(AnyAudioNode::ConstantSourceNode(node));
        node.node
            .parent()
            .queue_render_node_creation(create_constant_source_render_node(
                node.node.parent().node_id(),
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                Arc::clone(node.offset.render_param()),
            ));
        Ok(node)
    }

    pub(crate) fn offset(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.offset)
    }
}
