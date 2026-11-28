/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Boundary points and live ranges: the boundary points of a range, which C++ keeps in a core whose layout is defined
//! here, how boundary points compare, and the steps of tree and data mutation that keep live ranges where they were.
//! https://dom.spec.whatwg.org/#ranges

use core::cell::Cell;
use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::mem::offset_of;
use core::ptr::NonNull;

use super::hooks;
use super::node::Node;
use super::node::NodeCell;
use super::node_core::NodeMember;
use super::node_core::NodePointer;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use super::weak_cells::WeakCellRegistryField;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// Where DOM::AbstractRange embeds its core, in bytes from the start of the range's cell, which AbstractRange.cpp
/// asserts.
pub const RANGE_CORE_OFFSET: usize = 24;

/// Whether a mutation set the start or end of the live range, which the C++ side has yet to tell the range's selection
/// and highlights of, see hooks::live_range_boundary_points_set().
pub const RANGE_BOUNDARY_POINT_SET_BY_MUTATION: u8 = 1 << 0;

/// The boundary points of a range, which C++ embeds in DOM::AbstractRange as its only storage of them.
/// https://dom.spec.whatwg.org/#concept-range
#[repr(C)]
pub struct RangeCore {
    /// https://dom.spec.whatwg.org/#concept-range-start-node
    start_container: NodeMember,
    /// https://dom.spec.whatwg.org/#concept-range-start-offset
    start_offset: Cell<u32>,
    /// https://dom.spec.whatwg.org/#concept-range-end-node
    end_container: NodeMember,
    /// https://dom.spec.whatwg.org/#concept-range-end-offset
    end_offset: Cell<u32>,
    /// The RANGE_* flags.
    flags: Cell<u8>,
}

impl RangeCore {
    #[inline(always)]
    fn start_container(&self) -> NodePointer {
        self.start_container.get().expect("a range has a start node")
    }

    #[inline(always)]
    fn end_container(&self) -> NodePointer {
        self.end_container.get().expect("a range has an end node")
    }

    /// https://dom.spec.whatwg.org/#concept-range-start
    #[inline(always)]
    fn start<'a>(&self, main_thread: &'a MainThread<'_>) -> (Node<'a>, u32) {
        // SAFETY: A range keeps its start node alive.
        let node = unsafe { Node::from_pointer(self.start_container(), main_thread) };
        (node, self.start_offset.get())
    }

    /// https://dom.spec.whatwg.org/#concept-range-end
    #[inline(always)]
    fn end<'a>(&self, main_thread: &'a MainThread<'_>) -> (Node<'a>, u32) {
        // SAFETY: A range keeps its end node alive.
        let node = unsafe { Node::from_pointer(self.end_container(), main_thread) };
        (node, self.end_offset.get())
    }

    #[inline(always)]
    fn set_start(&self, node: NodePointer, offset: u32) {
        self.start_container.set(Some(node));
        self.start_offset.set(offset);
    }

    #[inline(always)]
    fn set_end(&self, node: NodePointer, offset: u32) {
        self.end_container.set(Some(node));
        self.end_offset.set(offset);
    }

    /// Marks the range for hooks::live_range_boundary_points_set().
    #[inline(always)]
    fn mark_boundary_point_set_by_mutation(&self) {
        self.flags.set(self.flags.get() | RANGE_BOUNDARY_POINT_SET_BY_MUTATION);
    }

    /// https://dom.spec.whatwg.org/#concept-range-root
    #[inline(always)]
    fn root(&self, main_thread: &MainThread<'_>) -> NodePointer {
        // The root of a live range is the root of its start node.
        self.start(main_thread).0.root().pointer()
    }
}

/// A DOM::AbstractRange as C++ allocates it: the start of the range's cell, with its core RANGE_CORE_OFFSET bytes into
/// it. FFI entry points take ranges as references to it, which C++ sees as DOM::AbstractRange pointers.
#[repr(C)]
pub struct RangeCell {
    // NB: The cell's state changes behind shared references to it.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which Rust never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

impl RangeCell {
    #[inline(always)]
    pub(super) fn core(&self) -> &RangeCore {
        // SAFETY: Every range embeds its core RANGE_CORE_OFFSET bytes into its cell, which AbstractRange.cpp asserts.
        unsafe { &*(self as *const Self).byte_add(RANGE_CORE_OFFSET).cast::<RangeCore>() }
    }
}

/// The core of the range whose cell starts at `cell`.
///
/// # Safety
///
/// `cell` must be a live range, which must stay alive for `'a`.
#[inline(always)]
unsafe fn range_core<'a>(cell: NonNull<u8>) -> &'a RangeCore {
    // SAFETY: Guaranteed by the caller.
    unsafe { cell.cast::<RangeCell>().as_ref() }.core()
}

/// Calls `callback` with the core of each live range of `live_ranges`, a document's registry of them.
///
/// The shared borrow of the main thread token keeps anything from changing the DOM or the registry while it runs.
#[inline(always)]
fn for_each_live_range(
    _main_thread: &MainThread<'_>,
    live_ranges: &WeakCellRegistryField,
    mut callback: impl FnMut(&RangeCore),
) {
    // SAFETY: The registry's entries are as C++ last mirrored them, as nothing can call into C++ while the main thread
    //         token is borrowed, and each live entry is a live range.
    unsafe { live_ranges.for_each_cell(|cell| callback(range_core(cell))) };
}

/// The position of one boundary point relative to another, laid out as C++'s DOM::RelativeBoundaryPointPosition, which
/// is this type.
/// https://dom.spec.whatwg.org/#concept-range-bp-position
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RelativeBoundaryPointPosition {
    Equal,
    Before,
    After,
}

/// The DOMException a range method throws, or none.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RangeException {
    None,
    /// An "InvalidNodeTypeError" DOMException.
    InvalidNodeTypeError,
    /// An "IndexSizeError" DOMException.
    IndexSizeError,
    /// A "NotSupportedError" DOMException.
    NotSupportedError,
    /// A "WrongDocumentError" DOMException.
    WrongDocumentError,
}

/// What a range method that compares returns, or the DOMException it throws instead.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RangeComparison {
    pub exception: RangeException,
    pub value: i16,
}

impl RangeComparison {
    fn value(value: i16) -> Self {
        Self {
            exception: RangeException::None,
            value,
        }
    }

    fn exception(exception: RangeException) -> Self {
        Self { exception, value: 0 }
    }
}

/// Which boundary point of a range to set, laid out as C++'s DOM::Range::StartOrEnd, which is this type.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StartOrEnd {
    Start,
    End,
}

