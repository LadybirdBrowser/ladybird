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
//! that into the exact style inputs of the flat-tree children for the next transaction.

use super::bridge::style_reaction_applied_fact as fact;
use super::transaction::{
    STYLE_REACTION_ANCESTOR_BECAME_VISIBLE, STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES, STYLE_REACTION_INHERITED_STYLE,
    STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES, STYLE_REACTION_RECOMPUTE_STYLE,
};
use super::{StyleEngine, StyleNodeID};

/// Every inherited style group, for a change that reaches all of them.
const ALL_INHERITED_STYLE_GROUPS: u8 = (1 << 7) - 1;

impl StyleEngine {
    /// Derive the children's reactions from a reaction C++ applied to `node`: `reaction` is what
    /// the element reacted to, `inherited_style_groups_changed` names the inherited groups its
    /// style moved, and `facts` says what else the application found.
    pub fn note_style_reaction_applied(
        &mut self,
        node: StyleNodeID,
        reaction: u8,
        inherited_style_groups_changed: u8,
        facts: u32,
    ) {
        let has = |bit: u32| facts & bit != 0;
        let did_change_custom_properties = has(fact::DID_CHANGE_CUSTOM_PROPERTIES);
        let invalidation_is_none = has(fact::INVALIDATION_IS_NONE);
        let ancestor_became_visible = reaction & STYLE_REACTION_ANCESTOR_BECAME_VISIBLE != 0;

        // A slot's assigned elements take their style from the slot, and a slot that moved at all
        // recomputes them.
        if self.facts.is_slot(node)
            && (!invalidation_is_none || did_change_custom_properties || ancestor_became_visible)
        {
            let assigned: Vec<StyleNodeID> = self.tree.assigned_nodes_of(node).to_vec();
            for assigned in assigned {
                self.record_derived_element_style_input(assigned, STYLE_REACTION_RECOMPUTE_STYLE, 0);
            }
        }

        // A descendant whose style was cleared on entry to display:none stays unmaterialized.
        if !has(fact::HAS_STYLE) {
            return;
        }

        let light_children: Vec<StyleNodeID> = self
            .tree
            .children(node)
            .filter(|&child| self.tree.assigned_slot_of(child).is_none())
            .collect();
        let shadow_children: Vec<StyleNodeID> = self
            .tree
            .shadow_root_of(node)
            .map(|shadow_root| self.tree.children(shadow_root).collect())
            .unwrap_or_default();

        if has(fact::IS_DISPLAY_NONE) {
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
            for child in light_children.into_iter().chain(shadow_children) {
                self.record_derived_element_style_input(child, child_reaction, groups);
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
        if ancestor_became_visible || (has(fact::WAS_DISPLAY_NONE) && !has(fact::IN_DISPLAY_NONE_SUBTREE)) {
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
        // A child's box-type transformation reads its parent's display: when that moved, the
        // child's record is driven again in full, whatever its own winners did.
        let display_changed = has(fact::DISPLAY_CHANGED);
        for child in light_children {
            let (light_reaction, light_groups) = child_reaction(
                has(fact::CHILDREN_EXPLICITLY_INHERIT) || self.node_explicitly_inherits_non_inherited_property(child),
            );
            self.record_derived_element_style_input(child, light_reaction, light_groups);
            if display_changed {
                self.parent_inputs_moved_nodes.insert(child);
            }
        }
        for child in shadow_children {
            let (shadow_reaction, shadow_groups) = child_reaction(
                has(fact::SHADOW_CHILDREN_EXPLICITLY_INHERIT)
                    || self.node_explicitly_inherits_non_inherited_property(child),
            );
            self.record_derived_element_style_input(child, shadow_reaction, shadow_groups);
            if display_changed {
                self.parent_inputs_moved_nodes.insert(child);
            }
        }
    }
}
