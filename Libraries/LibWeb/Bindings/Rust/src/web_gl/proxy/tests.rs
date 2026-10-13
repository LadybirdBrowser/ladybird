/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The recorder against a transport that plays the replayer: it logs what it is called with, and reads the region of
//! records the way the replayer does when a range of it is published.

use core::cell::{Cell, RefCell, UnsafeCell};
use core::sync::atomic::{AtomicU64, Ordering};
use std::collections::VecDeque;
use std::rc::{Rc, Weak};

use super::custom::FrameUpload;
use super::generated::GetActiveAttribOutputs;
use super::recorder::{MAX_PENDING_COMMAND_BYTES, Proxy, SHARED_DATA_REGION_OFFSET, bytes_of};
use super::transport::{ReadPixelsResult, SharedMemory, Transport};
use super::vocabulary::{CommandType, SyncCallType, commands, sync_calls};
use super::wire::{DataSpan, HEADER_SIZE, WireStruct};
use crate::web_gl::constants::{GL_ARRAY_BUFFER, GL_INVALID_ENUM, GL_OUT_OF_MEMORY, GL_RGBA, GL_UNSIGNED_BYTE};

#[derive(Clone)]
struct MockMemory {
    words: Rc<[UnsafeCell<u64>]>,
    size: usize,
}

impl MockMemory {
    fn new(size: usize) -> Self {
        Self {
            words: (0..size.div_ceil(8)).map(|_| UnsafeCell::new(0)).collect(),
            size,
        }
    }

    fn bytes(&self, offset: usize, size: usize) -> Vec<u8> {
        assert!(offset + size <= self.size);
        // SAFETY: The range is inside the memory, which nothing else reads or writes while the test runs this.
        unsafe { core::slice::from_raw_parts(self.as_ptr().add(offset), size) }.to_vec()
    }

    fn fill(&self, bytes: &[u8]) {
        // SAFETY: As above.
        unsafe { core::ptr::copy_nonoverlapping(bytes.as_ptr(), self.as_ptr(), bytes.len().min(self.size)) };
    }
}

impl SharedMemory for MockMemory {
    fn as_ptr(&self) -> *mut u8 {
        UnsafeCell::raw_get(self.words.as_ptr()).cast::<u8>()
    }

    fn len(&self) -> usize {
        self.size
    }
}

#[derive(Clone, Debug, PartialEq)]
struct Record {
    command_type: u16,
    payload: Vec<u8>,
}

fn records_in(mut bytes: &[u8]) -> Vec<Record> {
    let mut records = Vec::new();
    while !bytes.is_empty() {
        let command_type = u16::from_ne_bytes([bytes[0], bytes[1]]);
        let payload_size = u32::from_ne_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]) as usize;
        let (record, rest) = bytes[HEADER_SIZE..].split_at(payload_size);
        records.push(Record {
            command_type,
            payload: record.to_vec(),
        });
        bytes = rest;
    }
    records
}

fn record_of<C: super::wire::Command>(command: &C) -> Record {
    let mut payload = command.as_bytes().to_vec();
    payload.resize((HEADER_SIZE + payload.len()).next_multiple_of(16) - HEADER_SIZE, 0);
    Record {
        command_type: C::TYPE as u16,
        payload,
    }
}

#[derive(Clone, Debug, PartialEq)]
enum Call {
    DestroyContext,
    SetSharedCommandBuffer {
        size: usize,
    },
    SendFromSharedBuffer {
        offset: u64,
        records: Vec<Record>,
        flush_sequence_number: u64,
        frames: Vec<u32>,
    },
    WaitUntilPublishedCommandsExecuted,
    SendCommands {
        records: Vec<Record>,
        frames: Vec<u32>,
    },
    PresentCanvas {
        preserve_drawing_buffer: bool,
    },
    ClearDrawingBuffer,
    Synchronous {
        request: Vec<u8>,
    },
    ReadPixels {
        buf_size: i32,
        has_pixels: bool,
    },
    ReadBufferSubData {
        target: u32,
        offset: i64,
        size: i64,
    },
    ReadBackDrawingBuffer,
}

struct MockReplayer {
    calls: RefCell<Vec<Call>>,
    shared_command_buffer: RefCell<Option<MockMemory>>,
    last_published_flush_sequence_number: Cell<u64>,
    refuses_shared_memory: Cell<bool>,
    executes_at_once: Cell<bool>,
    wait_succeeds: Cell<bool>,
    max_frames_per_message: Cell<usize>,
    sync_replies: RefCell<VecDeque<Vec<u8>>>,
    read_data: RefCell<Vec<u8>>,
    read_pixels_result: Cell<ReadPixelsResult>,
    read_buffer_sub_data_succeeds: Cell<bool>,
    calls_are_forbidden: Cell<bool>,
    recorder_to_lose: RefCell<Weak<Proxy<MockTransport>>>,
}

impl MockReplayer {
    fn new() -> Rc<Self> {
        Rc::new(Self {
            calls: RefCell::new(Vec::new()),
            shared_command_buffer: RefCell::new(None),
            last_published_flush_sequence_number: Cell::new(0),
            refuses_shared_memory: Cell::new(false),
            executes_at_once: Cell::new(true),
            wait_succeeds: Cell::new(true),
            max_frames_per_message: Cell::new(16),
            sync_replies: RefCell::new(VecDeque::new()),
            read_data: RefCell::new(Vec::new()),
            read_pixels_result: Cell::new(ReadPixelsResult::default()),
            read_buffer_sub_data_succeeds: Cell::new(true),
            calls_are_forbidden: Cell::new(false),
            recorder_to_lose: RefCell::new(Weak::new()),
        })
    }

    fn recorder(self: &Rc<Self>, shared_data_capacity: usize) -> Proxy<MockTransport> {
        Proxy::with_shared_data_capacity(
            MockTransport {
                replayer: Rc::clone(self),
            },
            shared_data_capacity,
        )
    }

    fn called(&self, call: Call) {
        assert!(
            !self.calls_are_forbidden.get(),
            "{call:?} while the calls of the transport are deferred"
        );
        self.calls.borrow_mut().push(call);
        // NB: This is the host finding the replayer gone in the middle of a call.
        let recorder_to_lose = self.recorder_to_lose.borrow().upgrade();
        if let Some(recorder) = recorder_to_lose {
            recorder.set_lost();
        }
    }

    fn take_calls(&self) -> Vec<Call> {
        self.calls.take()
    }

