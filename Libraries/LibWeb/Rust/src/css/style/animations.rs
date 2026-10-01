/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS animations an element owns, mirrored for the style computation.
//!
//! Reconciling an element's `CSSAnimation` objects against its freshly computed `animation-*`
//! longhands needs to know which animations the element already owns. The names of those
//! animations are published here, so the computation decides the reconciliation from its own
//! inputs and hands the host a plan: per definition, the animation it claims, or none.
//!
//! Only the names are mirrored: matching an animation is matching its name, and everything the
//! host does once a definition has found its animation - applying the timing, cancelling what no
//! definition claimed - it does from the list it already holds.
//!
//! Which `@keyframes` a definition runs is decided here too, from the keyframes each style scope
//! publishes, so the computation never reaches into a scope's rule cache.

use super::tree::{StyleNodeID, TreeScopeID};
use crate::css::css_string::CssString;
use std::collections::HashMap;

/// Which of an element's animation lists a row belongs to, in the host's own numbering: zero for
/// the element itself, and the pseudo-element's value plus one for each pseudo-element.
pub(crate) type AnimationSlot = u8;

/// The definition that claimed no existing animation and asks for a new one.
pub(crate) const NO_MATCHED_ANIMATION: i32 = -1;

/// The names of the CSS animations the host holds in one of an element's lists, in its order.
type CssDefinedAnimationList = (AnimationSlot, Box<[CssString]>);

/// Per element, the names of the CSS animations the host holds for it and each of its
/// pseudo-elements, in the order the host holds them.
#[derive(Default)]
pub(crate) struct CssDefinedAnimations {
    /// Owning a CSS animation is rare, so only the elements that do have a row, and a row holds
    /// only the lists that are not empty.
    rows: HashMap<StyleNodeID, Vec<CssDefinedAnimationList>>,
}

impl CssDefinedAnimations {
    /// Replace one list. An empty list drops it, so an element that stops animating stops costing
    /// anything.
    pub(crate) fn set(&mut self, node: StyleNodeID, slot: AnimationSlot, names: Box<[CssString]>) {
        let lists = self.rows.entry(node).or_default();
        let existing = lists.iter().position(|(list_slot, _)| *list_slot == slot);
        match (existing, names.is_empty()) {
            (Some(index), true) => {
                lists.swap_remove(index);
            }
            (Some(index), false) => lists[index].1 = names,
            (None, true) => {}
            (None, false) => lists.push((slot, names)),
        }
        if lists.is_empty() {
            self.rows.remove(&node);
        }
    }

    /// The names of one of an element's lists, in the order the host holds the animations.
    #[must_use]
    pub(crate) fn names(&self, node: StyleNodeID, slot: AnimationSlot) -> &[CssString] {
        self.rows
            .get(&node)
            .and_then(|lists| lists.iter().find(|(list_slot, _)| *list_slot == slot))
            .map_or(&[], |(_, names)| names)
    }

    /// Give up the lists of an identity that retires. An identity can be minted again for another
    /// element, so a list left behind would be read as that element's.
    pub(crate) fn retire(&mut self, node: StyleNodeID) {
        self.rows.remove(&node);
    }
}

/// Match newly computed animation definitions against the animations the element already owns,
/// handing `claim` each definition that claims one, with the index of the animation it claims. A
/// definition not handed to `claim` asks for a new animation.
///
/// https://drafts.csswg.org/css-animations-1/#animations
/// The same @keyframes rule name may be repeated within an animation-name. Changes to the
/// animation-name update existing animations by iterating over the new list of animations from last
/// to first, and, for each animation, finding the last matching animation in the list of existing
/// animations. If a match is found, the existing animation is updated using the animation properties
/// corresponding to its position in the new list of animations, whilst maintaining its current
/// playback time as described above. The matching animation is removed from the existing list of
/// animations such that it will not match twice. If a match is not found, a new animation is
/// created. As a result, updating animation-name from ‘a’ to ‘a, a’ will cause the existing
/// animation for ‘a’ to become the second animation in the list and a new animation will be created
/// for the first item in the list.
pub(crate) fn match_existing_animations(
    existing: &[CssString],
    definition_names: &[&CssString],
    mut claim: impl FnMut(usize, usize),
) {
    // NB: Rather than removing a matched animation from the list, mark it claimed, so that the
    //     indices stay those of the list the host holds.
    let mut claimed = vec![false; existing.len()];
    for (index, name) in definition_names.iter().enumerate().rev() {
        let Some(candidate) = (0..existing.len())
            .rev()
            .find(|&candidate| !claimed[candidate] && existing[candidate] == **name)
        else {
            continue;
        };
        claimed[candidate] = true;
        claim(index, candidate);
    }
}

