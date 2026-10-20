/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The recorder of one WebGL context: it takes the place of GL for the implementation of the context, writes what
//! is asked of it as records into a buffer that it shares with the replayer, and publishes them in batches. A call
//! that returns something is a synchronous round trip, which everything recorded before it is flushed for.

use core::cell::{Cell, RefCell};
use core::ptr;
use core::sync::atomic::{AtomicU64, Ordering};
use std::rc::Rc;

use super::transport::{SharedMemory, Transport};
use super::vocabulary::{CommandType, MAX_COMMAND_STRUCT_SIZE};
use super::wire::{
    Command, DataSpan, HEADER_SIZE, InlineData, Record, SyncReply, SyncRequest, encode_sync_request,
    padded_record_size, resolve_data_span,
};
use crate::web_gl::constants::GL_OUT_OF_MEMORY;

/// The `u64` that the replayer stores the sequence number of the last executed flush in is at the start of the shared
/// command buffer, before the region of records.
pub(crate) const SHARED_DATA_REGION_OFFSET: usize = 128;
pub(crate) const DEFAULT_SHARED_DATA_REGION_CAPACITY: usize = 32 * 1024 * 1024;

pub(crate) const MAX_PENDING_COMMAND_BYTES: usize = 4 * 1024 * 1024;

/// What is kept free in the region of records for the command that names a frame, before the frame is added to the
/// pending ones. Making room only when the command is recorded could flush in between, and that flush would publish
/// the frame with the wrong batch.
const RESERVED_RECORD_SIZE_FOR_FRAME_COMMAND: usize = 256;
const _: () = assert!(HEADER_SIZE + MAX_COMMAND_STRUCT_SIZE <= RESERVED_RECORD_SIZE_FOR_FRAME_COMMAND);

pub(crate) struct Proxy<T: Transport> {
    transport: RefCell<Rc<T>>,
    lost: Cell<bool>,
    pending_local_error: Cell<u32>,
    next_object_id: Cell<u32>,

    // NB: A call of the transport can mark the context lost, which calls back into the recorder. So nothing below is
    //     borrowed across one: a number is in a Cell, and a vector is taken out of its Cell for as long as it is used.
    shared_command_buffer: RefCell<Option<T::SharedMemory>>,
    /// The start of the shared command buffer, which `shared_command_buffer` keeps mapped, or null without one.
    shared_command_buffer_start: Cell<*mut u8>,
    shared_data_capacity: Cell<usize>,
    wanted_shared_data_capacity: usize,
    shared_data_cursor: Cell<usize>,
    /// Where the records that are not published yet start.
    shared_data_flush_base: Cell<usize>,
    last_published_flush_sequence_number: Cell<u64>,

    /// The records of a context that could not get a shared command buffer.
    out_of_line_commands: Cell<Vec<u8>>,
    pending_frames: Cell<Vec<T::Frame>>,
    sync_request_bytes: Cell<Vec<u8>>,
    sync_reply_bytes: Cell<Vec<u8>>,
    /// By GL name. The strings do not change while a context lives.
    string_cache: RefCell<Vec<(u32, Rc<[u8]>)>>,
    max_frames_per_message: Cell<usize>,

    transport_calls_are_deferred: Cell<bool>,
    deferred_record_count: Cell<usize>,
    deferred_flush_is_due: Cell<bool>,
    /// The record that had no room at the cursor while the calls of the transport that make room were deferred.
    deferred_record: Cell<Vec<u8>>,
}

impl<T: Transport> Proxy<T> {
    pub(crate) fn new(transport: T) -> Self {
        Self::with_shared_data_capacity(transport, DEFAULT_SHARED_DATA_REGION_CAPACITY)
    }

    pub(super) fn with_shared_data_capacity(transport: T, shared_data_capacity: usize) -> Self {
        let proxy = Self {
            max_frames_per_message: Cell::new(transport.max_frames_per_message()),
            transport: RefCell::new(Rc::new(transport)),
            lost: Cell::new(false),
            pending_local_error: Cell::new(0),
            next_object_id: Cell::new(1),
            shared_command_buffer: RefCell::new(None),
            shared_command_buffer_start: Cell::new(ptr::null_mut()),
            shared_data_capacity: Cell::new(0),
            wanted_shared_data_capacity: shared_data_capacity,
            shared_data_cursor: Cell::new(0),
            shared_data_flush_base: Cell::new(0),
            last_published_flush_sequence_number: Cell::new(0),
            out_of_line_commands: Cell::new(Vec::new()),
            pending_frames: Cell::new(Vec::new()),
            sync_request_bytes: Cell::new(Vec::new()),
            sync_reply_bytes: Cell::new(Vec::new()),
            string_cache: RefCell::new(Vec::new()),
            transport_calls_are_deferred: Cell::new(false),
            deferred_record_count: Cell::new(0),
            deferred_flush_is_due: Cell::new(false),
            deferred_record: Cell::new(Vec::new()),
        };
        proxy.initialize_shared_command_buffer();
        proxy
    }

