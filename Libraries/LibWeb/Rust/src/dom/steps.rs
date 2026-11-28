/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Which nodes the insertion steps, the removing steps and the moving steps have nothing to do for, read from the cores,
//! so that the DOM algorithms in Rust do not call into C++ to run them.
//!
//! Most nodes that join, leave or move within a tree that is not connected are text, comments and elements of classes
//! whose steps only keep state that is about connected trees: the style engine's records, the document's registries,
//! focus, fullscreen, animations, custom element reactions. What is left for them is inherited state, which a node
//! takes from its parent wherever its tree is: whether it is editable, inert, or inside a blocking wheel event handler.
//! When none of that changes either, the steps are skipped. The editable flag, which the cores keep, is written here
//! instead.
//!
//! The steps are those of DOM::Node, DOM::Element and HTML::HTMLElement. An element whose class adds steps of its own,
//! or that is a body or frameset element, whose moving steps tell the page, has ELEMENT_HAS_OWN_TREE_STEPS, which
//! GC::CellConstructionHook in DOM/Node.h sets from the class's overrides as the element is constructed, and which also
//! makes sure that character data and doctypes add none. What the steps do for a node that is not connected, and why
//! that is nothing under the conditions below:
//!
//! - Node::inserted() sets the node's connected flag from its parent's, which is false, and it is already false. What
//!   it records for the style engine and the page, and the paint facts it publishes, are for a connected node or a node
//!   with an identity in the style engine, which is only ever given to a connected node. The editable flag is
//!   inherited from the parent unless the element has a contenteditable state of its own, and the blocking wheel event
//!   handler state is only worked out in a document that may have such a handler.
//! - Node::removed_from() clears the connected flag, which is clear, and the editable flag, and does the rest for a
//!   connected parent, a shadow root, or the focused area of the document, which is connected.
//! - Element::inserted() does its work for a connected element, besides settling the counter style scope of a shadow
//!   host. Element::removed_from() does its work for an element whose old root is connected, besides taking an element
//!   with an ID out of the shadow root it left, settling the counter style scope of a shadow host, and clearing the
//!   styles of the element and its synthetic pseudo-elements. An element only takes a style while it has an identity in
//!   the style engine, and drops it in the removing steps that run as it disconnects. Fullscreen elements and the
//!   animations it plays or cancels are connected.
//! - HTMLElement::inserted() and HTMLElement::removed_from() also make the subtree inert, or no longer inert, if the
//!   nearest HTML element of the parent is inert, which the cores tell. A popover that shows is connected.
//! - Node::moved_from() recomputes the editable flag and the blocking wheel event handler state as Node::inserted()
//!   does, and publishes the paint facts of a node with an identity in the style engine. Element::moved_from() does
//!   its work for a connected element, a shadow host, and the element a move starts from, whose subtree's
//!   form-associated elements it repositions in their forms. HTMLElement::moved_from() makes an element inert, or no
//!   longer inert, where it is not inert and the nearest HTML element of its new parent is, or the other way around.
//! - The callbacks that run the steps also enqueue custom element reactions for connected elements, and cancel the
//!   animations of elements the style engine tracked, which are connected.

use super::element_core::ELEMENT_HAS_CONTENT_EDITABLE_STATE;
use super::element_core::ELEMENT_HAS_OWN_TREE_STEPS;
use super::element_core::ELEMENT_IS_HTML_ELEMENT;
use super::element_core::ELEMENT_IS_INERT;
use super::element_core::ELEMENT_MAY_HAVE_SYNTHETIC_PSEUDO_ELEMENTS;
use super::hooks::IsSubtreeRoot;
use super::hooks::RemovalFacts;
use super::node_core::NodeType;
use super::node_ref::NodeRef;

/// Whether the node's class has no insertion or removing steps beyond those of every node, which is so for the nodes
/// that are no element, document, DocumentFragment or Attr.
#[inline(always)]
fn is_character_data_or_doctype(node: NodeRef) -> bool {
    matches!(
        node.node_type(),
        NodeType::TEXT_NODE
            | NodeType::CDATA_SECTION_NODE
            | NodeType::PROCESSING_INSTRUCTION_NODE
            | NodeType::COMMENT_NODE
            | NodeType::DOCUMENT_TYPE_NODE
    )
}

/// Whether the nearest inclusive ancestor of `node` that is an HTML element, looking past a shadow root to its host, is
/// inert, as nearest_html_element_is_inert() in HTMLElement.cpp works it out.
fn nearest_html_element_is_inert(node: NodeRef) -> bool {
    let mut current = Some(node);
    while let Some(node) = current {
        if let Some(core) = node.element_core()
            && core.flags() & ELEMENT_IS_HTML_ELEMENT != 0
        {
            return core.flags() & ELEMENT_IS_INERT != 0;
        }
        current = match node.is_shadow_root() {
            true => node.host(),
            false => node.parent(),
        };
    }
    false
}

