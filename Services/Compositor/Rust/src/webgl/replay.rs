/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The replay of the commands and the synchronous calls of one WebGL context, on a copy of what WebContent sent: every
//! struct, span and size is checked before GL sees it, and object ids become the names of GL objects.

use core::ffi::{c_char, c_void};

use libcompositing_rust::display_list::commands::CanvasId;
use libcompositing_rust::webgl::ImageSource;
use libcompositing_rust::webgl::vocabulary::{CommandType, SyncCallType, commands, sync_calls};
use libcompositing_rust::webgl::wire::{
    DataSpan, WireStruct, decode_sync_request, encode_reply, records, reply_data_spans,
};

use super::gl::GlFunctions;
use super::gl_constants::{GL_COLOR, GL_NO_ERROR};
use super::host::{DefaultBuffers, WebGLHost};
use super::object_map::ObjectMap;
use crate::host::WebContentClientId;

/// What WebContent sent that it must not send.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReplayError(pub &'static str);

/// A buffer that a synchronous call fills is never bigger than this.
const MAX_OUT_DATA_SIZE: usize = 64 * 1024 * 1024;

/// The payload of a record that is replayed where WebContent wrote it, of which only the struct was copied.
#[derive(Clone, Copy)]
pub struct PayloadInSharedMemory {
    pub start: *const u8,
    pub size: usize,
}

/// Data that only GL reads, which can be in memory that WebContent writes.
#[derive(Clone, Copy)]
pub(super) struct OpaqueData {
    pub pointer: *const u8,
    pub size: usize,
}

impl OpaqueData {
    pub const EMPTY: Self = Self {
        pointer: core::ptr::null(),
        size: 0,
    };
}

pub struct Replayer<'a, H: WebGLHost> {
    pub gl: &'a GlFunctions,
    pub host: &'a H,
    pub client: WebContentClientId,
    pub canvas_id: CanvasId,
    pub webgl2: bool,
    pub objects: &'a mut ObjectMap,
    pub stashed_errors: &'a mut Vec<u32>,
    pub defaults: DefaultBuffers,
    pub image_sources: &'a [ImageSource],
    pub payload_in_shared_memory: Option<PayloadInSharedMemory>,
}

impl<H: WebGLHost> Replayer<'_, H> {
    pub fn replay_commands(&mut self, commands: &[u8]) -> Result<(), ReplayError> {
        for record in records(commands) {
            let (command_type, payload) = record.map_err(ReplayError)?;
            self.replay_record(command_type, payload)?;
        }
        Ok(())
    }

    pub fn handle_sync_request(&mut self, request: &[u8]) -> Result<Vec<u8>, ReplayError> {
        let (call_type, payload) = decode_sync_request(request).map_err(ReplayError)?;
        self.handle_sync_call(call_type, payload)
    }

    /// The errors that the Compositor's own GL work raised are kept apart, for WebGL to collect before those of GL.
    pub fn get_error(&mut self) -> u32 {
        if self.stashed_errors.is_empty() {
            // SAFETY: The context of the replayer is current.
            return unsafe { (self.gl.get_error)() };
        }
        self.stashed_errors.remove(0)
    }

    /// Answers whether GL had errors to stash.
    pub fn stash_errors(&mut self) -> bool {
        let mut stashed_any_errors = false;
        loop {
            // SAFETY: The context of the replayer is current.
            let error = unsafe { (self.gl.get_error)() };
            if error == GL_NO_ERROR {
                return stashed_any_errors;
            }
            if !self.stashed_errors.contains(&error) {
                self.stashed_errors.push(error);
            }
            stashed_any_errors = true;
        }
    }

    pub(super) fn opaque_data(&self, payload: &[u8], span: DataSpan) -> Result<OpaqueData, ReplayError> {
        let Some(payload_in_shared_memory) = self.payload_in_shared_memory else {
            let bytes = data_span(payload, span)?;
            return Ok(OpaqueData {
                pointer: bytes.as_ptr(),
                size: bytes.len(),
            });
        };
        let start = span.offset as usize;
        start
            .checked_add(span.size as usize)
            .filter(|&end| end <= payload_in_shared_memory.size)
            .ok_or(ReplayError("WebGL data span lies outside its payload"))?;
        Ok(OpaqueData {
            // SAFETY: The span lies inside the payload, which stays mapped while the record is replayed.
            pointer: unsafe { payload_in_shared_memory.start.add(start) },
            size: span.size as usize,
        })
    }

    pub fn discard_errors(&mut self) {
        loop {
            // SAFETY: The context of the replayer is current.
            let error = unsafe { (self.gl.get_error)() };
            if error == GL_NO_ERROR {
                return;
            }
            eprintln!("Compositor: Discarding GL error {error:#x} raised by the Compositor's own work");
        }
    }
}

