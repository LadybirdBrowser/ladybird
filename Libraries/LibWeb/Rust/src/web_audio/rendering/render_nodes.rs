/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(
    clippy::needless_range_loop,
    clippy::manual_memcpy,
    clippy::mut_from_ref,
    clippy::needless_late_init,
    clippy::neg_cmp_op_on_partial_ord
)]

use std::cell::UnsafeCell;
use std::sync::{Arc, OnceLock};

use ak::math;

use super::audio_bus::AudioBus;
use super::biquad_coefficients::{BiquadFilterType, compute_biquad_coefficients};
use super::convolver_kernel::ConvolverKernel;
use super::fft::{FFT, FFTDirection};
use super::node_message::*;
use super::render_graph::RenderGraph;
use super::render_node::*;
use crate::web_audio::MAX_NUMBER_OF_CHANNELS;

// Additive synthesis of the built-in oscillator waveforms is capped to keep the per-sample cost bounded for very low
// fundamental frequencies.
const MAX_OSCILLATOR_HARMONICS: usize = 5000;
const FRAME_BOUNDARY_SNAPPING_TOLERANCE: f64 = 1. / 2048.;
const PI: f64 = std::f64::consts::PI;

fn clamp<T: PartialOrd + Copy>(value: T, min: T, max: T) -> T {
    assert!(max >= min);
    if value > max {
        return max;
    }
    if value < min {
        return min;
    }
    value
}

fn cpp_min<T: PartialOrd + Copy>(a: T, b: T) -> T {
    if b < a { b } else { a }
}

fn cpp_max<T: PartialOrd + Copy>(a: T, b: T) -> T {
    if a < b { b } else { a }
}

pub(super) struct BusRef(pub(super) *mut AudioBus);

impl BusRef {
    pub(super) unsafe fn channel_count(&self) -> usize {
        unsafe { &*self.0 }.channel_count()
    }

    pub(super) unsafe fn set_channel_count(&self, count: usize) {
        unsafe { &mut *self.0 }.set_channel_count(count);
    }

    pub(super) unsafe fn zero(&self) {
        unsafe { &mut *self.0 }.zero();
    }

    unsafe fn is_silent(&self) -> bool {
        unsafe { &*self.0 }.is_silent()
    }

    pub(super) unsafe fn channel(&self, index: usize) -> &[f32] {
        let (pointer, length) = unsafe { AudioBus::channel_pointer(self.0, index) };
        unsafe { ak::slice_from_raw_parts(pointer, length) }
    }

    pub(super) unsafe fn channel_mut(&self, index: usize) -> &mut [f32] {
        let (pointer, length) = unsafe { AudioBus::channel_pointer(self.0, index) };
        unsafe { ak::slice_from_raw_parts_mut(pointer, length) }
    }

    pub(super) unsafe fn copy_from(&self, source: &BusRef) {
        unsafe { &mut *self.0 }.copy_from(unsafe { &*source.0 });
    }

    unsafe fn sum_from(&self, source: &BusRef, interpretation: ChannelInterpretation) {
        unsafe { &mut *self.0 }.sum_from(unsafe { &*source.0 }, interpretation == ChannelInterpretation::Speakers);
    }
}

pub(super) trait NodeProcessor: Send {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext);

    fn handle_message(&mut self, _message: &NodeMessage) {}
    fn for_each_param(&self, _callback: &mut dyn FnMut(&RenderAudioParam)) {}
    fn take_ended_notification(&mut self) -> bool {
        false
    }
    fn has_tail_remaining(&self) -> bool {
        false
    }
}

struct NativeRenderNode<T: NodeProcessor> {
    data: RenderNodeData,
    processor: UnsafeCell<T>,
}

impl<T: NodeProcessor> RenderNode for NativeRenderNode<T> {
    fn data(&self) -> &RenderNodeData {
        &self.data
    }

    unsafe fn process(&self, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            (&mut *self.processor.get()).process(&self.data, graph, context);
        }
    }

    unsafe fn handle_message(&self, message: &NodeMessage) {
        unsafe { &mut *self.processor.get() }.handle_message(message);
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        unsafe { &*self.processor.get() }.for_each_param(callback);
    }

    unsafe fn take_ended_notification(&self) -> bool {
        unsafe { &mut *self.processor.get() }.take_ended_notification()
    }

    unsafe fn has_tail_remaining(&self) -> bool {
        unsafe { &*self.processor.get() }.has_tail_remaining()
    }
}

pub(super) fn node_handle<T: NodeProcessor + 'static>(data: RenderNodeData, processor: T) -> Box<NodeHandle> {
    Box::new(NodeHandle {
        node: Box::new(NativeRenderNode {
            data,
            processor: UnsafeCell::new(processor),
        }),
    })
}

struct PanGains {
    left: f32,
    right: f32,
}

// The equal-power panning curve maps a position in [0, 1] to a left/right gain pair whose squared sum is 1, shared by
// the StereoPannerNode and the equalpower PannerNode.
// https://webaudio.github.io/web-audio-api/#stereopanner-algorithm
fn equal_power_pan_gains(position: f64) -> PanGains {
    PanGains {
        left: math::cos(position * PI / 2.) as f32,
        right: math::sin(position * PI / 2.) as f32,
    }
}

// Captures the mixed input signal so the renderer can copy it into the rendered buffer.
// https://webaudio.github.io/web-audio-api/#AudioDestinationNode
struct DestinationRenderNode;

impl NodeProcessor for DestinationRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            let destination_output = BusRef(data.output(0));
            destination_output.set_channel_count(data.channel_count.get());
            destination_output.zero();
            destination_output.sum_from(&input, data.channel_interpretation.get());
        }
    }
}

// https://webaudio.github.io/web-audio-api/#GainNode
struct GainRenderNode {
    gain: Arc<RenderAudioParam>,
    gain_values: Vec<f32>,
}

impl NodeProcessor for GainRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            let gain_output = BusRef(data.output(0));
            gain_output.set_channel_count(input.channel_count());
            if input.is_silent() {
                gain_output.zero();
                return;
            }
            self.gain.compute(graph, context, &mut self.gain_values);
            for channel_index in 0..input.channel_count() {
                let input_samples = input.channel(channel_index);
                let output_samples = gain_output.channel_mut(channel_index);
                for frame in 0..context.quantum_size {
                    output_samples[frame] = input_samples[frame] * self.gain_values[frame];
                }
            }
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.gain);
    }
}

// Base class for source nodes that are scheduled with start() and stop().
// https://webaudio.github.io/web-audio-api/#AudioScheduledSourceNode
struct ScheduledSourceRenderNode {
    start_time: Option<f64>,
    stop_time: f64,
    start_time_needs_clamping: bool,

    // Playback naturally ran out of content before the scheduled stop time, e.g. a non-looping buffer finished.
    content_ended: bool,
    ended: bool,
    ended_pending: bool,
}

impl Default for ScheduledSourceRenderNode {
    fn default() -> Self {
        Self {
            start_time: None,
            stop_time: f64::INFINITY,
            start_time_needs_clamping: false,
            content_ended: false,
            ended: false,
            ended_pending: false,
        }
    }
}

impl ScheduledSourceRenderNode {
    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-start
    fn process(&mut self, context: &RenderContext) {
        // If 0 is passed in for this value or if the value is less than currentTime, then the sound will start playing
        // immediately.
        // NB: The start time is clamped to the first render quantum processed after the start message arrived, since the
        //     control thread's currentTime may already have advanced past the scheduled time by then.
        if self.start_time_needs_clamping {
            // AD-HOC: Start times built by summing buffer durations may induce rounding error and place the start part
            //         of a frame after the prior buffer's end. Snap starts that are close to a frame boundary onto it
            //         to prevent this from becoming audible in continuous streams.
            let start_frame = self.start_time.unwrap() * context.sample_rate;
            let nearest_frame = start_frame.round();
            if (start_frame - nearest_frame).abs() < FRAME_BOUNDARY_SNAPPING_TOLERANCE {
                self.start_time = Some(nearest_frame / context.sample_rate);
            }
            self.start_time = Some(cpp_max(self.start_time.unwrap(), context.quantum_start_time()));
            self.start_time_needs_clamping = false;
        }
    }

    fn handle_message(&mut self, message: &NodeMessage) {
        match message {
            NodeMessage::StartSource(start_source) => {
                self.start_time = Some(start_source.when);
                self.start_time_needs_clamping = true;
            }
            NodeMessage::StopSource(stop_source) => {
                self.stop_time = stop_source.when;
            }
            _ => {}
        }
    }

    fn has_started(&self) -> bool {
        self.start_time.is_some()
    }

    fn is_active_at(&self, time: f64) -> bool {
        self.start_time.is_some_and(|start_time| time >= start_time) && time < self.stop_time && !self.content_ended
    }

    // Marks this node as ended once the quantum has advanced past the stop time or the content was exhausted.
    fn update_ended_state(&mut self, context: &RenderContext) {
        if self.start_time.is_none() || self.ended {
            return;
        }
        let quantum_end_time = context.frame_time(context.quantum_size);
        if quantum_end_time >= self.stop_time || self.content_ended {
            self.ended = true;
            self.ended_pending = true;
        }
    }

