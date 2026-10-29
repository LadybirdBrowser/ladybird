/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::node_facts::ReplacedContentFacts;
use super::*;

/// Arena inputs borrowed while computing a layout result. The pass ends before commit takes a
/// mutable arena borrow, so its text and style views cannot survive commit.
///
/// A layout run reads only the subtree it lays out. Whatever it needs from above arrives through
/// its layout input, its root's used values and its run state, or was derived onto the subtree
/// before the pass, like ancestor facts. That is what lets partial relayout, the formatting context
/// run cache and the intrinsic size caches reuse a subtree's layout without looking outside it.
///
/// A pass holds no part of the host. Every host call takes the main thread token, which a pass is
/// not handed, so a pass has no way to call into the document.
#[derive(Clone, Copy)]
pub(crate) struct LayoutPass<'arena> {
    arena: &'arena LayoutNodeArena,
    scratch: &'arena super::run_state::LayoutScratch,
    pub(crate) initial_containing_block_inline_size: CssPixels,
    pub(crate) initial_containing_block_block_size: CssPixels,
    pub(crate) document_in_quirks_mode: bool,
    /// The frame of the formatting context the pass is laying out, in which style reads see sizes and box sides.
    pub(crate) frame: super::geometry::Frame,
    /// The root of the run this pass view was handed to and that root's containing block, where containing block
    /// searches from inside the run stop. Invalid outside any run.
    innermost_run: (Node, Node),
    /// What the run this pass view was handed to found out for the host so far, or the pass outside any run.
    host_findings: &'arena RefCell<super::run_state::HostFindings>,
}

impl<'arena> LayoutPass<'arena> {
    pub(crate) fn new(
        arena: &'arena LayoutNodeArena,
        scratch: &'arena super::run_state::LayoutScratch,
        stage: super::formatting_context::LayoutStageFacts,
    ) -> Self {
        Self {
            arena,
            scratch,
            initial_containing_block_inline_size: stage.viewport_inline_size,
            initial_containing_block_block_size: stage.viewport_block_size,
            document_in_quirks_mode: stage.document_in_quirks_mode,
            frame: super::geometry::Frame::PHYSICAL,
            innermost_run: (NodeSlotId::INVALID, NodeSlotId::INVALID),
            host_findings: &scratch.host_findings_outside_runs,
        }
    }

    /// A pass over `arena` and `scratch` with no viewport, in no-quirks mode.
    #[cfg(test)]
    pub(crate) fn for_test(arena: &'arena LayoutNodeArena, scratch: &'arena super::run_state::LayoutScratch) -> Self {
        Self::new(
            arena,
            scratch,
            super::formatting_context::LayoutStageFacts {
                viewport_inline_size: CssPixels::default(),
                viewport_block_size: CssPixels::default(),
                document_in_quirks_mode: false,
            },
        )
    }