    /// The calls are allowed again even when the function panics, as the recorder calls the transport when it is
    /// dropped.
    fn forbidding_calls<R>(&self, function: impl FnOnce() -> R) -> R {
        struct CallsAreAllowedAgain<'a>(&'a Cell<bool>);
        impl Drop for CallsAreAllowedAgain<'_> {
            fn drop(&mut self) {
                self.0.set(false);
            }
        }

        self.calls_are_forbidden.set(true);
        let _calls_are_allowed_again = CallsAreAllowedAgain(&self.calls_are_forbidden);
        function()
    }

    fn bytes_of_region(&self, offset: usize, size: usize) -> Vec<u8> {
        let shared_command_buffer = self.shared_command_buffer.borrow();
        shared_command_buffer
            .as_ref()
            .expect("a shared command buffer")
            .bytes(SHARED_DATA_REGION_OFFSET + offset, size)
    }

    fn store_executed_flush_sequence_number(&self, flush_sequence_number: u64) {
        let shared_command_buffer = self.shared_command_buffer.borrow();
        let start = shared_command_buffer
            .as_ref()
            .expect("a shared command buffer")
            .as_ptr();
        // SAFETY: The memory is larger than a u64 and aligned for one.
        unsafe { &*start.cast::<AtomicU64>() }.store(flush_sequence_number, Ordering::Release);
    }

    fn queue_reply<R: WireStruct>(&self, reply: &R, inline_data: &[u8]) {
        let mut bytes = reply.as_bytes().to_vec();
        if !inline_data.is_empty() {
            bytes.resize(bytes.len().next_multiple_of(16), 0);
            bytes.extend_from_slice(inline_data);
        }
        bytes.resize(bytes.len().next_multiple_of(16), 0);
        self.sync_replies.borrow_mut().push_back(bytes);
    }
}

struct MockTransport {
    replayer: Rc<MockReplayer>,
}

impl Transport for MockTransport {
    type SharedMemory = MockMemory;
    type Frame = u32;
    type Bitmap = (i32, i32, i32, i32);

    fn max_frames_per_message(&self) -> usize {
        self.replayer.max_frames_per_message.get()
    }

    fn create_shared_memory(&self, size: usize) -> Option<MockMemory> {
        (!self.replayer.refuses_shared_memory.get()).then(|| MockMemory::new(size))
    }

    fn canvas_id(&self) -> Option<u64> {
        Some(42)
    }

    fn destroy_context(&self) {
        self.replayer.called(Call::DestroyContext);
    }

    fn set_shared_command_buffer(&self, buffer: &MockMemory) {
        self.replayer.shared_command_buffer.replace(Some(buffer.clone()));
        self.replayer.last_published_flush_sequence_number.set(0);
        self.replayer
            .called(Call::SetSharedCommandBuffer { size: buffer.len() });
    }

    fn send_commands_from_shared_buffer(
        &self,
        offset: u64,
        size_in_bytes: u64,
        flush_sequence_number: u64,
        frames: &[u32],
    ) {
        let published = {
            let shared_command_buffer = self.replayer.shared_command_buffer.borrow();
            let shared_command_buffer = shared_command_buffer.as_ref().expect("a shared command buffer");
            shared_command_buffer.bytes(SHARED_DATA_REGION_OFFSET + offset as usize, size_in_bytes as usize)
        };
        self.replayer
            .last_published_flush_sequence_number
            .set(flush_sequence_number);
        if self.replayer.executes_at_once.get() {
            self.replayer
                .store_executed_flush_sequence_number(flush_sequence_number);
        }
        self.replayer.called(Call::SendFromSharedBuffer {
            offset,
            records: records_in(&published),
            flush_sequence_number,
            frames: frames.to_vec(),
        });
    }

    fn wait_until_published_commands_executed(&self) -> bool {
        let wait_succeeds = self.replayer.wait_succeeds.get();
        if wait_succeeds {
            let last_published = self.replayer.last_published_flush_sequence_number.get();
            self.replayer.store_executed_flush_sequence_number(last_published);
        }
        self.replayer.called(Call::WaitUntilPublishedCommandsExecuted);
        wait_succeeds
    }

    fn send_commands(&self, commands: &[u8], frames: &[u32]) {
        self.replayer.called(Call::SendCommands {
            records: records_in(commands),
            frames: frames.to_vec(),
        });
    }

    fn present_canvas(&self, preserve_drawing_buffer: bool) {
        self.replayer.called(Call::PresentCanvas {
            preserve_drawing_buffer,
        });
    }

    fn clear_drawing_buffer(&self) {
        self.replayer.called(Call::ClearDrawingBuffer);
    }

    fn sync_call(&self, request: &[u8], reply: &mut Vec<u8>) {
        if let Some(queued_reply) = self.replayer.sync_replies.borrow_mut().pop_front() {
            reply.extend_from_slice(&queued_reply);
        }
        self.replayer.called(Call::Synchronous {
            request: request.to_vec(),
        });
    }

    fn read_pixels_robust_angle(
        &self,
        _x: i32,
        _y: i32,
        _width: i32,
        _height: i32,
        _format: u32,
        _type: u32,
        buf_size: i32,
        pixels: Option<&MockMemory>,
    ) -> ReadPixelsResult {
        if let Some(pixels) = pixels {
            pixels.fill(&self.replayer.read_data.borrow());
        }
        self.replayer.called(Call::ReadPixels {
            buf_size,
            has_pixels: pixels.is_some(),
        });
        self.replayer.read_pixels_result.get()
    }

    fn read_buffer_sub_data(&self, target: u32, offset: i64, size: i64, data: &MockMemory) -> bool {
        data.fill(&self.replayer.read_data.borrow());
        self.replayer.called(Call::ReadBufferSubData { target, offset, size });
        self.replayer.read_buffer_sub_data_succeeds.get()
    }

    fn read_back_drawing_buffer(&self, x: i32, y: i32, width: i32, height: i32) -> Option<(i32, i32, i32, i32)> {
        self.replayer.called(Call::ReadBackDrawingBuffer);
        Some((x, y, width, height))
    }
}

const SMALL_CAPACITY: usize = 4096;
const VIEWPORT: commands::Viewport = commands::Viewport {
    x: 1,
    y: 2,
    width: 3,
    height: 4,
};
const VIEWPORT_RECORD_SIZE: usize = 32;

