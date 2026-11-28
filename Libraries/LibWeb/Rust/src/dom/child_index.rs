/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The child indices of elements, which the child-indexed pseudo-classes such as `:nth-child()` and `:nth-of-type()`
//! count with, remembered in the cores.
//!
//! An element remembers its 1-based index among its element siblings, and among those of its type, once it is counted,
//! together with the generation of its parent's child list it was counted in. A new generation of a child list begins
//! whenever an element is inserted before another child, or an element leaves the list. The indices an element
//! remembers hold while the generation it counted them in is its parent's, so an index is found by walking back to the
//! nearest sibling that remembers its own, and numbering the siblings from there. Counting every child of a list in
//! turn so costs one step each, and appending elements leaves every index already counted alone.
//!
//! Within one generation, elements can only have been appended to a list, so the generation and the last element child
//! together tell whether the element children of a node changed at all, which the live collections of a node's
//! children check, see collection.rs. NB: Removing the last element child would leave every index counted alone, but
//!     it begins a new generation as well, so that removing and appending elements cannot bring back a last element
//!     child of an earlier list of children within one generation.

use core::cell::Cell;

use super::element::Element;
use super::element_core::ElementCore;
use super::node::Node;
use super::node_ref::NodeRef;

/// Which of an element's siblings its child index counts.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ChildIndexAmong {
    /// Its inclusive element siblings.
    Siblings,
    /// Its inclusive element siblings with its local name and namespace.
    SiblingsOfType,
}

/// Whether `element` and `other` have the same local name and namespace.
#[inline(always)]
fn has_same_type(element: Element<'_>, other: Element<'_>) -> bool {
    element.local_name().raw_identity() == other.local_name().raw_identity()
        && element.namespace().map(|namespace| namespace.raw_identity())
            == other.namespace().map(|namespace| namespace.raw_identity())
}

