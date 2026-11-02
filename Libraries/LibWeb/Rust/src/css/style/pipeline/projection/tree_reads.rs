/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Walks over the projection's links that answer what the host tree answers of an element's place
//! in the shadow-including and flat trees.

use super::super::super::tree::StyleNodeID;
use super::Projection;

impl Projection {
    /// Every live element identity, including the roots of shadow trees, in element order.
    #[cfg(test)]
    pub(in crate::css::style) fn live_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        (1..self.slot_count).filter_map(|index| {
            let node = StyleNodeID::element(u32::try_from(index).expect("style node index space exhausted"));
            self.is_live(node).then_some(node)
        })
    }

    /// The element's parent in the tree it is in, or a shadow root's host. A text node has no row,
    /// so its parent is the host tree's to answer.
    #[must_use]
    pub(in crate::css::style) fn parent_or_shadow_host(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        assert!(node.element_index().is_some(), "only an element has projection links");
        self.parent(node).or_else(|| self.host_of(node))
    }

    /// Whether the element `node` is `root` or lies below it in the shadow-including tree: the climb
    /// out of a shadow tree continues at the host rather than stopping there. A climb that runs out
    /// of parents has reached a child of the document, which no element contains.
    #[must_use]
    pub(in crate::css::style) fn is_in_shadow_including_subtree_of(
        &self,
        node: StyleNodeID,
        root: StyleNodeID,
    ) -> bool {
        assert!(node.element_index().is_some(), "only an element has projection links");
        if root.text_index().is_some() {
            return false;
        }
        let mut candidate = node;
        loop {
            if candidate == root {
                return true;
            }
            candidate = match self.host_of(candidate).or_else(|| self.parent(candidate)) {
                Some(next) => next,
                None => return false,
            };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn element(index: u32) -> StyleNodeID {
        StyleNodeID::element(index)
    }

    /// document(1) -> host(2) -> [light 4, light 5]; host's shadow root(3) -> slot(6) -> fallback 7.
    /// Light 4 and text 1 are assigned to the slot. Element 8 is minted and never linked.
    fn shadow_document() -> Projection {
        let mut projection = Projection::default();
        for index in 1..=8 {
            projection.mint(element(index));
        }
        let link_children = |projection: &mut Projection, parent: u32, children: &[u32]| {
            projection.set_first_element_child(element(parent), children.first().map(|&child| element(child)));
            for (position, &child) in children.iter().enumerate() {
                projection.set_parent(element(child), Some(element(parent)));
                let previous = position.checked_sub(1).map(|previous| element(children[previous]));
                projection.set_previous_element_sibling(element(child), previous);
                projection
                    .set_next_element_sibling(element(child), children.get(position + 1).map(|&next| element(next)));
            }
        };
        link_children(&mut projection, 1, &[2]);
        link_children(&mut projection, 2, &[4, 5]);
        link_children(&mut projection, 3, &[6]);
        link_children(&mut projection, 6, &[7]);
        projection.set_shadow_root(element(2), element(3));
        projection.set_assigned_slot(element(4), Some(element(6)));
        projection.set_assigned_nodes(element(6), &[element(4), StyleNodeID::text(1)]);
        // Style reads an element's links as the last transaction committed them.
        projection.commit();
        projection
    }

    #[test]
    fn live_nodes_are_the_minted_elements_that_did_not_retire() {
        let mut projection = shadow_document();
        assert_eq!(
            projection.live_nodes().collect::<Vec<_>>(),
            (1..=8).map(element).collect::<Vec<_>>()
        );
        projection.retire(element(5));
        projection.retire(element(8));
        assert_eq!(
            projection.live_nodes().collect::<Vec<_>>(),
            [1, 2, 3, 4, 6, 7].map(element)
        );
    }

    #[test]
    fn parent_or_shadow_host_climbs_from_a_shadow_root_to_its_host() {
        let projection = shadow_document();
        assert_eq!(projection.parent_or_shadow_host(element(6)), Some(element(3)));
        assert_eq!(projection.parent_or_shadow_host(element(3)), Some(element(2)));
        // A slotted element continues through its light-tree parent.
        assert_eq!(projection.parent_or_shadow_host(element(4)), Some(element(2)));
        assert_eq!(projection.parent_or_shadow_host(element(1)), None);
        assert_eq!(projection.parent_or_shadow_host(element(8)), None);
    }

    #[test]
    fn shadow_including_subtrees_cross_into_shadow_trees() {
        let projection = shadow_document();
        let is_in = |node, root| projection.is_in_shadow_including_subtree_of(element(node), element(root));
        assert!(is_in(7, 7));
        assert!(is_in(7, 3));
        assert!(is_in(7, 2));
        assert!(is_in(7, 1));
        assert!(is_in(4, 2));
        assert!(!is_in(4, 6));
        assert!(!is_in(2, 3));
        assert!(!is_in(5, 4));
        assert!(!is_in(8, 1));
        assert!(!projection.is_in_shadow_including_subtree_of(element(4), StyleNodeID::text(1)));
    }

    #[test]
    fn flat_tree_parents_follow_shadow_roots_and_slots() {
        let mut projection = shadow_document();
        // The host's flat children are its shadow root's, and the slot's are its assigned nodes,
        // which leave its fallback and the unassigned light child out.
        assert_eq!(projection.flat_tree_parent(element(4)), Some(element(6)));
        assert_eq!(projection.flat_tree_parent(element(5)), None);
        assert_eq!(projection.flat_tree_parent(element(7)), None);

        // With nothing assigned, the slot shows its fallback content.
        projection.set_assigned_slot(element(4), None);
        projection.set_assigned_nodes(element(6), &[]);
        assert_eq!(projection.flat_tree_parent(element(7)), Some(element(6)));
    }
}
