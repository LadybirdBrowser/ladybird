/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Elements as Rust reaches them: nodes whose cells embed an element core, which Rust reads in place.

use super::element_core::Attribute;
use super::element_core::ELEMENT_CORE_OFFSET;
use super::element_core::ElementCore;
use super::node::Node;
use super::node::NodeCell;
use super::strings::FlyStringField;

/// An element of the DOM, borrowed on the main thread for `'a`, during which the DOM does not change. Made from a node
/// handle that is an element.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Element<'a> {
    node: Node<'a>,
}

impl<'a> Node<'a> {
    /// The node as an element, if it is one.
    #[inline(always)]
    pub fn as_element(self) -> Option<Element<'a>> {
        self.is_element().then_some(Element { node: self })
    }
}

impl<'a> Element<'a> {
    #[inline(always)]
    pub fn node(self) -> Node<'a> {
        self.node
    }

    #[inline(always)]
    pub fn core(self) -> &'a ElementCore {
        // SAFETY: Every element embeds its core ELEMENT_CORE_OFFSET bytes into its cell, which Element.cpp asserts. An
        //         element's cell starts with its node, which is where the handle points.
        unsafe {
            &*(self.node.cell() as *const NodeCell)
                .byte_add(ELEMENT_CORE_OFFSET)
                .cast::<ElementCore>()
        }
    }

    /// https://dom.spec.whatwg.org/#concept-element-local-name
    #[inline(always)]
    pub fn local_name(self) -> &'a FlyStringField {
        self.core().qualified_name().local_name()
    }

    /// The namespace, or none for the null namespace.
    /// https://dom.spec.whatwg.org/#concept-element-namespace
    #[inline(always)]
    pub fn namespace(self) -> Option<&'a FlyStringField> {
        self.core().qualified_name().namespace()
    }
    /// https://dom.spec.whatwg.org/#concept-id
    #[inline(always)]
    pub fn id(self) -> Option<&'a FlyStringField> {
        self.core().id()
    }

    /// https://dom.spec.whatwg.org/#concept-class
    #[inline(always)]
    pub fn classes(self) -> &'a [FlyStringField] {
        self.core().classes()
    }

    /// https://dom.spec.whatwg.org/#concept-element-attribute
    #[inline(always)]
    pub fn attributes(self) -> &'a [Attribute] {
        self.core().attributes()
    }

    /// The filter of the attribute local names and classes of the element and its descendants, see ElementCore.
    #[inline(always)]
    pub fn subtree_name_filter(self) -> u64 {
        self.core().subtree_name_filter()
    }
}

#[cfg(test)]
mod tests {
    use super::super::node_core::NodeType;
    use super::super::test_tree;
    use super::*;
    use crate::dom::qualified_name::QualifiedNameField;
    use crate::stage::MainThread;

    #[test]
    fn only_an_element_node_is_an_element() {
        let main_thread = MainThread::for_test();
        let element = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        assert_eq!(element.as_element().map(Element::node), Some(element));
        for node_type in [
            NodeType::TEXT_NODE,
            NodeType::COMMENT_NODE,
            NodeType::DOCUMENT_NODE,
            NodeType::DOCUMENT_FRAGMENT_NODE,
        ] {
            assert_eq!(test_tree::node(&main_thread, node_type).as_element(), None);
        }
    }

    #[test]
    fn an_element_reads_its_name_from_its_core() {
        let main_thread = MainThread::for_test();
        let svg = test_tree::element(
            &main_thread,
            ElementCore::for_test(QualifiedNameField::for_test("svg", Some("http://www.w3.org/2000/svg"))),
        );
        assert_eq!(
            svg.local_name().raw_identity(),
            FlyStringField::for_test("svg").raw_identity()
        );
        assert_eq!(
            svg.namespace().map(FlyStringField::raw_identity),
            Some(FlyStringField::for_test("http://www.w3.org/2000/svg").raw_identity())
        );
        let div = test_tree::node(&main_thread, NodeType::ELEMENT_NODE)
            .as_element()
            .unwrap();
        assert_eq!(
            div.local_name().raw_identity(),
            FlyStringField::for_test("div").raw_identity()
        );
        assert!(div.namespace().is_none());
    }

    #[test]
    fn an_element_reads_its_id_and_classes_from_its_core() {
        let main_thread = MainThread::for_test();
        let element = test_tree::element(
            &main_thread,
            ElementCore::for_test(QualifiedNameField::for_test("p", None))
                .with_id_for_test("intro")
                .with_classes_for_test(&["lead", "wide"]),
        );
        assert_eq!(
            element.id().map(FlyStringField::raw_identity),
            Some(FlyStringField::for_test("intro").raw_identity())
        );
        let classes: Vec<usize> = element.classes().iter().map(FlyStringField::raw_identity).collect();
        assert_eq!(
            classes,
            [
                FlyStringField::for_test("lead").raw_identity(),
                FlyStringField::for_test("wide").raw_identity()
            ]
        );
        let div = test_tree::node(&main_thread, NodeType::ELEMENT_NODE)
            .as_element()
            .unwrap();
        assert!(div.id().is_none());
        assert!(div.classes().is_empty());
    }
}