#[inline(always)]
fn previous_element_sibling(element: Element<'_>) -> Option<Element<'_>> {
    core::iter::successors(element.node().previous_sibling(), |node| node.previous_sibling()).find_map(Node::as_element)
}

#[inline(always)]
fn next_element_sibling(element: Element<'_>) -> Option<Element<'_>> {
    core::iter::successors(element.node().next_sibling(), |node| node.next_sibling()).find_map(Node::as_element)
}

/// The index `among` that `sibling` remembers for `generation` of its parent's child list, after it forgets the ones it
/// counted in another generation.
#[inline(always)]
fn remembered_index(sibling: Element<'_>, generation: u32, among: ChildIndexAmong) -> &Cell<u32> {
    let core = sibling.core();
    if core.parent_child_index_generation().get() != generation {
        core.parent_child_index_generation().set(generation);
        core.child_index().set(0);
        core.child_index_of_type().set(0);
    }
    match among {
        ChildIndexAmong::Siblings => core.child_index(),
        ChildIndexAmong::SiblingsOfType => core.child_index_of_type(),
    }
}

/// The 1-based index of `element` among its inclusive element siblings, as `among` counts them, which is 1 for an
/// element without a parent.
pub fn child_index(element: Element<'_>, among: ChildIndexAmong) -> u32 {
    let Some(parent) = element.node().parent() else {
        return 1;
    };
    let generation = parent.core().child_index_generation();
    let is_counted = |sibling: Element<'_>| among == ChildIndexAmong::Siblings || has_same_type(sibling, element);

    // Walk back to the nearest counted sibling that remembers its index...
    let mut index = 0;
    let mut first_to_number = None;
    let mut candidate = Some(element);
    while let Some(sibling) = candidate {
        if is_counted(sibling) {
            let known = remembered_index(sibling, generation, among).get();
            if known != 0 {
                if sibling == element {
                    return known;
                }
                index = known;
                first_to_number = next_element_sibling(sibling);
                break;
            }
        }
        candidate = previous_element_sibling(sibling);
    }

    // ...and number the counted siblings from there through this element.
    let mut sibling = first_to_number.or_else(|| parent.children().find_map(Node::as_element));
    while let Some(current) = sibling {
        if is_counted(current) {
            index += 1;
            remembered_index(current, generation, among).set(index);
            if current == element {
                return index;
            }
        }
        sibling = next_element_sibling(current);
    }
    unreachable!("an element is among the children of its parent")
}

/// The 1-based index of `element` among its inclusive element siblings, as `among` counts them, counting from the last
/// one, which is 1 for an element without a parent.
pub fn child_index_from_end(element: Element<'_>, among: ChildIndexAmong) -> u32 {
    let Some(parent) = element.node().parent() else {
        return 1;
    };
    let mut last = core::iter::successors(parent.last_child(), |node| node.previous_sibling())
        .find_map(Node::as_element)
        .expect("an element is among the children of its parent");
    // NB: The walk back to the last sibling of this type is not remembered, so it costs the number of siblings of other
    //     types that follow it on every call.
    if among == ChildIndexAmong::SiblingsOfType {
        while !has_same_type(last, element) {
            last = previous_element_sibling(last).expect("an element is among its siblings of its type");
        }
    }
    child_index(last, among) - child_index(element, among) + 1
}

/// Makes `element` forget the child indices it remembers, as it leaves its parent's children.
#[inline(always)]
fn forget_child_indices(element: &ElementCore) {
    element.parent_child_index_generation().set(0);
}

/// Begins a new generation of `parent`'s child list, in which no element child remembers a child index yet.
#[inline]
fn begin_child_index_generation(parent: NodeRef) {
    let generation = parent.core().child_index_generation().wrapping_add(1);
    if generation != 0 {
        parent.core().set_child_index_generation(generation);
        return;
    }
    // The generations have wrapped around, so no child may keep indices counted in the one that comes around again.
    parent.core().set_child_index_generation(1);
    let mut child = parent.first_child();
    while let Some(current) = child {
        if let Some(core) = current.element_core() {
            forget_child_indices(core);
        }
        child = current.next_sibling();
    }
}

/// Keeps the child indices of `parent`'s element children for `node`, which has just joined them before `child`, or
/// after them where `child` is null.
#[inline(always)]
pub(super) fn child_linked(parent: NodeRef, node: NodeRef, child: Option<NodeRef>) {
    if child.is_some() && node.is_element() {
        begin_child_index_generation(parent);
    }
}

/// Keeps the child indices of `parent`'s element children for `node`, which is about to leave them.
#[inline(always)]
pub(super) fn child_about_to_unlink(parent: NodeRef, node: NodeRef) {
    let Some(core) = node.element_core() else {
        return;
    };
    forget_child_indices(core);
    begin_child_index_generation(parent);
}

/// What tells whether the element children of `parent` changed: the generation of its child list, and its last element
/// child, which are the same as long as no element joined or left its children, see the module.
#[inline(always)]
pub(super) fn element_children_stamp(parent: NodeRef) -> u64 {
    let last_element_child = core::iter::successors(parent.last_child(), |node| node.previous_sibling())
        .find(|node| node.is_element())
        .map_or(0, |node| node.pointer().compressed());
    (u64::from(parent.core().child_index_generation()) << 32) | u64::from(last_element_child)
}

#[cfg(test)]
mod tests {
    use super::super::element_core::ElementCore;
    use super::super::links::insert_child;
    use super::super::links::remove_child;
    use super::super::node_core::NodeType;
    use super::super::qualified_name::QualifiedNameField;
    use super::super::test_tree;
    use super::*;
    use crate::stage::MainThread;

    fn element(main_thread: &MainThread<'_>, local_name: &str) -> NodeRef {
        NodeRef::from(
            test_tree::element(
                main_thread,
                ElementCore::for_test(QualifiedNameField::for_test(local_name, None)),
            )
            .node(),
        )
    }

    fn indices(main_thread: &MainThread<'_>, parent: NodeRef, among: ChildIndexAmong) -> Vec<(u32, u32)> {
        parent
            .read(main_thread)
            .children()
            .filter_map(Node::as_element)
            .map(|element| (child_index(element, among), child_index_from_end(element, among)))
            .collect()
    }

    #[test]
    fn elements_are_counted_among_their_element_siblings() {
        let mut main_thread = MainThread::for_test();
        let parent = element(&main_thread, "div");
        let children = [
            element(&main_thread, "p"),
            NodeRef::from(test_tree::node(&main_thread, NodeType::TEXT_NODE)),
            element(&main_thread, "span"),
            element(&main_thread, "p"),
        ];
        for child in children {
            insert_child(&mut main_thread, parent, child, None);
        }
        assert_eq!(
            indices(&main_thread, parent, ChildIndexAmong::Siblings),
            [(1, 3), (2, 2), (3, 1)]
        );
        assert_eq!(
            indices(&main_thread, parent, ChildIndexAmong::SiblingsOfType),
            [(1, 2), (1, 1), (2, 1)]
        );
        let lone = element(&main_thread, "p").read(&main_thread).as_element().unwrap();
        assert_eq!(child_index(lone, ChildIndexAmong::Siblings), 1);
        assert_eq!(child_index_from_end(lone, ChildIndexAmong::SiblingsOfType), 1);
    }

    #[test]
    fn indices_follow_insertions_and_removals() {
        let mut main_thread = MainThread::for_test();
        let parent = element(&main_thread, "ul");
        let [a, b, c, d] = core::array::from_fn(|_| element(&main_thread, "li"));
        insert_child(&mut main_thread, parent, a, None);
        insert_child(&mut main_thread, parent, c, None);
        assert_eq!(
            indices(&main_thread, parent, ChildIndexAmong::Siblings),
            [(1, 2), (2, 1)]
        );

        insert_child(&mut main_thread, parent, b, Some(c));
        insert_child(&mut main_thread, parent, d, None);
        assert_eq!(
            indices(&main_thread, parent, ChildIndexAmong::Siblings),
            [(1, 4), (2, 3), (3, 2), (4, 1)]
        );

        remove_child(&mut main_thread, parent, a);
        assert_eq!(
            indices(&main_thread, parent, ChildIndexAmong::SiblingsOfType),
            [(1, 3), (2, 2), (3, 1)]
        );

        // NB: The removed element counts among the siblings of its next parent, not with what it remembered.
        insert_child(&mut main_thread, parent, a, None);
        assert_eq!(
            indices(&main_thread, parent, ChildIndexAmong::Siblings),
            [(1, 4), (2, 3), (3, 2), (4, 1)]
        );
    }

    #[test]
    fn a_wrapped_generation_forgets_every_index() {
        let mut main_thread = MainThread::for_test();
        let parent = element(&main_thread, "div");
        let [a, b] = core::array::from_fn(|_| element(&main_thread, "div"));
        insert_child(&mut main_thread, parent, b, None);
        assert_eq!(indices(&main_thread, parent, ChildIndexAmong::Siblings), [(1, 1)]);

        parent.core().set_child_index_generation(u32::MAX);
        insert_child(&mut main_thread, parent, a, Some(b));
        assert_eq!(parent.core().child_index_generation(), 1);
        assert_eq!(
            indices(&main_thread, parent, ChildIndexAmong::Siblings),
            [(1, 2), (2, 1)]
        );
    }
}
