/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! NodeIterator and TreeWalker: their state, which C++ keeps in cores whose layout is defined here, filtering, their
//! traversals, and the NodeIterator pre-remove steps.
//! https://dom.spec.whatwg.org/#traversal
//!
//! Filtering calls the traverser's filter, which can run script, which can change the tree, a traverser's state and
//! anything else. So the traversals hold nodes as [`NodeRef`]s, and read the traverser's core anew after each filter.
//! The traversers themselves are C++ cells, which never move, and which the C++ caller keeps alive for the call.

use core::cell::Cell;
use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::mem::offset_of;
use core::ptr::NonNull;

use libgc_api::Trace;

use super::hooks;
use super::node::Node;
use super::node::NodeCell;
use super::node_core::NodeMember;
use super::node_core::NodePointer;
use super::node_ref::NodeRef;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// Where DOM::NodeIterator and DOM::TreeWalker embed their cores, in bytes from the start of their cells, which
/// NodeIterator.cpp and TreeWalker.cpp assert.
pub const TRAVERSER_CORE_OFFSET: usize = 24;

/// https://dom.spec.whatwg.org/#concept-traversal-active
pub const TRAVERSER_IS_ACTIVE: u8 = 1 << 0;
/// The pointer before of a NodeIterator's reference.
/// https://dom.spec.whatwg.org/#node-pointer-pointer-before
pub const NODE_ITERATOR_POINTER_BEFORE_REFERENCE: u8 = 1 << 1;
/// The pointer before of a NodeIterator's candidate reference, if it is non-null.
/// https://dom.spec.whatwg.org/#nodeiterator-candidate-reference
pub const NODE_ITERATOR_CANDIDATE_POINTER_BEFORE_REFERENCE: u8 = 1 << 2;

/// https://dom.spec.whatwg.org/#dom-nodefilter-filter_accept
pub const FILTER_ACCEPT: u8 = 1;
/// https://dom.spec.whatwg.org/#dom-nodefilter-filter_reject
pub const FILTER_REJECT: u8 = 2;
/// https://dom.spec.whatwg.org/#dom-nodefilter-filter_skip
pub const FILTER_SKIP: u8 = 3;

/// What NodeIterator and TreeWalker objects share, their traverser state, but for the filter, which C++ keeps.
/// https://dom.spec.whatwg.org/#traversal
#[derive(Trace)]
#[repr(C)]
pub struct TraverserCore {
    /// https://dom.spec.whatwg.org/#concept-traversal-root
    root: NodeMember,
    /// https://dom.spec.whatwg.org/#concept-traversal-whattoshow
    what_to_show: Cell<u32>,
    /// The TRAVERSER_* and NODE_ITERATOR_* flags.
    flags: Cell<u8>,
}

impl TraverserCore {
    #[inline(always)]
    fn root(&self) -> NodeRef {
        // SAFETY: A traverser keeps its root alive.
        unsafe { NodeRef::from_member(&self.root) }.expect("a traverser has a root")
    }

    #[inline(always)]
    fn has_flag(&self, flag: u8) -> bool {
        self.flags.get() & flag != 0
    }

    #[inline(always)]
    fn set_flag(&self, flag: u8, value: bool) {
        let flags = self.flags.get() & !flag;
        self.flags.set(if value { flags | flag } else { flags });
    }
}

/// The state of a NodeIterator, which C++ embeds in DOM::NodeIterator.
/// https://dom.spec.whatwg.org/#nodeiterator
#[derive(Trace)]
#[repr(C)]
pub struct NodeIteratorCore {
    traverser: TraverserCore,
    /// The node of the reference, whose pointer before is a flag.
    /// https://dom.spec.whatwg.org/#nodeiterator-reference
    reference: NodeMember,
    /// The node of the candidate reference, or null if the candidate reference is null, whose pointer before is a flag.
    /// https://dom.spec.whatwg.org/#nodeiterator-candidate-reference
    candidate_reference: NodeMember,
}

/// A node pointer, a node and its pointer before.
/// https://dom.spec.whatwg.org/#node-pointer
#[derive(Clone, Copy)]
struct NodePointerTuple<N> {
    node: N,
    pointer_before: bool,
}

impl NodeIteratorCore {
    #[inline(always)]
    fn reference(&self) -> NodePointerTuple<NodeRef> {
        NodePointerTuple {
            // SAFETY: A NodeIterator keeps its reference's node alive.
            node: unsafe { NodeRef::from_member(&self.reference) }.expect("a NodeIterator has a reference"),
            pointer_before: self.traverser.has_flag(NODE_ITERATOR_POINTER_BEFORE_REFERENCE),
        }
    }

    #[inline(always)]
    fn set_reference(&self, pointer: NodePointerTuple<NodePointer>) {
        self.reference.set(Some(pointer.node));
        self.traverser
            .set_flag(NODE_ITERATOR_POINTER_BEFORE_REFERENCE, pointer.pointer_before);
    }

    #[inline(always)]
    fn candidate_reference(&self) -> Option<NodePointerTuple<NodeRef>> {
        // SAFETY: A NodeIterator keeps its candidate reference's node alive.
        let node = unsafe { NodeRef::from_member(&self.candidate_reference) }?;
        Some(NodePointerTuple {
            node,
            pointer_before: self
                .traverser
                .has_flag(NODE_ITERATOR_CANDIDATE_POINTER_BEFORE_REFERENCE),
        })
    }

