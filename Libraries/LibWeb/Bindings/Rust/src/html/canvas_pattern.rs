/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvaspattern

use core::cell::Cell;

use libcompositing_rust::display_list::commands::Canvas2DPatternRepetition;
use libgfx_rust::AffineTransform;
use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::geometry::dom_matrix_read_only::{DOMMatrix2DInit, validate_and_fixup_2d};

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
            },
        )
    }

    pub(crate) fn image(&self) -> Option<&ImageFrameHandle> {
        self.image.as_ref()
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