fn record_viewport(recorder: &Proxy<MockTransport>) {
    recorder.viewport(VIEWPORT.x, VIEWPORT.y, VIEWPORT.width, VIEWPORT.height);
}

fn sent_from_shared_buffer(offset: u64, records: Vec<Record>, flush_sequence_number: u64) -> Call {
    Call::SendFromSharedBuffer {
        offset,
        records,
        flush_sequence_number,
        frames: Vec::new(),
    }
}

const BUFFER_SUB_DATA_STRUCT_SIZE: usize = 32;

fn buffer_sub_data_record_size(data: &[u8]) -> usize {
    (HEADER_SIZE + BUFFER_SUB_DATA_STRUCT_SIZE + data.len()).next_multiple_of(16)
}

/// `data` is not empty.
fn buffer_sub_data_record(data: &[u8]) -> Record {
    let command = commands::BufferSubData {
        target: GL_ARRAY_BUFFER,
        size: data.len() as i64,
        data: DataSpan {
            offset: BUFFER_SUB_DATA_STRUCT_SIZE as u32,
            size: data.len() as u32,
        },
        ..commands::BufferSubData::ZEROED
    };
    let mut payload = command.as_bytes().to_vec();
    payload.extend_from_slice(data);
    payload.resize(buffer_sub_data_record_size(data) - HEADER_SIZE, 0);
    Record {
        command_type: CommandType::BufferSubData as u16,
        payload,
    }
}

fn record_borrowed_buffer_sub_data(
    replayer: &MockReplayer,
    recorder: &Proxy<MockTransport>,
    data_size_hint: usize,
    data: &[u8],
    check_while_data_is_borrowed: impl FnOnce(),
) {
    recorder.record_with_borrowed_data(data_size_hint, || {
        replayer.forbidding_calls(|| recorder.buffer_sub_data(GL_ARRAY_BUFFER, 0, data.len() as i64, data));
        check_while_data_is_borrowed();
    });
}

fn frame_upload() -> FrameUpload {
    FrameUpload {
        destination_size: None,
        flip_y: false,
        premultiply_alpha: false,
    }
}

fn upload_frame(recorder: &Proxy<MockTransport>, frame: u32) {
    recorder.tex_image2d_from_bitmap(1, 0, 3, GL_RGBA, GL_UNSIGNED_BYTE, frame, frame_upload());
}

fn frame_record(bitmap_index: u32) -> Record {
    record_of(&commands::TexImage2DFromBitmap {
        target: 1,
        internalformat: 3,
        format: GL_RGBA,
        r#type: GL_UNSIGNED_BYTE,
        bitmap_index,
        ..commands::TexImage2DFromBitmap::ZEROED
    })
}

#[test]
fn a_command_waits_in_the_shared_buffer_for_a_flush() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    assert_eq!(
        replayer.take_calls(),
        [Call::SetSharedCommandBuffer {
            size: SHARED_DATA_REGION_OFFSET + SMALL_CAPACITY
        }]
    );

    record_viewport(&recorder);
    recorder.enable(7);
    assert_eq!(replayer.take_calls(), []);

    recorder.flush_commands();
    let records = vec![record_of(&VIEWPORT), record_of(&commands::Enable { cap: 7 })];
    assert_eq!(replayer.take_calls(), [sent_from_shared_buffer(0, records, 1)]);

    recorder.flush_commands();
    assert_eq!(replayer.take_calls(), []);
}

#[test]
fn the_cursor_rewinds_once_everything_published_is_executed() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    replayer.executes_at_once.set(false);
    record_viewport(&recorder);
    recorder.flush_commands();
    record_viewport(&recorder);
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT)], 1),
            sent_from_shared_buffer(VIEWPORT_RECORD_SIZE as u64, vec![record_of(&VIEWPORT)], 2),
        ]
    );

    replayer.store_executed_flush_sequence_number(2);
    record_viewport(&recorder);
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(0, vec![record_of(&VIEWPORT)], 3)]
    );
}

#[test]
fn a_full_region_is_published_and_waited_for() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(3 * VIEWPORT_RECORD_SIZE);
    replayer.take_calls();
    replayer.executes_at_once.set(false);

    for _ in 0..3 {
        record_viewport(&recorder);
    }
    assert_eq!(replayer.take_calls(), []);

    recorder.enable(7);
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); 3], 1),
            Call::WaitUntilPublishedCommandsExecuted
        ]
    );
    assert!(!recorder.is_lost());

    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(
            0,
            vec![record_of(&commands::Enable { cap: 7 })],
            2
        )]
    );
}

#[test]
fn a_replayer_that_never_executes_what_was_published_loses_the_context() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(VIEWPORT_RECORD_SIZE);
    replayer.executes_at_once.set(false);
    replayer.wait_succeeds.set(false);

    record_viewport(&recorder);
    record_viewport(&recorder);
    assert!(recorder.is_lost());
    replayer.take_calls();

    record_viewport(&recorder);
    recorder.flush_commands();
    assert_eq!(replayer.take_calls(), []);
}

#[test]
fn pending_commands_are_published_at_the_threshold() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(4 * MAX_PENDING_COMMAND_BYTES);
    replayer.take_calls();

    let data = vec![5u8; MAX_PENDING_COMMAND_BYTES / 2];
    recorder.buffer_sub_data(GL_ARRAY_BUFFER, 0, data.len() as i64, &data);
    assert_eq!(replayer.take_calls(), []);
    recorder.buffer_sub_data(GL_ARRAY_BUFFER, 0, data.len() as i64, &data);
    let calls = replayer.take_calls();
    let [
        Call::SendFromSharedBuffer {
            offset: 0,
            records,
            flush_sequence_number: 1,
            ..
        },
    ] = calls.as_slice()
    else {
        panic!("{} calls", calls.len());
    };
    assert_eq!(records.len(), 2);
    assert_eq!(records[1].command_type, CommandType::BufferSubData as u16);
    assert_eq!(records[1].payload[32..32 + data.len()], data);
}