    #[inline(always)]
    fn set_candidate_reference(&self, pointer: Option<NodePointerTuple<NodePointer>>) {
        self.candidate_reference.set(pointer.map(|pointer| pointer.node));
        self.traverser.set_flag(
            NODE_ITERATOR_CANDIDATE_POINTER_BEFORE_REFERENCE,
            pointer.is_some_and(|pointer| pointer.pointer_before),
        );
    }
}

/// The state of a TreeWalker, which C++ embeds in DOM::TreeWalker.
/// https://dom.spec.whatwg.org/#treewalker
#[derive(Trace)]
#[repr(C)]
pub struct TreeWalkerCore {
    traverser: TraverserCore,
    /// https://dom.spec.whatwg.org/#treewalker-current
    current: NodeMember,
}

impl TreeWalkerCore {
    #[inline(always)]
    fn current(&self) -> NodeRef {
        // SAFETY: A TreeWalker keeps its current alive.
        unsafe { NodeRef::from_member(&self.current) }.expect("a TreeWalker has a current")
    }

    #[inline(always)]
    fn set_current(&self, node: NodeRef) {
        self.current.set(Some(node.pointer()));
    }
}

/// A DOM::NodeIterator as C++ allocates it: the start of its cell, with its core TRAVERSER_CORE_OFFSET bytes into it.
#[repr(C)]
pub struct NodeIteratorCell {
    // NB: The cell's state changes behind shared references to it.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

impl NodeIteratorCell {
    #[inline(always)]
    fn core(&self) -> &NodeIteratorCore {
        // SAFETY: Every NodeIterator embeds its core TRAVERSER_CORE_OFFSET bytes into its cell, which NodeIterator.cpp
        //         asserts.
        unsafe {
            &*(self as *const Self)
                .byte_add(TRAVERSER_CORE_OFFSET)
                .cast::<NodeIteratorCore>()
        }
    }
}

/// A DOM::TreeWalker as C++ allocates it: the start of its cell, with its core TRAVERSER_CORE_OFFSET bytes into it.
#[repr(C)]
pub struct TreeWalkerCell {
    // NB: The cell's state changes behind shared references to it.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

impl TreeWalkerCell {
    #[inline(always)]
    fn core(&self) -> &TreeWalkerCore {
        // SAFETY: Every TreeWalker embeds its core TRAVERSER_CORE_OFFSET bytes into its cell, which TreeWalker.cpp
        //         asserts.
        unsafe {
            &*(self as *const Self)
                .byte_add(TRAVERSER_CORE_OFFSET)
                .cast::<TreeWalkerCore>()
        }
    }
}

/// What the filter of a traverser answers when hooks::call_node_filter() calls it: the result, or whether it threw.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NodeFilterCall {
    pub result: u8,
    pub threw: bool,
}

/// Why a traversal stopped without a node or null, which the caller throws.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TraversalError {
    /// An "InvalidStateError" DOMException, as the traverser is active.
    AlreadyActive,
    /// The exception the filter threw, which C++ keeps.
    CallbackException,
}

/// How a traversal ends, laid out as C++'s DOM::TraversalResult::Type, which is this type.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TraversalResultType {
    Node,
    Null,
    AlreadyActive,
    CallbackException,
}

/// The node a traversal returns, or null, which C++ sees as an opaque pointer.
#[repr(transparent)]
pub struct TraversedNode(*const NodeCell);

/// How a traversal ends, with the node it returns if it returns one, for C++.
#[repr(C)]
pub struct TraversalResult {
    pub kind: TraversalResultType,
    pub node: TraversedNode,
}

impl From<Result<Option<NodeRef>, TraversalError>> for TraversalResult {
    fn from(result: Result<Option<NodeRef>, TraversalError>) -> Self {
        let (kind, node) = match result {
            Ok(Some(node)) => (TraversalResultType::Node, node.cell() as *const NodeCell),
            Ok(None) => (TraversalResultType::Null, core::ptr::null()),
            Err(TraversalError::AlreadyActive) => (TraversalResultType::AlreadyActive, core::ptr::null()),
            Err(TraversalError::CallbackException) => (TraversalResultType::CallbackException, core::ptr::null()),
        };
        Self {
            kind,
            node: TraversedNode(node),
        }
    }
}

/// The filter of a traverser as C++ hands it over: a DOM::TraversalFilter to call through hooks::call_node_filter(),
/// or none if the traverser's filter is null.
type Filter = Option<NonNull<c_void>>;

/// https://dom.spec.whatwg.org/#concept-node-filter
fn filter(
    main_thread: &mut MainThread<'_>,
    traverser: &TraverserCore,
    traverser_filter: Filter,
    node: NodeRef,
) -> Result<u8, TraversalError> {
    // To filter a node node within a NodeIterator or TreeWalker object traverser:

    // 1. If traverser’s is active is true, then throw an "InvalidStateError" DOMException.
    if traverser.has_flag(TRAVERSER_IS_ACTIVE) {
        return Err(TraversalError::AlreadyActive);
    }

    // 2. Let n be node’s nodeType attribute value − 1.
    let n = u32::from(node.node_type().value()) - 1;

    // 3. If the nth bit (where 0 is the least significant bit) of traverser’s whatToShow is not set, then return
    //    FILTER_SKIP.
    if traverser.what_to_show.get() & (1 << n) == 0 {
        return Ok(FILTER_SKIP);
    }

    // 4. If traverser’s filter is null, then return FILTER_ACCEPT.
    let Some(traverser_filter) = traverser_filter else {
        return Ok(FILTER_ACCEPT);
    };

    // 5. Set traverser’s is active to true.
    traverser.set_flag(TRAVERSER_IS_ACTIVE, true);

    // 6. Let result be the return value of call a user object’s operation with traverser’s filter, "acceptNode", and
    //    « node ». If this throws an exception, then set traverser’s is active to false and rethrow the exception.
    let result = hooks::call_node_filter(main_thread, traverser_filter, node);
    if result.threw {
        traverser.set_flag(TRAVERSER_IS_ACTIVE, false);
        return Err(TraversalError::CallbackException);
    }

    // 7. Set traverser’s is active to false.
    traverser.set_flag(TRAVERSER_IS_ACTIVE, false);

    // 8. Return result.
    Ok(result.result)
}

