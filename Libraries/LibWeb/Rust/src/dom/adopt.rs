/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Adopting a node into a document.
//! https://dom.spec.whatwg.org/#concept-node-adopt

use super::hooks;
use super::node::NodeCell;
use super::node_ref::NodeRef;
use super::remove::remove;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

/// https://dom.spec.whatwg.org/#concept-node-adopt
pub(crate) fn adopt(main_thread: &mut MainThread<'_>, node: NodeRef, document: NodeRef) {
    // To adopt a node node into a document document:

    // 1. Let oldDocument be node’s node document.
    let old_document = node.document();

    // 2. If node’s parent is non-null, then remove node.
    if node.parent().is_some() {
        remove(main_thread, node, false);
    }

    // 3. If document is not oldDocument, then for each inclusiveDescendant of node’s shadow-including inclusive
    //    descendants, in shadow-including tree order: ...
    // NB: The callback runs the steps for each inclusiveDescendant, and moves what the systems whose state is still
    //     C++'s keep for the subtree from oldDocument to document.
    if document != old_document {
        hooks::adopt_from_other_document(main_thread, node, old_document, document);
    }
}

/// Adopts `node` into `document`, for DOM::Document.
///
/// # Safety
///
/// The nodes must be live, `document` must be a document, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_adopt(node: &NodeCell, document: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The nodes are live.
    let reference = |cell| unsafe { NodeRef::from_cell(cell, &main_thread) };
    let (node, document) = (reference(node), reference(document));
    adopt(&mut main_thread, node, document);
}
