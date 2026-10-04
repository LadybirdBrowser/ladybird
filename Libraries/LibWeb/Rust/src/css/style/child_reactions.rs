/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The reactions an applied style reaction derives for the element's children.
//!
//! What a child reads of its parent is the inherited half of its style, its custom-property
//! environment, and its display; a change confined to anything else reaches no child. C++ reports
//! each reaction it applied together with what its invalidation says moved, and the engine turns
//! that into the exact style inputs of the flat-tree children for the next transaction. A row the
//! engine settles itself derives the same reactions from its two records and the damage of their
//! move, which a child that is a row of the same batch takes before it settles.

use super::bridge::element_adjustment_fact;
use super::bridge::style_reaction_applied_fact as fact;
use super::style_invalidation::child_reaction_facts_of_damage;
use super::transaction::{
    STYLE_REACTION_ANCESTOR_BECAME_VISIBLE, STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES, STYLE_REACTION_INHERITED_STYLE,
    STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES, STYLE_REACTION_RECOMPUTE_STYLE,
};
use super::{StyleEngineState, StyleNodeID};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::display::FfiDisplay;
use crate::css::host_shared::SharedPayload;
use smallvec::SmallVec;

/// Every inherited style group, for a change that reaches all of them.
const ALL_INHERITED_STYLE_GROUPS: u8 = (1 << 7) - 1;

/// What an element's installed record generates, as its children read it.
struct InstalledRecordState {
    is_display_none: bool,
    in_display_none_subtree: bool,
}

/// One reaction a reaction applied to an element derives for a child of it, or for an element a
/// slot assigns.
#[derive(Clone, Copy)]
pub(super) struct DerivedChildReaction {
    pub(super) child: StyleNodeID,
    /// What the child reacts to; zero when only its parent's display moved.
    pub(super) reaction: u8,
    pub(super) inherited_style_groups: u8,
    /// The parent's display moved, which the child's box-type transformation reads: the child's
    /// record is driven again in full, whatever its own winners did.
    pub(super) parent_display_moved: bool,
}

impl StyleEngineState {
    /// What the element's installed record generates, or `None` for an element without style.
    fn installed_record_state(&self, node: StyleNodeID) -> Option<InstalledRecordState> {
        let record = self.retained.computed_group_sets.assigned_style_record(node)?;
        let view = self.retained.computed_group_sets.style_record_view(record.raw())?;
        let values = ComputedValuesView::new(SharedPayload::as_pointer_slice(view.payloads));
        Some(InstalledRecordState {
            is_display_none: values.display().is_none(),
            in_display_none_subtree: view.dependency_flags & (1 << 2) != 0,
        })
    }

    /// The display a record generates, or `None` for a record the engine cannot read.
    fn record_display(&self, style_record: u64) -> Option<FfiDisplay> {
        let view = self.retained.computed_group_sets.style_record_view(style_record)?;
        Some(ComputedValuesView::new(SharedPayload::as_pointer_slice(view.payloads)).display())
    }

    /// Derive the children's reactions from a reaction C++ applied to `node`: `reaction` is what
    /// the element reacted to, `inherited_style_groups_changed` names the inherited groups its
    /// style moved, and `facts` says what else the application found. What the element's
    /// installed record generates is read from the record.
    pub fn note_style_reaction_applied(
        &mut self,
        node: StyleNodeID,
        reaction: u8,
        inherited_style_groups_changed: u8,
        facts: u32,
    ) {
        let mut derived = SmallVec::<[DerivedChildReaction; 8]>::new();
        self.derive_child_reactions(node, reaction, inherited_style_groups_changed, facts, |child| {
            derived.push(child);
        });
        for child in derived {
            self.record_derived_element_style_input(child.child, child.reaction, child.inherited_style_groups);
            if child.parent_display_moved {
                self.retained.row_inputs_moved.note_parent_display_moved(child.child);
            }
        }
    }

    /// Whether the engine derives the children's reactions of a row it settled for `node`, moving
    /// the element from `old_style_record` to `new_style_record`, as the host's application of the
    /// row derives them: the host applies it over the record the engine moved it from, and the row
    /// moves nothing the host moves on its own once it installed it, the element's custom-property
    /// environment and the root's font metrics.
    pub(super) fn engine_row_derives_children(
        &self,
        node: StyleNodeID,
        old_style_record: u64,
        new_style_record: u64,
    ) -> bool {
        let records = &self.retained.computed_group_sets;
        new_style_record != 0
            && self.retained.held_style_records.get(&node).copied().unwrap_or(0) == old_style_record
            && records.adjustment_facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT == 0
            && (old_style_record == 0
                || records.style_record_custom_property_environment(old_style_record)
                    == records.style_record_custom_property_environment(new_style_record))
    }

    /// Derive the children's reactions from a row the engine settled for `node`, as the host's
    /// application of it would: the element moves from `old_style_record` to `new_style_record`,
    /// and `damages` are what its own move and its pseudo-elements' moves damage.
    pub(super) fn derive_engine_row_child_reactions(
        &self,
        node: StyleNodeID,
        reaction: u8,
        old_style_record: u64,
        new_style_record: u64,
        damages: impl IntoIterator<Item = u32>,
        sink: impl FnMut(DerivedChildReaction),
    ) {
        let (inherited_style_groups_changed, mut facts) = child_reaction_facts_of_damage(damages);
        if old_style_record == 0 {
            // A first style is a full invalidation.
            facts = (facts & fact::RECOMPUTE_DESCENDANT_STYLES) | fact::NEEDS_LAYOUT_TREE_REBUILD | fact::WAS_UNSTYLED;
        } else {
            let before = self.record_display(old_style_record);
            if before.is_some_and(|display| display.is_none()) {
                facts |= fact::WAS_DISPLAY_NONE;
            }
            if before.is_some_and(|before| self.record_display(new_style_record).is_some_and(|now| now != before)) {
                facts |= fact::DISPLAY_CHANGED;
            }
        }
        if self.retained.children_explicitly_inherit_marks.contains(&node) {
            facts |= fact::CHILDREN_EXPLICITLY_INHERIT;
        }
        if self
            .retained
            .tree
            .shadow_root_of(node)
            .is_some_and(|root| self.retained.children_explicitly_inherit_marks.contains(&root))
        {
            facts |= fact::SHADOW_CHILDREN_EXPLICITLY_INHERIT;
        }
        self.derive_child_reactions(node, reaction, inherited_style_groups_changed, facts, sink);
    }

