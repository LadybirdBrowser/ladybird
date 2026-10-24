/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasdrawimage

use core::ptr::NonNull;

use ak::Utf16String;
use libcompositing_rust::display_list::commands::CanvasId;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{FloatRect, ScalingMode};

use super::image_bitmap::ImageBitmap;
use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::canvas_2d::canvas::{CanvasImageSourceKind, Usability, capture_video_frame};
use crate::canvas_2d::context::{CanvasKind, ContextState, RenderingContext2D};
use crate::canvas_2d::float;
use crate::canvas_2d::host::{LentImageFrame, LentVideoFrameHold};

/// The union CanvasImageSource of HTML/Canvas/CanvasDrawImage.idl, whose members the bindings convert.
#[derive(Clone, Copy)]
pub(crate) enum CanvasImageSource {
    HTMLImageElement(CppInterface),
    SVGImageElement(CppInterface),
    HTMLVideoElement(CppInterface),
    HTMLCanvasElement(CppInterface),
    ImageBitmap(Ref<ImageBitmap>),
    OffscreenCanvas(CppInterface),
}

impl CanvasImageSource {
    /// The source and its kind, if it is implemented in C++.
    fn cpp_source(self) -> Option<(CppInterface, CanvasImageSourceKind)> {
        match self {
            Self::HTMLImageElement(source) => Some((source, CanvasImageSourceKind::HTMLImageElement)),
            Self::SVGImageElement(source) => Some((source, CanvasImageSourceKind::SVGImageElement)),
            Self::HTMLVideoElement(source) => Some((source, CanvasImageSourceKind::HTMLVideoElement)),
            Self::HTMLCanvasElement(source) => Some((source, CanvasImageSourceKind::HTMLCanvasElement)),
            Self::OffscreenCanvas(source) => Some((source, CanvasImageSourceKind::OffscreenCanvas)),
            Self::ImageBitmap(_) => None,
        }
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument
    /// Whether the image is good, or an exception.
    pub(crate) fn check_usability(self) -> WebIdlResult<bool> {
        let usability = match self.cpp_source() {
            Some((source, kind)) => kind.check_usability(source),
            None => {
                let Self::ImageBitmap(image_bitmap) = self else {
                    unreachable!("an image source that is not implemented in C++ is an ImageBitmap");
                };
                // FIXME: VideoFrame
                if image_bitmap.is_detached() {
                    Usability::InvalidState(Utf16String::from_ascii(b"Image bitmap is detached"))
                } else {
                    Usability::Good
                }
            }
        };
        match usability {
            Usability::Good => Ok(true),
            Usability::Bad => Ok(false),
            Usability::InvalidState(message) => Err(Exception::Dom("InvalidStateError", message)),
        }
    }

    /// The width and the height of the image, as the operations that do not take its size use them.
    pub(crate) fn size(self) -> (i32, i32) {
        match self.cpp_source() {
            Some((source, kind)) => kind.size(source),
            None => {
                let Self::ImageBitmap(image_bitmap) = self else {
                    unreachable!("an image source that is not implemented in C++ is an ImageBitmap");
                };
                match image_bitmap.frame() {
                    Some(frame) => (frame.width(), frame.height()),
                    None => (
                        i32::try_from(image_bitmap.width_of_bitmap()).unwrap_or(i32::MAX),
                        i32::try_from(image_bitmap.height_of_bitmap()).unwrap_or(i32::MAX),
                    ),
                }
            }
        }
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
    pub(crate) fn is_not_origin_clean(self) -> bool {
        match self.cpp_source() {
            Some((source, kind)) => kind.is_not_origin_clean(source),
            None => {
                let Self::ImageBitmap(image_bitmap) = self else {
                    unreachable!("an image source that is not implemented in C++ is an ImageBitmap");
                };
                // image's bitmap's origin-clean flag is false.
                !image_bitmap.origin_clean()
            }
        }
    }

    /// The frame of an image, a canvas or an ImageBitmap, if it has one. A canvas is read back for it, which flushes
    /// the commands of every canvas.
    pub(crate) fn frame(self) -> Option<ImageFrameHandle> {
        match self {
            Self::ImageBitmap(image_bitmap) => image_bitmap.frame(),
            Self::HTMLVideoElement(_) => unreachable!("a video is drawn by its frames, which it holds"),
            _ => {
                let (source, kind) = self.cpp_source().expect("the image source is implemented in C++");
                kind.frame(source)
            }
        }
    }
}

impl ContextState {
    /// 4.12.5.1.15 Drawing images, https://html.spec.whatwg.org/multipage/canvas.html#drawing-images
    #[allow(clippy::too_many_arguments, reason = "the arguments of drawImage()")]
    pub(crate) fn draw_image_internal(
        &self,
        image: CanvasImageSource,
        mut source_x: f32,
        mut source_y: f32,
        mut source_width: f32,
        mut source_height: f32,
        mut destination_x: f32,
        mut destination_y: f32,
        mut destination_width: f32,
        mut destination_height: f32,
    ) -> WebIdlResult<()> {
        // 1. If any of the arguments are infinite or NaN, then return.
        let arguments = [
            source_x,
            source_y,
            source_width,
            source_height,
            destination_x,
            destination_y,
            destination_width,
            destination_height,
        ];
        if !arguments.iter().all(|argument| argument.is_finite()) {
            return Ok(());
        }

        // 2. Let usability be the result of checking the usability of image.
        // 3. If usability is bad, then return (without drawing anything).
        if !image.check_usability()? {
            return Ok(());
        }

        // 4. Establish the source and destination rectangles as follows:
        //    If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that
        //    one CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
        //    If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's natural
        //    width in image pixels, and the image's natural height in image pixels, respectively.
        //    If the image has no natural dimensions, then the concrete object size must be used instead, as
        //    determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
        //    neither a definite width nor height, nor any additional constraints, the object's natural properties
        //    being those of the image argument, and the default object size being the size of the output bitmap.
        // NOTE: Implemented in drawImage() overloads
        if source_width < 0.0 {
            source_x += source_width;
            source_width = source_width.abs();
        }
        if source_height < 0.0 {
            source_y += source_height;
            source_height = source_height.abs();
        }
        if destination_width < 0.0 {
            destination_x += destination_width;
            destination_width = destination_width.abs();
        }
        if destination_height < 0.0 {
            destination_y += destination_height;
            destination_height = destination_height.abs();
        }

        //    The source rectangle is the rectangle whose corners are the four points (sx, sy), (sx+sw, sy),
        //    (sx+sw, sy+sh), (sx, sy+sh).
        let source_rect = FloatRect::new(source_x, source_y, source_width, source_height);
        //    The destination rectangle is the rectangle whose corners are the four points (dx, dy), (dx+dw, dy),
        //    (dx+dw, dy+dh), (dx, dy+dh).
        let destination_rect = FloatRect::new(destination_x, destination_y, destination_width, destination_height);
        // FIXME: When the source rectangle is outside the source image, the source rectangle must be clipped to the
        //        source image and the destination rectangle must be clipped in the same proportion.

        // 5. If one of the sw or sh arguments is zero, then return. Nothing is painted.
        if source_width == 0.0 || source_height == 0.0 {
            return Ok(());
        }

        // 6. Paint the region of the image argument specified by the source rectangle on the region of the rendering
        //    context's output bitmap specified by the destination rectangle, after applying the current
        //    transformation matrix to the destination rectangle.
        let scaling_mode = if self.drawing_state().image_smoothing_enabled {
            // FIXME: Honor the image smoothing quality.
            ScalingMode::BilinearMipmap
        } else {
            ScalingMode::NearestNeighbor
        };

        // 7. If image is not origin-clean, then set the CanvasRenderingContext2D's origin-clean flag to false.
        let update_origin_clean = || {
            if image.is_not_origin_clean() {
                self.set_origin_clean(false);
            }
        };

        // A canvas is drawn by its id. A 2D source needs no eager synchronization: its recorded commands precede this
        // DrawCanvas in the shared ordered stream, so the replay sees them by construction. WebGL frames are presented
        // by a separate message and the source's registry surface is its live drawing buffer, so the source is
        // presented now and the DrawCanvas is flushed right after it, before a later WebGL frame can overtake the
        // reference.
        if let CanvasImageSource::HTMLCanvasElement(source) | CanvasImageSource::OffscreenCanvas(source) = image {
            let kind = image.cpp_source().expect("a canvas is implemented in C++").1;
            if let Some((source_canvas_id, source_is_2d)) = kind.canvas_id(source) {
                if self.recorder_is_ready() {
                    let state = self.drawing_state();
                    self.record(|recorder| {
                        recorder.draw_canvas(
                            CanvasId(source_canvas_id),
                            destination_rect,
                            source_rect,
                            scaling_mode,
                            &Self::compositing(&state),
                        );
                    });
                    drop(state);
                    self.did_draw();
                    if !source_is_2d {
                        self.record(|recorder| recorder.flush());
                    }
                }
                update_origin_clean();
                return Ok(());
            }
        }

        if let CanvasImageSource::HTMLVideoElement(video) = image {
            let Some(captured_frame) = capture_video_frame(video) else {
                return Ok(());
            };
            if self.recorder_is_ready() {
                let state = self.drawing_state();
                self.record(|recorder| {
                    recorder.draw_video_frame(
                        &LentVideoFrameHold(captured_frame.hold()),
                        captured_frame.sink_handle,
                        captured_frame.generation,
                        destination_rect,
                        source_rect,
                        scaling_mode,
                        &Self::compositing(&state),
                    );
                });
                drop(state);
                self.did_draw();
            }
            update_origin_clean();
            return Ok(());
        }

        let Some(frame) = image.frame() else {
            return Ok(());
        };

        // NOTE: Reading a canvas back for its frame may flush the shared command stream, so the recorder is readied
        //       only afterwards.
        if self.recorder_is_ready() {
            let state = self.drawing_state();
            let frame = LentImageFrame(NonNull::new(frame.as_raw().cast_mut()).expect("an image frame has a frame"));
            self.record(|recorder| {
                recorder.draw_image_frame(
                    &frame,
                    destination_rect,
                    source_rect,
                    scaling_mode,
                    &Self::compositing(&state),
                );
            });
            drop(state);
            self.did_draw();
        }

        // 7. If image is not origin-clean, then set the CanvasRenderingContext2D's origin-clean flag to false.
        update_origin_clean();

        Ok(())
    }
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-drawimage
    pub(crate) fn draw_image(
        &self,
        _: &mut CallContext,
        image: CanvasImageSource,
        dx: f64,
        dy: f64,
    ) -> WebIdlResult<()> {
        // If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that one
        // CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
        // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's natural
        // width in image pixels, and the image's natural height in image pixels, respectively.
        let (dx, dy) = (float(dx), float(dy));
        let (width, height) = image.size();
        let (width, height) = (int_to_float(width), int_to_float(height));
        self.draw_image_internal(image, 0.0, 0.0, width, height, dx, dy, width, height)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-drawimage
    #[allow(clippy::too_many_arguments, reason = "the arguments of drawImage()")]
    pub(crate) fn draw_image_with_size(
        &self,
        _: &mut CallContext,
        image: CanvasImageSource,
        dx: f64,
        dy: f64,
        dw: f64,
        dh: f64,
    ) -> WebIdlResult<()> {
        // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's natural
        // width in image pixels, and the image's natural height in image pixels, respectively.
        let (dx, dy, dw, dh) = (float(dx), float(dy), float(dw), float(dh));
        let (width, height) = image.size();
        let (width, height) = (int_to_float(width), int_to_float(height));
        self.draw_image_internal(image, 0.0, 0.0, width, height, dx, dy, dw, dh)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-drawimage
    #[allow(clippy::too_many_arguments, reason = "the arguments of drawImage()")]
    pub(crate) fn draw_image_with_source_rectangle(
        &self,
        _: &mut CallContext,
        image: CanvasImageSource,
        sx: f64,
        sy: f64,
        sw: f64,
        sh: f64,
        dx: f64,
        dy: f64,
        dw: f64,
        dh: f64,
    ) -> WebIdlResult<()> {
        self.draw_image_internal(
            image,
            float(sx),
            float(sy),
            float(sw),
            float(sh),
            float(dx),
            float(dy),
            float(dw),
            float(dh),
        )
    }
}

#[allow(
    clippy::cast_precision_loss,
    reason = "a size of an image becomes a float, as the C++ had it"
)]
fn int_to_float(value: i32) -> f32 {
    value as f32
}