/// Whether the steps that every element runs, and every HTML element, have nothing to do for `element`, which is not
/// connected, with the nearest HTML element of `old_or_new_parent` deciding its inertness, besides what the steps would
/// do to the editable flag.
#[inline(always)]
fn element_steps_do_nothing(element: NodeRef, old_or_new_parent: NodeRef, extra_flags: u8) -> bool {
    let Some(core) = element.element_core() else {
        return false;
    };
    let flags = core.flags();
    if flags & (ELEMENT_HAS_OWN_TREE_STEPS | ELEMENT_HAS_CONTENT_EDITABLE_STATE | extra_flags) != 0
        || core.shadow_root().is_some()
    {
        return false;
    }
    flags & ELEMENT_IS_HTML_ELEMENT == 0 || !nearest_html_element_is_inert(old_or_new_parent)
}

/// Whether the insertion steps with `node`, which is in a subtree that has just joined its parent's children, and the
/// steps of inserting nodes that come with them, have nothing to do. If so, does what they would do to the editable
/// flag, which is to clear it. See the module documentation.
/// https://dom.spec.whatwg.org/#concept-node-insert-ext
pub(super) fn insertion_steps_do_nothing(node: NodeRef) -> bool {
    // NB: A shadow root, which has no parent, takes its connectedness from its host and has steps of its own.
    let Some(parent) = node.parent() else {
        return false;
    };
    if node.is_connected() || parent.is_connected() || parent.core().is_in_editable_subtree() {
        return false;
    }
    if node.core().style_node_id().is_some() || node.document_core().may_have_blocking_wheel_event_listener() {
        return false;
    }
    if !is_character_data_or_doctype(node) && !element_steps_do_nothing(node, parent, 0) {
        return false;
    }
    node.core().set_in_editable_subtree(false);
    true
}

/// Whether the removing steps with `node`, which is in a subtree that has just left `parent`, whose root is
/// `parent_root`, have nothing to do, and the steps of removing a node that come with them, for a subtree the style
/// engine did not track. If so, does what they would do to the editable flag, which is to clear it. See the module
/// documentation.
/// https://dom.spec.whatwg.org/#concept-node-remove-ext
pub(super) fn removing_steps_do_nothing(node: NodeRef, parent: NodeRef, parent_root: NodeRef) -> bool {
    if node.is_connected() || parent.is_connected() || parent_root.is_connected() {
        return false;
    }
    if node.core().style_node_id().is_some() {
        return false;
    }
    if !is_character_data_or_doctype(node)
        && !element_steps_do_nothing(node, parent, ELEMENT_MAY_HAVE_SYNTHETIC_PSEUDO_ELEMENTS)
    {
        return false;
    }
    node.core().set_in_editable_subtree(false);
    true
}

/// Whether the moving steps with `node`, which is in a subtree that has just been moved, and isSubtreeRoot
/// `is_subtree_root`, and the custom element reaction that comes with them, have nothing to do. If so, does what the
/// steps would do to the editable flag, which is to clear it. See the module documentation.
/// https://dom.spec.whatwg.org/#move
pub(super) fn moving_steps_do_nothing(node: NodeRef, is_subtree_root: IsSubtreeRoot) -> bool {
    // NB: A shadow root, which has no parent, takes its editability from its host.
    let Some(parent) = node.parent() else {
        return false;
    };
    if node.is_connected() || parent.is_connected() || parent.core().is_in_editable_subtree() {
        return false;
    }
    if node.core().style_node_id().is_some() || node.document_core().may_have_blocking_wheel_event_listener() {
        return false;
    }
    if !is_character_data_or_doctype(node) {
        // NB: The moving steps of the element a move starts from reposition the form-associated elements of its
        //     subtree in the lists of their forms.
        if is_subtree_root == IsSubtreeRoot::Yes {
            return false;
        }
        // NB: An inert HTML element stops being inert when its new parent is not.
        if !element_steps_do_nothing(node, parent, ELEMENT_IS_INERT) {
            return false;
        }
    }
    node.core().set_in_editable_subtree(false);
    true
}

/// Whether `node` or a shadow-including descendant of it has an identity in the style engine.
#[inline]
fn shadow_including_subtree_has_style_node(node: NodeRef) -> bool {
    if node.core().style_node_id().is_some() {
        return true;
    }
    if node.shadow_root().is_some_and(shadow_including_subtree_has_style_node) {
        return true;
    }
    let mut child = node.first_child();
    while let Some(current) = child {
        if shadow_including_subtree_has_style_node(current) {
            return true;
        }
        child = current.next_sibling();
    }
    false
}