/// The `@keyframes` every style scope of the document defines, as each scope's rule cache resolved
/// them, with the host's keyframe set for each name.
///
/// A scope publishes its row whenever its rule cache is built, and the host builds every scope's
/// cache before a style transaction, so resolving an animation's keyframes is a lookup here rather
/// than building a rule cache in the middle of a style computation. A keyframe set is the host's
/// refcounted object, borrowed: the scope keeps a reference to every set its row names until the
/// row is replaced or given up.
#[derive(Default)]
pub(crate) struct AnimationKeyframes {
    scopes: HashMap<TreeScopeID, HashMap<Box<[u16]>, usize>>,
    /// Which scope a shadow root's pointer identity names. The cascade attributes the winning
    /// `animation-name` declaration to a shadow root by that identity, and the scope it names is
    /// where the declaration's `@keyframes` are looked for first.
    scope_by_shadow_root: HashMap<usize, TreeScopeID>,
}

impl AnimationKeyframes {
    /// Replaces one scope's row. The names arrive packed into one buffer of code units with a length
    /// each, the way an element's animation names do. An empty row gives the scope's row up.
    pub(crate) fn set(
        &mut self,
        tree_scope: TreeScopeID,
        shadow_root_identity: usize,
        name_lengths: &[u32],
        name_units: &[u16],
        keyframe_sets: &[usize],
    ) {
        debug_assert_eq!(name_lengths.len(), keyframe_sets.len());
        if name_lengths.is_empty() {
            self.scopes.remove(&tree_scope);
            // A scope with no row answers like one that defines nothing, so its identity stops
            // naming it, unless another shadow root has since been allocated at that address and
            // published under it.
            if self.scope_by_shadow_root.get(&shadow_root_identity) == Some(&tree_scope) {
                self.scope_by_shadow_root.remove(&shadow_root_identity);
            }
            return;
        }
        if shadow_root_identity != 0 {
            self.scope_by_shadow_root.insert(shadow_root_identity, tree_scope);
        }
        let mut offset = 0;
        let sets = name_lengths
            .iter()
            .zip(keyframe_sets)
            .map(|(&length, &set)| {
                let units = &name_units[offset..offset + length as usize];
                offset += length as usize;
                (Box::from(units), set)
            })
            .collect();
        self.scopes.insert(tree_scope, sets);
    }

    /// The host's keyframe set an animation of this name runs, or `None` where no scope in its chain
    /// defines the name.
    ///
    /// The chain is the tree scope of the winning `animation-name` declaration first, because that
    /// declaration can come from a shadow-root rule - `:host()` and `::slotted()` - while the element
    /// it styles is outside that subtree, and a same-named document rule must not win over it; then
    /// the scope the element itself is in; then the document.
    #[must_use]
    pub(crate) fn resolve(
        &self,
        declaration_shadow_root_identity: usize,
        element_tree_scope: TreeScopeID,
        name: &[u16],
    ) -> Option<usize> {
        if self.scopes.is_empty() {
            return None;
        }
        let in_scope = |scope: TreeScopeID| self.scopes.get(&scope)?.get(name).copied();
        let declaration_scope = self.scope_by_shadow_root.get(&declaration_shadow_root_identity);
        declaration_scope
            .and_then(|&scope| in_scope(scope))
            .or_else(|| match element_tree_scope {
                TreeScopeID::DOCUMENT => None,
                scope => in_scope(scope),
            })
            .or_else(|| in_scope(TreeScopeID::DOCUMENT))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names(names: &[&str]) -> Box<[CssString]> {
        names
            .iter()
            .map(|name| CssString::from_utf16(&name.encode_utf16().collect::<Vec<_>>()))
            .collect()
    }

    #[test]
    fn a_list_is_replaced_per_slot_and_dropped_when_empty() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut animations = CssDefinedAnimations::default();
        animations.set(node, 0, names(&["a", "b"]));
        animations.set(node, 3, names(&["c"]));
        animations.set(node, 0, names(&["b"]));
        assert_eq!(animations.names(node, 0), &names(&["b"])[..]);
        assert_eq!(animations.names(node, 3), &names(&["c"])[..]);

        animations.set(node, 0, names(&[]));
        assert!(animations.names(node, 0).is_empty());
        animations.set(node, 3, names(&[]));
        assert!(animations.rows.is_empty());
    }