#[test]
fn a_record_larger_than_the_region_is_sent_on_its_own() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    record_viewport(&recorder);
    let data = vec![5u8; SMALL_CAPACITY];
    recorder.buffer_sub_data(GL_ARRAY_BUFFER, 8, data.len() as i64, &data);
    let calls = replayer.take_calls();
    let [first, Call::SendCommands { records, frames }] = calls.as_slice() else {
        panic!("{} calls", calls.len());
    };
    assert_eq!(*first, sent_from_shared_buffer(0, vec![record_of(&VIEWPORT)], 1));
    assert!(frames.is_empty());
    assert_eq!(records.len(), 1);
    let command = commands::BufferSubData {
        target: GL_ARRAY_BUFFER,
        offset: 8,
        size: SMALL_CAPACITY as i64,
        data: DataSpan {
            offset: 32,
            size: SMALL_CAPACITY as u32,
        },
        ..commands::BufferSubData::ZEROED
    };
    assert_eq!(records[0].payload[..32], *command.as_bytes());
    assert_eq!(records[0].payload[32..32 + data.len()], data);

    record_viewport(&recorder);
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(0, vec![record_of(&VIEWPORT)], 2)]
    );
}

#[test]
fn without_a_shared_buffer_commands_are_sent_as_bytes() {
    let replayer = MockReplayer::new();
    replayer.refuses_shared_memory.set(true);
    let recorder = replayer.recorder(SMALL_CAPACITY);
    assert_eq!(replayer.take_calls(), []);

    record_viewport(&recorder);
    upload_frame(&recorder, 70);
    assert_eq!(replayer.take_calls(), []);
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [Call::SendCommands {
            records: vec![record_of(&VIEWPORT), frame_record(0)],
            frames: vec![70]
        }]
    );
    recorder.flush_commands();
    assert_eq!(replayer.take_calls(), []);

    let data = vec![5u8; MAX_PENDING_COMMAND_BYTES];
    recorder.buffer_sub_data(GL_ARRAY_BUFFER, 0, data.len() as i64, &data);
    let calls = replayer.take_calls();
    assert!(matches!(calls.as_slice(), [Call::SendCommands { records, .. }] if records.len() == 1));
}

#[test]
fn a_frame_is_published_with_the_command_that_names_it() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    upload_frame(&recorder, 70);
    recorder.tex_sub_image2d_from_bitmap(
        1,
        0,
        5,
        6,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        90,
        FrameUpload {
            destination_size: Some((8, 9)),
            flip_y: true,
            premultiply_alpha: true,
        },
    );
    recorder.flush_commands();
    let sub_image = commands::TexSubImage2DFromBitmap {
        target: 1,
        xoffset: 5,
        yoffset: 6,
        format: GL_RGBA,
        r#type: GL_UNSIGNED_BYTE,
        bitmap_index: 1,
        has_explicit_destination_size: true,
        destination_width: 8,
        destination_height: 9,
        flip_y: true,
        premultiply_alpha: true,
        ..commands::TexSubImage2DFromBitmap::ZEROED
    };
    assert_eq!(
        replayer.take_calls(),
        [Call::SendFromSharedBuffer {
            offset: 0,
            records: vec![frame_record(0), record_of(&sub_image)],
            flush_sequence_number: 1,
            frames: vec![70, 90],
        }]
    );

    upload_frame(&recorder, 80);
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [Call::SendFromSharedBuffer {
            offset: 0,
            records: vec![frame_record(0)],
            flush_sequence_number: 2,
            frames: vec![80],
        }]
    );
}

#[test]
fn a_message_carries_no_more_frames_than_the_transport_allows() {
    let replayer = MockReplayer::new();
    replayer.max_frames_per_message.set(2);
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    for frame in [70, 80, 90] {
        upload_frame(&recorder, frame);
    }
    assert_eq!(
        replayer.take_calls(),
        [Call::SendFromSharedBuffer {
            offset: 0,
            records: vec![frame_record(0), frame_record(1)],
            flush_sequence_number: 1,
            frames: vec![70, 80],
        }]
    );
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [Call::SendFromSharedBuffer {
            offset: 0,
            records: vec![frame_record(0)],
            flush_sequence_number: 2,
            frames: vec![90],
        }]
    );
}

#[test]
fn room_for_the_command_of_a_frame_is_made_before_the_frame_is_pending() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(512);
    replayer.take_calls();

    // NB: This leaves less room than is kept free for a command that names a frame, though the command would fit.
    for _ in 0..9 {
        record_viewport(&recorder);
    }
    upload_frame(&recorder, 70);
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); 9], 1)]
    );
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [Call::SendFromSharedBuffer {
            offset: 0,
            records: vec![frame_record(0)],
            flush_sequence_number: 2,
            frames: vec![70],
        }]
    );
}

#[test]
fn a_lost_context_records_nothing_and_answers_with_zeros() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();
    recorder.set_lost();

    record_viewport(&recorder);
    upload_frame(&recorder, 70);
    recorder.flush_commands();
    assert_eq!(recorder.get_error(), 0);
    assert!(!recorder.is_buffer(1));
    let mut name = [0xAAu8; 4];
    assert_eq!(
        recorder.get_active_attrib(1, 0, 4, &mut name),
        GetActiveAttribOutputs::default()
    );
    assert_eq!(name, [0xAA; 4]);
    assert!(recorder.get_string(1).is_empty());
    assert_eq!(recorder.get_vertex_attrib_pointerv_robust_angle(0, 1), 0);
    recorder.clear_drawing_buffer();
    assert_eq!(recorder.read_back_drawing_buffer(0, 0, 1, 1), None);
    assert!(!recorder.read_buffer_sub_data(GL_ARRAY_BUFFER, 0, 4, |_| panic!("nothing was read")));
    recorder.read_pixels_robust_angle(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4, |_, _| {
        panic!("nothing was read")
    });
    assert_eq!(replayer.take_calls(), []);

    // NB: Objects are still created, so that the implementation of the context does not have to tell the cases apart.
    assert_eq!(recorder.create_program(), 1);
    assert_eq!(recorder.create_shader(2), 2);
}

#[test]
fn a_synchronous_call_publishes_what_was_recorded_before_it() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    replayer.queue_reply(
        &sync_calls::GetErrorReply {
            return_value: GL_INVALID_ENUM,
        },
        &[],
    );
    record_viewport(&recorder);
    assert_eq!(recorder.get_error(), GL_INVALID_ENUM);

    let mut request = Vec::new();
    request.extend_from_slice(&(SyncCallType::GetError as u16).to_ne_bytes());
    request.extend_from_slice(&[0, 0]);
    request.extend_from_slice(&16u32.to_ne_bytes());
    request.extend_from_slice(&[0; 16]);
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT)], 1),
            Call::Synchronous { request }
        ]
    );
    assert!(!recorder.is_lost());
}

