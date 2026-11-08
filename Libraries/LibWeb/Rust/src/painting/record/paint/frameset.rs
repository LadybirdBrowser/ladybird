/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::style::bridge::FfiFrameSetBorder;
use crate::layout::frameset_formatting_context::frameset_track_sizes;
use crate::layout::node_data::NodeSlotId;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paint_read::PaintRead;
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::record::PaintRecorder;
use crate::painting::record::trace::Observer;
use libgfx_rust::Color;

// https://html.spec.whatwg.org/multipage/rendering.html#frames-and-framesets
// FIXME: For each (visible) border that does not abut a rectangle that is assigned a frame element with a noresize
//        attribute (including rectangles in further nested frameset elements), the user agent is expected to allow the
//        user to move the border, resizing the rectangles within, keeping the proportions of any nested frameset grids.
pub(crate) fn paint_frameset_borders<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    let Some(layout) = recorder
        .source
        .replaced_paint_facts(paintable)
        .and_then(|facts| facts.frameset())
    else {
        return;
    };
    if layout.border_thickness == 0 {
        return;
    }
    let border_color = |border: &FfiFrameSetBorder| {
        let [alpha, red, green, blue] = border.color.to_be_bytes();
        Color::from_rgba(red, green, blue, alpha)
    };
    let thickness = CssPixels::from_integer(i64::from(layout.border_thickness));
    let frameset_rect = absolute_rect(recorder.source, paintable);

    let sized_cols = frameset_track_sizes(&layout.columns, frameset_rect.width, layout.border_thickness);
    let mut x = frameset_rect.x;
    for (border_after, size) in layout.column_borders.iter().zip(sized_cols) {
        x += size;
        if border_after.is_drawn {
            let border = CssPixelRect::new(x, frameset_rect.y, thickness, frameset_rect.height);
            let border = recorder.converter.rounded_device_rect(border);
            recorder
                .recorder
                .fill_rect(border, border_color(border_after), ForceDarkRole::Background);
        }
        x += thickness;
    }

    let sized_rows = frameset_track_sizes(&layout.rows, frameset_rect.height, layout.border_thickness);
    let mut y = frameset_rect.y;
    for (border_after, size) in layout.row_borders.iter().zip(sized_rows) {
        y += size;
        if border_after.is_drawn {
            let border = CssPixelRect::new(frameset_rect.x, y, frameset_rect.width, thickness);
            let border = recorder.converter.rounded_device_rect(border);
            recorder
                .recorder
                .fill_rect(border, border_color(border_after), ForceDarkRole::Background);
        }
        y += thickness;
    }
}
