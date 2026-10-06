/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host asks about the rows of its document's layout: what a layout node asks about its own row (its flags,
//! links and style), the row a node is bound to, and the shape and paint facts of a row. The host answers from the rows
//! the document's render state published, which it asks for again only after the host wrote them, so none of these
//! reaches the arena. What a row is and the row a node is bound to are read through [`RowIdentities`], which the host
//! reads again only after a write that changed them: installing a style, which a style update does for every element
//! it restyles, does not.
//!
//! The entries that find the layout node of an element, a document or a row take the read the host began, and the
//! reads of a node found so spend that read (see [`NodeRead`]).

use super::node_data::{CompositorAnimationFrameKind, FfiNodeLink, NodeSlotId};
use super::row_reads::{RowIdentities, RowSnapshot};
use crate::css::style::tree::StyleNodeID;
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintSource};
use crate::render_state::{BegunRead, DocumentHost, RenderWait};
use std::ffi::c_void;
use std::rc::Rc;

crate::render_state::held_node_entries!();

/// Answers `answer` from the render state of `host`'s document and `args`, as of every write the host made, spending
/// `wait`.
///
/// # Safety
///
/// `host` must be a live document host on its document's thread.
pub(crate) unsafe fn read_arena<A, R>(
    host: &DocumentHost,
    wait: impl RenderWait,
    args: A,
    answer: fn(&mut super::LayoutNodeArena, A) -> R,
) -> R {
    host.ask(wait, |state| answer(state.arena_mut(), args))
}

/// The rows of `host`'s document as of every write the host made, spending `wait`.
///
/// # Safety
///
/// `host` must be a live document host on its document's thread.
unsafe fn rows(host: &DocumentHost, wait: impl RenderWait) -> Rc<RowSnapshot> {
    host.fresh_rows(wait)
}

/// The style record each row of `host`'s document has, as of every write the host made, for a read of the row `id`,
/// spending `wait`.
///
/// # Safety
///
/// As for [`rows`].
unsafe fn styles(host: &DocumentHost, id: NodeSlotId, wait: impl RenderWait) -> crate::layout::row_reads::RowStyles {
    host.row_styles_of(id, wait)
}

/// What each row of `host`'s document is, and the row each node is bound to, as of every write the host made, spending
/// `wait`.
///
/// # Safety
///
/// As for [`rows`].
unsafe fn identities(host: &DocumentHost, wait: impl RenderWait) -> RowIdentities {
    host.row_identities(wait)
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `id` the slot of a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_flags(host: &DocumentHost, id: NodeSlotId) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host, node_read()) }.flags(id)
}

/// The row's flags that say what node it stands for (see [`super::node_data::NodeFlag::IDENTITY`]); every other flag
/// reads as unset.
///
/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_identity_flags(host: &DocumentHost, id: NodeSlotId) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { identities(host, node_read()) }.identity_flags(id)
}

/// The layout node of the row `id` links to, made if nothing has asked for it yet, or null.
///
/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_link_shell(host: &DocumentHost, id: NodeSlotId, link: FfiNodeLink) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let linked = unsafe { rows(host, node_read()) }.link(id, link);
    // SAFETY: As above.
    unsafe { shell_of(host, node_read(), (!linked.is_invalid()).then_some(linked)) }
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_generated_for(host: &DocumentHost, id: NodeSlotId) -> u8 {
    // SAFETY: Guaranteed by the caller.
    unsafe { identities(host, node_read()) }.generated_for(id)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_has_compositor_animation_frame(
    host: &DocumentHost,
    id: NodeSlotId,
    kind: CompositorAnimationFrameKind,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host, node_read()) }.has_compositor_animation_frame(id, kind)
}

/// The style node of the row, or 0 for a row that is gone or carries none.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_style_node(host: &DocumentHost, id: NodeSlotId) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { identities(host, node_read()) }
        .style_node(id)
        .map_or(0, crate::css::style::tree::StyleNodeID::raw)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_style_record(host: &DocumentHost, id: NodeSlotId) -> u64 {
    // SAFETY: Guaranteed by the caller.
    unsafe { styles(host, id, node_read()) }.style_record(id)
}

/// Whether the arena derived the style record of the row `id`, which only a job changes but for the layout node's own
/// writes, and a job tells the layout node of.
///
/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_style_is_derived(host: &DocumentHost, id: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { styles(host, id, node_read()) }.style_is_derived(id)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_style_payloads(host: &DocumentHost, id: NodeSlotId) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe { styles(host, id, node_read()) }
        .style_payloads(id)
        .as_ptr()
        .cast()
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_is_atomic_inline(host: &DocumentHost, id: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host, node_read()) }.is_atomic_inline(id)
}

/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_is_fragmented_inline(host: &DocumentHost, id: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { rows(host, node_read()) }.is_fragmented_inline(id)
}

