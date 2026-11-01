/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::style::tree::StyleNodeID;

/// What a commit message tells the document.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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
    /// The node is an element whose layout resolved a container-relative length, so that its
    /// style depends on size container queries.
    StyleDependsOnSizeContainerQuery,
    /// The node is a size query container a container-relative length resolved against.
    SizeQueryContainerResolvedAgainst,
    /// The node is a size query container a container-relative length resolved against before
    /// the container had a committed box, so the document evaluates container queries after
    /// layout.
    ContainerQueryEvaluationNeededAfterLayout,
}

/// One thing layout has to tell the document. The node it is about is named by the style node the
/// style tree gave it, with 0 for the document; no pointer crosses the boundary.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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
    /// Adds what the committed layout found out for the document ahead of what the commit itself found, which reads
    /// it: a commit message about a resized container asks whether lengths resolved against it.
    pub(crate) fn prepend_layout_messages(&mut self, messages: Vec<FfiCommitMessage>) {
        if !messages.is_empty() {
            self.messages.splice(0..0, messages);
        }
    }

    /// The size query containers whose content size the commit changed.
    pub(crate) fn resized_size_containers(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.messages
            .iter()
            .filter(|message| message.kind == FfiCommitMessageKind::ContentSizeChangedForContainerQueries)
            .filter_map(|message| StyleNodeID::from_raw(message.style_node))
    }

    /// Whether a length the committed layout resolved reads a size container that had no box, whose queries the host
    /// evaluates once a layout gives it one.
    pub(crate) fn awaits_container_query_evaluation(&self) -> bool {
        self.messages
            .iter()
            .any(|message| message.kind == FfiCommitMessageKind::ContainerQueryEvaluationNeededAfterLayout)
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

/// The fragment holding the links a commit meets: a box's fragment, or the top of the pass.
#[derive(Clone, Copy)]
struct Holder {
    /// Invalid at the top of the pass.
    node: Node,
    /// Which of its box's fragments the fragment is, or None at the top of the pass.
    fragment_index: Option<u32>,
    /// How many lines the box's earlier fragments hold, by which the fragment's line indices count on.
    line_index_offset: usize,
    line_root_changes: crate::painting::paintable_build::LineRootChanges,
}

impl Holder {
    fn top_of_pass() -> Self {
        Self {
            node: Node::INVALID,
            fragment_index: None,
            line_index_offset: 0,
            line_root_changes: Default::default(),
        }
    }

    /// Which fragment of its containing block shows `link`, and how its line indices count on there: a link the
    /// fragment of its containing block holds is shown by that fragment, any other by the first one.
    fn placement_of(&self, link: &FragmentLink) -> (Option<u32>, usize) {
        match self.fragment_index {
            None => (None, 0),
            Some(index) if link.containing_block == self.node => (Some(index), self.line_index_offset),
            Some(_) => (Some(0), 0),
        }
    }
}

/// The fragments of a box broken into several, which commit commits once it met them all.
struct BrokenBox<'tree> {
    fragments: fragment_tree::SlotFragments<'tree>,
    line_count: usize,
    enclosing_line_root_changes: crate::painting::paintable_build::LineRootChanges,
}

/// A box a commit committed anew, with the fragments it committed before, whose links the commit clears unless it met
/// them again.
struct RecommittedBox {
    node: Node,
    previous_fragments: smallvec::SmallVec<[std::sync::Arc<fragment_tree::Fragment>; 1]>,
    previous_inline_content: Option<std::sync::Arc<inline_content::InlineContent>>,
}