#[test]
fn a_call_without_a_reply_loses_the_context() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    assert_eq!(recorder.get_error(), 0);
    assert!(recorder.is_lost());
}

#[test]
fn a_synchronous_call_returns_its_outputs_and_fills_its_buffers() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);

    let reply = sync_calls::GetActiveAttribReply {
        length: 3,
        size: 1,
        r#type: 0x8B50,
        name: DataSpan { offset: 32, size: 4 },
    };
    replayer.queue_reply(&reply, b"pos\0");
    let mut name = [0xAAu8; 8];
    let outputs = recorder.get_active_attrib(5, 0, 8, &mut name);
    assert_eq!(
        outputs,
        GetActiveAttribOutputs {
            length: 3,
            size: 1,
            r#type: 0x8B50
        }
    );
    assert_eq!(name, *b"pos\0\xAA\xAA\xAA\xAA");

    replayer.queue_reply(&sync_calls::IsBufferReply { return_value: 1 }, &[]);
    assert!(recorder.is_buffer(5));

    let reply = sync_calls::GetIntegervRobustANGLEReply {
        length: 2,
        data: DataSpan { offset: 16, size: 8 },
    };
    replayer.queue_reply(&reply, bytes_of(&[640i32, 480]));
    let mut size = [0i32; 2];
    assert_eq!(recorder.get_integerv_robust_angle(0x0D3A, 2, &mut size), 2);
    assert_eq!(size, [640, 480]);

    replayer.queue_reply(
        &sync_calls::GetUniformIndicesReply {
            uniform_indices: DataSpan { offset: 16, size: 8 },
        },
        bytes_of(&[4u32, 6]),
    );
    let mut indices = [0u32; 2];
    replayer.take_calls();
    recorder.get_uniform_indices(5, &[b"first", b"second\0ignored"], &mut indices);
    assert_eq!(indices, [4, 6]);
    let calls = replayer.take_calls();
    let [Call::Synchronous { request }] = calls.as_slice() else {
        panic!("{} calls", calls.len());
    };
    assert_eq!(request[HEADER_SIZE + 16..HEADER_SIZE + 29], *b"first\0second\0");
}

#[test]
fn a_string_ends_at_its_first_nul() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    recorder.bind_attrib_location(1, 2, b"ab\0cd");
    recorder.shader_source(3, b"void\0main");
    recorder.transform_feedback_varyings(4, &[b"a", b"bc\0d"], 5);
    recorder.flush_commands();
    let calls = replayer.take_calls();
    let [Call::SendFromSharedBuffer { records, .. }] = calls.as_slice() else {
        panic!("{} calls", calls.len());
    };
    let bind = commands::BindAttribLocation {
        program: 1,
        index: 2,
        name: DataSpan { offset: 16, size: 3 },
    };
    assert_eq!(records[0].payload[..16], *bind.as_bytes());
    assert_eq!(records[0].payload[16..], *b"ab\0\0\0\0\0\0");
    // NB: The source of a shader is passed with its length, so a NUL does not end it.
    let source = commands::ShaderSource {
        shader: 3,
        source: DataSpan { offset: 16, size: 10 },
    };
    assert_eq!(records[1].payload[..12], *source.as_bytes());
    assert_eq!(records[1].payload[16..26], *b"void\0main\0");
    let varyings = commands::TransformFeedbackVaryings {
        program: 4,
        count: 2,
        varyings: DataSpan { offset: 32, size: 5 },
        buffer_mode: 5,
    };
    assert_eq!(records[2].payload[..20], *varyings.as_bytes());
    assert_eq!(records[2].payload[32..37], *b"a\0bc\0");
}

#[test]
fn the_replayer_is_asked_for_a_string_once() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    replayer.queue_reply(
        &sync_calls::GetStringReply {
            value: DataSpan { offset: 16, size: 6 },
        },
        b"ANGLE\0",
    );
    assert_eq!(*recorder.get_string(0x1F01), *b"ANGLE");
    assert_eq!(*recorder.get_string(0x1F01), *b"ANGLE");
    assert_eq!(replayer.take_calls().len(), 1);

    // NB: The string that a lost context answers with is not the string of the name.
    recorder.set_lost();
    assert!(recorder.get_string(0x1F00).is_empty());
    recorder.restore(MockTransport {
        replayer: Rc::clone(&replayer),
    });
    replayer.take_calls();
    replayer.queue_reply(
        &sync_calls::GetStringReply {
            value: DataSpan { offset: 16, size: 2 },
        },
        b"X\0",
    );
    assert_eq!(*recorder.get_string(0x1F01), *b"X");
}

#[test]
fn objects_get_their_ids_from_the_recorder() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    let mut buffers = [0u32; 3];
    recorder.gen_buffers(&mut buffers);
    assert_eq!(buffers, [1, 2, 3]);
    assert_eq!(recorder.create_shader(0x8B31), 4);
    recorder.gen_textures(&mut []);
    recorder.delete_buffers(2, &buffers);
    recorder.flush_commands();

    let calls = replayer.take_calls();
    let [Call::SendFromSharedBuffer { records, .. }] = calls.as_slice() else {
        panic!("{} calls", calls.len());
    };
    assert_eq!(records.len(), 3);
    let gen_buffers = commands::GenBuffers {
        n: 3,
        buffers: DataSpan { offset: 16, size: 12 },
    };
    assert_eq!(records[0].payload[..12], *gen_buffers.as_bytes());
    assert_eq!(records[0].payload[16..28], *bytes_of(&[1u32, 2, 3]));
    assert_eq!(records[1], record_of(&commands::CreateShader { id: 4, r#type: 0x8B31 }));
    let delete_buffers = commands::DeleteBuffers {
        n: 2,
        buffers: DataSpan { offset: 16, size: 8 },
    };
    assert_eq!(records[2].payload[..12], *delete_buffers.as_bytes());
    assert_eq!(records[2].payload[16..24], *bytes_of(&[1u32, 2]));
}

#[test]
fn data_without_bytes_takes_no_room() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    recorder.buffer_data(GL_ARRAY_BUFFER, 16, None, 9);
    recorder.buffer_data(GL_ARRAY_BUFFER, 4, Some(&[1, 2, 3, 4, 5]), 9);
    recorder.flush_commands();
    let calls = replayer.take_calls();
    let [Call::SendFromSharedBuffer { records, .. }] = calls.as_slice() else {
        panic!("{} calls", calls.len());
    };
    let without_data = commands::BufferData {
        target: GL_ARRAY_BUFFER,
        size: 16,
        data: DataSpan { offset: 32, size: 0 },
        usage: 9,
        ..commands::BufferData::ZEROED
    };
    assert_eq!(records[0], record_of(&without_data));
    let with_data = commands::BufferData {
        target: GL_ARRAY_BUFFER,
        size: 4,
        has_data: true,
        data: DataSpan { offset: 32, size: 4 },
        usage: 9,
        ..commands::BufferData::ZEROED
    };
    assert_eq!(records[1].payload[..32], *with_data.as_bytes());
    assert_eq!(records[1].payload[32..], [1, 2, 3, 4, 0, 0, 0, 0]);
}

#[test]
#[should_panic(expected = "as large as the command says")]
fn data_that_is_smaller_than_its_command_says_is_not_recorded() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    recorder.uniform2fv(1, 2, bytes_of(&[1.0f32, 2.0, 3.0]));
}

