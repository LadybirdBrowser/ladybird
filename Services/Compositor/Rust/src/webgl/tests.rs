/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libcompositing_rust::display_list::commands::{CanvasId, ImageFrameResourceId, VideoSinkHandle};
use libcompositing_rust::webgl::ImageSource;
use libcompositing_rust::webgl::SHARED_DATA_REGION_OFFSET;
use libcompositing_rust::webgl::vocabulary::{commands, sync_calls};
use libcompositing_rust::webgl::wire::{
    Command, DataSpan, InlineData, Record, SyncReply, WireStruct, encode_sync_request, resolve_data_span,
};
use libgfx_rust::{IntRect, IntSize};

use super::gl::recording;
use super::host::{ContextOptions, ImageConversionOutcome};
use super::replay::ReplayError;
use super::testing::{FakeHost, TestMemory, record, reset, take_calls};
use super::{WebGLContexts, gl_constants::*};
use crate::host::WebContentClientId;

const CLIENT: WebContentClientId = WebContentClientId(7);
const GL_ARRAY_BUFFER: u32 = 0x8892;
const GL_STATIC_DRAW: u32 = 0x88E4;
const GL_TEXTURE_2D: u32 = 0x0DE1;
const GL_UNSIGNED_BYTE: u32 = 0x1401;
const GL_FLOAT: u32 = 0x1406;
const GL_SYNC_GPU_COMMANDS_COMPLETE: u32 = 0x9117;

fn options(webgl2: bool) -> ContextOptions {
    ContextOptions {
        webgl2,
        depth: true,
        stencil: false,
        antialias: true,
        size: IntSize {
            width: 300,
            height: 150,
        },
    }
}

fn contexts_with_one(host: &FakeHost, webgl2: bool) -> (WebGLContexts, CanvasId) {
    reset();
    let mut contexts = WebGLContexts::with_gl_functions(recording::functions());
    let created = contexts
        .create(host, CLIENT, options(webgl2))
        .expect("the context is created");
    take_calls();
    (contexts, created.canvas_id)
}

fn record_of<C: Command>(command: &C, data: InlineData<'_>) -> Vec<u8> {
    Record::new(C::TYPE, command.as_bytes(), data).unwrap().encoded()
}

fn replay(
    contexts: &mut WebGLContexts,
    host: &FakeHost,
    canvas_id: CanvasId,
    records: &[Vec<u8>],
    image_sources: &[ImageSource],
) -> Result<(), ReplayError> {
    let commands = TestMemory::new(&records.concat());
    let size = commands.bytes().len() as u64;
    contexts.execute_commands(host, CLIENT, canvas_id, Some(&commands), size, image_sources)
}

fn ids(ids: &[u32]) -> Vec<u8> {
    ids.iter().flat_map(|id| id.to_ne_bytes()).collect()
}

fn gen_buffers(ids_bytes: &[u8], count: i32) -> Vec<u8> {
    let data = InlineData::Bytes(ids_bytes);
    record_of(
        &commands::GenBuffers {
            n: count,
            buffers: DataSpan::following::<commands::GenBuffers>(data),
        },
        data,
    )
}

#[test]
fn objects_are_named_by_the_ids_that_webcontent_gave_them() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let delete_ids = ids(&[2, 1]);
    let delete_data = InlineData::Bytes(&delete_ids);
    let records = [
        gen_buffers(&ids(&[1, 2]), 2),
        record_of(
            &commands::BindBuffer {
                target: GL_ARRAY_BUFFER,
                buffer: 2,
            },
            InlineData::NONE,
        ),
        record_of(
            &commands::DeleteBuffers {
                n: 2,
                buffers: DataSpan::following::<commands::DeleteBuffers>(delete_data),
            },
            delete_data,
        ),
        record_of(
            &commands::BindBuffer {
                target: GL_ARRAY_BUFFER,
                buffer: 2,
            },
            InlineData::NONE,
        ),
    ];
    replay(&mut contexts, &host, canvas_id, &records, &[]).unwrap();
    assert_eq!(
        take_calls(),
        [
            "host.make_current",
            "glGenBuffers(1, \"ptr\")",
            "glGenBuffers(1, \"ptr\")",
            "glBindBuffer(34962, 2)",
            "glDeleteBuffers(2, \"ptr\")",
            "glBindBuffer(34962, 0)",
        ]
    );
}