    fn matches(existing: &[&str], definitions: &[&str]) -> Vec<Option<usize>> {
        let existing = names(existing);
        let definitions = names(definitions);
        let definitions: Vec<&CssString> = definitions.iter().collect();
        let mut matches = vec![None; definitions.len()];
        match_existing_animations(&existing, &definitions, |definition, animation| {
            matches[definition] = Some(animation);
        });
        matches
    }

    #[test]
    fn an_empty_existing_list_creates_every_animation() {
        assert_eq!(matches(&[], &["a", "b"]), [None, None]);
    }

    #[test]
    fn a_repeated_name_takes_the_last_unclaimed_animation_first() {
        // `a` becoming `a, a` keeps the existing animation as the second entry and creates the first.
        assert_eq!(matches(&["a"], &["a", "a"]), [None, Some(0)]);
        assert_eq!(matches(&["a", "a"], &["a", "a"]), [Some(0), Some(1)]);
        assert_eq!(matches(&["a", "a"], &["a"]), [Some(1)]);
    }

    #[test]
    fn a_removed_name_claims_nothing() {
        assert_eq!(matches(&["a", "b"], &["b"]), [Some(1)]);
        assert_eq!(matches(&["a", "b"], &["c"]), [None]);
    }

    #[test]
    fn a_reordered_list_claims_by_name() {
        assert_eq!(matches(&["a", "b", "c"], &["c", "a", "b"]), [Some(2), Some(0), Some(1)]);
    }

    #[test]
    fn a_retired_identity_holds_no_lists_when_reissued() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut animations = CssDefinedAnimations::default();
        animations.set(node, 0, names(&["a"]));
        animations.set(node, 2, names(&["b"]));
        animations.retire(node);
        assert!(animations.names(node, 0).is_empty());
        assert!(animations.names(node, 2).is_empty());
        assert!(animations.rows.is_empty());
    }

    fn publish(keyframes: &mut AnimationKeyframes, scope: u32, identity: usize, rows: &[(&str, usize)]) {
        let lengths: Vec<u32> = rows
            .iter()
            .map(|(name, _)| name.encode_utf16().count() as u32)
            .collect();
        let units: Vec<u16> = rows.iter().flat_map(|(name, _)| name.encode_utf16()).collect();
        let sets: Vec<usize> = rows.iter().map(|&(_, set)| set).collect();
        keyframes.set(TreeScopeID(scope), identity, &lengths, &units, &sets);
    }

    fn resolve(keyframes: &AnimationKeyframes, identity: usize, scope: u32, name: &str) -> Option<usize> {
        keyframes.resolve(identity, TreeScopeID(scope), &name.encode_utf16().collect::<Vec<_>>())
    }

    #[test]
    fn keyframes_resolve_in_the_declaration_scope_then_the_element_scope_then_the_document() {
        let mut keyframes = AnimationKeyframes::default();
        publish(&mut keyframes, 0, 0, &[("a", 1), ("b", 2)]);
        publish(&mut keyframes, 1, 0x100, &[("a", 3)]);
        publish(&mut keyframes, 2, 0x200, &[("a", 4), ("b", 5)]);

        assert_eq!(resolve(&keyframes, 0, 0, "a"), Some(1));
        assert_eq!(resolve(&keyframes, 0, 1, "a"), Some(3));
        assert_eq!(resolve(&keyframes, 0, 1, "b"), Some(2));
        // A `:host` rule of scope 2 styling an element of scope 1.
        assert_eq!(resolve(&keyframes, 0x200, 1, "a"), Some(4));
        assert_eq!(resolve(&keyframes, 0x200, 1, "c"), None);
    }

    #[test]
    fn a_departed_scope_gives_up_its_row_but_not_a_reused_identity() {
        let mut keyframes = AnimationKeyframes::default();
        publish(&mut keyframes, 1, 0x100, &[("a", 3)]);
        // Another shadow root allocated at the same address publishes before the first one's row
        // is given up.
        publish(&mut keyframes, 2, 0x100, &[("a", 4)]);
        publish(&mut keyframes, 1, 0x100, &[]);
        assert_eq!(keyframes.scopes.len(), 1);
        assert_eq!(resolve(&keyframes, 0x100, 0, "a"), Some(4));

        publish(&mut keyframes, 2, 0x100, &[]);
        assert!(keyframes.scopes.is_empty());
        assert!(keyframes.scope_by_shadow_root.is_empty());
    }
}
