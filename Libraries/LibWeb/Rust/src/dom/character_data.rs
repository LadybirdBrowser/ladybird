/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Replacing the data of a CharacterData node, on the data in its core.
//! https://dom.spec.whatwg.org/#concept-cd-replace
//!
//! Rust builds the new data, stores it, and moves the live ranges. What the replacement does to the systems whose
//! state C++ keeps, mutation records, the editing history, the children changed steps of the node's parent, and the
//! style and layout of the node's text, C++ does in one callback, which Rust skips where it has nothing to do.

use super::hooks;
use super::hooks::CHARACTER_DATA_DATA_CHANGED;
use super::hooks::CHARACTER_DATA_NOTIFY_EDITING_HISTORY;
use super::hooks::CHARACTER_DATA_QUEUE_MUTATION_RECORD;
use super::hooks::CHARACTER_DATA_RUN_CHILDREN_CHANGED_STEPS;
use super::insert::insert;
use super::mutation_observers::character_data_mutation_record_is_heard;
use super::node::NodeCell;
use super::node_core::DOCUMENT_HAS_EDITING_HISTORY;
use super::node_core::DOCUMENT_PRESERVES_SELECTION_OFFSETS_DURING_IDENTICAL_REPLACEMENT;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use super::queries::concatenated_data;
use super::queries::is_text;
use super::range::adjust_live_ranges_for_normalize;
use super::range::adjust_live_ranges_for_replaced_data;
use super::range::adjust_live_ranges_for_split_text;
use super::remove::remove;
use super::rooted_nodes::RootedNodeListCore;
use super::rooted_nodes::RootedNodes;
use super::strings::BorrowedString;
use super::strings::raw_strings_equal;
use crate::stage::MainThread;
use ak::Utf16StringUnits;

crate::stage::main_thread_ffi_entries!(without_host);

/// The data `data` with `count` code units from `offset` on replaced by `replacement`.
fn replaced_data(
    data: &Utf16StringUnits<'_>,
    offset: usize,
    count: usize,
    replacement: &Utf16StringUnits<'_>,
) -> ak::Utf16String {
    use Utf16StringUnits::*;
    if let (Ascii(data) | Latin1(data), Ascii(replacement) | Latin1(replacement)) = (data, replacement) {
        let pieces = [&data[..offset], *replacement, &data[offset + count..]];
        let length = pieces.iter().map(|piece| piece.len()).sum();
        let is_ascii = pieces.iter().all(|piece| piece.is_ascii());
        return ak::Utf16String::from_latin1_concatenation(length, is_ascii, pieces);
    }
    fn push(code_units: &mut Vec<u16>, units: &Utf16StringUnits<'_>, range: core::ops::Range<usize>) {
        match units {
            Ascii(units) | Latin1(units) => code_units.extend(units[range].iter().map(|&unit| u16::from(unit))),
            Utf16(units) => code_units.extend_from_slice(&units[range]),
        }
    }
    let data_length = units_length(data);
    let replacement_length = units_length(replacement);
    let mut code_units = Vec::with_capacity(data_length - count + replacement_length);
    push(&mut code_units, data, 0..offset);
    push(&mut code_units, replacement, 0..replacement_length);
    push(&mut code_units, data, offset + count..data_length);
    ak::Utf16String::from_utf16(&code_units)
}

#[inline(always)]
fn units_length(units: &Utf16StringUnits<'_>) -> usize {
    match units {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.len(),
        Utf16StringUnits::Utf16(units) => units.len(),
    }
}

