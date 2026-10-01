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
#[derive(Clone, Copy, Debug, Default)]
pub struct FfiImageFrameSnapshot {
    pub id: u64,
    pub width: i32,
    pub height: i32,
}

struct ImageFrameEntry {
    snapshot: FfiImageFrameSnapshot,
    raw: NonNull<c_void>,
}

// SAFETY: The entry owns an immutable Gfx::DecodedImageFrame copy. Its bitmap is
// atomically reference counted, and neither the frame nor its color space is
// mutated while the entry is live.
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
    pub fn as_raw(&self) -> *const c_void {
        self.0.raw.as_ptr()
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
