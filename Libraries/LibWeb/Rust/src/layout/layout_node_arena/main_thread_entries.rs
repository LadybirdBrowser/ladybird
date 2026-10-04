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
use crate::render_state::DocumentHost;

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// The row's layout node, which the host's shell factory makes the first time it is asked for, or null for a row that
/// is not live.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. `id` may be invalid or stale.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_node_shell_if_live(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    id: NodeSlotId,
) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let document_host = unsafe { &*host };
    if let Some(shell) = document_host.held_shell(id) {
        return shell;
    }
    if let Some(identities) = document_host.known_row_identities() {
        return identities.shell_facts(id).map_or(std::ptr::null_mut(), |facts| {
            document_host.host_tables().shell_of(facts)
        });
    }
    // SAFETY: Guaranteed by the caller.
    let facts = unsafe {
        read_arena(host, read, id, |arena, id| {
            let kind = arena.node_kind_if_live(id)?;
            (kind != NodeKind::Unset).then_some(super::super::host_tables::ShellFacts { id, kind })
        })
    };
    // SAFETY: As above.
    facts.map_or(std::ptr::null_mut(), |facts| {
        unsafe { host_tables(host) }.shell_of(facts)
    })
}

/// The row takes the derived style record `record` its layout node made.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `record` a live record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_adopt_derived_node_style(
    host: *const DocumentHost,
    node: NodeSlotId,
    record: u64,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::AdoptDerivedNodeStyle { node, record }) };
}

/// The row's layout style takes the display `display`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_layout_display(host: *const DocumentHost, node: NodeSlotId, display: u32) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::SetLayoutDisplay { node, display }) };
}

/// The anonymous rows below the row inherit its style again.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_reinherit_anonymous_descendants(host: *const DocumentHost, node: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::ReinheritAnonymousDescendants { node }) };
}

/// Visits every subtree root the last layout tree build rebuilt and left live, as the row's layout
/// node. Anonymous roots stand for no DOM node and are skipped; the host resolves the rest.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `visit` must return synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_for_each_pending_rebuilt_subtree_root(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, crate::painting::host::FfiNodeIdentity),
) {
    // SAFETY: Guaranteed by the caller.
    let roots = unsafe {
        read_arena(host, read, (), |arena, ()| {
            let roots = arena.pending_rebuilt_subtree_roots.borrow();
            roots
                .iter()
                .filter(|&&root| arena.node_is_dom_backed(root))
                .map(|&root| crate::painting::hit_test::resolve::row_node_identity(arena, root, false))
                .collect::<Vec<_>>()
        })
    };
    for root in roots {
        // SAFETY: The callback copies the node it is handed synchronously.
        unsafe { visit(context, root) };
    }
}

/// Gives the image box `slot` the image provider it owns, which the arena destroys with the row.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `provider` a live provider the caller hands
/// over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_owned_image_provider(
    host: *const DocumentHost,
    slot: NodeSlotId,
    provider: *mut c_void,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { host_tables(host).set_owned_image_provider(slot, provider) }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_owned_image_provider(
    host: *const DocumentHost,
    slot: NodeSlotId,
) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe { host_tables(host).owned_image_provider(slot) }
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
    host: *const DocumentHost,
    slot: NodeSlotId,
    observers: *mut c_void,
) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe { host_tables(host).replace_image_observers(slot, observers) }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_image_observers(host: *const DocumentHost, slot: NodeSlotId) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe { host_tables(host).image_observers(slot) }
}

/// The host tables of `host`'s document.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, which outlives the borrow.
unsafe fn host_tables<'a>(host: *const DocumentHost) -> &'a crate::layout::HostTables {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.host_tables()
}

/// Has the render state of `host`'s document make `write`, pays what the write owes the host, and answers whether the
/// subtree it dropped was attached.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn write_and_pay(
    host: *const DocumentHost,
    wait: impl crate::render_state::RenderWait,
    write: LayoutWrite,
) -> bool {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    let written = crate::layout::layout_changes::write(wait, host, write);
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, host) };
    written.host_work.pay(&main_thread);
    written.was_attached
}

/// Prepares the row `slot` for leaving the layout tree. See [`prepare_row_for_detach`].
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `slot` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_prepare_node_for_detach(host: *const DocumentHost, slot: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::PrepareRowForDetach { row: slot }) };
}

/// Prepares every row of the subtree `root` heads for leaving the layout tree.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `root` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_prepare_subtree_for_detach(host: *const DocumentHost, root: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::PrepareSubtreeForDetach { root }) };
}

/// Clears the committed box of every row of the subtree `root` heads and prepares each for leaving the layout tree,
/// as a removal does before it drops the subtree. The host lets go of its rows first: the drop that follows changes
/// what they are, so the host reads none of them again, and these writes go in place rather than to copies of the
/// chunks its rows share.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `root` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_prepare_subtree_for_removal(host: *const DocumentHost, root: NodeSlotId) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.let_go_of_rows();
    // SAFETY: As above.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::PrepareSubtreeForRemoval { root }) };
}

/// Detaches the layout subtree `root` heads from its parent, if it has one, and frees it, every C++-side detach
/// preparation that walks the subtree having run. Answers whether the subtree was attached.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_drop_subtree(
    host: *mut DocumentHost,
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
    host: *mut DocumentHost,
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
