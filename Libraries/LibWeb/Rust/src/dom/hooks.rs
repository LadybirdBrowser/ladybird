/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The table of C++ callbacks through which the DOM algorithms in Rust run a node's virtual hooks, such as its insertion
//! and removing steps, and the steps of the systems whose state C++ keeps: the selections and highlights of live
//! ranges, the filters of traversers and collections, the effects of slot assignments, mutation records, custom element
//! reactions, the style engine's records and layout tree updates.
//!
//! C++ registers the table once, before any node exists. Every wrapper takes the main thread token mutably, so no read
//! handle lives across a call into C++. The documentation of each wrapper says whether its callback can run script,
//! allocate or change the tree; code that calls one that can holds its nodes as `NodeRef`s and reads every link it
//! needs again after it returns.

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::ptr::NonNull;

use super::collection::CollectionCell;
use super::mutation_observers::has_observed_inclusive_ancestor;
use super::mutation_observers::tree_mutation_record_is_heard;
use super::node_ref::NodeRef;
use super::node_ref::NullableNodeRef;
use super::node_ref::RawNodeRef;
use super::node_ref::RawNullableNodeRef;
use super::qualified_name::QualifiedNameImpl;
use super::range::RangeCell;
use super::traversal::NodeFilterCall;
use crate::stage::MainThread;

/// Whether a node a removing step runs for is the root of the subtree that is removed, laid out as C++'s
/// DOM::Node::IsSubtreeRoot, which is this type.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IsSubtreeRoot {
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

/// What the C++ side of removing a node found before the node left its parent's children, which the steps after need.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RemovalFacts {
    pub was_connected: bool,
    pub was_tracked_by_style_engine: bool,
    /// Whether the subtree holds the focused or hovered node, whose state its old ancestors lose.
    pub holds_a_propagating_state_source: bool,
}

/// What the C++ side of removing all children of a parent keeps across the removals, whose steps that concern only the
/// parent run once for all of them.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ChildrenRemoval {
    /// Whether some child holds the focused or hovered node, whose state its old ancestors lose.
    pub a_child_holds_a_propagating_state_source: bool,
    /// Whether a removed child has scheduled the renumbering of its list owner's list, which every child shares.
    pub list_owner_renumber_scheduled: bool,
}

/// What the C++ side of moving a node keeps across the move, opaque to Rust: DOM::MoveScope, which lives in the C++ frame
/// below the Rust code that moves the node, and so roots the nodes it holds for as long as the move takes.
#[repr(C)]
pub struct MoveScope {
    _private: [u8; 0],
    // NB: The marker makes the scope neither Send, Sync nor Unpin, as C++ owns it.
    _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
}

/// What the C++ side of cloning a node keeps across the clone, opaque to Rust: DOM::CloneScope, which lives in the C++
/// frame below the Rust code that clones the node. It holds the document and the fallback registry the copies are made
/// with, and the exception a callback threw.
#[repr(C)]
pub struct CloneScope {
    _private: [u8; 0],
    // NB: The marker makes the scope neither Send, Sync nor Unpin, as C++ owns it.
    _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
}

/// The callbacks of cloning a node.
#[repr(C)]
pub struct CloneHooks {
    /// See [`clone_single_node`].
    pub clone_single_node: unsafe extern "C" fn(
        scope: &mut CloneScope,
        node: RawNodeRef,
        uses_fallback_registry: bool,
        subtree: bool,
        copy: &mut NullableNodeRef,
    ) -> bool,
    /// See [`run_element_cloning_steps`].
    pub run_element_cloning_steps:
        unsafe extern "C" fn(scope: &mut CloneScope, node: RawNodeRef, copy: RawNodeRef, subtree: bool) -> bool,
    /// See [`attach_shadow_root_copy`].
    pub attach_shadow_root_copy: unsafe extern "C" fn(
        scope: &mut CloneScope,
        node: RawNodeRef,
        copy: RawNodeRef,
        shadow_root_copy: &mut NullableNodeRef,
    ) -> bool,
}

