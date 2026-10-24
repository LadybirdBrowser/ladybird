/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasimagedata
//!
//! The rectangles of these operations are computed in 64 bits from the IDL longs that they take, so that no sum or
//! difference of them overflows, and every rectangle that reads or writes pixels is checked against the bounds of the
//! pixels it reads or writes.

use core::ptr::NonNull;

use ak::Utf16String;
use libcompositing_rust::display_list::canvas_2d::Compositing;
use libcompositing_rust::display_list::canvas_2d_recorder::AlphaType;
use libgfx_rust::{AffineTransform, CompositingAndBlendingOperator, FloatRect, IntRect, ScalingMode};
use libjs_rust::api::{ErrorKind, ErrorType};

use super::image_data::{ImageData, ImageDataSettings};
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::canvas_2d::canvas::image_frame_from_pixels;
use crate::canvas_2d::context::{CanvasKind, ContextState, RenderingContext2D};
use crate::canvas_2d::host::LentImageFrame;

/// The size of a pixel of an ImageData and of a readback, whose pixels are RGBA8888.
const BYTES_PER_PIXEL: usize = 4;

/// The absolute magnitude of a size, with that of the smallest long, which a long cannot represent, being the largest.
fn absolute_magnitude(size: i32) -> u32 {
    if size == i32::MIN {
        i32::MAX.unsigned_abs()
    } else {
        size.unsigned_abs()
    }
}

fn index_size_error_for_zero_size() -> Exception {
    Exception::index_size_error(Utf16String::from_ascii(b"Width and height must not be zero"))
}

fn invalid_state_error(message: &[u8]) -> Exception {
    Exception::Dom("InvalidStateError", Utf16String::from_ascii(message))
}

/// A number of pixels or bytes that is known not to be negative.
fn to_usize(value: i64) -> usize {
    usize::try_from(value).expect("the number is not negative")
}

/// A coordinate or a size that is known to fit in an int.
fn to_i32(value: i64) -> i32 {
    i32::try_from(value).expect("the number fits in an int")
}

