/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
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

/// The bitmap data of a frame as Libraries/LibWeb/HTML/ImageBitmap.cpp hands it over and creates a frame from: the
/// values of its Gfx::Bitmap and Gfx::ColorSpace, and bytes that borrow from the frame or from the caller.
#[repr(C)]
struct FramePixels {
    width: i32,
    height: i32,
    pitch: u64,
    format: i32,
    alpha_type: i32,
    data: *const u8,
    data_size: usize,
    color_space_kind: u8,
    color_primaries: u8,
    transfer_characteristics: u8,
    icc_profile: *const u8,
    icc_profile_size: usize,
}

unsafe extern "C" {
    // Defined in Libraries/LibWeb/HTML/ImageBitmap.cpp, which encodes and decodes the Gfx::Bitmap of a transferred
    // frame. A frame that it hands over is a Gfx::DecodedImageFrame allocated with new, or null for none.
    fn web_image_bitmap_frame_size_in_bytes(frame: *const c_void) -> usize;
    fn web_image_bitmap_frame_pixels(frame: *const c_void, pixels: &mut FramePixels);
    fn web_image_bitmap_create_frame(
        realm: *mut c_void,
        pixels: &FramePixels,
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

const BITMAP_KIND_NONE: u8 = 0;
const BITMAP_KIND_SRGB: u8 = 1;
const BITMAP_KIND_WITH_COLOR_SPACE: u8 = 2;

const COLOR_SPACE_KIND_SRGB: u8 = 0;
const COLOR_SPACE_KIND_CICP: u8 = 1;
const COLOR_SPACE_KIND_ICC_PROFILE: u8 = 2;

const BITMAP_FORMATS: [(i32, &[u8]); 4] = [(1, b"BGRx8888"), (2, b"BGRA8888"), (3, b"RGBx8888"), (4, b"RGBA8888")];

const ALPHA_TYPES: [(i32, &[u8]); 2] = [(0, b"premultiplied"), (1, b"unpremultiplied")];

fn data_clone_error(message: &[u8]) -> Exception {
    Exception::Dom("DataCloneError", Utf16String::from_ascii(message))
}

/// Writes a value of Gfx as a record holds one: by its identifier in a storage record, and by its underlying value in
/// one for IPC.
fn serialize_enumeration(serialized: &mut SerializationWriter, values: &[(i32, &[u8])], value: i32, for_storage: bool) {
    if !for_storage {
        serialized.write_i32(value);
        return;
    }
    let (_, identifier) = values
        .iter()
        .find(|(underlying_value, _)| *underlying_value == value)
        .expect("a bitmap has a format and an alpha type that a record holds");
    serialized.write_utf16_string(&Utf16String::from_ascii(identifier));
}

/// Reads what serialize_enumeration() writes, which throws a "DataCloneError" DOMException with the message of an
/// invalid underlying value or of an unknown identifier for one that it does not know.
fn deserialize_enumeration(
    serialized: &mut SerializationReader,
    values: &[(i32, &[u8])],
    invalid_message: &[u8],
    unknown_message: &[u8],
) -> WebIdlResult<i32> {
    if !serialized.is_for_storage() {
        let value = serialized.read_i32()?;
        if !values.iter().any(|(underlying_value, _)| *underlying_value == value) {
            return Err(data_clone_error(invalid_message));
        }
        return Ok(value);
    }
    let identifier = serialized.read_utf16_string()?;
    values
        .iter()
        .find(|(_, value_identifier)| identifier == Utf16String::from_ascii(value_identifier))
        .map(|(value, _)| *value)
        .ok_or_else(|| data_clone_error(unknown_message))
}

/// Writes a copy of the bitmap data of the frame, or that there is none.
fn serialize_bitmap_data(serialized: &mut SerializationWriter, frame: *const c_void, for_storage: bool) {
    if frame.is_null() {
        serialized.write_u8(BITMAP_KIND_NONE);
        return;
    }
    let mut pixels = FramePixels {
        width: 0,
        height: 0,
        pitch: 0,
        format: 0,
        alpha_type: 0,
        data: core::ptr::null(),
        data_size: 0,
        color_space_kind: COLOR_SPACE_KIND_SRGB,
        color_primaries: 0,
        transfer_characteristics: 0,
        icc_profile: core::ptr::null(),
        icc_profile_size: 0,
    };
    // SAFETY: The frame is live.
    unsafe { web_image_bitmap_frame_pixels(frame, &mut pixels) };

    let has_color_space = pixels.color_space_kind != COLOR_SPACE_KIND_SRGB;
    serialized.write_u8(if has_color_space {
        BITMAP_KIND_WITH_COLOR_SPACE
    } else {
        BITMAP_KIND_SRGB
    });
    serialized.write_i32(pixels.width);
    serialized.write_i32(pixels.height);
    serialized.write_u64(pixels.pitch);
    serialize_enumeration(serialized, &BITMAP_FORMATS, pixels.format, for_storage);
    serialize_enumeration(serialized, &ALPHA_TYPES, pixels.alpha_type, for_storage);
    // SAFETY: The bytes borrow from the frame, which lives for the call.
    serialized.write_bytes(unsafe { ak::slice_from_raw_parts(pixels.data, pixels.data_size) });
    if !has_color_space {
        return;
    }
    serialized.write_u8(pixels.color_space_kind);
    if pixels.color_space_kind == COLOR_SPACE_KIND_CICP {
        serialized.write_u8(pixels.color_primaries);
        serialized.write_u8(pixels.transfer_characteristics);
    } else {
        // SAFETY: The bytes borrow from the frame, which lives for the call.
        serialized.write_bytes(unsafe { ak::slice_from_raw_parts(pixels.icc_profile, pixels.icc_profile_size) });
    }
}

/// Reads what serialize_bitmap_data() writes, and creates the bitmap data in the realm of the call context.
fn deserialize_bitmap_data(cx: &CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Option<BitmapData>> {
    let kind = serialized.read_u8()?;
    if kind == BITMAP_KIND_NONE {
        return Ok(None);
    }
    if kind != BITMAP_KIND_SRGB && kind != BITMAP_KIND_WITH_COLOR_SPACE {
        return Err(data_clone_error(b"Invalid ImageBitmap bitmap data"));
    }
    let width = serialized.read_i32()?;
    let height = serialized.read_i32()?;
    let pitch = serialized.read_u64()?;
    let format = deserialize_enumeration(
        serialized,
        &BITMAP_FORMATS,
        b"Invalid structured serialize bitmap format",
        b"Unknown structured serialize bitmap format",
    )?;
    let alpha_type = deserialize_enumeration(
        serialized,
        &ALPHA_TYPES,
        b"Invalid structured serialize alpha type",
        b"Unknown structured serialize alpha type",
    )?;
    let data = serialized.read_bytes()?;

    let (mut color_space_kind, mut color_primaries, mut transfer_characteristics) = (COLOR_SPACE_KIND_SRGB, 0, 0);
    let mut icc_profile = Vec::new();
    if kind == BITMAP_KIND_WITH_COLOR_SPACE {
        color_space_kind = serialized.read_u8()?;
        match color_space_kind {
            COLOR_SPACE_KIND_SRGB => {}
            COLOR_SPACE_KIND_CICP => {
                color_primaries = serialized.read_u8()?;
                transfer_characteristics = serialized.read_u8()?;
            }
            COLOR_SPACE_KIND_ICC_PROFILE => icc_profile = serialized.read_bytes()?,
            _ => return Err(data_clone_error(b"Invalid ImageBitmap color space")),
        }
    }

    let pixels = FramePixels {
        width,
        height,
        pitch,
        format,
        alpha_type,
        data: data.as_ptr(),
        data_size: data.len(),
        color_space_kind,
        color_primaries,
        transfer_characteristics,
        icc_profile: icc_profile.as_ptr(),
        icc_profile_size: icc_profile.len(),
    };
    let mut frame = core::ptr::null_mut();
    // SAFETY: The realm is live, and the call borrows the bytes.
    unsafe { web_image_bitmap_create_frame(cx.realm().as_ptr().cast(), &pixels, &mut frame) }.into_completion()?;
    // SAFETY: C++ hands over the frame it created.
    Ok(unsafe { BitmapData::from_handed_over_frame(frame) })
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
        for_storage: bool,
    ) -> WebIdlResult<()> {
        // 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.
        if !self.origin_clean.get() {
            return Err(Exception::Dom(
                "DataCloneError",
                Utf16String::from_ascii(b"Cannot serialize an ImageBitmap that is not origin-clean"),
            ));
        }

        // 2. Set serialized.[[BitmapData]] to a copy of value's bitmap data.
        serialize_bitmap_data(serialized, self.raw_frame(), for_storage);

        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:deserialization-steps
    fn deserialization_steps(cx: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        // 1. Set value's bitmap data to serialized.[[BitmapData]].
        Ok(Self::new(deserialize_bitmap_data(cx, serialized)?))
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
