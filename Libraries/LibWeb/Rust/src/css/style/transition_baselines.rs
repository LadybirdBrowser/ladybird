/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The before-change styles a style stabilization epoch decides CSS transitions against.

use super::boundary::StyleChange;
use super::tree::StyleNodeID;
use super::{HashMap, RetainedState};
use crate::css::animated_overlay::FfiAnimatedOverlayEntry;
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::style_value::StyleValueData;
use smallvec::SmallVec;

/// https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
/// Per transition target, by element and then pseudo-element kind, the before-change style its transitions are decided
/// against for the rest of the style stabilization epoch: the first record named for the target. The engine keeps them
/// pinned until the epoch commits, and the host follows every baseline it records for the engine, the commit that
/// releases them, and every identity a transaction releases, as the engine drops a retired node's baselines, so it
/// knows each without asking.
#[derive(Default)]
pub(crate) struct TransitionBaselines {
    baselines: HashMap<StyleNodeID, SmallVec<[(u8, u64); 1]>>,
}

impl TransitionBaselines {
    /// Keeps `style_record` as the target's baseline where it has none, answering whether it did.
    fn insert(&mut self, node: StyleNodeID, pseudo_kind: u8, style_record: u64) -> bool {
        let baselines = self.baselines.entry(node).or_default();
        if baselines.iter().any(|&(kind, _)| kind == pseudo_kind) {
            return false;
        }
        baselines.push((pseudo_kind, style_record));
        true
    }

    /// The baseline of the target, or 0 before a pass has recorded one.
    pub(crate) fn get(&self, node: StyleNodeID, pseudo_kind: u8) -> u64 {
        self.baselines
            .get(&node)
            .and_then(|baselines| baselines.iter().find(|&&(kind, _)| kind == pseudo_kind))
            .map_or(0, |&(_, style_record)| style_record)
    }

    /// Lets go of the baselines of `node`'s element and pseudo-elements, answering their records.
    pub(crate) fn remove(&mut self, node: StyleNodeID) -> impl Iterator<Item = u64> {
        self.baselines
            .remove(&node)
            .into_iter()
            .flatten()
            .map(|(_, style_record)| style_record)
    }

    /// Lets go of every baseline, answering their records.
    fn take(&mut self) -> impl Iterator<Item = u64> {
        std::mem::take(&mut self.baselines)
            .into_values()
            .flatten()
            .map(|(_, style_record)| style_record)
    }

    /// Follows `change`, a write the host queued for the engine, as the engine applies it.
    pub(crate) fn follow(&mut self, change: &StyleChange) {
        match *change {
            StyleChange::RecordTransitionBaseline {
                node: Some(node),
                pseudo_kind,
                style_record,
            } if style_record != 0 => {
                self.insert(node, pseudo_kind, style_record);
            }
            StyleChange::ReleaseTransitionBaselines => self.baselines.clear(),
            _ => {}
        }
    }

    /// Forgets the baselines of the nodes a transaction retired, whose identities it `released`.
    pub(crate) fn forget(&mut self, released: &[u32]) {
        for node in released.iter().filter_map(|&raw| StyleNodeID::from_raw(raw)) {
            self.baselines.remove(&node);
        }
    }
}

/// What a transition decides an inherited, animated value against.
#[derive(Clone, Copy)]
pub(crate) enum InheritedAnimatedValue<'a> {
    /// An ancestor's animation, other than a transition, sets the value.
    Animation(&'a FfiAnimatedOverlayEntry),
    /// Ancestors transition the value; this is the base value beneath their transitions.
    BeneathTransitions(&'a StyleValueData),
}

impl RetainedState {
    /// Style, layout or animation feedback can give a target a transition in any later pass of
    /// the epoch, and that transition starts from the style the target held before the epoch's
    /// first pass. The first record named for a target is that style: it is kept, pinned, until
    /// the epoch commits.
    pub(crate) fn record_transition_baseline(&mut self, node: StyleNodeID, pseudo_kind: u8, style_record: u64) {
        if style_record != 0 && self.transition_baselines.insert(node, pseudo_kind, style_record) {
            self.computed_group_sets.pin_style_record(style_record);
        }
    }

    /// The baseline of `node`'s element or pseudo-element of kind `pseudo_kind`, or 0 before a pass has recorded one.
    pub(crate) fn transition_baseline(&self, node: StyleNodeID, pseudo_kind: u8) -> u64 {
        self.transition_baselines.get(node, pseudo_kind)
    }

    /// Where an inherited value of `property` in `table`, a record of `node`'s, comes from when an
    /// ancestor animates it. A record the engine derives holds the animated value its parent passed
    /// on in its table, where a transition decides against the value beneath the animations: an
    /// animation's value, where an ancestor's animation other than a transition sets it, or else the
    /// base value of the nearest ancestor that does not inherit the property, above one that
    /// transitions it. None when no ancestor along that chain animates the property.
    pub(crate) fn inherited_animated_value(
        &self,
        node: StyleNodeID,
        table: &ComputedLonghandTable,
        property: u16,
    ) -> Option<InheritedAnimatedValue<'_>> {
        if !table.is_inherited(property) {
            return None;
        }
        let mut transition_entry = None;
        let mut ancestor = self.tree.inheritance_parent(node);
        while let Some(current) = ancestor {
            let record = self.computed_group_sets.assigned_style_record(current)?;
            let view = self.computed_group_sets.style_record_view(record.raw())?;
            let ancestor_table = unsafe { view.longhand_table.as_ref() }?;
            if let Some(entry) = unsafe { view.animated_overlay.as_ref() }.and_then(|overlay| overlay.get(property)) {
                if !entry.result_of_transition {
                    return Some(InheritedAnimatedValue::Animation(entry));
                }
                transition_entry = Some(entry);
            }
            if !ancestor_table.is_inherited(property) {
                transition_entry?;
                return Some(InheritedAnimatedValue::BeneathTransitions(
                    ancestor_table.get(property)?.data(),
                ));
            }
            ancestor = self.tree.inheritance_parent(current);
        }
        None
    }

    /// The epoch committed: no later pass decides against these styles.
    pub(crate) fn release_transition_baselines(&mut self) {
        for style_record in self.transition_baselines.take() {
            self.computed_group_sets.unpin_style_record(style_record);
        }
    }
}
