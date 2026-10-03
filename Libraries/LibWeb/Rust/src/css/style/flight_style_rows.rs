/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rows of a style transaction a frame applies to the layout nodes of their elements itself, ahead of the host that
//! installs the transaction on the elements once the frame has landed.
//!
//! A frame that goes on from its style transaction to a layout round lays out what the round reads of the
//! transaction's rows: each row's record, bound to the box of the row's element, and the relayout the row's move asks
//! for. It does so only for a transaction whose install leaves the host nothing the round reads: every row is an
//! element's record the engine computed over the one the element holds, with its damage answered, which rebuilds
//! nothing, reaches no descendant and no pseudo-element, moves no custom property environment, animates nothing and
//! names no image or anchor. The host installs any other transaction before it lays out, as without the frame.

use super::bridge::{FfiStyleDelta, FfiStyleDeltaGap, FfiStyleInvalidationField};
use super::transaction::{
    STYLE_REACTION_ANCESTOR_BECAME_VISIBLE, STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES,
    STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES,
};
use super::{StyleEngine, StyleNodeID};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::host_shared::SharedPayload;

/// A row of a style transaction a frame applies to the box of its element.
pub(crate) struct FlightStyleRow {
    pub(crate) style_node: StyleNodeID,
    pub(crate) old_style_record: u64,
    pub(crate) new_style_record: u64,
    /// Whether the move lays the element's box out again.
    pub(crate) relayout: bool,
}

/// Why a frame leaves a style transaction's rows to the host.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Decline {
    /// A row is a pseudo-element's, or one the host settles in a way of its own.
    Row,
    /// A row is an element's first style.
    FirstStyle,
    /// A row's damage is the host's to compare.
    Damage,
    /// A row rebuilds the layout tree.
    Rebuild,
    /// A row's move reaches its element's descendants.
    Descendants,
    /// A row's element owes its animations or transitions something.
    Animation,
    /// A row moved a custom property environment.
    CustomProperties,
    /// A row's records hold images or name anchors.
    Resources,
    /// A row's box is one the frame does not style.
    LayoutNode,
}

/// The `InvalidationLevel` of a style move at which its box lays out again, and the one at which the layout tree is
/// built again.
const RELAYOUT_LEVEL: u32 = 2;
const REBUILD_LEVEL: u32 = 3;

impl StyleEngine {
    /// The rows of `answers`, a style transaction's, that a frame applies to the boxes of their elements itself, or why
    /// it leaves them to the host.
    pub(crate) fn rows_the_flight_applies(&self, answers: &[FfiStyleDelta]) -> Result<Vec<FlightStyleRow>, Decline> {
        let mut rows = Vec::with_capacity(answers.len());
        for answer in answers {
            // An element the engine answered as hidden needs no style until a read or its subtree's reveal asks.
            if answer.gap == FfiStyleDeltaGap::Hidden {
                continue;
            }
            if answer.pseudo_kind != u8::MAX || answer.gap != FfiStyleDeltaGap::Computed {
                return Err(Decline::Row);
            }
            let style_node = StyleNodeID::from_raw(answer.style_node).ok_or(Decline::Row)?;
            if answer.reaction
                & (STYLE_REACTION_ANCESTOR_BECAME_VISIBLE
                    | STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES
                    | STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES)
                != 0
            {
                return Err(Decline::Descendants);
            }
            if answer.old_style_record == 0 || answer.new_style_record == 0 {
                return Err(Decline::FirstStyle);
            }
            if answer.owes_an_animation_plan || answer.owes_a_transition_step || answer.composed_by_the_host {
                return Err(Decline::Animation);
            }
            if answer.old_style_record == answer.new_style_record {
                continue;
            }
            let damage = answer.record_damage;
            if damage & FfiStyleInvalidationField::EngineComputed as u32 == 0 {
                return Err(Decline::Damage);
            }
            let level = damage & FfiStyleInvalidationField::LevelMask as u32;
            if level >= REBUILD_LEVEL {
                return Err(Decline::Rebuild);
            }
            let reaches_descendants = FfiStyleInvalidationField::RecomputeDescendants as u32
                | FfiStyleInvalidationField::ResnapScrollContainer as u32
                | FfiStyleInvalidationField::RepaintHighlights as u32
                | FfiStyleInvalidationField::NonInheritedInheritanceSource as u32
                | (FfiStyleInvalidationField::InheritedGroupsMask as u32)
                    << FfiStyleInvalidationField::InheritedGroupsShift as u32;
            if damage & reaches_descendants != 0 {
                return Err(Decline::Descendants);
            }
            let sets = &self.computed_group_sets;
            if sets.style_record_custom_property_environment(answer.old_style_record)
                != sets.style_record_custom_property_environment(answer.new_style_record)
            {
                return Err(Decline::CustomProperties);
            }
            if self.record_holds_resources(answer.old_style_record)
                || self.record_holds_resources(answer.new_style_record)
            {
                return Err(Decline::Resources);
            }
            // A box that becomes a size container, or stops being one, has its queries evaluated after a full layout,
            // which the round was sealed without.
            if self.record_is_size_container(answer.old_style_record)
                != self.record_is_size_container(answer.new_style_record)
            {
                return Err(Decline::LayoutNode);
            }
            rows.push(FlightStyleRow {
                style_node,
                old_style_record: answer.old_style_record,
                new_style_record: answer.new_style_record,
                relayout: level >= RELAYOUT_LEVEL,
            });
        }
        Ok(rows)
    }