#[test]
fn creating_objects_needs_as_many_ids_as_it_creates() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[gen_buffers(&ids(&[1, 2]), 3)], &[]),
        Err(ReplayError("GenBuffers has another number of objects than it creates"))
    );
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[gen_buffers(&ids(&[0]), 1)], &[]),
        Err(ReplayError("WebGL object id 0 is reserved"))
    );
}

#[test]
fn the_object_id_0_of_a_framebuffer_is_the_drawing_buffer() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let bind_default_framebuffer = record_of(
        &commands::BindFramebuffer {
            target: GL_FRAMEBUFFER,
            framebuffer: 0,
        },
        InlineData::NONE,
    );
    let resize = record_of(
        &commands::SetDrawingBufferSize { width: 10, height: 20 },
        InlineData::NONE,
    );
    let records = [bind_default_framebuffer.clone(), resize, bind_default_framebuffer];
    replay(&mut contexts, &host, canvas_id, &records[..1], &[]).unwrap();
    host.defaults.set(super::host::DefaultBuffers {
        framebuffer: 101,
        renderbuffer: 201,
    });
    replay(&mut contexts, &host, canvas_id, &records[1..], &[]).unwrap();
    assert_eq!(
        take_calls(),
        [
            "host.make_current",
            "glBindFramebuffer(36160, 100)",
            "host.make_current",
            "host.set_size(10x20)",
            "host.make_current",
            "glBindFramebuffer(36160, 101)",
        ]
    );
    let too_big = record_of(
        &commands::SetDrawingBufferSize {
            width: 16385,
            height: 1,
        },
        InlineData::NONE,
    );
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[too_big], &[]),
        Err(ReplayError("WebGL drawing buffer size is out of range"))
    );
}

#[test]
fn data_has_the_size_that_its_arguments_give() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let values = [1.0_f32, 2.0];
    let bytes: Vec<u8> = values.iter().flat_map(|value| value.to_ne_bytes()).collect();
    let data = InlineData::Bytes(&bytes);
    let uniform = |count| {
        record_of(
            &commands::Uniform2fv {
                location: 3,
                count,
                value: DataSpan::following::<commands::Uniform2fv>(data),
            },
            data,
        )
    };
    replay(&mut contexts, &host, canvas_id, &[uniform(1)], &[]).unwrap();
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[uniform(2)], &[]),
        Err(ReplayError(
            "the data of Uniform2fv.value has another size than its arguments give"
        ))
    );
    assert!(replay(&mut contexts, &host, canvas_id, &[uniform(-1)], &[]).is_err());
}

#[test]
fn data_that_may_be_null_is_either_there_or_empty() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let buffer_data = |has_data, size, span: DataSpan, data| {
        record_of(
            &commands::BufferData {
                target: GL_ARRAY_BUFFER,
                size,
                has_data,
                data: span,
                usage: GL_STATIC_DRAW,
                ..commands::BufferData::ZEROED
            },
            data,
        )
    };
    take_calls();
    replay(
        &mut contexts,
        &host,
        canvas_id,
        &[buffer_data(false, 64, DataSpan::EMPTY, InlineData::NONE)],
        &[],
    )
    .unwrap();
    assert_eq!(take_calls()[1], "glBufferData(34962, 64, \"null\", 35044)");
    let bytes = [9_u8; 4];
    let data = InlineData::Bytes(&bytes);
    replay(
        &mut contexts,
        &host,
        canvas_id,
        &[buffer_data(
            true,
            4,
            DataSpan::following::<commands::BufferData>(data),
            data,
        )],
        &[],
    )
    .unwrap();
    assert_eq!(take_calls()[1], "glBufferData(34962, 4, \"ptr\", 35044)");
    assert_eq!(
        replay(
            &mut contexts,
            &host,
            canvas_id,
            &[buffer_data(
                false,
                4,
                DataSpan::following::<commands::BufferData>(data),
                data
            )],
            &[],
        ),
        Err(ReplayError("BufferData.data has data although it has none"))
    );
}

struct BufferDataCall {
    data_address: usize,
    first_byte: u8,
    executed_flush_sequence_number: u64,
}