/// An offset as a range keeps it: a WebIDL unsigned long.
#[inline(always)]
fn offset(value: usize) -> u32 {
    u32::try_from(value).expect("a boundary point's offset fits in an unsigned long")
}

/// https://dom.spec.whatwg.org/#concept-range-bp-position
pub fn position_of_boundary_point(
    (node_a, offset_a): (Node<'_>, u32),
    (node_b, offset_b): (Node<'_>, u32),
) -> RelativeBoundaryPointPosition {
    use RelativeBoundaryPointPosition::*;

    // The position of a boundary point (nodeA, offsetA) relative to a boundary point (nodeB, offsetB) is before,
    // equal, or after, as returned by these steps:

    // 1. Assert: nodeA and nodeB have the same root.
    // NB: Nodes may not share the same root if they belong to different shadow trees, so this asserts that they share
    //     the same shadow-including root instead.
    assert!(node_a.shadow_including_root() == node_b.shadow_including_root());

    // 2. If nodeA is nodeB, then return equal if offsetA is offsetB, before if offsetA is less than offsetB, and after
    //    if offsetA is greater than offsetB.
    if node_a == node_b {
        return match offset_a.cmp(&offset_b) {
            core::cmp::Ordering::Equal => Equal,
            core::cmp::Ordering::Less => Before,
            core::cmp::Ordering::Greater => After,
        };
    }

    // 3. If nodeA is following nodeB, then if the position of (nodeB, offsetB) relative to (nodeA, offsetA) is before,
    //    return after, and if it is after, return before.
    if node_a.is_following(node_b) {
        match position_of_boundary_point((node_b, offset_b), (node_a, offset_a)) {
            Before => return After,
            After => return Before,
            Equal => {}
        }
    }

    // 4. If nodeA is an ancestor of nodeB:
    if node_a.is_ancestor_of(node_b) {
        // 1. Let child be nodeB.
        let mut child = node_b;

        // 2. While child is not a child of nodeA, set child to its parent.
        while child.parent() != Some(node_a) {
            child = child.parent().expect("a descendant has a parent");
        }

        // 3. If child’s index is less than offsetA, then return after.
        if child.index() < offset_a as usize {
            return After;
        }
    }

    // 5. Return before.
    Before
}

/// A node's index, worked out the first time it is asked for.
/// OPTIMIZATION: Working out a node's index walks all of its preceding siblings, so the live range steps defer it
///               until a live range needs it.
struct LazyIndex<'a> {
    node: Node<'a>,
    index: Option<u32>,
}

impl<'a> LazyIndex<'a> {
    fn new(node: Node<'a>) -> Self {
        Self { node, index: None }
    }

    fn get(&mut self) -> u32 {
        *self.index.get_or_insert_with(|| offset(self.node.index()))
    }
}

/// Steps 5.1 and 5.2 of inserting nodes, `count` of them, into `parent` before `child`, and step 18 of moving a node
/// into `parent` before `child` with a count of 1.
/// https://dom.spec.whatwg.org/#concept-node-insert
/// https://dom.spec.whatwg.org/#move
pub(crate) fn adjust_live_ranges_for_insertion(
    main_thread: &MainThread<'_>,
    parent: NodeRef,
    child: NodeRef,
    count: usize,
) {
    let live_ranges = parent.document_core().live_ranges();
    if live_ranges.is_empty() {
        return;
    }
    let count = offset(count);
    let parent_pointer = parent.read(main_thread).pointer();
    let mut child_index = LazyIndex::new(child.read(main_thread));

    // 1. For each live range whose start node is parent and start offset is greater than child’s index: increase its
    //    start offset by count.
    // 2. For each live range whose end node is parent and end offset is greater than child’s index: increase its end
    //    offset by count.
    // OPTIMIZATION: These steps are independent between ranges, so the live ranges are walked only once.
    for_each_live_range(main_thread, live_ranges, |range| {
        if range.start_container() == parent_pointer && range.start_offset.get() > child_index.get() {
            range.start_offset.set(range.start_offset.get() + count);
        }
        if range.end_container() == parent_pointer && range.end_offset.get() > child_index.get() {
            range.end_offset.set(range.end_offset.get() + count);
        }
    });
}

/// https://dom.spec.whatwg.org/#live-range-pre-remove-steps
#[inline(always)]
pub(crate) fn live_range_pre_remove_steps(main_thread: &mut MainThread<'_>, node: NodeRef) {
    // OPTIMIZATION: Most documents have no live range, so the check for none is inlined into the removal.
    if node.document_core().live_ranges().is_empty() {
        return;
    }
    if run_live_range_pre_remove_steps(main_thread, node.read(main_thread)) {
        hooks::live_range_boundary_points_set(main_thread, node.document());
    }
}

/// The live range pre-remove steps for each child of `parent`, which are all removed, run at once before the first is
/// removed. Each child is the first child when its turn comes, so the steps for every child together leave each boundary
/// point that is in a child, or on `parent`, at (`parent`, 0).
/// https://dom.spec.whatwg.org/#live-range-pre-remove-steps
pub(crate) fn live_range_pre_remove_steps_for_all_children(main_thread: &mut MainThread<'_>, parent: NodeRef) {
    if parent.document_core().live_ranges().is_empty() {
        return;
    }
    if run_live_range_pre_remove_steps_for_all_children(main_thread, parent.read(main_thread)) {
        hooks::live_range_boundary_points_set(main_thread, parent.document());
    }
}

/// The live range pre-remove steps given `node`, but for telling the selection and highlights of the live ranges whose
/// start or end they set, which they mark instead. Answers whether they marked any.
/// https://dom.spec.whatwg.org/#live-range-pre-remove-steps
#[inline(never)]
fn run_live_range_pre_remove_steps(main_thread: &MainThread<'_>, node: Node<'_>) -> bool {
    let node_ref = NodeRef::from(node);
    let live_ranges = node_ref.document_core().live_ranges();
    let node_root = node.root();

    // The live range pre-remove steps given a node node, are:

    // 1. Let parent be node’s parent.
    let parent = node.parent();

    // 2. Assert: parent is non-null.
    let parent = parent.expect("a node that is removed has a parent");
    let parent_pointer = parent.pointer();

    // 3. Let index be node’s index.
    let mut index = LazyIndex::new(node);

    // OPTIMIZATION: A node of another tree is no inclusive descendant of node, which its root tells without a walk, and
    //               only node is an inclusive descendant of a node without children. Many boundary points share their
    //               node, so the answer for the last node asked about is kept.
    let node_has_children = node.first_child().is_some();
    let mut last_answer: Option<(*const NodeCell, bool)> = None;
    let mut is_inclusive_descendant_of_node = |other: Node<'_>| {
        if other == node {
            return true;
        }
        if !node_has_children || other.root() != node_root {
            return false;
        }
        if let Some((last_node, answer)) = last_answer
            && core::ptr::eq(last_node, other.cell())
        {
            return answer;
        }
        let answer = node.is_ancestor_of(other);
        last_answer = Some((other.cell() as *const NodeCell, answer));
        answer
    };
    let mut any_boundary_point_set = false;

    // 4. For each live range whose start node is an inclusive descendant of node, set its start to (parent, index).
    // 5. For each live range whose end node is an inclusive descendant of node, set its end to (parent, index).
    // 6. For each live range whose start node is parent and start offset is greater than index, decrease its start
    //    offset by 1.
    // 7. For each live range whose end node is parent and end offset is greater than index, decrease its end offset
    //    by 1.
    // OPTIMIZATION: These steps are independent between ranges, so the live ranges are walked only once.
    for_each_live_range(main_thread, live_ranges, |range| {
        if is_inclusive_descendant_of_node(range.start(main_thread).0) {
            range.set_start(parent_pointer, index.get());
            range.mark_boundary_point_set_by_mutation();
            any_boundary_point_set = true;
        }
        if is_inclusive_descendant_of_node(range.end(main_thread).0) {
            range.set_end(parent_pointer, index.get());
            range.mark_boundary_point_set_by_mutation();
            any_boundary_point_set = true;
        }
        if range.start_container() == parent_pointer && range.start_offset.get() > index.get() {
            range.start_offset.set(range.start_offset.get() - 1);
        }
        if range.end_container() == parent_pointer && range.end_offset.get() > index.get() {
            range.end_offset.set(range.end_offset.get() - 1);
        }
    });
    any_boundary_point_set
}

