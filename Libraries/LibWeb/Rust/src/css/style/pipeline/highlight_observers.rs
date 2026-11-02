/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The elements the host shows a highlight of each kind in: those of a selected range and their
//! ancestors, say. Style passes style the highlight pseudo-elements of these elements alone, and a
//! read of the style of any other's computes it on demand.

use super::super::*;

/// The highlight kinds the host shows in each element, as bits by place in `HIGHLIGHT_KINDS`.
#[derive(Clone, Default)]
pub(crate) struct HighlightObservers {
    /// Shared with a fork until either side writes it.
    kinds: crate::fork::ForkShared<HashMap<StyleNodeID, u8>>,
}

fn bit_of(kind: u8) -> Option<u8> {
    super::HIGHLIGHT_KINDS
        .iter()
        .position(|&highlight| highlight == kind)
        .map(|place| 1 << place)
}

impl HighlightObservers {
    /// Whether the host shows the highlight of `kind` in `node`.
    pub(crate) fn observes(&self, node: StyleNodeID, kind: u8) -> bool {
        bit_of(kind).is_some_and(|bit| self.kinds_of(node) & bit != 0)
    }

    /// The highlight kinds the host shows in `node`, as bits.
    pub(crate) fn kinds_of(&self, node: StyleNodeID) -> u8 {
        self.kinds.get(&node).copied().unwrap_or(0)
    }

    /// Make `nodes` the elements the host shows the highlight of `kind` in, and call `moved` for
    /// each element that starts or stops showing it.
    pub(crate) fn replace(&mut self, kind: u8, nodes: &[StyleNodeID], mut moved: impl FnMut(StyleNodeID)) {
        let Some(bit) = bit_of(kind) else {
            return;
        };
        let incoming: HashSet<StyleNodeID> = nodes.iter().copied().collect();
        self.kinds.retain(|&node, kinds| {
            if *kinds & bit != 0 && !incoming.contains(&node) {
                *kinds &= !bit;
                moved(node);
            }
            *kinds != 0
        });
        for node in incoming {
            let kinds = self.kinds.entry(node).or_default();
            if *kinds & bit == 0 {
                *kinds |= bit;
                moved(node);
            }
        }
    }

    /// Forget `node`, whose identity retires.
    pub(crate) fn retire(&mut self, node: StyleNodeID) {
        if self.kinds.contains_key(&node) {
            self.kinds.remove(&node);
        }
    }
}
