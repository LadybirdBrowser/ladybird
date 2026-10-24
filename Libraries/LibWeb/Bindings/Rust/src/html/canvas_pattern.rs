/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvaspattern

use core::cell::Cell;

use ak::Utf16String;
use libcompositing_rust::display_list::canvas_2d::Compositing;
use libcompositing_rust::display_list::canvas_2d_recorder::{AlphaType, ColorSpace, Recorder};
use libcompositing_rust::display_list::commands::Canvas2DPatternRepetition;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{AffineTransform, CompositingAndBlendingOperator, FloatRect, IntRect, IntSize, ScalingMode};
use libjs_rust::api::{Trace, Vm};

use super::canvas_draw_image::CanvasImageSource;
use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::canvas_2d::canvas::{CanvasImageSourceKind, capture_video_frame, video_transport};
use crate::canvas_2d::host::LentVideoFrameHold;
use crate::geometry::dictionaries::{DOMMatrix2DInit, validate_and_fixup_2d};

/// The repetition that a value of the repetition argument of createPattern() names exactly, if it names one.
fn parse_repetition(repetition: &Utf16String) -> Option<Canvas2DPatternRepetition> {
    [
        ("repeat", Canvas2DPatternRepetition::Repeat),
        ("repeat-x", Canvas2DPatternRepetition::RepeatX),
        ("repeat-y", Canvas2DPatternRepetition::RepeatY),
        ("no-repeat", Canvas2DPatternRepetition::NoRepeat),
    ]
    .into_iter()
    .find(|(name, _)| *repetition == Utf16String::from_ascii(name.as_bytes()))
    .map(|(_, repetition)| repetition)
}

/// A copy of the frame of the video at its current playback position, in the color space, which it keeps. A pattern or
/// an ImageBitmap outlives any one video frame, so it reads the frame back instead of holding it.
pub(crate) fn read_back_current_video_frame(video: CppInterface, color_space: ColorSpace) -> Option<ImageFrameHandle> {
    let captured_frame = capture_video_frame(video)?;
    let transport = video_transport(video)?;
    let mut recorder = Recorder::new(transport);

    let (width, height) = CanvasImageSourceKind::HTMLVideoElement.size(video);
    let frame_rect = IntRect {
        x: 0,
        y: 0,
        width,
        height,
    };
    // NB: The context only exists to be read back.
    let will_read_frequently = true;
    if width <= 0
        || height <= 0
        || !recorder.create_context(IntSize { width, height }, true, will_read_frequently, color_space)
    {
        return None;
    }

    #[allow(
        clippy::cast_precision_loss,
        reason = "the rectangle is in floats, as the C++ had it"
    )]
    let destination = FloatRect::new(0.0, 0.0, width as f32, height as f32);
    recorder.draw_video_frame(
        &LentVideoFrameHold(captured_frame.hold()),
        captured_frame.sink_handle,
        captured_frame.generation,
        destination,
        destination,
        ScalingMode::NearestNeighbor,
        &Compositing {
            compositing_and_blending_operator: CompositingAndBlendingOperator::Copy,
            ..Compositing::default()
        },
    );
    recorder.flush();
    let bitmap = recorder.read_back_pixels(frame_rect, AlphaType::Premultiplied, color_space);
    recorder.destroy_context();
    Some(bitmap?.into_image_frame(color_space))
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CanvasPattern {
    /// The frame of the image when createPattern() was called, if the image had one.
    #[gc(untraced)]
    image: Option<ImageFrameHandle>,
    #[gc(untraced)]
    repetition: Canvas2DPatternRepetition,
    #[gc(untraced)]
    transform: Cell<Option<AffineTransform>>,
    /// Whether the pattern is marked as not origin-clean, as its image was.
    #[gc(untraced)]
    not_origin_clean: Cell<bool>,
}

impl Wrappable for CanvasPattern {}

impl CanvasPattern {
    /// The pattern of createPattern(), whose image and repetition the context has checked,
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createpattern
    pub(crate) fn create(vm: &Vm, image: Option<ImageFrameHandle>, repetition: Canvas2DPatternRepetition) -> Ref<Self> {
        allocate(
            vm,
            Self {
                image,
                repetition,
                transform: Cell::new(None),
                not_origin_clean: Cell::new(false),
            },
        )
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createpattern
    pub(crate) fn create_for_image(
        vm: &Vm,
        image: &CanvasImageSource,
        repetition: &Utf16String,
        color_space: ColorSpace,
    ) -> WebIdlResult<Option<Ref<Self>>> {
        // 1. Let usability be the result of checking the usability of image.
        // 2. If usability is bad, then return null.
        // 3. Assert: usability is good.
        if !image.check_usability()? {
            return Ok(None);
        }

        // 4. If repetition is the empty string, then set it to "repeat".
        // 5. If repetition is not identical to one of "repeat", "repeat-x", "repeat-y", or "no-repeat", then throw a
        //    "SyntaxError" DOMException.
        let repetition = if repetition.is_empty() {
            Some(Canvas2DPatternRepetition::Repeat)
        } else {
            parse_repetition(repetition)
        };
        let Some(repetition) = repetition else {
            return Err(Exception::syntax_error(Utf16String::from_ascii(
                b"Repetition value is not valid",
            )));
        };

        // 6. Let pattern be a new CanvasPattern object with the image image and the repetition behavior given by
        //    repetition.
        let frame = match *image {
            // NB: The frame of a video is read back in the color space of the context, which a draw with the pattern
            //     converts it to anyway.
            CanvasImageSource::HTMLVideoElement(video) => read_back_current_video_frame(video, color_space),
            _ => image.frame(),
        };
        let pattern = Self::create(vm, frame, repetition);

        // 7. If image is not origin-clean, then mark pattern as not origin-clean.
        if image.is_not_origin_clean() {
            pattern.not_origin_clean.set(true);
        }

        // 8. Return pattern.
        Ok(Some(pattern))
    }

    pub(crate) fn image(&self) -> Option<&ImageFrameHandle> {
        self.image.as_ref()
    }

    /// Whether the pattern is marked as not origin-clean.
    pub(crate) fn is_not_origin_clean(&self) -> bool {
        self.not_origin_clean.get()
    }

    pub(crate) fn repetition(&self) -> Canvas2DPatternRepetition {
        self.repetition
    }

    /// The transformation matrix of the pattern, which it has once setTransform() gave it one.
    pub(crate) fn transform(&self) -> Option<AffineTransform> {
        self.transform.get()
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvaspattern-settransform
    pub(crate) fn set_transform(&self, _: &mut CallContext, transform: DOMMatrix2DInit) -> WebIdlResult<()> {
        // 1. Let matrix be the result of creating a DOMMatrix from the 2D dictionary transform.
        let matrix = validate_and_fixup_2d(&transform)?;

        // 2. If one or more of matrix's m11 element, m12 element, m21 element, m22 element, m41 element, or m42
        //    element are infinite or NaN, then return.
        if !matrix.is_finite() {
            return Ok(());
        }

        // 3. Reset the pattern's transformation matrix to matrix.
        self.transform.set(Some(matrix.to_affine_transform()));

        Ok(())
    }
}
