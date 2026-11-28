/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The table of C++ callbacks through which the DOM algorithms in Rust run a node's virtual hooks, such as its insertion
//! and removing steps, and the steps of the systems whose state is still C++'s: live ranges, NodeIterators, slots,
//! mutation records, custom element reactions, the style engine's records, layout tree updates and collection caches.
//!
//! C++ registers the table once, before any node exists. Every wrapper takes the main thread token mutably, so no read
//! handle lives across a call into C++. The documentation of each wrapper says whether its callback can run script,
//! allocate or change the tree; code that calls one that can holds its nodes as `NodeRef`s and reads every link it
//! needs again after it returns.

use core::cell::UnsafeCell;

use super::node_ref::NodeRef;
use super::node_ref::NullableNodeRef;
use super::node_ref::RawNodeRef;
use super::node_ref::RawNullableNodeRef;
use crate::stage::MainThread;

/// Whether a node a removing step runs for is the root of the subtree that is removed, laid out as C++'s
/// DOM::Node::IsSubtreeRoot, which is this type.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IsSubtreeRoot {
    No,
    Yes,
}

/// Whether a change to the children of a node affects its element children, laid out as C++'s
/// DOM::Node::ChildrenChangedMetadata::AffectsElements, which is this type.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AffectsElements {
    No,
    Yes,
}

/// What inserts nodes, which decides the steps of inserting that the C++ side leaves out. Each kind has callbacks of its
/// own, see InsertionHooks, which the Rust side picks through an Inserter when it is compiled.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InsertionKind {
    /// The DOM's algorithms, which run every step.
    Dom,
    /// The HTML or XML parser, which inserts one node it has just created into a tree the style engine does not track.
    /// Script can hold such a tree, so observers, live ranges, collection caches and the post-connection steps are
    /// served, but the editing history, deferred style change events, the style engine's records, custom element
    /// reactions, layout tree marks and list numbering have nothing to do until the tree is tracked. The tree mutation
    /// record is queued only for an observer of tree mutations, DevTools or an assistive technology.
    Parser,
}

/// The kind of insertion code is compiled for, which picks the callbacks it calls.
pub(crate) trait Inserter {
    const KIND: InsertionKind;
}

/// See InsertionKind::Dom.
pub(crate) enum DomInserter {}

impl Inserter for DomInserter {
    const KIND: InsertionKind = InsertionKind::Dom;
}

/// See InsertionKind::Parser.
pub(crate) enum ParserInserter {}

impl Inserter for ParserInserter {
    const KIND: InsertionKind = InsertionKind::Parser;
}

/// What the C++ side of inserting nodes answers once the steps before the nodes are inserted have run.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct InsertionStart {
    /// Whether the insertion affects the element children of the parent.
    pub affects_elements: AffectsElements,
    /// The tree version the insertion steps start from.
    pub tree_version: u64,
}

/// What the C++ side of removing a node found before the node left its parent's children, which the steps after need.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RemovalFacts {
    pub was_connected: bool,
    pub was_tracked_by_style_engine: bool,
    /// Whether the subtree holds the focused or hovered node, whose state its old ancestors lose.
    pub holds_a_propagating_state_source: bool,
    /// Whether the removed node has an inclusive descendant that is a slot, in a parent whose root is a shadow root.
    pub has_descendant_slots: bool,
    /// Whether the removed node is assigned to a slot.
    pub is_assigned: bool,
}

/// What the C++ side of removing all children of a parent keeps across the removals, whose steps that concern only the
/// parent run once for all of them.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ChildrenRemoval {
    /// Whether the removals affect the element children of the parent, which is worked out only when
    /// `track_affected_elements` says a collection cache could care.
    pub affects_elements: AffectsElements,
    pub track_affected_elements: bool,
    /// Whether some child holds the focused or hovered node, whose state its old ancestors lose.
    pub a_child_holds_a_propagating_state_source: bool,
    /// Whether a removed child has scheduled the renumbering of its list owner's list, which every child shares.
    pub list_owner_renumber_scheduled: bool,
}