    fn take_ended_notification(&mut self) -> bool {
        std::mem::replace(&mut self.ended_pending, false)
    }
}

// https://webaudio.github.io/web-audio-api/#ConstantSourceNode
struct ConstantSourceRenderNode {
    source: ScheduledSourceRenderNode,
    offset: Arc<RenderAudioParam>,
    offset_values: Vec<f32>,
}

impl ConstantSourceRenderNode {
    unsafe fn process_source(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            self.offset.compute(graph, context, &mut self.offset_values);
            let output = BusRef(data.output(0));
            let output_samples = output.channel_mut(0);
            for frame in 0..context.quantum_size {
                output_samples[frame] = if self.source.is_active_at(context.frame_time(frame)) {
                    self.offset_values[frame]
                } else {
                    0.
                };
            }
            self.source.update_ended_state(context);
        }
    }
}

impl NodeProcessor for ConstantSourceRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        self.source.process(context);
        unsafe {
            self.process_source(data, graph, context);
        }
    }

    fn handle_message(&mut self, message: &NodeMessage) {
        self.source.handle_message(message);
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.offset);
    }

    fn take_ended_notification(&mut self) -> bool {
        self.source.take_ended_notification()
    }
}

// https://webaudio.github.io/web-audio-api/#OscillatorNode
struct OscillatorRenderNode {
    source: ScheduledSourceRenderNode,
    frequency: Arc<RenderAudioParam>,
    detune: Arc<RenderAudioParam>,
    frequency_values: Vec<f32>,
    detune_values: Vec<f32>,
    r#type: OscillatorType,
    periodic_wave: Option<Arc<PeriodicWaveData>>,
    periodic_wave_scale: f32,
    phase: f64,
}

impl OscillatorRenderNode {
    // https://webaudio.github.io/web-audio-api/#oscillator-coefficients
    fn sample_waveform(&self, phase: f64, frequency: f64, nyquist_frequency: f64) -> f32 {
        match self.r#type {
            OscillatorType::Sine => math::sin(phase) as f32,
            OscillatorType::Square => {
                // b[n] = 2/(nπ) * (1 - cos(nπ))
                // NB: This is 4/(nπ) for odd n and 0 for even n.
                let mut harmonic_count = if frequency != 0. {
                    (nyquist_frequency / frequency.abs()) as usize
                } else {
                    MAX_OSCILLATOR_HARMONICS
                };
                harmonic_count = harmonic_count.min(MAX_OSCILLATOR_HARMONICS);
                let mut value = 0.;
                for harmonic in (1..=harmonic_count).step_by(2) {
                    let coefficient = 4. / (harmonic as f64 * PI);
                    value += coefficient * math::sin(harmonic as f64 * phase);
                }
                value as f32
            }
            OscillatorType::Sawtooth => {
                // b[n] = (-1)^(n+1) * 2/(nπ)
                let mut harmonic_count = if frequency != 0. {
                    (nyquist_frequency / frequency.abs()) as usize
                } else {
                    MAX_OSCILLATOR_HARMONICS
                };
                harmonic_count = harmonic_count.min(MAX_OSCILLATOR_HARMONICS);
                let mut value = 0.;
                for harmonic in 1..=harmonic_count {
                    let mut coefficient = 2. / (harmonic as f64 * PI);
                    if harmonic % 2 == 0 {
                        coefficient = -coefficient;
                    }
                    value += coefficient * math::sin(harmonic as f64 * phase);
                }
                value as f32
            }
            OscillatorType::Triangle => {
                // b[n] = 8 * sin(nπ/2) / (nπ)²
                // NB: This alternates sign for odd n and is 0 for even n.
                let mut harmonic_count = if frequency != 0. {
                    (nyquist_frequency / frequency.abs()) as usize
                } else {
                    MAX_OSCILLATOR_HARMONICS
                };
                harmonic_count = harmonic_count.min(MAX_OSCILLATOR_HARMONICS);
                let mut value = 0.;
                let mut sign = 1.;
                for harmonic in (1..=harmonic_count).step_by(2) {
                    let coefficient = sign * 8. / ((harmonic * harmonic) as f64 * PI * PI);
                    value += coefficient * math::sin(harmonic as f64 * phase);
                    sign = -sign;
                }
                value as f32
            }
            OscillatorType::Custom => {
                let Some(periodic_wave) = &self.periodic_wave else {
                    return 0.;
                };
                let harmonic_limit = periodic_wave.real.len();
                let mut value = 0.;
                for harmonic_index in 1..harmonic_limit {
                    value += f64::from(periodic_wave.real[harmonic_index]) * math::cos(harmonic_index as f64 * phase)
                        + f64::from(periodic_wave.imag[harmonic_index]) * math::sin(harmonic_index as f64 * phase);
                }
                value as f32 * self.periodic_wave_scale
            }
        }
    }

    unsafe fn process_source(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            self.frequency.compute(graph, context, &mut self.frequency_values);
            self.detune.compute(graph, context, &mut self.detune_values);
            let nyquist_frequency = context.sample_rate / 2.;
            let output = BusRef(data.output(0));
            let output_samples = output.channel_mut(0);
            for frame in 0..context.quantum_size {
                if !self.source.is_active_at(context.frame_time(frame)) {
                    output_samples[frame] = 0.;
                    continue;
                }

                // computedOscFrequency(t) = frequency(t) * pow(2, detune(t) / 1200)
                // NB: The computed frequency is clamped to the Nyquist frequency.
                let mut computed_frequency =
                    f64::from(self.frequency_values[frame]) * math::exp2(f64::from(self.detune_values[frame]) / 1200.);
                computed_frequency = clamp(computed_frequency, -nyquist_frequency, nyquist_frequency);
                output_samples[frame] = self.sample_waveform(self.phase, computed_frequency, nyquist_frequency);
                self.phase += 2. * PI * computed_frequency / context.sample_rate;
                self.phase %= 2. * PI;
                if self.phase < 0. {
                    self.phase += 2. * PI;
                }
            }
            self.source.update_ended_state(context);
        }
    }
}

impl NodeProcessor for OscillatorRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        self.source.process(context);
        unsafe {
            self.process_source(data, graph, context);
        }
    }

    fn handle_message(&mut self, message: &NodeMessage) {
        match message {
            NodeMessage::SetOscillatorWaveform(set_waveform) => {
                self.r#type = set_waveform.r#type;
                self.periodic_wave = set_waveform.periodic_wave.clone();

                // https://webaudio.github.io/web-audio-api/#waveform-normalization
                // If the internal slot [[normalize]] of this PeriodicWave is true (the default), the waveform defined in
                // the previous section is normalized so that the maximum value is 1.
                self.periodic_wave_scale = 1.;
                if let Some(periodic_wave) = &self.periodic_wave
                    && periodic_wave.normalize
                {
                    let mut maximum_amplitude = 0_f32;
                    const NORMALIZATION_SAMPLE_COUNT: usize = 4096;
                    for sample_index in 0..NORMALIZATION_SAMPLE_COUNT {
                        let phase = 2. * PI * sample_index as f64 / NORMALIZATION_SAMPLE_COUNT as f64;
                        let mut amplitude = 0_f32;
                        for harmonic in 1..periodic_wave.real.len() {
                            amplitude += periodic_wave.real[harmonic] * math::cos(harmonic as f64 * phase) as f32
                                + periodic_wave.imag[harmonic] * math::sin(harmonic as f64 * phase) as f32;
                        }
                        maximum_amplitude = cpp_max(maximum_amplitude, amplitude.abs());
                    }
                    if maximum_amplitude > 0. {
                        self.periodic_wave_scale = 1. / maximum_amplitude;
                    }
                }
            }
            _ => self.source.handle_message(message),
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.frequency);
        callback(&self.detune);
    }

    fn take_ended_notification(&mut self) -> bool {
        self.source.take_ended_notification()
    }
}

// https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
struct AudioBufferSourceRenderNode {
    source: ScheduledSourceRenderNode,
    playback_rate: Arc<RenderAudioParam>,
    detune: Arc<RenderAudioParam>,
    playback_rate_values: Vec<f32>,
    detune_values: Vec<f32>,
    buffer: Option<Arc<AudioBufferContents>>,
    r#loop: bool,
    loop_start: f64,
    loop_end: f64,
    offset: f64,
    duration: Option<f64>,

    // Playback position within the buffer contents, in buffer sample frames.
    playhead: f64,
    playhead_valid: bool,
    played_time: f64,
}

impl AudioBufferSourceRenderNode {
    fn sample_buffer(&self, channel: usize, playhead_frames: f64, loop_start_frame: f64, loop_end_frame: f64) -> f32 {
        let samples = &self.buffer.as_ref().unwrap().channels[channel];
        let index = playhead_frames.floor() as i64;
        let fraction = (playhead_frames - index as f64) as f32;
        let sample_at = |mut sample_index: i64| -> f32 {
            // When looping, interpolation across the loop end wraps around to the loop start.
            if self.r#loop && sample_index >= loop_end_frame as i64 && loop_end_frame > loop_start_frame {
                sample_index -= (loop_end_frame - loop_start_frame) as i64;
            }
            if sample_index < 0 || sample_index >= samples.len() as i64 {
                return 0.;
            }
            samples[sample_index as usize]
        };
        if fraction == 0. {
            return sample_at(index);
        }