/// Whether a NodeIterator traverses to the next node or the previous one.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NodeIteratorDirection {
    Next,
    Previous,
}

/// https://dom.spec.whatwg.org/#concept-nodeiterator-traverse
fn traverse(
    main_thread: &mut MainThread<'_>,
    iterator: &NodeIteratorCore,
    iterator_filter: Filter,
    direction: NodeIteratorDirection,
) -> Result<Option<NodeRef>, TraversalError> {
    let to_pointer = |pointer: NodePointerTuple<NodeRef>| NodePointerTuple {
        node: pointer.node.pointer(),
        pointer_before: pointer.pointer_before,
    };

    // To traverse, given a NodeIterator object iterator and "next" or "previous" type:

    // AD-HOC: A traversal that a filter starts on its own iterator would set the candidate reference of the traversal
    //         that called the filter, and null it as filtering throws. It throws before it changes the candidate
    //         reference instead, which the traversal that called the filter goes on with.
    if iterator.traverser.has_flag(TRAVERSER_IS_ACTIVE) {
        return Err(TraversalError::AlreadyActive);
    }

    // 1. Set iterator’s candidate reference to iterator’s reference.
    iterator.set_candidate_reference(Some(to_pointer(iterator.reference())));

    // 2. Let result be null.
    let mut result = None;

    // OPTIMIZATION: A traverser's root never changes, so it is read once.
    let root = iterator.traverser.root();

    // 3. While true:
    loop {
        // NB: The filter can run script, which can change the candidate reference through the NodeIterator
        //     pre-remove steps, so it is read anew.
        let candidate_reference = iterator
            .candidate_reference()
            .expect("the traversal has a candidate reference");
        let main_thread_ref = &*main_thread;
        let candidate_node = candidate_reference.node.read(main_thread_ref);
        let root_node = root.read(main_thread_ref);

        let next_candidate = match direction {
            // 1. If type is "next":
            NodeIteratorDirection::Next => {
                // 1. If iterator’s candidate reference’s pointer before is false:
                if !candidate_reference.pointer_before {
                    // 1. Let following be the first node following iterator’s candidate reference’s node in
                    //    iterator’s iterator collection. If there is no such node, then break.
                    let Some(following) = candidate_node.next_in_subtree(root_node) else {
                        break;
                    };

                    // 2. Set iterator’s candidate reference to (following, false).
                    NodePointerTuple {
                        node: following.pointer(),
                        pointer_before: false,
                    }
                }
                // 2. Otherwise, set iterator’s candidate reference to (iterator’s candidate reference’s node, false).
                else {
                    NodePointerTuple {
                        node: candidate_node.pointer(),
                        pointer_before: false,
                    }
                }
            }
            // 2. Otherwise:
            NodeIteratorDirection::Previous => {
                // 1. If iterator’s candidate reference’s pointer before is true:
                if candidate_reference.pointer_before {
                    // 1. Let preceding be the first node preceding iterator’s candidate reference’s node in iterator’s
                    //    iterator collection. If there is no such node, then break.
                    // NB: The root is the first node of the iterator collection, which holds its inclusive descendants.
                    if candidate_node == root_node {
                        break;
                    }
                    let Some(preceding) = candidate_node.previous_in_tree_order() else {
                        break;
                    };

                    // 2. Set iterator’s candidate reference to (preceding, true).
                    NodePointerTuple {
                        node: preceding.pointer(),
                        pointer_before: true,
                    }
                }
                // 2. Otherwise, set iterator’s candidate reference to (iterator’s candidate reference’s node, true).
                else {
                    NodePointerTuple {
                        node: candidate_node.pointer(),
                        pointer_before: true,
                    }
                }
            }
        };
        iterator.set_candidate_reference(Some(next_candidate));

        // 3. Let node be iterator’s candidate reference’s node.
        let node = iterator
            .candidate_reference()
            .expect("the traversal has a candidate reference")
            .node;

        // 4. Let filterResult be the result of filtering node within iterator. If this throws an exception, then set
        //    iterator’s candidate reference to null and rethrow that exception.
        let filter_result = match filter(main_thread, &iterator.traverser, iterator_filter, node) {
            Ok(filter_result) => filter_result,
            Err(error) => {
                iterator.set_candidate_reference(None);
                return Err(error);
            }
        };

        // 5. If filterResult is FILTER_ACCEPT:
        if filter_result == FILTER_ACCEPT {
            // 1. Set iterator’s reference to iterator’s candidate reference.
            let candidate_reference = iterator
                .candidate_reference()
                .expect("the traversal has a candidate reference");
            iterator.set_reference(to_pointer(candidate_reference));

            // 2. Set result to node.
            result = Some(node);

            // 3. Break.
            break;
        }
    }

    // 4. Set iterator’s candidate reference to null.
    iterator.set_candidate_reference(None);

    // 5. Return result.
    Ok(result)
}

