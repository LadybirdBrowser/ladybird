/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#imagebitmap

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{RawNativeFunctionResult, Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::structured_data::{
    Serializable, SerializationReader, SerializationWriter, TransferDataReader, TransferDataWriter, Transferable,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    // Defined in Libraries/LibWeb/HTML/ImageBitmap.cpp, which encodes and decodes the Gfx::Bitmap of a frame. A frame
    // that it hands over is a Gfx::DecodedImageFrame allocated with new, or null for none.
    fn web_image_bitmap_frame_size_in_bytes(frame: *const c_void) -> usize;
    fn web_image_bitmap_serialize_bitmap(serialized: *mut c_void, frame: *const c_void);
    fn web_image_bitmap_deserialize_bitmap(
        serialized: *mut c_void,
        out_frame: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_image_bitmap_transfer_bitmap(data_holder: *mut c_void, frame: *const c_void) -> RawNativeFunctionResult;
    fn web_image_bitmap_receive_bitmap(
        data_holder: *mut c_void,
        out_frame: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
}

/// The bitmap data of an ImageBitmap: an image frame, which has the bitmap and keeps its id, so that each drawing of
/// the ImageBitmap names the same image.
struct BitmapData {
    frame: ImageFrameHandle,
    /// The size of the pixels of the bitmap, which the ImageBitmap counts as memory that it owns outside the heap.
    size_in_bytes: usize,
}

impl BitmapData {
    fn new(frame: ImageFrameHandle) -> Self {
        // SAFETY: The handle keeps the frame alive.
        let size_in_bytes = unsafe { web_image_bitmap_frame_size_in_bytes(frame.as_raw()) };
        Self { frame, size_in_bytes }
    }

    /// The bitmap data that C++ hands over, if any.
    ///
    /// # Safety
    ///
    /// `frame` must be a Gfx::DecodedImageFrame that C++ allocated with new for this, or null.
    unsafe fn from_handed_over_frame(frame: *mut c_void) -> Option<Self> {
        // SAFETY: The caller hands over a frame that C++ allocated with new, which nothing uses after this.
        (!frame.is_null()).then(|| Self::new(unsafe { ImageFrameHandle::adopt(frame) }))
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ImageBitmap {
    /// The bitmap data, which close() and the transfer steps unset.
    #[gc(untraced)]
    bitmap: RefCell<Option<BitmapData>>,
    /// The size of the bitmap data the ImageBitmap was created with, in CSS pixels.
    #[gc(untraced)]
    width: u32,
    #[gc(untraced)]
    height: u32,
    #[gc(untraced)]
    detached: Cell<bool>,
    /// An ImageBitmap object's bitmap has an origin-clean flag, which indicates whether the bitmap is tainted by
    /// content from a different origin. The flag is initially set to true and may be changed to false by the steps of
    /// createImageBitmap().
    #[gc(untraced)]
    origin_clean: Cell<bool>,
}

impl Wrappable for ImageBitmap {
    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        // NB: LibGC asks while it sweeps, when nothing borrows the bitmap data mutably.
        self.bitmap
            .try_borrow()
            .ok()
            .and_then(|bitmap| bitmap.as_ref().map(|bitmap| bitmap.size_in_bytes))
            .unwrap_or(0)
    }
}

impl ImageBitmap {
    fn new(bitmap: Option<BitmapData>) -> Self {
        let (width, height) = bitmap.as_ref().map_or((0, 0), |bitmap| {
            (
                u32::try_from(bitmap.frame.width()).expect("a bitmap has a width"),
                u32::try_from(bitmap.frame.height()).expect("a bitmap has a height"),
            )
        });
        Self {
            bitmap: RefCell::new(bitmap),
            width,
            height,
            detached: Cell::new(false),
            origin_clean: Cell::new(true),
        }
    }

    /// A new ImageBitmap whose bitmap data is the frame, or that has none.
    pub(crate) fn create(vm: &Vm, frame: Option<ImageFrameHandle>) -> Ref<Self> {
        allocate(vm, |_| Self::new(frame.map(BitmapData::new)))
    }

    /// The frame of the bitmap data, unless the ImageBitmap has none, as when it is closed or transferred.
    pub(crate) fn frame(&self) -> Option<ImageFrameHandle> {
        self.bitmap.borrow().as_ref().map(|bitmap| bitmap.frame.clone())
    }

    /// The raw frame of the bitmap data, which stays valid until the bitmap data is unset.
    pub(crate) fn raw_frame(&self) -> *const c_void {
        self.bitmap
            .borrow()
            .as_ref()
            .map_or(core::ptr::null(), |bitmap| bitmap.frame.as_raw())
    }

    pub(crate) fn is_detached(&self) -> bool {
        self.detached.get()
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    pub(crate) fn origin_clean(&self) -> bool {
        self.origin_clean.get()
    }

    pub(crate) fn set_origin_clean(&self, origin_clean: bool) {
        self.origin_clean.set(origin_clean);
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-width
    pub(crate) fn width(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.width_of_bitmap())
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-height
    pub(crate) fn height(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.height_of_bitmap())
    }

    pub(crate) fn width_of_bitmap(&self) -> u32 {
        // 1. If this's [[Detached]] internal slot's value is true, then return 0.
        if self.detached.get() {
            return 0;
        }
        // 2. Return this's width, in CSS pixels.
        self.width
    }

    pub(crate) fn height_of_bitmap(&self) -> u32 {
        // 1. If this's [[Detached]] internal slot's value is true, then return 0.
        if self.detached.get() {
            return 0;
        }
        // 2. Return this's height, in CSS pixels.
        self.height
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-close
    pub(crate) fn close(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.close_bitmap();
        Ok(())
    }

    pub(crate) fn close_bitmap(&self) {
        // 1. Set this's [[Detached]] internal slot value to true.
        self.detached.set(true);

        // 2. Unset this's bitmap data.
        self.bitmap.replace(None);
    }
}

impl Serializable for ImageBitmap {
    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:serialization-steps
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        // 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.
        if !self.origin_clean.get() {
            return Err(Exception::Dom(
                "DataCloneError",
                Utf16String::from_ascii(b"Cannot serialize an ImageBitmap that is not origin-clean"),
            ));
        }

        // 2. Set serialized.[[BitmapData]] to a copy of value's bitmap data.
        // SAFETY: The writer is lent for the steps, and the frame lives while the bitmap data is set.
        unsafe { web_image_bitmap_serialize_bitmap(serialized.as_raw(), self.raw_frame()) };

        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:deserialization-steps
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        // 1. Set value's bitmap data to serialized.[[BitmapData]].
        let mut frame = core::ptr::null_mut();
        // SAFETY: The reader is lent for the steps.
        unsafe { web_image_bitmap_deserialize_bitmap(serialized.as_raw(), &mut frame) }.into_completion()?;
        // SAFETY: C++ hands over the frame it decoded, or null.
        Ok(Self::new(unsafe { BitmapData::from_handed_over_frame(frame) }))
    }
}

impl Transferable for ImageBitmap {
    fn detached(&self) -> &Cell<bool> {
        &self.detached
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-steps
    fn transfer_steps(this: &Ref<Self>, _: &mut CallContext, data_holder: &mut TransferDataWriter) -> WebIdlResult<()> {
        // 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.
        if !this.origin_clean.get() {
            return Err(Exception::Dom(
                "DataCloneError",
                Utf16String::from_ascii(b"Cannot transfer an ImageBitmap that is not origin-clean"),
            ));
        }

        // 2. Set dataHolder.[[BitmapData]] to value's bitmap data.
        // NB: The pixels go to the shared memory of the transfer if there is room for them there, and inline otherwise.
        // SAFETY: The data holder is lent for the steps, and the frame lives while the bitmap data is set.
        unsafe { web_image_bitmap_transfer_bitmap(data_holder.as_raw(), this.raw_frame()) }.into_completion()?;

        // 3. Unset value's bitmap data.
        this.bitmap.replace(None);

        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-receiving-steps
    fn transfer_receiving_steps(cx: &mut CallContext, data_holder: &mut TransferDataReader) -> WebIdlResult<Ref<Self>> {
        // 1. Set value's bitmap data to dataHolder.[[BitmapData]].
        let mut frame = core::ptr::null_mut();
        // SAFETY: The data holder is lent for the steps.
        unsafe { web_image_bitmap_receive_bitmap(data_holder.as_raw(), &mut frame) }.into_completion()?;
        // SAFETY: C++ hands over the frame it decoded, or null.
        Ok(allocate(cx.vm(), |_| {
            Self::new(unsafe { BitmapData::from_handed_over_frame(frame) })
        }))
    }
}