        // Past the last frame there is nothing to interpolate toward, so the slope leading into it is continued instead.
        if !self.r#loop && index == samples.len() as i64 - 1 {
            if index == 0 {
                return sample_at(index);
            }
            return sample_at(index) + fraction * (sample_at(index) - sample_at(index - 1));
        }
        sample_at(index) + fraction * (sample_at(index + 1) - sample_at(index))
    }

    // https://webaudio.github.io/web-audio-api/#playback-AudioBufferSourceNode
    unsafe fn process_source(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            self.playback_rate
                .compute(graph, context, &mut self.playback_rate_values);
            self.detune.compute(graph, context, &mut self.detune_values);
            let source_output = BusRef(data.output(0));
            source_output.set_channel_count(self.buffer.as_ref().map_or(1, |buffer| buffer.channels.len()));
            source_output.zero();
            if self.buffer.is_none() || !self.source.has_started() {
                self.source.update_ended_state(context);
                return;
            }

            // NB: The playback rate is k-rate, so a single computed value is used for the entire quantum. The playhead is
            //     tracked in buffer sample frames so that unity-rate playback of a buffer at the context's sample rate
            //     advances by exactly one frame per rendered frame and reproduces the buffer bit-exactly.
            let computed_playback_rate =
                f64::from(self.playback_rate_values[0]) * math::exp2(f64::from(self.detune_values[0]) / 1200.);
            let buffer = self.buffer.as_ref().unwrap();
            let buffer_sample_rate = f64::from(buffer.sample_rate);
            let playhead_increment = computed_playback_rate * buffer_sample_rate / context.sample_rate;
            let buffer_length = buffer.length() as f64;

            // NB: Determine the effective loop region, in buffer sample frames.
            // https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
            let mut actual_loop_start = 0.;
            let mut actual_loop_end = buffer_length;
            if self.r#loop && self.loop_start >= 0. && self.loop_end > 0. && self.loop_start < self.loop_end {
                actual_loop_start = cpp_min(self.loop_start * buffer_sample_rate, buffer_length);
                actual_loop_end = cpp_min(self.loop_end * buffer_sample_rate, buffer_length);
            }
            for frame in 0..context.quantum_size {
                let frame_time = context.frame_time(frame);
                if !self.source.is_active_at(frame_time) {
                    continue;
                }
                if !self.playhead_valid {
                    // NB: Playback begins at the given offset, corrected for the source starting between two sample frames.
                    self.playhead = self.offset * buffer_sample_rate;
                    if self.r#loop {
                        self.playhead = cpp_min(self.playhead, actual_loop_end);
                    } else {
                        self.playhead = cpp_min(self.playhead, buffer_length);
                    }
                    self.playhead +=
                        (frame_time - self.source.start_time.unwrap()) * computed_playback_rate * buffer_sample_rate;
                    self.playhead_valid = true;
                }

                // NB: A half-frame tolerance prevents rounding error in played_time from extending playback by one frame.
                let frame_consumption = computed_playback_rate.abs() / context.sample_rate;
                if self
                    .duration
                    .is_some_and(|duration| self.played_time + frame_consumption / 2. >= duration)
                {
                    self.source.content_ended = true;
                    continue;
                }
                if self.r#loop {
                    let loop_duration = actual_loop_end - actual_loop_start;
                    if loop_duration > 0. {
                        while self.playhead >= actual_loop_end {
                            self.playhead -= loop_duration;
                        }
                        while self.playhead < actual_loop_start && playhead_increment < 0. {
                            self.playhead += loop_duration;
                        }
                    }
                } else if self.playhead >= buffer_length || (playhead_increment < 0. && self.playhead < 0.) {
                    self.source.content_ended = true;
                    continue;
                }
                for channel_index in 0..buffer.channels.len() {
                    source_output.channel_mut(channel_index)[frame] =
                        self.sample_buffer(channel_index, self.playhead, actual_loop_start, actual_loop_end);
                }
                self.playhead += playhead_increment;
                self.played_time += computed_playback_rate.abs() / context.sample_rate;
            }
            self.source.update_ended_state(context);
        }
    }
}

impl NodeProcessor for AudioBufferSourceRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        self.source.process(context);
        unsafe {
            self.process_source(data, graph, context);
        }
    }

    fn handle_message(&mut self, message: &NodeMessage) {
        match message {
            NodeMessage::StartBufferSource(start_buffer_source) => {
                self.source.handle_message(&NodeMessage::StartSource(StartSource {
                    node_id: start_buffer_source.node_id,
                    when: start_buffer_source.when,
                }));
                self.offset = start_buffer_source.offset;
                self.duration = start_buffer_source.duration;
                if start_buffer_source.buffer.is_some() {
                    self.buffer = start_buffer_source.buffer.clone();
                }
            }
            NodeMessage::SetBufferSourceParameters(set_parameters) => {
                self.r#loop = set_parameters.r#loop;
                self.loop_start = set_parameters.loop_start;
                self.loop_end = set_parameters.loop_end;
                if set_parameters.update_buffer {
                    self.buffer = set_parameters.buffer.clone();
                }
            }
            _ => self.source.handle_message(message),
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.playback_rate);
        callback(&self.detune);
    }

    fn take_ended_notification(&mut self) -> bool {
        self.source.take_ended_notification()
    }
}

// https://webaudio.github.io/web-audio-api/#DelayNode
struct DelayRenderNode {
    delay_time: Arc<RenderAudioParam>,
    delay_values: Vec<f32>,
    max_delay_time: f64,
    history: Vec<Vec<f32>>,
    write_index: usize,
}

impl NodeProcessor for DelayRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            self.delay_time.compute(graph, context, &mut self.delay_values);
            let history_length = (self.max_delay_time * context.sample_rate).ceil() as usize + 2;
            if self.history.len() != input.channel_count()
                || (!self.history.is_empty() && self.history[0].len() != history_length)
            {
                self.history.resize_with(input.channel_count(), Vec::new);
                for channel_history in &mut self.history {
                    channel_history.clear();
                    channel_history.resize(history_length, 0.);
                }
                self.write_index = 0;
            }
            let delay_output = BusRef(data.output(0));
            delay_output.set_channel_count(input.channel_count());
            for frame in 0..context.quantum_size {
                let delay_seconds = clamp(f64::from(self.delay_values[frame]), 0., self.max_delay_time);
                let delay_frames = delay_seconds * context.sample_rate;
                for channel_index in 0..input.channel_count() {
                    let channel_history = &mut self.history[channel_index];
                    channel_history[self.write_index] = input.channel(channel_index)[frame];

                    // Read with linear interpolation to support fractional delays.
                    let mut read_position = self.write_index as f64 - delay_frames;
                    while read_position < 0. {
                        read_position += history_length as f64;
                    }
                    let read_index = read_position as usize;
                    let fraction = (read_position - read_index as f64) as f32;
                    let sample_a = channel_history[read_index % history_length];
                    let sample_b = channel_history[(read_index + 1) % history_length];
                    delay_output.channel_mut(channel_index)[frame] = sample_a + fraction * (sample_b - sample_a);
                }
                self.write_index = (self.write_index + 1) % history_length;
            }
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.delay_time);
    }
}

// https://webaudio.github.io/web-audio-api/#StereoPannerNode
struct StereoPannerRenderNode {
    pan: Arc<RenderAudioParam>,
    pan_values: Vec<f32>,
}

impl NodeProcessor for StereoPannerRenderNode {
    // https://webaudio.github.io/web-audio-api/#stereopanner-algorithm
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            self.pan.compute(graph, context, &mut self.pan_values);
            let panner_output = BusRef(data.output(0));
            panner_output.set_channel_count(2);
            let output_left = panner_output.channel_mut(0);
            let output_right = panner_output.channel_mut(1);
            let input_left = input.channel(0);
            let input_right = if input.channel_count() > 1 {
                input.channel(1)
            } else {
                input.channel(0)
            };
            for frame in 0..context.quantum_size {
                // Let pan be the computedValue of the pan AudioParam of this StereoPannerNode.
                // Clamp pan to [-1, 1].
                let pan = clamp(self.pan_values[frame], -1., 1.);

                // Calculate x by normalizing pan value to [0, 1].
                let x;
                if input.channel_count() == 1 {
                    x = f64::from(pan + 1.) / 2.;
                } else {
                    x = if pan <= 0. { f64::from(pan) + 1. } else { f64::from(pan) };
                }

                // Left and right gain values are calculated as: gainL = cos(x * π / 2); gainR = sin(x * π / 2);
                let PanGains {
                    left: gain_left,
                    right: gain_right,
                } = equal_power_pan_gains(x);

                // For mono input, the stereo output is calculated as: outputL = input * gainL; outputR = input * gainR;
                // NB: For stereo input, the attenuated channel is mixed into the opposite output channel instead.
                if input.channel_count() == 1 {
                    output_left[frame] = input_left[frame] * gain_left;
                    output_right[frame] = input_left[frame] * gain_right;
                } else if pan <= 0. {
                    output_left[frame] = input_left[frame] + input_right[frame] * gain_left;
                    output_right[frame] = input_right[frame] * gain_right;
                } else {
                    output_left[frame] = input_left[frame] * gain_left;
                    output_right[frame] = input_right[frame] + input_left[frame] * gain_right;
                }
            }
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.pan);
    }
}