/// https://dom.spec.whatwg.org/#nodeiterator-adjust-a-node-pointer
fn adjust_a_node_pointer<'a>(
    node_pointer: NodePointerTuple<Node<'a>>,
    root: Node<'a>,
    to_be_removed_node: Node<'a>,
) -> NodePointerTuple<Node<'a>> {
    // To adjust a node pointer given a node pointer nodePointer, a NodeIterator object nodeIterator, and a node
    // toBeRemovedNode, perform the following steps. They return a node pointer.

    // 1. If toBeRemovedNode is not an inclusive ancestor of nodePointer’s node, or toBeRemovedNode is an inclusive
    //    ancestor of nodeIterator’s root, then return nodePointer.
    if !to_be_removed_node.is_inclusive_ancestor_of(node_pointer.node)
        || to_be_removed_node.is_inclusive_ancestor_of(root)
    {
        return node_pointer;
    }

    // 2. If nodePointer’s pointer before is true:
    if node_pointer.pointer_before {
        // 1. Let next be toBeRemovedNode’s first following node that is an inclusive descendant of nodeIterator’s root
        //    and is not an inclusive descendant of toBeRemovedNode, if there is such a node; otherwise null.
        // NB: Step 1 leaves toBeRemovedNode a descendant of the root.
        let next = to_be_removed_node.next_after_subtree(root);

        // 2. If next is non-null, then return (next, true).
        if let Some(next) = next {
            return NodePointerTuple {
                node: next,
                pointer_before: true,
            };
        }
    }

    // 3. Let newNode be toBeRemovedNode’s parent, if toBeRemovedNode’s previous sibling is null; otherwise the inclusive
    //    descendant of toBeRemovedNode’s previous sibling that appears last in tree order.
    // NB: This is toBeRemovedNode’s previous node in tree order, which is never null, as step 1 leaves toBeRemovedNode
    //     a descendant of the root.
    let new_node = to_be_removed_node
        .previous_in_tree_order()
        .expect("a descendant of the root has a parent");

    // 4. Return (newNode, false).
    NodePointerTuple {
        node: new_node,
        pointer_before: false,
    }
}

/// Step 4 of removing `node`, and step 10 of moving it: for each NodeIterator object iterator whose root’s node
/// document is node’s node document, runs the NodeIterator pre-remove steps given node and iterator.
/// https://dom.spec.whatwg.org/#concept-node-remove
/// https://dom.spec.whatwg.org/#move
#[inline(always)]
pub(crate) fn run_node_iterator_pre_remove_steps(main_thread: &MainThread<'_>, node: NodeRef) {
    // NB: Every NodeIterator is in the registry of its root's node document.
    // OPTIMIZATION: A document rarely has a NodeIterator, so the check for none is inlined into the removal.
    if node.document_core().node_iterators().is_empty() {
        return;
    }
    run_node_iterator_pre_remove_steps_slow(main_thread, node);
}

#[inline(never)]
fn run_node_iterator_pre_remove_steps_slow(main_thread: &MainThread<'_>, node: NodeRef) {
    let node_iterators = node.document_core().node_iterators();
    let to_be_removed_node = node.read(main_thread);
    let read = |pointer: NodePointerTuple<NodeRef>| NodePointerTuple {
        node: pointer.node.read(main_thread),
        pointer_before: pointer.pointer_before,
    };
    let write = |pointer: NodePointerTuple<Node<'_>>| NodePointerTuple {
        node: pointer.node.pointer(),
        pointer_before: pointer.pointer_before,
    };

    // SAFETY: The registry's entries are as C++ last mirrored them, as nothing can call into C++ while the main thread
    //         token is borrowed, and each live entry is a live NodeIterator.
    unsafe {
        node_iterators.for_each_cell(|cell| {
            let iterator = cell.cast::<NodeIteratorCell>().as_ref().core();
            let root = iterator.traverser.root().read(main_thread);

            // https://dom.spec.whatwg.org/#nodeiterator-pre-removing-steps
            // The NodeIterator pre-remove steps given a NodeIterator object nodeIterator and node toBeRemovedNode, are:

            // 1. Set nodeIterator’s reference to the result of adjusting a node pointer given nodeIterator’s reference,
            //    nodeIterator, and toBeRemovedNode.
            iterator.set_reference(write(adjust_a_node_pointer(
                read(iterator.reference()),
                root,
                to_be_removed_node,
            )));

            // 2. If nodeIterator’s candidate reference is non-null, then set nodeIterator’s candidate reference to the
            //    result of adjusting a node pointer given nodeIterator’s candidate reference, nodeIterator, and
            //    toBeRemovedNode.
            if let Some(candidate_reference) = iterator.candidate_reference() {
                iterator.set_candidate_reference(Some(write(adjust_a_node_pointer(
                    read(candidate_reference),
                    root,
                    to_be_removed_node,
                ))));
            }
        });
    }
}

/// The child traversals of a TreeWalker.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChildTraversalType {
    First,
    Last,
}

/// The sibling traversals of a TreeWalker.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SiblingTraversalType {
    Next,
    Previous,
}

/// The TreeWalker method that rust_dom_tree_walker_traverse() runs.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TreeWalkerTraversal {
    ParentNode,
    FirstChild,
    LastChild,
    PreviousSibling,
    NextSibling,
    PreviousNode,
    NextNode,
}

/// https://dom.spec.whatwg.org/#dom-treewalker-parentnode
fn parent_node(
    main_thread: &mut MainThread<'_>,
    this: &TreeWalkerCore,
    this_filter: Filter,
) -> Result<Option<NodeRef>, TraversalError> {
    // OPTIMIZATION: A traverser's root never changes, so it is read once.
    let root = this.traverser.root();
    // The parentNode() method steps are:

    // 1. Let node be this’s current.
    let mut node = Some(this.current());

    // 2. While node is non-null and is not this’s root:
    while let Some(current) = node
        && current != root
    {
        // 1. Set node to node’s parent.
        node = current.parent();

        // 2. If node is non-null and filtering node within this returns FILTER_ACCEPT, then set this’s current to node
        //    and return node.
        if let Some(parent) = node
            && filter(main_thread, &this.traverser, this_filter, parent)? == FILTER_ACCEPT
        {
            this.set_current(parent);
            return Ok(Some(parent));
        }
    }

    // 3. Return null.
    Ok(None)
}

