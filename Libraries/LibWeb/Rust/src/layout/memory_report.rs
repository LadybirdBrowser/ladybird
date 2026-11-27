/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document's layout holds on the heap, by owner.
//!
//! An owner is a store of the layout node arena or of the layout stage, or one kind of thing several stores share,
//! as the fragments are. A shared allocation is counted once, by the first owner the report reaches it from. See
//! [`crate::heap_size`] for what a count is.

use super::fragment_tree::Fragment;
use super::inline_content::InlineContent;
use crate::heap_size::HeapSizer;
use std::sync::Arc;

macro_rules! memory_owners {
    ($($owner:ident => $name:literal,)*) => {
        /// An owner of layout memory.
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(u8)]
        pub(crate) enum LayoutMemoryOwner {
            $($owner,)*
        }

        impl LayoutMemoryOwner {
            pub(crate) const ALL: &[LayoutMemoryOwner] = &[$(LayoutMemoryOwner::$owner,)*];
            pub(crate) const COUNT: usize = Self::ALL.len();

            pub(crate) fn name(self) -> &'static str {
                match self {
                    $(LayoutMemoryOwner::$owner => $name,)*
                }
            }
        }
    };
}

memory_owners! {
    // The rows of the nodes, and the column of them the paint side reads.
    NodeRows => "nodeRows",
    // The arena's other columns of a row per slot, and its tables by style node.
    ArenaColumns => "arenaColumns",
    // Each text row's state and the text it renders.
    TextRows => "textRows",
    // The paintable rows' columns, but for the ones the owners below name.
    PaintableRows => "paintableRows",
    // The column of the link each box committed.
    CommittedLinks => "committedLinks",
    // The committed fragments and the links to their children.
    Fragments => "fragments",
    // The lines and glyph runs of the committed inline content.
    InlineContent => "inlineContent",
    // Each box's visual context record and node handles.
    VisualContexts => "visualContexts",
    // What the layout stage keeps of one pass for the next: the results of runs and of in-flow blocks, the intrinsic
    // sizes it measured, and the text it chunked and prepared.
    Memos => "memos",
    // The table of the layout nodes the host made for rows.
    LayoutNodeTable => "layoutNodeTable",
}

/// A layout memory report under way, and then its result.
#[derive(Default)]
pub(crate) struct LayoutMemoryReport {
    pub(crate) sizer: HeapSizer,
    bytes: [usize; LayoutMemoryOwner::COUNT],
    /// The slots the arena's columns hold a row for.
    pub(crate) slots: usize,
    /// The slots that hold a live row.
    pub(crate) live_rows: usize,
    /// The rows the host made a layout node for.
    pub(crate) layout_nodes: usize,
}

impl LayoutMemoryReport {
    pub(crate) fn add(&mut self, owner: LayoutMemoryOwner, bytes: usize) {
        self.bytes[owner as usize] += bytes;
    }

    pub(crate) fn bytes(&self, owner: LayoutMemoryOwner) -> usize {
        self.bytes[owner as usize]
    }
}

impl LayoutMemoryReport {
    /// Counts `root` and the fragments below it that the report reaches for the first time, with the inline content
    /// they hold.
    pub(crate) fn add_fragment_tree(&mut self, root: &Arc<Fragment>) {
        let mut stack = vec![root];
        while let Some(fragment) = stack.pop() {
            let shallow = self.sizer.arc_shallow(fragment);
            if shallow == 0 {
                continue;
            }
            let mut placements = 0;
            for link in &fragment.children {
                placements += link.placement.heap_bytes();
                if let Some(inputs) = link.placement.abspos_layout_inputs() {
                    placements += self.sizer.arc_shallow(inputs);
                }
            }
            // NB: A payload counts as its own allocation. What its kind holds beyond that is not walked.
            let payload = fragment
                .payload
                .as_ref()
                .map_or(0, |payload| self.sizer.arc_shallow(payload));
            self.add(
                LayoutMemoryOwner::Fragments,
                shallow + crate::heap_size::vec_bytes(&fragment.children) + placements + payload,
            );
            if let Some(content) = &fragment.line_data {
                self.add_inline_content(content);
            }
            stack.extend(fragment.children.iter().map(|link| &link.fragment));
        }
    }

    /// Counts `content`, if the report reaches it for the first time.
    pub(crate) fn add_inline_content(&mut self, content: &Arc<InlineContent>) {
        use crate::heap_size::vec_bytes;
        let mut bytes = self.sizer.arc_shallow(content);
        if bytes == 0 {
            return;
        }
        bytes += vec_bytes(&content.lines)
            + vec_bytes(&content.fragments)
            + vec_bytes(&content.inline_box_pieces)
            + vec_bytes(&content.items)
            + vec_bytes(&content.elided_content_rects)
            + content.inline_boxes.heap_bytes()
            + content.text_fragments.heap_bytes()
            + content.glyph_ink_of_lines.heap_bytes()
            + content.lines_after_clamp_point_rect.as_deref().map_or(0, size_of_val)
            + content.inline_box_rects_after_clamp.as_deref().map_or(0, size_of_val);
        for fragment in &content.fragments {
            let Some(run) = &fragment.glyph_run else {
                continue;
            };
            let shallow = self.sizer.arc_shallow(run);
            if shallow != 0 {
                let sizer = &mut self.sizer;
                bytes += shallow + run.glyphs.heap_bytes(|glyphs| sizer.first_sight(glyphs));
                bytes += run
                    .cluster_geometry
                    .get()
                    .map_or(0, |geometry| size_of_val(&**geometry) + geometry.heap_bytes());
            }
        }
        self.add(LayoutMemoryOwner::InlineContent, bytes);
    }
}

/// The name of the layout memory owner at `index`, with its length written out, or null past the last one.
#[unsafe(no_mangle)]
pub extern "C" fn layout_memory_owner_name(index: usize, out_name_length: &mut usize) -> *const u8 {
    let Some(owner) = LayoutMemoryOwner::ALL.get(index) else {
        return std::ptr::null();
    };
    let name = owner.name();
    *out_name_length = name.len();
    name.as_ptr()
}

/// What the document's layout holds on the heap, by owner: the bytes of each owner in the order
/// `layout_memory_owner_name` names them, with the slots its arena holds, those of them that hold a live row, and the
/// rows the host made a layout node for.
#[unsafe(no_mangle)]
pub extern "C" fn layout_memory_report(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    out_bytes: ak::SpanMut<'_, u64>,
    out_slots: &mut u64,
    out_live_rows: &mut u64,
    out_layout_nodes: &mut u64,
) -> usize {
    let mut report = host.ask(read, |state| {
        let mut report = LayoutMemoryReport::default();
        let handle = state.arena_handle_mut();
        handle.arena().report_memory(&mut report);
        handle.layout_scratch().report_memory(&mut report);
        report
    });
    let shells = host.host_tables().shells.borrow();
    report.layout_nodes = shells.len();
    report.add(LayoutMemoryOwner::LayoutNodeTable, crate::heap_size::map_bytes(&shells));
    for (slot, owner) in out_bytes.into_slice().iter_mut().zip(LayoutMemoryOwner::ALL) {
        *slot = report.bytes(*owner) as u64;
    }
    *out_slots = report.slots as u64;
    *out_live_rows = report.live_rows as u64;
    *out_layout_nodes = report.layout_nodes as u64;
    LayoutMemoryOwner::COUNT
}
