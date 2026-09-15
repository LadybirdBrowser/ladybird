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
use crate::painting::paintable_rows::PaintableRowsRef;
use crate::painting::record::PaintPhase;
use crate::painting::{node_painting, style_queries};
use smallvec::SmallVec;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub(crate) enum StackingContextPaintPhase {
    BackgroundAndBorders = 0,
    Floats = 1,
    BackgroundAndBordersForInlineLevelAndReplaced = 2,
    Foreground = 3,
}

impl StackingContextPaintPhase {
    pub(crate) const COUNT: usize = Self::Foreground as usize + 1;
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
}

/// A producer of the scope's owner in paint order. A box phase is up to three producers of the
/// recorder; an SVG root paints its background, border and content in the foreground pass.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum PaintProducer {
    BoxPhase(PaintPhase),
    SvgRoot,
    SvgBoxForeground,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PaintOrderItem {
    Producer(PaintProducer),
    Scope(PaintScope),
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
}

impl PaintOrderInputs {
    pub(crate) fn is_initialized(self) -> bool {
        self.flags != 0
    }

    fn has(self, flag: PaintOrderFlag) -> bool {
        self.flags & (1 << flag as u32) != 0
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

    pub(crate) fn gather(arena: &PaintableRowsRef<'_>, row: NodeSlotId) -> Self {
        let display = style_queries::display(arena, row);
        let kind = arena.node_kind_if_live(row);
        let z_index = style_queries::z_index(arena, row);
        let (collapsed_borders, hidden_columns) = arena.with_committed_fragment_link(row, |link| {
            link.map_or((false, false), |link| {
                (
                    link.fragment.collapsed_table_borders.is_some(),
                    link.fragment.hidden_by_collapsed_columns,
                )
            })
        });
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
            (FragmentedInline, node_painting::is_fragmented_inline(arena, row)),
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

impl PaintScopePlan {
    /// With prepared inputs, planning reads the snapshots kept on the rows instead of style
    /// and layout facts. Without them it gathers current inputs, so a canonical plan can
    /// detect a stale snapshot.
    pub(crate) fn build(
        arena: &PaintableRowsRef<'_>,
        scope: PaintScope,
        paint_overlay: bool,
        use_prepared_inputs: bool,
    ) -> Self {
        let mut builder = PaintOrderBuilder {
            layout_arena: arena,
            paint_overlay,
            use_prepared_inputs,
            items: SmallVec::new(),
            last_inputs: std::cell::Cell::new(None),
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

struct PaintOrderBuilder<'a, 'arena> {
    layout_arena: &'a PaintableRowsRef<'arena>,
    paint_overlay: bool,
    use_prepared_inputs: bool,
    items: SmallVec<[PaintOrderItem; 16]>,
    // Planning asks several questions about the same row in a row; keep its answers.
    last_inputs: std::cell::Cell<Option<(NodeSlotId, PaintOrderInputs)>>,
}

impl PaintOrderBuilder<'_, '_> {
    fn inputs(&self, row: NodeSlotId) -> PaintOrderInputs {
        if let Some((previous, inputs)) = self.last_inputs.get()
            && previous == row
        {
            return inputs;
        }
        let prepared = if self.use_prepared_inputs {
            self.layout_arena.row_paint_state(row).order_inputs()
        } else {
            None
        };
        let inputs = prepared.unwrap_or_else(|| PaintOrderInputs::gather(self.layout_arena, row));
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

    fn append_stacking_context(&mut self, owner: NodeSlotId) {
        self.items
            .push(PaintOrderItem::Scope(PaintScope::stacking_context(owner)));
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
        self.append_box_phase(PaintPhase::Foreground);
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
        self.append_box_phase(PaintPhase::Foreground);
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
        let mut next_child = crate::painting::paint_order::first_paint_child(self.layout_arena, paintable);
        while let Some(child) = next_child {
            next_child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, child);
            if !matches(self, child) || self.descendant_phase_is_empty(child, phase) {
                continue;
            }
            self.items.push(PaintOrderItem::Scope(PaintScope {
                owner: child,
                kind: PaintScopeKind::Descendants(phase),
            }));
        }
    }

    fn descendant_phase_is_empty(&self, child: NodeSlotId, phase: StackingContextPaintPhase) -> bool {
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
                self.append_box_phase(PaintPhase::Foreground);
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
        arena.paintable_rows_mut().paintable_data_mut(child).offset.x = CssPixels::from_integer(100);
        let after_move = PaintOrderInputs::gather(&arena.paintable_rows(), child);
        assert!(!arena.row_paint_state(child).update_order_inputs(after_move));
        let flags = &arena.data(child).flags;
        flags.set(flags.get() | NodeFlag::IsFlexItem as u32);
        let as_flex_item = PaintOrderInputs::gather(&arena.paintable_rows(), child);
        assert!(arena.row_paint_state(child).update_order_inputs(as_flex_item));
    }

    #[test]
    fn canonical_planning_can_detect_stale_prepared_inputs() {
        let mut arena = LayoutNodeArena::new();
        let row = arena.allocate_for_test().slot;
        arena.data(row).kind.set(NodeKind::Box);
        arena.populate_paintable_row(row);
        arena.refresh_paint_order_inputs(row);
        // Deliberately omit the refresh after a participation change. The canonical planner
        // must see the new state independently of that snapshot.
        arena.data(row).flags.set(NodeFlag::IsFlexItem as u32);
        let scope = PaintScope {
            owner: row,
            kind: PaintScopeKind::Descendants(StackingContextPaintPhase::Foreground),
        };
        let prepared = PaintScopePlan::build(&arena.paintable_rows(), scope, false, true);
        let canonical = PaintScopePlan::build(&arena.paintable_rows(), scope, false, false);
        assert_ne!(prepared.items, canonical.items);
        arena.refresh_paint_order_inputs(row);
        let updated = PaintScopePlan::build(&arena.paintable_rows(), scope, false, true);
        assert_eq!(updated.items, canonical.items);
    }
}