/// https://dom.spec.whatwg.org/#concept-traverse-children
fn traverse_children(
    main_thread: &mut MainThread<'_>,
    walker: &TreeWalkerCore,
    walker_filter: Filter,
    kind: ChildTraversalType,
) -> Result<Option<NodeRef>, TraversalError> {
    // OPTIMIZATION: A traverser's root never changes, so it is read once.
    let root = walker.traverser.root();
    let first_or_last_child = |node: NodeRef| match kind {
        ChildTraversalType::First => node.first_child(),
        ChildTraversalType::Last => node.last_child(),
    };
    let next_or_previous_sibling = |node: NodeRef| match kind {
        ChildTraversalType::First => node.next_sibling(),
        ChildTraversalType::Last => node.previous_sibling(),
    };

    // To traverse children, given a TreeWalker object walker and "first" or "last" type:

    // 1. Let node be walker’s current.
    let node = walker.current();

    // 2. Set node to node’s first child if type is "first"; otherwise to node’s last child.
    let mut node = first_or_last_child(node);

    // 3. While node is non-null:
    'outer: while let Some(current) = node {
        // 1. Let result be the result of filtering node within walker.
        let result = filter(main_thread, &walker.traverser, walker_filter, current)?;

        // 2. If result is FILTER_ACCEPT, then set walker’s current to node and return node.
        if result == FILTER_ACCEPT {
            walker.set_current(current);
            return Ok(Some(current));
        }

        // 3. If result is FILTER_SKIP:
        if result == FILTER_SKIP {
            // 1. Let child be node’s first child if type is "first"; otherwise node’s last child.
            let child = first_or_last_child(current);

            // 2. If child is non-null, then set node to child and continue.
            if child.is_some() {
                node = child;
                continue;
            }
        }

        // 4. While node is non-null:
        while let Some(current) = node {
            // 1. Let sibling be node’s next sibling if type is "first"; otherwise node’s previous sibling.
            let sibling = next_or_previous_sibling(current);

            // 2. If sibling is non-null, then set node to sibling and break.
            if sibling.is_some() {
                node = sibling;
                continue 'outer;
            }

            // 3. Let parent be node’s parent.
            let parent = current.parent();

            // 4. If parent is null, walker’s root, or walker’s current, then return null.
            let Some(parent) = parent else {
                return Ok(None);
            };
            if parent == root || parent == walker.current() {
                return Ok(None);
            }

            // 5. Set node to parent.
            node = Some(parent);
        }
    }

    // 4. Return null.
    Ok(None)
}

/// https://dom.spec.whatwg.org/#concept-traverse-siblings
fn traverse_siblings(
    main_thread: &mut MainThread<'_>,
    walker: &TreeWalkerCore,
    walker_filter: Filter,
    kind: SiblingTraversalType,
) -> Result<Option<NodeRef>, TraversalError> {
    // OPTIMIZATION: A traverser's root never changes, so it is read once.
    let root = walker.traverser.root();
    let next_or_previous_sibling = |node: NodeRef| match kind {
        SiblingTraversalType::Next => node.next_sibling(),
        SiblingTraversalType::Previous => node.previous_sibling(),
    };
    let first_or_last_child = |node: NodeRef| match kind {
        SiblingTraversalType::Next => node.first_child(),
        SiblingTraversalType::Previous => node.last_child(),
    };

    // To traverse siblings, given a TreeWalker object walker and "next" or "previous" type:

    // 1. Let node be walker’s current.
    let mut node = walker.current();

    // 2. If node is root, then return null.
    if node == root {
        return Ok(None);
    }

    // 3. While true:
    loop {
        // 1. Let sibling be node’s next sibling if type is "next"; otherwise node’s previous sibling.
        let mut sibling = next_or_previous_sibling(node);

        // 2. While sibling is non-null:
        while let Some(current) = sibling {
            // 1. Set node to sibling.
            node = current;

            // 2. Let result be the result of filtering node within walker.
            let result = filter(main_thread, &walker.traverser, walker_filter, node)?;

            // 3. If result is FILTER_ACCEPT, then set walker’s current to node and return node.
            if result == FILTER_ACCEPT {
                walker.set_current(node);
                return Ok(Some(node));
            }

            // 4. Set sibling to node’s first child if type is "next"; otherwise to node’s last child.
            sibling = first_or_last_child(node);

            // 5. If result is FILTER_REJECT or sibling is null, then set sibling to node’s next sibling if type is
            //    "next"; otherwise to node’s previous sibling.
            if result == FILTER_REJECT || sibling.is_none() {
                sibling = next_or_previous_sibling(node);
            }
        }

        // 3. Set node to node’s parent.
        let parent = node.parent();

        // 4. If node is null or walker’s root, then return null.
        let Some(parent) = parent else {
            return Ok(None);
        };
        if parent == root {
            return Ok(None);
        }
        node = parent;

        // 5. If the return value of filtering node within walker is FILTER_ACCEPT, then return null.
        if filter(main_thread, &walker.traverser, walker_filter, node)? == FILTER_ACCEPT {
            return Ok(None);
        }
    }
}

