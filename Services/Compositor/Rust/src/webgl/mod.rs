/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebGL contexts of the clients: the GL contexts that C++ makes for them, and what replaying their commands needs
//! to know between messages.

pub mod custom;
pub mod gl;
pub mod gl_constants;
pub mod host;
pub mod object_map;
pub mod replay;
#[cfg(test)]
mod testing;
#[cfg(test)]
mod tests;

use std::collections::HashMap;
use std::ops::Range;
use std::sync::atomic::{AtomicU64, Ordering};

use libcompositing_rust::display_list::commands::CanvasId;
use libcompositing_rust::webgl::vocabulary::{CommandType, MAX_COMMAND_STRUCT_SIZE};
use libcompositing_rust::webgl::wire::{HEADER_SIZE, read_header};
use libcompositing_rust::webgl::{ImageSource, SHARED_DATA_REGION_OFFSET};
use libgfx_rust::{IntRect, IntSize};

use self::custom::{clear_buffer_to_default_values, supported_extensions};
use self::gl::GlFunctions;
use self::gl_constants::GL_MAP_READ_BIT;
use self::host::{ContextOptions, DefaultBuffers, MappedMemory, PresentOutcome, WebGLHost};
use self::object_map::ObjectMap;
use self::replay::{PayloadInSharedMemory, ReplayError, Replayer, only_gl_reads_the_data_of};
use crate::host::WebContentClientId;

const MAX_DRAWING_BUFFER_DIMENSION: i32 = 16384;

/// A copy of what is replayed that holds more than this is not kept for the next message.
const MAX_KEPT_SCRATCH_SIZE: usize = 1024 * 1024;

/// A record with a payload this big, which holds data that only GL reads, is replayed where WebContent wrote it rather
/// than copied.
const SMALLEST_PAYLOAD_REPLAYED_IN_PLACE: usize = 16 * 1024;

pub struct CreatedContext {
    pub canvas_id: CanvasId,
    pub supported_extensions: Vec<String>,
}

pub struct ReadPixelsResult {
    pub length: i32,
    pub columns: i32,
    pub rows: i32,
}

struct WebGLContext {
    webgl2: bool,
    objects: ObjectMap,
    command_buffer: Option<Box<dyn MappedMemory>>,
    /// A drawing buffer that is not preserved is cleared before the next command that draws or reads after the frame
    /// was presented, which goes on being the content of the canvas until then.
    needs_clear_before_next_frame: bool,
    stashed_errors: Vec<u32>,
    /// Where what is replayed is copied to, so that WebContent cannot change it while it is checked and replayed. Its
    /// elements keep the data of the records aligned as WebContent wrote it.
    scratch: Vec<u64>,
}

impl WebGLContext {
    fn new(webgl2: bool) -> Self {
        Self {
            webgl2,
            objects: ObjectMap::default(),
            command_buffer: None,
            needs_clear_before_next_frame: false,
            stashed_errors: Vec::new(),
            scratch: Vec::new(),
        }
    }

    fn give_back_scratch(&mut self, scratch: Vec<u64>) {
        if scratch.capacity() * size_of::<u64>() <= MAX_KEPT_SCRATCH_SIZE {
            self.scratch = scratch;
        }
    }
}

/// Copies bytes that WebContent can write while they are replayed into the scratch, which keeps the data of the records
/// as aligned as WebContent wrote it, and answers the copy.
///
/// # Safety
///
/// `bytes` is valid for reads of `size` bytes.
unsafe fn copy_to_scratch(scratch: &mut Vec<u64>, bytes: *const u8, size: usize) -> &[u8] {
    scratch.clear();
    scratch.resize(size.div_ceil(size_of::<u64>()), 0);
    // SAFETY: The caller passes as many readable bytes, and the scratch has room for them.
    unsafe {
        core::ptr::copy_nonoverlapping(bytes, scratch.as_mut_ptr().cast::<u8>(), size);
        ak::slice_from_raw_parts(scratch.as_ptr().cast::<u8>(), size)
    }
}

