/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The before-change styles a style stabilization epoch decides CSS transitions against.

use super::RetainedState;
use super::tree::StyleNodeID;

impl RetainedState {
    /// An epoch begins: the one before it committed and released every style it pinned.
    pub(crate) fn begin_transition_baselines(&self) {
        debug_assert!(self.transition_baselines.is_empty());
    }

    /// https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
    /// Style, layout or animation feedback can give a target a transition in any later pass of
    /// the epoch, and that transition starts from the style the target held before the epoch's
    /// first pass. The first record named for a target is that style: it is kept, pinned, until
    /// the epoch commits.
    pub(crate) fn record_transition_baseline(&mut self, node: StyleNodeID, pseudo_kind: u8, style_record: u64) {
        if style_record == 0 {
            return;
        }
        let baselines = self.transition_baselines.entry(node).or_default();
        if baselines.iter().any(|&(kind, _)| kind == pseudo_kind) {
            return;
        }
        self.computed_group_sets.pin_style_record(style_record);
        baselines.push((pseudo_kind, style_record));
    }

    /// The before-change style the epoch decides the target's transitions against, or 0 before a
    /// pass has recorded one.
    pub(crate) fn transition_baseline(&self, node: StyleNodeID, pseudo_kind: u8) -> u64 {
        self.transition_baselines
            .get(&node)
            .and_then(|baselines| baselines.iter().find(|&&(kind, _)| kind == pseudo_kind))
            .map_or(0, |&(_, style_record)| style_record)
    }

    /// The epoch committed: no later pass decides against these styles.
    pub(crate) fn release_transition_baselines(&mut self) {
        for (_, baselines) in std::mem::take(&mut self.transition_baselines) {
            for (_, style_record) in baselines {
                self.computed_group_sets.unpin_style_record(style_record);
            }
        }
    }
}
