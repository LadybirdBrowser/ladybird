/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};

use ak::SpanMut;

use super::audio_bus::AudioBus;
use crate::web_audio::audio_param_timeline::AudioParamTimeline;

// State for a single render quantum that is passed to every node being processed.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct RenderContext {
    pub sample_rate: f64,
    pub quantum_size: usize,
    pub quantum_start_frame: u64,
}

impl RenderContext {
    pub fn quantum_start_time(&self) -> f64 {
        self.quantum_start_frame as f64 / self.sample_rate
    }

    pub fn sample_period(&self) -> f64 {
        1. / self.sample_rate
    }

    pub fn frame_time(&self, frame: usize) -> f64 {
        (self.quantum_start_frame + frame as u64) as f64 / self.sample_rate
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum AutomationRate {
    ARate,
    KRate,
}

struct Input {
    source: u64,
    output_index: usize,
}

struct RenderAudioParamState {
    inputs: Vec<Input>,
    input_scratch: Option<Box<AudioBus>>,
}

// The rendering thread's view of an AudioParam. The automation timeline is shared with the AudioParam on the control
// thread; input connections are only manipulated by the rendering thread.
// https://webaudio.github.io/web-audio-api/#computedvalue
pub struct RenderAudioParam {
    timeline: Arc<AudioParamTimeline>,
    min_value: f32,
    max_value: f32,
    automation_rate: AtomicU8,
    state: UnsafeCell<RenderAudioParamState>,
}

// SAFETY: Only the rendering thread accesses state, and other threads use only the atomic automation rate.
unsafe impl Send for RenderAudioParam {}
unsafe impl Sync for RenderAudioParam {}

pub type PullOutput = unsafe extern "C" fn(*mut c_void, u64, usize, *const RenderContext) -> *const AudioBus;

struct RenderGraph {
    graph: *mut c_void,
    pull_output: PullOutput,
}

impl RenderAudioParam {
    fn new(timeline: Arc<AudioParamTimeline>, min_value: f32, max_value: f32, automation_rate: AutomationRate) -> Self {
        Self {
            timeline,
            min_value,
            max_value,
            automation_rate: AtomicU8::new(automation_rate as u8),
            state: UnsafeCell::new(RenderAudioParamState {
                inputs: Vec::new(),
                input_scratch: None,
            }),
        }
    }

    fn set_automation_rate(&self, automation_rate: AutomationRate) {
        self.automation_rate.store(automation_rate as u8, Ordering::SeqCst);
    }

    fn automation_rate(&self) -> AutomationRate {
        if self.automation_rate.load(Ordering::SeqCst) == AutomationRate::ARate as u8 {
            AutomationRate::ARate
        } else {
            AutomationRate::KRate
        }
    }

    // https://webaudio.github.io/web-audio-api/#computation-of-value
    unsafe fn compute(&self, graph: &RenderGraph, context: &RenderContext, output: &mut [f32]) {
        let state = unsafe { &mut *self.state.get() };

        // 1. paramIntrinsicValue will be calculated at each time, which is either the value set directly to the value
        //    attribute, or, if there are any automation events with times before or at this time, the value as calculated
        //    from these events.
        // NB: An a-rate parameter takes the exact value of the signal at each sample frame of the render quantum, while a
        //     k-rate parameter uses the value of the very first sample frame for the entire quantum.
        let rate = self.automation_rate();
        if rate == AutomationRate::KRate {
            let value = self.timeline.value_at_time(context.quantum_start_time());
            output.fill(value);
        } else {
            self.timeline
                .sample(context.quantum_start_time(), context.sample_period(), output);
        }

        // 2. Set [[current value]] to the value of paramIntrinsicValue at the beginning of this render quantum.
        // NB: The render param keeps no [[current value]] slot of its own; the AudioParam's value getter reads the shared
        //     automation timeline directly on the control thread.

        // 3. paramComputedValue is the sum of the paramIntrinsicValue value and the value of the input AudioParam
        //    buffer. If the sum is NaN, replace the sum with the defaultValue.
        for input in &state.inputs {
            let bus = unsafe { (graph.pull_output)(graph.graph, input.source, input.output_index, context) };
            let Some(bus) = (unsafe { bus.as_ref() }) else {
                continue;
            };

            // The AudioParam will take the rendered audio data from any AudioNode output connected to it and convert it to
            // mono by down-mixing if it is not already mono.
            if state
                .input_scratch
                .as_ref()
                .is_none_or(|scratch| scratch.frame_count() != context.quantum_size)
            {
                state.input_scratch = Some(Box::new(AudioBus::new(1, context.quantum_size)));
            }
            let input_scratch = state.input_scratch.as_mut().unwrap();
            input_scratch.zero();
            input_scratch.sum_from(bus, true);

            let input_samples = input_scratch.channel(0);
            if rate == AutomationRate::KRate {
                for value in output.iter_mut() {
                    *value += input_samples[0];
                }
            } else {
                for frame in 0..output.len() {
                    output[frame] += input_samples[frame];
                }
            }
        }
        let default_value = self.timeline.default_value();
        for value in output.iter_mut() {
            if value.is_nan() {
                *value = default_value;
            }
        }

        // 4. If this AudioParam is a compound parameter, compute its final value with other AudioParams.
        // NB: None of the AudioParams backed by a render node are compound parameters.

        // 5. Set computedValue to paramComputedValue.

        // NB: The computedValue is clamped to the simple nominal range for this parameter.
        // https://webaudio.github.io/web-audio-api/#simple-nominal-range
        for value in output {
            assert!(self.max_value >= self.min_value);
            *value = if *value > self.max_value {
                self.max_value
            } else if *value < self.min_value {
                self.min_value
            } else {
                *value
            };
        }
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_create(
    timeline: *const AudioParamTimeline,
    min_value: f32,
    max_value: f32,
    automation_rate: AutomationRate,
) -> *const RenderAudioParam {
    unsafe {
        Arc::increment_strong_count(timeline);
    }
    let timeline = unsafe { Arc::from_raw(timeline) };
    Arc::into_raw(Arc::new(RenderAudioParam::new(
        timeline,
        min_value,
        max_value,
        automation_rate,
    )))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_destroy(param: *const RenderAudioParam) {
    drop(unsafe { Arc::from_raw(param) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_set_automation_rate(
    param: *const RenderAudioParam,
    rate: AutomationRate,
) {
    unsafe { &*param }.set_automation_rate(rate);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_automation_rate(
    param: *const RenderAudioParam,
) -> AutomationRate {
    unsafe { &*param }.automation_rate()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_clear_inputs(param: *const RenderAudioParam) {
    unsafe { &mut *(*param).state.get() }.inputs.clear();
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_add_input(
    param: *const RenderAudioParam,
    source: u64,
    output_index: usize,
) {
    unsafe { &mut *(*param).state.get() }
        .inputs
        .push(Input { source, output_index });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_compute(
    param: *const RenderAudioParam,
    graph: *mut c_void,
    pull_output: PullOutput,
    context: *const RenderContext,
    output: SpanMut<'_, f32>,
) {
    let graph = RenderGraph { graph, pull_output };
    unsafe {
        (&*param).compute(&graph, &*context, output.into_slice());
    }
}
