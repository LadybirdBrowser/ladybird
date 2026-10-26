/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::style::tree::StyleNodeID;

/// What a commit message tells the document.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiCommitMessageKind {
    /// The node is a size query container whose content size changed.
    ContentSizeChangedForContainerQueries,
    /// The node is a navigable container whose viewport committed.
    NavigableContainerViewportCommitted,
    /// The node is the element a box escaped its rebuild root under, so its layout tree has to be
    /// built again.
    LayoutTreeRebuildRequested,
    /// A top layer member was reached with no box and nothing scheduled to rebuild it, so the
    /// document has to run another top layer zone pass. This one is about the document itself.
    TopLayerZoneRebuildNeeded,
    /// The node is the element a pseudo-element was generated for, and the pseudo-element's content
    /// or list marker shows the value of the `list-item` counter.
    ListItemCounterValueRendered,
    /// The node is an SVG resource, a `<mask>`, `<clipPath>` or `<pattern>`, whose content the
    /// tree build laid out under the graphics element `other_style_node` names. The resource
    /// outlives that box, so removing it has to rebuild the subtree the box sits in.
    SvgResourceReferenced,
    /// The node is an element a bypass path reached without a style, which no style update
    /// settled. The tree build built no box for it: the document styles it and builds its box
    /// again.
    UnstyledElementReached,
}

/// One thing layout has to tell the document. The node it is about is named by the style node the
/// style tree gave it, with 0 for the document; no pointer crosses the boundary.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCommitMessage {
    pub style_node: u32,
    /// A second node the message names, for the kinds that are about a pair. Zero otherwise.
    pub other_style_node: u32,
    pub kind: FfiCommitMessageKind,
}

impl FfiCommitMessage {
    pub(crate) fn new(style_node: u32, kind: FfiCommitMessageKind) -> Self {
        Self {
            style_node,
            other_style_node: 0,
            kind,
        }
    }
}

/// Host notifications contain no arena borrows. Dispatch them only after the
/// mutation phase returns, since C++ can reenter Rust to read or update paint state.
pub(crate) struct CommitNotifications {
    row_resets: Vec<crate::painting::paintable_rows::PaintableRowReset>,
    messages: Vec<FfiCommitMessage>,
}

impl CommitNotifications {
    /// The size query containers whose content size the commit changed.
    pub(crate) fn resized_size_containers(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.messages
            .iter()
            .filter(|message| message.kind == FfiCommitMessageKind::ContentSizeChangedForContainerQueries)
            .filter_map(|message| StyleNodeID::from_raw(message.style_node))
    }

    /// Tells the host of the resets and messages of the commit, which it answers synchronously, while no mutable arena
    /// borrow is active.
    pub(crate) fn notify_host(
        self,
        main_thread: &crate::stage::MainThread,
        read: &crate::render_state::BegunRead,
        host: &FfiLayoutHostCallbacks,
    ) {
        for reset in self.row_resets {
            reset.tell(main_thread);
        }
        host.deliver_commit_messages(main_thread, read, &self.messages);
    }
}

/// A box without children whose new fragment is the one it committed last has nothing a recommit would write, so it is
/// committed like the root of a reused subtree. Its committed geometry must still be current: a layout-affecting change
/// since that commit can keep the fragment while changing the facts a recommit gathers, such as its paint order inputs.
fn keeps_committed_childless_box(arena: &LayoutNodeArena, node: Node, link: &FragmentLink) -> bool {
    let data = arena.data(node);
    data.first_child.get().is_invalid()
        && node_facts::kind_is_box(data.kind.get())
        && arena.paintable_row_is_populated(node)
        && arena
            .with_current_committed_fragment(node, |committed| std::sync::Arc::ptr_eq(committed, &link.fragment))
            .unwrap_or(false)
}

