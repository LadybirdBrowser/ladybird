/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::RefCell;
use std::ffi::c_void;
use std::mem::ManuallyDrop;
use std::ptr::NonNull;
use std::thread::{self, ThreadId};

unsafe extern "C" {
    fn web_audio_current_event_loop() -> *mut c_void;
    fn web_audio_deferred_invoke(event_loop: *mut c_void, task: *mut c_void, invoke: unsafe extern "C" fn(*mut c_void));
}

#[derive(Clone, Copy)]
pub struct ControlThread {
    event_loop: NonNull<c_void>,
}

// SAFETY: Core::EventLoop::deferred_invoke() may be called from any thread, and the control thread's event loop
// outlives its audio contexts.
unsafe impl Send for ControlThread {}
unsafe impl Sync for ControlThread {}

impl ControlThread {
    pub fn current() -> Self {
        Self {
            event_loop: NonNull::new(unsafe { web_audio_current_event_loop() }).expect("the thread has an event loop"),
        }
    }

    pub fn post<F: FnOnce() + Send + 'static>(self, task: F) {
        unsafe extern "C" fn invoke<F: FnOnce()>(task: *mut c_void) {
            let task = unsafe { Box::from_raw(task.cast::<F>()) };
            task();
        }
        let task = Box::into_raw(Box::new(task));
        unsafe {
            web_audio_deferred_invoke(self.event_loop.as_ptr(), task.cast(), invoke::<F>);
        }
    }
}

pub struct ControlThreadCell<T> {
    thread: ThreadId,
    value: ManuallyDrop<RefCell<T>>,
}

// SAFETY: Every use of the value checks that it is on the thread that created the cell, and the value is only dropped
// there.
unsafe impl<T> Send for ControlThreadCell<T> {}
unsafe impl<T> Sync for ControlThreadCell<T> {}

impl<T> ControlThreadCell<T> {
    pub fn new(value: T) -> Self {
        Self {
            thread: thread::current().id(),
            value: ManuallyDrop::new(RefCell::new(value)),
        }
    }

    fn value(&self) -> &RefCell<T> {
        assert_eq!(
            thread::current().id(),
            self.thread,
            "only the control thread uses this state"
        );
        &self.value
    }

    pub fn replace(&self, value: T) -> T {
        self.value().replace(value)
    }
}

impl<T: Clone> ControlThreadCell<T> {
    pub fn get(&self) -> T {
        self.value().borrow().clone()
    }
}

impl<T> Drop for ControlThreadCell<T> {
    fn drop(&mut self) {
        if thread::current().id() == self.thread {
            unsafe { ManuallyDrop::drop(&mut self.value) };
        }
    }
}