/// The callbacks of one kind of insertion, see InsertionKind.
#[repr(C)]
pub struct InsertionHooks {
    /// See [`begin_insertion`].
    pub begin_insertion: unsafe extern "C" fn(
        parent: RawNodeRef,
        node: RawNullableNodeRef,
        nodes: ak::Span<'_, NodeRef>,
        child: RawNullableNodeRef,
    ) -> InsertionStart,
    /// See [`insert_node_into_children`].
    pub insert_node_into_children: unsafe extern "C" fn(
        parent: RawNodeRef,
        node: RawNodeRef,
        child: RawNullableNodeRef,
        any_node_has_post_connection_steps: &mut bool,
    ),
    /// See [`run_insertion_steps`].
    pub run_insertion_steps: unsafe extern "C" fn(node: RawNodeRef) -> bool,
    /// See [`end_insertion`].
    pub end_insertion: unsafe extern "C" fn(
        parent: RawNodeRef,
        node: RawNodeRef,
        nodes: ak::Span<'_, NodeRef>,
        previous_sibling: RawNullableNodeRef,
        child: RawNullableNodeRef,
        suppress_observers: bool,
        affects_elements: AffectsElements,
    ) -> u64,
    /// See [`finish_insertion`].
    pub finish_insertion:
        unsafe extern "C" fn(parent: RawNodeRef, nodes: ak::Span<'_, NodeRef>, run_post_connection_steps: bool),
}

/// The callbacks, as DOM/Node.cpp registers them. Each takes the nodes it acts on as DOM::Node pointers, null only
/// where a RawNullableNodeRef may be.
#[repr(C)]
pub struct DomHooks {
    /// The callbacks of InsertionKind::Dom.
    pub insertion: InsertionHooks,
    /// The callbacks of InsertionKind::Parser.
    pub parser_insertion: InsertionHooks,
    /// See [`begin_removing_all_children`].
    pub begin_removing_all_children: unsafe extern "C" fn(parent: RawNodeRef) -> ChildrenRemoval,
    /// See [`end_removing_all_children`].
    pub end_removing_all_children: unsafe extern "C" fn(parent: RawNodeRef, children_removal: ChildrenRemoval),
    /// See [`prepare_removal`].
    pub prepare_removal: unsafe extern "C" fn(
        node: RawNodeRef,
        parent: RawNodeRef,
        children_removal: Option<&mut ChildrenRemoval>,
    ) -> RemovalFacts,
    /// See [`assign_slottables_after_removal`].
    pub assign_slottables_after_removal:
        unsafe extern "C" fn(node: RawNodeRef, parent: RawNodeRef, parent_root: RawNodeRef, facts: RemovalFacts),
    /// See [`finish_removal`].
    pub finish_removal: unsafe extern "C" fn(
        node: RawNodeRef,
        parent: RawNodeRef,
        old_previous_sibling: RawNullableNodeRef,
        old_next_sibling: RawNullableNodeRef,
        suppress_observers: bool,
        is_one_of_all_children: bool,
    ),
    /// See [`run_removing_steps`].
    pub run_removing_steps: unsafe extern "C" fn(
        node: RawNodeRef,
        is_subtree_root: IsSubtreeRoot,
        parent: RawNodeRef,
        parent_root: RawNodeRef,
        was_tracked_by_style_engine: bool,
        is_parent_connected: bool,
    ),
    /// See [`adopt_into_document_of`].
    pub adopt_into_document_of: unsafe extern "C" fn(node: RawNodeRef, parent: RawNodeRef),
    /// See [`queue_tree_mutation_record`].
    pub queue_tree_mutation_record: unsafe extern "C" fn(
        target: RawNodeRef,
        added_nodes: ak::Span<'_, NodeRef>,
        removed_nodes: ak::Span<'_, NodeRef>,
        previous_sibling: RawNullableNodeRef,
        next_sibling: RawNullableNodeRef,
    ),
}

/// The registered table, which C++ sets once, before any node exists, and never changes after that.
#[repr(transparent)]
struct RegisteredHooks(UnsafeCell<Option<&'static DomHooks>>);

// SAFETY: The table is set once, before any node exists, and only the main thread, which made every node since, reads
//         it.
unsafe impl Sync for RegisteredHooks {}

static HOOKS: RegisteredHooks = RegisteredHooks(UnsafeCell::new(None));

#[inline(always)]
fn insertion_hooks<I: Inserter>(main_thread: &mut MainThread<'_>) -> &'static InsertionHooks {
    let hooks = hooks(main_thread);
    match I::KIND {
        InsertionKind::Dom => &hooks.insertion,
        InsertionKind::Parser => &hooks.parser_insertion,
    }
}

fn hooks(_main_thread: &mut MainThread<'_>) -> &'static DomHooks {
    // SAFETY: Nothing writes the table after the first node exists, see RegisteredHooks.
    unsafe { *HOOKS.0.get() }.expect("the DOM hooks are registered before any node exists")
}

/// Registers the DOM's callbacks.
///
/// # Safety
///
/// No node may exist yet, and the call must be made on the main thread, before any other thread starts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_register_hooks(hooks: &'static DomHooks) {
    // SAFETY: Guaranteed by the caller.
    unsafe { *HOOKS.0.get() = Some(hooks) };
}

/// Step 4.2 and step 5 of inserting `node`, whose nodes are `nodes`, into `parent` before `child`, after step 4.1 has
/// emptied a DocumentFragment: if `node` is a DocumentFragment, queues a tree mutation record for it with « », `nodes`,
/// null and null, and if `child` is non-null, increases the offsets of the live ranges in `parent` past `child`'s index
/// by the count of `nodes`. Before them, tells the editing history of the mutation and consumes the deferred style
/// change events of the documents. Answers whether the insertion affects the element children of `parent`, and the
/// tree version after the steps.
///
/// `node` is null for nodes that are inserted as the children of a DocumentFragment no one can observe, which is not
/// made. `I` picks the callbacks, which leave out what its InsertionKind says, as do the ones below.
///
/// Can run script, allocate and change the tree.
pub(crate) fn begin_insertion<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    node: Option<NodeRef>,
    nodes: &[NodeRef],
    child: Option<NodeRef>,
) -> InsertionStart {
    // SAFETY: The callback is called on the main thread with live nodes, which the caller roots.
    unsafe {
        (insertion_hooks::<I>(main_thread).begin_insertion)(parent, node.into(), ak::Span::new(nodes), child.into())
    }
}

/// Steps 7.1 to 7.6 of inserting nodes for `node`: adopts it into `parent`'s node document, inserts it into `parent`'s
/// children before `child`, or appends it if `child` is null, and assigns the slots and slottables that changes. Then
/// records the arrival of the subtree for the style engine, and runs step 7.7 through rust_dom_run_insertion_steps(),
/// inside a C++ scope that serves the insertion steps of the subtree.
///
/// NB: Node::append_child_impl() leaves out a node that is not allowed as a child of `parent`.
///
/// Can run script, allocate and change the tree.
pub(crate) fn insert_node_into_children<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    node: NodeRef,
    child: Option<NodeRef>,
    any_node_has_post_connection_steps: &mut bool,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe {
        (insertion_hooks::<I>(main_thread).insert_node_into_children)(
            parent,
            node,
            child.into(),
            any_node_has_post_connection_steps,
        );
    }
}

/// Runs the insertion steps with `node`, then, if it is connected, step 7.7.1 or 7.7.2 of inserting nodes for it.
/// Answers whether `node` has post-connection steps.
///
/// Can run script, allocate and change the tree.
pub(crate) fn run_insertion_steps<I: Inserter>(main_thread: &mut MainThread<'_>, node: NodeRef) -> bool {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (insertion_hooks::<I>(main_thread).run_insertion_steps)(node) }
}

/// Steps 8 and 9 of inserting `node`, whose nodes are `nodes`, into `parent` before `child`: queues the tree mutation
/// record unless `suppress_observers`, and runs the children changed steps for `parent`. Then invalidates the
/// collection caches the insertion changed, and marks the layout tree for the inserted nodes. Answers the current tree
/// version.
///
/// Can run script, allocate and change the tree.
#[allow(clippy::too_many_arguments)]
pub(crate) fn end_insertion<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    node: NodeRef,
    nodes: &[NodeRef],
    previous_sibling: Option<NodeRef>,
    child: Option<NodeRef>,
    suppress_observers: bool,
    affects_elements: AffectsElements,
) -> u64 {
    // SAFETY: The callback is called on the main thread with live nodes, which the caller roots.
    unsafe {
        (insertion_hooks::<I>(main_thread).end_insertion)(
            parent,
            node,
            ak::Span::new(nodes),
            previous_sibling.into(),
            child.into(),
            suppress_observers,
            affects_elements,
        )
    }
}