/// https://dom.spec.whatwg.org/#concept-cd-replace
/// Answers false where it throws an "IndexSizeError" DOMException. `data_string` is a string with the code units of
/// `data`, where the caller has one, which a replacement of the whole data shares.
fn replace_data(
    main_thread: &mut MainThread<'_>,
    node: NodeRef,
    offset: usize,
    count: usize,
    data: Utf16StringUnits<'_>,
    data_string: Option<BorrowedString>,
) -> bool {
    let field = core::ptr::from_ref(node.read(main_thread).data());
    // SAFETY: The node is live, and keeps its data until it is replaced below.
    let old_units = unsafe { (*field).units() };

    // 1. Let length be node's length.
    let length = units_length(&old_units);

    // 2. If offset is greater than length, then throw an "IndexSizeError" DOMException.
    if offset > length {
        return false;
    }

    // 3. If offset + count is greater than length, then set count to length − offset.
    let count = count.min(length - offset);

    // 4. Queue a mutation record of "characterData" for node with null, null, node's data, « », « », null, and null.
    // NB: The hook below queues the record, once the data is replaced, so that the mutation observer may notify UI
    //     clients of the node's new value.
    let mut steps = 0;
    if character_data_mutation_record_is_heard(node) {
        steps |= CHARACTER_DATA_QUEUE_MUTATION_RECORD;
    }

    // 5. Insert data into node's data after offset code units.
    // 6. Let deleteOffset be offset + data's length.
    // 7. Starting from deleteOffset code units, remove count code units from node's data.
    let data_length = units_length(&data);
    let new_data = match data_string {
        Some(data_string) if offset == 0 && count == length => data_string.to_utf16_string(),
        _ => replaced_data(&old_units, offset, count, &data),
    };
    // SAFETY: The node is live, nothing borrows its data past here, as old_units is not read again, and the old and the
    //         new data are live strings.
    let (old_data, data_changed) = unsafe {
        let old_data = (*field).replace(new_data);
        let data_changed = !raw_strings_equal(old_data.raw_identity(), (*field).raw());
        (old_data, data_changed)
    };

    let document_flags = node.document_core().flags();
    let range_to_skip = match !data_changed
        && document_flags & DOCUMENT_PRESERVES_SELECTION_OFFSETS_DURING_IDENTICAL_REPLACEMENT != 0
    {
        true => hooks::selection_range(main_thread, node.document()),
        false => None,
    };

    // 8. For each live range whose start node is node and start offset is greater than offset but less than or equal
    //    to offset + count: set its start offset to offset.
    // 9. For each live range whose end node is node and end offset is greater than offset but less than or equal to
    //    offset + count: set its end offset to offset.
    // 10. For each live range whose start node is node and start offset is greater than offset + count: increase its
    //     start offset by data's length and decrease it by count.
    // 11. For each live range whose end node is node and end offset is greater than offset + count: increase its end
    //     offset by data's length and decrease it by count.
    adjust_live_ranges_for_replaced_data(
        main_thread,
        node.read(main_thread),
        offset,
        count,
        data_length,
        // SAFETY: The selection keeps its range alive, and nothing changes it until the live ranges are moved.
        range_to_skip.map(|range| unsafe { range.as_ref() }.core()),
    );

    // 12. If node's parent is non-null, then run the children changed steps for node's parent.
    if node.parent().is_some() {
        steps |= CHARACTER_DATA_RUN_CHILDREN_CHANGED_STEPS;
    }

    // NB: Mutations during a recorded editing command must go through the Editing proxy functions.
    if document_flags & DOCUMENT_HAS_EDITING_HISTORY != 0 {
        steps |= CHARACTER_DATA_NOTIFY_EDITING_HISTORY;
    }
    // OPTIMIZATION: Data replaced with the same data leaves the rest as it is.
    if data_changed {
        steps |= CHARACTER_DATA_DATA_CHANGED;
    }
    if steps != 0 {
        hooks::character_data_replaced(
            main_thread,
            node,
            old_data.raw_identity(),
            offset,
            count,
            data_length,
            steps,
        );
    }
    true
}

/// The code units of the view C++ passes for `latin1`, `utf16` and `length`: `length` Latin-1 code units at `latin1`
/// where it is not null, and UTF-16 code units at `utf16` otherwise.
///
/// # Safety
///
/// The code units must be live for the lifetime the caller picks.
#[inline(always)]
unsafe fn units_of_view<'a>(latin1: *const u8, utf16: *const u16, length: usize) -> Utf16StringUnits<'a> {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        match latin1.is_null() {
            false => Utf16StringUnits::Latin1(ak::slice_from_raw_parts(latin1, length)),
            true => Utf16StringUnits::Utf16(ak::slice_from_raw_parts(utf16, length)),
        }
    }
}

/// "Replace data" of the CharacterData node, with the data whose code units C++ passes as a view of `length` Latin-1
/// code units at `latin1`, where it is not null, or UTF-16 code units at `utf16`, and that is the string whose raw word
/// is `data_string`, where it is not 0. Answers false where it throws an "IndexSizeError" DOMException.
/// https://dom.spec.whatwg.org/#concept-cd-replace
///
/// # Safety
///
/// The node must be a live CharacterData node, the code units live for the call, `data_string` the raw word of a live
/// string with those code units or 0, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_replace_data(
    node: &NodeCell,
    offset: usize,
    count: usize,
    latin1: *const u8,
    utf16: *const u16,
    length: usize,
    data_string: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // SAFETY: The code units and the string are live for the call.
    let (data, data_string) = unsafe {
        (
            units_of_view(latin1, utf16, length),
            (data_string != 0).then(|| BorrowedString::from_raw(data_string)),
        )
    };
    replace_data(&mut main_thread, node, offset, count, data, data_string)
}

