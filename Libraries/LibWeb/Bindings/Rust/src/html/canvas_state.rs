/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasstate

use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, CanvasRecorder, RenderingContext2D};

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-save
    pub(crate) fn save(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        // The save() method steps are to push a copy of the current drawing state onto the drawing state stack.
        self.push_drawing_state(cx.vm());

        self.record_if_ready(CanvasRecorder::save);
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-restore
    pub(crate) fn restore(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        // The restore() method steps are to pop the top entry in the drawing state stack, and reset the drawing state
        // it describes. If there is no saved state, then the method must do nothing.
        if !self.pop_drawing_state(cx.vm()) {
            return Ok(());
        }
        // NB: The current default path is not part of the drawing state.
        self.keep_path_in_place();

        self.record_if_ready(CanvasRecorder::restore);
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-reset
    pub(crate) fn reset(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The reset() method steps are to reset the rendering context to its default state.
        self.reset_to_default_state();
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-iscontextlost
    pub(crate) fn is_context_lost(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The isContextLost() method steps are to return this's context lost.
        Ok(self.context_is_lost())
    }
}