/// The last steps of inserting `nodes` into `parent`: if `run_post_connection_steps`, steps 10 to 12, which run the
/// post-connection steps of the connected shadow-including inclusive descendants of `nodes` that have some. Then
/// schedules the list item renumbering the insertion causes, and moves the tree version of `parent`.
///
/// Can run script, allocate and change the tree.
pub(crate) fn finish_insertion<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    nodes: &[NodeRef],
    run_post_connection_steps: bool,
) {
    // SAFETY: The callback is called on the main thread with live nodes, which the caller roots.
    unsafe {
        (insertion_hooks::<I>(main_thread).finish_insertion)(parent, ak::Span::new(nodes), run_post_connection_steps);
    }
}

/// The steps of removing all children of `parent` that concern only `parent`, and so run once before the first child
/// is removed: tells the editing history of the mutation, consumes the deferred style change events of the document,
/// and runs the live range pre-remove steps for every child at once, which leaves each boundary point that was in a
/// child or on `parent` at (`parent`, 0). Answers what the removals keep across them.
///
/// Allocates. Runs no script, and leaves the tree as it is.
pub(crate) fn begin_removing_all_children(main_thread: &mut MainThread<'_>, parent: NodeRef) -> ChildrenRemoval {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).begin_removing_all_children)(parent) }
}

/// The steps of removing all children of `parent` that concern only `parent`, and so run once after the last child is
/// removed: runs the children changed steps for `parent`, and invalidates the collection caches and the tree version
/// of `parent`.
///
/// Can run script, allocate and change the tree.
pub(crate) fn end_removing_all_children(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    children_removal: ChildrenRemoval,
) {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).end_removing_all_children)(parent, children_removal) }
}