fn commit_subtree(
    node: Node,
    messages: &mut Vec<FfiCommitMessage>,
    paintables: &mut crate::painting::paintable_build::PaintableCommit<'_>,
    links_by_slot: &fragment_tree::LinksBySlot<'_>,
    pass_fragments: &fragment_tree::CompletedPassFragments,
    enclosing_line_root_changes: crate::painting::paintable_build::LineRootChanges,
) {
    let slot_index = node.slot_index();
    let fragments = links_by_slot.fragments(slot_index);
    let fragments = fragments.as_slice();
    let link_with_merged_line_index;
    let entry = match fragments.first() {
        Some(fragment)
            if fragment.containing_block_line_index_offset != 0
                && fragment.link.containing_line_box_index.is_some() =>
        {
            link_with_merged_line_index = fragment_tree::FragmentLink {
                containing_line_box_index: fragment
                    .link
                    .containing_line_box_index
                    .map(|index| index + fragment.containing_block_line_index_offset),
                ..fragment.link.clone()
            };
            Some(&link_with_merged_line_index)
        }
        first => first.map(|fragment| fragment.link),
    };
    let has_more_fragments = fragments.len() > 1;
    let reuses_committed_subtree = !has_more_fragments
        && (pass_fragments.subtree_was_reused(slot_index)
            || entry.is_some_and(|link| keeps_committed_childless_box(paintables.arena(), node, link)));
    debug_assert!(!reuses_committed_subtree || entry.is_some());
    // A reused subtree whose root committed this very link already, in geometry that is still current, has nothing left
    // to commit. A box placing an absolutely positioned box, or a navigable's viewport, notes itself on every commit, and
    // a link without a containing block leaves the commit to find it among ancestors that may have changed.
    if reuses_committed_subtree
        && let Some(link) = entry
        && link.abspos_layout_inputs.is_none()
        && !link.containing_block.is_invalid()
        && paintables.arena().data(node).kind.get() != NodeKind::NavigableContainerViewport
        && paintables.arena().holds_current_committed_fragment_link(node, link)
        && fragments
            .first()
            .and_then(|fragment| fragment.containing_block_fragment)
            .is_none_or(|fragment| paintables.arena().committed_side_data(node).containing_block_fragment == fragment)
    {
        return;
    }
    if let Some(shift) = entry.and_then(|link| link.default_scroll_shift) {
        paintables.arena_mut().set_default_scroll_shift(
            node,
            shift.anchor,
            shift.compensates_for_horizontal_scroll,
            shift.compensates_for_vertical_scroll,
        );
    }
    if !reuses_committed_subtree {
        paintables.arena().forget_committed_out_of_flow_facts(node);
    }
    if let Some(inputs) = entry.and_then(|link| link.abspos_layout_inputs.as_ref()) {
        paintables.arena().note_committed_out_of_flow_box(node, inputs);
        paintables
            .arena_mut()
            .set_position_option_in_use(node, inputs.position_option);
    }
    let previous_fragmented_content = (paintables.arena().paintable_row_is_populated(node)
        && (has_more_fragments || !paintables.arena().committed_side_data(node).box_fragments().is_empty()))
    .then(|| paintables.arena().committed_side_data(node).inline_content.clone())
    .flatten();
    let prepared = paintables.prepare_node(
        node,
        entry.is_some(),
        reuses_committed_subtree,
        enclosing_line_root_changes,
    );

    let mut has_pending_inline_box_geometry = false;
    let mut line_root_changes_for_children = enclosing_line_root_changes;
    if let Some(link) = entry
        && prepared.has_paintable_row
    {
        let fragment = &link.fragment;
        debug_assert!(
            fragment.computed_svg_path.is_some()
                || !crate::painting::node_painting::is_svg_path(paintables.arena().data(node).kind.get()),
            "committed path-like fragment carries no computed SVG path"
        );
        let replaced = paintables.replace_committed_fragment_link(
            node,
            link,
            reuses_committed_subtree,
            enclosing_line_root_changes,
            prepared.previous_offset,
        );
        paintables.set_box_fragments(node, fragments);
        if fragment.line_data.is_some() {
            line_root_changes_for_children = replaced.line_root_changes;
        }
        if prepared.row_existed_before_this_commit
            && let Some((old_content_size, new_content_size)) = replaced.content_size_change
            && crate::layout::node_facts::node_style_view(paintables.arena().data(node)).is_some_and(|style| {
                content_size_change_affects_container_queries(style, old_content_size, new_content_size)
            })
            && let Some(style_node) = paintables.arena().commit_message_style_node(node)
        {
            messages.push(FfiCommitMessage::new(
                style_node,
                FfiCommitMessageKind::ContentSizeChangedForContainerQueries,
            ));
        }

        if !reuses_committed_subtree {
            let merged_line_data = has_more_fragments
                .then(|| {
                    inline_content::InlineContent::merge_box_fragments(
                        fragments
                            .iter()
                            .map(|fragment| fragment.link.fragment.line_data.as_deref()),
                    )
                })
                .flatten();
            if let Some(line_data) = merged_line_data.as_ref().or(fragment.line_data.as_ref()) {
                let change = paintables.set_line_data(node, line_data, previous_fragmented_content);
                has_pending_inline_box_geometry = change.has_pieces;
                line_root_changes_for_children.fragment_changed |= change.content_changed;
                line_root_changes_for_children.inline_content_changed |= change.content_changed;
                line_root_changes_for_children.inline_item_order_changed |= change.item_order_changed;
            }
        }
    }
    if entry.is_none() && prepared.has_paintable_row {
        paintables.set_box_fragments(node, fragments);
        paintables.schedule_scrollable_overflow_recalculation(node);
    }

    paintables.arena().gather_layout_style_snapshot_geometry(node);

    paintables.stamp_containing_block(node, entry);
    if reuses_committed_subtree {
        return;
    }
    paintables.arena().refresh_paint_order_inputs(node);

    let mut child = paintables.arena().data(node).first_child.get();
    while !child.is_invalid() {
        let next = paintables.arena().data(child).next_sibling.get();
        commit_subtree(
            child,
            messages,
            &mut *paintables,
            links_by_slot,
            pass_fragments,
            line_root_changes_for_children,
        );
        child = next;
    }

    if has_pending_inline_box_geometry {
        // Inline box geometry unites this block's piece rects with the box
        // models of its descendant inline paintables, which exist only now
        // that the whole subtree has committed.
        paintables.assign_inline_box_geometry(node);
    }
}

