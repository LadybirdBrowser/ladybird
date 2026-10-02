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
/// The arena must remain valid for the duration of the call, and `root` must name a live node
/// in this arena that has no parent. Every C++-side detach preparation that walks the subtree
/// must already have run. Every shell in the subtree is destroyed before this returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_free_subtree(arena: *mut c_void, root: NodeSlotId) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: The C++ wrapper keeps the arena alive for this call and serializes all access on
    // the document thread.
    crate::layout::tree_mutation::free_subtree_and_destroy_shells(&main_thread, arena.cast::<LayoutNodeArena>(), root);
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena. Every C++-side detach preparation that walks the subtree must already have
/// run. The node and every shell in its subtree are destroyed before this returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_detach_and_free_subtree(arena: *mut c_void, node: NodeSlotId) -> bool {
    assert!(!arena.is_null(), "layout node arena handle is null");
    let arena = arena.cast::<LayoutNodeArena>();
    // SAFETY: The C++ wrapper keeps the arena alive for this call and serializes all access on
    // the document thread; the shared borrow ends before the subtree is freed.
    let was_attached = unsafe { &*arena }.detach_from_parent(node);
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena.cast()) };
    crate::layout::tree_mutation::free_subtree_and_destroy_shells(&main_thread, arena, node);
    was_attached
}

/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_pre_order_label_violation_count(arena: *mut c_void, root: NodeSlotId) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    let arena = unsafe { &*arena.cast::<LayoutNodeArena>() };
    if arena.shell_if_live(&main_thread, root).is_null() {
        return 0;
    }
    let mut violation_count = 0u64;
    let mut previous_label: Option<u64> = None;
    arena.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
        let label = arena.node_pre_order_label(node);
        if previous_label.is_some_and(|previous| label <= previous) {
            violation_count += 1;
        }
        previous_label = Some(label);
    });
    violation_count
}

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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_link_shell(
    arena: *mut c_void,
    id: NodeSlotId,
    link: FfiNodeLink,
) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_link_shell(&main_thread, id, link)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_containing_block_shell_if_live(
    arena: *mut c_void,
    id: NodeSlotId,
) -> *mut c_void {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_containing_block_shell_if_live(&main_thread, id)
}

/// The arena and record must be live on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_adopt_derived_node_style(arena: *mut c_void, node: NodeSlotId, record: u64) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    let derived = arena.with_style_engine(|engine| DerivedStyleRecord::pin(engine, record));
    arena.apply_reinherited_style_record(&main_thread, node, derived);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_layout_display(arena: *mut c_void, node: NodeSlotId, display: u32) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    unsafe { LayoutNodeArena::from_handle(arena) }.update_layout_style(&main_thread, node, |style| {
        style.set_display(crate::css::display::FfiDisplay::from_raw(display));
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_reinherit_anonymous_descendants(arena: *mut c_void, node: NodeSlotId) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.reinherit_anonymous_descendants(&main_thread, node);
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
    visit: unsafe extern "C" fn(*mut c_void, *mut c_void),
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
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
        // SAFETY: The callback receives a layout node the arena keeps alive.
        unsafe { visit(context, arena.node_shell(&main_thread, root)) };
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
