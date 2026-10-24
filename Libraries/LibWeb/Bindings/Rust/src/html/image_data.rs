/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#imagedata

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferOrder, ArrayBufferShared, Gc, RawNativeFunctionResult, Trace, TypedArrayBase,
    TypedArrayKind, Value, create_typed_array_on_buffer, is_typed_array_out_of_bounds,
    make_typed_array_with_buffer_witness_record, typed_array_byte_length,
};

use super::predefined_color_space::PredefinedColorSpace;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    // Defined in Libraries/LibWeb/HTML/ImageData.cpp, which encodes the color space as StructuredSerialize encodes a
    // Web::Bindings::PredefinedColorSpace.
    fn web_image_data_serialize_color_space(serialized: *mut c_void, color_space: PredefinedColorSpace);
    fn web_image_data_deserialize_color_space(
        serialized: *mut c_void,
        out_color_space: &mut u8,
    ) -> RawNativeFunctionResult;
}

/// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#imagedatasettings
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct ImageDataSettings {
    pub(crate) color_space: Option<PredefinedColorSpace>,
}

/// The size of a pixel of an ImageData, whose pixels are RGBA8888.
const BYTES_PER_PIXEL: u32 = 4;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ImageData {
    /// The Uint8ClampedArray of the data attribute, in whose buffer the pixels are, RGBA8888 and unpremultiplied, row
    /// by row. Script can detach the buffer, and shrink one that is resizable, at any time.
    data: Gc<TypedArrayBase>,
    #[gc(untraced)]
    width: u32,
    #[gc(untraced)]
    height: u32,
    #[gc(untraced)]
    color_space: PredefinedColorSpace,
}

impl Wrappable for ImageData {}

/// Whether the typed array has room for the pixels of an ImageData of the size, so that they can be read and written
/// in it.
fn can_hold_pixels(data: &TypedArrayBase, width: u32, height: u32) -> bool {
    let data_record = make_typed_array_with_buffer_witness_record(data, ArrayBufferOrder::SeqCst);
    if is_typed_array_out_of_bounds(&data_record) {
        return false;
    }
    let Some(expected_byte_length) = (width as usize)
        .checked_mul(height as usize)
        .and_then(|pixels| pixels.checked_mul(BYTES_PER_PIXEL as usize))
    else {
        return false;
    };
    typed_array_byte_length(&data_record) as usize >= expected_byte_length
}