// https://webaudio.github.io/web-audio-api/#ChannelSplitterNode
struct ChannelSplitterRenderNode;

impl NodeProcessor for ChannelSplitterRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            for output_index in 0..data.output_count() {
                let output = BusRef(data.output(output_index));
                let output_samples = output.channel_mut(0);
                if output_index < input.channel_count() {
                    let input_samples = input.channel(output_index);
                    for frame in 0..context.quantum_size {
                        output_samples[frame] = input_samples[frame];
                    }
                } else {
                    output_samples.fill(0.);
                }
            }
        }
    }
}

// https://webaudio.github.io/web-audio-api/#ChannelMergerNode
struct ChannelMergerRenderNode;

impl NodeProcessor for ChannelMergerRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let merger_output = BusRef(data.output(0));
            merger_output.set_channel_count(data.input_count());
            for input_index in 0..data.input_count() {
                let input = BusRef(data.pull_input(graph, context, input_index).cast_mut());
                let input_samples = input.channel(0);
                let output_samples = merger_output.channel_mut(input_index);
                for frame in 0..context.quantum_size {
                    output_samples[frame] = input_samples[frame];
                }
            }
        }
    }
}

#[derive(Default)]
struct BiquadFilterState {
    x1: f64,
    x2: f64,
    y1: f64,
    y2: f64,
}

// https://webaudio.github.io/web-audio-api/#BiquadFilterNode
struct BiquadFilterRenderNode {
    frequency: Arc<RenderAudioParam>,
    detune: Arc<RenderAudioParam>,
    q: Arc<RenderAudioParam>,
    gain: Arc<RenderAudioParam>,
    frequency_values: Vec<f32>,
    detune_values: Vec<f32>,
    q_values: Vec<f32>,
    gain_values: Vec<f32>,
    r#type: BiquadFilterType,
    filter_states: Vec<BiquadFilterState>,
}

impl NodeProcessor for BiquadFilterRenderNode {
    fn handle_message(&mut self, message: &NodeMessage) {
        if let NodeMessage::SetBiquadFilterType(set_type) = message {
            self.r#type = set_type.r#type;
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.frequency);
        callback(&self.detune);
        callback(&self.q);
        callback(&self.gain);
    }

    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            self.frequency.compute(graph, context, &mut self.frequency_values);
            self.detune.compute(graph, context, &mut self.detune_values);
            self.q.compute(graph, context, &mut self.q_values);
            self.gain.compute(graph, context, &mut self.gain_values);
            if self.filter_states.len() != input.channel_count() {
                self.filter_states
                    .resize_with(input.channel_count(), BiquadFilterState::default);
            }
            let nyquist_frequency = context.sample_rate / 2.;
            let compute_coefficients_for_frame = |frame: usize| {
                let computed_frequency =
                    f64::from(self.frequency_values[frame]) * math::exp2(f64::from(self.detune_values[frame]) / 1200.);
                let normalized_frequency = clamp(computed_frequency / nyquist_frequency, 0., 1.);
                compute_biquad_coefficients(
                    self.r#type,
                    normalized_frequency,
                    f64::from(self.q_values[frame]),
                    f64::from(self.gain_values[frame]),
                )
            };
            let parameters_changed = |frame: usize| {
                self.frequency_values[frame] != self.frequency_values[frame - 1]
                    || self.detune_values[frame] != self.detune_values[frame - 1]
                    || self.q_values[frame] != self.q_values[frame - 1]
                    || self.gain_values[frame] != self.gain_values[frame - 1]
            };
            let filter_output = BusRef(data.output(0));
            filter_output.set_channel_count(input.channel_count());

            // AD-HOC: The coefficients are recomputed whenever the computed parameter values change, so a-rate automation is
            //         applied with per-frame resolution while k-rate parameters use a single set of coefficients for the
            //         entire quantum.
            let mut coefficients = compute_coefficients_for_frame(0);
            for frame in 0..context.quantum_size {
                if frame > 0 && parameters_changed(frame) {
                    coefficients = compute_coefficients_for_frame(frame);
                }
                for channel_index in 0..input.channel_count() {
                    let state = &mut self.filter_states[channel_index];
                    let x = f64::from(input.channel(channel_index)[frame]);
                    let y = coefficients.b0 * x + coefficients.b1 * state.x1 + coefficients.b2 * state.x2
                        - coefficients.a1 * state.y1
                        - coefficients.a2 * state.y2;
                    state.x2 = state.x1;
                    state.x1 = x;
                    state.y2 = state.y1;
                    state.y1 = y;
                    filter_output.channel_mut(channel_index)[frame] = y as f32;
                }
            }
        }
    }
}

// The previous inputs and outputs of the filter for one channel, as ring buffers indexed by the frame number.
#[derive(Default)]
struct IIRFilterState {
    inputs: [f64; 32],
    outputs: [f64; 32],
}

// https://webaudio.github.io/web-audio-api/#IIRFilterNode
struct IIRFilterRenderNode {
    feedforward: Vec<f64>,
    feedback: Vec<f64>,
    filter_states: Vec<IIRFilterState>,
    position: usize,
}

impl NodeProcessor for IIRFilterRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());

            // The number of channels of the output always equals the number of channels of the input.
            let filter_output = BusRef(data.output(0));
            filter_output.set_channel_count(input.channel_count());
            if self.filter_states.len() != input.channel_count() {
                self.filter_states
                    .resize_with(input.channel_count(), IIRFilterState::default);
            }

            // https://webaudio.github.io/web-audio-api/#IIRFilterNode-filter-definition
            // sum_{k=0}^{N} a_k y(n-k) = sum_{k=0}^{M} b_k x(n-k), where a_0 is 1 as the coefficients are normalized.
            const HISTORY_MASK: usize = 31;
            for channel_index in 0..input.channel_count() {
                let state = &mut self.filter_states[channel_index];
                let input_samples = input.channel(channel_index);
                let output_samples = filter_output.channel_mut(channel_index);
                let mut position = self.position;
                for frame in 0..context.quantum_size {
                    let x = f64::from(input_samples[frame]);
                    let mut y = self.feedforward[0] * x;
                    for k in 1..self.feedforward.len() {
                        y += self.feedforward[k] * state.inputs[position.wrapping_sub(k) & HISTORY_MASK];
                    }
                    for k in 1..self.feedback.len() {
                        y -= self.feedback[k] * state.outputs[position.wrapping_sub(k) & HISTORY_MASK];
                    }

                    // NB: A decaying output would otherwise settle on subnormal numbers, which are slow to compute with.
                    if y.abs() < f64::MIN_POSITIVE {
                        y = 0.;
                    }
                    state.inputs[position & HISTORY_MASK] = x;
                    state.outputs[position & HISTORY_MASK] = y;
                    output_samples[frame] = y as f32;
                    position = position.wrapping_add(1);
                }
            }
            self.position = self.position.wrapping_add(context.quantum_size);
        }
    }
}

// A lowpass FIR filter that passes the lower half of its band, which is the band of the signal at half its sample
// rate. Up-sampling by 2 filters the input with zeros between its samples, and down-sampling by 2 keeps every other
// sample of the filtered signal.
const HALF_BAND_FILTER_LENGTH: usize = 31;

// NB: The up-sampling and down-sampling filters are not specified. These use a windowed sinc filter whose cutoff is
//     half its band.
fn half_band_filter_coefficients() -> &'static [f32; HALF_BAND_FILTER_LENGTH] {
    static COEFFICIENTS: OnceLock<[f32; HALF_BAND_FILTER_LENGTH]> = OnceLock::new();
    COEFFICIENTS.get_or_init(|| {
        const LENGTH: usize = HALF_BAND_FILTER_LENGTH;
        let mut taps = [0.; LENGTH];
        let mut sum = 0.;
        for index in 0..LENGTH {
            let offset = index as f64 - (LENGTH - 1) as f64 / 2.;
            let sinc = if offset == 0. {
                1.
            } else {
                math::sin(PI * offset / 2.) / (PI * offset / 2.)
            };
            let phase = 2. * PI * index as f64 / (LENGTH - 1) as f64;
            let blackman_window = 0.42 - 0.5 * math::cos(phase) + 0.08 * math::cos(2. * phase);
            taps[index] = sinc * blackman_window;
            sum += taps[index];
        }
        let mut result = [0.; LENGTH];
        for index in 0..LENGTH {
            result[index] = (taps[index] / sum) as f32;
        }
        result
    })
}

struct HalfBandFilter {
    history: [f32; HALF_BAND_FILTER_LENGTH * 2],
    position: usize,
}

impl Default for HalfBandFilter {
    fn default() -> Self {
        Self {
            history: [0.; HALF_BAND_FILTER_LENGTH * 2],
            position: 0,
        }
    }
}