impl ContextState {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context2d-putimagedata-common
    /// The caller has readied the recorder.
    #[allow(clippy::too_many_arguments, reason = "the arguments of putImageData()")]
    fn put_pixels_from_an_image_data_onto_a_bitmap(
        &self,
        cx: &mut CallContext,
        image_data: Ref<ImageData>,
        dx: i32,
        dy: i32,
        dirty_x: i32,
        dirty_y: i32,
        dirty_width: i32,
        dirty_height: i32,
    ) -> WebIdlResult<()> {
        // 1. Let buffer be imageData's data attribute value's [[ViewedArrayBuffer]] internal slot.
        // 2. If IsDetachedBuffer(buffer) is true, then throw an "InvalidStateError" DOMException.
        if image_data.is_detached() {
            return Err(invalid_state_error(b"ImageData's underlying buffer is detached"));
        }

        let (width, height) = image_data.size();
        let (width, height) = (i64::from(width), i64::from(height));
        let (dx, dy) = (i64::from(dx), i64::from(dy));
        let (mut dirty_x, mut dirty_y) = (i64::from(dirty_x), i64::from(dirty_y));
        let (mut dirty_width, mut dirty_height) = (i64::from(dirty_width), i64::from(dirty_height));

        // 3. If dirtyWidth is negative, then let dirtyX be dirtyX+dirtyWidth, and let dirtyWidth be equal to the
        //    absolute magnitude of dirtyWidth.
        if dirty_width < 0 {
            dirty_x += dirty_width;
            dirty_width = -dirty_width;
        }
        // If dirtyHeight is negative, then let dirtyY be dirtyY+dirtyHeight, and let dirtyHeight be equal to the
        // absolute magnitude of dirtyHeight.
        if dirty_height < 0 {
            dirty_y += dirty_height;
            dirty_height = -dirty_height;
        }

        // 4. If dirtyX is negative, then let dirtyWidth be dirtyWidth+dirtyX, and let dirtyX be 0.
        if dirty_x < 0 {
            dirty_width += dirty_x;
            dirty_x = 0;
        }
        // If dirtyY is negative, then let dirtyHeight be dirtyHeight+dirtyY, and let dirtyY be 0.
        if dirty_y < 0 {
            dirty_height += dirty_y;
            dirty_y = 0;
        }

        // 5. If dirtyX+dirtyWidth is greater than the width attribute of the imageData argument, then let dirtyWidth
        //    be the value of that width attribute, minus the value of dirtyX.
        if dirty_x + dirty_width > width {
            dirty_width = width - dirty_x;
        }
        // If dirtyY+dirtyHeight is greater than the height attribute of the imageData argument, then let dirtyHeight
        // be the value of that height attribute, minus the value of dirtyY.
        if dirty_y + dirty_height > height {
            dirty_height = height - dirty_y;
        }

        // 6. If, after those changes, either dirtyWidth or dirtyHeight are negative or zero, then return without
        //    affecting any bitmaps.
        if dirty_width <= 0 || dirty_height <= 0 {
            return Ok(());
        }

        // 7. For all integer values of x and y where dirtyX ≤ x < dirtyX+dirtyWidth and dirtyY ≤ y <
        //    dirtyY+dirtyHeight, set the pixel with coordinate (dx+x, dy+y) in bitmap to the color of the pixel at
        //    coordinate (x, y) in the imageData data structure's bitmap, converted from imageData's colorSpace to the
        //    color space of bitmap using 'relative-colorimetric' rendering intent.
        let Some(pixels) = image_data.pixels() else {
            return Err(invalid_state_error(
                b"ImageData's underlying buffer is detached or out-of-bounds",
            ));
        };
        // NB: A lost context, or a canvas without backing storage, has no bitmap to set pixels in.
        if !self.recorder_is_ready() {
            return Ok(());
        }
        // NB: Only the pixels that land inside the output bitmap are set, so only those are copied. The source
        //     rectangle is also kept inside imageData here, whatever the steps above computed, before its pixels are
        //     read.
        let size = self.size();
        let source_left = dirty_x.max(-dx).max(0);
        let source_top = dirty_y.max(-dy).max(0);
        let source_right = (dirty_x + dirty_width).min(i64::from(size.width) - dx).min(width);
        let source_bottom = (dirty_y + dirty_height).min(i64::from(size.height) - dy).min(height);
        if source_left >= source_right || source_top >= source_bottom {
            return Ok(());
        }
        // NB: The destination rectangle is inside the output bitmap, so its coordinates fit in an int.
        let destination_rect = IntRect {
            x: to_i32(source_left + dx),
            y: to_i32(source_top + dy),
            width: to_i32(source_right - source_left),
            height: to_i32(source_bottom - source_top),
        };

        // NOTE: The ImageData buffer is mutable from script and the recorded draw is only serialized at flush time, so
        //       the dirty pixels are copied now. The copy is shareable, so that flushing to the Compositor passes the
        //       pixels by file descriptor instead of copying them again.
        let source_pitch = to_usize(width) * BYTES_PER_PIXEL;
        let first_row_offset = to_usize(source_top) * source_pitch + to_usize(source_left) * BYTES_PER_PIXEL;
        // SAFETY: The source rectangle is inside the width × height pixels of the ImageData, which pixels() vouched
        //         for, so its first pixel is too.
        let first_row = unsafe { NonNull::new_unchecked(pixels.as_ptr().add(first_row_offset)) };
        // SAFETY: The rows of the source rectangle are inside the pixels of the ImageData, and nothing runs script
        //         while they are copied.
        let Some(frame) = (unsafe {
            image_frame_from_pixels(first_row, source_pitch, destination_rect.width, destination_rect.height)
        }) else {
            return Err(cx
                .vm()
                .throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .expect_err("throwing a completion throws")
                .into());
        };

        #[allow(
            clippy::cast_precision_loss,
            reason = "the rectangle is in floats, as the C++ had it"
        )]
        let destination = FloatRect::new(
            destination_rect.x as f32,
            destination_rect.y as f32,
            destination_rect.width as f32,
            destination_rect.height as f32,
        );
        let frame_rect = IntRect {
            x: 0,
            y: 0,
            width: destination_rect.width,
            height: destination_rect.height,
        };
        let lent_frame = LentImageFrame(NonNull::new(frame.as_raw().cast_mut()).expect("an image frame has a frame"));
        self.record(|recorder| {
            recorder.save();
            recorder.set_transform(AffineTransform::identity());
            // NB: Step 7 sets each destination pixel to the source pixel: putImageData replaces the pixels (including
            //     their alpha), rather than compositing them. The copy operator makes a transparent source pixel
            //     overwrite an opaque destination pixel, rather than leave it unchanged.
            recorder.draw_image_frame(
                &lent_frame,
                destination,
                frame_rect,
                ScalingMode::NearestNeighbor,
                &Compositing {
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Copy,
                    ..Compositing::default()
                },
            );
            recorder.restore();
        });

        self.did_draw();

        Ok(())
    }
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata
    pub(crate) fn create_image_data(
        &self,
        cx: &mut CallContext,
        sw: i32,
        sh: i32,
        settings: ImageDataSettings,
    ) -> WebIdlResult<Ref<ImageData>> {
        // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
        if sw == 0 || sh == 0 {
            return Err(index_size_error_for_zero_size());
        }

        // 2. Let newImageData be a new ImageData object.
        // 3. Initialize newImageData given the absolute magnitude of sw, the absolute magnitude of sh, settings set to
        //    settings, and defaultColorSpace set to this's color space.
        // 4. Initialize the image data of newImageData to transparent black.
        // 5. Return newImageData.
        ImageData::create(
            cx,
            absolute_magnitude(sw),
            absolute_magnitude(sh),
            Some(&settings),
            Some(self.context_attributes.color_space),
        )
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata-imagedata
    pub(crate) fn create_image_data_from(
        &self,
        cx: &mut CallContext,
        image_data: Ref<ImageData>,
    ) -> WebIdlResult<Ref<ImageData>> {
        // 1. Let newImageData be a new ImageData object.
        // 2. Initialize newImageData given the value of imageData's width attribute, the value of imageData's height
        //    attribute, and defaultColorSpace set to the value of imageData's colorSpace attribute.
        // 3. Initialize the image data of newImageData to transparent black.
        // 4. Return newImageData.
        let (width, height) = image_data.size();
        ImageData::create(cx, width, height, None, Some(image_data.predefined_color_space()))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getimagedata
    pub(crate) fn get_image_data(
        &self,
        cx: &mut CallContext,
        sx: i32,
        sy: i32,
        sw: i32,
        sh: i32,
        settings: ImageDataSettings,
    ) -> WebIdlResult<Ref<ImageData>> {
        // 1. If either the sw or sh arguments are zero, then throw an "IndexSizeError" DOMException.
        if sw == 0 || sh == 0 {
            return Err(index_size_error_for_zero_size());
        }

        // 2. If the CanvasRenderingContext2D's origin-clean flag is set to false, then throw a "SecurityError"
        //    DOMException.
        if !self.origin_clean() {
            return Err(Exception::Dom(
                "SecurityError",
                Utf16String::from_ascii(b"CanvasRenderingContext2D is not origin-clean"),
            ));
        }

        // ImageData initialization requires positive width and height
        // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#initialize-an-imagedata-object
        let (width, height) = (absolute_magnitude(sw), absolute_magnitude(sh));

        // 3. Let imageData be a new ImageData object.
        // 4. Initialize imageData given sw, sh, settings set to settings, and defaultColorSpace set to this's color
        //    space.
        let image_data = ImageData::create(
            cx,
            width,
            height,
            Some(&settings),
            Some(self.context_attributes.color_space),
        )?;

        // 5. Let the source rectangle be the rectangle whose corners are the four points (sx, sy), (sx+sw, sy),
        //    (sx+sw, sy+sh), (sx, sy+sh).
        // NOTE: The spec doesn't seem to define this behavior, but MDN does and the WPT tests assume it works this
        //       way. https://developer.mozilla.org/en-US/docs/Web/API/CanvasRenderingContext2D/getImageData#sw
        let source_x = i64::from(sx) + i64::from(sw.min(0));
        let source_y = i64::from(sy) + i64::from(sh.min(0));
        let size = self.size();
        let left = source_x.max(0);
        let top = source_y.max(0);
        let right = (source_x + i64::from(width)).min(i64::from(size.width));
        let bottom = (source_y + i64::from(height)).min(i64::from(size.height));
        if left >= right || top >= bottom {
            return Ok(image_data);
        }
        let readback_rect = IntRect {
            x: to_i32(left),
            y: to_i32(top),
            width: to_i32(right - left),
            height: to_i32(bottom - top),
        };

        // NOTE: If reading back from the Compositor fails (no backing storage or no connection), it's like copying
        //       only transparent black pixels (which is a no-op).
        // NOTE: Internally we must use premultiplied alpha, but ImageData should hold unpremultiplied alpha. This
        //       conversion might result in a loss of precision, but is according to spec.
        //       See: https://html.spec.whatwg.org/multipage/canvas.html#premultiplied-alpha-and-the-2d-rendering-context
        let Some(readback) = self.read_pixels(readback_rect, AlphaType::Unpremultiplied) else {
            return Ok(image_data);
        };

        // 6. Set the pixel values of imageData to be the pixels of this's output bitmap in the area specified by the
        //    source rectangle in the bitmap's coordinate space units, converted from this's color space to imageData's
        //    colorSpace using 'relative-colorimetric' rendering intent.
        // NB: The pixels of imageData are RGBA8888 and unpremultiplied.
        let Some(readback_pixels) = readback.unpremultiplied_rgba_pixels() else {
            return Ok(image_data);
        };
        if readback_pixels.width != readback_rect.width || readback_pixels.height != readback_rect.height {
            return Ok(image_data);
        }
        let image_data_pixels = image_data.pixels().expect("a new ImageData has room for its pixels");

        // The readback only covers the part of the source rectangle that is inside the output bitmap.
        let destination_x = to_usize(left - source_x);
        let destination_y = to_usize(top - source_y);
        let destination_pitch = width as usize * BYTES_PER_PIXEL;
        let row_size = to_usize(i64::from(readback_rect.width)) * BYTES_PER_PIXEL;
        let rows = to_usize(i64::from(readback_rect.height));
        assert!(destination_x * BYTES_PER_PIXEL + row_size <= destination_pitch);
        assert!(destination_y + rows <= height as usize);
        assert!(row_size <= readback_pixels.pitch);
        for row in 0..rows {
            // SAFETY: The row is inside both the readback and the pixels of the new ImageData, as asserted above, and
            //         the two do not overlap.
            unsafe {
                let source = readback_pixels.first_row.as_ptr().add(row * readback_pixels.pitch);
                let destination = image_data_pixels
                    .as_ptr()
                    .add((destination_y + row) * destination_pitch + destination_x * BYTES_PER_PIXEL);
                core::ptr::copy_nonoverlapping(source, destination, row_size);
            }
        }

        // 7. Set the pixels values of imageData for areas of the source rectangle that are outside of the output
        //    bitmap to transparent black.
        // NOTE: No-op, already done during creation.

        // 8. Return imageData.
        Ok(image_data)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata-short
    pub(crate) fn put_image_data(
        &self,
        cx: &mut CallContext,
        image_data: Ref<ImageData>,
        dx: i32,
        dy: i32,
    ) -> WebIdlResult<()> {
        // The putImageData(imageData, dx, dy) method steps are to put pixels from an ImageData onto a bitmap, given
        // imageData, this's output bitmap, dx, dy, 0, 0, imageData's width, and imageData's height.
        let (width, height) = image_data.size();
        // NB: An ImageData's size fits in a long, as nothing creates a larger one.
        let (width, height) = (
            i32::try_from(width).unwrap_or(i32::MAX),
            i32::try_from(height).unwrap_or(i32::MAX),
        );
        self.put_pixels_from_an_image_data_onto_a_bitmap(cx, image_data, dx, dy, 0, 0, width, height)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata
    #[allow(clippy::too_many_arguments, reason = "the arguments of putImageData()")]
    pub(crate) fn put_image_data_with_dirty_rectangle(
        &self,
        cx: &mut CallContext,
        image_data: Ref<ImageData>,
        dx: i32,
        dy: i32,
        dirty_x: i32,
        dirty_y: i32,
        dirty_width: i32,
        dirty_height: i32,
    ) -> WebIdlResult<()> {
        // The putImageData(imageData, dx, dy, dirtyX, dirtyY, dirtyWidth, dirtyHeight) method steps are to put pixels
        // from an ImageData onto a bitmap, given imageData, this's output bitmap, dx, dy, dirtyX, dirtyY, dirtyWidth,
        // and dirtyHeight.
        self.put_pixels_from_an_image_data_onto_a_bitmap(
            cx,
            image_data,
            dx,
            dy,
            dirty_x,
            dirty_y,
            dirty_width,
            dirty_height,
        )
    }
}
