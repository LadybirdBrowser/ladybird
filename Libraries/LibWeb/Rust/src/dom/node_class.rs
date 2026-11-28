/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The classes of DOM nodes, which DOM/NodeClasses.json lists, numbered in depth-first order of the class hierarchy:
//! the classes that descend from a class have the ids right after its own, so whether a node is of a class, or of a
//! class that descends from it, is whether its class id is in the class's range.
//!
//! C++ writes a node's class id into the node's cell as it constructs the node, see GC::CellConstructionHook in
//! DOM/Node.h, which also fails to compile for a class of nodes that DOM/NodeClasses.json does not list.
//! DOM/NodeClassSteps.cpp fails to compile where the file lists a class under a class it does not derive from. Every event target keeps the id at the same place, and one that is no node has
//! NodeClassId::NotANode.

/// The id of a class of DOM nodes, as C++'s DOM::NodeClassId names it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct NodeClassId(u16);

include!(concat!(env!("OUT_DIR"), "/node_classes_generated.rs"));

/// Where every event target keeps its class id in its cell: in the padding at the end of the cell's header, before the
/// first field of Bindings::Wrappable, which Wrappable.h asserts.
pub const NODE_CLASS_ID_OFFSET: usize = 12;

#[allow(non_upper_case_globals, reason = "the ids are named as the C++ classes are")]
impl NodeClassId {
    /// The id of no class, which an event target that is no node has.
    pub const NotANode: Self = Self(0);

    /// Whether this is the id of `class`, or of a class that descends from it.
    #[inline(always)]
    pub const fn is_a(self, class: Self) -> bool {
        // NB: The subtraction wraps for an id below the class's, so that one comparison tells both bounds.
        self.0.wrapping_sub(class.0) <= LAST_DESCENDANT_IDS[class.0 as usize] - class.0
    }

    /// The id as an index into tables by class.
    #[inline(always)]
    pub const fn index(self) -> usize {
        self.0 as usize
    }

    /// The NODE_CLASS_HAS_* bits of the class, which say which steps it has.
    #[inline(always)]
    pub fn steps(self) -> u8 {
        ladybird_dom_node_class_steps[self.index()]
    }

    /// The class id of the event target whose cell starts at `cell`.
    ///
    /// # Safety
    ///
    /// `cell` must be the start of a live event target.
    #[inline(always)]
    pub(crate) unsafe fn of_cell<T>(cell: *const T) -> Self {
        // SAFETY: Every event target keeps its class id NODE_CLASS_ID_OFFSET bytes into its cell, see above.
        unsafe { *cell.byte_add(NODE_CLASS_ID_OFFSET).cast::<Self>() }
    }

    /// The id the test trees give their nodes.
    #[cfg(test)]
    pub(crate) const fn raw_for_test(self) -> u16 {
        self.0
    }
}

// The steps of a class of nodes, a bit each in the class's entry of ladybird_dom_node_class_steps, which say whether
// the class has steps beyond those of DOM::Node, DOM::Element and HTML::HTMLElement, which libweb_rust knows to do
// nothing for some nodes, or knows otherwise. DOM/NodeClassSteps.cpp works them out from the overrides of each class
// as it compiles. A form-associated class has the steps of a form-associated element, which HTML::HTMLElement runs.

