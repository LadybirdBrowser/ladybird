/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

unsafe extern "C" {
    fn ladybird_gfx_decoded_image_frame_retain(
        frame: *const c_void,
        out_snapshot: *mut FfiImageFrameSnapshot,
    ) -> *mut c_void;
    fn ladybird_gfx_decoded_image_frame_release(frame: *mut c_void);
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FfiImageFrameSnapshot {
    pub id: u64,
    pub width: i32,
    pub height: i32,
    pub bitmap_format_value: i32,
    pub alpha_type_value: i32,
    pub pixels: *const u8,
    pub pitch: usize,
}

impl Default for FfiImageFrameSnapshot {
    fn default() -> Self {
        Self {
            id: 0,
            width: 0,
            height: 0,
            bitmap_format_value: 0,
            alpha_type_value: 0,
            pixels: std::ptr::null(),
            pitch: 0,
        }
    }
}

const BYTES_PER_PIXEL: usize = 4;
const BITMAP_FORMAT_VALUE_OF_RGBA8888: i32 = 4;
const ALPHA_TYPE_VALUE_OF_UNPREMULTIPLIED: i32 = 1;

struct ImageFrameEntry {
    snapshot: FfiImageFrameSnapshot,
    raw: NonNull<c_void>,
}

// SAFETY: The entry owns an immutable Gfx::DecodedImageFrame copy. The shared
// memory or IOSurface of its pixels is atomically reference counted, and
// neither the frame nor its color space is mutated while the entry is live.
unsafe impl Send for ImageFrameEntry {}
// SAFETY: See the Send implementation above.
unsafe impl Sync for ImageFrameEntry {}

impl Drop for ImageFrameEntry {
    fn drop(&mut self) {
        // SAFETY: ImageFrameHandle::retain took the copy this releases.
        unsafe { ladybird_gfx_decoded_image_frame_release(self.raw.as_ptr()) };
    }
}

#[derive(Clone)]
pub struct ImageFrameHandle(Arc<ImageFrameEntry>);

impl ImageFrameHandle {
    /// # Safety
    ///
    /// `frame` must point to a live `Gfx::DecodedImageFrame`.
    pub unsafe fn retain(frame: *const c_void) -> Self {
        let mut snapshot = FfiImageFrameSnapshot::default();
        // SAFETY: The caller guarantees the frame is live, and the out-pointer
        // addresses a local. The returned copy is owned by the entry.
        let raw = unsafe { ladybird_gfx_decoded_image_frame_retain(frame, &raw mut snapshot) };
        let raw = NonNull::new(raw).expect("Gfx::DecodedImageFrame copy must not be null");
        Self(Arc::new(ImageFrameEntry { snapshot, raw }))
    }

    /// Takes over a frame that C++ allocated with `new` to hand it over, as when it decodes one for Rust.
    ///
    /// # Safety
    ///
    /// `frame` must point to a live `Gfx::DecodedImageFrame` that C++ allocated with `new`, which nothing uses after
    /// this.
    pub unsafe fn adopt(frame: *mut c_void) -> Self {
        // SAFETY: The caller passes a live frame.
        let handle = unsafe { Self::retain(frame) };
        // SAFETY: The caller hands the frame over, and the handle holds a copy of it.
        unsafe { ladybird_gfx_decoded_image_frame_release(frame) };
        handle
    }

    #[inline]
    pub fn id(&self) -> u64 {
        self.0.snapshot.id
    }

    #[inline]
    pub fn width(&self) -> i32 {
        self.0.snapshot.width
    }

    #[inline]
    pub fn height(&self) -> i32 {
        self.0.snapshot.height
    }

    #[inline]
    pub fn bitmap_format_value(&self) -> i32 {
        self.0.snapshot.bitmap_format_value
    }

    #[inline]
    pub fn alpha_type_value(&self) -> i32 {
        self.0.snapshot.alpha_type_value
    }

    #[inline]
    pub fn has_unpremultiplied_rgba8888_pixels(&self) -> bool {
        self.bitmap_format_value() == BITMAP_FORMAT_VALUE_OF_RGBA8888
            && self.alpha_type_value() == ALPHA_TYPE_VALUE_OF_UNPREMULTIPLIED
    }

    /// The bytes between the starts of two rows, which can be more than the rows hold.
    #[inline]
    pub fn pitch(&self) -> usize {
        self.0.snapshot.pitch
    }

    #[inline]
    pub fn minimum_pitch(&self) -> usize {
        usize::try_from(self.width()).expect("a frame has a width") * BYTES_PER_PIXEL
    }

    #[inline]
    pub fn size_in_bytes(&self) -> usize {
        self.pitch() * usize::try_from(self.height()).expect("a frame has a height")
    }

    /// The first of `size_in_bytes()` bytes of pixels, in rows `pitch()` bytes apart, which the handle keeps mapped.
    #[inline]
    pub fn pixels(&self) -> NonNull<u8> {
        NonNull::new(self.0.snapshot.pixels.cast_mut()).expect("a frame has pixels")
    }

    #[inline]
    pub fn as_raw(&self) -> *const c_void {
        self.0.raw.as_ptr()
    }

    /// A copy of the frame that C++ allocates with `new` and takes over: the opposite of `adopt`.
    pub fn hand_over_copy(&self) -> *mut c_void {
        let mut snapshot_of_the_copy = self.0.snapshot;
        // SAFETY: The handle keeps the frame live, and the out-pointer addresses a local.
        unsafe { ladybird_gfx_decoded_image_frame_retain(self.as_raw(), &raw mut snapshot_of_the_copy) }
    }
}

impl PartialEq for ImageFrameHandle {
    fn eq(&self, other: &Self) -> bool {
        self.id() == other.id()
    }
}

impl Eq for ImageFrameHandle {}

impl std::fmt::Debug for ImageFrameHandle {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("ImageFrameHandle")
            .field("id", &self.id())
            .field("width", &self.width())
            .field("height", &self.height())
            .finish()
    }
}