/// The callbacks of moving a node.
#[repr(C)]
pub struct MoveHooks {
    /// See [`begin_move`].
    pub begin_move: unsafe extern "C" fn(
        scope: &mut MoveScope,
        node: RawNodeRef,
        old_parent: RawNodeRef,
        old_parent_root: RawNodeRef,
        new_parent: RawNodeRef,
    ),
    /// See [`after_move_unlink`].
    pub after_unlink: unsafe extern "C" fn(node: RawNodeRef, old_parent: RawNodeRef),
    /// See [`after_move_link`].
    pub after_link: unsafe extern "C" fn(
        scope: &mut MoveScope,
        node: RawNodeRef,
        old_parent: RawNodeRef,
        old_parent_root: RawNodeRef,
        new_parent: RawNodeRef,
    ),
    /// See [`run_moving_steps`].
    pub run_moving_steps: unsafe extern "C" fn(
        node: RawNodeRef,
        is_subtree_root: IsSubtreeRoot,
        old_parent: RawNodeRef,
        new_parent: RawNodeRef,
    ),
    /// See [`end_move`].
    pub end_move: unsafe extern "C" fn(
        scope: &mut MoveScope,
        node: RawNodeRef,
        old_parent: RawNodeRef,
        new_parent: RawNodeRef,
        old_previous_sibling: RawNullableNodeRef,
        old_next_sibling: RawNullableNodeRef,
        new_previous_sibling: RawNullableNodeRef,
        child: RawNullableNodeRef,
    ),
}

/// The callbacks of slots, which fire only where a slot assignment changes.
#[repr(C)]
pub struct SlotHooks {
    /// See [`assigned_slot_changed`].
    pub assigned_slot_changed:
        unsafe extern "C" fn(slottable: RawNodeRef, old_slot: RawNullableNodeRef, new_slot: RawNullableNodeRef),
    /// See [`slot_changed`].
    pub slot_changed: unsafe extern "C" fn(slot: RawNodeRef, steps: u8),
    /// See [`shadow_root_needs_slot_assignment`].
    pub shadow_root_needs_slot_assignment: unsafe extern "C" fn(shadow_root: RawNodeRef),
}

// What the slot_changed hook runs, a bit each in its steps byte.

/// Append the slot to its relevant agent's signal slots, and queue a mutation observer microtask.
/// https://dom.spec.whatwg.org/#signal-a-slot-change
pub const SLOT_CHANGE_SIGNAL: u8 = 1 << 0;
/// Publish the slot's assigned nodes to the style engine.
pub const SLOT_CHANGE_RECORD_ASSIGNED_NODES: u8 = 1 << 1;
/// Invalidate the layout tree and the style that the slot's assignment changes.
pub const SLOT_CHANGE_INVALIDATE: u8 = 1 << 2;

/// The callbacks through which the maps of elements by ID hold their elements weakly, see id_map.rs.
#[repr(C)]
pub struct IdMapHooks {
    /// See [`create_weak`].
    pub create_weak: unsafe extern "C" fn(node: NodeRef) -> NonNull<c_void>,
    /// See [`release_weak`].
    pub release_weak: unsafe extern "C" fn(weak: NonNull<c_void>),
}

