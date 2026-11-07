/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::sync::Arc;

use super::rendering::node_message::{NodeMessage, SharedCppObject};
use super::rendering::render_node::{
    ChannelCountMode, ChannelInterpretation, NodeHandle, OutgoingNodeConnection, OutgoingParamConnection,
};

// Transfers a freshly constructed render node to the rendering thread.
pub struct AddNode {
    pub node: Box<NodeHandle>,
}

// Retires the render node of an AudioNode that is no longer alive on the control thread. The rendering loop only
// considers "the set of all nodes created by this BaseAudioContext, and still alive", so a node that was garbage
// collected must leave the render graph as well.
// https://webaudio.github.io/web-audio-api/#rendering-loop
pub struct RemoveNode {
    pub node_id: u64,
}

// Replaces the source node's outgoing connections with a full snapshot taken after a connect() or disconnect() call.
pub struct ReplaceConnections {
    pub source: u64,
    pub node_connections: Vec<OutgoingNodeConnection>,
    pub param_connections: Vec<OutgoingParamConnection>,
}

pub struct SetChannelConfig {
    pub node_id: u64,
    pub channel_count: usize,
    pub channel_count_mode: ChannelCountMode,
    pub channel_interpretation: ChannelInterpretation,
}

// https://webaudio.github.io/web-audio-api/#control-message
pub enum ControlMessage {
    AddNode(AddNode),
    RemoveNode(RemoveNode),
    ReplaceConnections(ReplaceConnections),
    SetChannelConfig(SetChannelConfig),
    NodeMessage(NodeMessage),
}

pub(crate) unsafe fn clone_arc<T>(pointer: *const T) -> Arc<T> {
    unsafe {
        Arc::increment_strong_count(pointer);
        Arc::from_raw(pointer)
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiSharedCppObject {
    pub pointer: *mut c_void,
    pub retain: unsafe extern "C" fn(*mut c_void),
    pub release: unsafe extern "C" fn(*mut c_void),
}

impl FfiSharedCppObject {
    pub(crate) unsafe fn acquire(self) -> Option<SharedCppObject> {
        if self.pointer.is_null() {
            return None;
        }
        unsafe {
            (self.retain)(self.pointer);
        }
        Some(SharedCppObject {
            pointer: self.pointer,
            retain: self.retain,
            release: self.release,
        })
    }
}