    /// Nothing that was recorded for the lost context reaches the new one.
    pub(crate) fn restore(&self, transport: T) {
        self.max_frames_per_message.set(transport.max_frames_per_message());
        self.transport.replace(Rc::new(transport));
        self.lost.set(false);
        self.out_of_line_commands.take();
        self.pending_frames.take();
        self.string_cache.borrow_mut().clear();
        self.initialize_shared_command_buffer();
    }

    pub(super) fn transport(&self) -> Rc<T> {
        assert!(
            !self.transport_calls_are_deferred.get(),
            "the WebGL transport is not called while the data of a command is borrowed"
        );
        Rc::clone(&self.transport.borrow())
    }

    fn initialize_shared_command_buffer(&self) {
        self.shared_data_cursor.set(0);
        self.shared_data_flush_base.set(0);
        self.last_published_flush_sequence_number.set(0);

        let transport = self.transport();
        let buffer = transport.create_shared_memory(SHARED_DATA_REGION_OFFSET + self.wanted_shared_data_capacity);
        match &buffer {
            Some(buffer) => {
                assert!(buffer.len() > SHARED_DATA_REGION_OFFSET);
                assert!(buffer.as_ptr().cast::<AtomicU64>().is_aligned());
                transport.set_shared_command_buffer(buffer);
                self.shared_command_buffer_start.set(buffer.as_ptr());
                self.shared_data_capacity.set(buffer.len() - SHARED_DATA_REGION_OFFSET);
            }
            None => {
                self.shared_command_buffer_start.set(ptr::null_mut());
                self.shared_data_capacity.set(0);
            }
        }
        // NB: The pointer above is into the new buffer already, so the old one is not used once it is unmapped here.
        self.shared_command_buffer.replace(buffer);
    }

    fn has_shared_command_buffer(&self) -> bool {
        !self.shared_command_buffer_start.get().is_null()
    }

    fn executed_flush_sequence_number(&self) -> u64 {
        // SAFETY: Only called with a shared command buffer, which is mapped, larger than a u64 and aligned for one.
        //         The replayer writes the number as an atomic as well.
        unsafe { &*self.shared_command_buffer_start.get().cast::<AtomicU64>() }.load(Ordering::Acquire)
    }

    pub(crate) fn set_lost(&self) {
        self.lost.set(true);
    }

    pub(crate) fn is_lost(&self) -> bool {
        self.lost.get()
    }

    pub(crate) fn canvas_id(&self) -> Option<u64> {
        self.transport().canvas_id()
    }

    pub(super) fn allocate_object_id(&self) -> u32 {
        let id = self.next_object_id.get();
        self.next_object_id.set(id + 1);
        id
    }

    pub(super) fn set_pending_local_error(&self, error: u32) {
        if self.pending_local_error.get() == 0 {
            self.pending_local_error.set(error);
        }
    }

    pub(crate) fn take_pending_local_error(&self) -> u32 {
        self.pending_local_error.replace(0)
    }

    #[inline]
    pub(super) fn record<C: Command>(&self, command: &C) {
        self.record_bytes(C::TYPE, command.as_bytes(), InlineData::NONE);
    }

    #[inline]
    pub(super) fn record_with_data<C: Command>(&self, command: &C, inline_data: InlineData<'_>) {
        self.record_bytes(C::TYPE, command.as_bytes(), inline_data);
    }

