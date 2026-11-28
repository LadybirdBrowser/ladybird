/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The validity checks that inserting a node into a parent makes first.
//! https://dom.spec.whatwg.org/#concept-node-ensure-pre-insertion-validity

use super::node::Node;
use super::node::NodeCell;
use super::node_core::NodeType;

crate::stage::main_thread_ffi_entries!(without_host);

/// The nodes among the parent's children that the checks leave out, as the spec's childrenToExclude list names them.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChildrenToExclude {
    /// « », as pre-insert passes it.
    Nothing,
    /// « child », as replace passes it.
    Child,
    /// The parent's children, as replaceChildren() passes them.
    ParentsChildren,
}

/// Whether a node can be inserted, or else which DOMException to throw and why.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreInsertValidity {
    Valid,
    /// A "HierarchyRequestError" DOMException: the parent is not a Document, DocumentFragment, or Element node.
    ParentCannotHaveChildren,
    /// A "HierarchyRequestError" DOMException: the node is a host-including inclusive ancestor of the parent.
    NodeIsAnAncestorOfParent,
    /// A "NotFoundError" DOMException: the child is not a child of the parent.
    ChildIsNotAChildOfParent,
    /// A "HierarchyRequestError" DOMException: the parent cannot hold the node, of its type, there.
    InvalidNodeType,
}

/// https://dom.spec.whatwg.org/#concept-node-ensure-pre-insertion-validity
pub fn ensure_pre_insert_validity<'a>(
    node: Node<'a>,
    parent: Node<'a>,
    child: Option<Node<'a>>,
    children_to_exclude: ChildrenToExclude,
) -> PreInsertValidity {
    use PreInsertValidity::*;

    let children_to_exclude_contains = |candidate: Node<'a>| match children_to_exclude {
        ChildrenToExclude::Nothing => false,
        ChildrenToExclude::Child => Some(candidate) == child,
        ChildrenToExclude::ParentsChildren => candidate.parent() == Some(parent),
    };
    let parent_has_child_not_excluded = |node_type: NodeType| {
        parent
            .children()
            .any(|child| child.node_type() == node_type && !children_to_exclude_contains(child))
    };

    // To ensure pre-insert validity given a node node, a node parent, null or a node child, and a list of nodes
    // childrenToExclude:

    // 1. If parent is not a Document, DocumentFragment, or Element node, then throw a "HierarchyRequestError"
    //    DOMException.
    if !matches!(
        parent.node_type(),
        NodeType::DOCUMENT_NODE | NodeType::DOCUMENT_FRAGMENT_NODE | NodeType::ELEMENT_NODE
    ) {
        return ParentCannotHaveChildren;
    }

    // 2. If node is a host-including inclusive ancestor of parent, then throw a "HierarchyRequestError" DOMException.
    if node.is_host_including_inclusive_ancestor_of(parent) {
        return NodeIsAnAncestorOfParent;
    }

    // 3. If child is non-null and its parent is not parent, then throw a "NotFoundError" DOMException.
    if let Some(child) = child
        && child.parent() != Some(parent)
    {
        return ChildIsNotAChildOfParent;
    }

    // 4. If node is not a DocumentFragment, DocumentType, Element, or CharacterData node, then throw a
    //    "HierarchyRequestError" DOMException.
    if !matches!(
        node.node_type(),
        NodeType::DOCUMENT_FRAGMENT_NODE | NodeType::DOCUMENT_TYPE_NODE | NodeType::ELEMENT_NODE
    ) && !is_character_data(node)
    {
        return InvalidNodeType;
    }

    // 5. If parent is not a document:
    if parent.node_type() != NodeType::DOCUMENT_NODE {
        // 1. If node is a doctype, then throw a "HierarchyRequestError" DOMException.
        if node.node_type() == NodeType::DOCUMENT_TYPE_NODE {
            return InvalidNodeType;
        }

        // 2. Return.
        return Valid;
    }

    // 6. If node is a Text node, then throw a "HierarchyRequestError" DOMException.
    if is_text(node) {
        return InvalidNodeType;
    }

    // 7. If node is a CharacterData node, then return.
    if is_character_data(node) {
        return Valid;
    }

    // 8. If node is a DocumentFragment node:
    if node.node_type() == NodeType::DOCUMENT_FRAGMENT_NODE {
        // 1. If node has more than one element child or has a Text node child, then throw a "HierarchyRequestError"
        //    DOMException.
        let element_child_count = node.children().filter(|child| child.is_element()).take(2).count();
        if element_child_count > 1 || node.children().any(is_text) {
            return InvalidNodeType;
        }

        // 2. If node has no element child, then return.
        if element_child_count == 0 {
            return Valid;
        }
    }

    // 9. If node is a DocumentFragment or Element node:
    if matches!(
        node.node_type(),
        NodeType::DOCUMENT_FRAGMENT_NODE | NodeType::ELEMENT_NODE
    ) {
        // 1. If any of the following are true:
        //    * parent has an element child that childrenToExclude does not contain;
        //    * child is non-null and a doctype is following child; or
        //    * child is a doctype that childrenToExclude does not contain,
        //    then throw a "HierarchyRequestError" DOMException.
        if parent_has_child_not_excluded(NodeType::ELEMENT_NODE)
            || child.is_some_and(|child| child.following().any(is_doctype))
            || child.is_some_and(|child| is_doctype(child) && !children_to_exclude_contains(child))
        {
            return InvalidNodeType;
        }

        // 2. Return.
        return Valid;
    }

    // 10. Assert: node is a doctype.
    assert!(is_doctype(node));

    // 11. If any of the following are true:
    //     * parent has a doctype child that childrenToExclude does not contain;
    //     * child is non-null and an element is preceding child; or
    //     * child is null and parent has an element child that childrenToExclude does not contain,
    //     then throw a "HierarchyRequestError" DOMException.
    if parent_has_child_not_excluded(NodeType::DOCUMENT_TYPE_NODE)
        || child.is_some_and(|child| child.preceding().any(|node| node.is_element()))
        || (child.is_none() && parent_has_child_not_excluded(NodeType::ELEMENT_NODE))
    {
        return InvalidNodeType;
    }

    Valid
}

