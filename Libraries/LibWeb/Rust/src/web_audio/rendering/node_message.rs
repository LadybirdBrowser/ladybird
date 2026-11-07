/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::sync::Arc;

use super::biquad_coefficients::BiquadFilterType;
use super::convolver_kernel::{ConvolverDelayLine, ConvolverKernel};

pub use super::audio_data::{AudioBufferContents, PeriodicWaveData, WaveShaperCurve};

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum OscillatorType {
    Sine,
    Square,
    Sawtooth,
    Triangle,
    Custom,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum DistanceModelType {
    Linear,
    Inverse,
    Exponential,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PanningModelType {
    Equalpower,
    HRTF,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum OverSampleType {
    None,
    _2x,
    _4x,
}

pub struct StartSource {
    pub node_id: u64,
    pub when: f64,
}

pub struct StopSource {
    pub node_id: u64,
    pub when: f64,
}

// Starts an AudioBufferSourceNode, transferring the acquired buffer contents along with the playback range.
pub struct StartBufferSource {
    pub node_id: u64,
    pub when: f64,
    pub offset: f64,
    pub duration: Option<f64>,
    pub buffer: Option<Arc<AudioBufferContents>>,
}

// Updates an AudioBufferSourceNode's loop parameters, and its buffer if it was assigned after the node started.
pub struct SetBufferSourceParameters {
    pub node_id: u64,
    pub r#loop: bool,
    pub loop_start: f64,
    pub loop_end: f64,
    pub buffer: Option<Arc<AudioBufferContents>>,
    pub update_buffer: bool,
}

pub struct SetOscillatorWaveform {
    pub node_id: u64,
    pub r#type: OscillatorType,
    pub periodic_wave: Option<Arc<PeriodicWaveData>>,
}

pub struct SetBiquadFilterType {
    pub node_id: u64,
    pub r#type: BiquadFilterType,
}

pub struct SetConvolverKernel {
    pub node_id: u64,
    pub kernel: Option<Arc<ConvolverKernel>>,
    pub delay_line: Option<Arc<ConvolverDelayLine>>,
}

#[derive(Clone, Copy)]
pub struct SetPannerParameters {
    pub node_id: u64,
    pub panning_model: PanningModelType,
    pub distance_model: DistanceModelType,
    pub ref_distance: f64,
    pub max_distance: f64,
    pub rolloff_factor: f64,
    pub cone_inner_angle: f64,
    pub cone_outer_angle: f64,
    pub cone_outer_gain: f64,
}

impl Default for SetPannerParameters {
    fn default() -> Self {
        Self {
            node_id: 0,
            panning_model: PanningModelType::Equalpower,
            distance_model: DistanceModelType::Inverse,
            ref_distance: 1.,
            max_distance: 10000.,
            rolloff_factor: 1.,
            cone_inner_angle: 360.,
            cone_outer_angle: 360.,
            cone_outer_gain: 0.,
        }
    }
}

pub struct SetWaveShaperParameters {
    pub node_id: u64,
    pub curve: Option<Arc<WaveShaperCurve>>,
    pub oversample: OverSampleType,
}

// Attaches the ring buffer that carries a MediaStreamTrack's audio into a
// MediaStreamAudioSourceNode's render node. Shipped as a message rather than a constructor
// argument so the ring can be replaced without rebuilding the render node.
pub struct SetMediaStreamSourceRing {
    pub node_id: u64,
    pub ring: Option<SharedCppObject>,
    pub channel_count: u32,
}

pub struct SharedCppObject {
    pub pointer: *mut c_void,
    pub retain: unsafe extern "C" fn(*mut c_void),
    pub release: unsafe extern "C" fn(*mut c_void),
}

// SAFETY: These handles own references to C++ objects whose reference counts are atomic.
unsafe impl Send for SharedCppObject {}
unsafe impl Sync for SharedCppObject {}
impl Clone for SharedCppObject {
    fn clone(&self) -> Self {
        unsafe {
            (self.retain)(self.pointer);
        }
        Self {
            pointer: self.pointer,
            retain: self.retain,
            release: self.release,
        }
    }
}

impl Drop for SharedCppObject {
    fn drop(&mut self) {
        unsafe {
            (self.release)(self.pointer);
        }
    }
}

// A control message that updates the state of a single render node.
pub enum NodeMessage {
    SetBiquadFilterType(SetBiquadFilterType),
    SetBufferSourceParameters(SetBufferSourceParameters),
    SetConvolverKernel(SetConvolverKernel),
    SetMediaStreamSourceRing(SetMediaStreamSourceRing),
    SetOscillatorWaveform(SetOscillatorWaveform),
    SetPannerParameters(SetPannerParameters),
    SetWaveShaperParameters(SetWaveShaperParameters),
    StartBufferSource(StartBufferSource),
    StartSource(StartSource),
    StopSource(StopSource),
}

pub fn node_message_target(message: &NodeMessage) -> u64 {
    match message {
        NodeMessage::SetBiquadFilterType(value) => value.node_id,
        NodeMessage::SetBufferSourceParameters(value) => value.node_id,
        NodeMessage::SetConvolverKernel(value) => value.node_id,
        NodeMessage::SetMediaStreamSourceRing(value) => value.node_id,
        NodeMessage::SetOscillatorWaveform(value) => value.node_id,
        NodeMessage::SetPannerParameters(value) => value.node_id,
        NodeMessage::SetWaveShaperParameters(value) => value.node_id,
        NodeMessage::StartBufferSource(value) => value.node_id,
        NodeMessage::StartSource(value) => value.node_id,
        NodeMessage::StopSource(value) => value.node_id,
    }
}
