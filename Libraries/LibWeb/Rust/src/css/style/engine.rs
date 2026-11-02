/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

// The engine may move to whichever thread runs a style stage. Every host object it keeps a
// reference to says by its type whether it may be shared between threads, so one that cannot be
// fails this rather than passing as an opaque pointer.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<StyleEngine>();
};

impl StyleEngine {
    #[must_use]
    pub fn counters(&self) -> &Counters {
        &self.retained.counters
    }

    /// Mint `out.len()` element identities from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_style_nodes(&mut self, out: &mut [u32]) {
        for slot in out.iter_mut() {
            *slot = self.retained.tree.mint_test_identity(false).raw();
        }
        self.mint_style_nodes(out);
    }

    /// Mint `out.len()` text identities from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_text_style_nodes(&mut self, out: &mut [u32]) {
        for slot in out.iter_mut() {
            *slot = self.retained.tree.mint_test_identity(true).raw();
        }
        self.mint_style_nodes(out);
    }

    /// Leaves the atom sweep of the transactions taken until the next call to a later one, where `defers`.
    pub(crate) fn defer_atom_sweep(&mut self, defers: bool) {
        self.host.defers_atom_sweep = defers;
    }
}