/// https://dom.spec.whatwg.org/#dom-treewalker-previousnode
fn previous_node(
    main_thread: &mut MainThread<'_>,
    this: &TreeWalkerCore,
    this_filter: Filter,
) -> Result<Option<NodeRef>, TraversalError> {
    // OPTIMIZATION: A traverser's root never changes, so it is read once.
    let root = this.traverser.root();
    // The previousNode() method steps are:

    // 1. Let node be this’s current.
    let mut node = this.current();

    // 2. While node is not this’s root:
    while node != root {
        // 1. Let sibling be node’s previous sibling.
        let mut sibling = node.previous_sibling();

        // 2. While sibling is non-null:
        while let Some(current) = sibling {
            // 1. Set node to sibling.
            node = current;

            // 2. Let result be the result of filtering node within this.
            let mut result = filter(main_thread, &this.traverser, this_filter, node)?;

            // 3. While result is not FILTER_REJECT and node has a child:
            while result != FILTER_REJECT
                && let Some(last_child) = node.last_child()
            {
                // 1. Set node to node’s last child.
                node = last_child;

                // 2. Set result to the result of filtering node within this.
                result = filter(main_thread, &this.traverser, this_filter, node)?;
            }

            // 4. If result is FILTER_ACCEPT, then set this’s current to node and return node.
            if result == FILTER_ACCEPT {
                this.set_current(node);
                return Ok(Some(node));
            }

            // 5. Set sibling to node’s previous sibling.
            sibling = node.previous_sibling();
        }

        // 3. If node is this’s root or node’s parent is null, then return null.
        if node == root {
            return Ok(None);
        }
        let Some(parent) = node.parent() else {
            return Ok(None);
        };

        // 4. Set node to node’s parent.
        node = parent;

        // 5. If the return value of filtering node within this is FILTER_ACCEPT, then set this’s current to node and
        //    return node.
        if filter(main_thread, &this.traverser, this_filter, node)? == FILTER_ACCEPT {
            this.set_current(node);
            return Ok(Some(node));
        }
    }

    // 3. Return null.
    Ok(None)
}

/// https://dom.spec.whatwg.org/#dom-treewalker-nextnode
fn next_node(
    main_thread: &mut MainThread<'_>,
    this: &TreeWalkerCore,
    this_filter: Filter,
) -> Result<Option<NodeRef>, TraversalError> {
    // OPTIMIZATION: A traverser's root never changes, so it is read once.
    let root = this.traverser.root();
    // The nextNode() method steps are:

    // 1. Let node be this’s current.
    let mut node = this.current();

    // 2. Let result be FILTER_ACCEPT.
    let mut result = FILTER_ACCEPT;

    // 3. While true:
    loop {
        // 1. While result is not FILTER_REJECT and node has a child:
        while result != FILTER_REJECT
            && let Some(first_child) = node.first_child()
        {
            // 1. Set node to its first child.
            node = first_child;

            // 2. Set result to the result of filtering node within this.
            result = filter(main_thread, &this.traverser, this_filter, node)?;

            // 3. If result is FILTER_ACCEPT, then set this’s current to node and return node.
            if result == FILTER_ACCEPT {
                this.set_current(node);
                return Ok(Some(node));
            }
        }

        // 2. Let sibling be null.
        let mut sibling = None;

        // 3. Let temporary be node.
        let mut temporary = Some(node);

        // 4. While temporary is non-null:
        while let Some(current) = temporary {
            // 1. If temporary is this’s root, then return null.
            if current == root {
                return Ok(None);
            }

            // 2. Set sibling to temporary’s next sibling.
            sibling = current.next_sibling();

            // 3. If sibling is non-null, then break.
            if sibling.is_some() {
                break;
            }

            // 4. Set temporary to temporary’s parent.
            temporary = current.parent();
        }

        // 5. If sibling is null, then return null.
        let Some(sibling) = sibling else {
            return Ok(None);
        };

        // 6. Set node to sibling.
        node = sibling;

        // 7. Set result to the result of filtering node within this.
        result = filter(main_thread, &this.traverser, this_filter, node)?;

        // 8. If result is FILTER_ACCEPT, then set this’s current to node and return node.
        if result == FILTER_ACCEPT {
            this.set_current(node);
            return Ok(Some(node));
        }
    }
}

/// Traverses `iterator` to its next or previous node, as nextNode() and previousNode() do, for DOM::NodeIterator, with
/// `filter`, its DOM::TraversalFilter, or null if the iterator's filter is null.
///
/// # Safety
///
/// The iterator must be live and kept alive by the caller, `filter` must be null or a live DOM::TraversalFilter, and
/// the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_node_iterator_traverse(
    iterator: &NodeIteratorCell,
    filter: *const c_void,
    direction: NodeIteratorDirection,
) -> TraversalResult {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    traverse(
        &mut main_thread,
        iterator.core(),
        NonNull::new(filter.cast_mut()),
        direction,
    )
    .into()
}

/// Runs `traversal` on `walker`, a TreeWalker method, for DOM::TreeWalker, with `filter`, its DOM::TraversalFilter, or
/// null if the walker's filter is null.
///
/// # Safety
///
/// The walker must be live and kept alive by the caller, `filter` must be null or a live DOM::TraversalFilter, and the
/// call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_tree_walker_traverse(
    walker: &TreeWalkerCell,
    filter: *const c_void,
    traversal: TreeWalkerTraversal,
) -> TraversalResult {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    let (main_thread, walker, filter) = (&mut main_thread, walker.core(), NonNull::new(filter.cast_mut()));
    match traversal {
        TreeWalkerTraversal::ParentNode => parent_node(main_thread, walker, filter),
        TreeWalkerTraversal::FirstChild => traverse_children(main_thread, walker, filter, ChildTraversalType::First),
        TreeWalkerTraversal::LastChild => traverse_children(main_thread, walker, filter, ChildTraversalType::Last),
        TreeWalkerTraversal::PreviousSibling => {
            traverse_siblings(main_thread, walker, filter, SiblingTraversalType::Previous)
        }
        TreeWalkerTraversal::NextSibling => traverse_siblings(main_thread, walker, filter, SiblingTraversalType::Next),
        TreeWalkerTraversal::PreviousNode => previous_node(main_thread, walker, filter),
        TreeWalkerTraversal::NextNode => next_node(main_thread, walker, filter),
    }
    .into()
}

