/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::rendering::node_message::NodeMessage;
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

use super::rendering::audio_data::{AudioBufferContents, PeriodicWaveData, WaveShaperCurve};
use super::rendering::biquad_coefficients::BiquadFilterType;
use super::rendering::convolver_kernel::{ConvolverDelayLine, ConvolverKernel};
use super::rendering::node_message::*;
use super::rendering::render_node::RenderAudioParam;
use ak::Span;

pub struct ControlMessages {
    pub messages: Vec<ControlMessage>,
}

use std::ffi::c_void;
use std::sync::Arc;

pub(crate) unsafe fn clone_arc<T>(pointer: *const T) -> Arc<T> {
    unsafe {
        Arc::increment_strong_count(pointer);
        Arc::from_raw(pointer)
    }
}

unsafe fn clone_optional_arc<T>(pointer: *const T) -> Option<Arc<T>> {
    if pointer.is_null() {
        None
    } else {
        Some(unsafe { clone_arc(pointer) })
    }
}

#[repr(C)]
pub struct FfiOutgoingParamConnection {
    pub destination: *const RenderAudioParam,
    pub output_index: usize,
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

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_add_node(node: *mut NodeHandle) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::AddNode(AddNode {
        node: unsafe { Box::from_raw(node) },
    })))
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_message_remove_node(node_id: u64) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::RemoveNode(RemoveNode { node_id })))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_replace_connections(
    source: u64,
    node_connections: Span<'_, OutgoingNodeConnection>,
    param_connections: Span<'_, FfiOutgoingParamConnection>,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::ReplaceConnections(ReplaceConnections {
        source,
        node_connections: node_connections.as_slice().to_vec(),
        param_connections: param_connections
            .as_slice()
            .iter()
            .map(|connection| OutgoingParamConnection {
                destination: unsafe { clone_arc(connection.destination) },
                output_index: connection.output_index,
            })
            .collect(),
    })))
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_message_set_channel_config(
    node_id: u64,
    channel_count: usize,
    channel_count_mode: ChannelCountMode,
    channel_interpretation: ChannelInterpretation,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::SetChannelConfig(SetChannelConfig {
        node_id,
        channel_count,
        channel_count_mode,
        channel_interpretation,
    })))
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_message_start_source(node_id: u64, when: f64) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(NodeMessage::StartSource(
        StartSource { node_id, when },
    ))))
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_message_stop_source(node_id: u64, when: f64) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(NodeMessage::StopSource(
        StopSource { node_id, when },
    ))))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_start_buffer_source(
    node_id: u64,
    when: f64,
    offset: f64,
    has_duration: bool,
    duration: f64,
    buffer: *const AudioBufferContents,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(NodeMessage::StartBufferSource(
        StartBufferSource {
            node_id,
            when,
            offset,
            duration: has_duration.then_some(duration),
            buffer: unsafe { clone_optional_arc(buffer) },
        },
    ))))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_set_buffer_source_parameters(
    node_id: u64,
    r#loop: bool,
    loop_start: f64,
    loop_end: f64,
    buffer: *const AudioBufferContents,
    update_buffer: bool,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(
        NodeMessage::SetBufferSourceParameters(SetBufferSourceParameters {
            node_id,
            r#loop,
            loop_start,
            loop_end,
            buffer: unsafe { clone_optional_arc(buffer) },
            update_buffer,
        }),
    )))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_set_oscillator_waveform(
    node_id: u64,
    r#type: OscillatorType,
    periodic_wave: *const PeriodicWaveData,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(
        NodeMessage::SetOscillatorWaveform(SetOscillatorWaveform {
            node_id,
            r#type,
            periodic_wave: unsafe { clone_optional_arc(periodic_wave) },
        }),
    )))
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_message_set_biquad_filter_type(
    node_id: u64,
    r#type: BiquadFilterType,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(NodeMessage::SetBiquadFilterType(
        SetBiquadFilterType { node_id, r#type },
    ))))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_set_convolver_kernel(
    node_id: u64,
    kernel: *const ConvolverKernel,
    delay_line: *const ConvolverDelayLine,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(NodeMessage::SetConvolverKernel(
        SetConvolverKernel {
            node_id,
            kernel: unsafe { clone_optional_arc(kernel) },
            delay_line: unsafe { clone_optional_arc(delay_line) },
        },
    ))))
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_message_set_panner_parameters(
    node_id: u64,
    panning_model: PanningModelType,
    distance_model: DistanceModelType,
    ref_distance: f64,
    max_distance: f64,
    rolloff_factor: f64,
    cone_inner_angle: f64,
    cone_outer_angle: f64,
    cone_outer_gain: f64,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(NodeMessage::SetPannerParameters(
        SetPannerParameters {
            node_id,
            panning_model,
            distance_model,
            ref_distance,
            max_distance,
            rolloff_factor,
            cone_inner_angle,
            cone_outer_angle,
            cone_outer_gain,
        },
    ))))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_set_wave_shaper_parameters(
    node_id: u64,
    curve: *const WaveShaperCurve,
    oversample: OverSampleType,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(
        NodeMessage::SetWaveShaperParameters(SetWaveShaperParameters {
            node_id,
            curve: unsafe { clone_optional_arc(curve) },
            oversample,
        }),
    )))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_message_set_media_stream_source_ring(
    node_id: u64,
    ring: FfiSharedCppObject,
    channel_count: u32,
) -> *mut ControlMessage {
    Box::into_raw(Box::new(ControlMessage::NodeMessage(
        NodeMessage::SetMediaStreamSourceRing(SetMediaStreamSourceRing {
            node_id,
            ring: unsafe { ring.acquire() },
            channel_count,
        }),
    )))
}
