/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::Mutex;

use super::control_message::ControlMessage;

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