/// The live range pre-remove steps for each child of `parent` at once, but for telling the selection and highlights
/// of the live ranges whose start or end they set, which they mark instead. Answers whether they marked any.
fn run_live_range_pre_remove_steps_for_all_children(main_thread: &MainThread<'_>, parent: Node<'_>) -> bool {
    let parent_ref = NodeRef::from(parent);
    let live_ranges = parent_ref.document_core().live_ranges();
    let parent_pointer = parent.pointer();
    let parent_root = parent.root();
    // OPTIMIZATION: As in run_live_range_pre_remove_steps(), the answer for the last node asked about is kept.
    let mut last_answer: Option<(*const NodeCell, bool)> = None;
    let mut is_descendant_of_parent = |other: Node<'_>| {
        if other.root() != parent_root {
            return false;
        }
        if let Some((last_node, answer)) = last_answer
            && core::ptr::eq(last_node, other.cell())
        {
            return answer;
        }
        let answer = parent.is_ancestor_of(other);
        last_answer = Some((other.cell() as *const NodeCell, answer));
        answer
    };
    let mut any_boundary_point_set = false;
    for_each_live_range(main_thread, live_ranges, |range| {
        let (start_node, _) = range.start(main_thread);
        if start_node == parent {
            range.start_offset.set(0);
        } else if is_descendant_of_parent(start_node) {
            range.set_start(parent_pointer, 0);
            range.mark_boundary_point_set_by_mutation();
            any_boundary_point_set = true;
        }
        let (end_node, _) = range.end(main_thread);
        if end_node == parent {
            range.end_offset.set(0);
        } else if is_descendant_of_parent(end_node) {
            range.set_end(parent_pointer, 0);
            range.mark_boundary_point_set_by_mutation();
            any_boundary_point_set = true;
        }
    });
    any_boundary_point_set
}

/// Steps 8 to 11 of replacing data of `node` with `offset`, `count`, which is clamped to the node's length already, and
/// data whose length is `data_length`, for each live range but `range_to_skip`.
/// https://dom.spec.whatwg.org/#concept-cd-replace
pub(super) fn adjust_live_ranges_for_replaced_data(
    main_thread: &MainThread<'_>,
    node: Node<'_>,
    replace_offset: usize,
    count: usize,
    data_length: usize,
    range_to_skip: Option<&RangeCore>,
) {
    let node_ref = NodeRef::from(node);
    let live_ranges = node_ref.document_core().live_ranges();
    if live_ranges.is_empty() {
        return;
    }
    let node_pointer = node.pointer();
    let replaced_end = offset(replace_offset + count);
    let replace_offset = offset(replace_offset);
    let (data_length, count) = (offset(data_length), offset(count));
    let moved = |boundary_offset: u32| boundary_offset + data_length - count;

    // 8. For each live range whose start node is node and start offset is greater than offset but less than or equal to
    //    offset + count: set its start offset to offset.
    // 9. For each live range whose end node is node and end offset is greater than offset but less than or equal to
    //    offset + count: set its end offset to offset.
    // 10. For each live range whose start node is node and start offset is greater than offset + count: increase its
    //     start offset by data’s length and decrease it by count.
    // 11. For each live range whose end node is node and end offset is greater than offset + count: increase its end
    //     offset by data’s length and decrease it by count.
    // OPTIMIZATION: These steps are independent between ranges, so the live ranges are walked only once.
    for_each_live_range(main_thread, live_ranges, |range| {
        if range_to_skip.is_some_and(|range_to_skip| core::ptr::eq(range, range_to_skip)) {
            return;
        }
        if range.start_container() == node_pointer {
            let start_offset = range.start_offset.get();
            if start_offset > replace_offset && start_offset <= replaced_end {
                range.start_offset.set(replace_offset);
            } else if start_offset > replaced_end {
                range.start_offset.set(moved(start_offset));
            }
        }
        if range.end_container() == node_pointer {
            let end_offset = range.end_offset.get();
            if end_offset > replace_offset && end_offset <= replaced_end {
                range.end_offset.set(replace_offset);
            } else if end_offset > replaced_end {
                range.end_offset.set(moved(end_offset));
            }
        }
    });
}

