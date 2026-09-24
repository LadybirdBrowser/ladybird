/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A layout run reads only the subtree it lays out. Whatever it needs from above arrives through its
//! layout input, its root's used values and its records, like the containing block of its root, or
//! was derived onto the subtree before the pass, like ancestor facts. Out-of-flow boxes learn their
//! containing block on the way up, from the run that reaches it. That is what lets partial relayout,
//! the formatting context run cache and the intrinsic size caches reuse a subtree's layout without
//! looking outside it.
//!
//! Debug builds check every node LayoutPass hands out against the subtree of the innermost run and,
//! during a partial relayout, the subtree of the boundary. Pre-order labels make each check a range
//! test: a subtree covers the labels from its root's up to its successor's.

use super::LayoutNodeArena;
use super::node_data::NodeSlotId;

#[cfg(debug_assertions)]
#[derive(Clone, Copy)]
pub(crate) struct ReadScope {
    root: NodeSlotId,
    first_label: u64,
    end_label: u64,
}

#[cfg(debug_assertions)]
impl Default for ReadScope {
    fn default() -> Self {
        Self {
            root: NodeSlotId::INVALID,
            first_label: 0,
            end_label: u64::MAX,
        }
    }
}

#[must_use]
pub(crate) struct ReadScopeGuard<'a> {
    #[cfg(debug_assertions)]
    arena: &'a LayoutNodeArena,
    #[cfg(debug_assertions)]
    previous: ReadScope,
    #[cfg(not(debug_assertions))]
    arena: std::marker::PhantomData<&'a LayoutNodeArena>,
}

#[cfg(debug_assertions)]
impl Drop for ReadScopeGuard<'_> {
    fn drop(&mut self) {
        self.arena.read_scope.set(self.previous);
    }
}

impl LayoutNodeArena {
    /// Limits the nodes layout may read to the inclusive subtree of `root`, within the current limit,
    /// until the guard drops.
    pub(crate) fn enter_read_scope(&self, root: NodeSlotId) -> ReadScopeGuard<'_> {
        #[cfg(debug_assertions)]
        {
            let previous = self.read_scope.get();
            self.read_scope.set(ReadScope {
                root,
                first_label: self.node_pre_order_label(root).max(previous.first_label),
                end_label: self.pre_order_label_of_subtree_successor(root).min(previous.end_label),
            });
            ReadScopeGuard { arena: self, previous }
        }
        #[cfg(not(debug_assertions))]
        {
            let _ = root;
            ReadScopeGuard {
                arena: std::marker::PhantomData,
            }
        }
    }

    #[inline]
    pub(crate) fn assert_layout_read_is_in_scope(&self, node: NodeSlotId) {
        #[cfg(debug_assertions)]
        {
            let scope = self.read_scope.get();
            let label = self.node_pre_order_label(node);
            assert!(
                scope.first_label <= label && label < scope.end_label,
                "layout read slot {} outside the subtree rooted at slot {}",
                node.slot_index(),
                scope.root.slot_index()
            );
        }
        #[cfg(not(debug_assertions))]
        let _ = node;
    }
}