/// # Safety
///
/// `commands` is valid for reads of the bytes of `records`.
unsafe fn replay_copied_records<H: WebGLHost>(
    replayer: &mut Replayer<'_, H>,
    scratch: &mut Vec<u64>,
    commands: *const u8,
    records: Range<usize>,
) -> Result<(), ReplayError> {
    if records.is_empty() {
        return Ok(());
    }
    // SAFETY: Guaranteed by the caller.
    let copy = unsafe { copy_to_scratch(scratch, commands.add(records.start), records.len()) };
    replayer.replay_commands(copy)
}

/// Replays records that WebContent can write while they are replayed. Runs of records are copied before they are
/// replayed, and a record with a large payload that only GL reads is replayed where it is, with only its struct copied.
///
/// # Safety
///
/// `commands` is valid for reads of `size` bytes, which stay mapped while they are replayed.
unsafe fn replay_records_in_shared_memory<H: WebGLHost>(
    replayer: &mut Replayer<'_, H>,
    scratch: &mut Vec<u64>,
    commands: *const u8,
    size: usize,
) -> Result<(), ReplayError> {
    let mut copied_records_start = 0;
    let mut offset = 0;
    while size - offset >= HEADER_SIZE {
        let mut header = [0_u8; HEADER_SIZE];
        // SAFETY: The header lies inside the records.
        unsafe { core::ptr::copy_nonoverlapping(commands.add(offset), header.as_mut_ptr(), HEADER_SIZE) };
        let Some((type_number, payload_size)) = read_header(&header) else {
            break;
        };
        let payload_start = offset + HEADER_SIZE;
        if payload_size > size - payload_start {
            break;
        }
        let command_type = CommandType::from_number(type_number).filter(|&command_type| {
            payload_size >= SMALLEST_PAYLOAD_REPLAYED_IN_PLACE && only_gl_reads_the_data_of(command_type)
        });
        if let Some(command_type) = command_type {
            // SAFETY: Guaranteed by the caller.
            unsafe { replay_copied_records(replayer, scratch, commands, copied_records_start..offset)? };
            // SAFETY: The payload lies inside the records.
            let payload = unsafe { commands.add(payload_start) };
            // SAFETY: As above.
            let struct_bytes = unsafe { copy_to_scratch(scratch, payload, payload_size.min(MAX_COMMAND_STRUCT_SIZE)) };
            replayer.payload_in_shared_memory = Some(PayloadInSharedMemory {
                start: payload,
                size: payload_size,
            });
            let result = replayer.replay_record(command_type, struct_bytes);
            replayer.payload_in_shared_memory = None;
            result?;
            copied_records_start = payload_start + payload_size;
        }
        offset = payload_start + payload_size;
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { replay_copied_records(replayer, scratch, commands, copied_records_start..size) }
}

#[derive(Default)]
pub struct WebGLContexts {
    gl: Option<Box<GlFunctions>>,
    clients: HashMap<WebContentClientId, HashMap<CanvasId, WebGLContext>>,
}

impl WebGLContexts {
    #[cfg(test)]
    fn with_gl_functions(gl: GlFunctions) -> Self {
        Self {
            gl: Some(Box::new(gl)),
            clients: HashMap::new(),
        }
    }

    fn context_and_gl(
        &mut self,
        client: WebContentClientId,
        canvas_id: CanvasId,
    ) -> Result<(&mut WebGLContext, &GlFunctions), ReplayError> {
        let context = self
            .clients
            .get_mut(&client)
            .and_then(|contexts| contexts.get_mut(&canvas_id))
            .ok_or(ReplayError("WebContent named a WebGL context that it has not created"))?;
        let gl = self
            .gl
            .as_deref()
            .expect("the GL functions are loaded before the first context is created");
        Ok((context, gl))
    }

    /// None where the context could not be created, which is no misbehavior: the canvas has no context then.
    pub fn create<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        options: ContextOptions,
    ) -> Option<CreatedContext> {
        let dimensions = 1..=MAX_DRAWING_BUFFER_DIMENSION;
        if !dimensions.contains(&options.size.width) || !dimensions.contains(&options.size.height) {
            return None;
        }
        let canvas_id = host.allocate_canvas_id();
        if !host.create_webgl_context(client, canvas_id, options) {
            return None;
        }
        if self.gl.is_none() {
            // SAFETY: The host answers the address of a GL function or null.
            match unsafe { GlFunctions::load(|name| host.gl_proc_address(name)) } {
                Ok(gl) => self.gl = Some(Box::new(gl)),
                Err(missing_function) => {
                    eprintln!("Compositor: Cannot replay WebGL without {missing_function:?}");
                    host.destroy_webgl_context(client, canvas_id);
                    return None;
                }
            }
        }
        let gl = self.gl.as_deref().expect("the GL functions are loaded");
        host.make_webgl_context_current(client, canvas_id);
        let supported_extensions = supported_extensions(gl);
        self.clients
            .entry(client)
            .or_default()
            .insert(canvas_id, WebGLContext::new(options.webgl2));
        Some(CreatedContext {
            canvas_id,
            supported_extensions,
        })
    }

    /// A context that is gone already is no misbehavior, as it is not for a 2D canvas.
    pub fn destroy<H: WebGLHost>(&mut self, host: &H, client: WebContentClientId, canvas_id: CanvasId) {
        if self
            .clients
            .get_mut(&client)
            .and_then(|contexts| contexts.remove(&canvas_id))
            .is_some()
        {
            host.destroy_webgl_context(client, canvas_id);
        }
    }

    /// The C++ side destroys the GL contexts of a client that went away with the rest of what it holds for it.
    pub fn forget_client(&mut self, client: WebContentClientId) {
        self.clients.remove(&client);
    }

    pub fn set_command_buffer(
        &mut self,
        client: WebContentClientId,
        canvas_id: CanvasId,
        command_buffer: Option<Box<dyn MappedMemory>>,
    ) -> Result<(), ReplayError> {
        let (context, _) = self.context_and_gl(client, canvas_id)?;
        let command_buffer = command_buffer
            .filter(|command_buffer| command_buffer.len() > SHARED_DATA_REGION_OFFSET)
            .ok_or(ReplayError("WebContent sent an invalid WebGL shared command buffer"))?;
        context.command_buffer = Some(command_buffer);
        Ok(())
    }

    fn replay<H: WebGLHost>(
        context: &mut WebGLContext,
        gl: &GlFunctions,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        image_sources: &[ImageSource],
        commands: impl FnOnce(&mut Replayer<'_, H>) -> Result<(), ReplayError>,
    ) -> Result<(), ReplayError> {
        let defaults = host.make_webgl_context_current(client, canvas_id);
        clear_drawing_buffer_if_needed(context, gl, defaults);
        let mut replayer = Replayer {
            gl,
            host,
            client,
            canvas_id,
            webgl2: context.webgl2,
            objects: &mut context.objects,
            stashed_errors: &mut context.stashed_errors,
            defaults,
            image_sources,
            payload_in_shared_memory: None,
        };
        commands(&mut replayer)
    }

    #[allow(clippy::too_many_arguments)]
    pub fn execute_commands_from_shared_buffer<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        offset: u64,
        size_in_bytes: u64,
        flush_sequence_number: u64,
        image_sources: &[ImageSource],
    ) -> Result<(), ReplayError> {
        let (context, gl) = self.context_and_gl(client, canvas_id)?;
        let Some(command_buffer) = context.command_buffer.as_deref() else {
            return Err(ReplayError(
                "WebContent published WebGL commands without a shared command buffer",
            ));
        };
        let base = command_buffer.as_mut_ptr();
        let region_size = command_buffer.len() - SHARED_DATA_REGION_OFFSET;
        let range = usize::try_from(offset)
            .ok()
            .zip(usize::try_from(size_in_bytes).ok())
            .filter(|&(offset, size)| {
                offset.is_multiple_of(libcompositing_rust::webgl::wire::RECORD_ALIGNMENT)
                    && offset <= region_size
                    && size <= region_size - offset
            });
        let Some((offset, size)) = range else {
            return Err(ReplayError(
                "WebContent published an invalid WebGL shared command buffer range",
            ));
        };
        let mut scratch = core::mem::take(&mut context.scratch);
        // SAFETY: The range lies inside the region of records of the mapping, which is readable.
        let commands = unsafe { base.add(SHARED_DATA_REGION_OFFSET + offset) };
        let result = Self::replay(context, gl, host, client, canvas_id, image_sources, |replayer| {
            // SAFETY: The context holds the mapping while the records are replayed.
            unsafe { replay_records_in_shared_memory(replayer, &mut scratch, commands, size) }
        });
        // NB: The records were replayed, so WebContent may write over them once it learns that they were taken.
        // SAFETY: The mapping starts at a page, where the u64 that the sequence number is stored in is, and WebContent
        //         only reads it.
        unsafe { AtomicU64::from_ptr(base.cast::<u64>()) }.store(flush_sequence_number, Ordering::Release);
        context.give_back_scratch(scratch);
        result
    }

    pub fn execute_commands<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        commands: Option<&dyn MappedMemory>,
        size_in_bytes: u64,
        image_sources: &[ImageSource],
    ) -> Result<(), ReplayError> {
        let (context, gl) = self.context_and_gl(client, canvas_id)?;
        let commands = commands.ok_or(ReplayError("WebContent sent an invalid WebGL command buffer"))?;
        let size = usize::try_from(size_in_bytes)
            .ok()
            .filter(|&size| size <= commands.len())
            .ok_or(ReplayError(
                "WebContent sent more WebGL commands than their buffer holds",
            ))?;
        let mut scratch = core::mem::take(&mut context.scratch);
        let result = Self::replay(context, gl, host, client, canvas_id, image_sources, |replayer| {
            // SAFETY: The mapping holds as many readable bytes, and the caller holds it while the records are replayed.
            unsafe { replay_records_in_shared_memory(replayer, &mut scratch, commands.as_mut_ptr(), size) }
        });
        context.give_back_scratch(scratch);
        result
    }

    pub fn execute_sync_call<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        request: &[u8],
    ) -> Result<Vec<u8>, ReplayError> {
        let (context, gl) = self.context_and_gl(client, canvas_id)?;
        let mut scratch = core::mem::take(&mut context.scratch);
        // SAFETY: The request is readable.
        let request = unsafe { copy_to_scratch(&mut scratch, request.as_ptr(), request.len()) };
        let mut reply = Vec::new();
        let result = Self::replay(context, gl, host, client, canvas_id, &[], |replayer| {
            reply = replayer.handle_sync_request(request)?;
            Ok(())
        });
        context.give_back_scratch(scratch);
        result.map(|()| reply)
    }

    pub fn present<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        preserve_drawing_buffer: bool,
    ) -> Result<(), ReplayError> {
        let (context, _) = self.context_and_gl(client, canvas_id)?;
        match host.present_webgl_context(
            client,
            canvas_id,
            preserve_drawing_buffer,
            context.needs_clear_before_next_frame,
        ) {
            PresentOutcome::Presented => {
                if !preserve_drawing_buffer {
                    context.needs_clear_before_next_frame = true;
                }
            }
            PresentOutcome::ShowedPresentedFrame => {}
            PresentOutcome::NoDrawingBuffer => {
                eprintln!("Compositor: Cannot present a WebGL context without a drawing buffer");
            }
        }
        Ok(())
    }

    /// A context that is gone already is left alone.
    pub fn clear_drawing_buffer<H: WebGLHost>(&mut self, host: &H, client: WebContentClientId, canvas_id: CanvasId) {
        let Ok((context, gl)) = self.context_and_gl(client, canvas_id) else {
            return;
        };
        let defaults = host.make_webgl_context_current(client, canvas_id);
        clear_buffer_to_default_values(gl, context.webgl2, defaults);
        context.needs_clear_before_next_frame = false;
    }

    #[allow(clippy::too_many_arguments)]
    pub fn read_pixels<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        rect: IntRect,
        format: u32,
        r#type: u32,
        buffer_size: i32,
        pixels: Option<&dyn MappedMemory>,
    ) -> Result<ReadPixelsResult, ReplayError> {
        let (context, gl) = self.context_and_gl(client, canvas_id)?;
        let pixels_size = pixels.map_or(0, |pixels| pixels.len());
        if usize::try_from(buffer_size).map_or(true, |buffer_size| buffer_size > pixels_size) {
            return Err(ReplayError("WebContent sent an invalid WebGL readPixels buffer"));
        }
        let defaults = host.make_webgl_context_current(client, canvas_id);
        clear_drawing_buffer_if_needed(context, gl, defaults);
        let mut result = ReadPixelsResult {
            length: 0,
            columns: 0,
            rows: 0,
        };
        let destination = pixels.map_or(core::ptr::null_mut(), |pixels| pixels.as_mut_ptr());
        // SAFETY: The context is current, and GL writes at most `buffer_size` bytes, which the mapping holds.
        unsafe {
            (gl.read_pixels_robust_angle)(
                rect.x,
                rect.y,
                rect.width,
                rect.height,
                format,
                r#type,
                buffer_size,
                &raw mut result.length,
                &raw mut result.columns,
                &raw mut result.rows,
                destination.cast(),
            );
        }
        Ok(result)
    }

    #[allow(clippy::too_many_arguments)]
    pub fn read_buffer_sub_data<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        target: u32,
        offset: i64,
        size: i64,
        data: Option<&dyn MappedMemory>,
    ) -> Result<bool, ReplayError> {
        let (_, gl) = self.context_and_gl(client, canvas_id)?;
        let data_size = data.map_or(0, |data| data.len());
        let Some(size) = usize::try_from(size).ok().filter(|&size| size <= data_size) else {
            return Err(ReplayError("WebContent sent an invalid WebGL buffer readback target"));
        };
        host.make_webgl_context_current(client, canvas_id);
        // SAFETY: The context is current.
        let mapped = unsafe { (gl.map_buffer_range)(target, offset as isize, size as isize, GL_MAP_READ_BIT) };
        if mapped.is_null() {
            return Ok(false);
        }
        if let Some(data) = data {
            // SAFETY: GL mapped `size` bytes of the buffer, and the shared memory holds at least as many.
            unsafe { core::ptr::copy_nonoverlapping(mapped.cast::<u8>(), data.as_mut_ptr(), size) };
        }
        // SAFETY: The context is current, and the buffer of the target is mapped.
        unsafe { (gl.unmap_buffer)(target) };
        Ok(true)
    }

    pub fn read_back_drawing_buffer<H: WebGLHost>(
        &mut self,
        host: &H,
        client: WebContentClientId,
        canvas_id: CanvasId,
        rect: IntRect,
        pixels: Option<&dyn MappedMemory>,
    ) -> Result<IntSize, ReplayError> {
        let (context, _) = self.context_and_gl(client, canvas_id)?;
        let Some(pixels) = pixels else {
            return Ok(IntSize::default());
        };
        // SAFETY: The mapping holds as many writable bytes.
        let size = unsafe {
            host.read_back_webgl_drawing_buffer(
                client,
                canvas_id,
                context.needs_clear_before_next_frame,
                rect,
                pixels.as_mut_ptr(),
                pixels.len(),
            )
        };
        Ok(size.unwrap_or_default())
    }
}

fn clear_drawing_buffer_if_needed(context: &mut WebGLContext, gl: &GlFunctions, defaults: DefaultBuffers) {
    if !context.needs_clear_before_next_frame {
        return;
    }
    clear_buffer_to_default_values(gl, context.webgl2, defaults);
    context.needs_clear_before_next_frame = false;
}