/// Whether the class has insertion steps of its own, or is form-associated.
/// https://dom.spec.whatwg.org/#concept-node-insert-ext
pub const NODE_CLASS_HAS_INSERTION_STEPS: u8 = 1 << 0;
/// Whether the class has removing steps of its own, or is form-associated.
/// https://dom.spec.whatwg.org/#concept-node-remove-ext
pub const NODE_CLASS_HAS_REMOVING_STEPS: u8 = 1 << 1;
/// Whether the class has moving steps of its own, is form-associated, or is a body or frameset element, whose moving
/// steps DOM::Node runs.
/// https://dom.spec.whatwg.org/#concept-node-move-ext
pub const NODE_CLASS_HAS_MOVING_STEPS: u8 = 1 << 2;
/// Whether the class has post-connection steps, which only a class that overrides post_connection() has.
/// https://dom.spec.whatwg.org/#concept-node-post-connection-ext
pub const NODE_CLASS_HAS_POST_CONNECTION_STEPS: u8 = 1 << 3;
/// Whether the class has attribute change steps beyond those of every element and every HTML element, whose names
/// attributes.rs knows, or is form-associated.
/// https://dom.spec.whatwg.org/#concept-element-attributes-change-ext
pub const NODE_CLASS_HAS_ATTRIBUTE_CHANGE_STEPS: u8 = 1 << 4;
/// Whether the class overrides has_activation_behavior(), which only then can answer true.
/// https://dom.spec.whatwg.org/#eventtarget-activation-behavior
pub const NODE_CLASS_HAS_ACTIVATION_BEHAVIOR: u8 = 1 << 5;

#[cfg(not(test))]
#[allow(non_upper_case_globals, reason = "the table is named as in C++")]
unsafe extern "C" {
    /// The NODE_CLASS_HAS_* bits of each class of nodes, by class id, which DOM/NodeClassSteps.cpp defines.
    safe static ladybird_dom_node_class_steps: [u8; NODE_CLASS_ID_COUNT];
}

/// The unit tests take an input element to have the steps it has in C++, and every other class to have none.
#[cfg(test)]
#[allow(non_upper_case_globals, reason = "the table is named as in C++")]
static ladybird_dom_node_class_steps: [u8; NODE_CLASS_ID_COUNT] = {
    let mut steps = [0; NODE_CLASS_ID_COUNT];
    steps[NodeClassId::HTMLInputElement.index()] = NODE_CLASS_HAS_INSERTION_STEPS
        | NODE_CLASS_HAS_REMOVING_STEPS
        | NODE_CLASS_HAS_MOVING_STEPS
        | NODE_CLASS_HAS_ATTRIBUTE_CHANGE_STEPS
        | NODE_CLASS_HAS_ACTIVATION_BEHAVIOR;
    steps
};

const _: () = assert!(size_of::<NodeClassId>() == 2 && align_of::<NodeClassId>() == 2);
const _: () = assert!(NODE_CLASS_ID_OFFSET.is_multiple_of(align_of::<NodeClassId>()));
// NB: The id is in the padding of the header, which ends before the event target core.
const _: () = assert!(NODE_CLASS_ID_OFFSET + size_of::<NodeClassId>() <= super::event_target::EVENT_TARGET_CORE_OFFSET);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_class_is_a_class_it_descends_from() {
        assert!(NodeClassId::HTMLInputElement.is_a(NodeClassId::HTMLInputElement));
        assert!(NodeClassId::HTMLInputElement.is_a(NodeClassId::HTMLElement));
        assert!(NodeClassId::HTMLInputElement.is_a(NodeClassId::Element));
        assert!(NodeClassId::HTMLInputElement.is_a(NodeClassId::Node));
        assert!(NodeClassId::CDATASection.is_a(NodeClassId::Text));
        assert!(NodeClassId::ShadowRoot.is_a(NodeClassId::DocumentFragment));
    }

    #[test]
    fn a_class_is_no_class_it_does_not_descend_from() {
        assert!(!NodeClassId::HTMLElement.is_a(NodeClassId::HTMLInputElement));
        assert!(!NodeClassId::SVGElement.is_a(NodeClassId::HTMLElement));
        assert!(!NodeClassId::Text.is_a(NodeClassId::CDATASection));
        assert!(!NodeClassId::Document.is_a(NodeClassId::Element));
        assert!(!NodeClassId::NotANode.is_a(NodeClassId::Node));
        assert!(!NodeClassId::Node.is_a(NodeClassId::NotANode));
    }
}
