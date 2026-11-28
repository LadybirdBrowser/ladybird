/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lists of nodes that hold across calls into C++, for the DOM algorithms that collect nodes before they run steps that
//! can allocate, run script or change the tree.
//!
//! A `NodeRef` lives only on the stack, see node_ref.rs, so code that needs more nodes than it can name in local
//! variables keeps them in a [`RootedNodes`]: a list whose storage C++ roots. C++ embeds the list's
//! [`RootedNodeListCore`] in a DOM::RootedNodeList, which it makes in the frame of the entry point that calls into
//! Rust, and which registers with the heap as a root for as long as that frame lives. The heap reads the nodes of the
//! list from the core, so every node the list holds is alive and stays in place, even where nothing else refers to it,
//! such as a node the algorithm has removed from its tree.
//!
//! The list's storage is a buffer Rust allocates, grows and frees. That is memory the collector does not scan, but it
//! reads the nodes in it through the root, which is what the rule for holding nodes asks of a list of them.

use core::cell::Cell;
use core::marker::PhantomData;
use core::mem::ManuallyDrop;

use super::node_ref::NodeRef;

/// The storage of a list of nodes that C++ roots, which DOM::RootedNodeList embeds. Rust alone writes it. C++ only reads
/// the nodes as it hands them to the heap as roots, and has Rust free the storage as it destroys the list.
#[repr(C)]
pub struct RootedNodeListCore {
    /// The nodes, as many as the length, in a buffer with room for the capacity.
    nodes: Cell<*mut NodeRef>,
    length: Cell<usize>,
    capacity: Cell<usize>,
}

const _: () = assert!(size_of::<RootedNodeListCore>() == 24 && align_of::<RootedNodeListCore>() == 8);

impl RootedNodeListCore {
    /// The buffer as the vector it was made from.
    ///
    /// # Safety
    ///
    /// The caller must not let both the vector and the core free the buffer.
    unsafe fn vector(&self) -> ManuallyDrop<Vec<NodeRef>> {
        let nodes = self.nodes.get();
        if nodes.is_null() {
            return ManuallyDrop::new(Vec::new());
        }
        // SAFETY: The buffer was made from a vector of this length and capacity, see set_vector().
        ManuallyDrop::new(unsafe { Vec::from_raw_parts(nodes, self.length.get(), self.capacity.get()) })
    }

    fn set_vector(&self, mut vector: ManuallyDrop<Vec<NodeRef>>) {
        self.nodes.set(vector.as_mut_ptr());
        self.length.set(vector.len());
        self.capacity.set(vector.capacity());
    }
}

/// A list of nodes that C++ roots, see the module. The nodes it hands out are valid for as long as it holds them, and
/// can be held on the stack as any `NodeRef` beyond that.
pub struct RootedNodes<'a> {
    core: &'a RootedNodeListCore,
    // NB: The marker makes the list neither Send nor Sync, as it holds nodes.
    _marker: PhantomData<*mut u8>,
}

impl<'a> RootedNodes<'a> {
    /// The list whose storage is `core`.
    ///
    /// # Safety
    ///
    /// `core` must be the core of a DOM::RootedNodeList that lives for `'a`, which nothing else reads or writes for
    /// `'a`, but the heap as it gathers its roots.
    #[inline(always)]
    pub(crate) unsafe fn from_core(core: &'a RootedNodeListCore) -> Self {
        Self {
            core,
            _marker: PhantomData,
        }
    }

    #[inline(always)]
    pub fn len(&self) -> usize {
        self.core.length.get()
    }

    #[inline(always)]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The node at `index`, which must be less than the length.
    #[inline(always)]
    pub fn get(&self, index: usize) -> NodeRef {
        assert!(index < self.len(), "the index is less than the length");
        // SAFETY: The buffer holds as many nodes as the length.
        unsafe { *self.core.nodes.get().add(index) }
    }

    /// The last node, which the list must have.
    #[inline(always)]
    pub fn last(&self) -> NodeRef {
        self.get(self.len() - 1)
    }

    #[inline(always)]
    pub fn push(&mut self, node: NodeRef) {
        if self.len() == self.core.capacity.get() {
            return self.push_slow(node);
        }
        // SAFETY: The buffer has room for one more node, which the heap reads only once the length counts it.
        unsafe { self.core.nodes.get().add(self.len()).write(node) };
        self.core.length.set(self.len() + 1);
    }

    #[inline(never)]
    fn push_slow(&mut self, node: NodeRef) {
        // SAFETY: The core keeps the buffer the vector grows into, and the old one is not read again.
        let mut vector = unsafe { self.core.vector() };
        vector.push(node);
        self.core.set_vector(vector);
    }

    /// Keeps the first `length` nodes, and lets go of the rest.
    #[inline(always)]
    pub fn truncate(&mut self, length: usize) {
        if length < self.len() {
            self.core.length.set(length);
        }
    }
}

/// Frees the storage of `core`, for DOM::RootedNodeList as it is destroyed.
///
/// # Safety
///
/// `core` must be the core of a DOM::RootedNodeList that is being destroyed, and the call must be made on the main
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_rooted_node_list_destroy(core: &RootedNodeListCore) {
    // SAFETY: The core is not read again.
    drop(ManuallyDrop::into_inner(unsafe { core.vector() }));
    core.set_vector(ManuallyDrop::new(Vec::new()));
}

#[cfg(test)]
impl RootedNodeListCore {
    pub(crate) fn for_test() -> Self {
        Self {
            nodes: Cell::new(core::ptr::null_mut()),
            length: Cell::new(0),
            capacity: Cell::new(0),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::node_core::NodeType;
    use super::super::test_tree;
    use super::*;
    use crate::stage::MainThread;

    #[test]
    fn a_rooted_list_holds_the_nodes_pushed_to_it() {
        let main_thread = MainThread::for_test();
        let nodes: [NodeRef; 5] =
            core::array::from_fn(|_| NodeRef::from(test_tree::node(&main_thread, NodeType::TEXT_NODE)));
        let core = RootedNodeListCore::for_test();
        // SAFETY: Nothing else reads the core.
        let mut list = unsafe { RootedNodes::from_core(&core) };
        assert!(list.is_empty());
        for node in nodes {
            list.push(node);
        }
        assert_eq!(list.len(), 5);
        assert!((0..5).all(|index| list.get(index) == nodes[index]));
        list.truncate(2);
        assert_eq!(list.len(), 2);
        assert_eq!(list.last(), nodes[1]);
        list.push(nodes[4]);
        assert_eq!(list.last(), nodes[4]);
        // SAFETY: The list is not used again.
        unsafe { rust_dom_rooted_node_list_destroy(&core) };
        assert_eq!(core.length.get(), 0);
    }
}