    fn record_bytes(&self, command_type: CommandType, payload: &[u8], inline_data: InlineData<'_>) {
        if self.lost.get() {
            return;
        }
        let transport_calls_are_deferred = self.transport_calls_are_deferred.get();
        if transport_calls_are_deferred {
            // NB: What is deferred runs once the record is made. After a second record it would run in the wrong
            //     order: a record that had no room after a record that was made after it.
            assert!(
                self.deferred_record_count.replace(1) == 0,
                "one command is recorded with borrowed data"
            );
        }
        let Some(record) = Record::new(command_type, payload, inline_data) else {
            self.set_pending_local_error(GL_OUT_OF_MEMORY);
            return;
        };

        if !self.has_shared_command_buffer() {
            let mut commands = self.out_of_line_commands.take();
            record.append_to(&mut commands);
            let flush_is_due = commands.len() >= MAX_PENDING_COMMAND_BYTES;
            self.out_of_line_commands.set(commands);
            if flush_is_due {
                self.flush_commands_or_defer();
            }
            return;
        }

        if transport_calls_are_deferred {
            self.rewind_shared_data_cursor_if_all_published_commands_executed();
            if record.size() > self.shared_data_capacity.get() - self.shared_data_cursor.get() {
                self.deferred_record.set(record.encoded());
                return;
            }
        } else if record.size() > self.shared_data_capacity.get() {
            self.send_oversized_record(&record.encoded());
            return;
        } else {
            self.rewind_shared_data_cursor_if_all_published_commands_executed();
            self.ensure_shared_data_capacity(record.size());
            if self.lost.get() {
                return;
            }
        }

        // SAFETY: The destination has room for the record, which was just made sure of, and neither the payload nor
        //         the data is in the region of records.
        self.write_at_shared_data_cursor(record.size(), |destination| unsafe { record.write_to(destination) });
    }

    fn record_encoded(&self, record: &[u8]) {
        if self.lost.get() {
            return;
        }
        if record.len() > self.shared_data_capacity.get() {
            self.send_oversized_record(record);
            return;
        }
        self.rewind_shared_data_cursor_if_all_published_commands_executed();
        self.ensure_shared_data_capacity(record.len());
        if self.lost.get() {
            return;
        }
        // SAFETY: The destination has room for the record, which was just made sure of, and the record is not in
        //         the region of records.
        self.write_at_shared_data_cursor(record.len(), |destination| unsafe {
            ptr::copy_nonoverlapping(record.as_ptr(), destination, record.len());
        });
    }

    fn write_at_shared_data_cursor(&self, record_size: usize, write_record_to: impl FnOnce(*mut u8)) {
        let cursor = self.shared_data_cursor.get();
        assert!(record_size <= self.shared_data_capacity.get() - cursor);
        // SAFETY: The region of records is mapped and has the bytes from the cursor on. The replayer does not read
        //         the region past what was published.
        let destination = unsafe {
            self.shared_command_buffer_start
                .get()
                .add(SHARED_DATA_REGION_OFFSET + cursor)
        };
        write_record_to(destination);
        let cursor = cursor + record_size;
        self.shared_data_cursor.set(cursor);
        if cursor - self.shared_data_flush_base.get() >= MAX_PENDING_COMMAND_BYTES {
            self.flush_commands_or_defer();
        }
    }

    fn flush_commands_or_defer(&self) {
        if self.transport_calls_are_deferred.get() {
            self.deferred_flush_is_due.set(true);
        } else {
            self.flush_commands();
        }
    }

    fn send_oversized_record(&self, record: &[u8]) {
        self.flush_commands();
        self.transport().send_commands(record, &[]);
    }

    /// Runs `record`, which records one command with data that is borrowed from memory of script, without a call of
    /// the transport while it runs, as nothing else may run while the data is borrowed: the calls that the record
    /// needs are made before and after it.
    ///
    /// `data_size_hint` is how many bytes of data the command is expected to carry. Room for a record with as many is
    /// made before the data is borrowed. A record that there is no room for then, whatever the hint was, is copied,
    /// and the copy is put into the stream once the data is given back.
    pub(crate) fn record_with_borrowed_data<R>(&self, data_size_hint: usize, record: impl FnOnce() -> R) -> R {
        if !self.lost.get() && self.has_shared_command_buffer() {
            // NB: Which command it is going to be is not known here, so room is made for the largest struct. A
            //     record that the region is too small for even with the smallest struct is sent on its own, and
            //     needs no room.
            let capacity = self.shared_data_capacity.get();
            let smallest_record_size = padded_record_size(1, data_size_hint);
            let largest_record_size = data_size_hint
                .checked_add(1)
                .and_then(|data_size| padded_record_size(MAX_COMMAND_STRUCT_SIZE, data_size));
            if let (Some(smallest_record_size), Some(largest_record_size)) = (smallest_record_size, largest_record_size)
                && smallest_record_size <= capacity
            {
                self.rewind_shared_data_cursor_if_all_published_commands_executed();
                self.ensure_shared_data_capacity(largest_record_size.min(capacity));
            }
        }

        assert!(!self.transport_calls_are_deferred.replace(true));
        self.deferred_record_count.set(0);
        let result = {
            let _deferral = EndOfDeferral(&self.transport_calls_are_deferred);
            record()
        };

        let deferred_record = self.deferred_record.take();
        if !deferred_record.is_empty() {
            self.record_encoded(&deferred_record);
        }
        if self.deferred_flush_is_due.replace(false) {
            self.flush_commands();
        }
        result
    }