/// Steps 7.2 to 7.5 of splitting `node` at `split_offset`, after `new_node` has been inserted into `parent`, node's
/// parent.
/// https://dom.spec.whatwg.org/#concept-text-split
pub(super) fn adjust_live_ranges_for_split_text(
    main_thread: &MainThread<'_>,
    node: Node<'_>,
    new_node: Node<'_>,
    split_offset: usize,
    parent: Node<'_>,
) {
    let node_ref = NodeRef::from(node);
    let live_ranges = node_ref.document_core().live_ranges();
    if live_ranges.is_empty() {
        return;
    }
    let (node_pointer, new_node_pointer, parent_pointer) = (node.pointer(), new_node.pointer(), parent.pointer());
    let split_offset = offset(split_offset);
    let mut node_index = LazyIndex::new(node);

    // 2. For each live range whose start node is node and start offset is greater than offset, set its start node to
    //    newNode and decrease its start offset by offset.
    // 3. For each live range whose end node is node and end offset is greater than offset, set its end node to newNode
    //    and decrease its end offset by offset.
    // 4. For each live range whose start node is parent and start offset is equal to the index of node plus 1, increase
    //    its start offset by 1.
    // 5. For each live range whose end node is parent and end offset is equal to the index of node plus 1, increase its
    //    end offset by 1.
    // OPTIMIZATION: These steps are independent between ranges, so the live ranges are walked only once.
    for_each_live_range(main_thread, live_ranges, |range| {
        if range.start_container() == node_pointer && range.start_offset.get() > split_offset {
            range.set_start(new_node_pointer, range.start_offset.get() - split_offset);
        }
        if range.end_container() == node_pointer && range.end_offset.get() > split_offset {
            range.set_end(new_node_pointer, range.end_offset.get() - split_offset);
        }
        if range.start_container() == parent_pointer && range.start_offset.get() == node_index.get() + 1 {
            range.start_offset.set(range.start_offset.get() + 1);
        }
        if range.end_container() == parent_pointer && range.end_offset.get() == node_index.get() + 1 {
            range.end_offset.set(range.end_offset.get() + 1);
        }
    });
}

/// Steps 6.1 to 6.4 of normalizing, for `current_node`, an exclusive Text node that follows `node`, whose data the
/// steps have made `length` code units long up to `current_node`'s.
/// https://dom.spec.whatwg.org/#dom-node-normalize
pub(super) fn adjust_live_ranges_for_normalize(
    main_thread: &MainThread<'_>,
    node: Node<'_>,
    current_node: Node<'_>,
    length: usize,
) {
    let node_ref = NodeRef::from(node);
    let live_ranges = node_ref.document_core().live_ranges();
    if live_ranges.is_empty() {
        return;
    }
    let (node_pointer, current_node_pointer) = (node.pointer(), current_node.pointer());
    let parent_pointer = current_node.parent().map(Node::pointer);
    let length = offset(length);
    let mut current_node_index = LazyIndex::new(current_node);

    // 1. For each live range whose start node is currentNode: add length to its start offset and set its start node to
    //    node.
    // 2. For each live range whose end node is currentNode: add length to its end offset and set its end node to node.
    // 3. For each live range whose start node is currentNode’s parent and start offset is currentNode’s index: set its
    //    start node to node and its start offset to length.
    // 4. For each live range whose end node is currentNode’s parent and end offset is currentNode’s index: set its end
    //    node to node and its end offset to length.
    // OPTIMIZATION: These steps are independent between ranges, so the live ranges are walked only once.
    for_each_live_range(main_thread, live_ranges, |range| {
        if range.start_container() == current_node_pointer {
            range.set_start(node_pointer, range.start_offset.get() + length);
        }
        if range.end_container() == current_node_pointer {
            range.set_end(node_pointer, range.end_offset.get() + length);
        }
        if Some(range.start_container()) == parent_pointer && range.start_offset.get() == current_node_index.get() {
            range.set_start(node_pointer, length);
        }
        if Some(range.end_container()) == parent_pointer && range.end_offset.get() == current_node_index.get() {
            range.set_end(node_pointer, length);
        }
    });
}

/// https://dom.spec.whatwg.org/#concept-range-bp-set
///
/// Answers which DOMException to throw, if one is thrown. The caller tells the range's document, selection and
/// highlights of the change.
fn set_the_start_or_end(
    main_thread: &MainThread<'_>,
    range: &RangeCore,
    node: Node<'_>,
    node_offset: u32,
    start_or_end: StartOrEnd,
) -> RangeException {
    // To set the start or end of a range to a boundary point (node, offset):

    // 1. If node is a doctype, then throw an "InvalidNodeTypeError" DOMException.
    if node.node_type() == NodeType::DOCUMENT_TYPE_NODE {
        return RangeException::InvalidNodeTypeError;
    }

    // 2. If offset is greater than node’s length, then throw an "IndexSizeError" DOMException.
    if node_offset as usize > node.length() {
        return RangeException::IndexSizeError;
    }

    // 3. Let boundaryPoint be the boundary point (node, offset).
    let boundary_point = (node, node_offset);
    let range_root_is_not_node_root = range.root(main_thread) != node.root().pointer();

    // 4. Switch on how these steps were invoked:
    match start_or_end {
        // -> If these steps were invoked as "set the start"
        StartOrEnd::Start => {
            // 1. If range’s root is not node’s root or boundaryPoint is after range’s end, then set range’s end to
            //    boundaryPoint.
            if range_root_is_not_node_root
                || position_of_boundary_point(boundary_point, range.end(main_thread))
                    == RelativeBoundaryPointPosition::After
            {
                range.set_end(node.pointer(), node_offset);
            }

            // 2. Set range’s start to boundaryPoint.
            range.set_start(node.pointer(), node_offset);
        }
        // -> If these steps were invoked as "set the end"
        StartOrEnd::End => {
            // 1. If range’s root is not node’s root or boundaryPoint is before range’s start, then set range’s start
            //    to boundaryPoint.
            if range_root_is_not_node_root
                || position_of_boundary_point(boundary_point, range.start(main_thread))
                    == RelativeBoundaryPointPosition::Before
            {
                range.set_start(node.pointer(), node_offset);
            }

            // 2. Set range’s end to boundaryPoint.
            range.set_end(node.pointer(), node_offset);
        }
    }
    RangeException::None
}

/// How compareBoundaryPoints() compares, laid out as C++'s DOM::Range::HowToCompareBoundaryPoints, whose values these
/// are.
/// https://dom.spec.whatwg.org/#dom-range-start_to_start
const START_TO_START: u16 = 0;
const START_TO_END: u16 = 1;
const END_TO_END: u16 = 2;
const END_TO_START: u16 = 3;