/// "Substring data" of the CharacterData node with `offset` and `count`: the raw word of a reference to the string it
/// answers, which passes to the caller, or 0 where it throws an "IndexSizeError" DOMException.
/// https://dom.spec.whatwg.org/#concept-cd-substring
///
/// # Safety
///
/// The node must be a live CharacterData node, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_substring_data(node: &NodeCell, offset: usize, count: usize) -> usize {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    let data = node.read(&main_thread).data();
    let units = data.units();

    // 1. Let length be node's length.
    let length = units_length(&units);

    // 2. If offset is greater than length, then throw an "IndexSizeError" DOMException.
    if offset > length {
        return 0;
    }

    // 3. If offset + count is greater than length, return a string whose value is the code units from the offsetth
    //    code unit to the end of node's data, and then return.
    // 4. Return a string whose value is the code units from the offsetth code unit to the offset+countth code unit in
    //    node's data.
    let end = offset + count.min(length - offset);
    let substring = match units {
        // OPTIMIZATION: The whole data is the data's own string.
        _ if offset == 0 && end == length => data.to_utf16_string(),
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => {
            let piece = &units[offset..end];
            ak::Utf16String::from_latin1_concatenation(piece.len(), piece.is_ascii(), [piece])
        }
        Utf16StringUnits::Utf16(units) => ak::Utf16String::from_utf16(&units[offset..end]),
    };
    substring.into_raw()
}

/// Runs steps 6 to 8 of splitting the Text node `node` at `offset`, which the caller checked against its length, with
/// `new_node`, the new Text node of step 5, which the caller roots. Gives back false where step 8 throws an
/// IndexSizeError, as it does where inserting the new node ran script that shortened the node's data.
/// https://dom.spec.whatwg.org/#concept-text-split
///
/// # Safety
///
/// The nodes must be live Text nodes, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_split_text(node: &NodeCell, new_node: &NodeCell, offset: usize) -> bool {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let [node, new_node] = [node, new_node].map(|cell| unsafe { NodeRef::from_cell(cell, &main_thread) });

    // 3. Let count be length minus offset.
    let count = units_length(&node.read(&main_thread).data().units()) - offset;

    // 6. Let parent be node's parent.
    // 7. If parent is not null, then:
    if let Some(parent) = node.parent() {
        // 1. Insert new node into parent before node's next sibling.
        insert(
            &mut main_thread,
            new_node,
            &[new_node],
            parent,
            node.next_sibling(),
            false,
        );

        // 2. For each live range whose start node is node and start offset is greater than offset, set its start node
        //    to new node and decrease its start offset by offset.
        // 3. For each live range whose end node is node and end offset is greater than offset, set its end node to new
        //    node and decrease its end offset by offset.
        // 4. For each live range whose start node is parent and start offset is equal to the index of node plus 1,
        //    increase its start offset by 1.
        // 5. For each live range whose end node is parent and end offset is equal to the index of node plus 1, increase
        //    its end offset by 1.
        adjust_live_ranges_for_split_text(
            &main_thread,
            node.read(&main_thread),
            new_node.read(&main_thread),
            offset,
            parent.read(&main_thread),
        );
    }

    // 8. Replace data with node node, offset offset, count count, and data the empty string.
    replace_data(
        &mut main_thread,
        node,
        offset,
        count,
        Utf16StringUnits::Ascii(&[]),
        None,
    )
}

/// Whether `node` is an exclusive Text node, a Text node that is not a CDATASection node.
/// https://dom.spec.whatwg.org/#exclusive-text-node
#[inline(always)]
fn is_exclusive_text(node: NodeRef) -> bool {
    node.node_type() == NodeType::TEXT_NODE
}

/// The contiguous exclusive Text nodes of `node`, excluding itself, in tree order.
/// https://dom.spec.whatwg.org/#contiguous-exclusive-text-nodes
fn contiguous_exclusive_text_nodes_excluding_itself(node: NodeRef) -> impl Iterator<Item = NodeRef> {
    // The contiguous exclusive Text nodes of a node node are node, node's previous sibling exclusive Text node, if any,
    // and its contiguous exclusive Text nodes, and node's next sibling exclusive Text node, if any, and its contiguous
    // exclusive Text nodes, avoiding any duplicates.
    let mut first = node;
    while let Some(previous) = first.previous_sibling().filter(|&previous| is_exclusive_text(previous)) {
        first = previous;
    }
    let mut current = Some(first);
    core::iter::from_fn(move || {
        loop {
            let candidate = current?;
            current = candidate.next_sibling().filter(|&next| is_exclusive_text(next));
            if candidate != node {
                return Some(candidate);
            }
        }
    })
}

/// The length of the CharacterData node `node`.
/// https://dom.spec.whatwg.org/#concept-node-length
#[inline(always)]
fn length_of(main_thread: &MainThread<'_>, node: NodeRef) -> usize {
    units_length(&node.read(main_thread).data().units())
}