/// The callbacks of one kind of insertion, see InsertionKind.
#[repr(C)]
pub struct InsertionHooks {
    /// See [`begin_insertion`].
    pub begin_insertion:
        unsafe extern "C" fn(parent: RawNodeRef, node: RawNullableNodeRef, nodes: ak::Span<'_, NodeRef>),
    /// See [`inserted_into_children`].
    pub inserted_into_children:
        unsafe extern "C" fn(parent: RawNodeRef, node: RawNodeRef, any_node_has_post_connection_steps: &mut bool),
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
    ),
    /// See [`finish_insertion`].
    pub finish_insertion: unsafe extern "C" fn(nodes: ak::Span<'_, NodeRef>, run_post_connection_steps: bool),
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
    pub end_removing_all_children: unsafe extern "C" fn(parent: RawNodeRef),
    /// See [`prepare_removal`].
    pub prepare_removal: unsafe extern "C" fn(
        node: RawNodeRef,
        parent: RawNodeRef,
        children_removal: Option<&mut ChildrenRemoval>,
    ) -> RemovalFacts,
    /// See [`after_removal_unlink`].
    pub after_removal_unlink: unsafe extern "C" fn(
        node: RawNodeRef,
        parent: RawNodeRef,
        old_assigned_slot: RawNullableNodeRef,
        facts: RemovalFacts,
    ),
    /// See [`finish_removal`].
    pub finish_removal: unsafe extern "C" fn(
        node: RawNodeRef,
        parent: RawNodeRef,
        old_previous_sibling: RawNullableNodeRef,
        old_next_sibling: RawNullableNodeRef,
        parent_is_observed: bool,
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
    /// See [`adopt_from_other_document`].
    pub adopt_from_other_document:
        unsafe extern "C" fn(node: RawNodeRef, old_document: RawNodeRef, document: RawNodeRef),
    /// The callbacks of moving a node.
    pub moving: MoveHooks,
    /// The callbacks of cloning a node.
    pub cloning: CloneHooks,
    /// The callbacks of slots.
    pub slots: SlotHooks,
    /// The callbacks of the maps of elements by ID.
    pub id_map: IdMapHooks,
    /// See [`live_range_boundary_points_set`].
    pub live_range_boundary_points_set: unsafe extern "C" fn(document: RawNodeRef),
    /// See [`call_node_filter`].
    pub call_node_filter: unsafe extern "C" fn(filter: *const c_void, node: RawNodeRef) -> NodeFilterCall,
    /// See [`collection_filter_matches`].
    pub collection_filter_matches: unsafe extern "C" fn(collection: &CollectionCell, node: RawNodeRef) -> bool,
    /// See [`queue_tree_mutation_record`].
    pub queue_tree_mutation_record: unsafe extern "C" fn(
        target: RawNodeRef,
        added_nodes: ak::Span<'_, NodeRef>,
        removed_nodes: ak::Span<'_, NodeRef>,
        previous_sibling: RawNullableNodeRef,
        next_sibling: RawNullableNodeRef,
    ),
    /// Creates an attribute whose local name, namespace prefix and namespace are the strings whose raw words are
    /// `local_name`, `prefix` and `namespace_uri` (0 for null), and whose value is the string `value` gives a reference
    /// to, and appends it to the element's attribute list. Allocates, and runs no script.
    pub append_attribute:
        unsafe extern "C" fn(element: RawNodeRef, local_name: usize, prefix: usize, namespace_uri: usize, value: usize),
    /// Makes room in the element's attribute list for `count` more attributes. Allocates, and runs no script.
    pub reserve_attributes: unsafe extern "C" fn(element: RawNodeRef, count: usize),
    /// Runs the steps of handling attribute changes that C++ owns, which `steps` names (see ATTRIBUTE_CHANGE_*),
    /// for the attribute named `name` of the element, whose old and new values are the strings whose raw words are
    /// `old_value` and `new_value`. Can run script.
    pub attribute_changed: unsafe extern "C" fn(
        element: RawNodeRef,
        name: NonNull<QualifiedNameImpl>,
        old_value: usize,
        new_value: usize,
        steps: u8,
    ),
    /// Writes the declarations of the element's inline style to its style attribute. Runs no script.
    pub synchronize_style_attribute: unsafe extern "C" fn(element: RawNodeRef),
    /// Drops the last reference to a qualified name's parts, which frees them. Runs no script.
    pub release_qualified_name: unsafe extern "C" fn(name: NonNull<QualifiedNameImpl>),
    /// Runs what replacing `count` code units from `offset` of the data of a CharacterData node with data
    /// `data_length` code units long does beyond the data and the live ranges, which `steps` names (see
    /// CHARACTER_DATA_*). `old_data` is the raw word of the data before. Can run script.
    pub character_data_replaced: unsafe extern "C" fn(
        node: RawNodeRef,
        old_data: usize,
        offset: usize,
        count: usize,
        data_length: usize,
        steps: u8,
    ),
    /// Whether the nodes `a` and `b`, two doctypes, processing instructions or Attr nodes, have equal state of the
    /// kind the "equals" algorithm compares for their interface. Runs no script.
    pub node_state_equals: unsafe extern "C" fn(a: RawNodeRef, b: RawNodeRef) -> bool,
    /// The range of the document's selection, or null. Runs no script.
    pub selection_range: unsafe extern "C" fn(document: RawNodeRef) -> Option<NonNull<RangeCell>>,
}

// What the character_data_replaced hook runs, a bit each in its steps byte.

/// Tell the document's editing history of the mutation.
pub const CHARACTER_DATA_NOTIFY_EDITING_HISTORY: u8 = 1 << 0;
/// Queue a mutation record of "characterData".
pub const CHARACTER_DATA_QUEUE_MUTATION_RECORD: u8 = 1 << 1;
/// Run the children changed steps for the node's parent.
pub const CHARACTER_DATA_RUN_CHILDREN_CHANGED_STEPS: u8 = 1 << 2;
/// The data is not what it was, so its spelling markers, its style, its layout and its version move.
pub const CHARACTER_DATA_DATA_CHANGED: u8 = 1 << 3;

// The steps of handling attribute changes that the attribute_changed hook runs, a bit each in its steps byte, and what
// the values it is given are.

/// The old value is null.
pub const ATTRIBUTE_CHANGE_OLD_VALUE_IS_NULL: u8 = 1 << 0;
/// The new value is null.
pub const ATTRIBUTE_CHANGE_NEW_VALUE_IS_NULL: u8 = 1 << 1;
/// Set the element of the removed attribute's Attr node to null.
pub const ATTRIBUTE_CHANGE_DETACH_ATTRIBUTE_NODE: u8 = 1 << 2;
/// Tell the document's editing history of the mutation.
pub const ATTRIBUTE_CHANGE_NOTIFY_EDITING_HISTORY: u8 = 1 << 3;
/// Queue a mutation record of "attributes".
pub const ATTRIBUTE_CHANGE_QUEUE_MUTATION_RECORD: u8 = 1 << 4;
/// Enqueue a custom element callback reaction with callback name "attributeChangedCallback".
pub const ATTRIBUTE_CHANGE_ENQUEUE_CUSTOM_ELEMENT_REACTION: u8 = 1 << 5;
/// Run the attribute change steps, but for what attributes.rs does after the hook returns.
pub const ATTRIBUTE_CHANGE_RUN_ATTRIBUTE_CHANGE_STEPS: u8 = 1 << 6;
/// Tell the style engine of the change, as the attribute change steps do, where no attribute change steps act on it.
pub const ATTRIBUTE_CHANGE_TELL_STYLE_ENGINE: u8 = 1 << 7;
/// The bits that name steps to run, rather than what the values are.
pub const ATTRIBUTE_CHANGE_STEPS: u8 = !(ATTRIBUTE_CHANGE_OLD_VALUE_IS_NULL | ATTRIBUTE_CHANGE_NEW_VALUE_IS_NULL);

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

#[inline]
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

/// Step 4.2 of inserting `node`, whose nodes are `nodes`, into `parent`, after step 4.1 has emptied a
/// DocumentFragment: if `node` is a DocumentFragment, queues a tree mutation record for it with « », `nodes`, null and
/// null. Before it, tells the editing history of the mutation and consumes the deferred style change events of the
/// documents.
///
/// `node` is null for nodes that are inserted as the children of a DocumentFragment no one can observe, which is not
/// made. `I` picks the callbacks, which leave out what its InsertionKind says, as do the ones below.
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn begin_insertion<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    node: Option<NodeRef>,
    nodes: &[NodeRef],
) {
    // SAFETY: The callback is called on the main thread with live nodes, which the caller roots.
    unsafe { (insertion_hooks::<I>(main_thread).begin_insertion)(parent, node.into(), ak::Span::new(nodes)) }
}

/// What inserting nodes does for `node` after steps 7.2 to 7.6, which have inserted it into `parent`'s children, or not
/// where `parent` does not allow it: puts it in the C++ state that follows the children of `parent`,
/// records the arrival of the subtree for the style engine, and runs step 7.7 through rust_dom_run_insertion_steps(),
/// inside a C++ scope that serves the insertion steps of the subtree.
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn inserted_into_children<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    node: NodeRef,
    any_node_has_post_connection_steps: &mut bool,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe {
        (insertion_hooks::<I>(main_thread).inserted_into_children)(parent, node, any_node_has_post_connection_steps);
    }
}