fn content_size_change_affects_container_queries(
    style: crate::css::computed_value_views::ComputedValuesView<'_>,
    old_size: FfiCssPixelSize,
    new_size: FfiCssPixelSize,
) -> bool {
    let box_values = style.box_values();
    if box_values.is_size_container {
        return old_size.width != new_size.width || old_size.height != new_size.height;
    }
    if !box_values.is_inline_size_container {
        return false;
    }
    if style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB {
        old_size.width != new_size.width
    } else {
        old_size.height != new_size.height
    }
}

pub(crate) fn commit_replacing(
    root: Node,
    arena: &mut LayoutNodeArena,
    pass_fragments: &fragment_tree::CompletedPassFragments,
) -> CommitNotifications {
    let links_by_slot = pass_fragments.links_by_slot();
    let mut paintables = crate::painting::paintable_build::PaintableCommit::new(arena, root);
    paintables.begin_commit();
    let mut messages = Vec::new();
    commit_subtree(
        root,
        &mut messages,
        &mut paintables,
        &links_by_slot,
        pass_fragments,
        Default::default(),
    );
    paintables.discard_absolute_rects_memoized_during_commit();
    for &viewport in paintables.committed_navigable_container_viewports() {
        if let Some(style_node) = paintables.arena().commit_message_style_node(viewport) {
            messages.push(FfiCommitMessage::new(
                style_node,
                FfiCommitMessageKind::NavigableContainerViewportCommitted,
            ));
        }
    }
    paintables.arena().publish_layout_style_snapshot_commit();
    CommitNotifications {
        row_resets: paintables.take_row_reset_notifications(),
        messages,
    }
}