    fn rewind_shared_data_cursor_if_all_published_commands_executed(&self) {
        let cursor = self.shared_data_cursor.get();
        if cursor == 0 || cursor != self.shared_data_flush_base.get() {
            return;
        }
        if self.executed_flush_sequence_number() >= self.last_published_flush_sequence_number.get() {
            self.shared_data_cursor.set(0);
            self.shared_data_flush_base.set(0);
        }
    }

    /// When the region is full, everything in it is published and the recorder waits for the replayer to have
    /// executed it, which loses the context if the replayer never will.
    fn ensure_shared_data_capacity(&self, record_size: usize) {
        let capacity = self.shared_data_capacity.get();
        assert!(record_size <= capacity);
        if record_size <= capacity - self.shared_data_cursor.get() {
            return;
        }

        self.flush_commands();
        assert!(self.shared_data_cursor.get() == self.shared_data_flush_base.get());
        if self.executed_flush_sequence_number() < self.last_published_flush_sequence_number.get() {
            if !self.transport().wait_until_published_commands_executed() {
                self.set_lost();
                return;
            }
            assert!(self.executed_flush_sequence_number() >= self.last_published_flush_sequence_number.get());
        }
        self.shared_data_cursor.set(0);
        self.shared_data_flush_base.set(0);
    }

    pub(crate) fn flush_commands(&self) {
        if self.has_shared_command_buffer() {
            let cursor = self.shared_data_cursor.get();
            let flush_base = self.shared_data_flush_base.get();
            if cursor == flush_base {
                return;
            }
            let flush_sequence_number = self.last_published_flush_sequence_number.get() + 1;
            self.last_published_flush_sequence_number.set(flush_sequence_number);
            self.shared_data_flush_base.set(cursor);
            let frames = self.pending_frames.take();
            self.transport().send_commands_from_shared_buffer(
                flush_base as u64,
                (cursor - flush_base) as u64,
                flush_sequence_number,
                &frames,
            );
            self.keep_capacity_of_pending_frames(frames);
            return;
        }

        let mut commands = self.out_of_line_commands.take();
        if commands.is_empty() {
            return;
        }
        let frames = self.pending_frames.take();
        self.transport().send_commands(&commands, &frames);
        commands.clear();
        self.out_of_line_commands.set(commands);
        self.keep_capacity_of_pending_frames(frames);
    }

    fn keep_capacity_of_pending_frames(&self, mut sent_frames: Vec<T::Frame>) {
        sent_frames.clear();
        self.pending_frames.set(sent_frames);
    }

    /// Returns the index that the command which uses the frame names it by.
    pub(super) fn append_pending_frame(&self, frame: T::Frame) -> u32 {
        if self.with_pending_frames(|frames| frames.len()) >= self.max_frames_per_message.get() {
            self.flush_commands();
        }
        if self.has_shared_command_buffer() {
            self.ensure_shared_data_capacity(RESERVED_RECORD_SIZE_FOR_FRAME_COMMAND);
        }
        self.with_pending_frames(|frames| {
            let frame_index = u32::try_from(frames.len()).expect("the frames of a message are fewer than 4 billion");
            frames.push(frame);
            frame_index
        })
    }

    fn with_pending_frames<O>(&self, use_frames: impl FnOnce(&mut Vec<T::Frame>) -> O) -> O {
        let mut frames = self.pending_frames.take();
        let output = use_frames(&mut frames);
        self.pending_frames.set(frames);
        output
    }

    /// `None` when the context is lost before or by the call, and when the stream cannot carry the request, which is
    /// a GL_OUT_OF_MEMORY error of the context.
    pub(super) fn sync_call<R: SyncRequest, O>(
        &self,
        request: &R,
        inline_data: InlineData<'_>,
        read_reply: impl FnOnce(R::Reply, &[u8]) -> O,
    ) -> Option<O> {
        if self.lost.get() {
            return None;
        }
        self.flush_commands();

        let mut request_bytes = self.sync_request_bytes.take();
        let mut reply_bytes = self.sync_reply_bytes.take();
        let output = if encode_sync_request(&mut request_bytes, request, inline_data) {
            reply_bytes.clear();
            self.transport().sync_call(&request_bytes, &mut reply_bytes);
            if reply_bytes.is_empty() {
                self.set_lost();
            }
            (!self.lost.get()).then(|| read_reply(R::Reply::from_reply_bytes(&reply_bytes), reply_bytes.as_slice()))
        } else {
            self.set_pending_local_error(GL_OUT_OF_MEMORY);
            None
        };
        self.sync_request_bytes.set(request_bytes);
        self.sync_reply_bytes.set(reply_bytes);
        output
    }