thread_local! {
    static BUFFER_DATA_CALLS: core::cell::RefCell<Vec<BufferDataCall>> = const { core::cell::RefCell::new(Vec::new()) };
    static SHARED_COMMAND_BUFFER_START: core::cell::Cell<usize> = const { core::cell::Cell::new(0) };
}

unsafe extern "system" fn buffer_data_that_notes_where_its_data_is(
    _: u32,
    _: isize,
    data: *const core::ffi::c_void,
    _: u32,
) {
    let shared_command_buffer_start = SHARED_COMMAND_BUFFER_START.get();
    // SAFETY: The replayer passes data that holds the bytes of the call, and a test that sets the start of a shared
    //         command buffer holds the buffer.
    let call = unsafe {
        BufferDataCall {
            data_address: data as usize,
            first_byte: data.cast::<u8>().read(),
            executed_flush_sequence_number: if shared_command_buffer_start == 0 {
                0
            } else {
                (shared_command_buffer_start as *const u64).read()
            },
        }
    };
    BUFFER_DATA_CALLS.with_borrow_mut(|calls| calls.push(call));
}

fn contexts_that_note_where_buffer_data_is(host: &FakeHost) -> (WebGLContexts, CanvasId) {
    reset();
    BUFFER_DATA_CALLS.take();
    SHARED_COMMAND_BUFFER_START.set(0);
    let mut gl = recording::functions();
    gl.buffer_data = buffer_data_that_notes_where_its_data_is;
    let mut contexts = WebGLContexts::with_gl_functions(gl);
    let canvas_id = contexts.create(host, CLIENT, options(false)).unwrap().canvas_id;
    (contexts, canvas_id)
}

fn buffer_data_of(bytes: &[u8]) -> Vec<u8> {
    let data = InlineData::Bytes(bytes);
    record_of(
        &commands::BufferData {
            target: GL_ARRAY_BUFFER,
            size: bytes.len() as i64,
            has_data: true,
            data: DataSpan::following::<commands::BufferData>(data),
            usage: GL_STATIC_DRAW,
            ..commands::BufferData::ZEROED
        },
        data,
    )
}

#[test]
fn a_large_upload_is_passed_to_gl_where_webcontent_wrote_it() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_that_note_where_buffer_data_is(&host);
    let records = [
        buffer_data_of(&[9; 64]),
        buffer_data_of(&vec![7; 64 * 1024]),
        buffer_data_of(&[5; 64]),
    ];
    let commands = TestMemory::new(&records.concat());
    let written = commands.bytes().as_ptr_range();
    let written = written.start as usize..written.end as usize;
    contexts
        .execute_commands(
            &host,
            CLIENT,
            canvas_id,
            Some(&commands),
            commands.bytes().len() as u64,
            &[],
        )
        .unwrap();
    let calls = BUFFER_DATA_CALLS.take();
    let placements: Vec<(u8, bool)> = calls
        .iter()
        .map(|call| (call.first_byte, written.contains(&call.data_address)))
        .collect();
    assert_eq!(placements, [(9, false), (7, true), (5, false)]);
}

#[test]
fn the_data_of_a_large_upload_lies_inside_its_record() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_that_note_where_buffer_data_is(&host);
    let bytes = vec![7; 64 * 1024];
    let data = InlineData::Bytes(&bytes);
    let span = DataSpan::following::<commands::BufferData>(data);
    let record = record_of(
        &commands::BufferData {
            target: GL_ARRAY_BUFFER,
            size: bytes.len() as i64 + 4096,
            has_data: true,
            data: DataSpan {
                size: span.size + 4096,
                ..span
            },
            usage: GL_STATIC_DRAW,
            ..commands::BufferData::ZEROED
        },
        data,
    );
    let commands = TestMemory::new(&record);
    assert_eq!(
        contexts.execute_commands(&host, CLIENT, canvas_id, Some(&commands), record.len() as u64, &[]),
        Err(ReplayError("WebGL data span lies outside its payload"))
    );
    assert!(BUFFER_DATA_CALLS.take().is_empty());
}

