/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries tests and the developer tools call to ask a document's render state about itself.

use super::{DocumentHost, ScriptForcedRead};
use crate::layout::LayoutNodeArena;

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

/// How many rows the document's layout arena holds.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_live_slot_count(host: &DocumentHost) -> u64 {
    read_arena(host, |arena| arena.live_slot_count().into())
}
