/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::dump::push_css_pixel_rect;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_geometry;
use crate::painting::style_queries;
use crate::stage::MainThread;
use std::ffi::c_void;
use std::fmt::Write;

/// What a stacking context dump asks the document. The fields are private: the callbacks are
/// reached only through the methods below, which take the main thread token.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStackingContextDumpCallbacks {
    context: *mut c_void,
    /// Describes the row in the slot, as its layout node describes itself.
    debug_description: unsafe extern "C" fn(context: *mut c_void, slot: NodeSlotId, description_sink: *mut c_void),
    append_text: unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, byte_count: usize),
}

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

impl FfiStackingContextDumpCallbacks {
    fn debug_description(&self, _: &MainThread, slot: NodeSlotId) -> String {
        let mut description = Vec::new();
        // SAFETY: The C++ host fills the description sink synchronously through the exported push
        // function.
        unsafe { (self.debug_description)(self.context, slot, (&raw mut description).cast()) };
        String::from_utf8_lossy(&description).into_owned()
    }

    fn append_text(&self, _: &MainThread, text: &str) {
        // SAFETY: The C++ sink copies the completed dump synchronously.
        unsafe { (self.append_text)(self.context, text.as_ptr(), text.len()) };
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. `debug_description` is called synchronously with a `Vec<u8>` sink the host fills through
/// `layout_arena_paint_push_bytes`, and `append_text` copies the completed dump synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_dump_stacking_context_tree(
    host: *const crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    viewport: NodeSlotId,
    callbacks: FfiStackingContextDumpCallbacks,
) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, &*host) };
    // SAFETY: As above.
    let lines = unsafe {
        crate::painting::ffi::read_arena(host, read, viewport, |arena, viewport| {
            let mut lines = Vec::new();
            if arena.stacking_context_entries(viewport).is_some() {
                visit(&mut lines, arena, viewport, 0);
            }
            lines
        })
    };
    if lines.is_empty() {
        return;
    }
    let mut output = String::new();
    for line in lines {
        output.extend(std::iter::repeat_n(' ', line.depth));
        match line.context {
            None => output.push_str("SC for (gone)\n"),
            Some(context) => push_line(
                &mut output,
                &callbacks.debug_description(&main_thread, context.slot),
                context.rect,
                context.effective_z_index,
                context.has_transform,
            ),
        }
    }
    callbacks.append_text(&main_thread, &output);
}

/// One line of a stacking context dump: how deep the context is, and what its row says of it, or none for a row that
/// is gone.
struct DumpLine {
    depth: usize,
    context: Option<DumpedContext>,
}

struct DumpedContext {
    slot: NodeSlotId,
    rect: CssPixelRect,
    effective_z_index: Option<i32>,
    has_transform: bool,
}

fn visit(lines: &mut Vec<DumpLine>, arena: &LayoutNodeArena, root: NodeSlotId, depth: usize) {
    lines.push(DumpLine {
        depth,
        context: arena.slot_is_live(root).then(|| DumpedContext {
            slot: root,
            rect: paintable_geometry::absolute_rect_or_default(&arena.paintable_rows(), root),
            effective_z_index: effective_z_index(arena, root),
            has_transform: has_css_transform(arena, root),
        }),
    });

    let Some(entries) = arena.stacking_context_entries(root) else {
        return;
    };
    for entry in entries.negative_z_index_child_contexts() {
        visit(lines, arena, entry.slot, depth + 1);
    }
    for &descendant in &entries.stack_level_zero_boxes {
        if arena.paintable_row_is_populated(descendant)
            && arena
                .paintable_rows()
                .paintable_data(descendant)
                .establishes_stacking_context
        {
            visit(lines, arena, descendant, depth + 1);
        }
    }
    for entry in entries.positive_z_index_child_contexts() {
        visit(lines, arena, entry.slot, depth + 1);
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
