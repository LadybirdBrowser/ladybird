/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host asks about the rows of its document's layout: what a layout node asks about its own row (its flags,
//! links and style), the row a node is bound to, and the shape and paint facts of a row. The host answers from the rows
//! the document's render state published, which it asks for again only after the host wrote them, so none of these
//! reaches the arena.

use super::node_data::{CompositorAnimationFrameKind, FfiNodeLink, NodeSlotId};
use super::row_reads::RowSnapshot;
use crate::css::style::tree::StyleNodeID;
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintSource};
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
    // SAFETY: Guaranteed by the caller.
    let linked = unsafe { rows(host) }.link(id, link);
    // SAFETY: As above.
    unsafe { shell_of(host, (!linked.is_invalid()).then_some(linked)) }
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

/// Answers `read` from the rows of `host`'s document through the paint side's reads.
///
/// # Safety
///
/// As for [`rows`].
unsafe fn read_rows<R>(host: *mut DocumentHost, read: impl FnOnce(&PaintSource<'_>) -> R) -> R {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.read_rows(LockstepProof::for_reason(&HOST_READS_ITS_OWN_WRITE), false, read)
}

/// The layout node of the live row `id`, made if nothing has asked for it yet, or null for none.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn shell_of(host: *mut DocumentHost, id: Option<NodeSlotId>) -> *mut c_void {
    // SAFETY: Guaranteed by the caller. The factory reads the rows again, through the shared host.
    let Some(facts) = id.and_then(|id| unsafe { rows(host) }.shell_facts(id)) else {
        return std::ptr::null_mut();
    };
    // SAFETY: As above.
    unsafe { &*host }.host_tables().shell_of(facts)
}

/// The layout node of the row the element or text node with `style_node` is bound to, made if nothing has asked for it
/// yet, or null if the node has no row.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_bound_shell(host: *mut DocumentHost, style_node: u32) -> *mut c_void {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return std::ptr::null_mut();
    };
    // SAFETY: Guaranteed by the caller.
    let row = unsafe { rows(host) }.bound_row(style_node);
    // SAFETY: As above.
    unsafe { shell_of(host, row) }
}

/// The layout node of the row the pseudo-element of kind `generated_for` on the element with `style_node` is bound to,
/// made if nothing has asked for it yet, or null.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_bound_pseudo_element_shell(
    host: *mut DocumentHost,
    style_node: u32,
    generated_for: u8,
) -> *mut c_void {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return std::ptr::null_mut();
    };
    // SAFETY: Guaranteed by the caller.
    let row = unsafe { rows(host) }.bound_pseudo_element_row(style_node, generated_for);
    // SAFETY: As above.
    unsafe { shell_of(host, row) }
}

/// The layout node of the viewport row the document is bound to, made if nothing has asked for it yet, or null.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_bound_viewport_shell(host: *mut DocumentHost) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let row = unsafe { rows(host) }.bound_viewport_row();
    // SAFETY: As above.
    unsafe { shell_of(host, row) }
}

/// The layout node of the box whose content box the row is laid out against, or null.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_containing_block_shell_if_live(
    host: *mut DocumentHost,
    id: NodeSlotId,
) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let containing_block = unsafe { read_rows(host, |rows| rows.node_containing_block_if_live(id)) };
    // SAFETY: As above.
    unsafe { shell_of(host, containing_block) }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_establishes_an_absolute_positioning_containing_block(
    host: *mut DocumentHost,
    id: NodeSlotId,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            crate::painting::style_queries::establishes_positioning_containing_blocks(rows, id).0
        })
    }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_establishes_a_fixed_positioning_containing_block(
    host: *mut DocumentHost,
    id: NodeSlotId,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            crate::painting::style_queries::establishes_positioning_containing_blocks(rows, id).1
        })
    }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_any_ancestor_establishes_a_fixed_position_containing_block(
    host: *mut DocumentHost,
    id: NodeSlotId,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            crate::painting::style_queries::any_ancestor_establishes_a_fixed_position_containing_block(rows, id)
        })
    }
}

/// The layout node of the row whose paintable row `slot` is, or null when the row has none.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_layout_node_shell(
    host: *mut DocumentHost,
    slot: NodeSlotId,
) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let populated = unsafe { rows(host) }.paintable.paintable_row_is_populated(slot);
    // SAFETY: As above.
    unsafe { shell_of(host, populated.then_some(slot)) }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_is_positioned(host: *mut DocumentHost, slot: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            rows.paintable_row_is_populated(slot) && crate::painting::style_queries::is_positioned(rows, slot)
        })
    }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_has_child_paintables(host: *mut DocumentHost, slot: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            crate::painting::paint_order::first_paint_child(rows, slot).is_some()
        })
    }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_is_chrome_mirrored(host: *mut DocumentHost, slot: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            crate::painting::chrome_geometry::is_chrome_mirrored(rows, slot)
        })
    }
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_has_css_transform(host: *mut DocumentHost, id: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            rows.node_style_if_live(id)
                .is_some_and(|style| crate::painting::style_queries::has_css_transform(rows, id, style))
        })
    }
}
