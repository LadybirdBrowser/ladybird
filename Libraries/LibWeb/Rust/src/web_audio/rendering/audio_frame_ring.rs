/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use super::node_message::SharedCppObject;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct AudioFrameRingCallbacks {
    pub channel_count: unsafe extern "C" fn(*mut c_void) -> u32,
    pub frames_available: unsafe extern "C" fn(*mut c_void) -> usize,
    pub frames_free: unsafe extern "C" fn(*mut c_void) -> usize,
    pub try_pop: unsafe extern "C" fn(*mut c_void, ak::SpanMut<'_, f32>) -> usize,
    pub try_push: unsafe extern "C" fn(*mut c_void, ak::Span<'_, f32>) -> usize,
}

pub struct AudioFrameRing {
    pub owner: SharedCppObject,
    pub callbacks: AudioFrameRingCallbacks,
}

impl AudioFrameRing {
    pub fn channel_count(&self) -> u32 {
        unsafe { (self.callbacks.channel_count)(self.owner.pointer) }
    }

    pub fn frames_available(&self) -> usize {
        unsafe { (self.callbacks.frames_available)(self.owner.pointer) }
    }

    pub fn frames_free(&self) -> usize {
        unsafe { (self.callbacks.frames_free)(self.owner.pointer) }
    }

    pub fn try_pop(&self, samples: &mut [f32]) -> usize {
        unsafe { (self.callbacks.try_pop)(self.owner.pointer, ak::SpanMut::new(samples)) }
    }

    pub fn try_push(&self, samples: &[f32]) -> usize {
        unsafe { (self.callbacks.try_push)(self.owner.pointer, ak::Span::new(samples)) }
    }
}
