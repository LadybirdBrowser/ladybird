/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries tests and the developer tools call to ask a document's render state about itself.

use super::{DocumentHost, RenderMessage, ScriptForcedRead, wait_for_render_state};

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
    wait_for_render_state(ScriptForcedRead::at_script_entry(&DEVTOOLS_ENTRY), host, |reply| {
        RenderMessage::PanicForTesting { reply }
    });
}
