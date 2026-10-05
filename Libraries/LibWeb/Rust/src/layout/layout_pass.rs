/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The one question a layout pass still asks the document while it runs: what the container-relative
/// lengths on an element resolve against. Answering it notes in the document that the element depends
/// on its query container's size, so the answer cannot be published ahead of the pass. A pass holds
/// this and no other part of the host. Every other host call takes the main thread token, which a
/// pass is not handed, so a pass has no way to make any other call into the document.
#[derive(Clone, Copy)]
pub(crate) struct ContainerLengthBasesQuery {
    context: *mut c_void,
    /// The query, or none for a pass that runs beside the host and cannot ask it.
    query: Option<unsafe extern "C" fn(*mut c_void, u32) -> svg_formatting_context::FfiContainerLengthBases>,
}

// SAFETY: A layout job calls the query while the host waits for the job's answer, so the document the C++ side reads
// changes under neither of them.
unsafe impl Send for ContainerLengthBasesQuery {}

impl ContainerLengthBasesQuery {
    /// Only the layout host makes one, for a caller holding the main thread token.
    pub(super) fn new(
        context: *mut c_void,
        query: unsafe extern "C" fn(*mut c_void, u32) -> svg_formatting_context::FfiContainerLengthBases,
    ) -> Self {
        Self {
            context,
            query: Some(query),
        }
    }

    /// The query for a pass that runs beside the host, which answers nothing.
    pub(crate) fn sealed(self) -> Self {
        Self {
            context: std::ptr::null_mut(),
            query: None,
        }
    }

    /// What `cqw` and `cqh` are 100 of for `element`, where the pass can ask the host.
    pub(crate) fn bases(
        self,
        element: crate::css::style::tree::StyleNodeID,
    ) -> Option<svg_formatting_context::FfiContainerLengthBases> {
        // SAFETY: The document registered the query with the arena and outlives the pass, which the host waits for.
        self.query.map(|query| unsafe { query(self.context, element.raw()) })
    }
}

/// Arena inputs borrowed while computing a layout result. The pass ends before commit takes a
/// mutable arena borrow, so its text and style views cannot survive commit.
///
/// A layout run reads only the subtree it lays out. Whatever it needs from above arrives through
/// its layout input, its root's used values and its records, or was derived onto the subtree
/// before the pass, like ancestor facts. That is what lets partial relayout, the formatting context
/// run cache and the intrinsic size caches reuse a subtree's layout without looking outside it.
#[derive(Clone, Copy)]
pub(crate) struct LayoutPass<'arena> {
    arena: &'arena LayoutNodeArena,
    scratch: &'arena super::run_records::LayoutScratch,
    pub(crate) container_length_bases: ContainerLengthBasesQuery,
    pub(crate) initial_containing_block_inline_size: CssPixels,
    pub(crate) initial_containing_block_block_size: CssPixels,
    pub(crate) document_in_quirks_mode: bool,
}

impl<'arena> LayoutPass<'arena> {
    pub(crate) fn new(
        arena: &'arena LayoutNodeArena,
        scratch: &'arena super::run_records::LayoutScratch,
        container_length_bases: ContainerLengthBasesQuery,
        initial_containing_block_inline_size: CssPixels,
        initial_containing_block_block_size: CssPixels,
        document_in_quirks_mode: bool,
    ) -> Self {
        Self {
            arena,
            scratch,
            container_length_bases,
            initial_containing_block_inline_size,
            initial_containing_block_block_size,
            document_in_quirks_mode,
        }
    }

    pub(crate) fn arena(&self) -> &'arena LayoutNodeArena {
        self.arena
    }

    /// The layout stage's own scratch, which the pass's runs keep their records in.
    pub(crate) fn scratch(&self) -> &'arena super::run_records::LayoutScratch {
        self.scratch
    }

    /// The intrinsic sizes passes measured, kept for the passes after them.
    pub(crate) fn intrinsic_size_caches(&self) -> &'arena super::layout_node_arena::IntrinsicSizeCaches {
        &self.scratch.intrinsic_size_caches
    }

    /// What an intrinsic size cache entry measured for `node` now is valid for.
    pub(crate) fn intrinsic_size_cache_stamp(&self, node: Node) -> super::layout_node_arena::IntrinsicSizeCacheStamp {
        self.arena.intrinsic_size_cache_stamp(node)
    }

    pub(crate) fn node_data(&self, node: Node) -> &'arena NodeData {
        self.arena().data(node)
    }

    pub(crate) fn text_content(&self, node: Node) -> &'arena super::rendered_text::TextContent {
        self.arena
            .text_content(node)
            .expect("text node content must be synced to the arena before layout")
    }

    pub(crate) fn style_payloads(&self, node: Node) -> &'arena FfiStylePayloads {
        self.arena
            .style_payloads(node)
            .expect("styled node must publish its style container before layout")
    }

    pub(crate) fn replaced_content_facts(&self, node: Node) -> Option<FfiReplacedContentFacts> {
        self.arena.replaced_content_facts(node)
    }

    pub(crate) fn computed_values_view_if_styled(&self, node: Node) -> Option<ComputedValuesView<'arena>> {
        self.arena
            .style_payloads(node)
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

    pub(crate) fn in_flow_containing_block(&self, node: Node) -> Node {
        let (innermost_root, innermost_root_containing_block) = self.arena.innermost_run.get();
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