impl HalfBandFilter {
    fn process(&mut self, sample: f32) -> f32 {
        let coefficients = half_band_filter_coefficients();

        // NB: Every sample is stored twice, so that the last samples are contiguous wherever the ring buffer starts.
        self.history[self.position] = sample;
        self.history[self.position + HALF_BAND_FILTER_LENGTH] = sample;
        let newest_sample = self.position + HALF_BAND_FILTER_LENGTH;
        let mut result = 0.;
        for index in 0..HALF_BAND_FILTER_LENGTH {
            result += coefficients[index] * self.history[newest_sample - index];
        }
        self.position = if self.position + 1 == HALF_BAND_FILTER_LENGTH {
            0
        } else {
            self.position + 1
        };
        result
    }
}

#[derive(Default)]
struct WaveShaperChannelState {
    up_sampling_filters: [HalfBandFilter; 2],
    down_sampling_filters: [HalfBandFilter; 2],
}

// https://webaudio.github.io/web-audio-api/#WaveShaperNode
struct WaveShaperRenderNode {
    curve: Option<Arc<WaveShaperCurve>>,
    oversample: OverSampleType,
    channel_states: Vec<WaveShaperChannelState>,
    two_times_samples: Vec<f32>,
    four_times_samples: Vec<f32>,
}

impl WaveShaperRenderNode {
    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
    fn shape(&self, sample: f32) -> f32 {
        // 1. Let x be the input sample, y be the corresponding output of the node, c_k be the k'th element of the curve,
        //    and N be the length of the curve.
        let curve = &self.curve.as_ref().unwrap().values;
        let curve_length = curve.len();

        // 2. Let v = (N-1)/2 (x + 1), k = floor(v), f = v - k
        let v = (curve_length - 1) as f64 / 2. * (f64::from(sample) + 1.);

        // 3. Then y = c_0 if v < 0, c_{N-1} if v >= N - 1, and (1-f) c_k + f c_{k+1} otherwise.
        // NB: A NaN sample maps to c_0 as well.
        if !(v >= 0.) {
            return curve[0];
        }
        if v >= (curve_length - 1) as f64 {
            return *curve.last().unwrap();
        }
        let k = v as usize;
        let f = v - k as f64;
        ((1. - f) * f64::from(curve[k]) + f * f64::from(curve[k + 1])) as f32
    }

    fn up_sample(filter: &mut HalfBandFilter, input: &[f32], output: &mut [f32]) {
        assert_eq!(output.len(), input.len() * 2);

        // NB: The zeros between the samples halve the gain of the filter, which the factor of 2 restores.
        for index in 0..input.len() {
            output[2 * index] = 2. * filter.process(input[index]);
            output[2 * index + 1] = 2. * filter.process(0.);
        }
    }

    fn down_sample(filter: &mut HalfBandFilter, input: &[f32], output: &mut [f32]) {
        assert_eq!(input.len(), output.len() * 2);
        for index in 0..output.len() {
            output[index] = filter.process(input[2 * index]);
            let _ = filter.process(input[2 * index + 1]);
        }
    }
}

impl NodeProcessor for WaveShaperRenderNode {
    fn handle_message(&mut self, message: &NodeMessage) {
        if let NodeMessage::SetWaveShaperParameters(parameters) = message {
            self.curve = parameters.curve.clone();
            self.oversample = parameters.oversample;
        }
    }

    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());

            // The number of channels of the output always equals the number of channels of the input.
            let shaper_output = BusRef(data.output(0));
            shaper_output.set_channel_count(input.channel_count());
            if self.channel_states.len() != input.channel_count() {
                self.channel_states
                    .resize_with(input.channel_count(), WaveShaperChannelState::default);
            }
            for channel_index in 0..input.channel_count() {
                let input_samples = &input.channel(channel_index)[..context.quantum_size];
                let output_samples = &mut shaper_output.channel_mut(channel_index)[..context.quantum_size];

                // Initially the curve attribute is null, which means that the WaveShaperNode will pass its input to its output
                // without modification.
                if self.curve.is_none() {
                    output_samples.copy_from_slice(input_samples);
                    continue;
                }
                match self.oversample {
                    OverSampleType::None => {
                        // The default value is "none", meaning the curve will be applied directly to the input samples.
                        for frame in 0..context.quantum_size {
                            output_samples[frame] = self.shape(input_samples[frame]);
                        }
                    }
                    OverSampleType::_2x => {
                        // 1. Up-sample the input samples to 2x or 4x the sample-rate of the AudioContext. Thus for each render
                        //    quantum, generate twice (for 2x) or four times (for 4x) samples.
                        Self::up_sample(
                            &mut self.channel_states[channel_index].up_sampling_filters[0],
                            input_samples,
                            &mut self.two_times_samples,
                        );

                        // 2. Apply the shaping curve.
                        for index in 0..self.two_times_samples.len() {
                            self.two_times_samples[index] = self.shape(self.two_times_samples[index]);
                        }

                        // 3. Down-sample the result back to the sample-rate of the AudioContext. Thus taking the previously
                        //    processed samples processed samples, generating a single render quantum worth of samples as the
                        //    final result.
                        Self::down_sample(
                            &mut self.channel_states[channel_index].down_sampling_filters[0],
                            &self.two_times_samples,
                            output_samples,
                        );
                    }
                    OverSampleType::_4x => {
                        // 1. Up-sample the input samples to 2x or 4x the sample-rate of the AudioContext. Thus for each render
                        //    quantum, generate twice (for 2x) or four times (for 4x) samples.
                        Self::up_sample(
                            &mut self.channel_states[channel_index].up_sampling_filters[0],
                            input_samples,
                            &mut self.two_times_samples,
                        );
                        Self::up_sample(
                            &mut self.channel_states[channel_index].up_sampling_filters[1],
                            &self.two_times_samples,
                            &mut self.four_times_samples,
                        );

                        // 2. Apply the shaping curve.
                        for index in 0..self.four_times_samples.len() {
                            self.four_times_samples[index] = self.shape(self.four_times_samples[index]);
                        }

                        // 3. Down-sample the result back to the sample-rate of the AudioContext. Thus taking the previously
                        //    processed samples processed samples, generating a single render quantum worth of samples as the
                        //    final result.
                        Self::down_sample(
                            &mut self.channel_states[channel_index].down_sampling_filters[1],
                            &self.four_times_samples,
                            &mut self.two_times_samples,
                        );
                        Self::down_sample(
                            &mut self.channel_states[channel_index].down_sampling_filters[0],
                            &self.two_times_samples,
                            output_samples,
                        );
                    }
                }
            }
        }
    }
}

#[derive(Clone, Copy, Default)]
struct DoubleVector3([f64; 3]);

impl DoubleVector3 {
    fn dot(self, other: Self) -> f64 {
        let mut result = 0.;
        for i in 0..3 {
            result += self.0[i] * other.0[i];
        }
        result
    }

    fn length(self) -> f64 {
        self.dot(self).sqrt()
    }

    fn normalized(self) -> Self {
        let inverse_length = 1. / self.length();
        self * inverse_length
    }

    fn cross(self, other: Self) -> Self {
        let [x, y, z] = self.0;
        let [ox, oy, oz] = other.0;
        Self([y * oz - z * oy, z * ox - x * oz, x * oy - y * ox])
    }
}

impl std::ops::Sub for DoubleVector3 {
    type Output = Self;

    fn sub(self, other: Self) -> Self {
        Self(std::array::from_fn(|i| self.0[i] - other.0[i]))
    }
}

impl std::ops::Mul<f64> for DoubleVector3 {
    type Output = Self;

    fn mul(self, scalar: f64) -> Self {
        Self(std::array::from_fn(|i| self.0[i] * scalar))
    }
}

// Spatialization requires normalizing a zero vector to a zero vector.
fn safe_normalized(vector: DoubleVector3) -> DoubleVector3 {
    if vector.length() == 0. {
        return DoubleVector3::default();
    }
    vector.normalized()
}

// https://webaudio.github.io/web-audio-api/#azimuth-elevation
fn compute_azimuth(
    source_position: DoubleVector3,
    listener_position: DoubleVector3,
    listener_forward: DoubleVector3,
    listener_up: DoubleVector3,
) -> f64 {
    // Calculate the source-listener vector.
    let mut source_listener = source_position - listener_position;

    // Handle degenerate case if source and listener are at the same point.
    if source_listener.length() == 0. {
        return 0.;
    }
    source_listener = source_listener.normalized();

    // Align axes.
    let listener_right = listener_forward.cross(listener_up);

    // Handle the case where listener's 'up' and 'forward' vectors are linearly dependent, in which case 'right'
    // cannot be determined.
    if listener_right.length() == 0. {
        return 0.;
    }

    // Determine a unit vector orthogonal to listener's right, forward.
    let listener_right_norm = listener_right.normalized();
    let listener_forward_norm = safe_normalized(listener_forward);
    let up = listener_right_norm.cross(listener_forward_norm);
    let up_projection = source_listener.dot(up);
    let mut projected_source = source_listener - up * up_projection;
    projected_source = safe_normalized(projected_source);
    let mut azimuth = 180. * math::acos(clamp(projected_source.dot(listener_right_norm), -1., 1.)) / PI;

    // Source in front or behind the listener.
    let front_back = projected_source.dot(listener_forward_norm);
    if front_back < 0. {
        azimuth = 360. - azimuth;
    }

    // Make azimuth relative to "forward" and not "right" listener vector.
    // AD-HOC: This is equivalent to the spec's mapping, but lands directly in the range [-180, 180].
    if azimuth <= 90. {
        azimuth = 90. - azimuth;
    } else {
        azimuth = 450. - azimuth;
    }
    if azimuth > 180. {
        azimuth -= 360.;
    }
    azimuth
}