/// https://dom.spec.whatwg.org/#concept-doctype
fn is_doctype(node: Node<'_>) -> bool {
    node.node_type() == NodeType::DOCUMENT_TYPE_NODE
}

/// Whether the node is a Text node, which a CDATASection node is too.
/// https://dom.spec.whatwg.org/#text
fn is_text(node: Node<'_>) -> bool {
    matches!(node.node_type(), NodeType::TEXT_NODE | NodeType::CDATA_SECTION_NODE)
}

/// https://dom.spec.whatwg.org/#characterdata
fn is_character_data(node: Node<'_>) -> bool {
    matches!(
        node.node_type(),
        NodeType::TEXT_NODE
            | NodeType::CDATA_SECTION_NODE
            | NodeType::PROCESSING_INSTRUCTION_NODE
            | NodeType::COMMENT_NODE
    )
}

/// Ensures the pre-insert validity of `node` into `parent` before `child`, for DOM::Node.
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_ensure_pre_insert_validity(
    node: &NodeCell,
    parent: &NodeCell,
    child: Option<&NodeCell>,
    children_to_exclude: ChildrenToExclude,
) -> PreInsertValidity {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live, and nothing changes the DOM while the checks read it.
    let from_cell = |cell| unsafe { Node::from_cell(cell, &main_thread) };
    ensure_pre_insert_validity(
        from_cell(node),
        from_cell(parent),
        child.map(from_cell),
        children_to_exclude,
    )
}

#[cfg(test)]
mod tests {
    use super::super::test_tree::append_child;
    use super::super::test_tree::node;
    use super::super::test_tree::set_host;
    use super::super::test_tree::shadow_root;
    use super::ChildrenToExclude::*;
    use super::PreInsertValidity::*;
    use super::*;
    use crate::stage::MainThread;

    #[test]
    fn only_a_document_fragment_or_element_can_be_a_parent() {
        let main_thread = MainThread::for_test();
        let text = node(&main_thread, NodeType::TEXT_NODE);
        let element = node(&main_thread, NodeType::ELEMENT_NODE);
        assert_eq!(
            ensure_pre_insert_validity(element, text, None, Nothing),
            ParentCannotHaveChildren
        );
        assert_eq!(ensure_pre_insert_validity(text, element, None, Nothing), Valid);
    }