#[cfg(test)]
mod tests {
    use super::super::test_tree::append_child;
    use super::super::test_tree::document_core;
    use super::super::test_tree::node;
    use super::super::test_tree::set_document;
    use super::*;
    use crate::dom::node_core::NodeType;
    use crate::gc_test_stubs::leaked_weak_impl;

    /// A DOM::NodeIterator for tests: its core, TRAVERSER_CORE_OFFSET bytes into the cell.
    #[repr(C)]
    struct TestNodeIteratorCell {
        header: [u8; TRAVERSER_CORE_OFFSET],
        core: NodeIteratorCore,
    }

    fn member(node: Option<Node<'_>>) -> NodeMember {
        let member = NodeMember::null_for_test();
        member.set(node.map(Node::pointer));
        member
    }

    /// A NodeIterator of `document` with `root` and `what_to_show`, registered with the document, which lives for the
    /// rest of the process.
    fn node_iterator<'a>(document: Node<'_>, root: Node<'_>, what_to_show: u32) -> &'a NodeIteratorCell {
        let cell = Box::leak(Box::new(TestNodeIteratorCell {
            header: [0; TRAVERSER_CORE_OFFSET],
            core: NodeIteratorCore {
                traverser: TraverserCore {
                    root: member(Some(root)),
                    what_to_show: Cell::new(what_to_show),
                    flags: Cell::new(NODE_ITERATOR_POINTER_BEFORE_REFERENCE),
                },
                reference: member(Some(root)),
                candidate_reference: member(None),
            },
        }));
        let weak = leaked_weak_impl((cell as *mut TestNodeIteratorCell).cast());
        let entries = Box::leak(Box::new([weak.cast_const().cast::<c_void>()]));
        document_core(document).node_iterators().set_for_test(entries);
        // SAFETY: A test cell starts with the iterator's cell, and lives for the rest of the process.
        unsafe { &*(cell as *const TestNodeIteratorCell as *const NodeIteratorCell) }
    }

