/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a layout node asks about its own row: its flags, links and style. The host answers from the rows the document's
//! render state published, which it asks for again only after the host wrote them, so a layout node never reaches the
//! arena.

use super::node_data::{CompositorAnimationFrameKind, FfiNodeLink, NodeSlotId};
use super::row_reads::RowSnapshot;
use crate::render_state::{DocumentHost, LockstepProof};
use std::ffi::c_void;
use std::rc::Rc;

/// The reason the host waits for its document's rows as a layout node reads them: the host wrote them since they were
/// published, through a change or an entry that reaches the arena directly.
pub(crate) struct HostReadsItsOwnWrite {
    _private: (),
}

const HOST_READS_ITS_OWN_WRITE: HostReadsItsOwnWrite = HostReadsItsOwnWrite { _private: () };

/// The rows of `host`'s document as of every write the host made.
///
/// # Safety
///
/// `host` must be a live document host on its document's thread.
unsafe fn rows(host: *mut DocumentHost) -> Rc<RowSnapshot> {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.fresh_rows(LockstepProof::for_reason(&HOST_READS_ITS_OWN_WRITE))
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `id` the slot of a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_flags(host: *mut DocumentHost, id: NodeSlotId) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.flags(id)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_link_slot(
    host: *mut DocumentHost,
    id: NodeSlotId,
    link: FfiNodeLink,
) -> NodeSlotId {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.link(id, link)
}

/// The layout node of the row `id` links to, made if nothing has asked for it yet, or null.
///
/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_link_shell(
    host: *mut DocumentHost,
    id: NodeSlotId,
    link: FfiNodeLink,
) -> *mut c_void {
    // SAFETY: Guaranteed by the caller. The rows are let go of before the factory runs, which reads them again.
    let rows = unsafe { rows(host) };
    let Some(facts) = rows.shell_facts(rows.link(id, link)) else {
        return std::ptr::null_mut();
    };
    // SAFETY: As above.
    unsafe { &*host }.host_tables().shell_of(facts)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_generated_for(host: *mut DocumentHost, id: NodeSlotId) -> u8 {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.generated_for(id)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_has_compositor_animation_frame(
    host: *mut DocumentHost,
    id: NodeSlotId,
    kind: CompositorAnimationFrameKind,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.has_compositor_animation_frame(id, kind)
}

/// The style node of the row, or 0 for a row that is gone or carries none.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_style_node(host: *mut DocumentHost, id: NodeSlotId) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }
        .style_node(id)
        .map_or(0, crate::css::style::tree::StyleNodeID::raw)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_style_record(host: *mut DocumentHost, id: NodeSlotId) -> u64 {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.style_record(id)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_style_payloads(host: *mut DocumentHost, id: NodeSlotId) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.style_payloads(id).as_ptr().cast()
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_is_atomic_inline(host: *mut DocumentHost, id: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.is_atomic_inline(id)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_is_fragmented_inline(host: *mut DocumentHost, id: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host) }.is_fragmented_inline(id)
}