/// https://dom.spec.whatwg.org/#dom-range-compareboundarypoints
fn compare_boundary_points(
    main_thread: &MainThread<'_>,
    this: &RangeCore,
    how: u16,
    source_range: &RangeCore,
) -> RangeComparison {
    // The compareBoundaryPoints(how, sourceRange) method steps are:

    // 1. If how is not one of
    //    - START_TO_START,
    //    - START_TO_END,
    //    - END_TO_END, and
    //    - END_TO_START,
    //    then throw a "NotSupportedError" DOMException.
    if !matches!(how, START_TO_START | START_TO_END | END_TO_END | END_TO_START) {
        return RangeComparison::exception(RangeException::NotSupportedError);
    }

    // 2. If this’s root is not sourceRange’s root, then throw a "WrongDocumentError" DOMException.
    if this.root(main_thread) != source_range.root(main_thread) {
        return RangeComparison::exception(RangeException::WrongDocumentError);
    }

    // 3. Let thisPoint and sourcePoint be null.
    // 4. Switch on how:
    let (this_point, source_point) = match how {
        // START_TO_START:
        //     Set thisPoint to this’s start and sourcePoint to sourceRange’s start.
        START_TO_START => (this.start(main_thread), source_range.start(main_thread)),
        // START_TO_END:
        //     Set thisPoint to this’s end and sourcePoint to sourceRange’s start.
        START_TO_END => (this.end(main_thread), source_range.start(main_thread)),
        // END_TO_END:
        //     Set thisPoint to this’s end and sourcePoint to sourceRange’s end.
        END_TO_END => (this.end(main_thread), source_range.end(main_thread)),
        // END_TO_START:
        //     Set thisPoint to this’s start and sourcePoint to sourceRange’s end.
        _ => (this.start(main_thread), source_range.end(main_thread)),
    };

    // 5. Switch on the position of thisPoint relative to sourcePoint:
    RangeComparison::value(match position_of_boundary_point(this_point, source_point) {
        // before
        //     Return −1.
        RelativeBoundaryPointPosition::Before => -1,
        // equal
        //     Return 0.
        RelativeBoundaryPointPosition::Equal => 0,
        // after
        //     Return 1.
        RelativeBoundaryPointPosition::After => 1,
    })
}

/// https://dom.spec.whatwg.org/#get-the-common-ancestor
fn get_the_common_ancestor<'a>(main_thread: &'a MainThread<'_>, range: &RangeCore) -> Node<'a> {
    // To get the common ancestor of a live range range:

    // 1. Let container be range’s start node.
    let (mut container, _) = range.start(main_thread);
    let (end_node, _) = range.end(main_thread);

    // 2. While container is not an inclusive ancestor of range’s end node: set container to container’s parent.
    while !container.is_inclusive_ancestor_of(end_node) {
        container = container
            .parent()
            .expect("a range's start node and end node share a root");
    }

    // 3. Return container.
    container
}

/// https://dom.spec.whatwg.org/#dom-range-ispointinrange
fn is_point_in_range(
    main_thread: &MainThread<'_>,
    this: &RangeCore,
    node: Node<'_>,
    node_offset: u32,
) -> RangeComparison {
    // The isPointInRange(node, offset) method steps are:

    // 1. If node’s root is not this’s root, then return false.
    if node.root().pointer() != this.root(main_thread) {
        return RangeComparison::value(0);
    }

    // 2. If node is a doctype, then throw an "InvalidNodeTypeError" DOMException.
    if node.node_type() == NodeType::DOCUMENT_TYPE_NODE {
        return RangeComparison::exception(RangeException::InvalidNodeTypeError);
    }

    // 3. If offset is greater than node’s length, then throw an "IndexSizeError" DOMException.
    if node_offset as usize > node.length() {
        return RangeComparison::exception(RangeException::IndexSizeError);
    }

    // 4. If (node, offset) is before start or after end, then return false.
    let point = (node, node_offset);
    if position_of_boundary_point(point, this.start(main_thread)) == RelativeBoundaryPointPosition::Before
        || position_of_boundary_point(point, this.end(main_thread)) == RelativeBoundaryPointPosition::After
    {
        return RangeComparison::value(0);
    }

    // 5. Return true.
    RangeComparison::value(1)
}

/// https://dom.spec.whatwg.org/#dom-range-comparepoint
fn compare_point(main_thread: &MainThread<'_>, this: &RangeCore, node: Node<'_>, node_offset: u32) -> RangeComparison {
    // The comparePoint(node, offset) method steps are:

    // 1. If node’s root is not this’s root, then throw a "WrongDocumentError" DOMException.
    if node.root().pointer() != this.root(main_thread) {
        return RangeComparison::exception(RangeException::WrongDocumentError);
    }

    // 2. If node is a doctype, then throw an "InvalidNodeTypeError" DOMException.
    if node.node_type() == NodeType::DOCUMENT_TYPE_NODE {
        return RangeComparison::exception(RangeException::InvalidNodeTypeError);
    }

    // 3. If offset is greater than node’s length, then throw an "IndexSizeError" DOMException.
    if node_offset as usize > node.length() {
        return RangeComparison::exception(RangeException::IndexSizeError);
    }

    // 4. If (node, offset) is before start, then return −1.
    let point = (node, node_offset);
    if position_of_boundary_point(point, this.start(main_thread)) == RelativeBoundaryPointPosition::Before {
        return RangeComparison::value(-1);
    }

    // 5. If (node, offset) is after end, then return 1.
    if position_of_boundary_point(point, this.end(main_thread)) == RelativeBoundaryPointPosition::After {
        return RangeComparison::value(1);
    }

    // 6. Return 0.
    RangeComparison::value(0)
}

/// https://dom.spec.whatwg.org/#dom-range-intersectsnode
fn intersects_node(main_thread: &MainThread<'_>, this: &RangeCore, node: Node<'_>) -> bool {
    // The intersectsNode(node) method steps are:

    // 1. If node’s root is not this’s root, then return false.
    if node.root().pointer() != this.root(main_thread) {
        return false;
    }

    // 2. Let parent be node’s parent.
    let parent = node.parent();

    // 3. If parent is null, then return true.
    let Some(parent) = parent else {
        return true;
    };

    // 4. Let offset be node’s index.
    let node_offset = offset(node.index());

    // 5. If (parent, offset) is before end and (parent, offset + 1) is after start, then return true.
    if position_of_boundary_point((parent, node_offset), this.end(main_thread)) == RelativeBoundaryPointPosition::Before
        && position_of_boundary_point((parent, node_offset + 1), this.start(main_thread))
            == RelativeBoundaryPointPosition::After
    {
        return true;
    }

    // 6. Return false.
    false
}

/// The position of the boundary point (`node_a`, `offset_a`) relative to (`node_b`, `offset_b`), for C++.
///
/// # Safety
///
/// The nodes must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_position_of_boundary_point(
    node_a: &NodeCell,
    offset_a: u32,
    node_b: &NodeCell,
    offset_b: u32,
) -> RelativeBoundaryPointPosition {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live, and nothing changes the DOM while the token is borrowed.
    let (node_a, node_b) = unsafe {
        (
            Node::from_cell(node_a, &main_thread),
            Node::from_cell(node_b, &main_thread),
        )
    };
    position_of_boundary_point((node_a, offset_a), (node_b, offset_b))
}