/// Runs the insertion steps with `node`, then, if it is connected, step 7.7.1 or 7.7.2 of inserting nodes for it.
/// Answers whether `node` has post-connection steps.
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn run_insertion_steps<I: Inserter>(main_thread: &mut MainThread<'_>, node: NodeRef) -> bool {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (insertion_hooks::<I>(main_thread).run_insertion_steps)(node) }
}

/// Steps 8 and 9 of inserting `node`, whose nodes are `nodes`, into `parent` before `child`: queues the tree mutation
/// record unless `suppress_observers`, and runs the children changed steps for `parent`. Then marks the layout tree for
/// the inserted nodes.
///
/// Can run script, allocate and change the tree.
#[allow(clippy::too_many_arguments)]
#[inline]
pub(crate) fn end_insertion<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    parent: NodeRef,
    node: NodeRef,
    nodes: &[NodeRef],
    previous_sibling: Option<NodeRef>,
    child: Option<NodeRef>,
    suppress_observers: bool,
) {
    // OPTIMIZATION: A record no one hears is not queued, see mutation_observers.rs.
    let suppress_observers = suppress_observers || !tree_mutation_record_is_heard(parent);
    // SAFETY: The callback is called on the main thread with live nodes, which the caller roots.
    unsafe {
        (insertion_hooks::<I>(main_thread).end_insertion)(
            parent,
            node,
            ak::Span::new(nodes),
            previous_sibling.into(),
            child.into(),
            suppress_observers,
        );
    }
}

/// The last steps of inserting `nodes`: if `run_post_connection_steps`, steps 10 to 12, which run the post-connection
/// steps of the connected shadow-including inclusive descendants of `nodes` that have some. Then schedules the list
/// item renumbering the insertion causes.
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn finish_insertion<I: Inserter>(
    main_thread: &mut MainThread<'_>,
    nodes: &[NodeRef],
    run_post_connection_steps: bool,
) {
    // SAFETY: The callback is called on the main thread with live nodes, which the caller roots.
    unsafe {
        (insertion_hooks::<I>(main_thread).finish_insertion)(ak::Span::new(nodes), run_post_connection_steps);
    }
}