    pub(super) fn cached_string(&self, name: u32) -> Option<Rc<[u8]>> {
        let string_cache = self.string_cache.borrow();
        let (_, string) = string_cache.iter().find(|(cached_name, _)| *cached_name == name)?;
        Some(Rc::clone(string))
    }

    pub(super) fn cache_string(&self, name: u32, string: Rc<[u8]>) {
        self.string_cache.borrow_mut().push((name, string));
    }

    pub(crate) fn present_canvas_for_compositing(&self, preserve_drawing_buffer: bool) {
        self.flush_commands();
        self.transport().present_canvas(preserve_drawing_buffer);
    }

    pub(crate) fn clear_drawing_buffer(&self) {
        if self.lost.get() {
            return;
        }
        self.flush_commands();
        self.transport().clear_drawing_buffer();
    }

    pub(crate) fn read_back_drawing_buffer(&self, x: i32, y: i32, width: i32, height: i32) -> Option<T::Bitmap> {
        if self.lost.get() {
            return None;
        }
        self.flush_commands();
        self.transport().read_back_drawing_buffer(x, y, width, height)
    }
}

/// Ends the deferral of the calls of the transport however the record ends, unwinding included.
struct EndOfDeferral<'a>(&'a Cell<bool>);

impl Drop for EndOfDeferral<'_> {
    fn drop(&mut self) {
        self.0.set(false);
    }
}

impl<T: Transport> Drop for Proxy<T> {
    fn drop(&mut self) {
        self.transport().destroy_context();
    }
}

/// The product of the factors that GLFunctions.json gives for the size of a pointer argument.
pub(super) fn payload_size(factors: &[i64]) -> usize {
    factors
        .iter()
        .try_fold(1usize, |size, &factor| size.checked_mul(usize::try_from(factor).ok()?))
        .expect("the size of the data of a WebGL command is validated before the command is recorded")
}

pub(super) fn leading_bytes(bytes: &[u8], size: usize) -> &[u8] {
    bytes
        .get(..size)
        .expect("the data of a WebGL command is as large as the command says")
}

pub(super) fn leading_bytes_mut(bytes: &mut [u8], size: usize) -> &mut [u8] {
    bytes
        .get_mut(..size)
        .expect("the buffer for the output of a WebGL call is as large as the call says")
}

pub(super) fn gen_count(ids: &[u32]) -> i32 {
    i32::try_from(ids.len()).expect("the objects that one WebGL command creates are counted by a GLsizei")
}

/// The replayer ends a string at its first NUL.
pub(super) fn until_nul(string: &[u8]) -> &[u8] {
    let length = string.iter().position(|&byte| byte == 0).unwrap_or(string.len());
    &string[..length]
}

/// # Safety
///
/// The type has no padding, and every bit pattern is a value of it.
pub(crate) unsafe trait PlainNumber: Copy + 'static {}
// SAFETY: The types are primitive numbers.
unsafe impl PlainNumber for u8 {}
// SAFETY: As above.
unsafe impl PlainNumber for i32 {}
// SAFETY: As above.
unsafe impl PlainNumber for u32 {}
// SAFETY: As above.
unsafe impl PlainNumber for f32 {}
// SAFETY: As above.
unsafe impl PlainNumber for i64 {}

pub(crate) fn bytes_of<N: PlainNumber>(numbers: &[N]) -> &[u8] {
    // SAFETY: Per the contract of PlainNumber the numbers are their bytes, all of which are initialized.
    unsafe { ak::slice_from_raw_parts(numbers.as_ptr().cast::<u8>(), size_of_val(numbers)) }
}

pub(super) fn bytes_of_mut<N: PlainNumber>(numbers: &mut [N]) -> &mut [u8] {
    // SAFETY: Per the contract of PlainNumber the numbers are their bytes, and any bytes written are numbers.
    unsafe { ak::slice_from_raw_parts_mut(numbers.as_mut_ptr().cast::<u8>(), size_of_val(numbers)) }
}

pub(super) fn copy_reply_data(reply: &[u8], span: DataSpan, destination: &mut [u8]) {
    let data = resolve_data_span(reply, span);
    assert!(
        data.len() <= destination.len(),
        "the replayer returns no more data than a WebGL call asks for"
    );
    destination[..data.len()].copy_from_slice(data);
}