    /// The nodes of a document with this tree, in tree order, each of which has the document as its node document:
    ///
    /// ```text
    /// document
    ///   html
    ///     head
    ///       comment
    ///     body
    ///       text
    /// ```
    fn document<'a>(main_thread: &'a MainThread<'_>) -> [Node<'a>; 6] {
        let new = |node_type| node(main_thread, node_type);
        let nodes = [
            new(NodeType::DOCUMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::COMMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::TEXT_NODE),
        ];
        let [document, html, head, comment, body, text] = nodes;
        append_child(head, comment);
        append_child(html, head);
        append_child(body, text);
        append_child(html, body);
        append_child(document, html);
        for node in &nodes[1..] {
            set_document(*node, document);
        }
        nodes
    }

    fn reference<'a>(main_thread: &'a MainThread<'_>, iterator: &NodeIteratorCell) -> (Node<'a>, bool) {
        let reference = iterator.core().reference();
        (reference.node.read(main_thread), reference.pointer_before)
    }

    /// A DOM::TreeWalker for tests: its core, TRAVERSER_CORE_OFFSET bytes into the cell.
    #[repr(C)]
    struct TestTreeWalkerCell {
        header: [u8; TRAVERSER_CORE_OFFSET],
        core: TreeWalkerCore,
    }

    /// A TreeWalker with `root` and `what_to_show`, which lives for the rest of the process.
    fn tree_walker<'a>(root: Node<'_>, what_to_show: u32) -> &'a TreeWalkerCore {
        let cell = Box::leak(Box::new(TestTreeWalkerCell {
            header: [0; TRAVERSER_CORE_OFFSET],
            core: TreeWalkerCore {
                traverser: TraverserCore {
                    root: member(Some(root)),
                    what_to_show: Cell::new(what_to_show),
                    flags: Cell::new(0),
                },
                current: member(Some(root)),
            },
        }));
        &cell.core
    }

    #[test]
    fn tree_walker_traversals_without_a_filter_move_between_shown_nodes() {
        let mut main_thread = MainThread::for_test();
        let main_thread = &mut main_thread;
        const SHOW_ELEMENT: u32 = 0x1;
        let (walker, [html, head, comment, body, text]) = {
            let [_, html, head, comment, body, text] = document(main_thread);
            (
                tree_walker(html, SHOW_ELEMENT),
                [html, head, comment, body, text].map(NodeRef::from),
            )
        };

        assert_eq!(next_node(main_thread, walker, None), Ok(Some(head)));
        assert_eq!(next_node(main_thread, walker, None), Ok(Some(body)));
        assert_eq!(next_node(main_thread, walker, None), Ok(None));
        assert_eq!(walker.current(), body);
        assert_eq!(previous_node(main_thread, walker, None), Ok(Some(head)));
        assert_eq!(previous_node(main_thread, walker, None), Ok(Some(html)));
        assert_eq!(previous_node(main_thread, walker, None), Ok(None));
        assert_eq!(parent_node(main_thread, walker, None), Ok(None));

        let first = ChildTraversalType::First;
        let last = ChildTraversalType::Last;
        assert_eq!(traverse_children(main_thread, walker, None, last), Ok(Some(body)));
        assert_eq!(traverse_children(main_thread, walker, None, first), Ok(None));
        let previous = SiblingTraversalType::Previous;
        let next = SiblingTraversalType::Next;
        assert_eq!(traverse_siblings(main_thread, walker, None, previous), Ok(Some(head)));
        assert_eq!(traverse_siblings(main_thread, walker, None, previous), Ok(None));
        assert_eq!(traverse_siblings(main_thread, walker, None, next), Ok(Some(body)));
        assert_eq!(parent_node(main_thread, walker, None), Ok(Some(html)));
        assert_eq!(traverse_children(main_thread, walker, None, first), Ok(Some(head)));

        walker.set_current(comment);
        assert_eq!(parent_node(main_thread, walker, None), Ok(Some(head)));
        walker.set_current(text);
        assert_eq!(traverse_siblings(main_thread, walker, None, previous), Ok(None));
        assert_eq!(previous_node(main_thread, walker, None), Ok(Some(body)));
    }

    #[test]
    fn filtering_an_active_traverser_throws() {
        let mut main_thread = MainThread::for_test();
        let main_thread = &mut main_thread;
        let (walker, head) = {
            let [_, html, head, ..] = document(main_thread);
            (tree_walker(html, u32::MAX), NodeRef::from(head))
        };
        walker.traverser.set_flag(TRAVERSER_IS_ACTIVE, true);
        assert_eq!(
            filter(main_thread, &walker.traverser, None, head),
            Err(TraversalError::AlreadyActive)
        );
        walker.traverser.set_flag(TRAVERSER_IS_ACTIVE, false);
        assert_eq!(filter(main_thread, &walker.traverser, None, head), Ok(FILTER_ACCEPT));
        walker.traverser.what_to_show.set(0);
        assert_eq!(filter(main_thread, &walker.traverser, None, head), Ok(FILTER_SKIP));
    }

    #[test]
    fn traversal_without_a_filter_walks_the_shown_nodes_in_tree_order() {
        let mut main_thread = MainThread::for_test();
        let main_thread = &mut main_thread;
        const SHOW_ELEMENT_AND_TEXT: u32 = 0x1 | 0x4;
        let (iterator, [html, head, body, text]) = {
            let [document, html, head, _, body, text] = document(main_thread);
            let iterator = node_iterator(document, html, SHOW_ELEMENT_AND_TEXT).core();
            (iterator, [html, head, body, text].map(NodeRef::from))
        };
        let mut step = |direction| traverse(main_thread, iterator, None, direction).unwrap();

        let next = NodeIteratorDirection::Next;
        let previous = NodeIteratorDirection::Previous;
        assert_eq!(step(next), Some(html));
        assert_eq!(step(next), Some(head));
        assert_eq!(step(next), Some(body));
        assert_eq!(step(next), Some(text));
        assert_eq!(step(next), None);
        assert_eq!(step(previous), Some(text));
        assert_eq!(step(previous), Some(body));
        assert_eq!(step(previous), Some(head));
        assert_eq!(step(previous), Some(html));
        assert_eq!(step(previous), None);
        assert!(iterator.candidate_reference().is_none());
    }

    #[test]
    fn pre_remove_steps_move_the_reference_out_of_the_removed_subtree() {
        let main_thread = MainThread::for_test();
        let [document, html, head, comment, body, text] = document(&main_thread);
        let iterator = node_iterator(document, html, u32::MAX);
        let set = |node: Node<'_>, pointer_before| {
            iterator.core().set_reference(NodePointerTuple {
                node: node.pointer(),
                pointer_before,
            })
        };

        set(comment, true);
        run_node_iterator_pre_remove_steps(&main_thread, NodeRef::from(head));
        assert_eq!(reference(&main_thread, iterator), (body, true));

        set(comment, false);
        run_node_iterator_pre_remove_steps(&main_thread, NodeRef::from(head));
        assert_eq!(reference(&main_thread, iterator), (html, false));

        set(text, true);
        run_node_iterator_pre_remove_steps(&main_thread, NodeRef::from(body));
        assert_eq!(reference(&main_thread, iterator), (comment, false));

        set(text, true);
        run_node_iterator_pre_remove_steps(&main_thread, NodeRef::from(head));
        assert_eq!(reference(&main_thread, iterator), (text, true));

        set(text, true);
        run_node_iterator_pre_remove_steps(&main_thread, NodeRef::from(html));
        assert_eq!(reference(&main_thread, iterator), (text, true));
    }
}

// NB: NodeIterator.cpp and TreeWalker.cpp assert the same layout of the structs they include.
const _: () = assert!(size_of::<TraverserCore>() == 12 && align_of::<TraverserCore>() == 4);
const _: () = assert!(offset_of!(TraverserCore, root) == 0);
const _: () = assert!(offset_of!(TraverserCore, what_to_show) == 4);
const _: () = assert!(offset_of!(TraverserCore, flags) == 8);
const _: () = assert!(size_of::<NodeIteratorCore>() == 20 && align_of::<NodeIteratorCore>() == 4);
const _: () = assert!(offset_of!(NodeIteratorCore, reference) == 12);
const _: () = assert!(offset_of!(NodeIteratorCore, candidate_reference) == 16);
const _: () = assert!(size_of::<TreeWalkerCore>() == 16 && align_of::<TreeWalkerCore>() == 4);
const _: () = assert!(offset_of!(TreeWalkerCore, current) == 12);