/// The steps of removing all children of `parent` that concern only `parent`, and so run once before the first child
/// is removed: tells the editing history of the mutation and consumes the deferred style change events of the document.
/// Answers what the removals keep across them.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn begin_removing_all_children(main_thread: &mut MainThread<'_>, parent: NodeRef) -> ChildrenRemoval {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).begin_removing_all_children)(parent) }
}

/// The steps of removing all children of `parent` that concern only `parent`, and so run once after the last child is
/// removed: runs the children changed steps for `parent`.
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn end_removing_all_children(main_thread: &mut MainThread<'_>, parent: NodeRef) {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).end_removing_all_children)(parent) }
}

/// The steps of removing `node` from `parent` that come before the node leaves its parent's children, but for the live
/// range and NodeIterator pre-remove steps: tells the editing history, the document and the page of the removal,
/// schedules the list item renumbering the removal causes, records the removal for layout and the style engine, and
/// takes the node out of the animation counts of `parent`.
///
/// For one of all the children of `parent`, which `children_removal` tracks, the steps that
/// begin_removing_all_children() ran for every child are left out, and the facts every child shares are read from and
/// kept in `children_removal`.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn prepare_removal(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    children_removal: Option<&mut ChildrenRemoval>,
) -> RemovalFacts {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).prepare_removal)(node, parent, children_removal) }
}

/// The steps of removing `node` from `parent` after it has left its parent's children and the slottables have been
/// assigned: pins the style records of the boxes of a subtree that was not connected, and takes the state of the focused
/// or hovered node out of the old ancestors of a subtree that held it, which start at `old_assigned_slot`, the slot the
/// node was assigned to, where it was an element assigned to one.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn after_removal_unlink(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    old_assigned_slot: Option<NodeRef>,
    facts: RemovalFacts,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).after_removal_unlink)(node, parent, old_assigned_slot.into(), facts) }
}

/// The last steps of removing `node` from `parent`: adds the transient registered observers, queues the tree mutation
/// record unless `suppress_observers`, and runs the children changed steps for `parent`.
///
/// For one of all the children of `parent`, `is_one_of_all_children` leaves out the steps for `parent`, which
/// end_removing_all_children() runs once.
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn finish_removal(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    parent: NodeRef,
    old_previous_sibling: Option<NodeRef>,
    old_next_sibling: Option<NodeRef>,
    suppress_observers: bool,
    is_one_of_all_children: bool,
) {
    // OPTIMIZATION: Only the registered observers of the inclusive ancestors of parent add transient ones, and a record
    //               no one hears is not queued, see mutation_observers.rs.
    let parent_is_observed = has_observed_inclusive_ancestor(parent);
    let suppress_observers = suppress_observers || !tree_mutation_record_is_heard(parent);
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe {
        (hooks(main_thread).finish_removal)(
            node,
            parent,
            old_previous_sibling.into(),
            old_next_sibling.into(),
            parent_is_observed,
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
#[inline]
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

/// Step 3 of adopting `node`, which has no parent, into `document`, which is not `old_document`, its node document
/// before the adoption: runs the steps for each of its shadow-including inclusive descendants, and moves what the
/// systems whose state C++ keeps keep for the subtree, such as adopted style sheets, and the document registries
/// of NodeIterators and live ranges, from `old_document` to `document`.
/// https://dom.spec.whatwg.org/#concept-node-adopt
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn adopt_from_other_document(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    old_document: NodeRef,
    document: NodeRef,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).adopt_from_other_document)(node, old_document, document) }
}

/// What moving `node` from `old_parent`, whose root is `old_parent_root`, into `new_parent` does before it runs the
/// live range pre-remove steps and the NodeIterator pre-remove steps given `node`, steps 9 and 10: tells the page of
/// the move, consumes the deferred style change events of the document, and keeps in `scope` the `:read-write` state
/// of the elements of a subtree that changes editability. Then has the style engine take in its pending arrivals, keeps in `scope` where an element was and
/// whether the subtree holds the focused or hovered node, marks the layout tree of `old_parent`, unregisters the anchor
/// names a subtree that leaves its tree scope registered, and takes `node` out of the animation counts of `old_parent`.
/// https://dom.spec.whatwg.org/#move
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn begin_move(
    main_thread: &mut MainThread<'_>,
    scope: &mut MoveScope,
    node: NodeRef,
    old_parent: NodeRef,
    old_parent_root: NodeRef,
    new_parent: NodeRef,
) {
    // SAFETY: The callback is called on the main thread with live nodes and the scope of the move.
    unsafe { (hooks(main_thread).moving.begin_move)(scope, node, old_parent, old_parent_root, new_parent) }
}