/// Whether the steps of removing `node`, which has just left its parent, that hooks::after_removal_unlink() runs with
/// `facts` have nothing to do:
///
/// - Only the boxes of nodes that have an identity in the style engine have style records to pin, which is done for a
///   subtree that was not connected.
/// - Only a subtree that holds the focused or hovered node takes their state out of its old ancestors.
pub(super) fn after_removal_unlink_does_nothing(node: NodeRef, facts: RemovalFacts) -> bool {
    if facts.holds_a_propagating_state_source {
        return false;
    }
    facts.was_connected || !shadow_including_subtree_has_style_node(node)
}

#[cfg(test)]
mod tests {
    use super::super::element_core::ElementCore;
    use super::super::node::Node;
    use super::super::qualified_name::QualifiedNameField;
    use super::super::test_tree::append_child;
    use super::super::test_tree::element;
    use super::super::test_tree::node;
    use super::super::test_tree::set_style_node_id;
    use super::super::test_tree::shadow_root;
    use super::*;
    use crate::css::style::tree::StyleNodeID;
    use crate::stage::MainThread;

    fn element_with_flags<'a>(main_thread: &'a MainThread<'_>, flags: u8) -> Node<'a> {
        element(
            main_thread,
            ElementCore::for_test(QualifiedNameField::for_test("div", None)).with_flags_for_test(flags),
        )
        .node()
    }

    fn html_element<'a>(main_thread: &'a MainThread<'_>) -> Node<'a> {
        element_with_flags(main_thread, ELEMENT_IS_HTML_ELEMENT)
    }

    /// `child`, appended to `parent`, as the insertion steps see it.
    fn appended(parent: Node<'_>, child: Node<'_>) -> NodeRef {
        append_child(parent, child);
        NodeRef::from(child)
    }

    #[test]
    fn text_and_plain_elements_joining_a_disconnected_tree_have_nothing_to_do() {
        let main_thread = MainThread::for_test();
        let parent = html_element(&main_thread);
        for child in [
            node(&main_thread, NodeType::TEXT_NODE),
            node(&main_thread, NodeType::COMMENT_NODE),
            html_element(&main_thread),
            node(&main_thread, NodeType::ELEMENT_NODE),
        ] {
            let child = appended(parent, child);
            child.core().set_in_editable_subtree(true);
            assert!(insertion_steps_do_nothing(child));
            assert!(!child.core().is_in_editable_subtree());
        }
    }

    #[test]
    fn a_connected_tree_runs_the_insertion_steps() {
        let main_thread = MainThread::for_test();
        let parent = html_element(&main_thread);
        parent.core().set_is_connected_for_test();
        let child = appended(parent, node(&main_thread, NodeType::TEXT_NODE));
        assert!(!insertion_steps_do_nothing(child));
    }

    #[test]
    fn inherited_state_runs_the_insertion_steps() {
        let main_thread = MainThread::for_test();

        let editable = html_element(&main_thread);
        NodeRef::from(editable).core().set_in_editable_subtree(true);
        assert!(!insertion_steps_do_nothing(appended(
            editable,
            node(&main_thread, NodeType::TEXT_NODE)
        )));

        let inert = element_with_flags(&main_thread, ELEMENT_IS_HTML_ELEMENT | ELEMENT_IS_INERT);
        let other = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(inert, other);
        assert!(!insertion_steps_do_nothing(appended(other, html_element(&main_thread))));
        assert!(insertion_steps_do_nothing(appended(
            other,
            node(&main_thread, NodeType::ELEMENT_NODE)
        )));
        assert!(insertion_steps_do_nothing(appended(
            inert,
            node(&main_thread, NodeType::TEXT_NODE)
        )));

        let text = node(&main_thread, NodeType::TEXT_NODE);
        NodeRef::from(text)
            .document_core()
            .set_may_have_blocking_wheel_event_listener_for_test();
        assert!(!insertion_steps_do_nothing(appended(html_element(&main_thread), text)));
    }

    #[test]
    fn elements_with_steps_of_their_own_run_the_insertion_steps() {
        let main_thread = MainThread::for_test();
        let parent = html_element(&main_thread);
        for flags in [ELEMENT_HAS_OWN_TREE_STEPS, ELEMENT_HAS_CONTENT_EDITABLE_STATE] {
            let child = appended(
                parent,
                element_with_flags(&main_thread, ELEMENT_IS_HTML_ELEMENT | flags),
            );
            assert!(!insertion_steps_do_nothing(child));
        }

        let host = html_element(&main_thread);
        append_child(parent, host);
        let shadow_root = shadow_root(&main_thread, host);
        assert!(!insertion_steps_do_nothing(NodeRef::from(host)));
        assert!(!insertion_steps_do_nothing(NodeRef::from(shadow_root)));

        let identified = node(&main_thread, NodeType::TEXT_NODE);
        set_style_node_id(identified, StyleNodeID::from_raw(1));
        assert!(!insertion_steps_do_nothing(appended(parent, identified)));
    }