#[test]
fn only_the_records_that_webcontent_wrote_into_a_command_buffer_are_replayed() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let enable = record_of(&commands::Enable { cap: 1 }, InlineData::NONE);
    let commands = TestMemory::new(&[enable.clone(), vec![0; 64]].concat());
    contexts
        .execute_commands(&host, CLIENT, canvas_id, Some(&commands), enable.len() as u64, &[])
        .unwrap();
    assert_eq!(take_calls(), ["host.make_current", "glEnable(1)"]);
    assert_eq!(
        contexts.execute_commands(
            &host,
            CLIENT,
            canvas_id,
            Some(&commands),
            commands.bytes().len() as u64 + 1,
            &[]
        ),
        Err(ReplayError(
            "WebContent sent more WebGL commands than their buffer holds"
        ))
    );
}

#[test]
fn a_shared_command_buffer_is_given_back_once_its_records_are_replayed() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_that_note_where_buffer_data_is(&host);
    let record = buffer_data_of(&vec![7; 64 * 1024]);
    let mut shared = vec![0_u8; SHARED_DATA_REGION_OFFSET + record.len()];
    shared[SHARED_DATA_REGION_OFFSET..].copy_from_slice(&record);
    let shared = Box::new(TestMemory::new(&shared));
    let shared_start = shared.bytes().as_ptr();
    SHARED_COMMAND_BUFFER_START.set(shared_start as usize);
    contexts.set_command_buffer(CLIENT, canvas_id, Some(shared)).unwrap();
    contexts
        .execute_commands_from_shared_buffer(&host, CLIENT, canvas_id, 0, record.len() as u64, 42, &[])
        .unwrap();
    let calls = BUFFER_DATA_CALLS.take();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].executed_flush_sequence_number, 0);
    // SAFETY: The test owns the memory, which the context holds.
    assert_eq!(unsafe { shared_start.cast::<u64>().read() }, 42);
    SHARED_COMMAND_BUFFER_START.set(0);
}

#[test]
fn a_record_whose_booleans_or_spans_are_out_of_range_is_rejected() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let mut record = record_of(
        &commands::BufferData {
            target: GL_ARRAY_BUFFER,
            has_data: false,
            ..commands::BufferData::ZEROED
        },
        InlineData::NONE,
    );
    record[8 + 16] = 2;
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[record], &[]),
        Err(ReplayError("WebGL payload does not hold its struct"))
    );
    let name = record_of(
        &commands::RequestExtensionANGLE {
            name: DataSpan { offset: 16, size: 64 },
        },
        InlineData::NONE,
    );
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[name], &[]),
        Err(ReplayError("WebGL data span lies outside its payload"))
    );
    let mut truncated = record_of(&commands::Enable { cap: 1 }, InlineData::NONE);
    truncated.truncate(7);
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[truncated], &[]),
        Err(ReplayError("Truncated WebGL command header"))
    );
}

#[test]
fn a_string_ends_with_a_nul_and_a_shader_source_is_passed_with_its_length() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let source = InlineData::NulTerminated(b"void main() {}");
    let shader_source = record_of(
        &commands::ShaderSource {
            shader: 0,
            source: DataSpan::following::<commands::ShaderSource>(source),
        },
        source,
    );
    replay(&mut contexts, &host, canvas_id, &[shader_source], &[]).unwrap();
    assert_eq!(take_calls()[1], "glShaderSource(0, 1, \"ptr\", \"ptr\")");

    let unterminated = InlineData::Bytes(b"main");
    let shader_source = record_of(
        &commands::ShaderSource {
            shader: 0,
            source: DataSpan::following::<commands::ShaderSource>(unterminated),
        },
        unterminated,
    );
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[shader_source], &[]),
        Err(ReplayError("WebGL string is not NUL-terminated"))
    );

    let varyings = InlineData::Bytes(b"a\0b");
    let transform_feedback_varyings = record_of(
        &commands::TransformFeedbackVaryings {
            program: 0,
            count: 2,
            varyings: DataSpan::following::<commands::TransformFeedbackVaryings>(varyings),
            buffer_mode: 0,
        },
        varyings,
    );
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[transform_feedback_varyings], &[]),
        Err(ReplayError("WebGL string is not NUL-terminated"))
    );
}

