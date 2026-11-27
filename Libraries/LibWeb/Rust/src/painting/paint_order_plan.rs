/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The ordered scopes and producer sites selected by CSS painting rules.
//!
//! Building a scope reads committed layout and stacking-context facts. It does not record
//! commands, inspect paint caches, or decide whether a producer's output is empty. Child scopes
//! are explicit references, so a consumer can record them, reuse them, or collect their order.

use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::node_facts::{self, NodeShape};
use crate::painting::paint_read::{PaintRead, PaintRow};
use crate::painting::record::PaintPhase;
use crate::painting::{node_painting, paint_order, style_queries};
use smallvec::SmallVec;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub(crate) enum StackingContextPaintPhase {
    BackgroundAndBorders = 0,
    Floats = 1,
    BackgroundAndBordersForInlineLevelAndReplaced = 2,
    Foreground = 3,
}

const _: () = assert!(
    StackingContextPaintPhase::Floats as usize == StackingContextPaintPhase::BackgroundAndBorders as usize + 1
        && StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced as usize
            == StackingContextPaintPhase::Floats as usize + 1
        && StackingContextPaintPhase::Foreground as usize
            == StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced as usize + 1
);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum PaintScopeKind {
    PaintedAsStackingContext,
    Descendants(StackingContextPaintPhase),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct PaintScope {
    pub owner: NodeSlotId,
    pub kind: PaintScopeKind,
}

impl PaintScope {
    pub(crate) fn stacking_context(owner: NodeSlotId) -> Self {
        Self {
            owner,
            kind: PaintScopeKind::PaintedAsStackingContext,
        }
    }

    /// Whether every box the scope paints has the owner on its containing block chain, so that what the owner
    /// gathers along that chain covers the whole scope. A walk below the owner only meets boxes laid out against it
    /// when the owner forms the containing block of its children: an inline box or a table row does not.
    pub(crate) fn paints_only_what_its_owner_contains(self, arena: &impl PaintRead) -> bool {
        let Some(row) = arena.node(self.owner) else {
            return false;
        };
        if row.kind() == NodeKind::Viewport {
            return true;
        }
        if !node_facts::node_forms_containing_block_for_children(&row, row.style()) {
            return false;
        }
        self.kind != PaintScopeKind::PaintedAsStackingContext
            || !arena.paintable_data(self.owner).establishes_stacking_context
            || self.owner_contains_the_boxes_hoisted_to_its_stacking_context(arena)
    }

    /// A stacking context also paints the positioned descendants hoisted to it, and those are laid out against its
    /// owner when each of the few it has is, or when the owner establishes both positioning containing blocks.
    pub(crate) fn owner_contains_the_boxes_hoisted_to_its_stacking_context(self, arena: &impl PaintRead) -> bool {
        contains_the_few_boxes_hoisted_to_its_stacking_context(arena, self.owner)
            || style_queries::establishes_positioning_containing_blocks(arena, self.owner) == (true, true)
    }
}

fn contains_the_few_boxes_hoisted_to_its_stacking_context(arena: &impl PaintRead, owner: NodeSlotId) -> bool {
    const MOST_HOISTED_BOXES_WALKED: usize = 8;
    let mut contexts_to_walk = SmallVec::<[NodeSlotId; MOST_HOISTED_BOXES_WALKED]>::new();
    contexts_to_walk.push(owner);
    let mut hoisted_boxes_walked = 0;
    while let Some(context) = contexts_to_walk.pop() {
        let Some(entries) = arena.stacking_context_entries(context) else {
            continue;
        };
        let hoisted_boxes = entries
            .child_contexts_with_nonzero_z_index
            .iter()
            .map(|entry| entry.slot)
            .chain(entries.stack_level_zero_boxes.iter().copied());
        for hoisted in hoisted_boxes {
            hoisted_boxes_walked += 1;
            if hoisted_boxes_walked > MOST_HOISTED_BOXES_WALKED
                || !arena.paintable_row_is_populated(hoisted)
                || !is_on_containing_block_chain_near(arena, owner, hoisted)
            {
                return false;
            }
            if arena.paintable_data(hoisted).establishes_stacking_context {
                contexts_to_walk.push(hoisted);
            }
        }
    }
    true
}

fn is_on_containing_block_chain_near(arena: &impl PaintRead, ancestor: NodeSlotId, descendant: NodeSlotId) -> bool {
    const LONGEST_CHAIN_WALKED: usize = 8;
    let mut containing_block = arena.paintable_data(descendant).containing_block;
    for _ in 0..LONGEST_CHAIN_WALKED {
        if containing_block == ancestor {
            return true;
        }
        if containing_block.is_invalid() || !arena.paintable_row_is_populated(containing_block) {
            return false;
        }
        containing_block = arena.paintable_data(containing_block).containing_block;
    }
    false
}

/// A producer of the scope's owner in paint order. A box phase is up to three producers of the
/// recorder; an SVG root paints its background, border and content in the foreground pass.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum PaintProducer {
    BoxPhase(PaintPhase),
    ForegroundHitTest,
    ForegroundCommands,
    InlinePiece(u32),
    TextFragment(u32),
    SvgRoot,
    SvgBoxForeground,
    // What a stacking context records before its content: the fill that triggers an SVG
    // filter and the mask contents.
    ScopePreamble,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum PaintOrderItem {
    Producer(PaintProducer),
    Scope(PaintScope),
}

/// A line root whose plain text is taller than this, in CSS pixels, has its text fragments planned one at a time, so
/// that a recording of a window can leave out the lines far from it.
const TALLEST_TEXT_PLANNED_IN_ONE_PIECE: i64 = 512;

/// Whether the foreground of a box with lines of `content` is planned with a producer for each of its text fragments:
/// where its lines interleave text with boxes, or where its lines of text reach further than may be painted in one
/// piece.
pub(crate) fn text_fragments_of_content_are_producers(content: &crate::layout::inline_content::InlineContent) -> bool {
    if !content.items.is_empty() {
        return true;
    }
    let (Some(first), Some(last)) = (content.lines.first(), content.lines.last()) else {
        return false;
    };
    let top = first.rect.y.min(last.rect.y);
    let bottom = (first.rect.y + first.rect.height).max(last.rect.y + last.rect.height);
    !content.fragments.is_empty()
        && bottom - top > crate::css::css_pixels::CssPixels::from_integer(TALLEST_TEXT_PLANNED_IN_ONE_PIECE)
}

/// Whether the foreground of a box with lines is planned the same for `content` as it was for `previous`, both as the
/// box's row holds them: the same producers, in the same order.
pub(crate) fn content_plans_same_foreground(
    previous: &crate::layout::inline_content::InlineContent,
    content: &crate::layout::inline_content::InlineContent,
) -> bool {
    let text_fragments_are_producers = text_fragments_of_content_are_producers(content);
    if text_fragments_are_producers != text_fragments_of_content_are_producers(previous) {
        return false;
    }
    // NB: Plain text has no items. Where its text fragments are producers, the plan lists every one of them.
    if text_fragments_are_producers && content.items.is_empty() && content.fragments.len() != previous.fragments.len() {
        return false;
    }
    previous.has_same_item_order(content)
}

pub(crate) fn text_fragments_are_producers(rows: &impl PaintRead, root: NodeSlotId) -> bool {
    rows.committed_side_data(root)
        .inline_content
        .as_ref()
        .is_some_and(|content| text_fragments_of_content_are_producers(content))
}

pub(crate) struct PaintScopePlan {
    pub establishes_stacking_context: bool,
    pub items: SmallVec<[PaintOrderItem; 16]>,
}

/// The per-row decisions read by the CSS order planner. Geometry and drawing data do not
/// belong here. Layout commit prepares the snapshot from the committed fragment and style;
/// visual-context assignment updates the facts it owns. A changed snapshot means the row is
/// placed differently by its ancestors' plans, or plans its own descendants differently.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct PaintOrderInputs {
    flags: u32,
    z_index: i32,
}

