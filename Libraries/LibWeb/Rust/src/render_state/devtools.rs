/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries tests and the developer tools call to ask a document's render state about itself.

use super::{DocumentHost, ScriptForcedRead};
use crate::layout::LayoutNodeArena;

/// How many tree builds a document's layout has run, and what the last one rebuilt.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiLayoutCounts {
    pub tree_builds: u64,
    pub last_tree_build_rebuilt_subtree_roots: u64,
    pub last_tree_build_escaped_rebuild_roots: bool,
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

/// How many layout passes and tree builds the document's layout has run.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_layout_counts(host: &DocumentHost) -> FfiLayoutCounts {
    read_arena(host, |arena| {
        let tree_builds = arena.layout_tree_build_stats();
        FfiLayoutCounts {
            tree_builds: tree_builds.builds,
            last_tree_build_rebuilt_subtree_roots: tree_builds.last_build_rebuilt_subtree_roots,
            last_tree_build_escaped_rebuild_roots: tree_builds.last_build_escaped_rebuild_roots,
        }
    })
}

/// How many rows the document's layout arena holds.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_live_slot_count(host: &DocumentHost) -> u64 {
    read_arena(host, |arena| arena.live_slot_count().into())
}

/// How many times scrollable overflow was recalculated outside layout, counting from zero again where `reset`.
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