fn sync_call<R: libcompositing_rust::webgl::wire::SyncRequest>(
    contexts: &mut WebGLContexts,
    host: &FakeHost,
    canvas_id: CanvasId,
    request: &R,
    data: InlineData<'_>,
) -> Result<Vec<u8>, ReplayError> {
    let mut bytes = Vec::new();
    assert!(encode_sync_request(&mut bytes, request, data));
    contexts.execute_sync_call(host, CLIENT, canvas_id, &bytes)
}

#[test]
fn a_page_never_blocks_the_compositor_on_a_sync_object() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, true);
    let fence = record_of(
        &commands::FenceSync {
            id: 5,
            condition: GL_SYNC_GPU_COMMANDS_COMPLETE,
            flags: 0,
        },
        InlineData::NONE,
    );
    replay(&mut contexts, &host, canvas_id, &[fence], &[]).unwrap();
    sync_call(
        &mut contexts,
        &host,
        canvas_id,
        &sync_calls::ClientWaitSyncRequest {
            sync: 5,
            flags: 1,
            timeout: 1_000_000,
            ..sync_calls::ClientWaitSyncRequest::ZEROED
        },
        InlineData::NONE,
    )
    .unwrap();
    assert_eq!(
        take_calls(),
        [
            "host.make_current",
            "glFenceSync(37143, 0)",
            "host.make_current",
            "glClientWaitSync(\"ptr\", 1, 0)",
        ]
    );
}

#[test]
fn a_synchronous_call_answers_its_outputs_after_its_struct() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let reply = sync_call(
        &mut contexts,
        &host,
        canvas_id,
        &sync_calls::GetShaderInfoLogRequest { shader: 0, buf_size: 5 },
        InlineData::NONE,
    )
    .unwrap();
    let decoded = sync_calls::GetShaderInfoLogReply::from_reply_bytes(&reply);
    assert_eq!(decoded.info_log, DataSpan { offset: 16, size: 5 });
    assert_eq!(resolve_data_span(&reply, decoded.info_log), [0; 5]);
    assert_eq!(reply.len(), 32);

    assert_eq!(
        sync_call(
            &mut contexts,
            &host,
            canvas_id,
            &sync_calls::GetShaderInfoLogRequest {
                shader: 0,
                buf_size: -1,
            },
            InlineData::NONE,
        ),
        Err(ReplayError("WebGL asks for a buffer of a size that it cannot have"))
    );
}

unsafe extern "system" fn errors_of_the_compositor_and_then_of_the_page() -> u32 {
    thread_local! {
        static ERRORS: core::cell::RefCell<Vec<u32>> = core::cell::RefCell::new(vec![0x0502, 0x0502, 0x0505, 0, 0x0500, 0]);
    }
    ERRORS.with_borrow_mut(|errors| if errors.is_empty() { 0 } else { errors.remove(0) })
}

#[test]
fn the_errors_of_the_compositor_are_collected_before_those_of_gl() {
    let host = FakeHost::default();
    reset();
    let mut gl = recording::functions();
    gl.get_error = errors_of_the_compositor_and_then_of_the_page;
    let mut contexts = WebGLContexts::with_gl_functions(gl);
    let canvas_id = contexts.create(&host, CLIENT, options(false)).unwrap().canvas_id;
    let (context, gl) = contexts.context_and_gl(CLIENT, canvas_id).unwrap();
    let mut replayer = super::replay::Replayer {
        gl,
        host: &host,
        client: CLIENT,
        canvas_id,
        webgl2: false,
        objects: &mut context.objects,
        stashed_errors: &mut context.stashed_errors,
        defaults: host.defaults.get(),
        image_sources: &[],
        payload_in_shared_memory: None,
    };
    assert!(replayer.stash_errors());
    let errors: Vec<u32> = (0..4).map(|_| replayer.get_error()).collect();
    assert_eq!(errors, [0x0502, 0x0505, 0x0500, 0]);
}

fn tex_image(bitmap_index: u32, format: u32, explicit_size: Option<(i32, i32)>) -> Vec<u8> {
    record_of(
        &commands::TexImage2DFromBitmap {
            target: GL_TEXTURE_2D,
            level: 0,
            internalformat: GL_RGBA as i32,
            format,
            r#type: GL_UNSIGNED_BYTE,
            bitmap_index,
            has_explicit_destination_size: explicit_size.is_some(),
            destination_width: explicit_size.map_or(0, |size| size.0),
            destination_height: explicit_size.map_or(0, |size| size.1),
            flip_y: false,
            premultiply_alpha: false,
            ..commands::TexImage2DFromBitmap::ZEROED
        },
        InlineData::NONE,
    )
}