/// Step 17 of moving `node`, which has left the children of `old_parent`: adds the transient registered observers
/// given `node` and `old_parent`.
/// https://dom.spec.whatwg.org/#move
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn after_move_unlink(main_thread: &mut MainThread<'_>, node: NodeRef, old_parent: NodeRef) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).moving.after_unlink)(node, old_parent) }
}

/// What moving `node`, which has joined the children of `new_parent`, from `old_parent`, whose root was
/// `old_parent_root`, does before steps 22 to 24: puts `node` in the C++ state that follows the children of
/// `new_parent`, records the move for the style engine, marks the layout tree, refreshes the lookup caches of the
/// document, and takes the state of the focused or hovered node out of the old ancestors of a subtree that holds it.
/// https://dom.spec.whatwg.org/#move
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn after_move_link(
    main_thread: &mut MainThread<'_>,
    scope: &mut MoveScope,
    node: NodeRef,
    old_parent: NodeRef,
    old_parent_root: NodeRef,
    new_parent: NodeRef,
) {
    // SAFETY: The callback is called on the main thread with live nodes and the scope of the move.
    unsafe { (hooks(main_thread).moving.after_link)(scope, node, old_parent, old_parent_root, new_parent) }
}

/// Steps 25.2 and 25.3 of moving a node from `old_parent` into `new_parent`, for `inclusive_descendant`, a
/// shadow-including inclusive descendant of the moved node: if `inclusive_descendant` is custom and `new_parent` is
/// connected, enqueues a custom element callback reaction with it, callback name "connectedMoveCallback", and « », and
/// then runs the moving steps with `inclusive_descendant`, `is_subtree_root` and `old_parent`.
/// https://dom.spec.whatwg.org/#move
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn run_moving_steps(
    main_thread: &mut MainThread<'_>,
    inclusive_descendant: NodeRef,
    is_subtree_root: IsSubtreeRoot,
    old_parent: NodeRef,
    new_parent: NodeRef,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe {
        (hooks(main_thread).moving.run_moving_steps)(inclusive_descendant, is_subtree_root, old_parent, new_parent);
    }
}

/// Steps 26 and 27 of moving `node` from `old_parent` into `new_parent`: queues the tree mutation records for
/// `old_parent` and `new_parent`. Before them, invalidates the styles of the elements whose `:read-write` state the
/// move changed, and after them, does for an <html> or media element parent what the children changed steps, which a
/// move does not run, would have done.
/// https://dom.spec.whatwg.org/#move
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[allow(clippy::too_many_arguments)]
#[inline]
pub(crate) fn end_move(
    main_thread: &mut MainThread<'_>,
    scope: &mut MoveScope,
    node: NodeRef,
    old_parent: NodeRef,
    new_parent: NodeRef,
    old_previous_sibling: Option<NodeRef>,
    old_next_sibling: Option<NodeRef>,
    new_previous_sibling: Option<NodeRef>,
    child: Option<NodeRef>,
) {
    // SAFETY: The callback is called on the main thread with live nodes and the scope of the move.
    unsafe {
        (hooks(main_thread).moving.end_move)(
            scope,
            node,
            old_parent,
            new_parent,
            old_previous_sibling.into(),
            old_next_sibling.into(),
            new_previous_sibling.into(),
            child.into(),
        );
    }
}

/// Tells the selection and the highlights of each live range of `document` that the live range pre-remove steps set the
/// start or end of, which RANGE_BOUNDARY_POINT_SET_BY_MUTATION marks, of the change, and clears the marks.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn live_range_boundary_points_set(main_thread: &mut MainThread<'_>, document: NodeRef) {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).live_range_boundary_points_set)(document) }
}

/// Calls `filter`, a DOM::TraversalFilter, with `node`: the user object operation "acceptNode" of a traverser's
/// filter, step 6 of filtering. Answers the result, or that it threw, in which case C++ keeps the exception.
/// https://dom.spec.whatwg.org/#concept-node-filter
///
/// Can run script, allocate and change the tree.
#[inline]
pub(crate) fn call_node_filter(
    main_thread: &mut MainThread<'_>,
    filter: NonNull<c_void>,
    node: NodeRef,
) -> NodeFilterCall {
    // SAFETY: The callback is called on the main thread with a live filter and node.
    unsafe { (hooks(main_thread).call_node_filter)(filter.as_ptr(), node) }
}

/// Whether the filter that C++ keeps for `collection` matches `node`.
///
/// Can allocate. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn collection_filter_matches(
    main_thread: &mut MainThread<'_>,
    collection: &CollectionCell,
    node: NodeRef,
) -> bool {
    // SAFETY: The callback is called on the main thread with a live collection and node.
    unsafe { (hooks(main_thread).collection_filter_matches)(collection, node) }
}