/// Sets the start or end of `range` to (`node`, `offset`), for DOM::Range, and answers which DOMException to throw, if
/// one is thrown.
///
/// # Safety
///
/// The range and node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_range_set_start_or_end(
    range: &RangeCell,
    node: &NodeCell,
    offset: u32,
    start_or_end: StartOrEnd,
) -> RangeException {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live, and nothing changes the DOM while the token is borrowed.
    let node = unsafe { Node::from_cell(node, &main_thread) };
    set_the_start_or_end(&main_thread, range.core(), node, offset, start_or_end)
}

/// Compares the boundary points of `range` and `source_range` as compareBoundaryPoints() does, for DOM::Range.
///
/// # Safety
///
/// The ranges must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_range_compare_boundary_points(
    range: &RangeCell,
    how: u16,
    source_range: &RangeCell,
) -> RangeComparison {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    compare_boundary_points(&main_thread, range.core(), how, source_range.core())
}

/// The common ancestor of `range`, for DOM::Range.
///
/// # Safety
///
/// The range must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_range_common_ancestor(range: &RangeCell) -> &NodeCell {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    let common_ancestor = get_the_common_ancestor(&main_thread, range.core());
    // SAFETY: The range keeps its start node, and so its ancestors, alive for as long as the range is borrowed.
    unsafe { &*(common_ancestor.cell() as *const NodeCell) }
}

/// Whether (`node`, `offset`) is in `range`, as isPointInRange() answers, as a value of 1 or 0, for DOM::Range.
///
/// # Safety
///
/// The range and node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_range_is_point_in_range(
    range: &RangeCell,
    node: &NodeCell,
    offset: u32,
) -> RangeComparison {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live, and nothing changes the DOM while the token is borrowed.
    let node = unsafe { Node::from_cell(node, &main_thread) };
    is_point_in_range(&main_thread, range.core(), node, offset)
}

/// Compares (`node`, `offset`) with `range` as comparePoint() does, for DOM::Range.
///
/// # Safety
///
/// The range and node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_range_compare_point(
    range: &RangeCell,
    node: &NodeCell,
    offset: u32,
) -> RangeComparison {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live, and nothing changes the DOM while the token is borrowed.
    let node = unsafe { Node::from_cell(node, &main_thread) };
    compare_point(&main_thread, range.core(), node, offset)
}

/// Whether `range` intersects `node`, as intersectsNode() answers, for DOM::Range.
///
/// # Safety
///
/// The range and node must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_range_intersects_node(range: &RangeCell, node: &NodeCell) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live, and nothing changes the DOM while the token is borrowed.
    let node = unsafe { Node::from_cell(node, &main_thread) };
    intersects_node(&main_thread, range.core(), node)
}

#[cfg(test)]
mod tests {
    use super::super::test_tree::append_child;
    use super::super::test_tree::character_data;
    use super::super::test_tree::document_core;
    use super::super::test_tree::node;
    use super::super::test_tree::set_document;
    use super::*;
    use crate::gc_test_stubs::leaked_weak_impl;
    use RelativeBoundaryPointPosition::*;
    use core::ffi::c_void;

    /// A DOM::AbstractRange for tests: its core, RANGE_CORE_OFFSET bytes into the cell.
    #[repr(C)]
    struct TestRangeCell {
        header: [u8; RANGE_CORE_OFFSET],
        core: RangeCore,
    }

