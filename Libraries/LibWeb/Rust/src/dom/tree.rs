/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Walks of the node tree, which read the links of the nodes' cores as they go.
//! https://dom.spec.whatwg.org/#trees

use super::node::Node;
use super::node_core::NodeType;
use super::node_ref::NodeRef;

impl<'a> Node<'a> {
    /// https://dom.spec.whatwg.org/#concept-tree-child
    pub fn children(self) -> impl Iterator<Item = Node<'a>> {
        core::iter::successors(self.first_child(), |child| child.next_sibling())
    }

    /// The node and its ancestors, from the node up.
    /// https://dom.spec.whatwg.org/#concept-tree-inclusive-ancestor
    pub fn inclusive_ancestors(self) -> impl Iterator<Item = Node<'a>> {
        core::iter::successors(Some(self), |node| node.parent())
    }

    /// https://dom.spec.whatwg.org/#concept-tree-inclusive-ancestor
    #[inline(always)]
    pub fn is_inclusive_ancestor_of(self, other: Node<'a>) -> bool {
        other == self || self.is_ancestor_of(other)
    }

    /// https://dom.spec.whatwg.org/#concept-tree-ancestor
    pub fn is_ancestor_of(self, other: Node<'a>) -> bool {
        // OPTIMIZATION: Each link is tested for null while it is still compressed, and then decoded without a null
        //               check, so each step up is one load, one test, one decode and one compare, as in C++'s
        //               TreeNode::is_ancestor_of().
        let mut node = other;
        while let Some(parent) = node.parent() {
            if parent == self {
                return true;
            }
            node = parent;
        }
        false
    }

    /// https://dom.spec.whatwg.org/#concept-tree-index
    pub fn index(self) -> usize {
        // The index of an object is its number of preceding siblings, or 0 if it has none.
        core::iter::successors(self.previous_sibling(), |sibling| sibling.previous_sibling()).count()
    }

    /// https://dom.spec.whatwg.org/#concept-tree-following
    pub fn is_following(self, other: Node<'a>) -> bool {
        // An object A is following an object B if A and B are in the same tree and A comes after B in tree order.
        other.is_preceding(self)
    }

    /// https://dom.spec.whatwg.org/#concept-tree-preceding
    pub fn is_preceding(self, other: Node<'a>) -> bool {
        // An object A is preceding an object B if A and B are in the same tree and A comes before B in tree order.
        // OPTIMIZATION: The order is read off the ancestor chains, which are as long as the tree is deep, rather than
        //               off a walk in tree order from one to the other, which can be as long as the tree is large.
        if self == other {
            return false;
        }
        let depth_and_root = |node: Node<'a>| {
            let mut depth = 0;
            let mut root = node;
            while let Some(parent) = root.parent() {
                depth += 1;
                root = parent;
            }
            (depth, root)
        };
        let (mut depth, root) = depth_and_root(self);
        let (mut other_depth, other_root) = depth_and_root(other);
        if root != other_root {
            return false;
        }

        // An inclusive ancestor comes before its descendants.
        let (mut ancestor, mut other_ancestor) = (self, other);
        while depth > other_depth {
            ancestor = ancestor.parent().expect("a node is no deeper than its depth");
            depth -= 1;
        }
        if ancestor == other {
            return false;
        }
        while other_depth > depth {
            other_ancestor = other_ancestor.parent().expect("a node is no deeper than its depth");
            other_depth -= 1;
        }
        if other_ancestor == self {
            return true;
        }

        // Otherwise, the children of the nodes' deepest common ancestor that hold them come in their order.
        while ancestor.parent() != other_ancestor.parent() {
            ancestor = ancestor.parent().expect("nodes of one tree have a common ancestor");
            other_ancestor = other_ancestor
                .parent()
                .expect("nodes of one tree have a common ancestor");
        }
        core::iter::successors(ancestor.next_sibling(), |sibling| sibling.next_sibling())
            .any(|sibling| sibling == other_ancestor)
    }

    /// https://dom.spec.whatwg.org/#concept-tree-host-including-inclusive-ancestor
    pub fn is_host_including_inclusive_ancestor_of(self, other: Node<'a>) -> bool {
        // An object A is a host-including inclusive ancestor of an object B, if either A is an inclusive ancestor of B,
        // or if B’s root has a non-null host and A is a host-including inclusive ancestor of B’s root’s host.
        let mut other = other;
        loop {
            if self.is_inclusive_ancestor_of(other) {
                return true;
            }
            let root = other.root();
            if root.node_type() != NodeType::DOCUMENT_FRAGMENT_NODE {
                return false;
            }
            let Some(host) = root.host() else {
                return false;
            };
            other = host;
        }
    }

    /// https://dom.spec.whatwg.org/#concept-shadow-including-root
    pub fn shadow_including_root(self) -> Node<'a> {
        // The shadow-including root of an object is its root’s host’s shadow-including root, if the object’s root is a
        // shadow root; otherwise its root.
        let mut root = self.root();
        while root.is_shadow_root()
            && let Some(host) = root.host()
        {
            root = host.root();
        }
        root
    }

    /// The nodes that come after this node in tree order, in tree order.
    /// https://dom.spec.whatwg.org/#concept-tree-following
    pub fn following(self) -> impl Iterator<Item = Node<'a>> {
        core::iter::successors(self.next_in_tree_order(), |node| node.next_in_tree_order())
    }

    /// The nodes that come before this node in tree order, in reverse tree order.
    /// https://dom.spec.whatwg.org/#concept-tree-preceding
    pub fn preceding(self) -> impl Iterator<Item = Node<'a>> {
        core::iter::successors(self.previous_in_tree_order(), |node| node.previous_in_tree_order())
    }

    /// The node after this one in tree order that is a descendant of `root`, an inclusive ancestor of this one.
    pub fn next_in_subtree(self, root: Node<'a>) -> Option<Node<'a>> {
        self.first_child().or_else(|| self.next_after_subtree(root))
    }

    /// The node after this one and its descendants in tree order that is a descendant of `root`, an inclusive ancestor
    /// of this one.
    pub fn next_after_subtree(self, root: Node<'a>) -> Option<Node<'a>> {
        let mut current = self;
        while current != root {
            if let Some(sibling) = current.next_sibling() {
                return Some(sibling);
            }
            current = current.parent()?;
        }
        None
    }

    /// The node after this one in tree order: its first child, or else the next sibling of its nearest inclusive
    /// ancestor that has one.
    /// https://dom.spec.whatwg.org/#concept-tree-order
    pub fn next_in_tree_order(self) -> Option<Node<'a>> {
        self.first_child()
            .or_else(|| self.inclusive_ancestors().find_map(|ancestor| ancestor.next_sibling()))
    }

    /// The node before this one in tree order: the last inclusive descendant of its previous sibling, along last
    /// children, or else its parent.
    /// https://dom.spec.whatwg.org/#concept-tree-order
    pub fn previous_in_tree_order(self) -> Option<Node<'a>> {
        let Some(previous_sibling) = self.previous_sibling() else {
            return self.parent();
        };
        core::iter::successors(Some(previous_sibling), |node| node.last_child()).last()
    }
}

