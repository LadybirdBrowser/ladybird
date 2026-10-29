/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries tests and the developer tools call to ask a document's render state about itself.

use super::{DocumentHost, ScriptForcedRead};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use std::ffi::c_void;

/// How many layout passes and tree builds a document's layout has run, and how many rows it holds.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiLayoutCounts {
    pub full_layouts: u64,
    pub partial_layouts: u64,
    pub tree_builds: u64,
    pub last_tree_build_rebuilt_subtree_roots: u64,
    pub last_tree_build_escaped_rebuild_roots: bool,
    pub live_slots: u64,
    pub pre_order_relabels: u64,
}

/// Mints the forced reads of this module's entries; only this module can make one.
pub(crate) struct DevtoolsEntry {
    _private: (),
}

const DEVTOOLS_ENTRY: DevtoolsEntry = DevtoolsEntry { _private: () };

/// Makes the render state of `host`'s document panic answering this call, which waits for it.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_panic_for_testing(host: &DocumentHost) {
    host.run(ScriptForcedRead::at_script_entry(&DEVTOOLS_ENTRY), false, |_| {
        panic!("the render state panicked for a test")
    });
}

/// Answers `read` of the layout arena of `host`'s document, spending the forced read of the call that reached the entry.
fn read_arena<R>(host: &DocumentHost, read: impl FnOnce(&mut LayoutNodeArena) -> R) -> R {
    host.ask(ScriptForcedRead::at_script_entry(&DEVTOOLS_ENTRY), |state| {
        read(state.arena_mut())
    })
}

/// How many layout passes and tree builds the document's layout has run, and how many rows it holds.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_layout_counts(host: &DocumentHost) -> FfiLayoutCounts {
    read_arena(host, |arena| {
        let tree_builds = arena.layout_tree_build_stats();
        FfiLayoutCounts {
            full_layouts: arena.full_layout_count(),
            partial_layouts: arena.partial_layout_count(),
            tree_builds: tree_builds.builds,
            last_tree_build_rebuilt_subtree_roots: tree_builds.last_build_rebuilt_subtree_roots,
            last_tree_build_escaped_rebuild_roots: tree_builds.last_build_escaped_rebuild_roots,
            live_slots: arena.live_slot_count().into(),
            pre_order_relabels: arena.pre_order_relabel_count(),
        }
    })
}

/// How many rows of the layout subtree `root` heads carry a pre-order label no greater than the row before them.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_pre_order_label_violation_count(host: &DocumentHost, root: NodeSlotId) -> u64 {
    read_arena(host, |arena| {
        if !arena.slot_is_live(root) {
            return 0;
        }
        let mut violation_count = 0;
        let mut previous_label: Option<u64> = None;
        arena.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
            let label = arena.node_pre_order_label(node);
            if previous_label.is_some_and(|previous| label <= previous) {
                violation_count += 1;
            }
            previous_label = Some(label);
        });
        violation_count
    })
}

/// How many times paint preparation recalculated scrollable overflow, counting from zero again where `reset`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_scrollable_overflow_recalculation_count(host: &DocumentHost, reset: bool) -> u64 {
    read_arena(host, |arena| {
        let recalculations = &mut arena.scrollable_overflow.recalculations;
        if reset {
            std::mem::take(recalculations)
        } else {
            *recalculations
        }
    })
}

/// How many times paint preparation rebuilt the index of boxes whose containing block is not their parent, counting
/// from zero again where `reset`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_overflow_contained_box_index_refill_count(host: &DocumentHost, reset: bool) -> u64 {
    read_arena(host, |arena| {
        let refills = &mut arena.scrollable_overflow.contained_box_index_refills;
        if reset { std::mem::take(refills) } else { *refills }
    })
}

/// Where the stacking context structure below `viewport` differs from what paint preparation recorded, handed to
/// `consume` unless it does not.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `consume` must copy the bytes synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_stacking_context_structure_verification_report(
    host: &DocumentHost,
    viewport: NodeSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let report = read_arena(host, |arena| {
        crate::painting::stacking_context::verify::verification_report(arena, viewport)
    });
    if !report.is_empty() {
        // SAFETY: Guaranteed by the caller.
        unsafe { consume(context, report.as_ptr(), report.len()) };
    }
}

/// How many layout nodes the host made for the rows of its document.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_shell_count(host: &DocumentHost) -> u32 {
    host.host_tables().shells.borrow().len() as u32
}