pub(super) fn decode<S: WireStruct>(payload: &[u8]) -> Result<S, ReplayError> {
    S::read_from(payload).ok_or(ReplayError("WebGL payload does not hold its struct"))
}

pub(super) fn data_span(payload: &[u8], span: DataSpan) -> Result<&[u8], ReplayError> {
    let start = span.offset as usize;
    start
        .checked_add(span.size as usize)
        .and_then(|end| payload.get(start..end))
        .ok_or(ReplayError("WebGL data span lies outside its payload"))
}

pub(super) fn typed_data<T>(bytes: &[u8]) -> Result<*const T, ReplayError> {
    typed_opaque_data(OpaqueData {
        pointer: bytes.as_ptr(),
        size: bytes.len(),
    })
}

pub(super) fn typed_opaque_data<T>(data: OpaqueData) -> Result<*const T, ReplayError> {
    if !data.pointer.cast::<T>().is_aligned() || !data.size.is_multiple_of(size_of::<T>()) {
        return Err(ReplayError("WebGL data does not hold whole values"));
    }
    Ok(data.pointer.cast())
}

/// A string that ends at the last byte of its span.
pub(super) fn string_data(payload: &[u8], span: DataSpan) -> Result<*const c_char, ReplayError> {
    let bytes = data_span(payload, span)?;
    if bytes.last() != Some(&0) {
        return Err(ReplayError("WebGL string is not NUL-terminated"));
    }
    Ok(bytes.as_ptr().cast())
}

pub(super) fn object_ids(bytes: &[u8]) -> Result<impl ExactSizeIterator<Item = u32>, ReplayError> {
    let (ids, rest) = bytes.as_chunks::<{ size_of::<u32>() }>();
    if !rest.is_empty() {
        return Err(ReplayError("WebGL object ids do not fill their span"));
    }
    Ok(ids.iter().map(|id| u32::from_ne_bytes(*id)))
}

fn product(factors: &[i64]) -> Option<i64> {
    factors
        .iter()
        .try_fold(1_i64, |product, &factor| product.checked_mul(factor))
}

/// Whether data is as big as the factors that its arguments give.
pub(super) fn size_matches(size: usize, factors: &[i64]) -> bool {
    product(factors).is_some_and(|expected| i64::try_from(size) == Ok(expected))
}

/// The size of a buffer that a synchronous call fills, which its arguments give.
pub(super) fn out_data_size(factors: &[i64]) -> Result<usize, ReplayError> {
    product(factors)
        .and_then(|size| usize::try_from(size).ok())
        .filter(|&size| size <= MAX_OUT_DATA_SIZE)
        .ok_or(ReplayError("WebGL asks for a buffer of a size that it cannot have"))
}

pub(super) fn bytes_of_slice<T: Copy>(values: &[T]) -> &[u8] {
    // SAFETY: The values are plain numbers or pointers, every byte of which is initialized.
    unsafe { ak::slice_from_raw_parts(values.as_ptr().cast(), size_of_val(values)) }
}

/// Splits `count` strings that follow each other, each with a NUL after it.
pub(super) fn split_packed_strings(bytes: &[u8], count: i32) -> Result<Vec<*const c_char>, ReplayError> {
    const MAX_STRING_LIST_ENTRIES: i32 = 16384;
    if !(0..=MAX_STRING_LIST_ENTRIES).contains(&count) {
        return Err(ReplayError("WebGL string list is too long"));
    }
    let mut strings = Vec::with_capacity(count as usize);
    let mut rest = bytes;
    for _ in 0..count {
        let end = rest
            .iter()
            .position(|&byte| byte == 0)
            .ok_or(ReplayError("WebGL string is not NUL-terminated"))?;
        strings.push(rest.as_ptr().cast());
        rest = &rest[end + 1..];
    }
    Ok(strings)
}

include!(concat!(env!("OUT_DIR"), "/webgl_replayer.rs"));