/// A commit of the fragments of a layout pass, which walks them from the top of the pass and commits each link that
/// changed. The fragment of a box whose committed link it meets again unchanged, in geometry still current, holds the
/// links its subtree committed, so the walk leaves that subtree as it is.
struct Commit<'arena, 'tree> {
    paintables: crate::painting::paintable_build::PaintableCommit<'arena>,
    messages: Vec<FfiCommitMessage>,
    /// The number of this commit, which tells the links it met from those it did not.
    number: u32,
    root: Node,
    is_full_commit: bool,
    committed_anew: Vec<Node>,
    recommitted: Vec<RecommittedBox>,
    broken_boxes: crate::fast_hash::FastMap<u32, BrokenBox<'tree>>,
    broken_box_order: Vec<Node>,
    out_of_flow_boxes: Vec<(Node, std::sync::Arc<crate::layout::abspos_inputs::AbsposLayoutInputs>)>,
    pass_level_links: Vec<Node>,
    line_roots: Vec<(Node, crate::painting::paintable_build::LineRootChanges)>,
}

impl<'tree> Commit<'_, 'tree> {
    fn arena(&self) -> &LayoutNodeArena {
        self.paintables.arena()
    }

    fn arena_mut(&mut self) -> &mut LayoutNodeArena {
        self.paintables.arena_mut()
    }

    fn was_met(&self, node: Node) -> bool {
        self.arena()
            .committed_link_holder(node)
            .is_some_and(|(_, number)| number == self.number)
    }

    fn meet(&mut self, link: &'tree FragmentLink, holder: &Holder) {
        let node = link.fragment.node;
        // NB: A partial relayout's root keeps the fragment of its parent as its holder, which the commit does not walk.
        let keeps_its_holder = holder.node.is_invalid() && !self.is_full_commit && node == self.root;
        let recorded_holder = if keeps_its_holder {
            self.arena()
                .committed_link_holder(node)
                .map_or(Node::INVALID, |(holder, _)| holder)
        } else {
            holder.node
        };
        let was_at_pass_level = self
            .arena()
            .committed_link_holder(node)
            .is_some_and(|(previous_holder, _)| previous_holder.is_invalid());
        let number = self.number;
        self.arena_mut()
            .note_committed_link_holder(node, recorded_holder, number);
        let containing_block = link.containing_block;
        if !containing_block.is_invalid() {
            let held_by_containing_block = recorded_holder == containing_block
                || self
                    .arena()
                    .committed_link_holder(recorded_holder)
                    .is_some_and(|(holder_of_holder, _)| holder_of_holder == containing_block);
            self.arena_mut()
                .note_whether_link_is_held_apart_from_its_containing_block(
                    node,
                    containing_block,
                    !held_by_containing_block,
                );
        }
        if holder.node.is_invalid() && !keeps_its_holder && (self.is_full_commit || !was_at_pass_level) {
            self.pass_level_links.push(node);
        }
        if let Some(inputs) = &link.abspos_layout_inputs {
            self.out_of_flow_boxes.push((node, inputs.clone()));
        }
        let (containing_block_fragment, line_index_offset) = holder.placement_of(link);
        if link.fragment.continues_from_previous_fragment || link.fragment.continues_in_next_fragment {
            self.meet_broken_box(link, containing_block_fragment, line_index_offset, holder);
            return;
        }
        if self.is_unchanged(node, link, containing_block_fragment, holder) {
            return;
        }
        let fragments: fragment_tree::SlotFragments<'tree> = smallvec::smallvec![fragment_tree::SlotFragment {
            link,
            containing_block_fragment,
            containing_block_line_index_offset: line_index_offset,
        }];
        let Some(children_holder) = self.commit_box(node, fragments, holder.line_root_changes) else {
            return;
        };
        for child in &link.fragment.children {
            self.meet(child, &children_holder);
        }
        self.finish_box(node, children_holder.line_root_changes);
    }

    /// Whether `link` is the committed link of its box already, placed the same way, in geometry still current, so
    /// that its subtree committed the links its fragment holds.
    fn is_unchanged(
        &self,
        node: Node,
        link: &FragmentLink,
        containing_block_fragment: Option<u32>,
        holder: &Holder,
    ) -> bool {
        let arena = self.arena();
        let data = arena.data(node);
        let kind = data.kind.get();
        // NB: A link without a containing block leaves the commit to find it among ancestors that may have changed,
        //     a navigable's viewport notes itself on every commit, and the paint of an inline box shows the lines of
        //     its line root.
        node_facts::kind_is_box(kind)
            && kind != NodeKind::NavigableContainerViewport
            && !link.containing_block.is_invalid()
            && !(holder.line_root_changes.any()
                && node_facts::node_is_fragmented_inline(data, node_facts::node_style_view(data)))
            && arena.paintable_row_is_populated(node)
            && arena.holds_current_committed_fragment_link(node, link)
            && arena.committed_side_data(node).box_fragments().is_empty()
            && containing_block_fragment
                .is_none_or(|fragment| arena.committed_side_data(node).containing_block_fragment == fragment)
    }

    fn meet_broken_box(
        &mut self,
        link: &'tree FragmentLink,
        containing_block_fragment: Option<u32>,
        line_index_offset: usize,
        holder: &Holder,
    ) {
        let node = link.fragment.node;
        let line_count = link
            .fragment
            .line_data
            .as_ref()
            .map_or(0, |line_data| line_data.lines.len());
        let fragment = fragment_tree::SlotFragment {
            link,
            containing_block_fragment,
            containing_block_line_index_offset: line_index_offset,
        };
        let (fragment_index, lines_before) = match self.broken_boxes.get_mut(&node.slot_index()) {
            Some(broken) => {
                let lines_before = broken.line_count;
                broken.line_count += line_count;
                broken.fragments.push(fragment);
                (broken.fragments.len() as u32 - 1, lines_before)
            }
            None => {
                self.broken_box_order.push(node);
                self.broken_boxes.insert(
                    node.slot_index(),
                    BrokenBox {
                        fragments: smallvec::smallvec![fragment],
                        line_count,
                        enclosing_line_root_changes: holder.line_root_changes,
                    },
                );
                (0, 0)
            }
        };
        // NB: The box commits once all its fragments are met. Its children see its lines as changed meanwhile.
        let children_holder = Holder {
            node,
            fragment_index: Some(fragment_index),
            line_index_offset: lines_before,
            line_root_changes: crate::painting::paintable_build::LineRootChanges::all(),
        };
        for child in &link.fragment.children {
            self.meet(child, &children_holder);
        }
    }

    fn commit_broken_boxes(&mut self) {
        for node in std::mem::take(&mut self.broken_box_order) {
            let broken = self
                .broken_boxes
                .remove(&node.slot_index())
                .expect("a met broken box is listed");
            if let Some(holder) = self.commit_box(node, broken.fragments, broken.enclosing_line_root_changes) {
                self.finish_box(node, holder.line_root_changes);
            }
        }
    }

    /// Whether the box of `link` keeps its committed subtree: its new fragment holds the lines and child fragments it
    /// committed last, placed the same way, in geometry still current, so that only the box itself commits. A stored run
    /// hit at another unobserved block size gives the box such a fragment around the committed contents.
    fn keeps_committed_subtree(&self, node: Node, link: &FragmentLink) -> bool {
        let arena = self.arena();
        node_facts::kind_is_box(arena.data(node).kind.get())
            && arena.paintable_row_is_populated(node)
            && arena.committed_side_data(node).box_fragments().is_empty()
            && arena
                .with_current_committed_fragment(node, |committed| {
                    std::sync::Arc::ptr_eq(committed, &link.fragment) || link.fragment.holds_same_contents_as(committed)
                })
                .unwrap_or(false)
    }

    /// Commits the box `node` anew with its `fragments`, answering how its fragment holds the links of its children, or
    /// None where the box keeps its committed subtree.
    fn commit_box(
        &mut self,
        node: Node,
        fragments: fragment_tree::SlotFragments<'tree>,
        enclosing_line_root_changes: crate::painting::paintable_build::LineRootChanges,
    ) -> Option<Holder> {
        let first = fragments.first().expect("a committed box has a fragment");
        let link_with_merged_line_index;
        let link = if first.containing_block_line_index_offset != 0 && first.link.containing_line_box_index.is_some() {
            link_with_merged_line_index = FragmentLink {
                containing_line_box_index: first
                    .link
                    .containing_line_box_index
                    .map(|index| index + first.containing_block_line_index_offset),
                ..first.link.clone()
            };
            &link_with_merged_line_index
        } else {
            first.link
        };
        let has_more_fragments = fragments.len() > 1;
        let keeps_committed_subtree = !has_more_fragments && self.keeps_committed_subtree(node, link);
        let previous = (!keeps_committed_subtree)
            .then(|| {
                let arena = self.arena();
                let previous_link = arena.with_committed_fragment_link(node, |link| link.cloned());
                previous_link.map(|previous_link| {
                    let row_is_populated = arena.paintable_row_is_populated(node);
                    let mut previous_fragments = smallvec::smallvec![previous_link.fragment];
                    let mut previous_inline_content = None;
                    if row_is_populated {
                        let side = arena.committed_side_data(node);
                        previous_fragments.extend(
                            side.box_fragments()
                                .iter()
                                .map(|fragment| fragment.link.fragment.clone()),
                        );
                        previous_inline_content = side.inline_content.clone();
                    }
                    RecommittedBox {
                        node,
                        previous_fragments,
                        previous_inline_content,
                    }
                })
            })
            .flatten();
        if let Some(shift) = link.default_scroll_shift {
            self.arena_mut().set_default_scroll_shift(
                node,
                shift.anchor,
                shift.compensates_for_horizontal_scroll,
                shift.compensates_for_vertical_scroll,
            );
        }
        if !keeps_committed_subtree {
            self.arena_mut().forget_committed_out_of_flow_facts(node);
        }
        let previous_fragmented_content = (has_more_fragments
            || previous
                .as_ref()
                .is_some_and(|previous| previous.previous_fragments.len() > 1))
        .then(|| {
            previous
                .as_ref()
                .and_then(|previous| previous.previous_inline_content.clone())
        })
        .flatten();
        let prepared = self
            .paintables
            .prepare_node(node, true, keeps_committed_subtree, enclosing_line_root_changes);
        self.committed_anew.push(node);
        if let Some(previous) = previous {
            self.recommitted.push(previous);
        }
        if !prepared.has_paintable_row {
            self.arena_mut().gather_layout_style_snapshot_geometry(node);
            return Some(Holder {
                node,
                fragment_index: Some(0),
                line_index_offset: 0,
                line_root_changes: enclosing_line_root_changes,
            });
        }
        let fragment = &link.fragment;
        debug_assert!(
            fragment.computed_svg_path.is_some()
                || !crate::painting::node_painting::is_svg_path(self.arena().data(node).kind.get()),
            "committed path-like fragment carries no computed SVG path"
        );
        let replaced = self.paintables.replace_committed_fragment_link(
            node,
            link,
            enclosing_line_root_changes,
            prepared.previous_offset,
        );
        self.paintables.set_box_fragments(node, &fragments);
        let mut line_root_changes_for_children = enclosing_line_root_changes;
        if fragment.line_data.is_some() {
            line_root_changes_for_children = replaced.line_root_changes;
        }
        if prepared.row_existed_before_this_commit
            && let Some((old_content_size, new_content_size)) = replaced.content_size_change
            && crate::layout::node_facts::node_style_view(self.arena().data(node)).is_some_and(|style| {
                content_size_change_affects_container_queries(style, old_content_size, new_content_size)
            })
            && let Some(style_node) = self.arena().commit_message_style_node(node)
        {
            self.messages.push(FfiCommitMessage::new(
                style_node,
                FfiCommitMessageKind::ContentSizeChangedForContainerQueries,
            ));
        }
        if keeps_committed_subtree {
            self.arena_mut().gather_layout_style_snapshot_geometry(node);
            self.arena_mut().refresh_paint_order_inputs(node);
            return None;
        }
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
            let change = self
                .paintables
                .set_line_data(node, line_data, previous_fragmented_content);
            line_root_changes_for_children.fragment_changed |= change.content_changed;
            line_root_changes_for_children.inline_content_changed |= change.content_changed;
            line_root_changes_for_children.inline_item_order_changed |= change.item_order_changed;
        } else {
            self.paintables.clear_line_data(node);
        }
        self.arena_mut().gather_layout_style_snapshot_geometry(node);
        // NB: A link can change what orders the painting of a box that keeps its fragment, such as whether collapsed
        //     columns hide it.
        self.arena_mut().refresh_paint_order_inputs(node);
        Some(Holder {
            node,
            fragment_index: Some(0),
            line_index_offset: 0,
            line_root_changes: line_root_changes_for_children,
        })
    }

    /// Notes a box committed anew whose fragment may hold lines, whose inline boxes commit after the walk.
    fn finish_box(&mut self, node: Node, line_root_changes: crate::painting::paintable_build::LineRootChanges) {
        if self.arena().paintable_row_is_populated(node)
            && self
                .arena()
                .committed_side_data(node)
                .inline_content
                .as_ref()
                .is_some_and(|content| content.inline_boxes.boxes().next().is_some())
        {
            self.line_roots.push((node, line_root_changes));
        }
    }

    /// Commits the rows of the inline boxes of the line roots this commit committed anew that have pieces but no link
    /// of their own, as an inline box laid out where no fragments are built has.
    fn commit_inline_boxes_without_links(&mut self) {
        for index in 0..self.line_roots.len() {
            let (root, line_root_changes) = self.line_roots[index];
            if !self.arena().slot_is_live(root) || !self.arena().paintable_row_is_populated(root) {
                continue;
            }
            let inline_boxes: Vec<Node> = self
                .arena()
                .committed_side_data(root)
                .inline_content
                .as_ref()
                .map(|content| content.inline_boxes.boxes().map(|(inline_box, _)| inline_box).collect())
                .unwrap_or_default();
            for inline_box in inline_boxes {
                if !self.arena().slot_is_live(inline_box) || self.was_met(inline_box) {
                    continue;
                }
                // NB: A link a fragment the walk left as it was holds stays, and one the walk did not meet again goes
                //     with the lost links, after which the box commits here.
                if self
                    .arena()
                    .with_committed_fragment_link(inline_box, |link| link.is_some())
                {
                    continue;
                }
                let number = self.number;
                self.arena_mut().note_committed_link_holder(inline_box, root, number);
                let prepared = self
                    .paintables
                    .prepare_node(inline_box, false, false, line_root_changes);
                self.committed_anew.push(inline_box);
                if prepared.has_paintable_row {
                    self.paintables.set_box_fragments(inline_box, &[]);
                    self.paintables.clear_line_data(inline_box);
                }
                self.arena_mut().gather_layout_style_snapshot_geometry(inline_box);
                self.arena_mut().refresh_paint_order_inputs(inline_box);
            }
        }
    }

    /// Clears what a box committed when this commit met it nowhere though the fragment that held it last committed
    /// anew, and so on for what the box's own fragments held.
    fn clear_lost_links(&mut self) {
        let mut lost: Vec<(Node, Node)> = Vec::new();
        for recommitted in std::mem::take(&mut self.recommitted) {
            for fragment in &recommitted.previous_fragments {
                lost.extend(
                    fragment
                        .children
                        .iter()
                        .map(|link| (link.fragment.node, recommitted.node)),
                );
            }
            if let Some(content) = &recommitted.previous_inline_content {
                lost.extend(
                    content
                        .inline_boxes
                        .boxes()
                        .map(|(inline_box, _)| (inline_box, recommitted.node)),
                );
            }
        }
        if self.is_full_commit {
            let previous_pass_level_links = self.arena_mut().take_pass_level_committed_links();
            lost.extend(previous_pass_level_links.into_iter().map(|node| (node, Node::INVALID)));
            let pass_level_links = std::mem::take(&mut self.pass_level_links);
            self.arena_mut().set_pass_level_committed_links(pass_level_links);
        } else {
            let mut pass_level_links = self.arena_mut().take_pass_level_committed_links();
            pass_level_links.append(&mut self.pass_level_links);
            self.arena_mut().set_pass_level_committed_links(pass_level_links);
        }
        while let Some((node, holder)) = lost.pop() {
            if !self.arena().slot_is_live(node)
                || self
                    .arena()
                    .committed_link_holder(node)
                    .is_none_or(|(last_holder, number)| last_holder != holder || number == self.number)
            {
                continue;
            }
            let number = self.number;
            self.arena_mut().note_committed_link_holder(node, Node::INVALID, number);
            let previous_link = self.arena().with_committed_fragment_link(node, |link| link.cloned());
            let row_is_populated = self.arena().paintable_row_is_populated(node);
            if let Some(previous_link) = &previous_link {
                lost.extend(
                    previous_link
                        .fragment
                        .children
                        .iter()
                        .map(|link| (link.fragment.node, node)),
                );
            }
            if row_is_populated {
                let side = self.arena().committed_side_data(node);
                for fragment in side.box_fragments() {
                    lost.extend(
                        fragment
                            .link
                            .fragment
                            .children
                            .iter()
                            .map(|link| (link.fragment.node, node)),
                    );
                }
                if let Some(content) = &side.inline_content {
                    lost.extend(content.inline_boxes.boxes().map(|(inline_box, _)| (inline_box, node)));
                }
            }
            if previous_link.is_none() && !row_is_populated {
                continue;
            }
            self.arena_mut().forget_committed_out_of_flow_facts(node);
            self.paintables.clear_node(node);
            self.arena_mut().gather_layout_style_snapshot_geometry(node);
        }
    }

    fn finish(mut self, pass_fragments: &'tree fragment_tree::CompletedPassFragments) -> CommitNotifications {
        for link in &pass_fragments.roots {
            self.meet(link, &Holder::top_of_pass());
        }
        self.commit_broken_boxes();
        self.commit_inline_boxes_without_links();
        self.clear_lost_links();
        // NB: An inline box whose link the commit cleared as lost commits again where its line root's pieces show it.
        self.commit_inline_boxes_without_links();
        for (root, _) in std::mem::take(&mut self.line_roots) {
            if self.arena().slot_is_live(root) && self.arena().paintable_row_is_populated(root) {
                self.paintables.note_inline_boxes_of_line_root_recommitted(root);
            }
        }
        for node in std::mem::take(&mut self.committed_anew) {
            if !self.arena().slot_is_live(node) {
                continue;
            }
            let link = self.arena().with_committed_fragment_link(node, |link| link.cloned());
            self.paintables.stamp_containing_block(node, link.as_ref());
        }
        for (node, inputs) in std::mem::take(&mut self.out_of_flow_boxes) {
            if !self.arena().slot_is_live(node)
                || !self.arena().with_committed_fragment_link(node, |link| link.is_some())
            {
                continue;
            }
            self.arena_mut().note_committed_out_of_flow_box(node, &inputs);
            self.arena_mut()
                .set_position_option_in_use(node, inputs.position_option);
        }
        let Self {
            mut paintables,
            mut messages,
            ..
        } = self;
        paintables.discard_absolute_rects_memoized_during_commit();
        for &viewport in paintables.committed_navigable_container_viewports() {
            if let Some(style_node) = paintables.arena().commit_message_style_node(viewport) {
                messages.push(FfiCommitMessage::new(
                    style_node,
                    FfiCommitMessageKind::NavigableContainerViewportCommitted,
                ));
            }
        }
        paintables.arena_mut().publish_layout_style_snapshot_commit();
        CommitNotifications {
            row_resets: paintables.take_row_reset_notifications(),
            messages,
        }
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
    arena.note_layout_commit();
    let number = arena.layout_commit_count() as u32;
    let is_full_commit = arena.data(root).kind.get() == NodeKind::Viewport;
    let mut paintables = crate::painting::paintable_build::PaintableCommit::new(arena);
    paintables.begin_commit();
    Commit {
        paintables,
        messages: Vec::new(),
        number,
        root,
        is_full_commit,
        committed_anew: Vec::new(),
        recommitted: Vec::new(),
        broken_boxes: Default::default(),
        broken_box_order: Vec::new(),
        out_of_flow_boxes: Vec::new(),
        pass_level_links: Vec::new(),
        line_roots: Vec::new(),
    }
    .finish(pass_fragments)
}