/// Calls `callback` with each shadow-including inclusive descendant of `node`, in shadow-including tree order, as
/// DOM::Node::for_each_shadow_including_inclusive_descendant() does. The callback may change the tree: the walk reads
/// each link as it reaches it, so it goes on from where the nodes it holds are when the callback returns.
/// https://dom.spec.whatwg.org/#concept-shadow-including-inclusive-descendant
pub(crate) fn for_each_shadow_including_inclusive_descendant(node: NodeRef, callback: &mut impl FnMut(NodeRef)) {
    callback(node);
    for_each_shadow_including_descendant(node, callback);
}

/// Calls `callback` with each shadow-including descendant of `node`, in shadow-including tree order, as
/// DOM::Node::for_each_shadow_including_descendant() does: an element's shadow root and its shadow-including
/// descendants come before the element's children.
/// https://dom.spec.whatwg.org/#concept-shadow-including-descendant
pub(crate) fn for_each_shadow_including_descendant(node: NodeRef, callback: &mut impl FnMut(NodeRef)) {
    // NB: The walk recurses, as the C++ one does, so that the nodes it holds stay on the stack.
    if let Some(shadow_root) = node.shadow_root() {
        for_each_shadow_including_inclusive_descendant(shadow_root, callback);
    }
    let mut child = node.first_child();
    while let Some(current) = child {
        for_each_shadow_including_inclusive_descendant(current, callback);
        child = current.next_sibling();
    }
}

#[cfg(test)]
mod tests {
    use super::super::test_tree::append_child;
    use super::super::test_tree::node;
    use super::*;
    use crate::stage::MainThread;

