/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop, clippy::manual_memcpy)]

use std::cell::UnsafeCell;
use std::collections::HashSet;
use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};

use super::render_graph::RenderGraph;
use super::render_node::RenderContext;
use super::renderer_callbacks::{CallbackService, RendererCallbacks};
use crate::web_audio::control_message::clone_arc;
use crate::web_audio::control_message_queue::ControlMessageQueue;

#[derive(Default)]
struct SuspensionState {
    suspend_frames: HashSet<u64>,

    // The next render quantum boundary at which the rendering thread will look for a scheduled suspension. It is
    // published under the same lock that guards the suspension set, so a suspension can never be registered for a
    // boundary the rendering thread has already checked.
    next_suspend_check_frame: u64,
    suspended: bool,
    shutting_down: bool,
}

pub struct OfflineAudioRenderer {
    control_message_queue: Arc<ControlMessageQueue>,
    destination_node_id: u64,
    number_of_channels: usize,
    length: usize,
    sample_rate: f32,
    quantum_size: usize,
    callbacks: CallbackService,
    thread: UnsafeCell<Option<std::thread::Thread>>,
    graph: UnsafeCell<RenderGraph>,
    rendered_channels: UnsafeCell<Vec<Vec<f32>>>,
    frames_rendered: AtomicU64,
    suspend_mutex: Mutex<SuspensionState>,
    resume_signal: Condvar,
}

// SAFETY: One render thread owns graph/sample mutations, and suspension/progress use the original synchronization.
unsafe impl Send for OfflineAudioRenderer {}
unsafe impl Sync for OfflineAudioRenderer {}
enum Notification {
    Complete,
    Suspended(f64),
    SourcesEnded(Vec<u64>),
}

struct ControlTask {
    renderer: Arc<OfflineAudioRenderer>,
    notification: Notification,
}

impl OfflineAudioRenderer {
    fn new(
        control_message_queue: Arc<ControlMessageQueue>,
        destination_node_id: u64,
        number_of_channels: usize,
        length: usize,
        sample_rate: f32,
        quantum_size: usize,
        callbacks: CallbackService,
    ) -> Self {
        let mut rendered_channels = Vec::new();
        rendered_channels.resize_with(number_of_channels, Vec::new);
        for channel in &mut rendered_channels {
            channel.resize(length, 0.);
        }
        Self {
            control_message_queue,
            destination_node_id,
            number_of_channels,
            length,
            sample_rate,
            quantum_size,
            callbacks,
            thread: UnsafeCell::new(None),
            graph: UnsafeCell::new(RenderGraph::default()),
            rendered_channels: UnsafeCell::new(rendered_channels),
            frames_rendered: AtomicU64::new(0),
            suspend_mutex: Mutex::new(SuspensionState::default()),
            resume_signal: Condvar::new(),
        }
    }

    unsafe fn start_rendering(self: &Arc<Self>) {
        assert!(unsafe { &*self.thread.get() }.is_none());
        unsafe { &mut *self.graph.get() }.set_destination(self.destination_node_id);
        let renderer = Arc::clone(self);
        let thread = std::thread::Builder::new()
            .name("OfflineAudioRenderer".into())
            .spawn(move || {
                renderer.render_thread_main();
            })
            .unwrap();
        unsafe {
            *self.thread.get() = Some(thread.thread().clone());
        }
        drop(thread);
    }

    fn stop(&self) {
        let mut state = self.suspend_mutex.lock().unwrap();
        state.shutting_down = true;
        self.resume_signal.notify_all();
    }

    fn request_suspend(&self, frame: u64) -> bool {
        let mut state = self.suspend_mutex.lock().unwrap();
        if frame < state.next_suspend_check_frame {
            return false;
        }
        state.suspend_frames.insert(frame);
        true
    }

    fn resume(&self) {
        let mut state = self.suspend_mutex.lock().unwrap();
        state.suspended = false;
        self.resume_signal.notify_all();
    }

    fn post_to_control_thread(self: &Arc<Self>, notification: Notification) {
        unsafe extern "C" fn invoke(context: *mut c_void) {
            let task = unsafe { Box::from_raw(context.cast::<ControlTask>()) };
            let callbacks = &task.renderer.callbacks;
            match &task.notification {
                Notification::Complete => unsafe {
                    (callbacks.callbacks.on_complete)(callbacks.owner.pointer);
                },
                Notification::Suspended(time) => unsafe {
                    (callbacks.callbacks.on_suspended)(callbacks.owner.pointer, *time);
                },
                Notification::SourcesEnded(nodes) => unsafe {
                    (callbacks.callbacks.on_sources_ended)(callbacks.owner.pointer, ak::Span::new(nodes));
                },
            }
        }
        let task = Box::into_raw(Box::new(ControlTask {
            renderer: Arc::clone(self),
            notification,
        }));
        unsafe {
            self.callbacks.post(task.cast(), invoke);
        }
    }

