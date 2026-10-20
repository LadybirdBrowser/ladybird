/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points of the recorder that are written by hand: the GL functions that GLFunctions.json marks as
//! custom, because their arguments do not serialize the way their signature suggests, and the commands that are no
//! GL functions.

use std::rc::Rc;

use super::recorder::{Proxy, bytes_of_mut, copy_reply_data, until_nul};
use super::transport::{ReadPixelsResult, SharedMemory, Transport};
use super::vocabulary::{commands, sync_calls};
use super::wire::{DataSpan, InlineData, WireStruct, resolve_data_span};
use crate::web_gl::constants::{GL_INVALID_VALUE, GL_OUT_OF_MEMORY};

fn pack_strings(strings: &[&[u8]]) -> Vec<u8> {
    let mut packed_strings = Vec::new();
    for string in strings {
        packed_strings.extend_from_slice(until_nul(string));
        packed_strings.push(0);
    }
    packed_strings
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct FrameUpload {
    /// `None` uploads the frame at its own size.
    pub(crate) destination_size: Option<(i32, i32)>,
    pub(crate) flip_y: bool,
    pub(crate) premultiply_alpha: bool,
}

impl FrameUpload {
    fn destination_width(self) -> i32 {
        self.destination_size.map_or(0, |(width, _)| width)
    }

    fn destination_height(self) -> i32 {
        self.destination_size.map_or(0, |(_, height)| height)
    }
}

impl<T: Transport> Proxy<T> {
    fn string_count(&self, strings: &[&[u8]]) -> Option<i32> {
        let count = i32::try_from(strings.len()).ok();
        if count.is_none() && !self.is_lost() {
            self.set_pending_local_error(GL_INVALID_VALUE);
        }
        count
    }

    pub(crate) fn set_size(&self, width: i32, height: i32) {
        self.record(&commands::SetDrawingBufferSize { width, height });
    }

    #[expect(clippy::too_many_arguments, reason = "the arguments of the GL function")]
    pub(crate) fn read_pixels_into_pixel_pack_buffer(
        &self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        offset: i64,
    ) {
        self.record(&commands::ReadPixelsIntoPixelPackBuffer {
            x,
            y,
            width,
            height,
            format,
            r#type,
            offset,
        });
    }

    #[expect(
        clippy::too_many_arguments,
        reason = "the arguments of the GL function, and the frame"
    )]
    pub(crate) fn tex_image2d_from_bitmap(
        &self,
        target: u32,
        level: i32,
        internalformat: i32,
        format: u32,
        r#type: u32,
        frame: T::Frame,
        upload: FrameUpload,
    ) {
        if self.is_lost() {
            return;
        }
        let bitmap_index = self.append_pending_frame(frame);
        self.record(&commands::TexImage2DFromBitmap {
            target,
            level,
            internalformat,
            format,
            r#type,
            bitmap_index,
            has_explicit_destination_size: upload.destination_size.is_some(),
            destination_width: upload.destination_width(),
            destination_height: upload.destination_height(),
            flip_y: upload.flip_y,
            premultiply_alpha: upload.premultiply_alpha,
            ..commands::TexImage2DFromBitmap::ZEROED
        });
    }

    #[expect(
        clippy::too_many_arguments,
        reason = "the arguments of the GL function, and the frame"
    )]
    pub(crate) fn tex_sub_image2d_from_bitmap(
        &self,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        format: u32,
        r#type: u32,
        frame: T::Frame,
        upload: FrameUpload,
    ) {
        if self.is_lost() {
            return;
        }
        let bitmap_index = self.append_pending_frame(frame);
        self.record(&commands::TexSubImage2DFromBitmap {
            target,
            level,
            xoffset,
            yoffset,
            format,
            r#type,
            bitmap_index,
            has_explicit_destination_size: upload.destination_size.is_some(),
            destination_width: upload.destination_width(),
            destination_height: upload.destination_height(),
            flip_y: upload.flip_y,
            premultiply_alpha: upload.premultiply_alpha,
            ..commands::TexSubImage2DFromBitmap::ZEROED
        });
    }

    #[expect(
        clippy::too_many_arguments,
        reason = "the arguments of the GL function, and the frame"
    )]
    pub(crate) fn tex_image3d_from_bitmap(
        &self,
        target: u32,
        level: i32,
        internalformat: i32,
        depth: i32,
        format: u32,
        r#type: u32,
        frame: T::Frame,
        upload: FrameUpload,
    ) {
        if self.is_lost() {
            return;
        }
        let bitmap_index = self.append_pending_frame(frame);
        self.record(&commands::TexImage3DFromBitmap {
            target,
            level,
            internalformat,
            depth,
            format,
            r#type,
            bitmap_index,
            has_explicit_destination_size: upload.destination_size.is_some(),
            destination_width: upload.destination_width(),
            destination_height: upload.destination_height(),
            flip_y: upload.flip_y,
            premultiply_alpha: upload.premultiply_alpha,
            ..commands::TexImage3DFromBitmap::ZEROED
        });
    }

    #[expect(
        clippy::too_many_arguments,
        reason = "the arguments of the GL function, and the frame"
    )]
    pub(crate) fn tex_sub_image3d_from_bitmap(
        &self,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        zoffset: i32,
        depth: i32,
        format: u32,
        r#type: u32,
        frame: T::Frame,
        upload: FrameUpload,
    ) {
        if self.is_lost() {
            return;
        }
        let bitmap_index = self.append_pending_frame(frame);
        self.record(&commands::TexSubImage3DFromBitmap {
            target,
            level,
            xoffset,
            yoffset,
            zoffset,
            depth,
            format,
            r#type,
            bitmap_index,
            has_explicit_destination_size: upload.destination_size.is_some(),
            destination_width: upload.destination_width(),
            destination_height: upload.destination_height(),
            flip_y: upload.flip_y,
            premultiply_alpha: upload.premultiply_alpha,
            ..commands::TexSubImage3DFromBitmap::ZEROED
        });
    }

    /// The source is all of `source`, whatever it contains.
    pub(crate) fn shader_source(&self, shader: u32, source: &[u8]) {
        let source_data = InlineData::NulTerminated(source);
        self.record_with_data(
            &commands::ShaderSource {
                shader,
                source: DataSpan::following::<commands::ShaderSource>(source_data),
            },
            source_data,
        );
    }

    pub(crate) fn transform_feedback_varyings(&self, program: u32, varyings: &[&[u8]], buffer_mode: u32) {
        let Some(count) = self.string_count(varyings) else {
            return;
        };
        let packed_varyings = pack_strings(varyings);
        let varyings_data = InlineData::Bytes(&packed_varyings);
        self.record_with_data(
            &commands::TransformFeedbackVaryings {
                program,
                count,
                varyings: DataSpan::following::<commands::TransformFeedbackVaryings>(varyings_data),
                buffer_mode,
            },
            varyings_data,
        );
    }

    /// Leaves `uniform_indices` as it is when the context is lost or has an error for the names.
    pub(crate) fn get_uniform_indices(&self, program: u32, uniform_names: &[&[u8]], uniform_indices: &mut [u32]) {
        assert!(uniform_indices.len() >= uniform_names.len());
        let Some(uniform_count) = self.string_count(uniform_names) else {
            return;
        };
        let packed_names = pack_strings(uniform_names);
        let names_data = InlineData::Bytes(&packed_names);
        let request = sync_calls::GetUniformIndicesRequest {
            program,
            uniform_count,
            uniform_names: DataSpan::following::<sync_calls::GetUniformIndicesRequest>(names_data),
        };
        self.sync_call(&request, names_data, |reply, reply_bytes| {
            copy_reply_data(
                reply_bytes,
                reply.uniform_indices,
                bytes_of_mut(&mut uniform_indices[..uniform_names.len()]),
            );
        });
    }

    /// Without the NUL that ends it, and empty when the context is lost.
    pub(crate) fn get_string(&self, name: u32) -> Rc<[u8]> {
        if let Some(string) = self.cached_string(name) {
            return string;
        }
        let request = sync_calls::GetStringRequest { name };
        let string = self.sync_call(&request, InlineData::NONE, |reply, reply_bytes| {
            let string = resolve_data_span(reply_bytes, reply.value);
            assert!(
                string.last() == Some(&0),
                "the replayer ends the string that it returns with a NUL"
            );
            Rc::<[u8]>::from(until_nul(string))
        });
        match string {
            Some(string) => {
                self.cache_string(name, Rc::clone(&string));
                string
            }
            None => Rc::from([]),
        }
    }

    pub(crate) fn get_vertex_attrib_pointerv_robust_angle(&self, index: u32, pname: u32) -> i64 {
        let request = sync_calls::GetVertexAttribPointervRobustANGLERequest { index, pname };
        self.sync_call(&request, InlineData::NONE, |reply, _| reply.pointer)
            .unwrap_or(0)
    }

    /// `use_pixels` does not run when the context is lost or when there is no memory to read into, which is a GL
    /// error of the context.
    #[expect(clippy::too_many_arguments, reason = "the arguments of the GL function")]
    pub(crate) fn read_pixels_robust_angle(
        &self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        buf_size: i32,
        use_pixels: impl FnOnce(ReadPixelsResult, &[u8]),
    ) {
        if self.is_lost() {
            return;
        }
        let transport = self.transport();
        let shared_pixels = match usize::try_from(buf_size) {
            Ok(size) if size > 0 => {
                let Some(shared_pixels) = transport.create_shared_memory(size) else {
                    self.set_pending_local_error(GL_OUT_OF_MEMORY);
                    return;
                };
                Some(shared_pixels)
            }
            _ => None,
        };

        self.flush_commands();
        let result =
            transport.read_pixels_robust_angle(x, y, width, height, format, r#type, buf_size, shared_pixels.as_ref());
        if self.is_lost() {
            return;
        }
        let pixels = match (&shared_pixels, usize::try_from(result.length)) {
            (Some(shared_pixels), Ok(length)) if length > 0 => {
                assert!(
                    length <= shared_pixels.len(),
                    "the replayer reads no more pixels than it has room for"
                );
                // SAFETY: The memory is mapped and at least `length` bytes long, and the replayer, which is the only
                //         other user of it, has answered the call and does not write to it any more.
                unsafe { ak::slice_from_raw_parts(shared_pixels.as_ptr(), length) }
            }
            _ => &[],
        };
        use_pixels(result, pixels);
    }

    /// False when the context is lost, when the replayer refuses, or when there is no memory to read into, which is a
    /// GL error of the context.
    pub(crate) fn read_buffer_sub_data(
        &self,
        target: u32,
        offset: i64,
        size: usize,
        use_data: impl FnOnce(&[u8]),
    ) -> bool {
        if self.is_lost() {
            return false;
        }
        if size == 0 {
            return true;
        }
        let Ok(size_for_replayer) = i64::try_from(size) else {
            self.set_pending_local_error(GL_OUT_OF_MEMORY);
            return false;
        };
        let transport = self.transport();
        let Some(shared_data) = transport.create_shared_memory(size) else {
            self.set_pending_local_error(GL_OUT_OF_MEMORY);
            return false;
        };

        self.flush_commands();
        if !transport.read_buffer_sub_data(target, offset, size_for_replayer, &shared_data) {
            return false;
        }
        if self.is_lost() {
            return false;
        }
        assert!(size <= shared_data.len());
        // SAFETY: The memory is mapped and at least `size` bytes long, and the replayer, which is the only other
        //         user of it, has answered the call and does not write to it any more.
        use_data(unsafe { ak::slice_from_raw_parts(shared_data.as_ptr(), size) });
        true
    }
}
