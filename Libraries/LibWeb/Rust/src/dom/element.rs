/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Elements as Rust reaches them: nodes whose cells embed an element core, which Rust reads in place.

use super::element_core::Attribute;
use super::element_core::ElementCore;
use super::node::Node;
use super::prefix::ElementPrefix;
use super::prefix::prefix_of;
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
        // SAFETY: Every element starts with an ElementPrefix, which Element.cpp asserts.
        &unsafe { prefix_of::<ElementPrefix, _>(self.node.cell()) }.core
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

    /// https://dom.spec.whatwg.org/#concept-element-shadow-root
    #[inline(always)]
    pub fn shadow_root(self) -> Option<Node<'a>> {
        self.node.follow(self.core().shadow_root())
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
}