/// Answers `read` from the rows of `host`'s document through the paint side's reads, for a layout node the host holds.
///
/// # Safety
///
/// As for [`rows`].
unsafe fn read_rows<R>(host: &DocumentHost, read: impl FnOnce(&PaintSource<'_>) -> R) -> R {
    host.read_rows(node_read(), false, read)
}

/// The layout node of the live row `id`, made if nothing has asked for it yet, or null for none, spending `wait`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn shell_of(host: &DocumentHost, wait: impl RenderWait, id: Option<NodeSlotId>) -> *mut c_void {
    let Some(id) = id else {
        return std::ptr::null_mut();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { bound_shell(host, wait, |_| Some(id)) }
}

/// The layout node of the live row `bound` finds among what each row is and the row each node is bound to, made if
/// nothing has asked for it yet, or null for none, spending `wait`: the identities are read once for both.
///
/// # Safety
///
/// As for [`shell_of`].
unsafe fn bound_shell(
    host: &DocumentHost,
    wait: impl RenderWait,
    bound: impl FnOnce(&RowIdentities) -> Option<NodeSlotId>,
) -> *mut c_void {
    let facts = {
        // SAFETY: Guaranteed by the caller.
        let identities = unsafe { identities(host, wait) };
        bound(&identities).and_then(|id| identities.shell_facts(id))
    };
    let Some(facts) = facts else {
        return std::ptr::null_mut();
    };
    // SAFETY: As above. The factory reads the rows again, through the shared host.
    host.host_tables().shell_of(facts)
}

/// The layout node of the row the element or text node with `style_node` is bound to, made if nothing has asked for it
/// yet, or null if the node has no row.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_bound_shell(host: &DocumentHost, read: &BegunRead, style_node: u32) -> *mut c_void {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return std::ptr::null_mut();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { bound_shell(host, read, |identities| identities.bound_row(style_node)) }
}

/// The content size of the committed box of the row the element with `style_node` is bound to, or none where the
/// element has no row or its row no committed box. It makes no layout node, so a host callback a layout round makes
/// on the render owner may ask it.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_bound_committed_content_size(
    host: &DocumentHost,
    read: &BegunRead,
    style_node: u32,
    size: &mut crate::layout::FfiCssPixelSize,
) -> bool {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    let Some(row) = unsafe { identities(host, read) }.bound_row(style_node) else {
        return false;
    };
    host.read_rows(read, false, |rows| {
        if !rows.paintable_row_is_populated(row) {
            return false;
        }
        *size = crate::painting::paintable_geometry::committed_content_size(rows, row);
        true
    })
}

/// The layout node of the row the pseudo-element of kind `generated_for` on the element with `style_node` is bound to,
/// made if nothing has asked for it yet, or null.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_bound_pseudo_element_shell(
    host: &DocumentHost,
    read: &BegunRead,
    style_node: u32,
    generated_for: u8,
) -> *mut c_void {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return std::ptr::null_mut();
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        bound_shell(host, read, |identities| {
            identities.bound_pseudo_element_row(style_node, generated_for)
        })
    }
}

/// The layout node of the viewport row the document is bound to, made if nothing has asked for it yet, or null.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_bound_viewport_shell(host: &DocumentHost, read: &BegunRead) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    unsafe { bound_shell(host, read, RowIdentities::bound_viewport_row) }
}

/// The read of the layout node the host holds, which the host lends the entries it calls about the node's document.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, of a layout node the host holds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_read_of_held_node(host: &DocumentHost) -> *const BegunRead {
    host.begun_read_of_held_node(node_read())
}

/// Whether the row `id` is the one the element or text node with `style_node` is bound to, or, with no style node, the
/// viewport row the document is bound to.
///
/// # Safety
///
/// As for [`layout_row_flags`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_is_bound_to(host: &DocumentHost, id: NodeSlotId, style_node: u32) -> bool {
    // SAFETY: Guaranteed by the caller.
    let identities = unsafe { identities(host, node_read()) };
    let bound = match StyleNodeID::from_raw(style_node) {
        Some(style_node) => identities.bound_row(style_node),
        None => identities.bound_viewport_row(),
    };
    bound == Some(id)
}

/// The layout node of the box whose content box the row is laid out against, or null.
///
/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_containing_block_shell_if_live(host: &DocumentHost, id: NodeSlotId) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let containing_block = unsafe { read_rows(host, |rows| rows.node_containing_block_if_live(id)) };
    // SAFETY: As above.
    unsafe { shell_of(host, node_read(), containing_block) }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_establishes_an_absolute_positioning_containing_block(
    host: &DocumentHost,
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
    host: &DocumentHost,
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
    host: &DocumentHost,
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
    host: &DocumentHost,
    read: &BegunRead,
    slot: NodeSlotId,
) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let populated = unsafe { rows(host, read) }.paintable.paintable_row_is_populated(slot);
    // SAFETY: As above.
    unsafe { shell_of(host, read, populated.then_some(slot)) }
}

/// # Safety
///
/// As for [`layout_row_bound_shell`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_paintable_is_positioned(host: &DocumentHost, slot: NodeSlotId) -> bool {
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
pub unsafe extern "C" fn layout_row_paintable_has_child_paintables(host: &DocumentHost, slot: NodeSlotId) -> bool {
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
pub unsafe extern "C" fn layout_row_paintable_is_chrome_mirrored(host: &DocumentHost, slot: NodeSlotId) -> bool {
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
pub unsafe extern "C" fn layout_row_has_css_transform(host: &DocumentHost, id: NodeSlotId) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_rows(host, |rows| {
            rows.node_style_if_live(id)
                .is_some_and(|style| crate::painting::style_queries::has_css_transform(rows, id, style))
        })
    }
}

/// The characters the generated text row `id` renders, as a raw `AK::Utf16String` representation for which the caller
/// takes one reference.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `id` a live generated text row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_row_generated_text(host: &DocumentHost, id: NodeSlotId) -> usize {
    let text = host.ask(node_read(), |state| {
        state
            .arena_mut()
            .published_text_source(id, false)
            .data
            .to_utf16()
            .into_owned()
    });
    ak::Utf16String::from_utf16(&text).into_raw()
}