    /// Whether `record` makes its box a size query container.
    fn record_is_size_container(&self, record: u64) -> bool {
        self.style_record_payloads(record).is_some_and(|payloads| {
            let box_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads)).box_values();
            box_values.is_size_container || box_values.is_inline_size_container
        })
    }

    /// Whether `record` holds an image a box loads, or names an anchor the host registers, or is gone.
    fn record_holds_resources(&self, record: u64) -> bool {
        let holds_images = self
            .style_record_dependency_flags(record)
            .is_none_or(|flags| flags & super::computed::HOLDS_IMAGE_VALUES != 0);
        holds_images
            || self.style_record_payloads(record).is_none_or(|payloads| {
                !ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads))
                    .anchor()
                    .anchor_names
                    .as_slice()
                    .is_empty()
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::bridge::FfiStyleDeltaDamage;

    fn row(style_node: u32) -> FfiStyleDelta {
        FfiStyleDelta {
            style_node,
            match_answer: 0,
            old_style_record: 1,
            new_style_record: 2,
            damage: FfiStyleDeltaDamage::Full,
            reaction: 0,
            inherited_style_groups: 0,
            pseudo_kind: u8::MAX,
            gap: FfiStyleDeltaGap::Computed,
            uses_substitution: false,
            record_reads: 0,
            explicitly_inherited_groups: 0,
            record_damage: FfiStyleInvalidationField::EngineComputed as u32,
            owes_an_animation_plan: false,
            owes_a_transition_step: false,
            composed_by_the_host: false,
        }
    }

    fn decline(answer: FfiStyleDelta) -> Option<Decline> {
        let host = crate::render_state::TestHost::new();
        // SAFETY: The engine lives as long as the test host, which nothing else reaches meanwhile.
        let engine = unsafe { host.engine().get() };
        engine.rows_the_flight_applies(&[answer]).err()
    }

    #[test]
    fn a_frame_leaves_the_host_every_row_it_cannot_apply_alone() {
        assert_eq!(
            decline(FfiStyleDelta {
                pseudo_kind: 0,
                ..row(1)
            }),
            Some(Decline::Row)
        );
        assert_eq!(
            decline(FfiStyleDelta {
                gap: FfiStyleDeltaGap::Materialize,
                ..row(1)
            }),
            Some(Decline::Row)
        );
        assert_eq!(
            decline(FfiStyleDelta {
                old_style_record: 0,
                ..row(1)
            }),
            Some(Decline::FirstStyle)
        );
        assert_eq!(
            decline(FfiStyleDelta {
                reaction: STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES,
                ..row(1)
            }),
            Some(Decline::Descendants)
        );
        assert_eq!(
            decline(FfiStyleDelta {
                owes_a_transition_step: true,
                ..row(1)
            }),
            Some(Decline::Animation)
        );
        assert_eq!(
            decline(FfiStyleDelta {
                record_damage: 0,
                ..row(1)
            }),
            Some(Decline::Damage)
        );
        assert_eq!(
            decline(FfiStyleDelta {
                record_damage: FfiStyleInvalidationField::EngineComputed as u32 | REBUILD_LEVEL,
                ..row(1)
            }),
            Some(Decline::Rebuild)
        );
        assert_eq!(
            decline(FfiStyleDelta {
                record_damage: FfiStyleInvalidationField::EngineComputed as u32
                    | FfiStyleInvalidationField::RecomputeDescendants as u32,
                ..row(1)
            }),
            Some(Decline::Descendants)
        );
    }

    #[test]
    fn a_hidden_row_or_one_that_keeps_its_record_leaves_the_frame_nothing_to_apply() {
        let host = crate::render_state::TestHost::new();
        // SAFETY: As above.
        let engine = unsafe { host.engine().get() };
        let hidden = FfiStyleDelta {
            gap: FfiStyleDeltaGap::Hidden,
            ..row(1)
        };
        let kept = FfiStyleDelta {
            new_style_record: 1,
            ..row(2)
        };
        assert!(
            engine
                .rows_the_flight_applies(&[hidden, kept])
                .is_ok_and(|rows| rows.is_empty())
        );
    }
}