/// Queues a tree mutation record for `target` with `added_nodes`, `removed_nodes`, `previous_sibling` and
/// `next_sibling`, one of whose lists is not empty.
/// https://dom.spec.whatwg.org/#queue-a-tree-mutation-record
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn queue_tree_mutation_record(
    main_thread: &mut MainThread<'_>,
    target: NodeRef,
    added_nodes: &[NodeRef],
    removed_nodes: &[NodeRef],
    previous_sibling: Option<NodeRef>,
    next_sibling: Option<NodeRef>,
) {
    // OPTIMIZATION: A record no one hears is not queued, see mutation_observers.rs.
    if !tree_mutation_record_is_heard(target) {
        return;
    }
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

/// Appends an attribute to the element's attribute list, see DomHooks::append_attribute. `local_name`, `prefix` and
/// `namespace_uri` are raw words of live strings, or 0 for a null prefix or namespace.
#[inline]
pub(crate) fn append_attribute(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    local_name: usize,
    prefix: usize,
    namespace_uri: usize,
    value: ak::Utf16String,
) {
    // SAFETY: The callback is called on the main thread with a live element and live strings, and takes over the
    //         reference to the value.
    unsafe { (hooks(main_thread).append_attribute)(element, local_name, prefix, namespace_uri, value.into_raw()) }
}

/// Makes room in the element's attribute list for `count` more attributes, see DomHooks::reserve_attributes.
#[inline]
pub(crate) fn reserve_attributes(main_thread: &mut MainThread<'_>, element: NodeRef, count: usize) {
    // SAFETY: The callback is called on the main thread with a live element.
    unsafe { (hooks(main_thread).reserve_attributes)(element, count) }
}

/// Steps 1 to 4 and 6 of cloning a single node `node` with the document and, where `uses_fallback_registry` says so,
/// the fallback registry of `scope`: makes the copy, and gives it the state of `node` that its interface has but for an
/// element's attributes, which the caller copies. For a node that is no element, also runs the cloning steps of
/// cloning a node with `subtree`, step 3 of it. Answers the copy, or none where a step threw, whose exception `scope`
/// then holds.
/// https://dom.spec.whatwg.org/#clone-a-single-node
/// https://dom.spec.whatwg.org/#concept-node-clone
///
/// Allocates, and changes no tree but for the template contents the cloning steps of a template clone into. Runs no
/// script, as it creates elements without synchronous custom element constructors.
#[inline]
pub(crate) fn clone_single_node(
    main_thread: &mut MainThread<'_>,
    scope: &mut CloneScope,
    node: NodeRef,
    uses_fallback_registry: bool,
    subtree: bool,
) -> Option<NodeRef> {
    let mut copy = NullableNodeRef::from(None);
    // SAFETY: The callback is called on the main thread with a live node and the scope of the clone.
    let ok = unsafe {
        (hooks(main_thread).cloning.clone_single_node)(scope, node, uses_fallback_registry, subtree, &mut copy)
    };
    ok.then(|| Option::from(copy).expect("a callback that does not throw makes a copy"))
}

/// The rest of cloning a single node, the element `node`, into `copy` after its attributes: has `copy` take the inline
/// style declarations of `node`, and runs the cloning steps of cloning a node with `subtree`, step 3 of it. Answers
/// false where a step threw, whose exception `scope` then holds.
/// https://dom.spec.whatwg.org/#clone-a-single-node
/// https://dom.spec.whatwg.org/#concept-node-clone
///
/// Allocates, and changes no tree but for the template contents the cloning steps of a template clone into. Runs no
/// script.
#[inline]
pub(crate) fn run_element_cloning_steps(
    main_thread: &mut MainThread<'_>,
    scope: &mut CloneScope,
    node: NodeRef,
    copy: NodeRef,
    subtree: bool,
) -> bool {
    // SAFETY: The callback is called on the main thread with live nodes and the scope of the clone.
    unsafe { (hooks(main_thread).cloning.run_element_cloning_steps)(scope, node, copy, subtree) }
}

/// Steps 6.1 to 6.6 of cloning a node, for `node`, a shadow host, and its copy `copy`: where the shadow root of `node`
/// is clonable, attaches a shadow root to `copy` like it, and answers it. Answers Ok(None) where the shadow root is not
/// clonable, and Err(()) where a step threw, whose exception `scope` then holds.
/// https://dom.spec.whatwg.org/#concept-node-clone
///
/// Allocates. Runs no script, and changes no tree.
#[inline]
pub(crate) fn attach_shadow_root_copy(
    main_thread: &mut MainThread<'_>,
    scope: &mut CloneScope,
    node: NodeRef,
    copy: NodeRef,
) -> Result<Option<NodeRef>, ()> {
    let mut shadow_root_copy = NullableNodeRef::from(None);
    // SAFETY: The callback is called on the main thread with live nodes and the scope of the clone.
    let ok = unsafe { (hooks(main_thread).cloning.attach_shadow_root_copy)(scope, node, copy, &mut shadow_root_copy) };
    match ok {
        true => Ok(shadow_root_copy.into()),
        false => Err(()),
    }
}

/// Has `slottable`'s assigned slot go from `old_slot` to `new_slot`, and tells the systems whose state C++ keeps,
/// the page, the document and the style engine, of the change.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn assigned_slot_changed(
    main_thread: &mut MainThread<'_>,
    slottable: NodeRef,
    old_slot: Option<NodeRef>,
    new_slot: Option<NodeRef>,
) {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).slots.assigned_slot_changed)(slottable, old_slot.into(), new_slot.into()) }
}

