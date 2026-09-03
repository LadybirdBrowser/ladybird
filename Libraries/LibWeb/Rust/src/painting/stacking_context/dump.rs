/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::dump::push_css_pixel_rect;
use crate::painting::paintable_geometry;
use crate::painting::style_queries;
use std::ffi::c_void;
use std::fmt::Write;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStackingContextDumpCallbacks {
    pub context: *mut c_void,
    pub debug_description:
        unsafe extern "C" fn(context: *mut c_void, layout_node_shell: *mut c_void, description_sink: *mut c_void),
    pub append_text: unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, byte_count: usize),
}

impl FfiStackingContextDumpCallbacks {
    fn debug_description(&self, layout_node_shell: *mut c_void) -> String {
        let mut description = Vec::new();
        // SAFETY: The C++ host fills the description sink synchronously through the exported push
        // function.
        unsafe { (self.debug_description)(self.context, layout_node_shell, (&raw mut description).cast()) };
        String::from_utf8_lossy(&description).into_owned()
    }

    fn append_text(&self, text: &str) {
        // SAFETY: The C++ sink copies the completed dump synchronously.
        unsafe { (self.append_text)(self.context, text.as_ptr(), text.len()) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
/// `debug_description` is called synchronously with a `Vec<u8>` sink the host fills through
/// `layout_arena_paint_push_bytes`, and `append_text` copies the completed dump synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_dump_stacking_context_tree(
    arena: *mut c_void,
    viewport: NodeSlotId,
    callbacks: FfiStackingContextDumpCallbacks,
) {
    // SAFETY: The caller guarantees a live arena handle borrowed for this call.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    if arena.stacking_context_entries(viewport).is_none() {
        return;
    }
    let mut output = String::new();
    visit(&mut output, arena, viewport, 0, &callbacks);
    callbacks.append_text(&output);
}

fn visit(
    output: &mut String,
    arena: &LayoutNodeArena,
    root: NodeSlotId,
    depth: usize,
    callbacks: &FfiStackingContextDumpCallbacks,
) {
    output.extend(std::iter::repeat_n(' ', depth));
    if !arena.slot_is_live(root) {
        output.push_str("SC for (gone)\n");
    } else {
        push_line(
            output,
            &callbacks.debug_description(arena.node_shell(root)),
            paintable_geometry::absolute_rect_or_default(&arena.paintable_rows(), root),
            effective_z_index(arena, root),
            has_css_transform(arena, root),
        );
    }

    let Some(entries) = arena.stacking_context_entries(root) else {
        return;
    };
    for entry in entries.negative_z_index_child_contexts() {
        visit(output, arena, entry.slot, depth + 1, callbacks);
    }
    for &descendant in &entries.stack_level_zero_boxes {
        if arena.paintable_row_is_populated(descendant)
            && arena
                .paintable_rows()
                .paintable_data(descendant)
                .establishes_stacking_context
        {
            visit(output, arena, descendant, depth + 1, callbacks);
        }
    }
    for entry in entries.positive_z_index_child_contexts() {
        visit(output, arena, entry.slot, depth + 1, callbacks);
    }
}

fn push_line(
    output: &mut String,
    description: &str,
    rect: CssPixelRect,
    effective_z_index: Option<i32>,
    has_transform: bool,
) {
    let _ = write!(output, "SC for {description} ");
    push_css_pixel_rect(output, rect);
    output.push_str(" (z-index: ");
    if let Some(z_index) = effective_z_index {
        let _ = write!(output, "{z_index}");
    } else {
        output.push_str("auto");
    }
    output.push(')');
    if has_transform {
        output.push_str(", has_transform");
    }
    output.push('\n');
}

fn effective_z_index(arena: &LayoutNodeArena, slot: NodeSlotId) -> Option<i32> {
    arena
        .paintable_visual_context_record(slot)
        .and_then(|record| record.stacking_context.effective_z_index)
}

fn has_css_transform(arena: &LayoutNodeArena, slot: NodeSlotId) -> bool {
    arena.paintable_row_is_populated(slot)
        && arena
            .node_style_if_live(slot)
            .is_some_and(|style| style_queries::has_css_transform(arena, slot, style))
}

#[cfg(test)]
mod tests {
    use super::push_line;
    use crate::css::css_pixels::{CssPixelRect, CssPixels};

    #[test]
    fn stacking_context_lines_match_the_canonical_format() {
        let rect = CssPixelRect::new(
            CssPixels::from_integer(1),
            CssPixels::from_integer(2),
            CssPixels::from_integer(30),
            CssPixels::from_integer(40),
        );
        let mut output = String::new();
        push_line(&mut output, "Viewport<#document>", rect, None, false);
        push_line(&mut output, "BlockContainer<DIV>#target.a.b", rect, Some(-1), true);
        assert_eq!(
            output,
            concat!(
                "SC for Viewport<#document> [1,2 30x40] (z-index: auto)\n",
                "SC for BlockContainer<DIV>#target.a.b [1,2 30x40] (z-index: -1), has_transform\n",
            )
        );
    }
}