#[test]
fn the_transport_is_not_called_while_data_is_borrowed() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(8 * VIEWPORT_RECORD_SIZE);
    replayer.take_calls();
    let record_with_borrowed_data = |data: &[u8]| {
        recorder.record_with_borrowed_data(data.len(), || {
            replayer.forbidding_calls(|| recorder.buffer_sub_data(GL_ARRAY_BUFFER, 0, data.len() as i64, data));
        });
    };

    // Room that the record needs is made before the data is borrowed.
    for _ in 0..7 {
        record_viewport(&recorder);
    }
    record_with_borrowed_data(&[5; 16]);
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); 7], 1)]
    );
    recorder.flush_commands();
    let calls = replayer.take_calls();
    assert!(matches!(calls.as_slice(), [Call::SendFromSharedBuffer { offset: 0, records, .. }] if records.len() == 1));

    // A record that the region is too small for is sent after the data is given back.
    record_viewport(&recorder);
    record_with_borrowed_data(&[5; 1024]);
    let calls = replayer.take_calls();
    let [first, Call::SendCommands { records, .. }] = calls.as_slice() else {
        panic!("{} calls", calls.len());
    };
    assert_eq!(*first, sent_from_shared_buffer(0, vec![record_of(&VIEWPORT)], 3));
    assert_eq!(records[0].payload[32..32 + 1024], [5; 1024]);
}

#[test]
fn a_flush_that_is_due_while_data_is_borrowed_follows_the_record() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(4 * MAX_PENDING_COMMAND_BYTES);
    replayer.take_calls();

    let data = vec![5u8; MAX_PENDING_COMMAND_BYTES];
    recorder.record_with_borrowed_data(data.len(), || {
        replayer.forbidding_calls(|| recorder.buffer_sub_data(GL_ARRAY_BUFFER, 0, data.len() as i64, &data));
    });
    let calls = replayer.take_calls();
    assert!(matches!(calls.as_slice(), [Call::SendFromSharedBuffer { offset: 0, records, .. }] if records.len() == 1));
}

#[test]
#[should_panic(expected = "one command is recorded with borrowed data")]
fn only_one_command_is_recorded_with_borrowed_data() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    recorder.record_with_borrowed_data(0, || {
        record_viewport(&recorder);
        record_viewport(&recorder);
    });
}

#[test]
fn data_of_the_expected_size_is_recorded_while_it_is_borrowed() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(8 * VIEWPORT_RECORD_SIZE);
    replayer.take_calls();
    replayer.executes_at_once.set(false);

    for _ in 0..7 {
        record_viewport(&recorder);
    }
    let data = [5u8; 16];
    record_borrowed_buffer_sub_data(&replayer, &recorder, data.len(), &data, || {
        // The room that the record needs was made before the data was borrowed, and the record is in the region
        // before the data is given back.
        assert_eq!(
            replayer.take_calls(),
            [
                sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); 7], 1),
                Call::WaitUntilPublishedCommandsExecuted
            ]
        );
        let record_in_region = replayer.bytes_of_region(0, buffer_sub_data_record_size(&data));
        assert_eq!(records_in(&record_in_region), [buffer_sub_data_record(&data)]);
    });
    assert_eq!(replayer.take_calls(), []);

    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(0, vec![buffer_sub_data_record(&data)], 2)]
    );
}

#[test]
fn a_record_with_less_data_than_expected_gets_its_room_once_the_data_is_given_back() {
    let replayer = MockReplayer::new();
    let capacity = 8 * VIEWPORT_RECORD_SIZE;
    let recorder = replayer.recorder(capacity);
    replayer.take_calls();
    replayer.executes_at_once.set(false);

    // NB: This is an upload of the first bytes of a view that is larger than the region of records, with less room
    //     left in the region than the record takes.
    for _ in 0..7 {
        record_viewport(&recorder);
    }
    let data = [5u8; 16];
    let size_of_view = 1024 * capacity;
    record_borrowed_buffer_sub_data(&replayer, &recorder, size_of_view, &data, || {
        assert_eq!(replayer.take_calls(), []);
        let room_at_cursor = replayer.bytes_of_region(7 * VIEWPORT_RECORD_SIZE, VIEWPORT_RECORD_SIZE);
        assert_eq!(room_at_cursor, [0; VIEWPORT_RECORD_SIZE]);
    });
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); 7], 1),
            Call::WaitUntilPublishedCommandsExecuted
        ]
    );
    assert!(!recorder.is_lost());

    record_viewport(&recorder);
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(
            0,
            vec![buffer_sub_data_record(&data), record_of(&VIEWPORT)],
            2
        )]
    );
}

#[test]
fn a_record_with_more_data_than_expected_gets_its_room_once_the_data_is_given_back() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(8 * VIEWPORT_RECORD_SIZE);
    replayer.take_calls();
    replayer.executes_at_once.set(false);

    // NB: The room that is left is enough for a record with as much data as expected, so none is made before the
    //     data is borrowed.
    for _ in 0..4 {
        record_viewport(&recorder);
    }
    let data = [5u8; 100];
    assert!(buffer_sub_data_record_size(&data) > 4 * VIEWPORT_RECORD_SIZE);
    record_borrowed_buffer_sub_data(&replayer, &recorder, 0, &data, || {
        assert_eq!(replayer.take_calls(), []);
    });
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); 4], 1),
            Call::WaitUntilPublishedCommandsExecuted
        ]
    );

    // A record that the region is too small for is sent on its own, after what was recorded before it.
    record_viewport(&recorder);
    let data_larger_than_region = [6u8; 1024];
    record_borrowed_buffer_sub_data(&replayer, &recorder, 0, &data_larger_than_region, || {
        assert_eq!(replayer.take_calls(), []);
    });
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![buffer_sub_data_record(&data), record_of(&VIEWPORT)], 2),
            Call::SendCommands {
                records: vec![buffer_sub_data_record(&data_larger_than_region)],
                frames: Vec::new()
            }
        ]
    );
}

