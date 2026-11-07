/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::DeferGc;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, StartSource, StopSource};

use super::audio_node::AudioNode;
use super::base_audio_context::AnyBaseAudioContext;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#AudioScheduledSourceNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioScheduledSourceNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-source-started-slot
    source_started: Cell<bool>,
}

impl Inherits<AudioNode> for AudioScheduledSourceNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for AudioScheduledSourceNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for AudioScheduledSourceNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl AudioScheduledSourceNode {
    pub(crate) fn new(defer_gc: &DeferGc<'_>, context: AnyBaseAudioContext) -> Self {
        Self {
            node: AudioNode::new(defer_gc, context, 2),
            source_started: Cell::new(false),
        }
    }

    pub(crate) fn source_started(&self) -> bool {
        self.source_started.get()
    }

    pub(crate) fn set_source_started(&self, value: bool) {
        self.source_started.set(value);
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-start
    pub(crate) fn start(&self, cx: &mut CallContext, when: f64) -> WebIdlResult<()> {
        // 1. If this AudioScheduledSourceNode internal slot [[source started]] is true, an InvalidStateError exception MUST be thrown.
        if self.source_started() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"AudioScheduledSourceNode source has already started"),
            ));
        }

        // 2. Check for any errors that must be thrown due to parameter constraints described below.
        //    If any exception is thrown during this step, abort those steps.
        //    A RangeError exception MUST be thrown if when is negative.
        if when < 0. {
            return Err(Exception::range_error("when must not be negative"));
        }

        // 3. Set the internal slot [[source started]] on this AudioScheduledSourceNode to true.
        self.set_source_started(true);

        // 4. Queue a control message to start the AudioScheduledSourceNode, including the parameter values in the message.
        self.node
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::StartSource(StartSource {
                node_id: self.node.node_id(),
                when,
            })));
        self.node.context_value().add_playing_source(cx.vm(), self.node.this());

        // FIXME: 5. Send a control message to the associated AudioContext to start running its rendering thread only when all the following conditions are met:
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-stop
    pub(crate) fn stop(&self, _: &mut CallContext, when: f64) -> WebIdlResult<()> {
        // 1. If this AudioScheduledSourceNode internal slot [[source started]] is not true, an InvalidStateError exception MUST be thrown.
        if !self.source_started.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"AudioScheduledSourceNode source has not been started"),
            ));
        }

        // 2. Check for any errors that must be thrown due to parameter constraints described below.
        //    A RangeError exception MUST be thrown if when is negative.
        if when < 0. {
            return Err(Exception::range_error("when must not be negative"));
        }

        // 3. Queue a control message to stop the AudioScheduledSourceNode, including the parameter values in the message.
        self.node
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::StopSource(StopSource {
                node_id: self.node.node_id(),
                when,
            })));
        Ok(())
    }
}
