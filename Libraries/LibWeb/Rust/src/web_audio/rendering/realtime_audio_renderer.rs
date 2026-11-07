/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop, clippy::manual_memcpy)]
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU32, AtomicU64, Ordering, fence};

use super::audio_bus::AudioBus;
use super::render_graph::RenderGraph;
use super::render_node::RenderContext;
use super::renderer_callbacks::{CallbackService, RendererCallbacks};
use crate::web_audio::control_message::clone_arc;
use crate::web_audio::control_message_queue::ControlMessageQueue;

// The output buffer most recently handed to the stream, and when its first frame plays.
#[derive(Default)]
struct OutputAnchor {
    version: AtomicU64,
    first_frame_index: AtomicU64,
    end_frame_index: AtomicU64,
    plays_at_nanoseconds: AtomicI64,
}

impl OutputAnchor {
    fn store(&self, first_frame_index: u64, end_frame_index: u64, plays_at_nanoseconds: i64) {
        let version = self.version.load(Ordering::Relaxed);
        self.version.store(version.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::SeqCst);
        self.first_frame_index.store(first_frame_index, Ordering::Relaxed);
        self.end_frame_index.store(end_frame_index, Ordering::Relaxed);
        self.plays_at_nanoseconds.store(plays_at_nanoseconds, Ordering::Relaxed);
        self.version.store(version.wrapping_add(2), Ordering::Release);
    }

    fn read(&self) -> Option<(u64, u64, i64)> {
        for _retry in 0..32 {
            let version_before = self.version.load(Ordering::Acquire);
            if !version_before.is_multiple_of(2) {
                continue;
            }
            let copy = (
                self.first_frame_index.load(Ordering::Relaxed),
                self.end_frame_index.load(Ordering::Relaxed),
                self.plays_at_nanoseconds.load(Ordering::Relaxed),
            );
            fence(Ordering::Acquire);
            let version_after = self.version.load(Ordering::Relaxed);
            if version_before == version_after {
                return Some(copy);
            }
        }
        None
    }
}

// State below is only used on the audio thread, except that the device configuration is recorded on the control
// thread after stream creation, before the first data request callback can run.
struct RenderState {
    graph: RenderGraph,
    device_bus: Option<Box<AudioBus>>,
    device_channel_count: usize,
    frames_output: u64,
    playhead_step: f64,
    playhead: f64,
    pending_samples: Vec<f32>,
}

pub struct RealtimeAudioRenderer {
    control_message_queue: Arc<ControlMessageQueue>,
    destination_node_id: u64,
    sample_rate: f32,
    quantum_size: usize,
    callbacks: CallbackService,
    frames_rendered: AtomicU64,
    suspended: AtomicBool,
    shutting_down: AtomicBool,
    output_anchor: OutputAnchor,
    device_sample_rate: AtomicU32,
    state: UnsafeCell<RenderState>,
}

// SAFETY: Only the audio thread mutates render state after the suspended stream's device configuration is recorded.
unsafe impl Send for RealtimeAudioRenderer {}
unsafe impl Sync for RealtimeAudioRenderer {}
struct ControlTask {
    renderer: Arc<RealtimeAudioRenderer>,
    ended_nodes: Vec<u64>,
}

impl RealtimeAudioRenderer {
    fn new(
        queue: Arc<ControlMessageQueue>,
        destination_node_id: u64,
        sample_rate: f32,
        quantum_size: usize,
        callbacks: CallbackService,
    ) -> Self {
        Self {
            control_message_queue: queue,
            destination_node_id,
            sample_rate,
            quantum_size,
            callbacks,
            frames_rendered: AtomicU64::new(0),
            suspended: AtomicBool::new(false),
            shutting_down: AtomicBool::new(false),
            output_anchor: OutputAnchor::default(),
            device_sample_rate: AtomicU32::new(0),
            state: UnsafeCell::new(RenderState {
                graph: RenderGraph::default(),
                device_bus: None,
                device_channel_count: 0,
                frames_output: 0,
                playhead_step: 1.,
                playhead: 0.,
                pending_samples: Vec::new(),
            }),
        }
    }

    fn output_time_played(&self, now_nanoseconds: i64) -> f64 {
        let maybe_anchor = self.output_anchor.read();
        let device_sample_rate = self.device_sample_rate.load(Ordering::Relaxed);
        if maybe_anchor.is_none() || device_sample_rate == 0 {
            return 0.;
        }
        let (first_frame_index, end_frame_index, plays_at_nanoseconds) = maybe_anchor.unwrap();
        let seconds_since_buffer_started = (now_nanoseconds - plays_at_nanoseconds).max(0) as f64 / 1e9;
        let buffer_start_seconds = first_frame_index as f64 / f64::from(device_sample_rate);
        let buffer_end_seconds = end_frame_index as f64 / f64::from(device_sample_rate);
        if buffer_end_seconds < buffer_start_seconds + seconds_since_buffer_started {
            buffer_end_seconds
        } else {
            buffer_start_seconds + seconds_since_buffer_started
        }
    }