#[test]
fn the_replayer_is_waited_for_only_when_a_record_with_borrowed_data_finds_the_region_full() {
    let replayer = MockReplayer::new();
    let capacity = 8 * VIEWPORT_RECORD_SIZE;
    let recorder = replayer.recorder(capacity);
    replayer.take_calls();
    let data = [5u8; 16];
    let unexpected_data_size = 1024 * capacity;

    // With room where the cursor is, the record is written there while its data is borrowed, whatever was expected.
    replayer.executes_at_once.set(false);
    record_viewport(&recorder);
    record_borrowed_buffer_sub_data(&replayer, &recorder, unexpected_data_size, &data, || {
        let record_in_region = replayer.bytes_of_region(VIEWPORT_RECORD_SIZE, buffer_sub_data_record_size(&data));
        assert_eq!(records_in(&record_in_region), [buffer_sub_data_record(&data)]);
    });
    assert_eq!(replayer.take_calls(), []);

    // Without room, what is in the region is published, and the replayer is waited for unless it has executed it.
    for _ in 0..4 {
        record_viewport(&recorder);
    }
    record_borrowed_buffer_sub_data(&replayer, &recorder, unexpected_data_size, &data, || {});
    let mut published_records = vec![record_of(&VIEWPORT), buffer_sub_data_record(&data)];
    published_records.extend(vec![record_of(&VIEWPORT); 4]);
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, published_records, 1),
            Call::WaitUntilPublishedCommandsExecuted
        ]
    );

    replayer.executes_at_once.set(true);
    for _ in 0..5 {
        record_viewport(&recorder);
    }
    record_borrowed_buffer_sub_data(&replayer, &recorder, unexpected_data_size, &data, || {});
    let mut published_records = vec![buffer_sub_data_record(&data)];
    published_records.extend(vec![record_of(&VIEWPORT); 5]);
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(0, published_records, 2)]
    );

    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [sent_from_shared_buffer(0, vec![buffer_sub_data_record(&data)], 3)]
    );
}

#[test]
fn a_context_that_is_lost_while_room_is_made_for_a_record_with_borrowed_data_drops_the_record() {
    let replayer = MockReplayer::new();
    let capacity = 8 * VIEWPORT_RECORD_SIZE;
    let recorder = replayer.recorder(capacity);
    replayer.take_calls();
    replayer.executes_at_once.set(false);
    replayer.wait_succeeds.set(false);

    for _ in 0..7 {
        record_viewport(&recorder);
    }
    record_borrowed_buffer_sub_data(&replayer, &recorder, 1024 * capacity, &[5; 16], || {});
    assert!(recorder.is_lost());
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); 7], 1),
            Call::WaitUntilPublishedCommandsExecuted
        ]
    );
    recorder.flush_commands();
    assert_eq!(replayer.take_calls(), []);
}

#[test]
fn a_flush_that_is_due_for_a_record_that_got_its_room_late_is_made() {
    let replayer = MockReplayer::new();
    let viewport_count = 129;
    let recorder = replayer.recorder(MAX_PENDING_COMMAND_BYTES + (viewport_count - 1) * VIEWPORT_RECORD_SIZE);
    replayer.take_calls();

    for _ in 0..viewport_count {
        record_viewport(&recorder);
    }
    let data = vec![5u8; MAX_PENDING_COMMAND_BYTES];
    record_borrowed_buffer_sub_data(&replayer, &recorder, 0, &data, || {
        assert_eq!(replayer.take_calls(), []);
    });
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT); viewport_count], 1),
            sent_from_shared_buffer(0, vec![buffer_sub_data_record(&data)], 2),
        ]
    );
}

#[test]
fn without_a_shared_buffer_borrowed_data_of_any_size_is_recorded_in_order() {
    let replayer = MockReplayer::new();
    replayer.refuses_shared_memory.set(true);
    let recorder = replayer.recorder(SMALL_CAPACITY);

    record_viewport(&recorder);
    let data = [5u8; 16];
    record_borrowed_buffer_sub_data(&replayer, &recorder, 1024 * SMALL_CAPACITY, &data, || {});
    record_borrowed_buffer_sub_data(&replayer, &recorder, 0, &data, || {});
    record_viewport(&recorder);
    assert_eq!(replayer.take_calls(), []);
    recorder.flush_commands();
    assert_eq!(
        replayer.take_calls(),
        [Call::SendCommands {
            records: vec![
                record_of(&VIEWPORT),
                buffer_sub_data_record(&data),
                buffer_sub_data_record(&data),
                record_of(&VIEWPORT)
            ],
            frames: Vec::new()
        }]
    );

    // A flush that is due once the record is made waits for the data to be given back.
    record_viewport(&recorder);
    let data = vec![5u8; MAX_PENDING_COMMAND_BYTES];
    record_borrowed_buffer_sub_data(&replayer, &recorder, 0, &data, || {
        assert_eq!(replayer.take_calls(), []);
    });
    assert_eq!(
        replayer.take_calls(),
        [Call::SendCommands {
            records: vec![record_of(&VIEWPORT), buffer_sub_data_record(&data)],
            frames: Vec::new()
        }]
    );
}