    fn range_core(start: (Node<'_>, u32), end: (Node<'_>, u32)) -> RangeCore {
        let member = |node: Node<'_>| {
            let member = NodeMember::null_for_test();
            member.set(Some(node.pointer()));
            member
        };
        RangeCore {
            start_container: member(start.0),
            start_offset: Cell::new(start.1),
            end_container: member(end.0),
            end_offset: Cell::new(end.1),
            flags: Cell::new(0),
        }
    }

    /// Makes each of `ranges` a live range of `document`, in order, with a cleared weak pointer before each, as the
    /// registry holds one until it drops it after a collection. The ranges live for the rest of the process.
    fn live_ranges<'a>(document: Node<'_>, ranges: &[((Node<'_>, u32), (Node<'_>, u32))]) -> Vec<&'a RangeCore> {
        let mut entries = Vec::new();
        let mut cores = Vec::new();
        for &(start, end) in ranges {
            entries.push(leaked_weak_impl(core::ptr::null_mut()).cast_const().cast::<c_void>());
            let cell = Box::leak(Box::new(TestRangeCell {
                header: [0; RANGE_CORE_OFFSET],
                core: range_core(start, end),
            }));
            entries.push(
                leaked_weak_impl((cell as *mut TestRangeCell).cast())
                    .cast_const()
                    .cast::<c_void>(),
            );
            cores.push(&cell.core);
        }
        document_core(document)
            .live_ranges()
            .set_for_test(Box::leak(entries.into_boxed_slice()));
        cores
    }

    fn start_of<'a>(main_thread: &'a MainThread<'_>, range: &RangeCore) -> (Node<'a>, u32) {
        range.start(main_thread)
    }

    fn end_of<'a>(main_thread: &'a MainThread<'_>, range: &RangeCore) -> (Node<'a>, u32) {
        range.end(main_thread)
    }

    /// The nodes of a document with this tree, in tree order, each of which has the document as its node document:
    ///
    /// ```text
    /// document
    ///   doctype
    ///   html
    ///     head
    ///       title
    ///         text "hello"
    ///     body
    ///       comment "abc"
    /// ```
    fn document<'a>(main_thread: &'a MainThread<'_>) -> [Node<'a>; 8] {
        let new = |node_type| node(main_thread, node_type);
        let nodes = [
            new(NodeType::DOCUMENT_NODE),
            new(NodeType::DOCUMENT_TYPE_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            new(NodeType::ELEMENT_NODE),
            character_data(main_thread, NodeType::TEXT_NODE, "hello"),
            new(NodeType::ELEMENT_NODE),
            character_data(main_thread, NodeType::COMMENT_NODE, "abc"),
        ];
        let [document, doctype, html, head, title, text, body, comment] = nodes;
        append_child(title, text);
        append_child(head, title);
        append_child(html, head);
        append_child(body, comment);
        append_child(html, body);
        append_child(document, doctype);
        append_child(document, html);
        for node in &nodes[1..] {
            set_document(*node, document);
        }
        nodes
    }

    #[test]
    fn boundary_points_compare_in_tree_order() {
        let main_thread = MainThread::for_test();
        let [document, _, html, head, _, text, body, comment] = document(&main_thread);
        let position = |a, b| position_of_boundary_point(a, b);

        assert_eq!(position((html, 1), (html, 1)), Equal);
        assert_eq!(position((html, 0), (html, 1)), Before);
        assert_eq!(position((html, 2), (html, 1)), After);
        assert_eq!(position((head, 0), (html, 0)), After);
        assert_eq!(position((html, 0), (head, 0)), Before);
        assert_eq!(position((html, 1), (head, 1)), After);
        assert_eq!(position((html, 2), (comment, 0)), After);
        assert_eq!(position((html, 1), (comment, 3)), Before);
        assert_eq!(position((text, 5), (comment, 0)), Before);
        assert_eq!(position((comment, 0), (text, 5)), After);
        assert_eq!(position((document, 1), (body, 0)), Before);
        assert_eq!(position((document, 2), (body, 0)), After);
    }

    #[test]
    fn node_length_counts_data_or_children() {
        let main_thread = MainThread::for_test();
        let [document, doctype, html, _, title, text, body, comment] = document(&main_thread);
        assert_eq!(document.length(), 2);
        assert_eq!(doctype.length(), 0);
        assert_eq!(html.length(), 2);
        assert_eq!(title.length(), 1);
        assert_eq!(text.length(), 5);
        assert_eq!(body.length(), 1);
        assert_eq!(comment.length(), 3);
    }

    #[test]
    fn insertion_moves_boundary_points_past_the_child() {
        let main_thread = MainThread::for_test();
        let [document, _, html, head, _, _, body, _] = document(&main_thread);
        let [before, after] = live_ranges(document, &[((html, 1), (html, 1)), ((html, 0), (html, 2))])[..] else {
            unreachable!()
        };

        adjust_live_ranges_for_insertion(&main_thread, NodeRef::from(html), NodeRef::from(body), 3);
        assert_eq!(start_of(&main_thread, before), (html, 1));
        assert_eq!(end_of(&main_thread, before), (html, 1));
        assert_eq!(start_of(&main_thread, after), (html, 0));
        assert_eq!(end_of(&main_thread, after), (html, 5));

        adjust_live_ranges_for_insertion(&main_thread, NodeRef::from(html), NodeRef::from(head), 1);
        assert_eq!(start_of(&main_thread, before), (html, 2));
        assert_eq!(start_of(&main_thread, after), (html, 0));
        assert_eq!(end_of(&main_thread, after), (html, 6));
    }

    #[test]
    fn pre_remove_steps_move_boundary_points_out_of_the_node() {
        let main_thread = MainThread::for_test();
        let [document, _, html, head, _, text, _, comment] = document(&main_thread);
        let [inside, after, elsewhere] = live_ranges(
            document,
            &[
                ((text, 1), (comment, 2)),
                ((html, 1), (html, 2)),
                ((comment, 0), (comment, 1)),
            ],
        )[..] else {
            unreachable!()
        };

        assert!(run_live_range_pre_remove_steps(&main_thread, head));
        assert_eq!(start_of(&main_thread, inside), (html, 0));
        assert_eq!(end_of(&main_thread, inside), (comment, 2));
        assert_eq!(inside.flags.get(), RANGE_BOUNDARY_POINT_SET_BY_MUTATION);
        assert_eq!(start_of(&main_thread, after), (html, 0));
        assert_eq!(end_of(&main_thread, after), (html, 1));
        assert_eq!(after.flags.get(), 0);
        assert_eq!(start_of(&main_thread, elsewhere), (comment, 0));
        assert_eq!(end_of(&main_thread, elsewhere), (comment, 1));

        assert!(!run_live_range_pre_remove_steps(&main_thread, text));
    }

    #[test]
    fn pre_remove_steps_for_all_children_collapse_onto_the_parent() {
        let main_thread = MainThread::for_test();
        let [document, _, html, _, title, text, _, comment] = document(&main_thread);
        let [inside, on_parent, outside] = live_ranges(
            document,
            &[
                ((text, 1), (comment, 2)),
                ((html, 1), (html, 2)),
                ((document, 1), (document, 2)),
            ],
        )[..] else {
            unreachable!()
        };

        assert!(run_live_range_pre_remove_steps_for_all_children(&main_thread, html));
        assert_eq!(start_of(&main_thread, inside), (html, 0));
        assert_eq!(end_of(&main_thread, inside), (html, 0));
        assert_eq!(start_of(&main_thread, on_parent), (html, 0));
        assert_eq!(end_of(&main_thread, on_parent), (html, 0));
        assert_eq!(on_parent.flags.get(), 0);
        assert_eq!(start_of(&main_thread, outside), (document, 1));
        assert_eq!(end_of(&main_thread, outside), (document, 2));

        assert!(!run_live_range_pre_remove_steps_for_all_children(&main_thread, title));
    }

    #[test]
    fn replacing_data_moves_boundary_points_in_the_node() {
        let main_thread = MainThread::for_test();
        let [document, _, _, _, title, text, _, _] = document(&main_thread);
        let [range, skipped, unaffected] = live_ranges(
            document,
            &[((text, 1), (text, 5)), ((text, 1), (text, 5)), ((text, 0), (title, 1))],
        )[..] else {
            unreachable!()
        };

        adjust_live_ranges_for_replaced_data(&main_thread, text, 0, 2, 3, Some(skipped));
        assert_eq!(start_of(&main_thread, range), (text, 0));
        assert_eq!(end_of(&main_thread, range), (text, 6));
        assert_eq!(start_of(&main_thread, skipped), (text, 1));
        assert_eq!(end_of(&main_thread, skipped), (text, 5));
        assert_eq!(start_of(&main_thread, unaffected), (text, 0));
        assert_eq!(end_of(&main_thread, unaffected), (title, 1));
    }

    #[test]
    fn splitting_a_text_node_moves_boundary_points_to_the_new_node() {
        let main_thread = MainThread::for_test();
        let [document, _, _, _, title, text, _, _] = document(&main_thread);
        let new_node = character_data(&main_thread, NodeType::TEXT_NODE, "llo");
        set_document(new_node, document);
        append_child(title, new_node);
        let [range, before] = live_ranges(document, &[((text, 3), (title, 1)), ((text, 1), (text, 2))])[..] else {
            unreachable!()
        };

        adjust_live_ranges_for_split_text(&main_thread, text, new_node, 2, title);
        assert_eq!(start_of(&main_thread, range), (new_node, 1));
        assert_eq!(end_of(&main_thread, range), (title, 2));
        assert_eq!(start_of(&main_thread, before), (text, 1));
        assert_eq!(end_of(&main_thread, before), (text, 2));
    }

    #[test]
    fn normalizing_moves_boundary_points_into_the_first_text_node() {
        let main_thread = MainThread::for_test();
        let [document, _, _, _, title, text, _, _] = document(&main_thread);
        let next = character_data(&main_thread, NodeType::TEXT_NODE, "cd");
        set_document(next, document);
        append_child(title, next);
        let [in_next, between] = live_ranges(document, &[((next, 1), (title, 2)), ((title, 1), (title, 1))])[..] else {
            unreachable!()
        };

        adjust_live_ranges_for_normalize(&main_thread, text, next, 5);
        assert_eq!(start_of(&main_thread, in_next), (text, 6));
        assert_eq!(end_of(&main_thread, in_next), (title, 2));
        assert_eq!(start_of(&main_thread, between), (text, 5));
        assert_eq!(end_of(&main_thread, between), (text, 5));
    }

    #[test]
    fn setting_the_start_or_end_keeps_the_range_ordered() {
        let main_thread = MainThread::for_test();
        let [document, doctype, html, _, _, text, body, _] = document(&main_thread);
        let range = range_core((html, 0), (html, 1));

        assert_eq!(
            set_the_start_or_end(&main_thread, &range, doctype, 0, StartOrEnd::Start),
            RangeException::InvalidNodeTypeError
        );
        assert_eq!(
            set_the_start_or_end(&main_thread, &range, html, 3, StartOrEnd::Start),
            RangeException::IndexSizeError
        );
        assert_eq!(range.start(&main_thread), (html, 0));
        assert_eq!(range.end(&main_thread), (html, 1));

        assert_eq!(
            set_the_start_or_end(&main_thread, &range, body, 0, StartOrEnd::Start),
            RangeException::None
        );
        assert_eq!(range.start(&main_thread), (body, 0));
        assert_eq!(range.end(&main_thread), (body, 0));

        assert_eq!(
            set_the_start_or_end(&main_thread, &range, text, 2, StartOrEnd::End),
            RangeException::None
        );
        assert_eq!(range.start(&main_thread), (text, 2));
        assert_eq!(range.end(&main_thread), (text, 2));

        assert_eq!(
            set_the_start_or_end(&main_thread, &range, document, 2, StartOrEnd::End),
            RangeException::None
        );
        assert_eq!(range.start(&main_thread), (text, 2));
        assert_eq!(range.end(&main_thread), (document, 2));

        let other_tree = node(&main_thread, NodeType::ELEMENT_NODE);
        assert_eq!(
            set_the_start_or_end(&main_thread, &range, other_tree, 0, StartOrEnd::End),
            RangeException::None
        );
        assert_eq!(range.start(&main_thread), (other_tree, 0));
        assert_eq!(range.end(&main_thread), (other_tree, 0));
    }

    #[test]
    fn range_methods_compare_against_the_boundary_points() {
        let main_thread = MainThread::for_test();
        let [document, doctype, html, head, title, text, body, comment] = document(&main_thread);
        let range = range_core((text, 2), (body, 1));
        let other = range_core((html, 1), (html, 2));
        let value = RangeComparison::value;
        let exception = RangeComparison::exception;

        assert_eq!(
            compare_boundary_points(&main_thread, &range, 4, &other),
            exception(RangeException::NotSupportedError)
        );
        let other_tree = range_core(
            (node(&main_thread, NodeType::ELEMENT_NODE), 0),
            (node(&main_thread, NodeType::ELEMENT_NODE), 0),
        );
        assert_eq!(
            compare_boundary_points(&main_thread, &range, START_TO_START, &other_tree),
            exception(RangeException::WrongDocumentError)
        );
        assert_eq!(
            compare_boundary_points(&main_thread, &range, START_TO_START, &other),
            value(-1)
        );
        assert_eq!(
            compare_boundary_points(&main_thread, &range, START_TO_END, &other),
            value(1)
        );
        assert_eq!(
            compare_boundary_points(&main_thread, &range, END_TO_END, &other),
            value(-1)
        );
        assert_eq!(
            compare_boundary_points(&main_thread, &range, END_TO_START, &other),
            value(-1)
        );
        assert_eq!(
            compare_boundary_points(&main_thread, &other, END_TO_END, &other),
            value(0)
        );

        assert_eq!(get_the_common_ancestor(&main_thread, &range), html);
        assert_eq!(
            get_the_common_ancestor(&main_thread, &range_core((text, 0), (text, 1))),
            text
        );
        assert_eq!(
            get_the_common_ancestor(&main_thread, &range_core((title, 0), (text, 1))),
            title
        );

        assert_eq!(is_point_in_range(&main_thread, &range, text, 2), value(1));
        assert_eq!(is_point_in_range(&main_thread, &range, text, 1), value(0));
        assert_eq!(is_point_in_range(&main_thread, &range, comment, 3), value(1));
        assert_eq!(is_point_in_range(&main_thread, &range, document, 2), value(0));
        assert_eq!(
            is_point_in_range(&main_thread, &range, doctype, 0),
            exception(RangeException::InvalidNodeTypeError)
        );
        assert_eq!(
            is_point_in_range(&main_thread, &range, text, 6),
            exception(RangeException::IndexSizeError)
        );
        let detached = node(&main_thread, NodeType::ELEMENT_NODE);
        assert_eq!(is_point_in_range(&main_thread, &range, detached, 0), value(0));

        assert_eq!(compare_point(&main_thread, &range, head, 0), value(-1));
        assert_eq!(compare_point(&main_thread, &range, body, 0), value(0));
        assert_eq!(compare_point(&main_thread, &range, html, 2), value(1));
        assert_eq!(
            compare_point(&main_thread, &range, detached, 0),
            exception(RangeException::WrongDocumentError)
        );
        assert_eq!(
            compare_point(&main_thread, &range, text, 6),
            exception(RangeException::IndexSizeError)
        );

        assert!(intersects_node(&main_thread, &range, document));
        assert!(intersects_node(&main_thread, &range, head));
        assert!(intersects_node(&main_thread, &range, comment));
        assert!(intersects_node(&main_thread, &range, text));
        assert!(!intersects_node(&main_thread, &range, doctype));
        assert!(!intersects_node(&main_thread, &range, detached));
        assert!(!intersects_node(
            &main_thread,
            &range_core((body, 0), (body, 0)),
            comment
        ));
    }
}

// NB: AbstractRange.cpp asserts the same layout of the struct it includes.
const _: () = assert!(size_of::<RangeCore>() == 20 && align_of::<RangeCore>() == 4);
const _: () = assert!(offset_of!(RangeCore, start_container) == 0);
const _: () = assert!(offset_of!(RangeCore, start_offset) == 4);
const _: () = assert!(offset_of!(RangeCore, end_container) == 8);
const _: () = assert!(offset_of!(RangeCore, end_offset) == 12);
const _: () = assert!(offset_of!(RangeCore, flags) == 16);
