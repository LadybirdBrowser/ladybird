/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};

use super::pipe_callbacks::{CallbackService, PipeCallbacks};

#[derive(Clone, Copy)]
#[repr(C)]
pub struct ScriptProcessorPipeConfig {
    pub buffer_size: usize,
    pub input_channel_count: usize,
    pub output_channel_count: usize,
    pub block_count: usize,
    pub waits_for_processing: bool,
}

#[derive(Default)]
struct Block {
    is_owned_by_control_thread: AtomicBool,
    has_output: UnsafeCell<bool>,
}

pub struct ScriptProcessorPipe {
    pub config: ScriptProcessorPipeConfig,
    input_samples: Box<[UnsafeCell<f32>]>,
    output_samples: Box<[UnsafeCell<f32>]>,
    blocks: Box<[Block]>,
    callbacks: CallbackService,
    mutex: Mutex<()>,
    block_returned: Condvar,
    is_shut_down: AtomicBool,
}

// SAFETY: A block's acquire/release ownership flag transfers exclusive access to its sample ranges and output flag.
unsafe impl Send for ScriptProcessorPipe {}
unsafe impl Sync for ScriptProcessorPipe {}
struct ProcessTask {
    pipe: Arc<ScriptProcessorPipe>,
    block: usize,
    start_frame: u64,
}

impl ScriptProcessorPipe {
    fn new(config: ScriptProcessorPipeConfig, callbacks: CallbackService) -> Self {
        assert!(config.buffer_size > 0);
        assert!(config.block_count >= 2);
        Self {
            config,
            input_samples: (0..config.block_count * config.input_channel_count * config.buffer_size)
                .map(|_| UnsafeCell::new(0.))
                .collect(),
            output_samples: (0..config.block_count * config.output_channel_count * config.buffer_size)
                .map(|_| UnsafeCell::new(0.))
                .collect(),
            blocks: (0..config.block_count).map(|_| Block::default()).collect(),
            callbacks,
            mutex: Mutex::new(()),
            block_returned: Condvar::new(),
            is_shut_down: AtomicBool::new(false),
        }
    }

    /// # Safety
    /// The caller must own the block until it finishes accessing the samples.
    pub unsafe fn input_channel(&self, block: usize, channel: usize) -> (*mut f32, usize) {
        assert!(block < self.config.block_count);
        assert!(channel < self.config.input_channel_count);
        let offset = (block * self.config.input_channel_count + channel) * self.config.buffer_size;
        let samples = self.input_samples.as_ptr().cast::<f32>().cast_mut();
        (unsafe { samples.add(offset) }, self.config.buffer_size)
    }

    /// # Safety
    /// The caller must own the block until it finishes accessing the samples.
    pub unsafe fn output_channel(&self, block: usize, channel: usize) -> (*mut f32, usize) {
        assert!(block < self.config.block_count);
        assert!(channel < self.config.output_channel_count);
        let offset = (block * self.config.output_channel_count + channel) * self.config.buffer_size;
        let samples = self.output_samples.as_ptr().cast::<f32>().cast_mut();
        (unsafe { samples.add(offset) }, self.config.buffer_size)
    }

    pub(crate) unsafe fn has_output(&self, block: usize) -> bool {
        unsafe { *self.blocks[block].has_output.get() }
    }

    pub fn is_block_owned_by_rendering_thread(&self, block: usize) -> bool {
        !self.blocks[block].is_owned_by_control_thread.load(Ordering::Acquire)
    }

    pub fn hand_block_to_control_thread(self: &Arc<Self>, block: usize, start_frame: u64) {
        self.blocks[block]
            .is_owned_by_control_thread
            .store(true, Ordering::Release);
        unsafe extern "C" fn invoke(task: *mut c_void) {
            let task = unsafe { Box::from_raw(task.cast::<ProcessTask>()) };
            let pipe = &task.pipe;
            let processed = !pipe.is_shut_down()
                && unsafe {
                    (pipe.callbacks.callbacks.on_process)(pipe.callbacks.owner.pointer, task.block, task.start_frame)
                };
            if !processed {
                unsafe {
                    pipe.hand_block_back_to_rendering_thread(task.block, false);
                }
            }
        }
        let task = Box::into_raw(Box::new(ProcessTask {
            pipe: Arc::clone(self),
            block,
            start_frame,
        }));
        unsafe {
            self.callbacks.post(task.cast(), invoke);
        }
        if !self.config.waits_for_processing {
            return;
        }
        let mut locker = self.mutex.lock().unwrap();
        while !self.is_block_owned_by_rendering_thread(block) && !self.is_shut_down() {
            locker = self.block_returned.wait(locker).unwrap();
        }
    }

    /// # Safety
    /// The control thread must own the block and have finished accessing its samples.
    pub unsafe fn hand_block_back_to_rendering_thread(&self, block: usize, has_output: bool) {
        unsafe {
            *self.blocks[block].has_output.get() = has_output;
        }
        self.blocks[block]
            .is_owned_by_control_thread
            .store(false, Ordering::Release);

        // The lock orders this notification after a waiting rendering thread has checked the block's owner.
        let _locker = self.mutex.lock().unwrap();
        self.block_returned.notify_all();
    }

    pub fn shut_down(&self) {
        self.is_shut_down.store(true, Ordering::Release);
        let _locker = self.mutex.lock().unwrap();
        self.block_returned.notify_all();
    }

    pub fn is_shut_down(&self) -> bool {
        self.is_shut_down.load(Ordering::Acquire)
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_script_pipe_create(
    config: ScriptProcessorPipeConfig,
    callbacks: PipeCallbacks,
) -> *const ScriptProcessorPipe {
    Arc::into_raw(Arc::new(ScriptProcessorPipe::new(config, unsafe {
        CallbackService::new(callbacks)
    })))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_script_pipe_destroy(pipe: *const ScriptProcessorPipe) {
    drop(unsafe { Arc::from_raw(pipe) });
}
