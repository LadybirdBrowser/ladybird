/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS animations an element owns, mirrored for the style computation.
//!
//! Reconciling an element's `CSSAnimation` objects against its freshly computed `animation-*`
//! longhands needs to know which animations the element already owns. The names of those
//! animations are published here, so the computation can decide the reconciliation from its own
//! inputs.
//!
//! Only the names are mirrored: matching an animation is matching its name, and everything the
//! host does once a definition has found its animation - applying the timing, resolving the
//! keyframes, cancelling what no definition claimed - it does from the list it already holds.

use super::tree::StyleNodeID;
use crate::css::css_string::CssString;
use std::collections::HashMap;

/// Which of an element's animation lists a row belongs to, in the host's own numbering: zero for
/// the element itself, and the pseudo-element's value plus one for each pseudo-element.
pub(crate) type AnimationSlot = u8;

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

    /// Give up the lists of an identity that retires. An identity can be minted again for another
    /// element, so a list left behind would be read as that element's.
    pub(crate) fn retire(&mut self, node: StyleNodeID) {
        self.rows.remove(&node);
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

    fn list(animations: &CssDefinedAnimations, node: StyleNodeID, slot: AnimationSlot) -> Option<&[CssString]> {
        animations
            .rows
            .get(&node)?
            .iter()
            .find(|(list_slot, _)| *list_slot == slot)
            .map(|(_, names)| &names[..])
    }

    #[test]
    fn a_list_is_replaced_per_slot_and_dropped_when_empty() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut animations = CssDefinedAnimations::default();
        animations.set(node, 0, names(&["a", "b"]));
        animations.set(node, 3, names(&["c"]));
        animations.set(node, 0, names(&["b"]));
        assert_eq!(list(&animations, node, 0), Some(&names(&["b"])[..]));
        assert_eq!(list(&animations, node, 3), Some(&names(&["c"])[..]));

        animations.set(node, 0, names(&[]));
        assert_eq!(list(&animations, node, 0), None);
        animations.set(node, 3, names(&[]));
        assert!(animations.rows.is_empty());
    }

    #[test]
    fn a_retired_identity_holds_no_lists_when_reissued() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut animations = CssDefinedAnimations::default();
        animations.set(node, 0, names(&["a"]));
        animations.set(node, 2, names(&["b"]));
        animations.retire(node);
        assert_eq!(list(&animations, node, 0), None);
        assert_eq!(list(&animations, node, 2), None);
        assert!(animations.rows.is_empty());
    }
}
