/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasimagesmoothing

use super::canvas_rendering_context2d::ImageSmoothingQuality;
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-imagesmoothingenabled
    pub(crate) fn image_smoothing_enabled(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.drawing_state().image_smoothing_enabled)
    }

    pub(crate) fn set_image_smoothing_enabled(&self, cx: &mut CallContext, enabled: bool) -> WebIdlResult<()> {
        self.drawing_state_mut(cx.vm()).image_smoothing_enabled = enabled;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-imagesmoothingquality
    pub(crate) fn image_smoothing_quality(&self, _: &mut CallContext) -> WebIdlResult<ImageSmoothingQuality> {
        Ok(self.drawing_state().image_smoothing_quality)
    }

    pub(crate) fn set_image_smoothing_quality(
        &self,
        cx: &mut CallContext,
        quality: ImageSmoothingQuality,
    ) -> WebIdlResult<()> {
        self.drawing_state_mut(cx.vm()).image_smoothing_quality = quality;
        Ok(())
    }
}