/// A new GC::WeakImpl for `node`, to which the caller holds a reference.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn create_weak(main_thread: &mut MainThread<'_>, node: NodeRef) -> NonNull<c_void> {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).id_map.create_weak)(node) }
}

/// Releases the caller's reference to the GC::WeakImpl `weak`, which create_weak() made.
///
/// Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn release_weak(main_thread: &mut MainThread<'_>, weak: NonNull<c_void>) {
    // SAFETY: The callback is called on the main thread with a GC::WeakImpl the caller holds a reference to.
    unsafe { (hooks(main_thread).id_map.release_weak)(weak) }
}

/// Runs the C++ steps for `slot` that `steps` names, see SLOT_CHANGE_*.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn slot_changed(main_thread: &mut MainThread<'_>, slot: NodeRef, steps: u8) {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).slots.slot_changed)(slot, steps) }
}

/// Has the document of `shadow_root`, a slot of whose tree has just come to need assignment, bring the assigned nodes
/// of its slots up to date before the style engine next reads them.
///
/// Allocates. Runs no script, and leaves the tree as it is.
#[inline]
pub(crate) fn shadow_root_needs_slot_assignment(main_thread: &mut MainThread<'_>, shadow_root: NodeRef) {
    // SAFETY: The callback is called on the main thread with a live node.
    unsafe { (hooks(main_thread).slots.shadow_root_needs_slot_assignment)(shadow_root) }
}

/// Runs the C++ steps of handling attribute changes, see DomHooks::attribute_changed. The callback can run script.
#[inline]
pub(crate) fn attribute_changed(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    name: NonNull<QualifiedNameImpl>,
    old_value: usize,
    new_value: usize,
    steps: u8,
) {
    // SAFETY: The callback is called on the main thread with a live element, whose name and values the caller holds
    //         references to.
    unsafe { (hooks(main_thread).attribute_changed)(element, name, old_value, new_value, steps) }
}

#[inline]
pub(crate) fn synchronize_style_attribute(main_thread: &mut MainThread<'_>, element: NodeRef) {
    // SAFETY: The callback is called on the main thread with a live element.
    unsafe { (hooks(main_thread).synchronize_style_attribute)(element) }
}

/// Drops a reference to a qualified name's parts, which the caller holds.
#[inline]
pub(crate) fn release_qualified_name(main_thread: &mut MainThread<'_>, name: NonNull<QualifiedNameImpl>) {
    // SAFETY: The caller holds the reference, on the main thread.
    if unsafe { name.as_ref().unref_unless_last() } {
        return;
    }
    // SAFETY: The callback is called on the main thread with the last reference to the parts, which it takes over.
    unsafe { (hooks(main_thread).release_qualified_name)(name) }
}

/// Runs the C++ steps of replacing the data of a CharacterData node, see DomHooks::character_data_replaced. The
/// callback can run script.
#[inline]
pub(crate) fn character_data_replaced(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    old_data: usize,
    offset: usize,
    count: usize,
    data_length: usize,
    steps: u8,
) {
    // SAFETY: The callback is called on the main thread with a live node, whose old data the caller holds.
    unsafe { (hooks(main_thread).character_data_replaced)(node, old_data, offset, count, data_length, steps) }
}

/// The range of the document's selection, which the selection keeps alive while nothing changes it.
#[inline]
pub(crate) fn selection_range(main_thread: &mut MainThread<'_>, document: NodeRef) -> Option<NonNull<RangeCell>> {
    // SAFETY: The callback is called on the main thread with a live document.
    unsafe { (hooks(main_thread).selection_range)(document) }
}

/// Whether the doctypes, processing instructions or Attr nodes `a` and `b` have equal state, see
/// DomHooks::node_state_equals.
#[inline]
pub(crate) fn node_state_equals(main_thread: &mut MainThread<'_>, a: NodeRef, b: NodeRef) -> bool {
    // SAFETY: The callback is called on the main thread with live nodes.
    unsafe { (hooks(main_thread).node_state_equals)(a, b) }
}