#[test]
fn an_image_upload_names_a_known_image_in_a_known_format() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let image = [ImageSource::RegisteredImageFrame(ImageFrameResourceId(9))];
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[tex_image(1, GL_RGBA, None)], &image),
        Err(ReplayError("WebGL image upload references an out-of-range bitmap"))
    );
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[tex_image(0, GL_FLOAT, None)], &image),
        Err(ReplayError(
            "WebGL image upload has an unsupported format and type combination"
        ))
    );
    *host.conversion.borrow_mut() = Some(ImageConversionOutcome::UnknownImageFrame);
    assert_eq!(
        replay(&mut contexts, &host, canvas_id, &[tex_image(0, GL_RGBA, None)], &image),
        Err(ReplayError(
            "WebGL image upload names an image frame that is not registered"
        ))
    );
}

#[test]
fn a_converted_image_is_uploaded_with_the_unpack_state_of_packed_pixels() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let image = [ImageSource::RegisteredImageFrame(ImageFrameResourceId(9))];
    *host.conversion.borrow_mut() = Some(ImageConversionOutcome::Converted {
        pixels: vec![0; 16],
        width: 2,
        height: 2,
    });
    replay(&mut contexts, &host, canvas_id, &[tex_image(0, GL_RGBA, None)], &image).unwrap();
    assert_eq!(
        take_calls(),
        [
            "host.make_current",
            "host.convert(RegisteredImageFrame(ImageFrameResourceId(9)), RGBA8888)",
            "glGetIntegerv(3317, \"ptr\")",
            "glPixelStorei(3317, 1)",
            "glGetString(7939)",
            "glTexImage2DRobustANGLE(3553, 0, 6408, 2, 2, 0, 6408, 5121, 16, \"ptr\")",
            "glPixelStorei(3317, 0)",
        ]
    );

    replay(
        &mut contexts,
        &host,
        canvas_id,
        &[tex_image(0, GL_RGBA, Some((-1, 2)))],
        &image,
    )
    .unwrap();
    assert_eq!(
        take_calls()[4],
        "glTexImage2DRobustANGLE(3553, 0, 6408, -1, 2, 0, 6408, 5121, 0, \"ptr\")"
    );
}

#[test]
fn a_held_video_frame_is_copied_on_the_gpu_where_it_can_be() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let video = [ImageSource::HeldVideoFrame {
        sink_handle: VideoSinkHandle(4),
        generation: 2,
    }];
    host.can_copy_images_into_textures.set(true);
    host.held_video_frame_size.set(Some(IntSize { width: 8, height: 6 }));
    replay(&mut contexts, &host, canvas_id, &[tex_image(0, GL_RGBA, None)], &video).unwrap();
    assert_eq!(
        take_calls(),
        [
            "host.make_current",
            "host.held_video_frame(4, 2)",
            "host.can_copy",
            "host.video_frame_image(4)",
            "glGetError()",
            "glGetString(7939)",
            "glTexImage2DRobustANGLE(3553, 0, 6408, 8, 6, 0, 6408, 5121, 0, \"null\")",
            "glGetError()",
            "host.copy(4, 8x6)",
            "glGetError()",
        ]
    );

    host.copy_succeeds.set(false);
    *host.conversion.borrow_mut() = Some(ImageConversionOutcome::Failed);
    replay(&mut contexts, &host, canvas_id, &[tex_image(0, GL_RGBA, None)], &video).unwrap();
    let calls = take_calls();
    assert_eq!(calls[8], "host.copy(4, 8x6)");
    assert_eq!(
        calls[9],
        "host.convert(HeldVideoFrame { sink_handle: VideoSinkHandle(4), generation: 2 }, RGBA8888)"
    );
    assert_eq!(calls.len(), 10);
}

