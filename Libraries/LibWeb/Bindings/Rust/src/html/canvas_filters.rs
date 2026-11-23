/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasfilters

use std::rc::Rc;

use ak::Utf16String;
use libcompositing_rust::filter_bytes::filter_functions_graph;
use libgfx_rust::filter::Filter;

use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::canvas::CanvasFilterOperation;
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filter
    pub(crate) fn filter(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self
            .drawing_state()
            .filter_string
            .clone()
            .unwrap_or_else(|| Utf16String::from_ascii(b"none")))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filter
    pub(crate) fn set_filter(&self, cx: &mut CallContext, filter: Utf16String) -> WebIdlResult<()> {
        // 1. If the given value is "none", then set this's current filter to "none" and return.
        if filter == Utf16String::from_ascii(b"none") {
            let mut state = self.drawing_state_mut(cx.vm());
            state.filter = None;
            state.filter_operations = None;
            state.filter_string = None;
            return Ok(());
        }

        // 2. Let parsedValue be the result of parsing the given values as a <filter-value-list>. If any
        //    property-independent style sheet syntax like 'inherit' or 'initial' is present, then this parsing must
        //    return failure.
        // 3. If parsedValue is failure, then return.
        let Some(operations) = self.canvas_handle().parse_filter(&filter, self.font_metrics_source()) else {
            return Ok(());
        };

        // 4. Set this's current filter to the given value.
        let mut state = self.drawing_state_mut(cx.vm());
        if operations
            .iter()
            .any(|operation| matches!(operation, CanvasFilterOperation::Reference(_)))
        {
            state.filter = None;
            state.filter_operations = Some(Rc::from(operations));
        } else {
            let graph = filter_functions_graph(operations.into_iter().map(|operation| {
                let CanvasFilterOperation::Function(function) = operation else {
                    unreachable!("the list contains only filter functions");
                };
                Filter::from(function)
            }));
            state.filter = graph.map(|graph| Rc::from(graph.serialize()));
            state.filter_operations = None;
        }
        state.filter_string = Some(filter);
        Ok(())
    }
}