    #[test]
    fn nodes_leaving_a_disconnected_tree_have_nothing_to_do() {
        let main_thread = MainThread::for_test();
        let parent = NodeRef::from(html_element(&main_thread));
        let text = NodeRef::from(node(&main_thread, NodeType::TEXT_NODE));
        text.core().set_in_editable_subtree(true);
        assert!(removing_steps_do_nothing(text, parent, parent));
        assert!(!text.core().is_in_editable_subtree());

        let element = NodeRef::from(html_element(&main_thread));
        assert!(removing_steps_do_nothing(element, parent, parent));
        let with_pseudo_elements = NodeRef::from(element_with_flags(
            &main_thread,
            ELEMENT_IS_HTML_ELEMENT | ELEMENT_MAY_HAVE_SYNTHETIC_PSEUDO_ELEMENTS,
        ));
        assert!(!removing_steps_do_nothing(with_pseudo_elements, parent, parent));

        parent.core().set_is_connected_for_test();
        assert!(!removing_steps_do_nothing(text, parent, parent));
    }

    #[test]
    fn an_element_with_an_id_leaving_a_disconnected_shadow_tree_has_nothing_to_do() {
        let main_thread = MainThread::for_test();
        let host = html_element(&main_thread);
        let shadow_root = NodeRef::from(shadow_root(&main_thread, host));
        let parent = NodeRef::from(html_element(&main_thread));
        let with_id = |id: Option<&str>| {
            let core = ElementCore::for_test(QualifiedNameField::for_test("div", None));
            let core = match id {
                Some(id) => core.with_id_for_test(id),
                None => core,
            };
            NodeRef::from(element(&main_thread, core).node())
        };
        // NB: Only a connected tree keeps a map of its elements by ID, see id_map.rs.
        assert!(removing_steps_do_nothing(with_id(None), parent, shadow_root));
        assert!(removing_steps_do_nothing(with_id(Some("a")), parent, shadow_root));
        assert!(removing_steps_do_nothing(with_id(Some("a")), parent, parent));
    }

    #[test]
    fn plain_descendants_moving_within_a_disconnected_tree_have_nothing_to_do() {
        let main_thread = MainThread::for_test();
        let parent = html_element(&main_thread);
        let moved_node = html_element(&main_thread);
        let moved = appended(parent, moved_node);
        let descendant = appended(moved_node, html_element(&main_thread));
        let text = appended(parent, node(&main_thread, NodeType::TEXT_NODE));
        text.core().set_in_editable_subtree(true);

        assert!(moving_steps_do_nothing(text, IsSubtreeRoot::Yes));
        assert!(!text.core().is_in_editable_subtree());
        assert!(moving_steps_do_nothing(descendant, IsSubtreeRoot::No));
        // NB: The element a move starts from repositions the form-associated elements of its subtree.
        assert!(!moving_steps_do_nothing(moved, IsSubtreeRoot::Yes));

        let inert = appended(
            parent,
            element_with_flags(&main_thread, ELEMENT_IS_HTML_ELEMENT | ELEMENT_IS_INERT),
        );
        assert!(!moving_steps_do_nothing(inert, IsSubtreeRoot::No));
        let with_steps = appended(
            parent,
            element_with_flags(&main_thread, ELEMENT_IS_HTML_ELEMENT | ELEMENT_HAS_OWN_TREE_STEPS),
        );
        assert!(!moving_steps_do_nothing(with_steps, IsSubtreeRoot::No));

        NodeRef::from(parent).core().set_is_connected_for_test();
        assert!(!moving_steps_do_nothing(text, IsSubtreeRoot::No));
    }

    #[test]
    fn nothing_follows_the_unlinking_of_a_plain_subtree() {
        let main_thread = MainThread::for_test();
        let facts = RemovalFacts {
            was_connected: false,
            was_tracked_by_style_engine: false,
            holds_a_propagating_state_source: false,
        };
        let subtree = html_element(&main_thread);
        let text = node(&main_thread, NodeType::TEXT_NODE);
        append_child(subtree, text);
        let subtree = NodeRef::from(subtree);
        assert!(after_removal_unlink_does_nothing(subtree, facts));

        let holding = RemovalFacts {
            holds_a_propagating_state_source: true,
            ..facts
        };
        assert!(!after_removal_unlink_does_nothing(subtree, holding));

        // NB: The callback pins style records only for a subtree that was not connected.
        set_style_node_id(text, StyleNodeID::from_raw(1));
        assert!(!after_removal_unlink_does_nothing(subtree, facts));
        let connected = RemovalFacts {
            was_connected: true,
            ..facts
        };
        assert!(after_removal_unlink_does_nothing(subtree, connected));
    }
}
