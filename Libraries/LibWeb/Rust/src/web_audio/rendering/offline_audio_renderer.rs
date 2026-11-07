/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop, clippy::manual_memcpy)]

use std::cell::UnsafeCell;
use std::collections::HashSet;
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};

use super::render_graph::RenderGraph;
use super::render_node::RenderContext;
use crate::web_audio::control_message_queue::ControlMessageQueue;
use crate::web_audio::control_thread::{ControlThread, ControlThreadCell};

pub trait OfflineAudioRendererClient {
    fn rendering_suspended(&self, suspend_time: f64);
    fn rendering_completed(&self, rendered_channels: &[Vec<f32>]);
    fn sources_ended(&self, nodes: &[u64]);
}

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

// Renders an OfflineAudioContext's graph on a dedicated rendering thread, which is created when rendering starts and
// exits once the last render quantum has been processed. Progress is reported back to the control thread by posting
// events to the main thread's event loop.
// https://webaudio.github.io/web-audio-api/#OfflineAudioContext
pub struct OfflineAudioRenderer {
    control_message_queue: Arc<ControlMessageQueue>,
    destination_node_id: u64,
    number_of_channels: usize,
    length: usize,
    sample_rate: f32,
    quantum_size: usize,
    control_thread: ControlThread,
    client: ControlThreadCell<Option<Rc<dyn OfflineAudioRendererClient>>>,
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

impl OfflineAudioRenderer {
    pub fn new(
        control_message_queue: Arc<ControlMessageQueue>,
        destination_node_id: u64,
        number_of_channels: usize,
        length: usize,
        sample_rate: f32,
        quantum_size: usize,
    ) -> Arc<Self> {
        let mut rendered_channels = Vec::new();
        rendered_channels.resize_with(number_of_channels, Vec::new);
        for channel in &mut rendered_channels {
            channel.resize(length, 0.);
        }
        Arc::new(Self {
            control_message_queue,
            destination_node_id,
            number_of_channels,
            length,
            sample_rate,
            quantum_size,
            control_thread: ControlThread::current(),
            client: ControlThreadCell::new(None),
            thread: UnsafeCell::new(None),
            graph: UnsafeCell::new(RenderGraph::default()),
            rendered_channels: UnsafeCell::new(rendered_channels),
            frames_rendered: AtomicU64::new(0),
            suspend_mutex: Mutex::new(SuspensionState::default()),
            resume_signal: Condvar::new(),
        })
    }

    /// The callbacks below are set on the control thread before rendering starts and are always invoked on the control
    /// thread. They are cleared once rendering completes to break the reference cycle with the context.
    pub fn set_client(&self, client: Option<Rc<dyn OfflineAudioRendererClient>>) {
        self.client.replace(client);
    }

    pub fn start_rendering(self: &Arc<Self>) {
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

    /// Ends rendering without completing it, unblocking the rendering thread if it is waiting in a suspension.
    pub fn stop(&self) {
        let mut state = self.suspend_mutex.lock().unwrap();
        state.shutting_down = true;
        self.resume_signal.notify_all();
    }

    /// Registers a suspension at the given render quantum boundary. Returns false if the rendering thread already
    /// looked for suspensions at that boundary.
    pub fn request_suspend(&self, frame: u64) -> bool {
        let mut state = self.suspend_mutex.lock().unwrap();
        if frame < state.next_suspend_check_frame {
            return false;
        }
        state.suspend_frames.insert(frame);
        true
    }

    pub fn resume(&self) {
        let mut state = self.suspend_mutex.lock().unwrap();
        state.suspended = false;
        self.resume_signal.notify_all();
    }

    pub fn frames_rendered(&self) -> u64 {
        self.frames_rendered.load(Ordering::SeqCst)
    }

    fn post_to_control_thread(self: &Arc<Self>, notification: Notification) {
        let renderer = Arc::clone(self);
        self.control_thread.post(move || {
            let Some(client) = renderer.client.get() else {
                return;
            };
            match notification {
                Notification::Complete => {
                    // SAFETY: The rendering thread posts the completion after its last write.
                    client.rendering_completed(unsafe { &*renderer.rendered_channels.get() });
                }
                Notification::Suspended(time) => client.rendering_suspended(time),
                Notification::SourcesEnded(nodes) => client.sources_ended(&nodes),
            }
        });
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