    /// The nodes of a document with this tree, in tree order:
    ///
    /// ```text
    /// document
    ///   doctype
    ///   html
    ///     head
    ///       title
    ///         text
    ///     body
    ///       comment
    /// ```
    fn document<'a>(main_thread: &'a MainThread<'_>) -> [Node<'a>; 8] {
        let new = |node_type| node(main_thread, node_type);
        let nodes = [
            new(NodeType::DOCUMENT_NODE),
            new(NodeType::DOCUMENT_TYPE_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::TEXT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::COMMENT_NODE),
        ];
        let [document, doctype, html, head, title, text, body, comment] = nodes;
        append_child(title, text);
        append_child(head, title);
        append_child(html, head);
        append_child(body, comment);
        append_child(html, body);
        append_child(document, doctype);
        append_child(document, html);
        nodes
    }

    #[test]
    fn children_are_in_order() {
        let main_thread = MainThread::for_test();
        let [document, doctype, html, head, _, text, body, _] = document(&main_thread);
        assert_eq!(document.children().collect::<Vec<_>>(), [doctype, html]);
        assert_eq!(html.children().collect::<Vec<_>>(), [head, body]);
        assert_eq!(text.children().next(), None);
    }

    #[test]
    fn inclusive_ancestors_go_from_the_node_up_to_the_root() {
        let main_thread = MainThread::for_test();
        let [document, _, html, head, title, text, _, _] = document(&main_thread);
        assert_eq!(
            text.inclusive_ancestors().collect::<Vec<_>>(),
            [text, title, head, html, document]
        );
        assert_eq!(document.inclusive_ancestors().collect::<Vec<_>>(), [document]);
    }

    #[test]
    fn a_node_is_an_inclusive_ancestor_of_itself_and_its_descendants_only() {
        let main_thread = MainThread::for_test();
        let [document, doctype, html, head, _, text, body, comment] = document(&main_thread);
        assert!(document.is_inclusive_ancestor_of(comment));
        assert!(head.is_inclusive_ancestor_of(text));
        assert!(text.is_inclusive_ancestor_of(text));
        assert!(!body.is_inclusive_ancestor_of(text));
        assert!(!text.is_inclusive_ancestor_of(head));
        assert!(!doctype.is_inclusive_ancestor_of(html));

        let other_tree = node(&main_thread, NodeType::ELEMENT_NODE);
        assert!(!document.is_inclusive_ancestor_of(other_tree));
        assert!(!other_tree.is_inclusive_ancestor_of(document));
    }

    #[test]
    fn following_walks_the_rest_of_the_tree_in_tree_order() {
        let main_thread = MainThread::for_test();
        let nodes = document(&main_thread);
        let [document, doctype, _, head, _, text, body, comment] = nodes;
        assert_eq!(document.following().collect::<Vec<_>>(), nodes[1..]);
        assert_eq!(doctype.following().collect::<Vec<_>>(), nodes[2..]);
        assert_eq!(head.following().collect::<Vec<_>>(), nodes[4..]);
        assert_eq!(text.following().collect::<Vec<_>>(), [body, comment]);
        assert_eq!(comment.following().next(), None);
    }

    #[test]
    fn a_walk_of_a_subtree_stays_under_its_root() {
        let main_thread = MainThread::for_test();
        let nodes = document(&main_thread);
        let [document, doctype, html, head, title, text, body, comment] = nodes;
        fn walk(root: Node<'_>) -> impl Iterator<Item = Node<'_>> {
            core::iter::successors(root.next_in_subtree(root), move |node| node.next_in_subtree(root))
        }
        assert_eq!(walk(document).collect::<Vec<_>>(), nodes[1..]);
        assert_eq!(walk(head).collect::<Vec<_>>(), [title, text]);
        assert_eq!(walk(comment).next(), None);
        assert_eq!(head.next_after_subtree(html), Some(body));
        assert_eq!(text.next_after_subtree(head), None);
        assert_eq!(text.next_after_subtree(document), Some(body));
        assert_eq!(doctype.next_after_subtree(document), Some(html));
        assert_eq!(body.next_after_subtree(body), None);
    }

    #[test]
    fn a_shadow_including_walk_visits_a_shadow_root_before_its_hosts_children() {
        let main_thread = MainThread::for_test();
        let host = node(&main_thread, NodeType::ELEMENT_NODE);
        let light_child = node(&main_thread, NodeType::ELEMENT_NODE);
        append_child(host, light_child);
        let shadow_root = super::super::test_tree::shadow_root(&main_thread, host);
        let shadow_child = node(&main_thread, NodeType::TEXT_NODE);
        append_child(shadow_root, shadow_child);
        let root = node(&main_thread, NodeType::DOCUMENT_FRAGMENT_NODE);
        append_child(root, host);

        let mut visited = Vec::new();
        for_each_shadow_including_inclusive_descendant(NodeRef::from(root), &mut |node| visited.push(node));
        let expected = [root, host, shadow_root, shadow_child, light_child].map(NodeRef::from);
        assert_eq!(visited, expected);

        visited.clear();
        for_each_shadow_including_descendant(NodeRef::from(host), &mut |node| visited.push(node));
        assert_eq!(visited, expected[2..]);
    }

    #[test]
    fn preceding_walks_back_to_the_root_in_reverse_tree_order() {
        let main_thread = MainThread::for_test();
        let nodes = document(&main_thread);
        let [document, doctype, html, head, title, text, body, comment] = nodes;
        assert_eq!(
            comment.preceding().collect::<Vec<_>>(),
            [body, text, title, head, html, doctype, document]
        );
        assert_eq!(
            body.preceding().collect::<Vec<_>>(),
            [text, title, head, html, doctype, document]
        );
        assert_eq!(html.preceding().collect::<Vec<_>>(), [doctype, document]);
        assert_eq!(document.preceding().next(), None);
    }
}