    unsafe fn fill_output_buffer(self: &Arc<Self>, buffer: &mut [f32], buffer_starts_playing_at: i64) -> usize {
        let state = unsafe { &mut *self.state.get() };
        if self.shutting_down.load(Ordering::SeqCst) || state.device_channel_count == 0 {
            return 0;
        }
        let channel_count = state.device_channel_count;
        let frames_requested = buffer.len() / channel_count;
        self.output_anchor.store(
            state.frames_output,
            state.frames_output + frames_requested as u64,
            buffer_starts_playing_at,
        );
        state.frames_output += frames_requested as u64;
        for frame in 0..frames_requested {
            let first_frame = state.playhead as usize;
            let fraction = (state.playhead - first_frame as f64) as f32;

            // Interpolation between adjacent rendered frames requires one frame of lookahead.
            while state.pending_samples.len() / channel_count < first_frame + 2 {
                self.render_quantum_into_pending_samples(state);
            }
            for channel in 0..channel_count {
                let mut sample = state.pending_samples[first_frame * channel_count + channel];
                if fraction != 0. {
                    let next_sample = state.pending_samples[(first_frame + 1) * channel_count + channel];
                    sample += fraction * (next_sample - sample);
                }
                buffer[frame * channel_count + channel] = sample;
            }
            state.playhead += state.playhead_step;
        }

        // Trim rendered frames preceding the playhead. If resampling advances the playhead beyond the rendered frames,
        // carry the remaining offset into the next callback.
        let rendered_frames = state.pending_samples.len() / channel_count;
        let consumed_frames = (state.playhead as usize).min(rendered_frames);
        if consumed_frames > 0 {
            state.pending_samples.drain(..consumed_frames * channel_count);
            state.playhead -= consumed_frames as f64;
        }
        buffer.len()
    }

    fn render_quantum_into_pending_samples(self: &Arc<Self>, state: &mut RenderState) {
        state.graph.apply_control_messages(self.control_message_queue.drain());
        let context = RenderContext {
            sample_rate: f64::from(self.sample_rate),
            quantum_size: self.quantum_size,
            quantum_start_frame: self.frames_rendered.load(Ordering::SeqCst),
        };
        state.graph.render_quantum(&context);

        // Mix the destination's output to the stream's channel layout and append it as interleaved samples.
        if let Some(device_bus) = &mut state.device_bus {
            device_bus.zero();
            if let Some(destination_node) = state.graph.destination() {
                device_bus.sum_from(unsafe { &*destination_node.data().output(0) }, true);
            }
            let sample_offset = state.pending_samples.len();
            state
                .pending_samples
                .resize(sample_offset + self.quantum_size * state.device_channel_count, 0.);
            for frame in 0..self.quantum_size {
                for channel in 0..state.device_channel_count {
                    state.pending_samples[sample_offset + frame * state.device_channel_count + channel] =
                        device_bus.channel(channel)[frame];
                }
            }
        }
        self.frames_rendered
            .fetch_add(self.quantum_size as u64, Ordering::SeqCst);
        let ended_nodes = state.graph.take_ended_nodes();
        if !ended_nodes.is_empty() {
            unsafe extern "C" fn invoke(context: *mut c_void) {
                let task = unsafe { Box::from_raw(context.cast::<ControlTask>()) };
                let callbacks = &task.renderer.callbacks;
                unsafe {
                    (callbacks.callbacks.on_sources_ended)(callbacks.owner.pointer, ak::Span::new(&task.ended_nodes));
                }
            }
            let task = Box::into_raw(Box::new(ControlTask {
                renderer: Arc::clone(self),
                ended_nodes,
            }));
            unsafe {
                self.callbacks.post(task.cast(), invoke);
            }
        }
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_create(
    queue: *const ControlMessageQueue,
    destination_node_id: u64,
    sample_rate: f32,
    quantum_size: usize,
    callbacks: RendererCallbacks,
) -> *const RealtimeAudioRenderer {
    Arc::into_raw(Arc::new(RealtimeAudioRenderer::new(
        unsafe { clone_arc(queue) },
        destination_node_id,
        sample_rate,
        quantum_size,
        unsafe { CallbackService::new(callbacks) },
    )))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_destroy(renderer: *const RealtimeAudioRenderer) {
    drop(unsafe { Arc::from_raw(renderer) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_prepare(renderer: *const RealtimeAudioRenderer) {
    let renderer = unsafe { &*renderer };
    unsafe { &mut *renderer.state.get() }
        .graph
        .set_destination(renderer.destination_node_id);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_configure(
    renderer: *const RealtimeAudioRenderer,
    channel_count: usize,
    sample_rate: u32,
) {
    let renderer = unsafe { &*renderer };
    let state = unsafe { &mut *renderer.state.get() };
    state.device_channel_count = channel_count;
    renderer.device_sample_rate.store(sample_rate, Ordering::Relaxed);
    state.device_bus = Some(Box::new(AudioBus::new(channel_count, renderer.quantum_size)));
    state.playhead_step = f64::from(renderer.sample_rate) / f64::from(sample_rate);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_fill(
    renderer: *const RealtimeAudioRenderer,
    output: ak::SpanMut<'_, f32>,
    plays_at_nanoseconds: i64,
) -> usize {
    let renderer = unsafe { clone_arc(renderer) };
    unsafe { renderer.fill_output_buffer(output.into_slice(), plays_at_nanoseconds) }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_frames_rendered(
    renderer: *const RealtimeAudioRenderer,
) -> u64 {
    unsafe { &*renderer }.frames_rendered.load(Ordering::SeqCst)
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_output_time(
    renderer: *const RealtimeAudioRenderer,
    now_nanoseconds: i64,
) -> f64 {
    unsafe { &*renderer }.output_time_played(now_nanoseconds)
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_set_suspended(
    renderer: *const RealtimeAudioRenderer,
    suspended: bool,
) {
    unsafe { &*renderer }.suspended.store(suspended, Ordering::SeqCst);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_is_suspended(
    renderer: *const RealtimeAudioRenderer,
) -> bool {
    unsafe { &*renderer }.suspended.load(Ordering::SeqCst)
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_stop(renderer: *const RealtimeAudioRenderer) {
    unsafe { &*renderer }.shutting_down.store(true, Ordering::SeqCst);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_realtime_renderer_is_shutting_down(
    renderer: *const RealtimeAudioRenderer,
) -> bool {
    unsafe { &*renderer }.shutting_down.load(Ordering::SeqCst)
}