const _: () = assert!(std::mem::size_of::<PaintOrderInputs>() == 8);

#[derive(Clone, Copy)]
enum PaintOrderFlag {
    EstablishesContext,
    Positioned,
    Floating,
    Inline,
    FlexOrGridItem,
    FragmentedInline,
    Replaced,
    InlineLevelContext,
    TableInside,
    TableColumn,
    Box,
    SvgRoot,
    CollapsedBorders,
    HiddenColumns,
    HasZIndex,
    AtomicInlineContent,
    FormsContainingBlockForChildren,
}

impl PaintOrderInputs {
    pub(crate) fn is_initialized(self) -> bool {
        self.flags != 0
    }

    fn has(self, flag: PaintOrderFlag) -> bool {
        self.flags & (1 << flag as u32) != 0
    }

    pub(crate) fn is_atomic_inline_content(self) -> bool {
        self.has(PaintOrderFlag::AtomicInlineContent)
    }

    pub(crate) fn forms_containing_block_for_children(self) -> bool {
        self.has(PaintOrderFlag::FormsContainingBlockForChildren)
    }

    fn z_index(self) -> Option<i32> {
        self.has(PaintOrderFlag::HasZIndex).then_some(self.z_index)
    }

    // Display, node kind, fragmentation, flex/grid membership and table geometry are prepared
    // by layout commit. Visual-context assignment only changes these live facts. A z-index on
    // a flex or grid item also changes its positioned paint participation.
    pub(crate) fn with_visual_context(
        mut self,
        facts: &crate::painting::stacking_context::StackingContextFacts,
        z_index: Option<i32>,
    ) -> Self {
        for (flag, value) in [
            (PaintOrderFlag::EstablishesContext, facts.establishes_stacking_context),
            (PaintOrderFlag::Positioned, facts.is_positioned),
            (PaintOrderFlag::HasZIndex, z_index.is_some()),
        ] {
            self.flags = (self.flags & !(1 << flag as u32)) | (u32::from(value) << flag as u32);
        }
        self.z_index = z_index.unwrap_or(0);
        self
    }

