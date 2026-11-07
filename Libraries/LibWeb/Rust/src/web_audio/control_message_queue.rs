/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::Mutex;

use super::control_message::{ControlMessage, ControlMessages};

// https://webaudio.github.io/web-audio-api/#control-message-queue
#[derive(Default)]
pub struct ControlMessageQueue {
    messages: Mutex<Vec<ControlMessage>>,
}

impl ControlMessageQueue {
    // Called by the control thread.
    pub fn enqueue(&self, message: ControlMessage) {
        let mut messages = self.messages.lock().unwrap();
        messages.push(message);
    }

    // Called by the rendering thread.
    pub fn drain(&self) -> Vec<ControlMessage> {
        let mut messages = self.messages.lock().unwrap();
        std::mem::take(&mut *messages)
    }
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_control_queue_create() -> *mut ControlMessageQueue {
    Box::into_raw(Box::new(ControlMessageQueue::default()))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_control_queue_destroy(queue: *mut ControlMessageQueue) {
    drop(unsafe { Box::from_raw(queue) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_control_queue_enqueue(
    queue: *const ControlMessageQueue,
    message: *mut ControlMessage,
) {
    unsafe { &*queue }.enqueue(*unsafe { Box::from_raw(message) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_control_queue_drain(
    queue: *const ControlMessageQueue,
) -> *mut ControlMessages {
    Box::into_raw(Box::new(ControlMessages {
        messages: unsafe { &*queue }.drain(),
    }))
}
