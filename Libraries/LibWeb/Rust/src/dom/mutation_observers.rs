/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Whether anyone hears of a tree mutation, read from the cores, so that the DOM algorithms in Rust do not call into
//! C++ to queue a mutation record that no one would receive.
//! https://dom.spec.whatwg.org/#queueing-a-mutation-record
//!
//! The registered observer lists, the observers and the records stay C++'s. What the cores keep of them are flags: on
//! each node, NODE_HAS_REGISTERED_OBSERVERS, which C++ keeps as the node's registered observer list gains and loses
//! registered observers, transient ones too; and on each document, which record types its observers have ever asked
//! for, and whether something but its observers hears of every mutation in it.
//!
//! Queuing a "childList" record for a target finds its interested observers among the registered observers of the
//! target's inclusive ancestors, so a record reaches no observer unless the document's observers have asked for
//! "childList" records and one of those ancestors has registered observers. It reaches no one at all unless that holds
//! or the document has a mutation listener.

use super::node_core::DOCUMENT_HAS_MUTATION_LISTENER;
use super::node_core::DOCUMENT_OBSERVES_ATTRIBUTES;
use super::node_core::DOCUMENT_OBSERVES_CHILD_LIST;
use super::node_ref::NodeRef;

/// Whether `node` or one of its ancestors has registered observers.
#[inline(always)]
pub(crate) fn has_observed_inclusive_ancestor(node: NodeRef) -> bool {
    let mut current = Some(node);
    while let Some(node) = current {
        if node.core().has_registered_observers() {
            return true;
        }
        current = node.parent();
    }
    false
}

/// Whether queuing a tree mutation record for `target` reaches anyone, see the module documentation.
#[inline(always)]
pub(crate) fn tree_mutation_record_is_heard(target: NodeRef) -> bool {
    let flags = target.document_core().flags();
    if flags & DOCUMENT_HAS_MUTATION_LISTENER != 0 {
        return true;
    }
    flags & DOCUMENT_OBSERVES_CHILD_LIST != 0 && has_observed_inclusive_ancestor(target)
}

/// Whether queuing an "attributes" record for `target` would reach anyone. Like a "childList" record, it reaches no
/// observer unless one of the registered observers of the target's inclusive ancestors asks for its type.
#[inline(always)]
pub(crate) fn attribute_mutation_record_is_heard(target: NodeRef) -> bool {
    let flags = target.document_core().flags();
    if flags & DOCUMENT_HAS_MUTATION_LISTENER != 0 {
        return true;
    }
    flags & DOCUMENT_OBSERVES_ATTRIBUTES != 0 && has_observed_inclusive_ancestor(target)
}

#[cfg(test)]
mod tests {
    use super::super::element_core::ElementCore;
    use super::super::links::insert_child;
    use super::super::node_core::NodeType;
    use super::super::qualified_name::QualifiedNameField;
    use super::super::test_tree;
    use super::*;
    use crate::stage::MainThread;

    fn element(main_thread: &MainThread<'_>) -> NodeRef {
        let core = ElementCore::for_test(QualifiedNameField::for_test("div", None));
        NodeRef::from(test_tree::element(main_thread, core).node())
    }

    #[test]
    fn a_tree_mutation_record_is_heard_by_observers_of_ancestors_and_by_listeners() {
        let mut main_thread = MainThread::for_test();
        let document = NodeRef::from(test_tree::node(&main_thread, NodeType::DOCUMENT_NODE));
        let [root, child, other] = core::array::from_fn(|_| element(&main_thread));
        for node in [document, root, child, other] {
            test_tree::set_document(node.read(&main_thread), document.read(&main_thread));
        }
        insert_child(&mut main_thread, root, child, None);

        assert!(!tree_mutation_record_is_heard(child));

        // NB: An observer of another type of record hears of no tree mutation.
        let document_core = test_tree::document_core(document.read(&main_thread));
        document_core.set_flags_for_test(DOCUMENT_OBSERVES_ATTRIBUTES);
        root.core().set_has_registered_observers_for_test();
        assert!(!tree_mutation_record_is_heard(child));

        document_core.set_flags_for_test(DOCUMENT_OBSERVES_CHILD_LIST);
        assert!(tree_mutation_record_is_heard(child));
        assert!(tree_mutation_record_is_heard(root));
        assert!(!tree_mutation_record_is_heard(other));

        document_core.set_flags_for_test(DOCUMENT_HAS_MUTATION_LISTENER);
        assert!(tree_mutation_record_is_heard(other));
    }
}
