/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries tests and the developer tools call to ask a document's render state about itself.

use super::{DocumentHost, ScriptForcedRead, ask};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use std::ffi::c_void;

/// A read for tests and debugging, which only Internals and the WebContent debug requests ask.
pub(crate) enum DevToolsQuery {
    /// How many layout passes, tree builds and measurements the document's layout has run, and how many rows it holds.
    LayoutCounts,
    /// How many rows of the layout subtree `root` heads carry a pre-order label no greater than the row before them.
    PreOrderLabelViolations { root: NodeSlotId },
    /// How many times paint preparation recalculated scrollable overflow, counting from zero again where `reset`.
    ScrollableOverflowRecalculations { reset: bool },
    /// How many boxes wait for their visual context to be updated.
    VisualContextPendingDirtyBoxes,
    /// Where the stacking context structure below `viewport` differs from what paint preparation recorded.
    StackingContextVerification { viewport: NodeSlotId },
}

/// The answer to a [`DevToolsQuery`].
pub(crate) enum DevToolsAnswer {
    LayoutCounts(FfiLayoutCounts),
    Number(u64),
    Report(String),
}

/// How many layout passes, tree builds and measurements a document's layout has run, and how many rows it holds.
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
    pub intrinsic_measurements: u64,
    pub intrinsic_inline_measurements: u64,
    pub table_cell_measurement_cache_misses: u64,
}

impl DevToolsQuery {
    pub(super) fn answer(self, arena: &LayoutNodeArena) -> DevToolsAnswer {
        match self {
            Self::LayoutCounts => {
                let tree_builds = arena.layout_tree_build_stats();
                DevToolsAnswer::LayoutCounts(FfiLayoutCounts {
                    full_layouts: arena.full_layout_count(),
                    partial_layouts: arena.partial_layout_count(),
                    tree_builds: tree_builds.builds,
                    last_tree_build_rebuilt_subtree_roots: tree_builds.last_build_rebuilt_subtree_roots,
                    last_tree_build_escaped_rebuild_roots: tree_builds.last_build_escaped_rebuild_roots,
                    live_slots: arena.live_slot_count().into(),
                    pre_order_relabels: arena.pre_order_relabel_count(),
                    intrinsic_measurements: arena.intrinsic_measurement_count(),
                    intrinsic_inline_measurements: arena.intrinsic_inline_measurement_count(),
                    table_cell_measurement_cache_misses: arena.table_cell_measurement_cache_miss_count(),
                })
            }
            Self::PreOrderLabelViolations { root } => {
                if !arena.slot_is_live(root) {
                    return DevToolsAnswer::Number(0);
                }
                let mut violation_count = 0u64;
                let mut previous_label: Option<u64> = None;
                arena.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
                    let label = arena.node_pre_order_label(node);
                    if previous_label.is_some_and(|previous| label <= previous) {
                        violation_count += 1;
                    }
                    previous_label = Some(label);
                });
                DevToolsAnswer::Number(violation_count)
            }
            Self::ScrollableOverflowRecalculations { reset } => {
                let recalculations = &arena.scrollable_overflow.recalculations;
                DevToolsAnswer::Number(if reset {
                    recalculations.replace(0)
                } else {
                    recalculations.get()
                })
            }
            Self::VisualContextPendingDirtyBoxes => {
                DevToolsAnswer::Number(arena.paint_state().borrow().visual_context.dirty_boxes.boxes.len() as u64)
            }
            Self::StackingContextVerification { viewport } => DevToolsAnswer::Report(
                crate::painting::stacking_context::verify::verification_report(arena, viewport),
            ),
        }
    }
}

/// Mints the forced reads of this module's entries; only this module can make one.
pub(crate) struct DevtoolsEntry {
    _private: (),
}

const DEVTOOLS_ENTRY: DevtoolsEntry = DevtoolsEntry { _private: () };

/// Makes the render state of `host`'s document panic answering this call, which waits for it.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_panic_for_testing(host: *mut DocumentHost) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    host.run(ScriptForcedRead::at_script_entry(&DEVTOOLS_ENTRY), false, |_| {
        panic!("the render state panicked for a test")
    });
}

/// Asks the render state of `host`'s document `query`, spending the forced read of the call that reached the entry.
fn ask_devtools(host: *mut DocumentHost, query: DevToolsQuery) -> DevToolsAnswer {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Every entry here is called with a live document host, on its document's thread.
    let host = unsafe { &*host };
    ask(ScriptForcedRead::at_script_entry(&DEVTOOLS_ENTRY), host, query)
}

fn number(answer: DevToolsAnswer) -> u64 {
    let DevToolsAnswer::Number(number) = answer else {
        unreachable!("the question is answered with a number");
    };
    number
}

/// How many layout passes, tree builds and measurements the document's layout has run, and how many rows it holds.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_layout_counts(host: *mut DocumentHost) -> FfiLayoutCounts {
    let DevToolsAnswer::LayoutCounts(counts) = ask_devtools(host, DevToolsQuery::LayoutCounts) else {
        unreachable!("layout counts are answered with layout counts");
    };
    counts
}

/// How many rows of the layout subtree `root` heads carry a pre-order label no greater than the row before them.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_pre_order_label_violation_count(
    host: *mut DocumentHost,
    root: NodeSlotId,
) -> u64 {
    number(ask_devtools(host, DevToolsQuery::PreOrderLabelViolations { root }))
}

/// How many times paint preparation recalculated scrollable overflow, counting from zero again where `reset`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_scrollable_overflow_recalculation_count(
    host: *mut DocumentHost,
    reset: bool,
) -> u64 {
    number(ask_devtools(
        host,
        DevToolsQuery::ScrollableOverflowRecalculations { reset },
    ))
}

/// How many boxes wait for their visual context to be updated.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_visual_context_pending_dirty_box_count(host: *mut DocumentHost) -> usize {
    number(ask_devtools(host, DevToolsQuery::VisualContextPendingDirtyBoxes)) as usize
}

/// Where the stacking context structure below `viewport` differs from what paint preparation recorded, handed to
/// `consume` unless it does not.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `consume` must copy the bytes synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_stacking_context_structure_verification_report(
    host: *mut DocumentHost,
    viewport: NodeSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let DevToolsAnswer::Report(report) = ask_devtools(host, DevToolsQuery::StackingContextVerification { viewport })
    else {
        unreachable!("a verification is answered with a report");
    };
    if !report.is_empty() {
        // SAFETY: Guaranteed by the caller.
        unsafe { consume(context, report.as_ptr(), report.len()) };
    }
}

/// How many layout nodes the host made for the rows of its document.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_shell_count(host: *const DocumentHost) -> u32 {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.host_tables().shells.borrow().len() as u32
}
