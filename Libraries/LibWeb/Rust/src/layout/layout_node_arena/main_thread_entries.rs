/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;
use crate::layout::layout_changes::LayoutWrite;
use crate::painting::paint_read::GeometryRead;
use crate::render_state::DocumentHost;

crate::stage::main_thread_ffi_entries!();

/// The row's layout node, which the host's shell factory makes the first time it is asked for, or null for a row that
/// is not live.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. `id` may be invalid or stale.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_node_shell_if_live(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    id: NodeSlotId,
) -> *mut c_void {
    if let Some(shell) = host.held_shell(id) {
        return shell;
    }
    if let Some(identities) = host.known_row_identities() {
        return identities
            .shell_facts(id)
            .map_or(std::ptr::null_mut(), |facts| host.host_tables().shell_of(facts));
    }
    // SAFETY: Guaranteed by the caller.
    let facts = unsafe {
        read_arena(host, read, id, |arena, id| {
            let kind = arena.node_kind_if_live(id)?;
            (kind != NodeKind::Unset).then_some(super::super::host_tables::ShellFacts { id, kind })
        })
    };
    facts.map_or(std::ptr::null_mut(), |facts| host.host_tables().shell_of(facts))
}

/// The row takes the derived style record `record` its layout node made.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `record` a live record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_adopt_derived_node_style(host: &DocumentHost, node: NodeSlotId, record: u64) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::AdoptDerivedNodeStyle { node, record }) };
}

/// The row's layout style takes the display `display`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_layout_display(host: &DocumentHost, node: NodeSlotId, display: u32) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::SetLayoutDisplay { node, display }) };
}

/// The anonymous rows below the row inherit its style again, which their layout nodes hear of once the host has its next
/// job back. A table box, which `is_table_box` says the row's style makes it, may itself take a record the arena derives
/// as its wrapper inherits its style again, which its layout node hears of before it reads its style again.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_reinherit_anonymous_descendants(
    host: &DocumentHost,
    node: NodeSlotId,
    is_table_box: bool,
) {
    if is_table_box {
        // SAFETY: Guaranteed by the caller.
        unsafe { write_and_pay(host, node_read(), LayoutWrite::ReinheritAnonymousDescendants { node }) };
        return;
    }
    host.queue_change(crate::render_state::ArenaChange::ReinheritAnonymousDescendants(node));
}

/// Gives the image box `slot` the image provider it owns, which the arena destroys with the row.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `provider` a live provider the caller hands
/// over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_owned_image_provider(
    host: &DocumentHost,
    slot: NodeSlotId,
    provider: *mut c_void,
) {
    host.host_tables().set_owned_image_provider(slot, provider);
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_owned_image_provider(host: &DocumentHost, slot: NodeSlotId) -> *mut c_void {
    host.host_tables().owned_image_provider(slot)
}

/// Gives `slot` the image observer set `observers`, or none for null, and hands the caller the set
/// it held, or null.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `observers` null or a live set the caller
/// hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_replace_image_observers(
    host: &DocumentHost,
    slot: NodeSlotId,
    observers: *mut c_void,
) -> *mut c_void {
    host.host_tables().replace_image_observers(slot, observers)
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
// SAFETY: Guaranteed by the caller.
pub unsafe extern "C" fn document_host_image_observers(host: &DocumentHost, slot: NodeSlotId) -> *mut c_void {
    host.host_tables().image_observers(slot)
}

/// Has the render state of `host`'s document make `write`, pays what the write owes the host, and answers whether the
/// subtree it dropped was attached.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn write_and_pay(host: &DocumentHost, wait: impl crate::render_state::RenderWait, write: LayoutWrite) -> bool {
    let written = crate::layout::layout_changes::write(wait, host, write);
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    written.host_work.pay(&main_thread);
    written.was_attached
}

/// Prepares every row of the subtree `root` heads for leaving the layout tree.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `root` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_prepare_subtree_for_detach(host: &DocumentHost, root: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::PrepareSubtreeForDetach { root }) };
}

/// Detaches the layout subtree `root` heads from its parent, if it has one, and frees it, every C++-side detach
/// preparation that walks the subtree having run. Answers whether the subtree was attached.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_drop_subtree(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    root: NodeSlotId,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, read, LayoutWrite::DropSubtree { root }) }
}

/// Detaches the layout placement of the top layer element `element` and clears every stale projected subtree of it.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_detach_top_layer_element(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    element: u32,
) {
    // A top layer member the style engine no longer tracks has left the DOM. Nothing of it is in the mirror, and
    // nothing of it is bound to a row, so there is nothing to detach or clear.
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, read, LayoutWrite::DetachTopLayerElement(element)) };
}