    pub(crate) fn gather(arena: &impl PaintRead, row: NodeSlotId) -> Self {
        let display = style_queries::display(arena, row);
        let kind = arena.node_kind_if_live(row);
        let z_index = style_queries::z_index(arena, row);
        let (collapsed_borders, hidden_columns, committed_containing_block) =
            arena.with_committed_fragment_link(row, |link| {
                link.map_or((false, false, NodeSlotId::INVALID), |link| {
                    (
                        link.fragment.collapsed_table_borders().is_some(),
                        link.placement.hidden_by_collapsed_columns(),
                        link.placement.containing_block,
                    )
                })
            });
        let fragmented_inline = node_painting::is_fragmented_inline(arena, row);
        let containing_block = if committed_containing_block.is_invalid() {
            arena.paintable_data(row).containing_block
        } else {
            committed_containing_block
        };
        let atomic_inline_content = !fragmented_inline
            && display.is_inline_outside()
            && is_an_atomic_inline_fragment_of(arena, row, containing_block);
        use PaintOrderFlag::*;
        let decisions = [
            (
                EstablishesContext,
                arena.paintable_data(row).establishes_stacking_context,
            ),
            (Positioned, style_queries::is_positioned(arena, row)),
            (Floating, style_queries::is_floating(arena, row)),
            (Inline, display.is_inline_outside()),
            (FlexOrGridItem, style_queries::is_flex_or_grid_item(arena, row)),
            (FragmentedInline, fragmented_inline),
            (Replaced, style_queries::is_replaced_box(arena, row)),
            (
                InlineLevelContext,
                display.is_inline_outside() && (display.is_flow_root_inside() || display.is_table_inside()),
            ),
            (TableInside, display.is_table_inside()),
            (
                TableColumn,
                display.is_table_column_group() || display.is_table_column(),
            ),
            (Box, kind == Some(NodeKind::Box)),
            (SvgRoot, kind == Some(NodeKind::SVGSVGBox)),
            (CollapsedBorders, collapsed_borders),
            (HiddenColumns, hidden_columns),
            (HasZIndex, z_index.is_some()),
            (AtomicInlineContent, atomic_inline_content),
            (
                FormsContainingBlockForChildren,
                arena
                    .node(row)
                    .is_some_and(|node| node_facts::node_forms_containing_block_for_children(&node, node.style())),
            ),
        ];
        // The high bit distinguishes the initial empty snapshot from a prepared row.
        let flags = decisions.iter().fold(1 << 31, |flags, &(flag, value)| {
            flags | (u32::from(value) << flag as u32)
        });
        Self {
            flags,
            z_index: z_index.unwrap_or(0),
        }
    }
}

fn is_an_atomic_inline_fragment_of(arena: &impl PaintRead, row: NodeSlotId, line_root: NodeSlotId) -> bool {
    if line_root.is_invalid() || !arena.paintable_row_is_populated(line_root) {
        return false;
    }
    let side = arena.committed_side_data(line_root);
    side.fragment_indices_of(row)
        .iter()
        .any(|&index| side.fragments()[index as usize].is_atomic_inline)
}

impl PaintScopePlan {
    /// Planning reads the snapshots kept on the rows, and gathers style and layout facts only
    /// for a row that has none.
    pub(crate) fn build(arena: &impl PaintRead, scope: PaintScope, paint_overlay: bool) -> Self {
        Self::build_leaving_out(arena, scope, paint_overlay, &mut LeavesOutNothing)
    }

    pub(crate) fn build_leaving_out(
        arena: &impl PaintRead,
        scope: PaintScope,
        paint_overlay: bool,
        leaves_out: &mut impl PlanLeavesOut,
    ) -> Self {
        let mut builder = PaintOrderBuilder {
            layout_arena: arena,
            paint_overlay,
            items: SmallVec::new(),
            last_inputs: std::cell::Cell::new(None),
            leaves_out,
            last_line_left_out: None,
        };
        let establishes_stacking_context =
            scope.kind == PaintScopeKind::PaintedAsStackingContext && builder.has_stacking_context(scope.owner);
        match scope.kind {
            PaintScopeKind::PaintedAsStackingContext if establishes_stacking_context => {
                builder.append_context_contents(scope.owner);
            }
            PaintScopeKind::PaintedAsStackingContext => builder.append_as_stacking_context(scope.owner),
            PaintScopeKind::Descendants(phase) => builder.append_descendant(scope.owner, phase),
        }
        Self {
            establishes_stacking_context,
            items: builder.items,
        }
    }
}