/// https://dom.spec.whatwg.org/#dom-node-normalize
/// `nodes` is an empty list that holds the descendant exclusive Text nodes, which the steps remove from their trees.
fn normalize(main_thread: &mut MainThread<'_>, this: NodeRef, nodes: &mut RootedNodes<'_>) {
    // The normalize() method steps are to run these steps for each descendant exclusive Text node node of this:
    // NB: The nodes are collected first, as the steps change the tree.
    let mut current = this.first_child();
    while let Some(node) = current {
        if is_exclusive_text(node) {
            nodes.push(node);
        }
        current = node.first_child().or_else(|| {
            let mut up = node;
            loop {
                if up == this {
                    return None;
                }
                if let Some(next) = up.next_sibling() {
                    return Some(next);
                }
                up = up.parent().expect("a descendant of this has a parent");
            }
        });
    }

    for index in 0..nodes.len() {
        let node = nodes.get(index);

        // NB: A node that an earlier node merged removed from its parent is no longer a descendant of this.
        if node.parent().is_none() {
            continue;
        }

        // 1. Let length be node's length.
        let mut length = length_of(main_thread, node);

        // 2. If length is zero, then remove node and continue with the next exclusive Text node, if any.
        if length == 0 {
            remove(main_thread, node, false);
            continue;
        }

        // 3. Let data be the concatenation of the data of node's contiguous exclusive Text nodes (excluding itself), in
        //    tree order.
        let data = concatenated_data(main_thread, contiguous_exclusive_text_nodes_excluding_itself(node));

        // 4. Replace data with node node, offset length, count 0, and data data.
        let data_units = data.as_units();
        let replaced = replace_data(main_thread, node, length, 0, data_units, None);
        debug_assert!(replaced, "the offset is the node's length");
        drop(data);

        // 5. Let currentNode be node's next sibling.
        let mut current_node = node.next_sibling();

        // 6. While currentNode is an exclusive Text node:
        while let Some(text) = current_node.filter(|&current_node| is_exclusive_text(current_node)) {
            // 1. For each live range whose start node is currentNode: add length to its start offset and set its start
            //    node to node.
            // 2. For each live range whose end node is currentNode: add length to its end offset and set its end node
            //    to node.
            // 3. For each live range whose start node is currentNode's parent and start offset is currentNode's index:
            //    set its start node to node and its start offset to length.
            // 4. For each live range whose end node is currentNode's parent and end offset is currentNode's index: set
            //    its end node to node and its end offset to length.
            adjust_live_ranges_for_normalize(main_thread, node.read(main_thread), text.read(main_thread), length);

            // 5. Add currentNode's length to length.
            length += length_of(main_thread, text);

            // 6. Set currentNode to its next sibling.
            current_node = text.next_sibling();
        }

        // 7. Remove node's contiguous exclusive Text nodes (excluding itself), in tree order.
        // NB: Each removal changes the siblings, so the first one left is removed until none is.
        while let Some(text) = contiguous_exclusive_text_nodes_excluding_itself(node).next() {
            remove(main_thread, text, false);
        }
    }
}

/// Runs normalize() for `node`, with `nodes`, the core of an empty DOM::RootedNodeList that the caller makes for the
/// call.
/// https://dom.spec.whatwg.org/#dom-node-normalize
///
/// # Safety
///
/// The node must be live, `nodes` the core of a DOM::RootedNodeList that lives for the call, and the call must be made
/// on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_normalize(node: &NodeCell, nodes: &RootedNodeListCore) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // SAFETY: The list lives for the call, and only this code uses it.
    let mut nodes = unsafe { RootedNodes::from_core(nodes) };
    normalize(&mut main_thread, node, &mut nodes);
}

/// The raw word of a reference to the string of the Text node's wholeText, which passes to the caller.
/// https://dom.spec.whatwg.org/#dom-text-wholetext
///
/// # Safety
///
/// The node must be a live Text node, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_whole_text(node: &NodeCell) -> usize {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The node is live.
    let node = unsafe { NodeRef::from_cell(node, &main_thread) };
    // The wholeText getter steps are to return the concatenation of the data of the contiguous Text nodes of this, in
    // tree order.
    // https://dom.spec.whatwg.org/#contiguous-text-nodes
    // The contiguous Text nodes of a node node are node, node's previous sibling Text node, if any, and its contiguous
    // Text nodes, and node's next sibling Text node, if any, and its contiguous Text nodes, avoiding any duplicates.
    let mut first = node;
    while let Some(previous) = first.previous_sibling().filter(|node| is_text(*node)) {
        first = previous;
    }
    let mut current = Some(first);
    let nodes = core::iter::from_fn(move || {
        let node = current?;
        current = node.next_sibling().filter(|node| is_text(*node));
        Some(node)
    });
    concatenated_data(&main_thread, nodes).into_raw()
}