#[test]
fn what_is_read_back_goes_through_memory_shared_for_the_read() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();
    replayer.read_data.replace(vec![1, 2, 3, 4, 5, 6, 7, 8]);

    record_viewport(&recorder);
    let mut read = Vec::new();
    assert!(recorder.read_buffer_sub_data(GL_ARRAY_BUFFER, 12, 6, |data| read.extend_from_slice(data)));
    assert_eq!(read, [1, 2, 3, 4, 5, 6]);
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(0, vec![record_of(&VIEWPORT)], 1),
            Call::ReadBufferSubData {
                target: GL_ARRAY_BUFFER,
                offset: 12,
                size: 6
            },
        ]
    );
    assert!(recorder.read_buffer_sub_data(GL_ARRAY_BUFFER, 12, 0, |_| panic!("nothing to read")));
    assert_eq!(replayer.take_calls(), []);
    replayer.read_buffer_sub_data_succeeds.set(false);
    assert!(!recorder.read_buffer_sub_data(GL_ARRAY_BUFFER, 12, 6, |_| panic!("the replayer refused")));

    replayer.read_pixels_result.set(ReadPixelsResult {
        length: 4,
        columns: 1,
        rows: 1,
    });
    let mut read = Vec::new();
    recorder.read_pixels_robust_angle(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 8, |result, pixels| {
        assert_eq!(
            result,
            ReadPixelsResult {
                length: 4,
                columns: 1,
                rows: 1
            }
        );
        read.extend_from_slice(pixels);
    });
    assert_eq!(read, [1, 2, 3, 4]);
    let mut was_called = false;
    recorder.read_pixels_robust_angle(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 0, |_, pixels| {
        was_called = pixels.is_empty();
    });
    assert!(was_called);
    assert_eq!(
        replayer.take_calls().last(),
        Some(&Call::ReadPixels {
            buf_size: 0,
            has_pixels: false
        })
    );

    replayer.refuses_shared_memory.set(true);
    assert!(!recorder.read_buffer_sub_data(GL_ARRAY_BUFFER, 12, 6, |_| panic!("no memory to read into")));
    recorder.read_pixels_robust_angle(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 8, |_, _| panic!("no memory"));
    assert_eq!(replayer.take_calls(), []);
    assert_eq!(recorder.take_pending_local_error(), GL_OUT_OF_MEMORY);
    assert_eq!(recorder.take_pending_local_error(), 0);
}

#[test]
fn the_drawing_buffer_is_presented_after_what_was_recorded() {
    let replayer = MockReplayer::new();
    let recorder = replayer.recorder(SMALL_CAPACITY);
    replayer.take_calls();

    recorder.set_size(300, 150);
    recorder.present_canvas_for_compositing(true);
    recorder.clear_drawing_buffer();
    assert_eq!(recorder.read_back_drawing_buffer(1, 2, 3, 4), Some((1, 2, 3, 4)));
    assert_eq!(recorder.canvas_id(), Some(42));
    assert_eq!(
        replayer.take_calls(),
        [
            sent_from_shared_buffer(
                0,
                vec![record_of(&commands::SetDrawingBufferSize {
                    width: 300,
                    height: 150
                })],
                1
            ),
            Call::PresentCanvas {
                preserve_drawing_buffer: true
            },
            Call::ClearDrawingBuffer,
            Call::ReadBackDrawingBuffer,
        ]
    );

    drop(recorder);
    assert_eq!(replayer.take_calls(), [Call::DestroyContext]);
}

#[test]
fn a_call_of_the_transport_can_mark_the_context_lost() {
    let replayer = MockReplayer::new();
    let recorder = Rc::new(replayer.recorder(2 * VIEWPORT_RECORD_SIZE));
    replayer.take_calls();
    let transport_loses_the_context = |loses: bool| {
        replayer
            .recorder_to_lose
            .replace(if loses { Rc::downgrade(&recorder) } else { Weak::new() });
    };
    let restore = || {
        transport_loses_the_context(false);
        recorder.restore(MockTransport {
            replayer: Rc::clone(&replayer),
        });
        assert!(!recorder.is_lost());
        replayer.take_calls();
        transport_loses_the_context(true);
    };

    // While a flush publishes.
    restore();
    record_viewport(&recorder);
    recorder.flush_commands();
    assert!(recorder.is_lost());

    // While the region is drained for a record, which is then not written.
    restore();
    transport_loses_the_context(false);
    replayer.executes_at_once.set(false);
    record_viewport(&recorder);
    record_viewport(&recorder);
    recorder.flush_commands();
    transport_loses_the_context(true);
    record_viewport(&recorder);
    assert!(recorder.is_lost());
    replayer.executes_at_once.set(true);

    // While a call waits for its reply, which is then not read.
    restore();
    replayer.queue_reply(
        &sync_calls::GetErrorReply {
            return_value: GL_INVALID_ENUM,
        },
        &[],
    );
    assert_eq!(recorder.get_error(), 0);
    assert!(recorder.is_lost());

    // While something is read back, which is then not handed out.
    restore();
    assert!(!recorder.read_buffer_sub_data(GL_ARRAY_BUFFER, 0, 4, |_| panic!("the context is lost")));
    restore();
    recorder.read_pixels_robust_angle(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4, |_, _| {
        panic!("the context is lost")
    });
    assert!(recorder.is_lost());

    // While a record that is too large for the region is sent, and while the canvas is presented.
    restore();
    recorder.buffer_sub_data(GL_ARRAY_BUFFER, 0, 128, &[5; 128]);
    assert!(recorder.is_lost());
    restore();
    recorder.present_canvas_for_compositing(false);
    assert!(recorder.is_lost());

    // While the new shared command buffer of a restored context is announced.
    transport_loses_the_context(true);
    recorder.restore(MockTransport {
        replayer: Rc::clone(&replayer),
    });
    assert!(recorder.is_lost());
    transport_loses_the_context(false);
}

#[test]
fn a_restored_context_starts_over_with_its_new_transport() {
    let old_replayer = MockReplayer::new();
    let recorder = old_replayer.recorder(SMALL_CAPACITY);
    assert_eq!(recorder.create_program(), 1);
    upload_frame(&recorder, 70);
    recorder.flush_commands();
    record_viewport(&recorder);
    upload_frame(&recorder, 80);
    recorder.set_lost();
    old_replayer.take_calls();

    let new_replayer = MockReplayer::new();
    recorder.restore(MockTransport {
        replayer: Rc::clone(&new_replayer),
    });
    assert!(!recorder.is_lost());
    assert_eq!(old_replayer.take_calls(), []);
    assert_eq!(
        new_replayer.take_calls(),
        [Call::SetSharedCommandBuffer {
            size: SHARED_DATA_REGION_OFFSET + SMALL_CAPACITY
        }]
    );

    // NB: What the lost context did not publish is gone, and so is the frame that it named.
    assert_eq!(recorder.create_program(), 2);
    recorder.flush_commands();
    let created_program = record_of(&commands::CreateProgram { id: 2 });
    assert_eq!(
        new_replayer.take_calls(),
        [sent_from_shared_buffer(0, vec![created_program], 1)]
    );
}