impl ImageData {
    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagedata
    pub(crate) fn construct_with_size(
        cx: &mut CallContext,
        sw: u32,
        sh: u32,
        settings: ImageDataSettings,
    ) -> WebIdlResult<Self> {
        // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
        if sw == 0 || sh == 0 {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"The source width and height must be greater than zero.",
            )));
        }

        // 2. Initialize this given sw, sh, and settings set to settings.
        // 3. Initialize the image data of this to transparent black.
        Self::initialize(cx, sh, sw, Some(&settings), None, None)
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagedata-with-data
    pub(crate) fn construct_with_data(
        cx: &mut CallContext,
        data: Gc<TypedArrayBase>,
        sw: u32,
        sh: Option<u32>,
        settings: ImageDataSettings,
    ) -> WebIdlResult<Self> {
        let data_record = make_typed_array_with_buffer_witness_record(&data, ArrayBufferOrder::SeqCst);
        if is_typed_array_out_of_bounds(&data_record) {
            return Err(invalid_state_error(b"Source data must not be out-of-bounds."));
        }

        // 1. Let length be the number of bytes in data.
        let length = typed_array_byte_length(&data_record);

        // 2. If length is not a nonzero integral multiple of four, then throw an "InvalidStateError" DOMException.
        if length == 0 || !length.is_multiple_of(BYTES_PER_PIXEL) {
            return Err(invalid_state_error(
                b"Source data must have a non-zero length that is a multiple of four.",
            ));
        }

        // 3. Let length be length divided by four.
        let length = length / BYTES_PER_PIXEL;

        // 4. If length is not an integral multiple of sw, then throw an "IndexSizeError" DOMException.
        // NOTE: At this step, the length is guaranteed to be greater than zero (otherwise the second step above would
        //       have aborted the steps), so if sw is zero, this step will throw the exception and return.
        if sw == 0 || !length.is_multiple_of(sw) {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"Source width must be a multiple of source data's length.",
            )));
        }

        // 5. Let height be length divided by sw.
        let height = length / sw;

        // 6. If sh was given and its value is not equal to height, then throw an "IndexSizeError" DOMException.
        if sh.is_some_and(|sh| sh != height) {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"Source height must be equal to the calculated height of the data.",
            )));
        }

        // 7. Initialize this given sw, sh, settings set to settings, and source set to data.
        // FIXME: This seems to be a spec issue, sh is an optional but height always have a value.
        Self::initialize(cx, height, sw, Some(&settings), Some(data), None)
    }

    /// A new ImageData of the size, with transparent black pixels, as the 2D contexts create them: steps 2 to 4 of
    /// createImageData() and steps 3 to 4 of getImageData().
    pub(crate) fn create(
        cx: &mut CallContext,
        width: u32,
        height: u32,
        settings: Option<&ImageDataSettings>,
        default_color_space: Option<PredefinedColorSpace>,
    ) -> WebIdlResult<Ref<Self>> {
        let image_data = Self::initialize(cx, height, width, settings, None, default_color_space)?;
        Ok(allocate(cx.vm(), image_data))
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#initialize-an-imagedata-object
    fn initialize(
        cx: &mut CallContext,
        rows: u32,
        pixels_per_row: u32,
        settings: Option<&ImageDataSettings>,
        source: Option<Gc<TypedArrayBase>>,
        default_color_space: Option<PredefinedColorSpace>,
    ) -> WebIdlResult<Self> {
        let data = match source {
            // 1. If source was given, then initialize the data attribute of imageData to source.
            Some(source) => source,
            None => {
                let Some(size) = rows
                    .checked_mul(pixels_per_row)
                    .and_then(|pixels| pixels.checked_mul(BYTES_PER_PIXEL))
                else {
                    return Err(Exception::index_size_error(Utf16String::from_ascii(
                        b"The specified image size could not be created",
                    )));
                };

                // 2. Otherwise (source was not given), initialize the data attribute of imageData to a new
                //    Uint8ClampedArray object. The Uint8ClampedArray object must use a new Canvas Pixel ArrayBuffer for
                //    its storage, and must have a zero start offset and a length equal to the length of its storage,
                //    in bytes. The Canvas Pixel ArrayBuffer must have the correct size to store rows × pixelsPerRow
                //    pixels.
                // 3. If the Canvas Pixel ArrayBuffer cannot be allocated, then rethrow the RangeError thrown by
                //    JavaScript, and return.
                let buffer = ArrayBuffer::create(cx.vm(), cx.realm(), size as usize, ArrayBufferShared::No)?;
                create_typed_array_on_buffer(cx.vm(), cx.realm(), TypedArrayKind::Uint8ClampedArray, size, buffer)
            }
        };

        // AD-HOC: Validate that the data has room for the pixels.
        if !can_hold_pixels(&data, pixels_per_row, rows) {
            return Err(invalid_state_error(
                b"Source data must not be detached or out-of-bounds.",
            ));
        }

        // 4. Initialize the width attribute of imageData to pixelsPerRow.
        // 5. Initialize the height attribute of imageData to rows.
        // 6. If settings was given and settings["colorSpace"] exists, then initialize the colorSpace attribute of
        //    imageData to settings["colorSpace"].
        // 7. Otherwise, if defaultColorSpace was given, then initialize the colorSpace attribute of imageData to
        //    defaultColorSpace.
        // 8. Otherwise, initialize the colorSpace attribute of imageData to "srgb".
        let color_space = settings
            .and_then(|settings| settings.color_space)
            .or(default_color_space)
            .unwrap_or(PredefinedColorSpace::Srgb);

        Ok(Self {
            data,
            width: pixels_per_row,
            height: rows,
            color_space,
        })
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagedata-width
    pub(crate) fn width(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.width)
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagedata-height
    pub(crate) fn height(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.height)
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagedata-data
    pub(crate) fn data(&self, _: &mut CallContext) -> WebIdlResult<Gc<TypedArrayBase>> {
        Ok(self.data)
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagedata-colorspace
    pub(crate) fn color_space(&self, _: &mut CallContext) -> WebIdlResult<PredefinedColorSpace> {
        Ok(self.color_space)
    }

    pub(crate) fn predefined_color_space(&self) -> PredefinedColorSpace {
        self.color_space
    }

    pub(crate) fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// Whether the buffer of the data attribute is detached.
    pub(crate) fn is_detached(&self) -> bool {
        self.data.viewed_array_buffer().is_detached()
    }

    /// The first byte of the pixels, which are width × height × 4 bytes, unless the buffer of the data attribute is
    /// detached or the data no longer has room for them. The pointer is valid until script runs: script can detach
    /// or shrink the buffer, and write to it, so the pixels are read and written through the pointer, never as a
    /// slice.
    pub(crate) fn pixels(&self) -> Option<NonNull<u8>> {
        if !can_hold_pixels(&self.data, self.width, self.height) {
            return None;
        }
        let buffer = self.data.viewed_array_buffer();
        let byte_offset = self.data.byte_offset() as usize;
        NonNull::new(buffer.with_data_block(|data_block| data_block.data_at(byte_offset)))
    }
}

fn invalid_state_error(message: &[u8]) -> Exception {
    Exception::Dom("InvalidStateError", Utf16String::from_ascii(message))
}

impl Serializable for ImageData {
    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagedata-interface:serialization-steps
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        // 1. Set serialized.[[Data]] to the sub-serialization of the value of value's data attribute.
        serialized.write_value(Value::from_object(self.data))?;

        // 2. Set serialized.[[Width]] to the value of value's width attribute.
        serialized.write_u32(self.width);

        // 3. Set serialized.[[Height]] to the value of value's height attribute.
        serialized.write_u32(self.height);

        // 4. Set serialized.[[ColorSpace]] to the value of value's colorSpace attribute.
        // SAFETY: The writer is lent for the steps.
        unsafe { web_image_data_serialize_color_space(serialized.as_raw(), self.color_space) };

        // FIXME: 5. Set serialized.[[PixelFormat]] to the value of value's pixelFormat attribute.

        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagedata-interface:deserialization-steps
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        // 1. Initialize value's data attribute to the sub-deserialization of serialized.[[Data]].
        let data_value = serialized.read_value()?;
        let data = data_value
            .is_object()
            .then(|| data_value.as_object().downcast::<TypedArrayBase>())
            .flatten()
            .filter(|data| data.kind() == TypedArrayKind::Uint8ClampedArray)
            .ok_or_else(|| {
                Exception::Dom(
                    "DataCloneError",
                    Utf16String::from_ascii(b"ImageData data must be a Uint8ClampedArray"),
                )
            })?;

        // 2. Initialize value's width attribute to serialized.[[Width]].
        let width = serialized.read_u32()?;

        // 3. Initialize value's height attribute to serialized.[[Height]].
        let height = serialized.read_u32()?;

        // 4. Initialize value's colorSpace attribute to serialized.[[ColorSpace]].
        let mut color_space = 0;
        // SAFETY: The reader is lent for the steps.
        unsafe { web_image_data_deserialize_color_space(serialized.as_raw(), &mut color_space) }.into_completion()?;
        let color_space = PredefinedColorSpace::from_u8(color_space).expect("C++ decodes a color space");

        // FIXME: 5. Initialize value's pixelFormat attribute to serialized.[[PixelFormat]].

        // AD-HOC: Validate that the data has room for the pixels.
        if !can_hold_pixels(&data, width, height) {
            return Err(invalid_state_error(
                b"Image data must not be detached or out-of-bounds.",
            ));
        }

        Ok(Self {
            data,
            width,
            height,
            color_space,
        })
    }
}