    #[test]
    fn a_node_cannot_be_inserted_into_its_inclusive_descendants() {
        let main_thread = MainThread::for_test();
        let outer = node(&main_thread, NodeType::ELEMENT_NODE);
        let inner = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(outer, inner);
        assert_eq!(
            ensure_pre_insert_validity(outer, inner, None, Nothing),
            NodeIsAnAncestorOfParent
        );
        assert_eq!(
            ensure_pre_insert_validity(outer, outer, None, Nothing),
            NodeIsAnAncestorOfParent
        );
        assert_eq!(ensure_pre_insert_validity(inner, outer, None, Nothing), Valid);
    }

    #[test]
    fn a_node_cannot_be_inserted_into_its_hosts_shadow_trees_or_template_contents() {
        let main_thread = MainThread::for_test();
        let outer = node(&main_thread, NodeType::ELEMENT_NODE);
        let host = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(outer, host);
        let shadow_root = shadow_root(&main_thread, host);
        let in_shadow_tree = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(shadow_root, in_shadow_tree);
        assert_eq!(
            ensure_pre_insert_validity(outer, in_shadow_tree, None, Nothing),
            NodeIsAnAncestorOfParent
        );
        assert_eq!(
            ensure_pre_insert_validity(host, shadow_root, None, Nothing),
            NodeIsAnAncestorOfParent
        );

        let template_contents = node(&main_thread, NodeType::DOCUMENT_FRAGMENT_NODE);
        let template = node(&main_thread, NodeType::ELEMENT_NODE);
        set_host(template_contents, template);
        append_child(in_shadow_tree, template);
        assert_eq!(
            ensure_pre_insert_validity(outer, template_contents, None, Nothing),
            NodeIsAnAncestorOfParent
        );

        let unrelated = node(&main_thread, NodeType::ELEMENT_NODE);
        assert_eq!(
            ensure_pre_insert_validity(unrelated, template_contents, None, Nothing),
            Valid
        );
    }

    #[test]
    fn the_child_must_be_a_child_of_the_parent() {
        let main_thread = MainThread::for_test();
        let parent = node(&main_thread, NodeType::ELEMENT_NODE);
        let child = node(&main_thread, NodeType::ELEMENT_NODE);
        let elsewhere = node(&main_thread, NodeType::ELEMENT_NODE);
        let inserted = node(&main_thread, NodeType::TEXT_NODE);
        append_child(parent, child);
        assert_eq!(
            ensure_pre_insert_validity(inserted, parent, Some(child), Nothing),
            Valid
        );
        assert_eq!(
            ensure_pre_insert_validity(inserted, parent, Some(elsewhere), Nothing),
            ChildIsNotAChildOfParent
        );
    }

    #[test]
    fn documents_and_attributes_are_never_inserted_and_doctypes_only_into_documents() {
        let main_thread = MainThread::for_test();
        let element = node(&main_thread, NodeType::ELEMENT_NODE);
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let attribute = node(&main_thread, NodeType::ATTRIBUTE_NODE);
        let doctype = node(&main_thread, NodeType::DOCUMENT_TYPE_NODE);
        assert_eq!(
            ensure_pre_insert_validity(document, element, None, Nothing),
            InvalidNodeType
        );
        assert_eq!(
            ensure_pre_insert_validity(attribute, element, None, Nothing),
            InvalidNodeType
        );
        assert_eq!(
            ensure_pre_insert_validity(doctype, element, None, Nothing),
            InvalidNodeType
        );
        assert_eq!(ensure_pre_insert_validity(doctype, document, None, Nothing), Valid);
    }

    #[test]
    fn a_document_holds_no_text_and_any_other_character_data() {
        let main_thread = MainThread::for_test();
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        for (node_type, validity) in [
            (NodeType::TEXT_NODE, InvalidNodeType),
            (NodeType::CDATA_SECTION_NODE, InvalidNodeType),
            (NodeType::COMMENT_NODE, Valid),
            (NodeType::PROCESSING_INSTRUCTION_NODE, Valid),
        ] {
            let inserted = node(&main_thread, node_type);
            assert_eq!(ensure_pre_insert_validity(inserted, document, None, Nothing), validity);
        }
    }