// https://webaudio.github.io/web-audio-api/#enumdef-distancemodeltype
fn compute_distance_gain(distance: f64, config: &SetPannerParameters) -> f64 {
    let ref_distance = config.ref_distance;
    let max_distance = config.max_distance;
    let rolloff_factor = config.rolloff_factor;

    match config.distance_model {
        DistanceModelType::Linear => {
            let lower_distance = cpp_min(ref_distance, max_distance);
            let upper_distance = cpp_max(ref_distance, max_distance);
            let rolloff_factor = clamp(rolloff_factor, 0., 1.);
            if lower_distance == upper_distance {
                return 1. - rolloff_factor;
            }
            let distance = clamp(distance, lower_distance, upper_distance);
            1. - rolloff_factor * (distance - lower_distance) / (upper_distance - lower_distance)
        }
        DistanceModelType::Inverse => {
            if ref_distance == 0. {
                return 0.;
            }
            let distance = cpp_max(distance, ref_distance);
            ref_distance / (ref_distance + rolloff_factor * (distance - ref_distance))
        }
        DistanceModelType::Exponential => {
            if ref_distance == 0. {
                return 0.;
            }
            let distance = cpp_max(distance, ref_distance);
            math::pow(distance / ref_distance, -rolloff_factor)
        }
    }
}

// https://webaudio.github.io/web-audio-api/#Spatialization-sound-cones
fn compute_cone_gain(
    source_position: DoubleVector3,
    source_orientation: DoubleVector3,
    listener_position: DoubleVector3,
    config: &SetPannerParameters,
) -> f64 {
    if source_orientation.length() == 0. || (config.cone_inner_angle == 360. && config.cone_outer_angle == 360.) {
        return 1.;
    }

    // Normalized source-listener vector.
    let source_to_listener = safe_normalized(listener_position - source_position);
    let normalized_source_orientation = source_orientation.normalized();

    // Angle between the source orientation vector and the source-listener vector.
    let angle = 180. * math::acos(clamp(source_to_listener.dot(normalized_source_orientation), -1., 1.)) / PI;
    let absolute_angle = angle.abs();

    // Divide by 2 here since API is entire angle (not half-angle).
    let absolute_inner_angle = config.cone_inner_angle.abs() / 2.;
    let absolute_outer_angle = config.cone_outer_angle.abs() / 2.;
    if absolute_angle <= absolute_inner_angle {
        // No attenuation.
        return 1.;
    }
    if absolute_angle >= absolute_outer_angle {
        // Max attenuation.
        return config.cone_outer_gain;
    }

    // Between inner and outer cones: inner -> outer, x goes from 0 -> 1.
    let x = (absolute_angle - absolute_inner_angle) / (absolute_outer_angle - absolute_inner_angle);
    1. + (config.cone_outer_gain - 1.) * x
}

// Which impulse response channel is convolved with which input channel, and where the result is summed into.
// https://webaudio.github.io/web-audio-api/#Convolution-channel-configurations
#[derive(Clone, Copy, Default)]
struct Route {
    input_channel: usize,
    kernel_channel: usize,
    output_channel: usize,
}

// Convolves the input with an impulse response. The head of the impulse response is convolved directly, and the rest
// through uniformly partitioned overlap-save convolution: every quantum, the window spanning the previous and the
// current quantum is transformed and pushed into a frequency delay line, whose spectra are then multiplied with the
// impulse response partitions and summed. That keeps the cost per quantum constant and adds no latency, at the price of
// transforming a window that is only half new each time.
// https://webaudio.github.io/web-audio-api/#ConvolverNode
struct ConvolverRenderNode {
    fft: FFT,
    kernel: Option<Arc<ConvolverKernel>>,
    delay_line: Option<Arc<super::convolver_kernel::ConvolverDelayLine>>,

    // The slot of the delay line that holds the spectrum of the current quantum's window.
    delay_line_index: usize,

    // The previous and the current quantum's samples for each input channel, which the direct convolution reaches back
    // into and which the transform consumes as a whole.
    window: [Vec<f32>; 2],
    input_channel_count: usize,
    transform_real: Vec<f32>,
    transform_imag: Vec<f32>,

    // The partition sums are accumulated in double precision: a long impulse response adds up hundreds of partitions
    // per bin, and single precision loses too much of the quiet parts of the result.
    accumulator_real: Vec<f64>,
    accumulator_imag: Vec<f64>,

    // How many quanta in a row have brought only silence, which tells how much of the tail is left to play.
    silent_input_quanta: usize,
}

// Folds a second channel's history into the first the way down-mixing folds the channels themselves.
fn average_history(into: &mut [f32], other: &[f32]) {
    for index in 0..into.len() {
        into[index] = 0.5 * (into[index] + other[index]);
    }
}

impl NodeProcessor for ConvolverRenderNode {
    fn handle_message(&mut self, message: &NodeMessage) {
        if let NodeMessage::SetConvolverKernel(set_kernel) = message {
            self.kernel = set_kernel.kernel.clone();
            self.delay_line = set_kernel.delay_line.clone();
            self.delay_line_index = 0;
        }
    }

    // The window and the delay line hold the last partition_count + 2 quanta of input, so the tail has played out once
    // that many quanta of silence have gone through.
    fn has_tail_remaining(&self) -> bool {
        let Some(kernel) = &self.kernel else {
            return false;
        };
        self.silent_input_quanta < kernel.partition_count() + 2
    }

    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            let input_is_silent =
                (0..input.channel_count()).all(|channel| input.channel(channel).iter().all(|sample| *sample == 0.));
            self.silent_input_quanta = if input_is_silent {
                self.silent_input_quanta.saturating_add(1)
            } else {
                0
            };
            let convolver_output = BusRef(data.output(0));
            let input_channel_count = input
                .channel_count()
                .min(super::convolver_kernel::ConvolverDelayLine::MAX_CHANNEL_COUNT);

            // Mix the history the same way the input itself is mixed, so that the tail of the impulse response keeps playing
            // across a change in the number of input channels.
            // https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
            if self.input_channel_count == 1 && input_channel_count == 2 {
                let [left, right] = &mut self.window;
                right.copy_from_slice(left);
                if let Some(delay_line) = &self.delay_line {
                    delay_line.real_shared(1).copy_from_slice(delay_line.real_shared(0));
                    delay_line.imag_shared(1).copy_from_slice(delay_line.imag_shared(0));
                }
            } else if self.input_channel_count == 2 && input_channel_count == 1 {
                let [left, right] = &mut self.window;
                average_history(left, right);
                if let Some(delay_line) = &self.delay_line {
                    average_history(delay_line.real_shared(0), delay_line.real_shared(1));
                    average_history(delay_line.imag_shared(0), delay_line.imag_shared(1));
                }
            }
            self.input_channel_count = input_channel_count;
            let partition_size = context.quantum_size;
            for channel in 0..input_channel_count {
                // Slide this quantum's samples into the window, dropping the oldest half.
                let window = &mut self.window[channel];
                let input_samples = input.channel(channel);
                for frame in 0..partition_size {
                    window[frame] = window[partition_size + frame];
                    window[partition_size + frame] = input_samples[frame];
                }
            }
            let Some(kernel) = &self.kernel else {
                convolver_output.set_channel_count(1);
                convolver_output.zero();
                return;
            };
            let transform_size = self.fft.size();
            let partition_count = kernel.partition_count();
            let delay_line = self.delay_line.as_ref().unwrap();
            let delay_line_length = delay_line.length();
            let bin_count = kernel.bin_count();
            if partition_count > 0 {
                for channel in 0..input_channel_count {
                    let window = &self.window[channel];
                    for frame in 0..transform_size {
                        self.transform_real[frame] = window[frame];
                        self.transform_imag[frame] = 0.;
                    }
                    self.fft.transform(
                        &mut self.transform_real,
                        &mut self.transform_imag,
                        FFTDirection::Forward,
                    );
                    let offset = self.delay_line_index * bin_count;
                    let delay_line_real = &mut delay_line.real_shared(channel)[offset..];
                    let delay_line_imag = &mut delay_line.imag_shared(channel)[offset..];
                    for bin in 0..bin_count {
                        delay_line_real[bin] = self.transform_real[bin];
                        delay_line_imag[bin] = self.transform_imag[bin];
                    }
                }
            }

