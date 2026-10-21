/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvassettings

use super::canvas_rendering_context2d_settings::CanvasRenderingContext2DSettings;
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-canvas-getcontextattributes
    pub(crate) fn get_context_attributes(&self, _: &mut CallContext) -> WebIdlResult<CanvasRenderingContext2DSettings> {
        Ok(self.context_attributes)
    }
}