    /// The reactions a reaction applied to `node` derives for its children, given to `sink`.
    fn derive_child_reactions(
        &self,
        node: StyleNodeID,
        reaction: u8,
        inherited_style_groups_changed: u8,
        facts: u32,
        mut sink: impl FnMut(DerivedChildReaction),
    ) {
        let has = |bit: u32| facts & bit != 0;
        let did_change_custom_properties = has(fact::DID_CHANGE_CUSTOM_PROPERTIES);
        let invalidation_is_none = has(fact::INVALIDATION_IS_NONE);
        let ancestor_became_visible = reaction & STYLE_REACTION_ANCESTOR_BECAME_VISIBLE != 0;
        let mut derive = |child: StyleNodeID, reaction: u8, inherited_style_groups: u8, parent_display_moved: bool| {
            sink(DerivedChildReaction {
                child,
                reaction,
                inherited_style_groups,
                parent_display_moved,
            });
        };

        // A slot's assigned elements take their style from the slot, and a slot that moved at all
        // recomputes them.
        if self.retained.facts.is_slot(node)
            && (!invalidation_is_none || did_change_custom_properties || ancestor_became_visible)
        {
            for &assigned in self.retained.tree.assigned_nodes_of(node) {
                // A text slottable holds a place in the list but has no style of its own to recompute.
                if assigned.text_index().is_some() {
                    continue;
                }
                derive(assigned, STYLE_REACTION_RECOMPUTE_STYLE, 0, false);
            }
        }

        // A descendant whose style was cleared on entry to display:none stays unmaterialized.
        let Some(installed) = self.installed_record_state(node) else {
            return;
        };

        if installed.is_display_none {
            let (child_reaction, groups) = if has(fact::WAS_UNSTYLED) {
                (STYLE_REACTION_RECOMPUTE_STYLE, 0)
            } else {
                let mut child_reaction = 0;
                if did_change_custom_properties {
                    child_reaction |= STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES;
                }
                if inherited_style_groups_changed != 0 {
                    child_reaction |= STYLE_REACTION_INHERITED_STYLE;
                }
                if has(fact::RECOMPUTE_DESCENDANT_STYLES) {
                    child_reaction |= STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES;
                }
                (child_reaction, inherited_style_groups_changed)
            };
            if child_reaction == 0 {
                return;
            }
            for parent in [Some(node), self.retained.tree.shadow_root_of(node)]
                .into_iter()
                .flatten()
            {
                let mut next = self.retained.tree.first_element_child(parent);
                while let Some(child) = next {
                    next = self.retained.tree.next_element_sibling(child);
                    if parent != node || self.retained.tree.assigned_slot_of(child).is_none() {
                        derive(child, child_reaction, groups, false);
                    }
                }
            }
            return;
        }

        let mut common_child_reaction = 0;
        if reaction & STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES != 0 || did_change_custom_properties {
            common_child_reaction |= STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES;
        }
        if has(fact::NEEDS_LAYOUT_TREE_REBUILD) {
            common_child_reaction |= STYLE_REACTION_RECOMPUTE_STYLE;
        }
        if reaction & STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES != 0 || has(fact::RECOMPUTE_DESCENDANT_STYLES) {
            common_child_reaction |= STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES;
        }
        if ancestor_became_visible || (has(fact::WAS_DISPLAY_NONE) && !installed.in_display_none_subtree) {
            common_child_reaction |= STYLE_REACTION_ANCESTOR_BECAME_VISIBLE;
        }

        let child_reaction = |children_explicitly_inherit: bool| {
            let groups = if !invalidation_is_none && children_explicitly_inherit {
                ALL_INHERITED_STYLE_GROUPS
            } else {
                inherited_style_groups_changed
            };
            let reaction = common_child_reaction | if groups != 0 { STYLE_REACTION_INHERITED_STYLE } else { 0 };
            (reaction, groups)
        };
        let display_changed = has(fact::DISPLAY_CHANGED);
        let mut next = self.retained.tree.first_element_child(node);
        while let Some(child) = next {
            next = self.retained.tree.next_element_sibling(child);
            if self.retained.tree.assigned_slot_of(child).is_some() {
                continue;
            }
            let (light_reaction, light_groups) = child_reaction(
                !invalidation_is_none
                    && (has(fact::CHILDREN_EXPLICITLY_INHERIT)
                        || self.node_explicitly_inherits_non_inherited_property(child)),
            );
            derive(child, light_reaction, light_groups, display_changed);
        }
        let mut next = self
            .tree
            .shadow_root_of(node)
            .and_then(|root| self.retained.tree.first_element_child(root));
        while let Some(child) = next {
            next = self.retained.tree.next_element_sibling(child);
            let (shadow_reaction, shadow_groups) = child_reaction(
                !invalidation_is_none
                    && (has(fact::SHADOW_CHILDREN_EXPLICITLY_INHERIT)
                        || self.node_explicitly_inherits_non_inherited_property(child)),
            );
            derive(child, shadow_reaction, shadow_groups, display_changed);
        }
    }
}