            // https://webaudio.github.io/web-audio-api/#Convolution-channel-configurations
            let right_channel = if input_channel_count > 1 { 1 } else { 0 };
            let mut routes = [Route::default(); 4];
            let mut route_count = 0;
            match kernel.channel_count() {
                1 => {
                    routes[route_count] = Route {
                        input_channel: 0,
                        kernel_channel: 0,
                        output_channel: 0,
                    };
                    route_count += 1;
                    if input_channel_count > 1 {
                        routes[route_count] = Route {
                            input_channel: 1,
                            kernel_channel: 0,
                            output_channel: 1,
                        };
                        route_count += 1;
                    }
                }
                2 => {
                    routes[route_count] = Route {
                        input_channel: 0,
                        kernel_channel: 0,
                        output_channel: 0,
                    };
                    route_count += 1;
                    routes[route_count] = Route {
                        input_channel: right_channel,
                        kernel_channel: 1,
                        output_channel: 1,
                    };
                    route_count += 1;
                }
                4 => {
                    routes[route_count] = Route {
                        input_channel: 0,
                        kernel_channel: 0,
                        output_channel: 0,
                    };
                    route_count += 1;
                    routes[route_count] = Route {
                        input_channel: 0,
                        kernel_channel: 1,
                        output_channel: 1,
                    };
                    route_count += 1;
                    routes[route_count] = Route {
                        input_channel: right_channel,
                        kernel_channel: 2,
                        output_channel: 0,
                    };
                    route_count += 1;
                    routes[route_count] = Route {
                        input_channel: right_channel,
                        kernel_channel: 3,
                        output_channel: 1,
                    };
                    route_count += 1;
                }
                _ => unreachable!(),
            }
            let output_channel_count = if kernel.channel_count() == 1 && input_channel_count == 1 {
                1
            } else {
                2
            };
            convolver_output.set_channel_count(output_channel_count);
            for output_channel in 0..output_channel_count {
                let output_samples = convolver_output.channel_mut(output_channel);
                for frame in 0..partition_size {
                    output_samples[frame] = 0.;
                }
                for bin in 0..bin_count {
                    self.accumulator_real[bin] = 0.;
                    self.accumulator_imag[bin] = 0.;
                }
                for route_index in 0..route_count {
                    let route = routes[route_index];
                    if route.output_channel != output_channel {
                        continue;
                    }
                    let window = &self.window[route.input_channel];
                    let head = kernel.head(route.kernel_channel);
                    for frame in 0..partition_size {
                        let mut sample = 0_f32;
                        for tap in 0..partition_size {
                            sample += head[tap] * window[partition_size + frame - tap];
                        }
                        output_samples[frame] += sample;
                    }
                    let delay_line_real = delay_line.real_shared(route.input_channel);
                    let delay_line_imag = delay_line.imag_shared(route.input_channel);
                    for partition in 0..partition_count {
                        // Partition N of the impulse response pairs up with the window from N + 1 quanta ago.
                        let delay_line_slot =
                            (self.delay_line_index + delay_line_length - partition - 1) % delay_line_length;
                        let input_real = &delay_line_real[delay_line_slot * bin_count..];
                        let input_imag = &delay_line_imag[delay_line_slot * bin_count..];
                        let kernel_real = kernel.partition_real(route.kernel_channel, partition);
                        let kernel_imag = kernel.partition_imag(route.kernel_channel, partition);
                        for bin in 0..bin_count {
                            self.accumulator_real[bin] +=
                                f64::from(input_real[bin] * kernel_real[bin] - input_imag[bin] * kernel_imag[bin]);
                            self.accumulator_imag[bin] +=
                                f64::from(input_real[bin] * kernel_imag[bin] + input_imag[bin] * kernel_real[bin]);
                        }
                    }
                }
                if partition_count == 0 {
                    continue;
                }

                // Restore the bins above Nyquist that the kernel and the delay line leave out.
                for bin in 0..bin_count {
                    self.transform_real[bin] = self.accumulator_real[bin] as f32;
                    self.transform_imag[bin] = self.accumulator_imag[bin] as f32;
                }
                for bin in bin_count..transform_size {
                    self.transform_real[bin] = self.accumulator_real[transform_size - bin] as f32;
                    self.transform_imag[bin] = -self.accumulator_imag[transform_size - bin] as f32;
                }
                self.fft.transform(
                    &mut self.transform_real,
                    &mut self.transform_imag,
                    FFTDirection::Inverse,
                );

                // The first half of the result is where the cyclic convolution wrapped around, and is discarded.
                for frame in 0..partition_size {
                    output_samples[frame] += self.transform_real[partition_size + frame];
                }
            }
            if partition_count > 0 {
                self.delay_line_index = (self.delay_line_index + 1) % delay_line_length;
            }
        }
    }
}

struct PannerParams {
    position_x: Arc<RenderAudioParam>,
    position_y: Arc<RenderAudioParam>,
    position_z: Arc<RenderAudioParam>,
    orientation_x: Arc<RenderAudioParam>,
    orientation_y: Arc<RenderAudioParam>,
    orientation_z: Arc<RenderAudioParam>,
}

struct ListenerParams {
    position_x: Arc<RenderAudioParam>,
    position_y: Arc<RenderAudioParam>,
    position_z: Arc<RenderAudioParam>,
    forward_x: Arc<RenderAudioParam>,
    forward_y: Arc<RenderAudioParam>,
    forward_z: Arc<RenderAudioParam>,
    up_x: Arc<RenderAudioParam>,
    up_y: Arc<RenderAudioParam>,
    up_z: Arc<RenderAudioParam>,
}

// https://webaudio.github.io/web-audio-api/#PannerNode
struct PannerRenderNode {
    params: PannerParams,
    listener_params: ListenerParams,
    config: SetPannerParameters,

    // One buffer of computed values per panner and listener param, in the order of the Params and ListenerParams
    // struct members.
    param_values: Vec<Vec<f32>>,
}

impl NodeProcessor for PannerRenderNode {
    fn handle_message(&mut self, message: &NodeMessage) {
        if let NodeMessage::SetPannerParameters(parameters) = message {
            self.config = *parameters;
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        callback(&self.params.position_x);
        callback(&self.params.position_y);
        callback(&self.params.position_z);
        callback(&self.params.orientation_x);
        callback(&self.params.orientation_y);
        callback(&self.params.orientation_z);
        callback(&self.listener_params.position_x);
        callback(&self.listener_params.position_y);
        callback(&self.listener_params.position_z);
        callback(&self.listener_params.forward_x);
        callback(&self.listener_params.forward_y);
        callback(&self.listener_params.forward_z);
        callback(&self.listener_params.up_x);
        callback(&self.listener_params.up_y);
        callback(&self.listener_params.up_z);
    }

    // https://webaudio.github.io/web-audio-api/#Spatialization
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            let panner_output = BusRef(data.output(0));
            panner_output.set_channel_count(2);
            if input.is_silent() {
                panner_output.zero();
                return;
            }
            let params = [
                &self.params.position_x,
                &self.params.position_y,
                &self.params.position_z,
                &self.params.orientation_x,
                &self.params.orientation_y,
                &self.params.orientation_z,
                &self.listener_params.position_x,
                &self.listener_params.position_y,
                &self.listener_params.position_z,
                &self.listener_params.forward_x,
                &self.listener_params.forward_y,
                &self.listener_params.forward_z,
                &self.listener_params.up_x,
                &self.listener_params.up_y,
                &self.listener_params.up_z,
            ];
            for param_index in 0..params.len() {
                params[param_index].compute(graph, context, &mut self.param_values[param_index]);
            }
            let output_left = panner_output.channel_mut(0);
            let output_right = panner_output.channel_mut(1);
            let input_left = input.channel(0);
            let input_right = if input.channel_count() > 1 {
                input.channel(1)
            } else {
                input.channel(0)
            };
            for frame in 0..context.quantum_size {
                let source_position = DoubleVector3([
                    f64::from(self.param_values[0][frame]),
                    f64::from(self.param_values[1][frame]),
                    f64::from(self.param_values[2][frame]),
                ]);
                let source_orientation = DoubleVector3([
                    f64::from(self.param_values[3][frame]),
                    f64::from(self.param_values[4][frame]),
                    f64::from(self.param_values[5][frame]),
                ]);
                let listener_position = DoubleVector3([
                    f64::from(self.param_values[6][frame]),
                    f64::from(self.param_values[7][frame]),
                    f64::from(self.param_values[8][frame]),
                ]);
                let listener_forward = DoubleVector3([
                    f64::from(self.param_values[9][frame]),
                    f64::from(self.param_values[10][frame]),
                    f64::from(self.param_values[11][frame]),
                ]);
                let listener_up = DoubleVector3([
                    f64::from(self.param_values[12][frame]),
                    f64::from(self.param_values[13][frame]),
                    f64::from(self.param_values[14][frame]),
                ]);
                let mut azimuth = compute_azimuth(source_position, listener_position, listener_forward, listener_up);
                let distance = (source_position - listener_position).length();
                let gain = (compute_distance_gain(distance, &self.config)
                    * compute_cone_gain(source_position, source_orientation, listener_position, &self.config))
                    as f32;

                // https://webaudio.github.io/web-audio-api/#Spatialization-panning-algorithm
                // The azimuth value is first contained to be within the range [-90, 90].
                // NB: compute_azimuth() already produces a value in [-180, 180], so only the wrapping step remains.
                if azimuth < -90. {
                    azimuth = -180. - azimuth;
                } else if azimuth > 90. {
                    azimuth = 180. - azimuth;
                }
                let x;
                if input.channel_count() == 1 {
                    x = (azimuth + 90.) / 180.;
                } else {
                    x = if azimuth <= 0. {
                        (azimuth + 90.) / 90.
                    } else {
                        azimuth / 90.
                    };
                }
                let PanGains {
                    left: gain_left,
                    right: gain_right,
                } = equal_power_pan_gains(x);
                if input.channel_count() == 1 {
                    output_left[frame] = input_left[frame] * gain_left * gain;
                    output_right[frame] = input_left[frame] * gain_right * gain;
                } else if azimuth <= 0. {
                    output_left[frame] = (input_left[frame] + input_right[frame] * gain_left) * gain;
                    output_right[frame] = input_right[frame] * gain_right * gain;
                } else {
                    output_left[frame] = input_left[frame] * gain_left * gain;
                    output_right[frame] = (input_right[frame] + input_left[frame] * gain_right) * gain;
                }
            }
        }
    }
}