    /// This pass, as the run rooted at `root` sees it, which notes what it finds out for the host in `host_findings`.
    #[inline]
    pub(super) fn for_run<'run>(
        self,
        root: Node,
        root_containing_block: Node,
        host_findings: &'run RefCell<super::run_state::HostFindings>,
    ) -> LayoutPass<'run>
    where
        'arena: 'run,
    {
        let pass: LayoutPass<'run> = self;
        LayoutPass {
            innermost_run: (root, root_containing_block),
            host_findings,
            ..pass
        }
    }

    /// This pass, laying out in the frame of `writing_mode`.
    #[inline]
    pub(crate) fn in_frame_of(mut self, writing_mode: u8) -> Self {
        self.frame = super::geometry::Frame::of(writing_mode);
        self
    }

    #[inline]
    pub(crate) fn arena(&self) -> &'arena LayoutNodeArena {
        self.arena
    }

    /// The layout stage's own scratch, which the runs of the pass share.
    pub(crate) fn scratch(&self) -> &'arena super::run_state::LayoutScratch {
        self.scratch
    }

    /// Notes what the run this pass view was handed to found out for the host, or the pass outside any run.
    pub(crate) fn note_host_findings(&self, note: impl FnOnce(&mut super::run_state::HostFindings)) {
        note(&mut self.host_findings.borrow_mut());
    }

    /// Hands what a finished run found out to the run this pass view was handed to, which used its result.
    pub(crate) fn receive_host_findings(&self, findings: Option<Box<super::run_state::HostFindings>>) {
        if let Some(findings) = findings {
            self.note_host_findings(|into| into.receive(*findings));
        }
    }

    /// The intrinsic sizes passes measured, kept for the passes after them.
    pub(crate) fn intrinsic_size_caches(&self) -> &'arena super::layout_node_arena::IntrinsicSizeCaches {
        &self.scratch.intrinsic_size_caches
    }

    /// What an intrinsic size cache entry measured for `node` now is valid for.
    pub(crate) fn intrinsic_size_cache_stamp(&self, node: Node) -> super::layout_node_arena::IntrinsicSizeCacheStamp {
        self.arena.intrinsic_size_cache_stamp(node)
    }

    #[inline]
    pub(crate) fn node_data(&self, node: Node) -> &'arena NodeData {
        self.arena().data(node)
    }

    pub(crate) fn text_content(&self, node: Node) -> &'arena super::rendered_text::TextContent {
        self.arena
            .text_content(node)
            .expect("text node content must be synced to the arena before layout")
    }

    #[inline]
    pub(crate) fn style_payloads(&self, node: Node) -> &'arena FfiStylePayloads {
        self.style_payloads_if_styled(node)
            .expect("styled node must publish its style container before layout")
    }

    /// The style of `node`: that of its row, or the position option it is being laid out with.
    #[inline]
    pub(crate) fn style_payloads_if_styled(&self, node: Node) -> Option<&'arena FfiStylePayloads> {
        if let Some(style) = self.scratch.position_option_style(node) {
            // SAFETY: A position option's style record stays pinned while a box is laid out with it.
            return unsafe { super::node_data::style_payloads(style) };
        }
        self.arena.style_payloads(node)
    }

    pub(crate) fn replaced_content_facts(&self, node: Node) -> Option<ReplacedContentFacts> {
        self.arena.replaced_content_facts(node)
    }

    pub(crate) fn computed_values_view_if_styled(&self, node: Node) -> Option<ComputedValuesView<'arena>> {
        self.style_payloads_if_styled(node)
            .map(|payloads| ComputedValuesView::new(&payloads.groups))
    }

    pub(crate) fn can_skip_is_anonymous_text_run(&self, node: Node) -> bool {
        let data = self.node_data(node);
        if !node_facts::has_flag(data, NodeFlag::Anonymous) || data.generated_for.get() != 0 {
            return false;
        }

        let mut child = data.first_child.get();
        while !child.is_invalid() {
            let data = self.node_data(child);
            if !node_facts::kind_is_text(data.kind.get())
                || !self.text_content(child).untransformed_text_is_ascii_whitespace
            {
                return false;
            }
            child = data.next_sibling.get();
        }
        true
    }

    pub(crate) fn is_before(&self, node: Node, other: Node) -> bool {
        self.arena().is_before(self.node_data(node), self.node_data(other))
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, node: Node) -> Option<abspos_inputs::AbsposLayoutInputs> {
        let data = self.node_data(node);
        assert!(node_facts::kind_is_box(data.kind.get()));
        self.arena().saved_abspos_layout_inputs(data)
    }

    pub(crate) fn committed_fragment_link(&self, node: Node) -> Option<FragmentLink> {
        self.arena().committed_fragment_link(self.node_data(node))
    }

    #[inline]
    pub(crate) fn has_committed_fragment_link(&self, node: Node) -> bool {
        self.node_data(node).flags.get() & NodeFlag::HasCommittedFragmentLink as u32 != 0
    }

    #[inline]
    pub(crate) fn parent(&self, node: Node) -> Node {
        self.node_data(node).parent.get()
    }

    #[inline]
    pub(crate) fn first_child(&self, node: Node) -> Node {
        self.node_data(node).first_child.get()
    }

    #[inline]
    pub(crate) fn last_child(&self, node: Node) -> Node {
        self.node_data(node).last_child.get()
    }

    #[inline]
    pub(crate) fn next_sibling(&self, node: Node) -> Node {
        self.node_data(node).next_sibling.get()
    }

    #[inline]
    pub(crate) fn previous_sibling(&self, node: Node) -> Node {
        self.node_data(node).previous_sibling.get()
    }

    /// The children of `node`, in tree order.
    pub(crate) fn children(self, node: Node) -> impl Iterator<Item = Node> + 'arena {
        std::iter::successors(link(self.first_child(node)), move |&child| {
            link(self.next_sibling(child))
        })
    }

    pub(crate) fn siblings_from(self, first: Node, last: Node) -> impl Iterator<Item = Node> + 'arena {
        std::iter::successors(link(first), move |&child| {
            (child != last).then(|| link(self.next_sibling(child))).flatten()
        })
    }

    /// The children of `node`, last first.
    pub(crate) fn children_rev(self, node: Node) -> impl Iterator<Item = Node> + 'arena {
        std::iter::successors(link(self.last_child(node)), move |&child| {
            link(self.previous_sibling(child))
        })
    }

    pub(crate) fn in_flow_containing_block(&self, node: Node) -> Node {
        let (innermost_root, innermost_root_containing_block) = self.innermost_run;
        if node == innermost_root {
            return innermost_root_containing_block;
        }
        let mut ancestor = self.parent(node);
        while !ancestor.is_invalid() {
            let data = self.node_data(ancestor);
            if node_facts::node_forms_containing_block_for_children(data, node_facts::node_style_view(data)) {
                return ancestor;
            }
            if ancestor == innermost_root {
                return innermost_root_containing_block;
            }
            ancestor = data.parent.get();
        }
        NodeSlotId::INVALID
    }

    pub(crate) fn containing_block_for_child_run(
        &self,
        child: Node,
        participation: &ParticipationInParentFormattingContext,
    ) -> Node {
        match participation {
            ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs) => inputs.containing_block,
            _ => self.in_flow_containing_block(child),
        }
    }

    /// Whether `ancestor` is `node` or one of its ancestors. The walk stops at `root`, so `node` must be
    /// in `root`'s subtree and `ancestor` must not be above `root`.
    pub(crate) fn is_ancestor(&self, ancestor: Node, mut node: Node, root: Node) -> bool {
        while !node.is_invalid() {
            if node == ancestor {
                return true;
            }
            if node == root {
                return false;
            }
            node = self.node_data(node).parent.get();
        }
        false
    }
}

/// A node's link to another, which is none for an invalid slot.
fn link(node: Node) -> Option<Node> {
    (!node.is_invalid()).then_some(node)
}
