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

#[cfg(test)]
mod tests {
    use super::render_state_node_shell_if_live;
    use crate::css::style::tree::StyleNodeID;
    use crate::layout::layout_changes::render_state_detach_remaining_rows_for_removal;
    use crate::layout::node_data::{GENERATED_FOR_BEFORE, NodeKind, NodeSlotId};
    use crate::render_state::{ScriptForcedRead, TestHost};

    #[test]
    fn queued_detach_for_removal_is_applied_before_resolving_a_layout_node() {
        let test_host = TestHost::new();
        let host = test_host.host();
        let (child_node, parent, before) = host.run(ScriptForcedRead::for_test(), true, |state| {
            let mut identities = [0; 2];
            state.engine_mut().allocate_style_nodes(&mut identities);
            let [parent_node, child_node] = identities.map(|raw| StyleNodeID::from_raw(raw).unwrap());
            let arena = state.arena_mut();
            let parent = arena.allocate_for_test().slot;
            let child = arena.allocate_for_test().slot;
            let before = arena.allocate_for_test().slot;
            arena.write_shape(parent).set_kind(NodeKind::BlockContainer);
            arena.write_shape(child).set_kind(NodeKind::BlockContainer);
            arena.write_shape(before).set_kind(NodeKind::InlineNode);
            arena.set_style_node_for_test(parent, Some(parent_node));
            arena.set_style_node_for_test(child, Some(child_node));
            arena.bind_row(parent);
            arena.bind_row(child);
            arena.stamp_pseudo_element_box(before, child_node, GENERATED_FOR_BEFORE);
            arena.insert_child(parent, child, NodeSlotId::INVALID);
            arena.insert_child(child, before, NodeSlotId::INVALID);
            (child_node, parent, before)
        });
        // The test's shell destruction stub never dereferences this stand-in for a C++ layout node.
        let shell = std::ptr::without_provenance_mut(8);
        host.host_tables().attach_shell(before, shell);
        let read = host.read_for_test();
        assert_eq!(render_state_node_shell_if_live(host, read, before), shell);

        // The removed element's pseudo-element boxes go with the queued detach, while the host still holds the layout
        // node of its ::before box.
        let style_nodes = [child_node.raw()];
        render_state_detach_remaining_rows_for_removal(host, ak::Span::new(&style_nodes));
        // A recording can still name the ::before box. Resolving it must apply the detach before answering.
        let resolved = render_state_node_shell_if_live(host, read, before);
        let shell_was_destroyed = !host.host_tables().shells.borrow().contains_key(&before);
        host.run(ScriptForcedRead::for_test(), true, |state| {
            state.arena_mut().free_subtree(parent)
        })
        .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test_with_host(host));
        assert!(resolved.is_null());
        assert!(shell_was_destroyed);
    }
}

/// The row's layout node, which the host's shell factory makes the first time it is asked for, or null for a row that
/// is not live.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_node_shell_if_live(
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
    let facts = {
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

/// The style record the render side installed in the box `node` ahead of the host with the style update the host
/// took last, which the box's layout node adopts without publishing it again, or 0 where it installed none.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_installed_style_record(host: &DocumentHost, node: NodeSlotId) -> u64 {
    host.render_state_installed_box_record(node).unwrap_or(0)
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

/// The anonymous boxes that give the text of the `display: contents` element `element` its style take it again, which
/// their layout nodes hear of once the host has its next job back.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_reinherit_display_contents_text(host: &DocumentHost, element: u32) {
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    host.queue_change(crate::render_state::ArenaChange::ReinheritDisplayContentsText(element));
}

/// Each `::first-letter` box whose element containing the letter took another style takes its style again, which its
/// layout node hears of once the host has its next job back.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_refresh_first_letter_styles(host: &DocumentHost) {
    if host.first_letter_styles_may_be_derived() {
        host.queue_change(crate::render_state::ArenaChange::RefreshFirstLetterStyles);
    }
}

/// The viewport and the boxes of the root element and its body take the styles viewport propagation gives them, which
/// their layout nodes hear of before the host goes on.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_propagate_root_styles_to_viewport(host: &DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { write_and_pay(host, node_read(), LayoutWrite::PropagateRootStylesToViewport) };
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

#[unsafe(no_mangle)]
pub extern "C" fn document_host_owned_image_provider(host: &DocumentHost, slot: NodeSlotId) -> *mut c_void {
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

#[unsafe(no_mangle)]
pub extern "C" fn document_host_image_observers(host: &DocumentHost, slot: NodeSlotId) -> *mut c_void {
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