/// The steps of removing `node` from `parent` that come before the node leaves its parent's children: tells the
/// editing history, the document and the page of the removal, runs the live range pre-remove steps and the NodeIterator
/// pre-remove steps, schedules the list item renumbering the removal causes, settles the slot assignments of a shadow
/// tree the subtree leaves, records the removal for layout and the style engine, and takes the node out of the C++
/// state that follows the children of `parent`: the animation counts, the child index generation and the child indices.
///
/// For one of all the children of `parent`, which `children_removal` tracks, the steps that
/// begin_removing_all_children() ran for every child are left out, and the facts every child shares are read from and
/// kept in `children_removal`.
///
/// Allocates. Runs no script, and leaves the tree as it is.
pub(crate) fn prepare_removal(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    children_removal: Option<&mut ChildrenRemoval>,
) -> RemovalFacts {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).prepare_removal)(node, parent, children_removal) }
}

/// The steps of removing `node` from `parent`, whose root is `parent_root`, that assign the slottables a removal
/// changes, after the node has left its parent's children. Then pins the style records of the boxes of a subtree that
/// was not connected, and takes the state of the focused or hovered node out of the old ancestors of a subtree that
/// held it.
///
/// Allocates. Runs no script, and leaves the tree as it is.
pub(crate) fn assign_slottables_after_removal(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    parent_root: NodeRef,
    facts: RemovalFacts,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).assign_slottables_after_removal)(node, parent, parent_root, facts) }
}

/// The last steps of removing `node` from `parent`: adds the transient registered observers, queues the tree mutation
/// record unless `suppress_observers`, runs the children changed steps for `parent`, and invalidates the collection
/// caches and tree versions the removal changed.
///
/// For one of all the children of `parent`, `is_one_of_all_children` leaves out the steps for `parent`, which
/// end_removing_all_children() runs once.
///
/// Can run script, allocate and change the tree.
pub(crate) fn finish_removal(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    old_previous_sibling: Option<NodeRef>,
    old_next_sibling: Option<NodeRef>,
    suppress_observers: bool,
    is_one_of_all_children: bool,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe {
        (hooks(main_thread).finish_removal)(
            node,
            parent,
            old_previous_sibling.into(),
            old_next_sibling.into(),
            suppress_observers,
            is_one_of_all_children,
        );
    }
}

/// Runs the removing steps with `node`, `is_subtree_root` and `parent`, as Node::removed_from() does, after cancelling
/// the CSS animations and transitions of an element the style engine tracked. Then, if `is_parent_connected`, enqueues
/// a "disconnectedCallback" custom element callback reaction for `node` if it is custom. For the root of the removed
/// subtree, `is_parent_connected` is ignored: the callback reads whether `parent` is connected after the root's
/// removing steps.
/// https://dom.spec.whatwg.org/#concept-node-remove-ext
///
/// Can run script, allocate and change the tree.
#[allow(clippy::too_many_arguments)]
pub(crate) fn run_removing_steps(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    is_subtree_root: IsSubtreeRoot,
    parent: NodeRef,
    parent_root: NodeRef,
    was_tracked_by_style_engine: bool,
    is_parent_connected: bool,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe {
        (hooks(main_thread).run_removing_steps)(
            node,
            is_subtree_root,
            parent,
            parent_root,
            was_tracked_by_style_engine,
            is_parent_connected,
        );
    }
}

/// Adopts `node` into `parent`'s node document.
/// https://dom.spec.whatwg.org/#concept-node-adopt
///
/// Can run script, allocate and change the tree.
pub(crate) fn adopt_into_document_of(main_thread: &mut MainThread<'_>, node: NodeRef, parent: NodeRef) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).adopt_into_document_of)(node, parent) }
}

/// Queues a tree mutation record for `target` with `added_nodes`, `removed_nodes`, `previous_sibling` and
/// `next_sibling`, one of whose lists is not empty.
/// https://dom.spec.whatwg.org/#queue-a-tree-mutation-record
///
/// Allocates. Runs no script, and leaves the tree as it is.
pub(crate) fn queue_tree_mutation_record(
    main_thread: &mut MainThread<'_>,
    target: NodeRef,
    added_nodes: &[NodeRef],
    removed_nodes: &[NodeRef],
    previous_sibling: Option<NodeRef>,
    next_sibling: Option<NodeRef>,
) {
    // SAFETY: The callback is called on the main thread with live nodes, which the caller roots.
    unsafe {
        (hooks(main_thread).queue_tree_mutation_record)(
            target,
            ak::Span::new(added_nodes),
            ak::Span::new(removed_nodes),
            previous_sibling.into(),
            next_sibling.into(),
        );
    }
}