#[test]
fn a_drawing_buffer_that_is_not_preserved_is_cleared_before_the_next_frame() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    contexts.present(&host, CLIENT, canvas_id, false).unwrap();
    let pixels = TestMemory::new(&[0; 16]);
    let rect = IntRect {
        x: 0,
        y: 0,
        width: 2,
        height: 2,
    };
    contexts
        .read_back_drawing_buffer(&host, CLIENT, canvas_id, rect, Some(&pixels))
        .unwrap();
    contexts.present(&host, CLIENT, canvas_id, false).unwrap();
    replay(
        &mut contexts,
        &host,
        canvas_id,
        &[record_of(&commands::Flush::ZEROED, InlineData::NONE)],
        &[],
    )
    .unwrap();
    contexts
        .read_back_drawing_buffer(&host, CLIENT, canvas_id, rect, Some(&pixels))
        .unwrap();
    let calls = take_calls();
    assert_eq!(
        calls[..4],
        [
            "host.present(false, false)",
            "host.read_back(true)",
            "host.present(false, true)",
            "host.make_current",
        ]
    );
    assert_eq!(calls[4], "glGetIntegerv(36006, \"ptr\")");
    assert!(calls.contains(&"glClear(17664)".to_string()));
    assert_eq!(calls[calls.len() - 2], "glFlush()");
    assert_eq!(calls[calls.len() - 1], "host.read_back(false)");
}

#[test]
fn published_commands_lie_inside_the_shared_command_buffer() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let enable = record_of(&commands::Enable { cap: 1 }, InlineData::NONE);
    let mut shared = vec![0_u8; SHARED_DATA_REGION_OFFSET + 64];
    shared[SHARED_DATA_REGION_OFFSET..SHARED_DATA_REGION_OFFSET + enable.len()].copy_from_slice(&enable);
    let shared = Box::new(TestMemory::new(&shared));
    let shared_bytes = shared.bytes().as_ptr();
    assert_eq!(
        contexts.execute_commands_from_shared_buffer(&host, CLIENT, canvas_id, 0, 16, 1, &[]),
        Err(ReplayError(
            "WebContent published WebGL commands without a shared command buffer"
        ))
    );
    contexts.set_command_buffer(CLIENT, canvas_id, Some(shared)).unwrap();
    contexts
        .execute_commands_from_shared_buffer(&host, CLIENT, canvas_id, 0, enable.len() as u64, 42, &[])
        .unwrap();
    // SAFETY: The test owns the memory, which the context holds.
    let executed = unsafe { shared_bytes.cast::<u64>().read() };
    assert_eq!(executed, 42);
    assert_eq!(take_calls()[1], "glEnable(1)");
    for (offset, size) in [(8, 16), (0, 65), (80, 0)] {
        assert_eq!(
            contexts.execute_commands_from_shared_buffer(&host, CLIENT, canvas_id, offset, size, 43, &[]),
            Err(ReplayError(
                "WebContent published an invalid WebGL shared command buffer range"
            ))
        );
    }
    assert_eq!(
        contexts.set_command_buffer(CLIENT, canvas_id, Some(Box::new(TestMemory::new(&[0; 128])))),
        Err(ReplayError("WebContent sent an invalid WebGL shared command buffer"))
    );
}

#[test]
fn a_context_is_named_by_its_client_and_canvas() {
    let host = FakeHost::default();
    let (mut contexts, canvas_id) = contexts_with_one(&host, false);
    let other_client = WebContentClientId(8);
    assert_eq!(
        contexts.present(&host, other_client, canvas_id, true),
        Err(ReplayError("WebContent named a WebGL context that it has not created"))
    );
    contexts.clear_drawing_buffer(&host, other_client, canvas_id);
    assert!(take_calls().is_empty());
    let pixels = TestMemory::new(&[0; 4]);
    let rect = IntRect {
        x: 0,
        y: 0,
        width: 1,
        height: 1,
    };
    assert_eq!(
        contexts
            .read_pixels(
                &host,
                CLIENT,
                canvas_id,
                rect,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                5,
                Some(&pixels)
            )
            .err(),
        Some(ReplayError("WebContent sent an invalid WebGL readPixels buffer"))
    );
    contexts.forget_client(CLIENT);
    assert!(contexts.present(&host, CLIENT, canvas_id, true).is_err());
    contexts.destroy(&host, CLIENT, canvas_id);
    record("end".into());
    assert_eq!(take_calls(), ["end"]);
}