    fn render_thread_main(self: &Arc<Self>) {
        let mut current_frame = 0_u64;
        while current_frame < self.length as u64 {
            // Suspend rendering if a suspension was scheduled at this render quantum boundary.
            {
                let mut state = self.suspend_mutex.lock().unwrap();
                if state.shutting_down {
                    return;
                }
                state.next_suspend_check_frame = current_frame + self.quantum_size as u64;
                if state.suspend_frames.remove(&current_frame) {
                    state.suspended = true;
                    let suspend_time = current_frame as f64 / f64::from(self.sample_rate);
                    self.post_to_control_thread(Notification::Suspended(suspend_time));
                    while state.suspended && !state.shutting_down {
                        state = self.resume_signal.wait(state).unwrap();
                    }
                    if state.shutting_down {
                        return;
                    }
                }
            }
            let graph = unsafe { &mut *self.graph.get() };
            graph.apply_control_messages(self.control_message_queue.drain());
            let context = RenderContext {
                sample_rate: f64::from(self.sample_rate),
                quantum_size: self.quantum_size,
                quantum_start_frame: current_frame,
            };
            graph.render_quantum(&context);

            // Copy the destination's captured output into the rendered buffer.
            if let Some(destination_node) = graph.destination() {
                let destination_output = unsafe { &*destination_node.data().output(0) };
                let frames_to_copy = self.quantum_size.min(self.length - current_frame as usize);
                let rendered_channels = unsafe { &mut *self.rendered_channels.get() };
                for channel_index in 0..self.number_of_channels {
                    if channel_index >= destination_output.channel_count() {
                        break;
                    }
                    let samples = destination_output.channel(channel_index);
                    for frame in 0..frames_to_copy {
                        rendered_channels[channel_index][current_frame as usize + frame] = samples[frame];
                    }
                }
            }
            current_frame += self.quantum_size as u64;
            self.frames_rendered.store(current_frame, Ordering::SeqCst);
            let ended_nodes = graph.take_ended_nodes();
            if !ended_nodes.is_empty() {
                self.post_to_control_thread(Notification::SourcesEnded(ended_nodes));
            }
        }
        self.post_to_control_thread(Notification::Complete);
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_create(
    queue: *const ControlMessageQueue,
    destination_node_id: u64,
    number_of_channels: usize,
    length: usize,
    sample_rate: f32,
    quantum_size: usize,
    callbacks: RendererCallbacks,
) -> *const OfflineAudioRenderer {
    Arc::into_raw(Arc::new(OfflineAudioRenderer::new(
        unsafe { clone_arc(queue) },
        destination_node_id,
        number_of_channels,
        length,
        sample_rate,
        quantum_size,
        unsafe { CallbackService::new(callbacks) },
    )))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_destroy(renderer: *const OfflineAudioRenderer) {
    drop(unsafe { Arc::from_raw(renderer) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_start(renderer: *const OfflineAudioRenderer) {
    let renderer = unsafe { clone_arc(renderer) };
    unsafe {
        renderer.start_rendering();
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_stop(renderer: *const OfflineAudioRenderer) {
    unsafe { &*renderer }.stop();
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_resume(renderer: *const OfflineAudioRenderer) {
    unsafe { &*renderer }.resume();
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_request_suspend(
    renderer: *const OfflineAudioRenderer,
    frame: u64,
) -> bool {
    unsafe { &*renderer }.request_suspend(frame)
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_started(renderer: *const OfflineAudioRenderer) -> bool {
    unsafe { &*(*renderer).thread.get() }.is_some()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_frames_rendered(
    renderer: *const OfflineAudioRenderer,
) -> u64 {
    unsafe { &*renderer }.frames_rendered.load(Ordering::SeqCst)
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_offline_renderer_channel(
    renderer: *const OfflineAudioRenderer,
    channel: usize,
    output: *mut ak::Span<'static, f32>,
) {
    unsafe {
        *output = ak::Span::new(&(&*(*renderer).rendered_channels.get())[channel]);
    }
}