    #[test]
    fn a_document_holds_one_element_after_its_doctype() {
        let main_thread = MainThread::for_test();
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let doctype = node(&main_thread, NodeType::DOCUMENT_TYPE_NODE);
        let comment = node(&main_thread, NodeType::COMMENT_NODE);
        append_child(document, comment);
        append_child(document, doctype);
        let element = node(&main_thread, NodeType::ELEMENT_NODE);
        assert_eq!(ensure_pre_insert_validity(element, document, None, Nothing), Valid);
        // NB: The doctype would follow the element.
        assert_eq!(
            ensure_pre_insert_validity(element, document, Some(comment), Nothing),
            InvalidNodeType
        );
        assert_eq!(
            ensure_pre_insert_validity(element, document, Some(doctype), Nothing),
            InvalidNodeType
        );
        // NB: Replacing the doctype with an element leaves no doctype after it.
        assert_eq!(
            ensure_pre_insert_validity(element, document, Some(doctype), Child),
            Valid
        );

        let html = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(document, html);
        assert_eq!(
            ensure_pre_insert_validity(element, document, None, Nothing),
            InvalidNodeType
        );
        assert_eq!(ensure_pre_insert_validity(element, document, Some(html), Child), Valid);
        assert_eq!(
            ensure_pre_insert_validity(element, document, None, ParentsChildren),
            Valid
        );
    }

    #[test]
    fn a_fragment_inserted_into_a_document_brings_at_most_one_element_and_no_text() {
        let main_thread = MainThread::for_test();
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let fragment = |children: &[NodeType]| {
            let fragment = node(&main_thread, NodeType::DOCUMENT_FRAGMENT_NODE);
            for &node_type in children {
                append_child(fragment, node(&main_thread, node_type));
            }
            fragment
        };
        let check = |fragment| ensure_pre_insert_validity(fragment, document, None, Nothing);
        assert_eq!(check(fragment(&[])), Valid);
        assert_eq!(
            check(fragment(&[NodeType::COMMENT_NODE, NodeType::ELEMENT_NODE])),
            Valid
        );
        assert_eq!(
            check(fragment(&[NodeType::ELEMENT_NODE, NodeType::ELEMENT_NODE])),
            InvalidNodeType
        );
        assert_eq!(check(fragment(&[NodeType::TEXT_NODE])), InvalidNodeType);
    }

    #[test]
    fn a_doctype_goes_before_every_element_of_a_document() {
        let main_thread = MainThread::for_test();
        let document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let comment = node(&main_thread, NodeType::COMMENT_NODE);
        let html = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(document, comment);
        append_child(document, html);
        let doctype = node(&main_thread, NodeType::DOCUMENT_TYPE_NODE);
        assert_eq!(
            ensure_pre_insert_validity(doctype, document, Some(comment), Nothing),
            Valid
        );
        assert_eq!(
            ensure_pre_insert_validity(doctype, document, Some(html), Nothing),
            Valid
        );
        assert_eq!(
            ensure_pre_insert_validity(doctype, document, None, Nothing),
            InvalidNodeType
        );
        assert_eq!(
            ensure_pre_insert_validity(doctype, document, None, ParentsChildren),
            Valid
        );

        let body = node(&main_thread, NodeType::ELEMENT_NODE);
        let after_body = node(&main_thread, NodeType::COMMENT_NODE);
        append_child(html, body);
        append_child(document, after_body);
        // NB: The element preceding the child need not be a sibling of it.
        assert_eq!(
            ensure_pre_insert_validity(doctype, document, Some(after_body), Nothing),
            InvalidNodeType
        );

        let other_doctype = node(&main_thread, NodeType::DOCUMENT_TYPE_NODE);
        let second_document = node(&main_thread, NodeType::DOCUMENT_NODE);
        let existing_doctype = node(&main_thread, NodeType::DOCUMENT_TYPE_NODE);
        append_child(second_document, existing_doctype);
        assert_eq!(
            ensure_pre_insert_validity(other_doctype, second_document, Some(existing_doctype), Nothing),
            InvalidNodeType
        );
        assert_eq!(
            ensure_pre_insert_validity(other_doctype, second_document, Some(existing_doctype), Child),
            Valid
        );
    }
}