pub(crate) trait PlanLeavesOut {
    const LEAVES_OUT_THE_SAME_CHILD_SCOPES_IN_EVERY_PHASE: bool = false;

    fn leaves_out_child_scope(&mut self, child: PaintScope, child_inputs: PaintOrderInputs) -> bool;
    fn leaves_out_line(&mut self, line_root: NodeSlotId, line_index: u32) -> bool;

    fn paint_children_kept_by_an_earlier_phase(&mut self, _owner: NodeSlotId) -> Option<&[NodeSlotId]> {
        None
    }

    fn keep_paint_children_for_later_phases(&mut self, _owner: NodeSlotId, _children_in_tree_order: &[NodeSlotId]) {}
}

struct LeavesOutNothing;

impl PlanLeavesOut for LeavesOutNothing {
    fn leaves_out_child_scope(&mut self, _: PaintScope, _: PaintOrderInputs) -> bool {
        false
    }

    fn leaves_out_line(&mut self, _: NodeSlotId, _: u32) -> bool {
        false
    }
}

const FEWEST_PAINT_CHILDREN_WORTH_KEEPING_FOR_LATER_PHASES: usize = 32;

struct PaintOrderBuilder<'a, R: PaintRead, LeavesOut: PlanLeavesOut> {
    layout_arena: &'a R,
    paint_overlay: bool,
    items: SmallVec<[PaintOrderItem; 16]>,
    // Planning asks several questions about the same row in a row; keep its answers.
    last_inputs: std::cell::Cell<Option<(NodeSlotId, PaintOrderInputs)>>,
    leaves_out: &'a mut LeavesOut,
    last_line_left_out: Option<(NodeSlotId, u32, bool)>,
}

