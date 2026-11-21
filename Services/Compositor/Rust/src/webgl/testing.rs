/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use core::ffi::{CStr, c_void};

use libcompositing_rust::display_list::commands::{CanvasId, VideoSinkHandle};
use libcompositing_rust::webgl::ImageSource;
use libgfx_rust::{IntRect, IntSize};

use super::host::{
    ContextOptions, DefaultBuffers, ImageConversion, ImageConversionOutcome, MappedMemory, PresentOutcome,
    VideoFrameCopy, WebGLHost,
};
use crate::host::WebContentClientId;

thread_local! {
    static CALLS: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    static NEXT_NAME: Cell<u32> = const { Cell::new(1) };
}

pub fn record(call: String) {
    CALLS.with_borrow_mut(|calls| calls.push(call));
}

pub fn take_calls() -> Vec<String> {
    CALLS.take()
}

pub fn next_name() -> u32 {
    NEXT_NAME.replace(NEXT_NAME.get() + 1)
}

pub fn reset() {
    take_calls();
    NEXT_NAME.set(1);
}

pub struct TestMemory {
    data: *mut u8,
    len: usize,
    _storage: Box<[u64]>,
}

impl TestMemory {
    pub fn new(bytes: &[u8]) -> Self {
        let mut storage = vec![0_u64; bytes.len().div_ceil(8)].into_boxed_slice();
        let data = storage.as_mut_ptr().cast::<u8>();
        // SAFETY: The storage has room for the bytes.
        unsafe { core::ptr::copy_nonoverlapping(bytes.as_ptr(), data, bytes.len()) };
        Self {
            data,
            len: bytes.len(),
            _storage: storage,
        }
    }

    pub fn bytes(&self) -> &[u8] {
        // SAFETY: The storage holds `len` bytes, and nothing else writes them during a test.
        unsafe { ak::slice_from_raw_parts(self.data, self.len) }
    }
}

impl MappedMemory for TestMemory {
    fn as_mut_ptr(&self) -> *mut u8 {
        self.data
    }

    fn len(&self) -> usize {
        self.len
    }
}

pub struct FakeHost {
    pub next_canvas_id: Cell<u64>,
    pub defaults: Cell<DefaultBuffers>,
    pub can_copy_images_into_textures: Cell<bool>,
    pub held_video_frame_size: Cell<Option<IntSize>>,
    pub copy_succeeds: Cell<bool>,
    pub conversion: RefCell<Option<ImageConversionOutcome<Vec<u8>>>>,
    pub has_presented_frame: Cell<bool>,
}

impl Default for FakeHost {
    fn default() -> Self {
        Self {
            next_canvas_id: Cell::new(1),
            defaults: Cell::new(DefaultBuffers {
                framebuffer: 100,
                renderbuffer: 200,
            }),
            can_copy_images_into_textures: Cell::new(false),
            held_video_frame_size: Cell::new(None),
            copy_succeeds: Cell::new(true),
            conversion: RefCell::new(None),
            has_presented_frame: Cell::new(false),
        }
    }
}

impl WebGLHost for FakeHost {
    type Pixels = Vec<u8>;
    type VideoFrame = u64;
    type VideoFrameImage = u64;

    fn gl_proc_address(&self, _: &CStr) -> *const c_void {
        core::ptr::null()
    }

    fn allocate_canvas_id(&self) -> CanvasId {
        CanvasId(self.next_canvas_id.replace(self.next_canvas_id.get() + 1))
    }

    fn create_webgl_context(&self, client: WebContentClientId, canvas_id: CanvasId, options: ContextOptions) -> bool {
        record(format!(
            "host.create({}, {}, {}x{})",
            client.0, canvas_id.0, options.size.width, options.size.height
        ));
        true
    }

    fn destroy_webgl_context(&self, client: WebContentClientId, canvas_id: CanvasId) {
        record(format!("host.destroy({}, {})", client.0, canvas_id.0));
    }

    fn make_webgl_context_current(&self, _: WebContentClientId, _: CanvasId) -> DefaultBuffers {
        record("host.make_current".into());
        self.defaults.get()
    }

    fn set_webgl_drawing_buffer_size(&self, _: WebContentClientId, _: CanvasId, size: IntSize) {
        record(format!("host.set_size({}x{})", size.width, size.height));
    }

    fn present_webgl_context(
        &self,
        _: WebContentClientId,
        _: CanvasId,
        preserve_drawing_buffer: bool,
        show_presented_frame: bool,
    ) -> PresentOutcome {
        record(format!(
            "host.present({preserve_drawing_buffer}, {show_presented_frame})"
        ));
        if show_presented_frame && self.has_presented_frame.get() {
            return PresentOutcome::ShowedPresentedFrame;
        }
        self.has_presented_frame.set(true);
        PresentOutcome::Presented
    }

    unsafe fn read_back_webgl_drawing_buffer(
        &self,
        _: WebContentClientId,
        _: CanvasId,
        from_presented_frame: bool,
        rect: IntRect,
        _: *mut u8,
        _: usize,
    ) -> Option<IntSize> {
        record(format!("host.read_back({from_presented_frame})"));
        Some(IntSize {
            width: rect.width,
            height: rect.height,
        })
    }

    fn webgl_can_copy_images_into_textures(&self, _: WebContentClientId, _: CanvasId) -> bool {
        record("host.can_copy".into());
        self.can_copy_images_into_textures.get()
    }

    fn held_video_frame(
        &self,
        _: WebContentClientId,
        sink_handle: VideoSinkHandle,
        generation: u64,
    ) -> Option<(u64, IntSize)> {
        record(format!("host.held_video_frame({}, {generation})", sink_handle.0));
        self.held_video_frame_size.get().map(|size| (sink_handle.0, size))
    }

    fn video_frame_image(&self, frame: &u64) -> Option<u64> {
        record(format!("host.video_frame_image({frame})"));
        Some(*frame)
    }

    fn copy_video_frame_image_into_bound_texture(
        &self,
        _: WebContentClientId,
        _: CanvasId,
        image: &u64,
        copy: VideoFrameCopy,
    ) -> bool {
        record(format!("host.copy({image}, {}x{})", copy.size.width, copy.size.height));
        self.copy_succeeds.get()
    }

    fn convert_image_source(
        &self,
        _: WebContentClientId,
        source: ImageSource,
        conversion: ImageConversion,
    ) -> ImageConversionOutcome<Vec<u8>> {
        record(format!("host.convert({source:?}, {:?})", conversion.export_format));
        self.conversion
            .take()
            .expect("the test sets up what the conversion answers")
    }
}