// Passes the input through unchanged; used for nodes whose processing is not implemented yet but which should not
// silence the signal path, like DynamicsCompressorNode.
struct PassthroughRenderNode;

impl NodeProcessor for PassthroughRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());
            BusRef(data.output(0)).copy_from(&input);
        }
    }
}

pub fn create_destination_render_node(node_id: u64, quantum_size: usize, channel_count: usize) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, channel_count);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    data.channel_count.set(channel_count);
    data.channel_count_mode.set(ChannelCountMode::Explicit);
    node_handle(data, DestinationRenderNode)
}

pub fn create_gain_render_node(node_id: u64, quantum_size: usize, gain: Arc<RenderAudioParam>) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    node_handle(
        data,
        GainRenderNode {
            gain,
            gain_values: vec![0.; quantum_size],
        },
    )
}

pub fn create_constant_source_render_node(
    node_id: u64,
    quantum_size: usize,
    offset: Arc<RenderAudioParam>,
) -> Box<NodeHandle> {
    let data = RenderNodeData::new(node_id, 0, 1, quantum_size, 1);
    node_handle(
        data,
        ConstantSourceRenderNode {
            source: ScheduledSourceRenderNode::default(),
            offset,
            offset_values: vec![0.; quantum_size],
        },
    )
}

pub fn create_oscillator_render_node(
    node_id: u64,
    quantum_size: usize,
    frequency: Arc<RenderAudioParam>,
    detune: Arc<RenderAudioParam>,
) -> Box<NodeHandle> {
    let data = RenderNodeData::new(node_id, 0, 1, quantum_size, 1);
    node_handle(
        data,
        OscillatorRenderNode {
            source: ScheduledSourceRenderNode::default(),
            frequency,
            detune,
            frequency_values: vec![0.; quantum_size],
            detune_values: vec![0.; quantum_size],
            r#type: OscillatorType::Sine,
            periodic_wave: None,
            periodic_wave_scale: 1.,
            phase: 0.,
        },
    )
}

pub fn create_audio_buffer_source_render_node(
    node_id: u64,
    quantum_size: usize,
    playback_rate: Arc<RenderAudioParam>,
    detune: Arc<RenderAudioParam>,
) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 0, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    node_handle(
        data,
        AudioBufferSourceRenderNode {
            source: ScheduledSourceRenderNode::default(),
            playback_rate,
            detune,
            playback_rate_values: vec![0.; quantum_size],
            detune_values: vec![0.; quantum_size],
            buffer: None,
            r#loop: false,
            loop_start: 0.,
            loop_end: 0.,
            offset: 0.,
            duration: None,
            playhead: 0.,
            playhead_valid: false,
            played_time: 0.,
        },
    )
}

pub fn create_delay_render_node(
    node_id: u64,
    quantum_size: usize,
    delay_time: Arc<RenderAudioParam>,
    max_delay_time: f64,
) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    node_handle(
        data,
        DelayRenderNode {
            delay_time,
            delay_values: vec![0.; quantum_size],
            max_delay_time,
            history: Vec::new(),
            write_index: 0,
        },
    )
}

pub fn create_stereo_panner_render_node(
    node_id: u64,
    quantum_size: usize,
    pan: Arc<RenderAudioParam>,
) -> Box<NodeHandle> {
    let data = RenderNodeData::new(node_id, 1, 1, quantum_size, 2);
    data.channel_count_mode.set(ChannelCountMode::ClampedMax);
    node_handle(
        data,
        StereoPannerRenderNode {
            pan,
            pan_values: vec![0.; quantum_size],
        },
    )
}

pub fn create_channel_splitter_render_node(
    node_id: u64,
    quantum_size: usize,
    number_of_outputs: usize,
) -> Box<NodeHandle> {
    let data = RenderNodeData::new(node_id, 1, number_of_outputs, quantum_size, 1);
    data.channel_count.set(number_of_outputs);
    data.channel_count_mode.set(ChannelCountMode::Explicit);
    data.channel_interpretation.set(ChannelInterpretation::Discrete);
    node_handle(data, ChannelSplitterRenderNode)
}

pub fn create_channel_merger_render_node(
    node_id: u64,
    quantum_size: usize,
    number_of_inputs: usize,
) -> Box<NodeHandle> {
    let data = RenderNodeData::new(node_id, number_of_inputs, 1, quantum_size, number_of_inputs);
    data.channel_count.set(1);
    data.channel_count_mode.set(ChannelCountMode::Explicit);
    node_handle(data, ChannelMergerRenderNode)
}

pub fn create_biquad_filter_render_node(
    node_id: u64,
    quantum_size: usize,
    frequency: Arc<RenderAudioParam>,
    detune: Arc<RenderAudioParam>,
    q: Arc<RenderAudioParam>,
    gain: Arc<RenderAudioParam>,
) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    node_handle(
        data,
        BiquadFilterRenderNode {
            frequency,
            detune,
            q,
            gain,
            frequency_values: vec![0.; quantum_size],
            detune_values: vec![0.; quantum_size],
            q_values: vec![0.; quantum_size],
            gain_values: vec![0.; quantum_size],
            r#type: BiquadFilterType::Lowpass,
            filter_states: Vec::with_capacity(MAX_NUMBER_OF_CHANNELS),
        },
    )
}

pub fn create_iir_filter_render_node(
    node_id: u64,
    quantum_size: usize,
    feedforward: &[f64],
    feedback: &[f64],
) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    assert!(feedforward.len() < 32);
    assert!(feedback.len() < 32);
    node_handle(
        data,
        IIRFilterRenderNode {
            feedforward: feedforward.to_vec(),
            feedback: feedback.to_vec(),
            filter_states: Vec::with_capacity(MAX_NUMBER_OF_CHANNELS),
            position: 0,
        },
    )
}

pub fn create_wave_shaper_render_node(node_id: u64, quantum_size: usize) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    node_handle(
        data,
        WaveShaperRenderNode {
            curve: None,
            oversample: OverSampleType::None,
            channel_states: Vec::with_capacity(MAX_NUMBER_OF_CHANNELS),
            two_times_samples: vec![0.; quantum_size * 2],
            four_times_samples: vec![0.; quantum_size * 4],
        },
    )
}

pub fn create_convolver_render_node(node_id: u64, quantum_size: usize) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, 2);
    data.channel_count_mode.set(ChannelCountMode::ClampedMax);
    node_handle(
        data,
        ConvolverRenderNode {
            fft: FFT::new(2 * quantum_size),
            kernel: None,
            delay_line: None,
            delay_line_index: 0,
            window: std::array::from_fn(|_| vec![0.; 2 * quantum_size]),
            input_channel_count: 0,
            transform_real: vec![0.; 2 * quantum_size],
            transform_imag: vec![0.; 2 * quantum_size],
            accumulator_real: vec![0.; quantum_size + 1],
            accumulator_imag: vec![0.; quantum_size + 1],
            // Nothing has come in yet, so there's no tail to play.
            silent_input_quanta: usize::MAX,
        },
    )
}

pub fn create_panner_render_node(
    node_id: u64,
    quantum_size: usize,
    params: &[&Arc<RenderAudioParam>; 15],
) -> Box<NodeHandle> {
    let data = RenderNodeData::new(node_id, 1, 1, quantum_size, 2);
    data.channel_count_mode.set(ChannelCountMode::ClampedMax);
    node_handle(
        data,
        PannerRenderNode {
            params: PannerParams {
                position_x: Arc::clone(params[0]),
                position_y: Arc::clone(params[1]),
                position_z: Arc::clone(params[2]),
                orientation_x: Arc::clone(params[3]),
                orientation_y: Arc::clone(params[4]),
                orientation_z: Arc::clone(params[5]),
            },
            listener_params: ListenerParams {
                position_x: Arc::clone(params[6]),
                position_y: Arc::clone(params[7]),
                position_z: Arc::clone(params[8]),
                forward_x: Arc::clone(params[9]),
                forward_y: Arc::clone(params[10]),
                forward_z: Arc::clone(params[11]),
                up_x: Arc::clone(params[12]),
                up_y: Arc::clone(params[13]),
                up_z: Arc::clone(params[14]),
            },
            config: SetPannerParameters::default(),
            param_values: (0..15).map(|_| vec![0.; quantum_size]).collect(),
        },
    )
}

pub fn create_passthrough_render_node(node_id: u64, quantum_size: usize) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);
    node_handle(data, PassthroughRenderNode)
}
