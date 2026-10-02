/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// # Safety
///
/// The arena must remain valid for the duration of the call. `id` may be
/// invalid or stale; null is returned in that case.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_shell_if_live(arena: *mut c_void, id: NodeSlotId) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.shell_if_live(&main_thread, id)
}

/// The arena and record must be live on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_adopt_derived_node_style(arena: *mut c_void, node: NodeSlotId, record: u64) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    let derived = arena.with_style_engine(|engine| DerivedStyleRecord::pin(engine, record));
    arena.apply_reinherited_style_record(HostCalls::Now(&main_thread), node, derived);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_layout_display(arena: *mut c_void, node: NodeSlotId, display: u32) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    unsafe { LayoutNodeArena::from_handle(arena) }.update_layout_style(HostCalls::Now(&main_thread), node, |style| {
        style.set_display(crate::css::display::FfiDisplay::from_raw(display));
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_reinherit_anonymous_descendants(arena: *mut c_void, node: NodeSlotId) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.reinherit_anonymous_descendants(HostCalls::Now(&main_thread), node);
}

/// Visits every subtree root the last layout tree build rebuilt and left live, as the row's layout
/// node. Anonymous roots stand for no DOM node and are skipped; the host resolves the rest.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread, and `visit` must return synchronously
/// without mutating the arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_for_each_pending_rebuilt_subtree_root(
    arena: *mut c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, crate::painting::host::FfiNodeIdentity),
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above; the roots are copied out so no borrow spans the callback.
    let roots = unsafe { &*arena.cast::<LayoutNodeArena>() }
        .pending_rebuilt_subtree_roots
        .borrow()
        .clone();
    for root in roots {
        // SAFETY: As above.
        let arena = unsafe { &*arena.cast::<LayoutNodeArena>() };
        if !arena.node_is_dom_backed(root) {
            continue;
        }
        // SAFETY: The callback copies the node it is handed synchronously.
        unsafe {
            visit(
                context,
                crate::painting::hit_test::resolve::row_node_identity(arena, root, false),
            );
        };
    }
}

/// Gives the image box `slot` the image provider it owns, which the arena destroys with the row.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread, and `provider` a live provider the
/// caller hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_owned_image_provider(
    arena: *mut c_void,
    slot: NodeSlotId,
    provider: *mut c_void,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    host_tables(&main_thread).set_owned_image_provider(slot, provider);
}

/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_owned_image_provider(arena: *mut c_void, slot: NodeSlotId) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    host_tables(&main_thread).owned_image_provider(slot)
}

/// Gives `slot` the image observer set `observers`, or none for null, and hands the caller the set
/// it held, or null.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread, and `observers` null or a live set the
/// caller hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_replace_image_observers(
    arena: *mut c_void,
    slot: NodeSlotId,
    observers: *mut c_void,
) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    host_tables(&main_thread).replace_image_observers(slot, observers)
}

/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_image_observers(arena: *mut c_void, slot: NodeSlotId) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    host_tables(&main_thread).image_observers(slot)
}

fn host_tables<'host>(main_thread: &crate::stage::MainThread<'host>) -> &'host crate::layout::HostTables {
    main_thread
        .host_tables()
        .expect("an entry point's token carries its arena's host tables")
}

/// Prepares the row `slot` for leaving the layout tree. See [`prepare_row_for_detach`].
///
/// # Safety
///
/// `arena` must be a live handle on the document thread, and `slot` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_prepare_node_for_detach(arena: *mut c_void, slot: NodeSlotId) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: As above.
    prepare_row_for_detach(
        HostCalls::Now(&main_thread),
        unsafe { LayoutNodeArena::from_handle(arena) },
        slot,
    );
}

/// Prepares every row of the subtree `root` heads for leaving the layout tree.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread, and `root` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_prepare_subtree_for_detach(arena: *mut c_void, root: NodeSlotId) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: As above.
    prepare_subtree_for_detach(
        HostCalls::Now(&main_thread),
        unsafe { LayoutNodeArena::from_handle(arena) },
        root,
    );
}

/// Clears the committed box of every row of the subtree `root` heads and prepares each for leaving the layout tree,
/// as a removal does before it drops the subtree. The host lets go of its rows first: the drop that follows changes
/// what they are, so the host reads none of them again, and these writes go in place rather than to copies of the
/// chunks its rows share.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread, and `root` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_prepare_subtree_for_removal(arena: *mut c_void, root: NodeSlotId) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    if let Some(host) = main_thread.host() {
        host.let_go_of_rows();
    }
    let mut rows = Vec::new();
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }
        .for_each_node_in_layout_subtree_in_pre_order(root, |row| rows.push(row));
    for &row in &rows {
        // SAFETY: As above.
        unsafe { crate::painting::ffi::paintable_cleared_from_node(HostCalls::Now(&main_thread), arena, row) };
    }
    // SAFETY: As above.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    for row in rows {
        prepare_row_for_detach(HostCalls::Now(&main_thread), arena, row);
    }
}

/// Detaches the layout subtree `root` heads from its parent, if it has one, and frees it, every C++-side detach
/// preparation that walks the subtree having run. Answers whether the subtree was attached.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_drop_subtree(
    host: *mut crate::render_state::DocumentHost,
    root: NodeSlotId,
) -> bool {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    let written =
        crate::layout::layout_changes::write(host, crate::layout::layout_changes::LayoutWrite::DropSubtree { root });
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry_with_host(&MAIN_THREAD_FFI_ENTRY, host) };
    pay_for_write(&main_thread, host, written.host_work);
    written.was_attached
}

/// Detaches the layout placement of the top layer element `element` and clears every stale projected subtree of it.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_detach_top_layer_element(
    host: *mut crate::render_state::DocumentHost,
    element: u32,
) {
    assert!(!host.is_null(), "document host is null");
    // A top layer member the style engine no longer tracks has left the DOM. Nothing of it is in the mirror, and
    // nothing of it is bound to a row, so there is nothing to detach or clear.
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    let written = crate::layout::layout_changes::write(
        host,
        crate::layout::layout_changes::LayoutWrite::DetachTopLayerElement(element),
    );
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry_with_host(&MAIN_THREAD_FFI_ENTRY, host) };
    pay_for_write(&main_thread, host, written.host_work);
}

/// Pays what a layout write owed the host once the write is over. Telling the host which nodes gained or lost a box,
/// and a layout node of its row's style, still reads the arena the write left.
fn pay_for_write(
    main_thread: &crate::stage::MainThread,
    host: &crate::render_state::DocumentHost,
    host_work: crate::layout::tree_mutation::OwedHostWork,
) {
    let arena = host.arena_for_unconverted_entry();
    // SAFETY: The render state of a live host's document is on this thread, and the write is over.
    host_work.apply(main_thread, unsafe { LayoutNodeArena::from_handle(arena) });
}