impl<R: PaintRead, LeavesOut: PlanLeavesOut> PaintOrderBuilder<'_, R, LeavesOut> {
    fn inputs(&self, row: NodeSlotId) -> PaintOrderInputs {
        if let Some((previous, inputs)) = self.last_inputs.get()
            && previous == row
        {
            return inputs;
        }
        let inputs = self
            .layout_arena
            .prepared_paint_order_inputs(row)
            .unwrap_or_else(|| PaintOrderInputs::gather(self.layout_arena, row));
        self.last_inputs.set(Some((row, inputs)));
        inputs
    }

    fn has_stacking_context(&self, owner: NodeSlotId) -> bool {
        self.inputs(owner).has(PaintOrderFlag::EstablishesContext)
    }

    fn is_replaced_box(&self, owner: NodeSlotId) -> bool {
        self.inputs(owner).has(PaintOrderFlag::Replaced)
    }

    fn append_box_phase(&mut self, phase: PaintPhase) {
        self.items
            .push(PaintOrderItem::Producer(PaintProducer::BoxPhase(phase)));
    }

    fn append_foreground(&mut self, paintable: NodeSlotId) {
        let arena = self.layout_arena;
        let root = if node_painting::has_lines(arena, paintable) {
            paintable
        } else if crate::painting::fragment_ownership::is_self_painting_inline(arena, paintable) {
            arena.paintable_data(paintable).containing_block
        } else {
            self.append_box_phase(PaintPhase::Foreground);
            return;
        };
        if root.is_invalid() || !arena.paintable_row_is_populated(root) {
            self.append_box_phase(PaintPhase::Foreground);
            return;
        }
        if !text_fragments_are_producers(arena, root) {
            self.append_box_phase(PaintPhase::Foreground);
            return;
        }
        let side = arena.committed_side_data(root);
        let content = side
            .inline_content
            .as_ref()
            .expect("text fragments are producers of a root with lines");
        self.items
            .push(PaintOrderItem::Producer(PaintProducer::ForegroundHitTest));

        let filter = crate::painting::fragment_ownership::effective_filter(arena, paintable);
        if content.items.is_empty() {
            for index in 0..content.fragments.len() as u32 {
                if filter.owns_fragment(index)
                    && !self.leaves_out_line(root, content.fragments[index as usize].line_index)
                {
                    self.items
                        .push(PaintOrderItem::Producer(PaintProducer::TextFragment(index)));
                }
            }
        }
        // CSS 2.2 Appendix E, step 7.2: backgrounds, borders and text are
        // interleaved in each line. Layout retains that order alongside geometry.
        for item in &content.items {
            use crate::layout::inline_content::InlineItem;
            let line_index = match *item {
                InlineItem::BoxPiece(index) => content.inline_box_pieces[index as usize].line_index,
                InlineItem::TextFragment(index) | InlineItem::AtomicInline(index) => {
                    content.fragments[index as usize].line_index
                }
            };
            if self.leaves_out_line(root, line_index) {
                continue;
            }
            match *item {
                InlineItem::BoxPiece(index) => {
                    let piece = &content.inline_box_pieces[index as usize];
                    if !arena.paintable_row_is_populated(piece.node) {
                        continue;
                    }
                    let owner = if crate::painting::fragment_ownership::is_self_painting_inline(arena, piece.node) {
                        piece.node
                    } else {
                        crate::painting::fragment_ownership::nearest_self_painting_inline_box(arena, piece.node)
                            .unwrap_or(root)
                    };
                    if owner == paintable {
                        self.items
                            .push(PaintOrderItem::Producer(PaintProducer::InlinePiece(index)));
                    }
                }
                InlineItem::TextFragment(index) if filter.owns_fragment(index) => {
                    self.items
                        .push(PaintOrderItem::Producer(PaintProducer::TextFragment(index)));
                }
                InlineItem::AtomicInline(index) if filter.owns_fragment(index) => {
                    let node = content.fragments[index as usize].layout_node;
                    if arena.paintable_row_is_populated(node)
                        && !self.has_stacking_context(node)
                        && !self.inputs(node).has(PaintOrderFlag::Positioned)
                        && !self.inputs(node).has(PaintOrderFlag::Floating)
                    {
                        self.append_stacking_context(node);
                    }
                }
                _ => {}
            }
        }
        // The caret is painted over the line's content.
        self.items
            .push(PaintOrderItem::Producer(PaintProducer::ForegroundCommands));
    }

    fn leaves_out_line(&mut self, line_root: NodeSlotId, line_index: u32) -> bool {
        if let Some((root, index, left_out)) = self.last_line_left_out
            && root == line_root
            && index == line_index
        {
            return left_out;
        }
        let left_out = self.leaves_out.leaves_out_line(line_root, line_index);
        self.last_line_left_out = Some((line_root, line_index, left_out));
        left_out
    }

    fn is_atomic_inline_content(&self, paintable: NodeSlotId) -> bool {
        self.inputs(paintable).is_atomic_inline_content()
    }

    fn append_stacking_context(&mut self, owner: NodeSlotId) {
        self.append_child_scope(PaintScope::stacking_context(owner));
    }

    fn append_child_scope(&mut self, scope: PaintScope) {
        let owner_inputs = self.inputs(scope.owner);
        if self.leaves_out.leaves_out_child_scope(scope, owner_inputs) {
            return;
        }
        self.items.push(PaintOrderItem::Scope(scope));
    }

    fn append_svg_root(&mut self) {
        self.items.push(PaintOrderItem::Producer(PaintProducer::SvgRoot));
    }

    fn append_svg_box_foreground(&mut self) {
        self.items
            .push(PaintOrderItem::Producer(PaintProducer::SvgBoxForeground));
    }

    fn z_index(&self, paintable: NodeSlotId) -> Option<i32> {
        self.inputs(paintable).z_index()
    }

    fn is_fragmented_inline(&self, paintable: NodeSlotId) -> bool {
        self.inputs(paintable).has(PaintOrderFlag::FragmentedInline)
    }

    fn establishes_inline_level_painting_context(&self, paintable: NodeSlotId) -> bool {
        // CSS 2.2 painting order puts inline-block and inline-table boxes in the inline-level painting step and
        // says to paint each "as if it created a new stacking context", while keeping positioned descendants and
        // actual child stacking contexts in the parent stacking context:
        // https://drafts.csswg.org/css2/#painting-order
        // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
        self.inputs(paintable).has(PaintOrderFlag::InlineLevelContext)
    }

    fn is_pure_inline_box(&self, paintable: NodeSlotId) -> bool {
        self.is_fragmented_inline(paintable)
            && !self.inputs(paintable).has(PaintOrderFlag::Floating)
            && !self.inputs(paintable).has(PaintOrderFlag::Positioned)
    }

    fn append_context_contents(&mut self, paintable: NodeSlotId) {
        self.items.push(PaintOrderItem::Producer(PaintProducer::ScopePreamble));
        let entries = self.layout_arena.stacking_context_entries(paintable);
        if self.inputs(paintable).has(PaintOrderFlag::SvgRoot) {
            self.append_box_phase(PaintPhase::Background);
            self.append_box_phase(PaintPhase::Border);
            self.append_svg_box_foreground();
            // An `<svg>` that establishes a stacking context still has descendants that establish
            // one of their own - a `<foreignObject>` always does - and those are painted by their
            // own context rather than by the SVG walk.
            if let Some(entries) = &entries {
                for entry in entries.negative_z_index_child_contexts() {
                    self.append_stacking_context(entry.slot);
                }
                for &descendant in &entries.stack_level_zero_boxes {
                    if self.layout_arena.paintable_row_is_populated(descendant) && self.has_stacking_context(descendant)
                    {
                        self.append_stacking_context(descendant);
                    }
                }
                for entry in entries.positive_z_index_child_contexts() {
                    self.append_stacking_context(entry.slot);
                }
            }
            self.append_box_phase(PaintPhase::Outline);
            if self.paint_overlay {
                self.append_box_phase(PaintPhase::Overlay);
            }
            return;
        }

        // For a more elaborate description of the algorithm, see CSS 2.1 Appendix E
        // Draw the background and borders for the context root (steps 1, 2)
        self.append_box_phase(PaintPhase::Background);
        self.append_box_phase(PaintPhase::Border);

        // Stacking contexts formed by positioned descendants with negative z-indices (excluding 0) in z-index order
        // (most negative first) then tree order. (step 3)
        // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
        // account for new properties like `transform` and `opacity` that can create stacking contexts.
        // https://github.com/w3c/csswg-drafts/issues/2717
        if let Some(entries) = &entries {
            for entry in entries.negative_z_index_child_contexts() {
                self.append_stacking_context(entry.slot);
            }
        }

        // Draw the background and borders for block-level children (step 4)
        self.append_descendants(paintable, StackingContextPaintPhase::BackgroundAndBorders);
        if self.inputs(paintable).has(PaintOrderFlag::CollapsedBorders) {
            self.append_box_phase(PaintPhase::TableCollapsedBorder);
        }
        // Draw the non-positioned floats (step 5)
        if entries
            .as_ref()
            .is_some_and(|entries| entries.non_positioned_float_count > 0)
        {
            self.append_descendants(paintable, StackingContextPaintPhase::Floats);
        }
        // Draw inline content, replaced content, etc. (steps 6, 7)
        if entries
            .as_ref()
            .is_some_and(|entries| entries.inline_or_replaced_count > 0)
        {
            self.append_descendants(
                paintable,
                StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced,
            );
        }
        self.append_foreground(paintable);
        self.append_descendants(paintable, StackingContextPaintPhase::Foreground);

        // Draw positioned descendants with z-index `0` or `auto` in tree order. (step 8)
        // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
        // account for new properties like `transform` and `opacity` that can create stacking contexts.
        // https://github.com/w3c/csswg-drafts/issues/2717
        if let Some(entries) = &entries {
            for &descendant in &entries.stack_level_zero_boxes {
                debug_assert!(self.layout_arena.paintable_row_is_populated(descendant));
                if !self.layout_arena.paintable_row_is_populated(descendant) {
                    continue;
                }
                self.append_stacking_context(descendant);
            }
        }

        // Stacking contexts formed by positioned descendants with z-indices greater than or equal
        // to 1 in z-index order (smallest first) then tree order. (Step 9)
        // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
        // account for new properties like `transform` and `opacity` that can create stacking contexts.
        // https://github.com/w3c/csswg-drafts/issues/2717
        if let Some(entries) = &entries {
            for entry in entries.positive_z_index_child_contexts() {
                self.append_stacking_context(entry.slot);
            }
        }

        self.append_box_phase(PaintPhase::Outline);
        if self.paint_overlay {
            self.append_box_phase(PaintPhase::Overlay);
        }
    }

    fn append_subtree_backgrounds_and_borders(&mut self, paintable: NodeSlotId) {
        self.append_box_phase(PaintPhase::Background);
        self.append_box_phase(PaintPhase::Border);
        // A pure inline paintable paints its own background/border in the inline-level phase. Its block descendants, if
        // any, are painted by the earlier BackgroundAndBorders descent through pure inline boxes. In today's layout
        // trees, this subtree sweep is a no-op for InlineNodes: it can only find inline children, floats, or positioned
        // boxes, all of which are skipped by the BackgroundAndBorders phase.
        if !self.is_pure_inline_box(paintable) {
            self.append_descendants(paintable, StackingContextPaintPhase::BackgroundAndBorders);
        }
        if self.inputs(paintable).has(PaintOrderFlag::CollapsedBorders) {
            self.append_box_phase(PaintPhase::TableCollapsedBorder);
        }
    }

    fn append_inline_level_non_positioned_descendant(&mut self, paintable: NodeSlotId) {
        self.append_subtree_backgrounds_and_borders(paintable);
        // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
        // "For inline-block and inline-table elements: [...] treat the element as if it created a new stacking context,
        // but any positioned descendants and descendants which actually create a new stacking context should be
        // considered part of the parent stacking context, not this new one."
        if self.establishes_inline_level_painting_context(paintable) {
            self.append_descendants(paintable, StackingContextPaintPhase::Floats);
        }
    }

    fn append_as_stacking_context(&mut self, paintable: NodeSlotId) {
        if self.inputs(paintable).has(PaintOrderFlag::SvgRoot) {
            self.append_svg_root();
            return;
        }
        self.append_subtree_backgrounds_and_borders(paintable);
        self.append_descendants(paintable, StackingContextPaintPhase::Floats);
        self.append_descendants(
            paintable,
            StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced,
        );
        self.append_foreground(paintable);
        self.append_descendants(paintable, StackingContextPaintPhase::Foreground);
        self.append_box_phase(PaintPhase::Outline);
        self.append_box_phase(PaintPhase::Overlay);
    }

    fn append_descendants(&mut self, paintable: NodeSlotId, phase: StackingContextPaintPhase) {
        // CSS 2.2 §17.5.1 stacks the backgrounds of a table's parts in layers: the table, then the column groups,
        // the columns, the row groups, the rows and the cells. Column boxes may come after the row groups in the
        // tree (the HTML parser puts a <colgroup> that follows a row after it), so a table box paints its column
        // groups and columns before its other children. https://www.w3.org/TR/CSS22/tables.html#table-layers
        let is_column_box = |this: &Self, child: NodeSlotId| this.inputs(child).has(PaintOrderFlag::TableColumn);
        let paints_columns_first = phase == StackingContextPaintPhase::BackgroundAndBorders
            && self.inputs(paintable).has(PaintOrderFlag::Box)
            && self.inputs(paintable).has(PaintOrderFlag::TableInside);
        if paints_columns_first {
            self.append_descendants_matching(paintable, phase, |this, child| is_column_box(this, child));
            self.append_descendants_matching(paintable, phase, |this, child| !is_column_box(this, child));
        } else {
            self.append_descendants_matching(paintable, phase, |_, _| true);
        }
    }

    fn append_descendants_matching(
        &mut self,
        paintable: NodeSlotId,
        phase: StackingContextPaintPhase,
        matches: impl Fn(&Self, NodeSlotId) -> bool,
    ) {
        let begin = self.items.len();
        let scope_of = |child| PaintScope {
            owner: child,
            kind: PaintScopeKind::Descendants(phase),
        };
        let children_kept_by_an_earlier_phase = self
            .leaves_out
            .paint_children_kept_by_an_earlier_phase(paintable)
            .map(SmallVec::<[NodeSlotId; 8]>::from_slice);
        if let Some(children) = children_kept_by_an_earlier_phase {
            for child in children {
                if matches(self, child) && !self.descendant_phase_is_empty(child, phase) {
                    self.append_child_scope(scope_of(child));
                }
            }
        } else {
            let mut children_with_no_scope_left_out = SmallVec::<[NodeSlotId; 8]>::new();
            let mut child_count = 0;
            let mut next_child = crate::painting::paint_order::first_paint_child(self.layout_arena, paintable);
            while let Some(child) = next_child {
                next_child = crate::painting::paint_order::next_paint_sibling_of_paint_child(self.layout_arena, child);
                child_count += 1;
                let has_a_scope_in_this_phase = matches(self, child) && !self.descendant_phase_is_empty(child, phase);
                let item_count_before = self.items.len();
                if has_a_scope_in_this_phase {
                    self.append_child_scope(scope_of(child));
                }
                let scope_was_left_out = has_a_scope_in_this_phase && self.items.len() == item_count_before;
                if LeavesOut::LEAVES_OUT_THE_SAME_CHILD_SCOPES_IN_EVERY_PHASE && !scope_was_left_out {
                    children_with_no_scope_left_out.push(child);
                }
            }
            if LeavesOut::LEAVES_OUT_THE_SAME_CHILD_SCOPES_IN_EVERY_PHASE
                && child_count >= FEWEST_PAINT_CHILDREN_WORTH_KEEPING_FOR_LATER_PHASES
            {
                self.leaves_out
                    .keep_paint_children_for_later_phases(paintable, &children_with_no_scope_left_out);
            }
        }
        let display = style_queries::display(self.layout_arena, paintable);
        if display.is_flex_inside() || display.is_grid_inside() {
            // A stable sort preserves document order for items in the same ordinal group.
            self.items[begin..].sort_by_key(|item| match item {
                PaintOrderItem::Scope(scope) => paint_order::order_for_painting(self.layout_arena, scope.owner),
                PaintOrderItem::Producer(_) => unreachable!("only descendant scopes were appended"),
            });
        }
    }

    fn descendant_phase_is_empty(&self, child: NodeSlotId, phase: StackingContextPaintPhase) -> bool {
        if self.is_painted_at_its_place_on_a_line(child) {
            return true;
        }
        // Inline-blocks and inline-tables paint their backgrounds and internal floats
        // in the inline-level phase. Earlier passes through their parent do no work,
        // so there is no need to resolve, copy, or update an empty subtree capture.
        matches!(
            phase,
            StackingContextPaintPhase::BackgroundAndBorders | StackingContextPaintPhase::Floats
        ) && !self.inputs(child).has(PaintOrderFlag::SvgRoot)
            && self.establishes_inline_level_painting_context(child)
            && !self.inputs(child).has(PaintOrderFlag::Floating)
            && (phase == StackingContextPaintPhase::Floats || !self.is_pure_inline_box(child))
    }

    fn is_painted_at_its_place_on_a_line(&self, child: NodeSlotId) -> bool {
        !self.inputs(child).has(PaintOrderFlag::Floating)
            && !self.inputs(child).has(PaintOrderFlag::Positioned)
            && self.is_atomic_inline_content(child)
    }

    fn append_descendant(&mut self, child: NodeSlotId, phase: StackingContextPaintPhase) {
        if self.has_stacking_context(child) {
            return;
        }
        let positioned = self.inputs(child).has(PaintOrderFlag::Positioned);
        let floating = self.inputs(child).has(PaintOrderFlag::Floating);
        let inline = self.inputs(child).has(PaintOrderFlag::Inline);
        let is_item = self.inputs(child).has(PaintOrderFlag::FlexOrGridItem);

        // Positioned descendants at stack level 0 are painted in a separate pass.
        if positioned && self.z_index(child).unwrap_or(0) == 0 {
            return;
        }

        // These boxes are painted atomically at their fragment's position in the
        // containing line's foreground plan, including their internal floats.
        if self.is_painted_at_its_place_on_a_line(child) {
            return;
        }

        if self.inputs(child).has(PaintOrderFlag::SvgRoot) {
            if phase == StackingContextPaintPhase::Foreground {
                self.append_svg_root();
            }
            return;
        }

        // NOTE: Flex and grid items should be treated the same way as CSS2 defines for inline-blocks:
        //       - https://drafts.csswg.org/css-flexbox-1/#painting
        //       - https://www.w3.org/TR/css-grid-2/#z-order
        //       "For each one of these, treat the element as if it created a new stacking context, but any positioned
        //       descendants and descendants which actually create a new stacking context should be considered part of
        //       the parent stacking context, not this new one."
        if is_item && self.z_index(child).is_none() {
            // FIXME: This may not be fully correct with respect to the paint phases.
            if phase == StackingContextPaintPhase::Foreground {
                self.append_as_stacking_context(child);
            }
            return;
        }

        // All non-positioned floating descendants, in tree order.
        if floating && !positioned && self.z_index(child).is_none() {
            if phase == StackingContextPaintPhase::Floats {
                self.append_as_stacking_context(child);
            }
            return;
        }

        let child_is_inline_or_replaced = inline || self.is_replaced_box(child);
        let child_has_inline_level_painting_context = self.establishes_inline_level_painting_context(child);
        match phase {
            StackingContextPaintPhase::BackgroundAndBorders => {
                if !child_is_inline_or_replaced && !floating {
                    self.append_subtree_backgrounds_and_borders(child);
                } else if self.is_pure_inline_box(child) {
                    self.append_descendants(child, phase);
                }
            }
            StackingContextPaintPhase::Floats => {
                if floating {
                    self.append_subtree_backgrounds_and_borders(child);
                }
                // Atomic inline-level descendants participate in the parent's inline-level
                // painting step, so their internal floats are not painted early.
                if !child_has_inline_level_painting_context {
                    self.append_descendants(child, phase);
                }
            }
            StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced => {
                if child_is_inline_or_replaced {
                    self.append_inline_level_non_positioned_descendant(child);
                }
                self.append_descendants(child, phase);
            }
            StackingContextPaintPhase::Foreground => {
                self.append_foreground(child);
                self.append_descendants(child, phase);
                self.append_box_phase(PaintPhase::Outline);
                self.append_box_phase(PaintPhase::Overlay);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixels;
    use crate::layout::LayoutNodeArena;
    use crate::layout::node_data::NodeFlag;

    #[test]
    fn geometry_is_not_an_ordering_input_but_flex_item_participation_is() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        arena.insert_child(root, child, NodeSlotId::INVALID);
        for node in [root, child] {
            arena.populate_paintable_row(node);
            arena.refresh_paint_order_inputs(node);
        }
        let mut moved = crate::layout::fragment_tree::FragmentLink::for_test(child);
        moved.offset.x = CssPixels::from_integer(100);
        arena.set_committed_fragment_link(child, moved, None);
        let after_move = PaintOrderInputs::gather(&arena.paintable_rows(), child);
        assert!(!arena.update_paint_order_inputs(child, after_move));
        arena.set_node_flag(child, NodeFlag::IsFlexItem, true);
        let as_flex_item = PaintOrderInputs::gather(&arena.paintable_rows(), child);
        assert!(arena.update_paint_order_inputs(child, as_flex_item));
    }
}
