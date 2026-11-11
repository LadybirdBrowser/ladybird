/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::style::layout_style::LayoutStyle;
use crate::css::style::position_try::PositionOptionStyle;
use crate::css::style::tree::StyleNodeID;
use crate::css::style_value::{AnchorPayload, AnchorSizePayload};
use crate::painting::paint_read::{GeometryRead, PaintRead};
use box_alignment::{AxisDirections, OverflowSafety, SelfAlignment, SelfKeyword, SelfPosition};

pub(crate) fn aligned_static_offset(
    static_position_rect: abspos_inputs::StaticPositionRect,
    margin_box_inline_size: CssPixels,
    margin_box_block_size: CssPixels,
) -> geometry::LogicalOffset {
    let mut offset = static_position_rect.rect.offset;
    match static_position_rect.inline_alignment {
        AxisEdge::Start => {}
        AxisEdge::Center => {
            offset.inline_offset += (static_position_rect.rect.size.inline_size - margin_box_inline_size) / 2;
        }
        AxisEdge::End => {
            offset.inline_offset += static_position_rect.rect.size.inline_size - margin_box_inline_size;
        }
    }
    match static_position_rect.block_alignment {
        AxisEdge::Start => {}
        AxisEdge::Center => {
            offset.block_offset += (static_position_rect.rect.size.block_size - margin_box_block_size) / 2;
        }
        AxisEdge::End => {
            offset.block_offset += static_position_rect.rect.size.block_size - margin_box_block_size;
        }
    }
    offset
}

/// The static position rect relative to the containing block rect that insets are measured from, as the sizing
/// rules take it.
/// https://www.w3.org/TR/CSS22/visudet.html#abs-non-replaced-width
/// The static position for 'left' is the distance from the left edge of the containing block to the left margin edge
/// of a hypothetical box that would have been the first box of the element if its 'position' property had been
/// 'static' and 'float' had been 'none'.
/// NB: An axis that is not placed at its static position adds the containing block rect offset to its used inset, so
///     its static position rect already stands relative to the containing block rect.
fn static_position_rect_for_sizing(inputs: &abspos_inputs::AbsposLayoutInputs) -> abspos_inputs::StaticPositionRect {
    let info = &inputs.containing_block_info;
    let translation = |mode: abspos_inputs::AbsposAxisMode, origin: CssPixels| {
        if mode == abspos_inputs::AbsposAxisMode::StaticPosition {
            CssPixels::default() - origin
        } else {
            CssPixels::default()
        }
    };
    formatting_context::translate_static_position_rect(
        inputs.static_position_rect,
        FfiCssPixelPoint {
            x: translation(info.inline_placement.mode, info.rect.offset.inline_offset),
            y: translation(info.block_placement.mode, info.rect.offset.block_offset),
        },
    )
}

fn out_of_flow_root_space(inputs: &abspos_inputs::AbsposLayoutInputs) -> (AvailableSpace, ContainingBlockConstraints) {
    let containing_block_size = geometry::LogicalSize {
        inline_size: clamp_to_max_dimension_value(inputs.containing_block_info.rect.size.inline_size),
        block_size: clamp_to_max_dimension_value(inputs.containing_block_info.rect.size.block_size),
    };
    (
        AvailableSpace {
            inline_size: AvailableSize::definite(containing_block_size.inline_size),
            block_size: AvailableSize::definite(containing_block_size.block_size),
        },
        ContainingBlockConstraints {
            percentage_basis_inline_size: Some(containing_block_size.inline_size),
            percentage_basis_block_size: Some(containing_block_size.block_size),
            quirks_mode_percentage_basis_block_size: None,
        },
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ContainingBlockGeometry {
    pub(crate) content_origin_in_entry_space: FfiCssPixelPoint,
    pub(crate) padding_left: CssPixels,
    pub(crate) padding_right: CssPixels,
    pub(crate) padding_top: CssPixels,
    pub(crate) padding_bottom: CssPixels,
    pub(crate) content_inline_size: CssPixels,
    pub(crate) content_block_size: CssPixels,
}

impl ContainingBlockGeometry {
    pub(crate) fn from_box_metrics(
        metrics: &impl BoxMetricCells,
        content_origin_in_entry_space: FfiCssPixelPoint,
    ) -> Self {
        Self {
            content_origin_in_entry_space,
            padding_left: metrics.padding_left(),
            padding_right: metrics.padding_right(),
            padding_top: metrics.padding_top(),
            padding_bottom: metrics.padding_bottom(),
            content_inline_size: metrics.content_inline_size(),
            content_block_size: metrics.content_block_size(),
        }
    }

    pub(crate) fn padding_box_inline_size(&self) -> CssPixels {
        self.content_inline_size + self.padding_left + self.padding_right
    }

    pub(crate) fn padding_box_block_size(&self) -> CssPixels {
        self.content_block_size + self.padding_top + self.padding_bottom
    }
}

/// An absolutely positioned box this engine sizes: its cells, held in `frame`, the used insets the engine resolves for
/// it, and the anchor-size() resolutions its style reads.
#[derive(Clone, Copy)]
pub(crate) struct OutOfFlowBox<'a> {
    pub(crate) node: Node,
    pub(crate) cells: &'a Cell<used_values::RootInputCells>,
    pub(crate) frame: geometry::Frame,
    used_insets: &'a Cell<run_state::UsedInsets>,
    anchor_resolved_values: Option<&'a style_values::AnchorResolvedValues>,
}

impl<'a> OutOfFlowBox<'a> {
    pub(crate) fn new(
        sized: sizing_context::SizedCells<'a>,
        used_insets: &'a Cell<run_state::UsedInsets>,
        anchor_resolved_values: Option<&'a style_values::AnchorResolvedValues>,
    ) -> Self {
        Self {
            node: sized.node,
            cells: sized.cells,
            frame: sized.frame,
            used_insets,
            anchor_resolved_values,
        }
    }

    fn sized_cells(&self) -> sizing_context::SizedCells<'a> {
        sizing_context::SizedCells::new(self.node, self.cells, self.frame)
    }

    fn sized_box(&self) -> sizing_context::SizedBox<'a> {
        sizing_context::SizedBox::of_own_metrics(self.node, self.cells, self.frame)
    }
}

pub(crate) struct AbsposEngine<'pass> {
    state: &'pass RunState,
    pass: LayoutPass<'pass>,
    fragments: Option<&'pass fragment_tree::RunFragmentBuilder>,
}

impl<'pass> AbsposEngine<'pass> {
    pub(crate) fn for_run(run: &FormattingContextRun<'pass>) -> Self {
        Self {
            state: run.state,
            pass: run.pass,
            fragments: run.fragments,
        }
    }

    fn containing_block_geometry_for_pending_child(
        &self,
        entry: &abspos_inputs::PendingAbsposChild,
    ) -> ContainingBlockGeometry {
        let containing_block = entry.containing_block();
        let Some(containing_block_cells) = self
            .placed_box_fragments()
            .cells_of_placed_containing_block(containing_block, self.pass.frame)
        else {
            panic!(
                "the run placed no containing block for slot {} (cb slot {})",
                entry.child_box.slot_index(),
                containing_block.slot_index()
            );
        };
        let content_origin_in_entry_space =
            self.origin_of_containing_block_in_entry_space(containing_block, entry.coordinate_space_box);
        ContainingBlockGeometry::from_box_metrics(&containing_block_cells, content_origin_in_entry_space)
    }

    fn origin_of_containing_block_in_entry_space(&self, containing_block: Node, entry_space: Node) -> FfiCssPixelPoint {
        if entry_space == containing_block {
            return FfiCssPixelPoint::default();
        }
        let mut origins_by_chain_box = Vec::new();
        let mut chain_box = containing_block;
        let mut origin = FfiCssPixelPoint::default();
        loop {
            origins_by_chain_box.push((chain_box, origin));
            let Some(placement) = self.placement_of(chain_box) else {
                break;
            };
            origin = formatting_context::point_add(origin, placement.content_offset_in_containing_block);
            chain_box = self.placement_containing_block(chain_box);
            if chain_box.is_invalid() {
                break;
            }
        }
        let mut space_box = entry_space;
        let mut space_origin = FfiCssPixelPoint::default();
        loop {
            if let Some((_, origin)) = origins_by_chain_box
                .iter()
                .find(|(chain_box, _)| *chain_box == space_box)
            {
                return formatting_context::point_sub(*origin, space_origin);
            }
            let placement = self.placement_of(space_box).unwrap_or_else(|| {
                panic!(
                    "an entry's space chain crossed a box the run did not place before meeting its containing block's chain (slot {})",
                    space_box.slot_index()
                )
            });
            space_origin = formatting_context::point_add(space_origin, placement.content_offset_in_containing_block);
            space_box = self.placement_containing_block(space_box);
            assert!(
                !space_box.is_invalid(),
                "an entry's space chain ended before meeting its containing block's chain"
            );
        }
    }

    fn translation_between_payload_resting_spaces(&self, from_space: Node, to_space: Node) -> FfiCssPixelPoint {
        if from_space == to_space || from_space.is_invalid() || to_space.is_invalid() {
            return FfiCssPixelPoint::default();
        }
        let mut merge_point = from_space;
        while merge_point != to_space && !self.pass.is_ancestor(merge_point, to_space, self.state.root()) {
            merge_point = self.placement_containing_block(merge_point);
            assert!(!merge_point.is_invalid());
        }
        let offset_relative_to_merge_point = |descendant: Node| {
            let mut offset = FfiCssPixelPoint::default();
            let mut current = descendant;
            while current != merge_point {
                let content_offset = self
                    .placement_of(current)
                    .map_or(FfiCssPixelPoint::default(), |placement| {
                        placement.content_offset_in_containing_block
                    });
                offset = formatting_context::point_add(offset, content_offset);
                current = self.placement_containing_block(current);
                assert!(!current.is_invalid());
            }
            offset
        };
        formatting_context::point_sub(
            offset_relative_to_merge_point(from_space),
            offset_relative_to_merge_point(to_space),
        )
    }

    /// The fragments of the run this engine lays out boxes in, where its placements are kept.
    fn placed_box_fragments(&self) -> &fragment_tree::RunFragmentBuilder {
        self.fragments
            .expect("absolutely positioned boxes are laid out in runs that build fragments")
    }

    /// Where the run placed `node`, if it placed the box.
    fn placement_of(&self, node: Node) -> Option<run_state::BoxPlacement> {
        self.placed_box_fragments().placement_of(&self.pass, node)
    }

    fn placement_containing_block(&self, node: Node) -> Node {
        self.placed_box_fragments().placement_containing_block(&self.pass, node)
    }

    #[inline]
    fn sizing(&self) -> sizing_context::SizingContext<'pass, 'static> {
        sizing_context::SizingContext::new(self.state, self.pass)
    }

    #[inline]
    fn style(&self, node: Node) -> StyleValues<'pass> {
        StyleValues::for_node(&self.pass, node)
    }

    /// The style of `sized`, an absolutely positioned box this engine sizes, with its anchor-size() resolutions.
    fn style_of<'sized>(&self, sized: OutOfFlowBox<'sized>) -> StyleValues<'sized>
    where
        'pass: 'sized,
    {
        StyleValues::for_node(&self.pass, sized.node).with_anchor_resolved_values(sized.anchor_resolved_values)
    }

    #[inline]
    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.pass, node)
    }

    fn base_containing_block_info(
        &self,
        node: Node,
        inline_containing_block_rect: Option<CssPixelRect>,
        entry_containing_block_geometry: &ContainingBlockGeometry,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) -> abspos_inputs::AbsposContainingBlockInfo {
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let rect = match inline_containing_block_rect {
            Some(rect) => geometry::LogicalRect {
                offset: geometry::LogicalOffset {
                    inline_offset: rect.x,
                    block_offset: rect.y,
                },
                size: geometry::LogicalSize {
                    inline_size: rect.width,
                    block_size: rect.height,
                },
            },
            None => geometry::LogicalRect {
                offset: geometry::LogicalOffset {
                    inline_offset: -entry_containing_block_geometry.padding_left,
                    block_offset: -entry_containing_block_geometry.padding_top,
                },
                size: geometry::LogicalSize {
                    inline_size: entry_containing_block_geometry.padding_box_inline_size(),
                    block_size: entry_containing_block_geometry.padding_box_block_size(),
                },
            },
        };
        abspos_inputs::AbsposContainingBlockInfo::of_own_insets(rect, style)
    }
}

struct AnchorResolutionState {
    default_anchor_box: Node,
    compensates_for_horizontal_scroll: bool,
    compensates_for_vertical_scroll: bool,
    /// Whether an anchor function resolved against an anchor box, rather than to its fallback value.
    resolved_against_anchor_box: bool,
}

impl AnchorResolutionState {
    const NO_SCROLL_SHIFT: Self = Self {
        default_anchor_box: NodeSlotId::INVALID,
        compensates_for_horizontal_scroll: false,
        compensates_for_vertical_scroll: false,
        resolved_against_anchor_box: false,
    };
}

/// The interpreted <anchor-side> argument of an anchor() function; the
/// percentage carries its value as a fraction.
#[derive(Clone, Copy, PartialEq)]
enum AnchorSide {
    Invalid,
    Top,
    Right,
    Bottom,
    Left,
    Center,
    Start,
    End,
    SelfStart,
    SelfEnd,
    Inside,
    Outside,
    Percentage(f64),
}

fn anchor_side_from_style_value(side: &crate::css::style_value::StyleValueData) -> AnchorSide {
    use crate::css::style_value::StyleValueData;
    match side {
        StyleValueData::Keyword { keyword } => match *keyword {
            keyword::TOP => AnchorSide::Top,
            keyword::RIGHT => AnchorSide::Right,
            keyword::BOTTOM => AnchorSide::Bottom,
            keyword::LEFT => AnchorSide::Left,
            keyword::CENTER => AnchorSide::Center,
            keyword::START => AnchorSide::Start,
            keyword::END => AnchorSide::End,
            keyword::SELF_START => AnchorSide::SelfStart,
            keyword::SELF_END => AnchorSide::SelfEnd,
            keyword::INSIDE => AnchorSide::Inside,
            keyword::OUTSIDE => AnchorSide::Outside,
            _ => AnchorSide::Invalid,
        },
        StyleValueData::Percentage { value } => AnchorSide::Percentage(value / 100.0),
        _ => AnchorSide::Invalid,
    }
}

#[derive(Clone, Copy)]
struct AnchorValueAxis {
    is_from_end: bool,
    is_horizontal: bool,
    containing_block_extent: CssPixels,
}

#[derive(Clone, Copy)]
struct AnchorCalcCallbackContext<'pass> {
    engine: *const AbsposEngine<'pass>,
    positioned_box: Node,
    containing_block: Node,
    containing_block_geometry: ContainingBlockGeometry,
    entry_coordinate_space_box: Node,
    is_from_end: bool,
    is_horizontal_axis: bool,
    containing_block_extent: CssPixels,
    resolution_state: *mut AnchorResolutionState,
}

impl AbsposEngine<'_> {
    /// The box `anchor_name` names for `positioned_box`.
    ///
    /// https://drafts.csswg.org/css-shadow-1/#tree-scoped-name
    /// An anchor name is tree-scoped: the search starts in the tree scope the querying element is
    /// in, and when that is a shadow tree and nothing acceptable is found there, it continues in the
    /// tree scope of the shadow host, up to the document tree.
    fn anchor_lookup(&self, positioned_box: Node, anchor_name: usize) -> Option<Node> {
        let arena = self.pass.arena();
        // A pseudo-element queries from its generator. Any other anonymous box stands for no element
        // and has no tree scope to search.
        let is_pseudo_element = arena.node_is_generated_for_pseudo_element(positioned_box);
        let querying_element = if is_pseudo_element {
            arena.node_style_node(positioned_box)
        } else {
            arena.dom_node_style_node(positioned_box)
        }?;
        // NB: A pseudo-element is a descendant of its generator, so the generator's anchor-scope applies to it.
        let querier_scope_start = if is_pseudo_element {
            Some(querying_element)
        } else {
            arena.style_node_parent(querying_element)
        };
        let containing_block = arena.node_containing_block_if_live(positioned_box)?;
        let mut scope_host = arena.tree_scope_host(querying_element);
        loop {
            // https://drafts.csswg.org/css-anchor-position-1/#anchor-scope
            // NB: An anchor-scope element both keeps the matching anchor names of its subtree from everything outside
            //     it, and limits lookups from its descendants to anchors inside it. So an anchor is visible exactly
            //     when the nearest element that scopes its name around it also scopes the name around the querier.
            let querier_scope =
                querier_scope_start.and_then(|start| self.nearest_anchor_scope(start, anchor_name, scope_host));
            let found = arena.last_element_with_anchor_name(scope_host, anchor_name, |&element| {
                self.anchor_is_acceptable(arena.bound_row(element), positioned_box, containing_block)
                    && self.nearest_anchor_scope(element, anchor_name, scope_host) == querier_scope
            });
            if let Some(element) = found {
                return Some(arena.bound_row(element));
            }
            scope_host = arena.tree_scope_host(scope_host?);
        }
    }

    /// The nearest inclusive ancestor of `element` whose anchor-scope covers `anchor_name`, among the elements of the
    /// tree scope `scope_host` names.
    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-scope
    fn nearest_anchor_scope(
        &self,
        element: StyleNodeID,
        anchor_name: usize,
        scope_host: Option<StyleNodeID>,
    ) -> Option<StyleNodeID> {
        let arena = self.pass.arena();
        let mut candidate = Some(element);
        while let Some(current) = candidate {
            if let Some(style) = arena.style_node_style(current) {
                // SAFETY: The record the element published is retained for the pass.
                let anchor = unsafe { style.view() }.anchor();
                // The <anchor-name> represents a strictly matched tree-scoped name, i.e. it can only match against
                // anchor names in the same shadow tree.
                if (anchor.anchor_scope_all || anchor.anchor_scope_names.raws().contains(&anchor_name))
                    && arena.tree_scope_host(current) == scope_host
                {
                    return Some(current);
                }
            }
            candidate = arena.style_node_parent(current);
        }
        None
    }

    /// Whether `anchor_box` may act as the anchor of `positioned_box`: it must have used values
    /// from this pass, and its containing block chain must reach the positioned box's own.
    fn anchor_is_acceptable(&self, anchor_box: Node, positioned_box: Node, containing_block: Node) -> bool {
        let arena = self.pass.arena();
        if anchor_box == positioned_box
            || !arena
                .node_kind_if_live(anchor_box)
                .is_some_and(|kind| super::node_facts::kind_is_box(kind) || kind == NodeKind::InlineNode)
            || !self
                .fragments
                .is_some_and(|fragments| fragments.find_anchor_candidate(anchor_box).is_some())
        {
            return false;
        }
        let mut ancestor = arena.node_containing_block_if_live(anchor_box);
        while let Some(block) = ancestor {
            if block == containing_block {
                return true;
            }
            ancestor = arena.node_containing_block_if_live(block);
        }
        false
    }

    fn nearest_scroll_container_ancestor(&self, node: Node) -> Node {
        let mut ancestor = self.placement_containing_block(node);
        while !ancestor.is_invalid() {
            if self.facts(ancestor).is_scroll_container() {
                return ancestor;
            }
            if ancestor == self.state.root() {
                break;
            }
            ancestor = self.placement_containing_block(ancestor);
        }
        NodeSlotId::INVALID
    }

    fn anchor_rect(
        &self,
        anchor_box: Node,
        entry_containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
    ) -> CssPixelRect {
        let (rect, coordinate_space_box) = self
            .fragments
            .and_then(|fragments| fragments.find_anchor_candidate(anchor_box))
            .expect("an accepted anchor is placed in the draining subtree and propagated here");
        let fold_into_entry_space =
            self.translation_between_payload_resting_spaces(coordinate_space_box, entry_coordinate_space_box);
        CssPixelRect {
            x: rect.x + fold_into_entry_space.x - entry_containing_block_geometry.content_origin_in_entry_space.x
                + entry_containing_block_geometry.padding_left,
            y: rect.y + fold_into_entry_space.y - entry_containing_block_geometry.content_origin_in_entry_space.y
                + entry_containing_block_geometry.padding_top,
            width: rect.width,
            height: rect.height,
        }
    }

    fn anchor_side(
        &self,
        side: AnchorSide,
        rect: CssPixelRect,
        positioned_box: Node,
        containing_block: Node,
        is_from_end: bool,
        is_horizontal_axis: bool,
    ) -> Option<CssPixels> {
        let frame = self.pass.frame;
        // NB: The physical sides an anchor() names lie on the sides of the frame the box is laid out in.
        let side = match side {
            AnchorSide::Top | AnchorSide::Right | AnchorSide::Bottom | AnchorSide::Left if !frame.is_physical() => {
                let physical_side = match side {
                    AnchorSide::Top => geometry::Side::Top,
                    AnchorSide::Right => geometry::Side::Right,
                    AnchorSide::Bottom => geometry::Side::Bottom,
                    _ => geometry::Side::Left,
                };
                match frame.frame_side(physical_side) {
                    geometry::Side::Top => AnchorSide::Top,
                    geometry::Side::Right => AnchorSide::Right,
                    geometry::Side::Bottom => AnchorSide::Bottom,
                    geometry::Side::Left => AnchorSide::Left,
                }
            }
            side => side,
        };
        let (axis_start, axis_end) = if is_horizontal_axis {
            (rect.left(), rect.right())
        } else {
            (rect.top(), rect.bottom())
        };
        // https://drafts.csswg.org/css-anchor-position-1/#valdef-anchor-start
        // Refers to one of the sides of the anchor box in the same axis as the inset property it's used in, by
        // resolving the keyword against the writing mode of either the positioned box (for self-start and self-end)
        // or the positioned box's containing block (for start and end).
        let start_and_end_sides = |writing_mode_box: Node| {
            let style = self.style(writing_mode_box);
            // NB: The start side may run either way along the axis of the frame the box is laid out in.
            if geometry::start_is_low_side_in_frame(frame, style.writing_mode(), style.direction(), is_horizontal_axis)
            {
                (axis_start, axis_end)
            } else {
                (axis_end, axis_start)
            }
        };
        // https://drafts.csswg.org/css-anchor-position-1/#valdef-anchor-percentage
        // Refers to a position a corresponding percentage between the start and end sides, with 0% being equivalent
        // to start and 100% being equivalent to end.
        let between_start_and_end = |fraction: f64| {
            let (start, end) = start_and_end_sides(containing_block);
            start + CssPixels::nearest_value_for((end - start).to_double() * fraction)
        };
        match side {
            AnchorSide::Invalid => None,
            AnchorSide::Top => (!is_horizontal_axis).then_some(rect.top()),
            AnchorSide::Bottom => (!is_horizontal_axis).then_some(rect.bottom()),
            AnchorSide::Left => is_horizontal_axis.then_some(rect.left()),
            AnchorSide::Right => is_horizontal_axis.then_some(rect.right()),
            // https://drafts.csswg.org/css-anchor-position-1/#valdef-anchor-center
            // center is equivalent to 50%.
            AnchorSide::Center => Some(between_start_and_end(0.5)),
            AnchorSide::Start => Some(start_and_end_sides(containing_block).0),
            AnchorSide::End => Some(start_and_end_sides(containing_block).1),
            AnchorSide::SelfStart => Some(start_and_end_sides(positioned_box).0),
            AnchorSide::SelfEnd => Some(start_and_end_sides(positioned_box).1),
            AnchorSide::Inside | AnchorSide::Outside => {
                let same_side = side == AnchorSide::Inside;
                Some(if is_from_end == same_side { axis_end } else { axis_start })
            }
            AnchorSide::Percentage(fraction) => Some(between_start_and_end(fraction)),
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-size-fn
    /// The length an anchor-size() function with the given <anchor-size> keyword resolves to against the anchor box
    /// `rect`, used in a property of the horizontal axis or not.
    fn anchor_size(
        &self,
        anchor_size: Option<u8>,
        rect: CssPixelRect,
        positioned_box: Node,
        containing_block: Node,
        is_horizontal_axis: bool,
    ) -> CssPixels {
        use crate::css::css_enums::anchor_size;
        // The logical <anchor-size> keywords (block, inline, self-block, and self-inline) map to one of the physical
        // keywords according to either the writing mode of the box (for self-block and self-inline) or the writing
        // mode of the box's containing block (for block and inline).
        let inline_axis_is_horizontal =
            |writing_mode_box: Node| self.style(writing_mode_box).writing_mode() == writing_mode::HORIZONTAL_TB;
        // If the <anchor-size> keyword is omitted, it defaults to behaving as whatever keyword matches the axis of
        // the property that anchor-size() is used in.
        let measures_physical_width = |anchor_size: u8| match anchor_size {
            anchor_size::WIDTH => true,
            anchor_size::HEIGHT => false,
            anchor_size::INLINE => inline_axis_is_horizontal(containing_block),
            anchor_size::BLOCK => !inline_axis_is_horizontal(containing_block),
            anchor_size::SELF_INLINE => inline_axis_is_horizontal(positioned_box),
            anchor_size::SELF_BLOCK => !inline_axis_is_horizontal(positioned_box),
            _ => unreachable!("anchor-size() holds an unknown <anchor-size> keyword"),
        };
        // NB: The rect and the axis of the property lie in the frame the box is laid out in, whose horizontal axis is
        //     the physical vertical axis in a vertical writing mode.
        let measures_width = match anchor_size {
            None => is_horizontal_axis,
            Some(anchor_size) => measures_physical_width(anchor_size) != self.pass.frame.is_transposed(),
        };
        if measures_width { rect.width } else { rect.height }
    }

    fn note_resolved_anchor_function(
        &self,
        anchor_box: Node,
        is_horizontal_axis: bool,
        state: &mut AnchorResolutionState,
    ) {
        if state.default_anchor_box.is_invalid() {
            return;
        }
        if anchor_box != state.default_anchor_box
            && self.nearest_scroll_container_ancestor(anchor_box)
                != self.nearest_scroll_container_ancestor(state.default_anchor_box)
        {
            return;
        }
        if is_horizontal_axis {
            state.compensates_for_horizontal_scroll = true;
        } else {
            state.compensates_for_vertical_scroll = true;
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn resolve_anchor_value(
        &self,
        calculated: *const c_void,
        positioned_box: Node,
        containing_block: Node,
        containing_block_geometry: ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
        axis: AnchorValueAxis,
        resolution_state: &mut AnchorResolutionState,
    ) -> Option<CssPixels> {
        let mut callback_context = AnchorCalcCallbackContext {
            engine: self,
            positioned_box,
            containing_block,
            containing_block_geometry,
            entry_coordinate_space_box,
            is_from_end: axis.is_from_end,
            is_horizontal_axis: axis.is_horizontal,
            containing_block_extent: axis.containing_block_extent,
            resolution_state,
        };
        // SAFETY: The calculated handle is retained by the style cache and
        // all callback state remains live for this synchronous resolution.
        let result = unsafe {
            style_values::resolve_calc_with_external_resolutions(
                calculated,
                axis.containing_block_extent,
                (&raw mut callback_context).cast(),
                Some(resolve_anchor_non_math_function),
            )
        };
        result.resolved.then(|| CssPixels::truncated_value_for(result.value))
    }

    fn resolve_anchor_insets(
        &self,
        node: Node,
        containing_block: Node,
        entry_containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
        position_area_anchor_box: Option<Node>,
    ) -> (
        Option<formatting_context::ResolvedAnchorInsets>,
        abspos_inputs::DefaultScrollShift,
    ) {
        let (resolved, default_scroll_shift) = self.resolve_anchor_insets_and_default_scroll_shift(
            node,
            containing_block,
            entry_containing_block_geometry,
            entry_coordinate_space_box,
        );
        // https://drafts.csswg.org/css-anchor-position/#scroll
        // A non-none position-area makes the box compensate for its default anchor's scroll in both axes.
        let default_scroll_shift = match position_area_anchor_box {
            Some(anchor_box) => AnchorResolutionState {
                default_anchor_box: anchor_box,
                compensates_for_horizontal_scroll: true,
                compensates_for_vertical_scroll: true,
                resolved_against_anchor_box: false,
            },
            None => default_scroll_shift,
        };
        // NB: Scrolling is physical, while the axes the shift was resolved in are those of the frame.
        let (compensates_for_horizontal_scroll, compensates_for_vertical_scroll) = if self.pass.frame.is_transposed() {
            (
                default_scroll_shift.compensates_for_vertical_scroll,
                default_scroll_shift.compensates_for_horizontal_scroll,
            )
        } else {
            (
                default_scroll_shift.compensates_for_horizontal_scroll,
                default_scroll_shift.compensates_for_vertical_scroll,
            )
        };
        (
            resolved,
            abspos_inputs::DefaultScrollShift {
                anchor: default_scroll_shift.default_anchor_box,
                compensates_for_horizontal_scroll,
                compensates_for_vertical_scroll,
            },
        )
    }

    fn resolve_anchor_insets_and_default_scroll_shift(
        &self,
        node: Node,
        containing_block: Node,
        entry_containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
    ) -> (Option<formatting_context::ResolvedAnchorInsets>, AnchorResolutionState) {
        let style = self.style(node);
        if ![
            style.inset_top(),
            style.inset_right(),
            style.inset_bottom(),
            style.inset_left(),
        ]
        .into_iter()
        .any(style_values::InsetValue::contains_anchor_function)
        {
            return (None, AnchorResolutionState::NO_SCROLL_SHIFT);
        }

        if containing_block.is_invalid() {
            return (None, AnchorResolutionState::NO_SCROLL_SHIFT);
        }
        let containing_block_geometry = *entry_containing_block_geometry;
        let default_anchor_box = if style.has_position_anchor() {
            self.anchor_lookup(node, style.position_anchor_name())
                .unwrap_or(NodeSlotId::INVALID)
        } else {
            NodeSlotId::INVALID
        };
        let mut resolution_state = AnchorResolutionState {
            default_anchor_box,
            compensates_for_horizontal_scroll: false,
            compensates_for_vertical_scroll: false,
            resolved_against_anchor_box: false,
        };
        let mut resolve_inset = |inset: style_values::InsetValue<'_>, axis: AnchorValueAxis| {
            inset.contains_anchor_function().then(|| {
                self.resolve_anchor_value(
                    inset.anchor_bearing_calculated(),
                    node,
                    containing_block,
                    containing_block_geometry,
                    entry_coordinate_space_box,
                    axis,
                    &mut resolution_state,
                )
                .map_or(
                    style_values::ResolvedInsetOverride::Auto,
                    style_values::ResolvedInsetOverride::Pixels,
                )
            })
        };
        let resolved = formatting_context::ResolvedAnchorInsets {
            top: resolve_inset(
                style.inset_top(),
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: false,
                    containing_block_extent: containing_block_geometry.padding_box_block_size(),
                },
            ),
            right: resolve_inset(
                style.inset_right(),
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: true,
                    containing_block_extent: containing_block_geometry.padding_box_inline_size(),
                },
            ),
            bottom: resolve_inset(
                style.inset_bottom(),
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: false,
                    containing_block_extent: containing_block_geometry.padding_box_block_size(),
                },
            ),
            left: resolve_inset(
                style.inset_left(),
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: true,
                    containing_block_extent: containing_block_geometry.padding_box_inline_size(),
                },
            ),
        };

        let default_scroll_shift =
            if resolution_state.compensates_for_horizontal_scroll || resolution_state.compensates_for_vertical_scroll {
                resolution_state
            } else {
                AnchorResolutionState::NO_SCROLL_SHIFT
            };
        (Some(resolved), default_scroll_shift)
    }

    /// Resolves the anchor-size() functions in the sizing and margin properties of `node`.
    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-size-fn
    fn resolve_anchor_sizes(
        &self,
        node: Node,
        containing_block: Node,
        containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
    ) -> style_values::ResolvedAnchorSizes {
        // NB: Read the computed values themselves, not what an earlier resolution made of them, in the frame the box
        //     is laid out in.
        let style = StyleValues::for_node(&self.pass, node);
        // NB: Each resolution also tells whether any of its anchor functions resolved against an anchor box.
        let resolve = |calculated: *const c_void, is_horizontal: bool, containing_block_extent: CssPixels| {
            let mut resolution_state = AnchorResolutionState::NO_SCROLL_SHIFT;
            let resolved = self.resolve_anchor_value(
                calculated,
                node,
                containing_block,
                *containing_block_geometry,
                entry_coordinate_space_box,
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal,
                    containing_block_extent,
                },
                &mut resolution_state,
            );
            (resolved, resolution_state.resolved_against_anchor_box)
        };
        let inline_size = containing_block_geometry.padding_box_inline_size();
        let block_size = containing_block_geometry.padding_box_block_size();
        // https://drafts.csswg.org/css-anchor-position-1/#anchor-size-resolution
        // If no fallback value is specified, it makes the declaration referencing it invalid at computed-value time.
        let resolve_size = |size: &ComputedSize, is_horizontal: bool, initial: ComputedSizeKind| {
            if !size.contains_anchor_function() {
                return None;
            }
            let extent = if is_horizontal { inline_size } else { block_size };
            match resolve(size.length_percentage().calculated_pointer(), is_horizontal, extent) {
                // NB: A size whose anchor functions all resolve to their fallback values resolves like anywhere
                //     else, so it keeps reading from style. A resolved size thus always comes from an anchor box.
                (Some(_), false) => None,
                (Some(px), true) => Some(style_values::ResolvedSizeOverride::Length(px)),
                (None, _) => Some(style_values::ResolvedSizeOverride::Keyword(initial)),
            }
        };
        let mut resolved = style_values::ResolvedAnchorSizes {
            width: resolve_size(style.unresolved_width(), true, ComputedSizeKind::Auto),
            min_width: resolve_size(style.unresolved_min_width(), true, ComputedSizeKind::Auto),
            max_width: resolve_size(style.unresolved_max_width(), true, ComputedSizeKind::None),
            height: resolve_size(style.unresolved_height(), false, ComputedSizeKind::Auto),
            min_height: resolve_size(style.unresolved_min_height(), false, ComputedSizeKind::Auto),
            max_height: resolve_size(style.unresolved_max_height(), false, ComputedSizeKind::None),
            ..Default::default()
        };
        let resolve_margin = |margin: &ComputedLengthPercentageOrAuto, is_horizontal: bool| {
            let length_percentage = margin.length_percentage()?;
            if !length_percentage.contains_anchor_function() {
                return None;
            }
            // NB: Margin percentages resolve against the inline size of the containing block in both axes.
            Some(
                resolve(length_percentage.calculated_pointer(), is_horizontal, inline_size)
                    .0
                    .unwrap_or_default(),
            )
        };
        resolved.margin_top = resolve_margin(style.unresolved_margin_top(), false);
        resolved.margin_right = resolve_margin(style.unresolved_margin_right(), true);
        resolved.margin_bottom = resolve_margin(style.unresolved_margin_bottom(), false);
        resolved.margin_left = resolve_margin(style.unresolved_margin_left(), true);
        resolved
    }

    /// Zeroes the auto margins of `node` in the axes a position area or anchor-center aligns it in.
    fn zero_aligned_auto_margins(
        &self,
        node: Node,
        resolved: &mut style_values::ResolvedAnchorSizes,
        containing_block_info: &abspos_inputs::AbsposContainingBlockInfo,
    ) {
        // https://drafts.csswg.org/css-anchor-position-1/#position-area
        // The used value of any auto inset properties and auto margin properties resolves to 0.
        // https://drafts.csswg.org/css-anchor-position-1/#anchor-center
        // The used value of any auto inset properties and auto margin properties resolves to 0.
        let (aligns_inline, aligns_block) = containing_block_info.anchor_aligned_axes();
        if !aligns_inline && !aligns_block {
            return;
        }
        let style = self.style(node);
        let zero_if_auto = |margin: &ComputedLengthPercentageOrAuto, current: &mut Option<CssPixels>| {
            if current.is_none() && margin.is_auto() {
                *current = Some(CssPixels::default());
            }
        };
        if aligns_inline {
            zero_if_auto(style.margin_left(), &mut resolved.margin_left);
            zero_if_auto(style.margin_right(), &mut resolved.margin_right);
        }
        if aligns_block {
            zero_if_auto(style.margin_top(), &mut resolved.margin_top);
            zero_if_auto(style.margin_bottom(), &mut resolved.margin_bottom);
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-center
    /// If the positioned box has a default anchor box, then it is centered (insofar as possible) over the default
    /// anchor box in the relevant axis.
    /// NB: This handles an axis whose insets are both auto. A position area handles anchor-center itself.
    #[allow(clippy::too_many_arguments)]
    fn apply_anchor_center_alignment(
        &self,
        positioned_box: Node,
        containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
        containing_block_info: &mut abspos_inputs::AbsposContainingBlockInfo,
    ) {
        let style = self.style(positioned_box).with_resolved_insets(resolved_anchor_insets);
        // FIXME: Center an axis with exactly one auto inset too, within an inset-modified containing block that
        //        treats that inset as 0.
        let insets_are_both_auto_or_both_not =
            |start: style_values::InsetValue<'_>, end: style_values::InsetValue<'_>| start.is_auto() == end.is_auto();
        let centers_inline = style.justify_self() == justify_self::ANCHOR_CENTER
            && insets_are_both_auto_or_both_not(style.inset_left(), style.inset_right());
        let centers_block = style.align_self() == align_self::ANCHOR_CENTER
            && insets_are_both_auto_or_both_not(style.inset_top(), style.inset_bottom());
        if (!centers_inline && !centers_block)
            || !style.has_position_anchor()
            || !self.facts(positioned_box).is_absolutely_positioned()
        {
            return;
        }
        let Some(anchor_box) = self.anchor_lookup(positioned_box, style.position_anchor_name()) else {
            return;
        };
        let anchor_rect = self.anchor_rect(anchor_box, containing_block_geometry, entry_coordinate_space_box);
        // NB: The containing block rect is relative to the containing block's content box, while anchor_rect() is
        //     relative to its padding box.
        let anchor_centered = |anchor_start: CssPixels, anchor_size| abspos_inputs::AxisPlacement {
            mode: abspos_inputs::AbsposAxisMode::InsetFromRect,
            alignment: Some(AbsposAxisAlignment::AnchorCenter {
                anchor_start,
                anchor_size,
            }),
        };
        let rect = containing_block_info.rect;
        if centers_inline {
            containing_block_info.inline_placement = anchor_centered(
                anchor_rect.left() - containing_block_geometry.padding_left - rect.offset.inline_offset,
                anchor_rect.width,
            );
        }
        if centers_block {
            containing_block_info.block_placement = anchor_centered(
                anchor_rect.top() - containing_block_geometry.padding_top - rect.offset.block_offset,
                anchor_rect.height,
            );
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#position-area
    /// The used value of any auto inset properties [...] resolves to 0.
    /// NB: Only a stretched axis needs its auto insets as 0 here: the other alignments size the box to fit its
    ///     contents, and then place it within the position area themselves.
    fn position_area_stretch_insets(
        &self,
        node: Node,
        resolved: Option<formatting_context::ResolvedAnchorInsets>,
        stretches_inline: bool,
        stretches_block: bool,
    ) -> Option<formatting_context::ResolvedAnchorInsets> {
        if !stretches_inline && !stretches_block {
            return resolved;
        }
        let insets = resolved.unwrap_or_default();
        let style = self.style(node).with_resolved_insets(Some(&insets));
        let zero_if_auto = |value: style_values::InsetValue<'_>, stretches: bool, current| {
            if stretches && value.is_auto() {
                Some(style_values::ResolvedInsetOverride::Pixels(CssPixels::default()))
            } else {
                current
            }
        };
        Some(formatting_context::ResolvedAnchorInsets {
            top: zero_if_auto(style.inset_top(), stretches_block, insets.top),
            right: zero_if_auto(style.inset_right(), stretches_inline, insets.right),
            bottom: zero_if_auto(style.inset_bottom(), stretches_block, insets.bottom),
            left: zero_if_auto(style.inset_left(), stretches_inline, insets.left),
        })
    }

    /// The layout inputs a non-none position-area gives `positioned_box`: its containing block info, the geometry of
    /// its new containing block, its default anchor box, and whether each of its axes (inline, then block) stretches to
    /// fill its position area.
    fn position_area_layout_inputs(
        &self,
        positioned_box: Node,
        containing_block: Node,
        containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
        mut containing_block_info: abspos_inputs::AbsposContainingBlockInfo,
    ) -> Option<(
        abspos_inputs::AbsposContainingBlockInfo,
        ContainingBlockGeometry,
        Node,
        (bool, bool),
    )> {
        let style = self.style(positioned_box);
        let position_area = style.anchor().position_area.as_slice();
        if position_area.is_empty() {
            return None;
        }

        // https://drafts.csswg.org/css-anchor-position-1/#position-area
        // If the box does not have a default anchor box, or is not an absolutely positioned box, this value has no
        // effect.
        if !style.has_position_anchor() || !self.facts(positioned_box).is_absolutely_positioned() {
            return None;
        }
        let containing_block_style = self.style(containing_block);
        let tracks = resolve_position_area_tracks(
            position_area,
            PositionAreaWritingMode {
                writing_mode: containing_block_style.writing_mode(),
                direction: containing_block_style.direction(),
            },
            PositionAreaWritingMode {
                writing_mode: style.writing_mode(),
                direction: style.direction(),
            },
        )?
        .in_frame(self.pass.frame);

        let anchor_box = self.anchor_lookup(positioned_box, style.position_anchor_name())?;
        let anchor_rect = self.anchor_rect(anchor_box, containing_block_geometry, entry_coordinate_space_box);

        // anchor_rect() is padding-box-relative, while containing_block_info.rect is content-box-relative. Normalize
        // them before comparing their edges.
        let anchor_inline_offset = anchor_rect.left() - containing_block_geometry.padding_left;
        let anchor_block_offset = anchor_rect.top() - containing_block_geometry.padding_top;
        let anchor_inline_end = anchor_rect.right() - containing_block_geometry.padding_left;
        let anchor_block_end = anchor_rect.bottom() - containing_block_geometry.padding_top;
        let containing_block_end = geometry::LogicalOffset {
            inline_offset: containing_block_info.rect.offset.inline_offset
                + containing_block_info.rect.size.inline_size,
            block_offset: containing_block_info.rect.offset.block_offset + containing_block_info.rect.size.block_size,
        };

        // https://drafts.csswg.org/css-anchor-position-1/#position-area-grid-resolution
        // The position-area grid is a 3×3 grid, composed of four grid lines in each axis. In order (using the writing
        // mode of the containing block):
        // - the start edge of the box's pre-modification containing block, or the start edge of the default anchor
        //   box if that is more start-ward
        // - the start edge of the default anchor box
        // - the end edge of the default anchor box
        // - the end edge of the box's pre-modification containing block, or the end edge of the default anchor box
        //   if that is more end-ward.
        // NB: The lines are kept in physical order here; the resolved tracks are physical as well.
        let inline_lines = [
            containing_block_info
                .rect
                .offset
                .inline_offset
                .min(anchor_inline_offset),
            anchor_inline_offset,
            anchor_inline_end,
            containing_block_end.inline_offset.max(anchor_inline_end),
        ];
        let block_lines = [
            containing_block_info.rect.offset.block_offset.min(anchor_block_offset),
            anchor_block_offset,
            anchor_block_end,
            containing_block_end.block_offset.max(anchor_block_end),
        ];
        let inline_offset = inline_lines[tracks.horizontal.first as usize];
        let inline_size = inline_lines[tracks.horizontal.last as usize + 1] - inline_offset;
        let block_offset = block_lines[tracks.vertical.first as usize];
        let block_size = block_lines[tracks.vertical.last as usize + 1] - block_offset;

        // https://drafts.csswg.org/css-anchor-position-1/#position-area
        // Otherwise, selects a region of the position-area grid, and makes that the box's containing block.
        containing_block_info.rect = geometry::LogicalRect {
            offset: geometry::LogicalOffset {
                inline_offset,
                block_offset,
            },
            size: geometry::LogicalSize {
                inline_size,
                block_size,
            },
        };

        // https://drafts.csswg.org/css-anchor-position-1/#position-area-alignment
        // When position-area is not none, the used value of normal self-alignment changes depending on the
        // <position-area> value, to align the box towards the anchor.
        // NB: anchor-center centers the box over the anchor as the new containing block's coordinate space has it.
        let inline_alignment = self_alignment_for_position_area(
            SelfKeyword::justify_self_of(*style, None).0,
            tracks.horizontal,
            AxisDirections {
                self_start_is_low: style.direction() == direction::LTR,
                ..AxisDirections::of(containing_block_style.direction() == direction::LTR, Some(true))
            },
            AbsposAxisAlignment::AnchorCenter {
                anchor_start: anchor_inline_offset - inline_offset,
                anchor_size: anchor_rect.width,
            },
        );
        let block_alignment = self_alignment_for_position_area(
            SelfKeyword::align_self_of(*style, None).0,
            tracks.vertical,
            AxisDirections::of(true, None),
            AbsposAxisAlignment::AnchorCenter {
                anchor_start: anchor_block_offset - block_offset,
                anchor_size: anchor_rect.height,
            },
        );
        let stretches = (
            inline_alignment == AbsposAxisAlignment::Stretch,
            block_alignment == AbsposAxisAlignment::Stretch,
        );
        let placed_by_area = |alignment| abspos_inputs::AxisPlacement {
            mode: abspos_inputs::AbsposAxisMode::InsetFromRect,
            alignment: Some(alignment),
        };
        containing_block_info.inline_placement = placed_by_area(inline_alignment);
        containing_block_info.block_placement = placed_by_area(block_alignment);
        containing_block_info.is_position_area = true;

        let position_area_geometry = ContainingBlockGeometry {
            content_origin_in_entry_space: formatting_context::point_add(
                containing_block_geometry.content_origin_in_entry_space,
                FfiCssPixelPoint {
                    x: inline_offset,
                    y: block_offset,
                },
            ),
            padding_left: CssPixels::default(),
            padding_right: CssPixels::default(),
            padding_top: CssPixels::default(),
            padding_bottom: CssPixels::default(),
            content_inline_size: inline_size,
            content_block_size: block_size,
        };

        Some((containing_block_info, position_area_geometry, anchor_box, stretches))
    }
}

/// The tracks of the position-area grid a <position-area> selects in one physical axis, counted from the left or top.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PositionAreaTrackRange {
    pub(crate) first: u8,
    pub(crate) last: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PositionAreaTracks {
    pub(crate) horizontal: PositionAreaTrackRange,
    pub(crate) vertical: PositionAreaTrackRange,
}

impl PositionAreaTrackRange {
    pub(crate) fn reversed(self) -> Self {
        Self {
            first: 2 - self.last,
            last: 2 - self.first,
        }
    }
}

impl PositionAreaTracks {
    /// The tracks along the axes of `frame`, counted from its left or top, rather than along the physical axes.
    fn in_frame(self, frame: geometry::Frame) -> Self {
        if frame.is_physical() {
            return self;
        }
        let along_frame_axis = |frame_side: geometry::Side| {
            let physical_low_side = frame.physical_side(frame_side);
            let range = match physical_low_side {
                geometry::Side::Left | geometry::Side::Right => self.horizontal,
                geometry::Side::Top | geometry::Side::Bottom => self.vertical,
            };
            if matches!(physical_low_side, geometry::Side::Right | geometry::Side::Bottom) {
                range.reversed()
            } else {
                range
            }
        };
        Self {
            horizontal: along_frame_axis(geometry::Side::Left),
            vertical: along_frame_axis(geometry::Side::Top),
        }
    }
}

#[derive(Clone, Copy)]
pub(crate) struct PositionAreaWritingMode {
    pub(crate) writing_mode: u8,
    pub(crate) direction: u8,
}

impl PositionAreaWritingMode {
    fn inline_axis_is_horizontal(self) -> bool {
        self.writing_mode == writing_mode::HORIZONTAL_TB
    }

    /// Whether the start side of the given physical axis is its left or top side.
    fn start_is_left_or_top(self, horizontal: bool) -> bool {
        geometry::start_is_left_or_top(self.writing_mode, self.direction, horizontal)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PositionAreaAxis {
    Horizontal,
    Vertical,
    Block,
    Inline,
    Ambiguous,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PositionAreaReference {
    Physical,
    ContainingBlock,
    OwnWritingMode,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PositionAreaSelection {
    Start,
    End,
    SpanStart,
    SpanEnd,
    Center,
    All,
}

fn classify_position_area_keyword(code: u8) -> (PositionAreaAxis, PositionAreaSelection, PositionAreaReference) {
    use PositionAreaAxis::*;
    use PositionAreaReference::*;
    use PositionAreaSelection::*;
    match code {
        position_area::LEFT => (Horizontal, Start, Physical),
        position_area::RIGHT => (Horizontal, End, Physical),
        position_area::SPAN_LEFT => (Horizontal, SpanStart, Physical),
        position_area::SPAN_RIGHT => (Horizontal, SpanEnd, Physical),
        position_area::X_START => (Horizontal, Start, ContainingBlock),
        position_area::X_END => (Horizontal, End, ContainingBlock),
        position_area::SPAN_X_START => (Horizontal, SpanStart, ContainingBlock),
        position_area::SPAN_X_END => (Horizontal, SpanEnd, ContainingBlock),
        position_area::SELF_X_START => (Horizontal, Start, OwnWritingMode),
        position_area::SELF_X_END => (Horizontal, End, OwnWritingMode),
        position_area::SPAN_SELF_X_START => (Horizontal, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_X_END => (Horizontal, SpanEnd, OwnWritingMode),
        position_area::TOP => (Vertical, Start, Physical),
        position_area::BOTTOM => (Vertical, End, Physical),
        position_area::SPAN_TOP => (Vertical, SpanStart, Physical),
        position_area::SPAN_BOTTOM => (Vertical, SpanEnd, Physical),
        position_area::Y_START => (Vertical, Start, ContainingBlock),
        position_area::Y_END => (Vertical, End, ContainingBlock),
        position_area::SPAN_Y_START => (Vertical, SpanStart, ContainingBlock),
        position_area::SPAN_Y_END => (Vertical, SpanEnd, ContainingBlock),
        position_area::SELF_Y_START => (Vertical, Start, OwnWritingMode),
        position_area::SELF_Y_END => (Vertical, End, OwnWritingMode),
        position_area::SPAN_SELF_Y_START => (Vertical, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_Y_END => (Vertical, SpanEnd, OwnWritingMode),
        position_area::BLOCK_START => (Block, Start, ContainingBlock),
        position_area::BLOCK_END => (Block, End, ContainingBlock),
        position_area::SPAN_BLOCK_START => (Block, SpanStart, ContainingBlock),
        position_area::SPAN_BLOCK_END => (Block, SpanEnd, ContainingBlock),
        position_area::INLINE_START => (Inline, Start, ContainingBlock),
        position_area::INLINE_END => (Inline, End, ContainingBlock),
        position_area::SPAN_INLINE_START => (Inline, SpanStart, ContainingBlock),
        position_area::SPAN_INLINE_END => (Inline, SpanEnd, ContainingBlock),
        position_area::SELF_BLOCK_START => (Block, Start, OwnWritingMode),
        position_area::SELF_BLOCK_END => (Block, End, OwnWritingMode),
        position_area::SPAN_SELF_BLOCK_START => (Block, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_BLOCK_END => (Block, SpanEnd, OwnWritingMode),
        position_area::SELF_INLINE_START => (Inline, Start, OwnWritingMode),
        position_area::SELF_INLINE_END => (Inline, End, OwnWritingMode),
        position_area::SPAN_SELF_INLINE_START => (Inline, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_INLINE_END => (Inline, SpanEnd, OwnWritingMode),
        position_area::START => (Ambiguous, Start, ContainingBlock),
        position_area::END => (Ambiguous, End, ContainingBlock),
        position_area::SPAN_START => (Ambiguous, SpanStart, ContainingBlock),
        position_area::SPAN_END => (Ambiguous, SpanEnd, ContainingBlock),
        position_area::SELF_START => (Ambiguous, Start, OwnWritingMode),
        position_area::SELF_END => (Ambiguous, End, OwnWritingMode),
        position_area::SPAN_SELF_START => (Ambiguous, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_END => (Ambiguous, SpanEnd, OwnWritingMode),
        position_area::SPAN_ALL => (Ambiguous, All, ContainingBlock),
        _ => (Ambiguous, Center, ContainingBlock),
    }
}

/// https://drafts.csswg.org/css-anchor-position-1/#position-area-syntax
pub(crate) fn resolve_position_area_tracks(
    codes: &[u8],
    containing_block: PositionAreaWritingMode,
    own: PositionAreaWritingMode,
) -> Option<PositionAreaTracks> {
    let writing_mode_for = |reference: PositionAreaReference| match reference {
        PositionAreaReference::OwnWritingMode => own,
        PositionAreaReference::Physical | PositionAreaReference::ContainingBlock => containing_block,
    };
    // Whether the keyword specifies the horizontal axis, if it is unambiguous about its axis.
    let axis_is_horizontal = |code: u8| {
        let (axis, _, reference) = classify_position_area_keyword(code);
        match axis {
            PositionAreaAxis::Horizontal => Some(true),
            PositionAreaAxis::Vertical => Some(false),
            PositionAreaAxis::Inline => Some(writing_mode_for(reference).inline_axis_is_horizontal()),
            PositionAreaAxis::Block => Some(!writing_mode_for(reference).inline_axis_is_horizontal()),
            PositionAreaAxis::Ambiguous => None,
        }
    };
    // INTEROP: When both keywords are self-* keywords, other browsers use the block axis of the box's own writing
    //          mode. This keeps a value like self-block-end self-inline-start working once it computes to its short
    //          form, self-end self-start.
    let (_, _, first_reference) = classify_position_area_keyword(codes[0]);
    let block_axis_is_horizontal = !writing_mode_for(first_reference).inline_axis_is_horizontal();

    if codes.is_empty() {
        return None;
    }

    // If only a single keyword is given, it behaves as if the second keyword is span-all if the given keyword is
    // unambiguous about its axis; otherwise, it behaves as if the given keyword was repeated.
    let (first, second) = match *codes {
        [first] if axis_is_horizontal(first).is_some() => (first, position_area::SPAN_ALL),
        [first] => (first, first),
        [first, second] => (first, second),
        _ => return None,
    };

    // If the other keyword is unambiguous about its axis, then the ambiguous keyword is referring to the opposite
    // axis. If both keywords are ambiguous, however, then the first refers to the block axis of the box's
    // containing block, and the second to the inline axis.
    let first_is_horizontal = match (axis_is_horizontal(first), axis_is_horizontal(second)) {
        (Some(first), Some(second)) if first == second => return None,
        (Some(first), _) => first,
        (None, Some(second)) => !second,
        (None, None) => block_axis_is_horizontal,
    };

    let range_for = |code: u8, horizontal: bool| {
        let (_, selection, reference) = classify_position_area_keyword(code);
        let (first, last) = match selection {
            PositionAreaSelection::Start => (0, 0),
            PositionAreaSelection::End => (2, 2),
            PositionAreaSelection::SpanStart => (0, 1),
            PositionAreaSelection::SpanEnd => (1, 2),
            PositionAreaSelection::Center => (1, 1),
            PositionAreaSelection::All => (0, 2),
        };
        let start_is_left_or_top = reference == PositionAreaReference::Physical
            || writing_mode_for(reference).start_is_left_or_top(horizontal);
        if start_is_left_or_top {
            PositionAreaTrackRange { first, last }
        } else {
            PositionAreaTrackRange {
                first: 2 - last,
                last: 2 - first,
            }
        }
    };
    let first_range = range_for(first, first_is_horizontal);
    let second_range = range_for(second, !first_is_horizontal);
    Some(if first_is_horizontal {
        PositionAreaTracks {
            horizontal: first_range,
            vertical: second_range,
        }
    } else {
        PositionAreaTracks {
            horizontal: second_range,
            vertical: first_range,
        }
    })
}

/// The used self-alignment of a box with a non-none position-area in one physical axis, where Start and End are
/// the left/top and right/bottom sides.
fn self_alignment_for_position_area(
    keyword: SelfKeyword,
    tracks: PositionAreaTrackRange,
    directions: AxisDirections,
    anchor_center: AbsposAxisAlignment,
) -> AbsposAxisAlignment {
    match keyword {
        SelfKeyword::AnchorCenter => anchor_center,
        SelfKeyword::Stretch => AbsposAxisAlignment::Stretch,
        // https://drafts.csswg.org/css-anchor-position-1/#position-area-alignment
        // - If the only the center track in an axis is selected, the default alignment in that axis is center.
        // - If all three tracks are selected, the default alignment in that axis is anchor-center.
        // - Otherwise, the default alignment in that axis is toward the non-specified side track: if it's
        //   specifying the “start” track of its axis, the default alignment in that axis is end; etc.
        SelfKeyword::Normal => match (tracks.first, tracks.last) {
            (1, 1) => AbsposAxisAlignment::Edge(AxisEdge::Center),
            (0, 2) => anchor_center,
            (0, _) => AbsposAxisAlignment::Edge(AxisEdge::End),
            _ => AbsposAxisAlignment::Edge(AxisEdge::Start),
        },
        keyword => AbsposAxisAlignment::Edge(edge_of_own_alignment(
            keyword.resolve(OverflowSafety::Default, directions),
        )),
    }
}

/// The edge that a box aligns itself to in its inset-modified containing block, where baseline alignment aligns it
/// as start.
fn edge_of_own_alignment(alignment: SelfAlignment) -> AxisEdge {
    match alignment.position {
        SelfPosition::Edge(edge) => edge,
        _ => alignment.container_start,
    }
}

unsafe extern "C" fn resolve_anchor_non_math_function(context: *mut c_void, shell: *const c_void) -> *const c_void {
    use crate::css::style_value::StyleValueData;
    // SAFETY: The CSS calc engine calls this only during resolve_anchor_value,
    // whose stack owns this callback context.
    let context = unsafe { &mut *context.cast::<AnchorCalcCallbackContext>() };
    // SAFETY: The engine pointer is live for the enclosing resolution.
    let engine = unsafe { &*context.engine };
    // SAFETY: `shell` is the live Rust style-value data retained by the CSS
    // calc core's non-math-function node for this synchronous resolution.
    let Some((has_anchor_name, explicit_anchor_name, fallback_value)) =
        unsafe { &*shell.cast::<StyleValueData>() }.anchor_name_and_fallback()
    else {
        return std::ptr::null();
    };

    let style = engine.style(context.positioned_box);
    let bound_anchor_name = has_anchor_name.then(|| explicit_anchor_name.to_fly_string());
    let anchor_name = if let Some(name) = &bound_anchor_name {
        Some(name.raw())
    } else if style.has_position_anchor() {
        Some(style.position_anchor_name())
    } else {
        None
    };
    if engine.facts(context.positioned_box).is_absolutely_positioned()
        && let Some(anchor_name) = anchor_name
        && let Some(anchor_box) = engine.anchor_lookup(context.positioned_box, anchor_name)
    {
        let rect = engine.anchor_rect(
            anchor_box,
            &context.containing_block_geometry,
            context.entry_coordinate_space_box,
        );
        // SAFETY: As above.
        let resolved = match unsafe { &*shell.cast::<StyleValueData>() } {
            StyleValueData::Anchor(anchor_payload) => {
                let AnchorPayload { anchor_side, .. } = &**anchor_payload;
                engine
                    .anchor_side(
                        anchor_side_from_style_value(anchor_side.data()),
                        rect,
                        context.positioned_box,
                        context.containing_block,
                        context.is_from_end,
                        context.is_horizontal_axis,
                    )
                    .map(|side| {
                        // SAFETY: The state pointer is live and uniquely used by this
                        // synchronous resolver.
                        let resolution_state = unsafe { &mut *context.resolution_state };
                        engine.note_resolved_anchor_function(anchor_box, context.is_horizontal_axis, resolution_state);
                        if context.is_from_end {
                            context.containing_block_extent - side
                        } else {
                            side
                        }
                    })
            }
            StyleValueData::AnchorSize(anchor_size_payload) => {
                let AnchorSizePayload {
                    has_anchor_size,
                    anchor_size,
                    ..
                } = &**anchor_size_payload;
                Some(engine.anchor_size(
                    has_anchor_size.then_some(*anchor_size),
                    rect,
                    context.positioned_box,
                    context.containing_block,
                    context.is_horizontal_axis,
                ))
            }
            _ => unreachable!("the shell was matched as an anchor function above"),
        };
        if let Some(resolved) = resolved {
            // SAFETY: The state pointer is live and uniquely used by this synchronous resolver.
            unsafe { (*context.resolution_state).resolved_against_anchor_box = true };
            // SAFETY: This CSS crate export transfers one Arc reference to
            // the external-resolution snapshot, which releases it after
            // calc resolution.
            return style_values::calc_node_create_px_dimension(resolved.to_double());
        }
    }

    // SAFETY: The callback context is live for this synchronous resolution.
    unsafe {
        style_values::anchor_function_fallback_node(
            fallback_value,
            context.containing_block_extent,
            std::ptr::from_mut(context).cast(),
            resolve_anchor_non_math_function,
        )
    }
}

/// https://drafts.csswg.org/css-align-3/#overflow-values
/// The offset of a box that overflows its containing block on one side, shifted so it stays within it, if it fits.
fn shift_into_containing_block(offset: CssPixels, maximum_offset: CssPixels) -> CssPixels {
    if maximum_offset < CssPixels::default() {
        return offset;
    }
    offset.max(CssPixels::default()).min(maximum_offset)
}

type AutoPx = Option<CssPixels>;

/// The start and end insets of the inset-modified containing block along an axis of a containing block of size
/// `containing_block_size`, whose inset properties are `start` and `end`. An auto inset is zero, but where both are
/// auto, `static_position_insets` gives the insets at the static position, if any.
/// https://drafts.csswg.org/css-position-3/#inset-modified-containing-block
fn inset_modified_containing_block_insets(
    start: style_values::InsetValue<'_>,
    end: style_values::InsetValue<'_>,
    containing_block_size: CssPixels,
    static_position_insets: Option<(CssPixels, CssPixels)>,
) -> (CssPixels, CssPixels) {
    if start.is_auto()
        && end.is_auto()
        && let Some(insets) = static_position_insets
    {
        return insets;
    }
    let inset = |value: style_values::InsetValue<'_>| {
        if value.is_auto() {
            CssPixels::default()
        } else {
            value.to_px(containing_block_size)
        }
    };
    (inset(start), inset(end))
}

fn resolve_or_auto(value: style_values::InsetValue, basis: CssPixels) -> AutoPx {
    (!value.is_auto()).then(|| value.to_px(basis))
}

fn resolve_margin_or_auto(value: &ComputedLengthPercentageOrAuto, basis: CssPixels) -> AutoPx {
    (!value.is_auto()).then(|| value.to_px(basis))
}

fn auto_px_value(value: AutoPx) -> CssPixels {
    value.unwrap_or_default()
}

/// The insets and margins of an absolutely positioned box along one axis, which the absolute positioning equation
/// solves for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AxisSolution {
    pub(crate) start: CssPixels,
    pub(crate) end: CssPixels,
    pub(crate) margin_start: CssPixels,
    pub(crate) margin_end: CssPixels,
}

#[derive(Clone, Copy)]
pub(crate) enum AutoMarginOverflow {
    Split,
    LowSide,
    HighSide,
}

impl AutoMarginOverflow {
    fn distribute(self, remainder: CssPixels) -> (CssPixels, CssPixels) {
        if remainder < CssPixels::default() {
            match self {
                Self::LowSide => return (remainder, CssPixels::default()),
                Self::HighSide => return (CssPixels::default(), remainder),
                Self::Split => {}
            }
        }
        (remainder / 2, remainder / 2)
    }
}

/// The absolute positioning equation along one axis of a containing block, with the insets and margins of the box,
/// None where they are auto.
/// https://drafts.csswg.org/css-position-3/#abspos-layout
#[derive(Clone, Copy)]
pub(crate) struct AxisEquation {
    pub(crate) containing_block_size: CssPixels,
    pub(crate) start: AutoPx,
    pub(crate) end: AutoPx,
    pub(crate) margin_start: AutoPx,
    pub(crate) margin_end: AutoPx,
    pub(crate) borders_and_paddings: CssPixels,
    pub(crate) auto_margin_overflow: AutoMarginOverflow,
}

impl AxisEquation {
    fn has_both_insets(self) -> bool {
        self.start.is_some() && self.end.is_some()
    }

    /// The content size that fills the space between the insets and the margins, taking auto ones as zero.
    fn stretch_fit_size(self) -> CssPixels {
        (self.containing_block_size
            - auto_px_value(self.start)
            - auto_px_value(self.end)
            - auto_px_value(self.margin_start)
            - auto_px_value(self.margin_end)
            - self.borders_and_paddings)
            .max(CssPixels::default())
    }

    /// The insets and margins of a box whose size along the axis is unresolved: auto ones as zero.
    fn unresolved(self) -> AxisSolution {
        AxisSolution {
            start: auto_px_value(self.start),
            end: auto_px_value(self.end),
            margin_start: auto_px_value(self.margin_start),
            margin_end: auto_px_value(self.margin_end),
        }
    }

    /// The insets and margins of a box with the content size `size`, whose margin box `static_offset` places at its
    /// static position where both insets are auto. An over-constrained equation keeps both insets as specified, since
    /// their resolved values are then their computed values.
    /// https://drafts.csswg.org/cssom/#resolved-value-special-case-property-like-top
    fn solve(self, size: CssPixels, static_offset: impl FnOnce() -> CssPixels) -> AxisSolution {
        let Self {
            mut start,
            mut end,
            mut margin_start,
            mut margin_end,
            ..
        } = self;
        let available = self.containing_block_size - size - self.borders_and_paddings;
        if start.is_none() && end.is_none() {
            start = Some(static_offset());
        }
        // https://drafts.csswg.org/css-position-3/#abspos-margins
        // If either inset property in the relevant axis is auto, then any auto margins resolve to zero.
        if start.is_none() || end.is_none() {
            margin_start = Some(auto_px_value(margin_start));
            margin_end = Some(auto_px_value(margin_end));
        }
        if margin_start.is_none() && margin_end.is_none() {
            let remainder = available - auto_px_value(start) - auto_px_value(end);
            let (start_margin, end_margin) = self.auto_margin_overflow.distribute(remainder);
            margin_start = Some(start_margin);
            margin_end = Some(end_margin);
        }
        let rest = |a: AutoPx, b: AutoPx, c: AutoPx| available - auto_px_value(a) - auto_px_value(b) - auto_px_value(c);
        if start.is_none() {
            start = Some(rest(end, margin_start, margin_end));
        } else if end.is_none() {
            end = Some(rest(start, margin_start, margin_end));
        } else if margin_start.is_none() {
            margin_start = Some(rest(start, end, margin_end));
        } else if margin_end.is_none() {
            margin_end = Some(rest(start, margin_start, end));
        }
        AxisSolution {
            start: auto_px_value(start),
            end: auto_px_value(end),
            margin_start: auto_px_value(margin_start),
            margin_end: auto_px_value(margin_end),
        }
    }
}

impl AbsposEngine<'_> {
    fn auto_margin_overflow(&self, containing_block: Node, axis: SizingAxis) -> AutoMarginOverflow {
        let (writing_mode, direction) = if containing_block.is_invalid() {
            (writing_mode::HORIZONTAL_TB, direction::LTR)
        } else {
            let style = self.style(containing_block);
            (style.writing_mode(), style.direction())
        };
        let frame_horizontal = matches!(axis, SizingAxis::Inline);
        let containing_inline_is_frame_horizontal =
            (writing_mode == writing_mode::HORIZONTAL_TB) != self.pass.frame.is_transposed();
        if frame_horizontal != containing_inline_is_frame_horizontal {
            return AutoMarginOverflow::Split;
        }
        // https://drafts.csswg.org/css-position-3/#abspos-margins
        // However, (all with respect to the writing mode of the containing block), if in the inline axis the remaining
        // space is negative and both margins are auto, the start margin resolves to zero and the end margin receives
        // the remaining space.
        // NB: The negative remainder goes to the containing block's inline-end in this frame.
        if geometry::start_is_low_side_in_frame(self.pass.frame, writing_mode, direction, frame_horizontal) {
            AutoMarginOverflow::HighSide
        } else {
            AutoMarginOverflow::LowSide
        }
    }

    fn static_offset(
        &self,
        sized: OutOfFlowBox<'_>,
        rect: abspos_inputs::StaticPositionRect,
    ) -> geometry::LogicalOffset {
        let metrics = sized.cells;
        let collapsed = metrics.uses_collapsing_borders_model();
        aligned_static_offset(
            rect,
            metrics.margin_box_inline_size(collapsed),
            metrics.margin_box_block_size(collapsed),
        )
    }

    /// The inner size that a size property gives the box in `axis`. A stretch size fills the inset-modified
    /// containing block, where an auto inset is the static position at the start, or the containing block edge.
    /// https://drafts.csswg.org/css-sizing-3/#stretch-fit-sizing
    fn calculate_inner_size(
        &self,
        sized: OutOfFlowBox<'_>,
        axis: SizingAxis,
        size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        inputs: &abspos_inputs::AbsposLayoutInputs,
    ) -> CssPixels {
        let sizing = self.sizing();
        if !size.is_stretch() {
            return match axis {
                SizingAxis::Inline => sizing.calculate_inner_inline_size(
                    sized.sized_box(),
                    available_space.inline_size,
                    size,
                    constraints,
                ),
                SizingAxis::Block => {
                    sizing.calculate_inner_block_size(sized.sized_box(), available_space, size, constraints)
                }
            };
        }
        let style = self
            .style_of(sized)
            .with_resolved_insets(inputs.resolved_anchor_insets.as_ref());
        let (start, end) = axis.select(
            (style.inset_left(), style.inset_right()),
            (style.inset_top(), style.inset_bottom()),
        );
        let containing_block_size = axis
            .select(available_space.inline_size, available_space.block_size)
            .to_px_or_zero();
        let percentage_basis = axis.select(
            constraints.percentage_basis_inline_size,
            constraints.percentage_basis_block_size,
        );
        let static_position_rect = static_position_rect_for_sizing(inputs);
        let static_offset = self.static_offset(sized, static_position_rect);
        let (anchor_aligns_inline, anchor_aligns_block) = inputs.containing_block_info.anchor_aligned_axes();
        let rect = static_position_rect.rect;
        // https://drafts.csswg.org/css-position-3/#resolving-inset-auto
        // If both inset properties in a given axis are auto, then, depending on the box’s self-alignment property in
        // the relevant axis:
        // - for self-start alignment or its equivalent: Set its start-edge inset property to the static position, and
        //   its end-edge inset property to zero.
        // - for self-end alignment or its equivalent: Set its end-edge inset property to the static position, and its
        //   start-edge inset property to zero.
        // NB: The static position rect's alignment is that of the static position it stands for. A position area and
        //     anchor-center resolve auto insets to zero instead.
        // FIXME: Implement the center alignment case.
        let static_position_insets = (!axis.select(anchor_aligns_inline, anchor_aligns_block)).then(|| {
            match axis.select(
                static_position_rect.inline_alignment,
                static_position_rect.block_alignment,
            ) {
                AxisEdge::End => (
                    CssPixels::default(),
                    containing_block_size
                        - axis.select(rect.offset.inline_offset, rect.offset.block_offset)
                        - axis.select(rect.size.inline_size, rect.size.block_size),
                ),
                _ => (
                    axis.select(static_offset.inline_offset, static_offset.block_offset),
                    CssPixels::default(),
                ),
            }
        });
        let (start_offset, end_offset) =
            inset_modified_containing_block_insets(start, end, containing_block_size, static_position_insets);
        let stretch_fit_size = sizing.stretch_fit_inner_size(
            sized.sized_box(),
            axis,
            containing_block_size - start_offset - end_offset,
            constraints,
        );
        sizing.resolve_calc_size(sized.sized_box(), axis, size, stretch_fit_size, percentage_basis)
    }

    /// The absolute positioning equation of the box along an axis, whose percentage margins resolve against
    /// `margin_basis`.
    fn axis_equation(
        &self,
        sized: OutOfFlowBox<'_>,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        axis: SizingAxis,
        containing_block_size: CssPixels,
        margin_basis: CssPixels,
        auto_margin_overflow: AutoMarginOverflow,
    ) -> AxisEquation {
        let style = self
            .style_of(sized)
            .with_resolved_insets(inputs.resolved_anchor_insets.as_ref());
        let used = sized.cells;
        let (start, end, margin_start, margin_end, borders_and_paddings) = axis.select(
            (
                style.inset_left(),
                style.inset_right(),
                style.margin_left(),
                style.margin_right(),
                style.border_left_width() + used.padding_left() + used.padding_right() + style.border_right_width(),
            ),
            (
                style.inset_top(),
                style.inset_bottom(),
                style.margin_top(),
                style.margin_bottom(),
                style.border_top_width() + used.padding_top() + used.padding_bottom() + style.border_bottom_width(),
            ),
        );
        AxisEquation {
            containing_block_size,
            start: resolve_or_auto(start, containing_block_size),
            end: resolve_or_auto(end, containing_block_size),
            margin_start: resolve_margin_or_auto(margin_start, margin_basis),
            margin_end: resolve_margin_or_auto(margin_end, margin_basis),
            borders_and_paddings,
            auto_margin_overflow,
        }
    }

    /// Whether the box takes its fit-content size along an axis where its insets are both specified, rather than
    /// fill the space between them.
    /// https://drafts.csswg.org/css-position-3/#abspos-auto-size
    fn sizes_to_fit_content(&self, sized: OutOfFlowBox<'_>, equation: AxisEquation, inline_axis: bool) -> bool {
        let style = *self.style_of(sized);
        let (keyword, _) = if inline_axis {
            SelfKeyword::justify_self_of(style, None)
        } else {
            SelfKeyword::align_self_of(style, None)
        };
        !equation.has_both_insets() || !matches!(keyword, SelfKeyword::Normal | SelfKeyword::Stretch)
    }

    // https://drafts.csswg.org/css-position-3/#abspos-auto-size
    // Resizable form controls use non-replaced sizing here, so their intrinsic dimensions don't prevent stretching.
    fn form_control_stretches(&self, sized: OutOfFlowBox<'_>, equation: AxisEquation, axis: SizingAxis) -> bool {
        let style = self.style_of(sized);
        self.facts(sized.node).is_native_form_control_box()
            && axis.select(style.width(), style.height()).is_auto()
            && !self.sizes_to_fit_content(sized, equation, matches!(axis, SizingAxis::Inline))
    }

    fn shrink_to_fit_available_size(
        inputs: &abspos_inputs::AbsposLayoutInputs,
        axis: SizingAxis,
        mut equation: AxisEquation,
    ) -> CssPixels {
        let placement = axis.select(
            inputs.containing_block_info.inline_placement,
            inputs.containing_block_info.block_placement,
        );
        if equation.start.is_none()
            && equation.end.is_none()
            && placement.mode == abspos_inputs::AbsposAxisMode::StaticPosition
        {
            let static_position = static_position_rect_for_sizing(inputs);
            let rect = static_position.rect;
            let offset = axis.select(rect.offset.inline_offset, rect.offset.block_offset);
            // https://drafts.csswg.org/css-position-3/#resolving-inset-auto
            // Resolve the static-position inset before measuring the available space, without depending on the size
            // of the box being sized. The opposite inset and any auto margins contribute zero.
            match axis.select(static_position.inline_alignment, static_position.block_alignment) {
                AxisEdge::Start => equation.start = Some(offset),
                AxisEdge::End => {
                    equation.end = Some(
                        equation.containing_block_size
                            - offset
                            - axis.select(rect.size.inline_size, rect.size.block_size),
                    );
                }
                AxisEdge::Center => {
                    let center = offset + axis.select(rect.size.inline_size, rect.size.block_size) / 2;
                    equation.start = Some((center * 2 - equation.containing_block_size).max(CssPixels::default()));
                    equation.end = Some((equation.containing_block_size - center * 2).max(CssPixels::default()));
                }
            }
        }
        equation.stretch_fit_size()
    }

    fn stretch_block_size_for_intrinsic_inline_sizing(
        &self,
        sized: OutOfFlowBox<'_>,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> Option<CssPixels> {
        let style = self.style_of(sized);
        if !style.height().is_auto() || self.sizing().establishes_orthogonal_flow(sized.node) {
            return None;
        }
        let equation = self.axis_equation(
            sized,
            inputs,
            SizingAxis::Block,
            available_space.block_size.to_px_or_zero(),
            available_space.inline_size.to_px_or_zero(),
            self.auto_margin_overflow(inputs.containing_block, SizingAxis::Block),
        );
        if self.sizes_to_fit_content(sized, equation, false) {
            return None;
        }
        // https://drafts.csswg.org/css-sizing-3/#definite
        // The stretch-fit block size is definite without laying out the contents. Descendant percentages resolve
        // against it during intrinsic inline sizing, with only definite minimum and maximum sizes applied.
        let mut block_size = equation.stretch_fit_size();
        if style.height().calc_size().is_some() {
            block_size = self.sizing().resolve_calc_size(
                sized.sized_box(),
                SizingAxis::Block,
                style.height(),
                block_size,
                constraints.percentage_basis_block_size,
            );
        }
        let inner_size = |size: &ComputedSize| {
            self.calculate_inner_size(sized, SizingAxis::Block, size, available_space, constraints, inputs)
        };
        if !style.max_height().is_none() && !style.max_height().is_intrinsic_sizing_constraint() {
            block_size = block_size.min(inner_size(style.max_height()));
        }
        if !crate::layout::sizing_context::is_plain_auto(style.min_height())
            && !style.min_height().is_intrinsic_sizing_constraint()
        {
            block_size = block_size.max(inner_size(style.min_height()));
        }
        Some(block_size)
    }

    /// The content inline size of a non-replaced box: its specified size, or the size that auto gives it in the space
    /// between its insets and margins, adjusted by calc-size() and its limits.
    fn non_replaced_inline_size(
        &self,
        sized: OutOfFlowBox<'_>,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        equation: AxisEquation,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        let style = self.style_of(sized);
        let sizing = self.sizing();
        let inner_size = |size: &ComputedSize| {
            self.calculate_inner_size(sized, SizingAxis::Inline, size, available_space, constraints, inputs)
        };
        let mut inline_size = if self.facts(node).is_table_wrapper() {
            sizing
                .compute_table_box_inline_size_inside_wrapper(
                    node,
                    available_space,
                    constraints,
                    None,
                    formatting_context::TableWrapperInlineSizeMode::ClampToAvailableInlineSize,
                )
                .0
        } else if !style.width().is_auto() {
            inner_size(style.width())
        } else if !self.sizes_to_fit_content(sized, equation, true) {
            equation.stretch_fit_size()
        } else if sizing.establishes_orthogonal_flow(node) && sized.cells.has_definite_block_size() {
            sizing.calculate_max_content_inline_size_at_definite_block_size(
                sized.sized_box(),
                constraints,
                sized.cells.content_block_size(),
            )
        } else {
            // https://drafts.csswg.org/css-position-3/#abspos-auto-size
            // For the purpose of computing these sizes, any auto margins are treated as zero.
            let available = Self::shrink_to_fit_available_size(inputs, SizingAxis::Inline, equation);
            let block_size =
                self.stretch_block_size_for_intrinsic_inline_sizing(sized, inputs, available_space, constraints);
            let preferred = block_size.map_or_else(
                || sizing.calculate_max_content_inline_size(sized.sized_box(), constraints),
                |block_size| {
                    sizing.calculate_max_content_inline_size_at_definite_block_size(
                        sized.sized_box(),
                        constraints,
                        block_size,
                    )
                },
            );
            if preferred <= available {
                preferred
            } else {
                let preferred_minimum = block_size.map_or_else(
                    || sizing.calculate_min_content_inline_size(sized.sized_box(), constraints),
                    |block_size| {
                        sizing.calculate_min_content_inline_size_at_definite_block_size(
                            sized.sized_box(),
                            constraints,
                            block_size,
                        )
                    },
                );
                preferred_minimum.max(available).min(preferred)
            }
        };
        // NB: A calc-size() with an auto basis resolves against the inline size auto gives the box.
        if style.width().is_auto() && style.width().calc_size().is_some() {
            inline_size = sizing.resolve_calc_size(
                sized.sized_box(),
                SizingAxis::Inline,
                style.width(),
                inline_size,
                constraints.percentage_basis_inline_size,
            );
        }
        sized.cells.set_content_inline_size(inline_size);
        if !sizing.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            inline_size = inline_size.min(inner_size(style.max_width()));
        }
        if let Some(limits) = sizing.transferred_inline_size_limits(sized.sized_box(), available_space, constraints) {
            inline_size = limits.clamp(inline_size);
        }
        if !crate::layout::sizing_context::is_plain_auto(style.min_width()) {
            inline_size = inline_size.max(inner_size(style.min_width()));
        }
        inline_size
    }

    fn set_used_inline_insets(sized: OutOfFlowBox<'_>, left: CssPixels, right: CssPixels) {
        let insets = sized.used_insets;
        insets.set(run_state::UsedInsets {
            left,
            right,
            ..insets.get()
        });
    }

    fn set_used_block_insets(sized: OutOfFlowBox<'_>, top: CssPixels, bottom: CssPixels) {
        let insets = sized.used_insets;
        insets.set(run_state::UsedInsets {
            top,
            bottom,
            ..insets.get()
        });
    }

    fn compute_inline_size(
        &self,
        sized: OutOfFlowBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        auto_margin_overflow: AutoMarginOverflow,
    ) {
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let equation = self.axis_equation(
            sized,
            inputs,
            SizingAxis::Inline,
            containing_block_inline_size,
            containing_block_inline_size,
            auto_margin_overflow,
        );
        let sizing = self.sizing();
        let uses_replaced_size =
            sizing.box_is_sized_as_replaced_element(sized.sized_box(), available_space, constraints)
                && !self.form_control_stretches(sized, equation, SizingAxis::Inline);
        let inline_size = if uses_replaced_size {
            // NB: A stretch inline size fills the inset-modified containing block, which replaced element sizing does
            //     not know about.
            let width = self.style_of(sized).width();
            if width.is_stretch() {
                self.calculate_inner_size(sized, SizingAxis::Inline, width, available_space, constraints, inputs)
            } else {
                sizing.compute_inline_size_for_replaced_element(sized.sized_box(), available_space, constraints)
            }
        } else {
            self.non_replaced_inline_size(sized, inputs, equation, available_space, constraints)
        };
        let used = sized.cells;
        used.set_content_inline_size(inline_size);
        let solution = equation.solve(inline_size, || {
            self.static_offset(sized, static_position_rect_for_sizing(inputs))
                .inline_offset
        });
        Self::set_used_inline_insets(sized, solution.start, solution.end);
        used.update_cells(|cells| {
            cells.margin_left = solution.margin_start;
            cells.margin_right = solution.margin_end;
        });
    }
}

/// The block sizes the run of an absolutely positioned box found for its contents.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct InsideLayoutBlockSizes {
    pub(crate) automatic_content_block_size: CssPixels,
    /// For a box with a preferred aspect ratio, the block size of its laid out content.
    pub(crate) content_block_size_for_aspect_ratio_minimum: Option<CssPixels>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum BlockSizePass {
    BeforeInsideLayout {
        inside_layout_resolves_automatic_block_size: bool,
    },
    AfterInsideLayout {
        inside_layout: Option<InsideLayoutBlockSizes>,
    },
}

impl AbsposEngine<'_> {
    fn automatic_block_size(
        &self,
        sized: OutOfFlowBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        pass: BlockSizePass,
        stretch_fit_block_size: CssPixels,
    ) -> AutoPx {
        let node = sized.node;
        let sizing = self.sizing();
        if sizing.establishes_orthogonal_flow(node) {
            // https://drafts.csswg.org/css-writing-modes-4/#orthogonal-flows
            // In the sizing phase [...] the dimensions of the box and the containing block are mapped to the inline
            // size and block size and calculations are performed accordingly using the writing mode of the box
            // establishing the orthogonal flow.
            // NB: That makes the automatic block size the shrink-to-fit size of the box's own inline axis, within the
            //     inset-modified containing block, as it is in the inline axis of a box in the frame's writing mode.
            if let BlockSizePass::AfterInsideLayout { .. } = pass {
                return Some(sized.cells.content_block_size());
            }
            let inline_size = sized.cells.content_inline_size();
            let preferred = sizing.calculate_max_content_block_size(sized.sized_box(), inline_size, constraints);
            let size = if preferred <= stretch_fit_block_size {
                preferred
            } else {
                sizing
                    .calculate_min_content_block_size(sized.sized_box(), inline_size, constraints)
                    .max(stretch_fit_block_size)
                    .min(preferred)
            };
            return Some(size);
        }
        if let BlockSizePass::AfterInsideLayout {
            inside_layout: Some(inside_layout),
        } = pass
        {
            return Some(inside_layout.automatic_content_block_size);
        }
        if let BlockSizePass::BeforeInsideLayout {
            inside_layout_resolves_automatic_block_size: true,
        } = pass
        {
            return None;
        }
        if self.facts(node).creates_block_formatting_context() {
            return match pass {
                BlockSizePass::BeforeInsideLayout { .. } => None,
                BlockSizePass::AfterInsideLayout { .. } => Some(CssPixels::default()),
            };
        }
        let inner = sized.cells.available_inner_space_or_constraints_from(available_space);
        Some(sizing.calculate_fit_content_size(sized.sized_box(), SizingAxis::Block, inner, constraints))
    }

    /// The content block size of a non-replaced box: its specified size, or the size that auto gives it in the space
    /// between its insets and margins, adjusted by calc-size() and its limits. Where the box's own layout has yet to
    /// find its automatic block size, the error holds its positive minimum block size, if any.
    fn non_replaced_block_size(
        &self,
        sized: OutOfFlowBox<'_>,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        equation: AxisEquation,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        pass: BlockSizePass,
    ) -> Result<CssPixels, AutoPx> {
        let style = self.style_of(sized);
        let sizing = self.sizing();
        let mut intrinsic_available_space = available_space;
        intrinsic_available_space.inline_size = AvailableSize::definite(sized.cells.content_inline_size());
        let inner_size = |size: &ComputedSize| {
            self.calculate_inner_size(
                sized,
                SizingAxis::Block,
                size,
                intrinsic_available_space,
                constraints,
                inputs,
            )
        };
        let block_size = if self.facts(sized.node).is_table_wrapper() {
            Some(sizing.compute_table_box_block_size_inside_wrapper(sized.sized_box(), available_space, constraints))
        } else if !sizing.should_treat_block_size_as_auto(sized.sized_box(), available_space, constraints) {
            Some(inner_size(style.height()))
        } else if !self.sizes_to_fit_content(sized, equation, false) {
            Some(equation.stretch_fit_size())
        } else {
            self.automatic_block_size(
                sized,
                available_space,
                constraints,
                pass,
                Self::shrink_to_fit_available_size(inputs, SizingAxis::Block, equation),
            )
        };
        let minimum =
            (!crate::layout::sizing_context::is_plain_auto(style.min_height())).then(|| inner_size(style.min_height()));
        let Some(mut block_size) = block_size else {
            return Err(minimum.filter(|&minimum| minimum > CssPixels::default()));
        };
        // NB: As in the inline axis, a calc-size() with an auto basis resolves against the block size auto gives the
        //     box.
        if style.height().is_auto() && style.height().calc_size().is_some() {
            block_size = sizing.resolve_calc_size(
                sized.sized_box(),
                SizingAxis::Block,
                style.height(),
                block_size,
                constraints.percentage_basis_block_size,
            );
        }
        if !style.max_height().is_none() {
            block_size = block_size.min(inner_size(style.max_height()));
        }
        Ok(minimum.map_or(block_size, |minimum| block_size.max(minimum)))
    }

    /// The content block size of a replaced box.
    fn replaced_block_size(
        &self,
        sized: OutOfFlowBox<'_>,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        pass: BlockSizePass,
    ) -> CssPixels {
        let sizing = self.sizing();
        // NB: As for the inline axis, a stretch block size fills the inset-modified containing block.
        let height = self.style_of(sized).height();
        let mut block_size = if height.is_stretch() {
            self.calculate_inner_size(sized, SizingAxis::Block, height, available_space, constraints, inputs)
        } else {
            sizing.compute_block_size_for_replaced_element(sized.sized_box(), available_space, constraints)
        };
        if let BlockSizePass::AfterInsideLayout { inside_layout } = pass
            && sizing.block_size_is_ratio_dependent(sized.sized_box(), available_space, constraints)
        {
            // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
            // NB: A box that is only sized like a replaced element through its aspect ratio grows to fit its content.
            // INTEROP: CSS Positioned Layout says "The automatic minimum size of an absolutely-positioned box is always
            //          zero", but Chrome applies the aspect-ratio minimum to absolutely positioned boxes too.
            let content_block_size =
                inside_layout.and_then(|inside_layout| inside_layout.content_block_size_for_aspect_ratio_minimum);
            if let Some(minimum) = sizing.automatic_minimum_size_from_aspect_ratio(
                sized.sized_box(),
                SizingAxis::Block,
                sized.cells.content_inline_size(),
                block_size,
                available_space,
                constraints,
                content_block_size,
            ) {
                block_size = block_size.max(minimum);
            }
        }
        block_size
    }

    fn compute_block_size(
        &self,
        sized: OutOfFlowBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        pass: BlockSizePass,
        auto_margin_overflow: AutoMarginOverflow,
    ) {
        let sizing = self.sizing();
        let is_replaced = sizing.box_is_sized_as_replaced_element(sized.sized_box(), available_space, constraints);
        let containing_block_block_size = available_space.block_size.to_px_or_zero();
        // NB: The margins of a non-replaced box resolve against the inline size of its containing block, and those of a
        //     replaced box against its block size.
        let margin_basis = if is_replaced && !self.facts(sized.node).is_native_form_control_box() {
            containing_block_block_size
        } else {
            available_space.inline_size.to_px_or_zero()
        };
        let equation = self.axis_equation(
            sized,
            inputs,
            SizingAxis::Block,
            containing_block_block_size,
            margin_basis,
            auto_margin_overflow,
        );
        let is_replaced = is_replaced && !self.form_control_stretches(sized, equation, SizingAxis::Block);
        let resolved_block_size = if is_replaced {
            Ok(self.replaced_block_size(sized, inputs, available_space, constraints, pass))
        } else {
            self.non_replaced_block_size(sized, inputs, equation, available_space, constraints, pass)
        };
        let block_size = resolved_block_size.map_or_else(|minimum| minimum, Some);
        let used = sized.cells;
        used.set_content_block_size(auto_px_value(block_size));
        let height = self.style_of(sized).height();
        if height.is_auto() && matches!(pass, BlockSizePass::BeforeInsideLayout { .. }) {
            // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
            // A block size transferred through the preferred aspect ratio from a definite inline size is definite, so
            // the content resolves percentages against it.
            // NB: Stretch sizing and orthogonal-flow sizing also resolve a non-replaced box's height before inside
            //     layout. A provisional minimum on a content-sized box doesn't provide a percentage basis.
            let is_definite = if is_replaced {
                !sizing.should_treat_block_size_as_auto(sized.sized_box(), available_space, constraints)
            } else {
                resolved_block_size.is_ok()
            };
            if is_definite {
                used.update_cells(|cells| cells.has_definite_block_size = true);
            }
            return;
        }
        if !height.is_intrinsic_sizing_constraint() {
            used.update_cells(|cells| cells.has_definite_block_size = true);
        }
        let solution = match resolved_block_size {
            Ok(block_size) => equation.solve(block_size, || {
                self.static_offset(sized, static_position_rect_for_sizing(inputs))
                    .block_offset
            }),
            Err(_) => equation.unresolved(),
        };
        Self::set_used_block_insets(sized, solution.start, solution.end);
        used.update_cells(|cells| {
            cells.margin_top = solution.margin_start;
            cells.margin_bottom = solution.margin_end;
        });
    }
}

impl<'pass> AbsposEngine<'pass> {
    // Run-prelude sizing for an absolutely positioned root: box-model
    // metrics, the inset-aware inline solve, the pre-inside-layout block
    // pass, and the definiteness overrides insets and aspect ratios provide.
    // Answers the space a flex root resolves its own automatic block size in,
    // if it does.
    pub(crate) fn dimension_out_of_flow_root(
        &self,
        sized: OutOfFlowBox<'_>,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        inside_layout_follows: bool,
    ) -> Option<AvailableSpace> {
        let node = sized.node;
        let (available_space, constraints) = out_of_flow_root_space(inputs);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let resolved = inputs.resolved_anchor_insets.as_ref();
        debug_assert!(
            resolved.is_some()
                || !(self.style_of(sized).inset_top().contains_anchor_function()
                    || self.style_of(sized).inset_right().contains_anchor_function()
                    || self.style_of(sized).inset_bottom().contains_anchor_function()
                    || self.style_of(sized).inset_left().contains_anchor_function()),
            "an out-of-flow root with anchor() insets must arrive with their resolutions"
        );
        let style = self.style_of(sized).with_resolved_insets(resolved);
        sized.cells.update_cells(|cells| {
            cells.border_left = style.border_left_width();
            cells.border_right = style.border_right_width();
            cells.border_top = style.border_top_width();
            cells.border_bottom = style.border_bottom_width();
            cells.padding_left = style.padding_left().to_px(containing_block_inline_size);
            cells.padding_right = style.padding_right().to_px(containing_block_inline_size);
            cells.padding_top = style.padding_top().to_px(containing_block_inline_size);
            cells.padding_bottom = style.padding_bottom().to_px(containing_block_inline_size);
        });

        self.compute_inline_size(
            sized,
            available_space,
            constraints,
            inputs,
            self.auto_margin_overflow(inputs.containing_block, SizingAxis::Inline),
        );
        let fc_type = formatting_context::formatting_context_type_created_by_box(self.facts(node));
        let flex_self_resolution_space = AvailableSpace {
            inline_size: AvailableSize::definite(sized.cells.content_inline_size()),
            block_size: AvailableSize::Indefinite,
        };
        let inside_layout_resolves_automatic_block_size = inside_layout_follows
            && self.inside_layout_resolves_automatic_block_size(
                sized,
                style,
                fc_type,
                available_space,
                flex_self_resolution_space,
                constraints,
            );
        self.compute_block_size(
            sized,
            available_space,
            constraints,
            inputs,
            BlockSizePass::BeforeInsideLayout {
                inside_layout_resolves_automatic_block_size,
            },
            self.auto_margin_overflow(inputs.containing_block, SizingAxis::Block),
        );

        if self.sizing().establishes_orthogonal_flow(node)
            && style.width().is_auto()
            && !self
                .sizing()
                .box_is_sized_as_replaced_element(sized.sized_box(), available_space, constraints)
        {
            // The frame's block size is the orthogonal box's inline size. Its content-based block size depends on
            // wrapping at that resolved size.
            self.compute_inline_size(
                sized,
                available_space,
                constraints,
                inputs,
                self.auto_margin_overflow(inputs.containing_block, SizingAxis::Inline),
            );
        }

        {
            let used = sized.cells;
            if !style.inset_left().is_auto() && !style.inset_right().is_auto() {
                used.update_cells(|cells| cells.has_definite_inline_size = true);
            }
        }
        if !self.facts(node).creates_block_formatting_context() {
            let block_size_resolved_from_aspect_ratio = style.height().is_auto()
                && self.facts(node).has_preferred_aspect_ratio()
                && sized.cells.has_definite_inline_size();
            let used = sized.cells;
            used.update_cells(|cells| cells.has_definite_inline_size = true);
            if (!style.height().is_auto() && !style.height().is_intrinsic_sizing_constraint())
                || block_size_resolved_from_aspect_ratio
            {
                used.update_cells(|cells| cells.has_definite_block_size = true);
            }
        }

        self.sizing()
            .make_form_control_content_box_definite(sized.sized_cells(), available_space, constraints, None);
        (inside_layout_resolves_automatic_block_size
            && fc_type == Some(formatting_context::FormattingContextType::Flex))
        .then_some(flex_self_resolution_space)
    }

    fn inside_layout_resolves_automatic_block_size(
        &self,
        sized: OutOfFlowBox<'_>,
        style: StyleValues<'_>,
        fc_type: Option<formatting_context::FormattingContextType>,
        available_space: AvailableSpace,
        flex_self_resolution_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let node = sized.node;
        let sizing = self.sizing();
        let facts = self.facts(node);
        if !crate::layout::sizing_context::is_plain_auto(style.height())
            || (!style.inset_top().is_auto() && !style.inset_bottom().is_auto())
            || sizing.establishes_orthogonal_flow(node)
            || facts.has_preferred_aspect_ratio()
            || facts.node_has_size_containment()
            || sizing.box_is_sized_as_replaced_element(sized.sized_box(), available_space, constraints)
        {
            return false;
        }
        let style_seen_by_inside_layout = self.style(node);
        let inside_layout_reads_the_same_min_and_max_height = style_seen_by_inside_layout.min_height()
            == style.min_height()
            && style_seen_by_inside_layout.max_height() == style.max_height();
        if !inside_layout_reads_the_same_min_and_max_height {
            return false;
        }
        match fc_type {
            Some(formatting_context::FormattingContextType::Grid) => {
                crate::layout::sizing_context::is_plain_auto(style_seen_by_inside_layout.min_height())
                    || style_seen_by_inside_layout.max_height().is_none()
            }
            Some(formatting_context::FormattingContextType::Flex) => {
                let main_axis_is_block_axis = matches!(
                    style_seen_by_inside_layout.effective_flex_direction(),
                    flex_direction::COLUMN | flex_direction::COLUMN_REVERSE
                );
                let lays_out_its_lines_against_its_block_size =
                    main_axis_is_block_axis && style_seen_by_inside_layout.flex_wrap() != flex_wrap::NOWRAP;
                !lays_out_its_lines_against_its_block_size
                    && matches!(
                        style_seen_by_inside_layout.max_height().kind,
                        ComputedSizeKind::None | ComputedSizeKind::Length | ComputedSizeKind::Percentage
                    )
                    && formatting_context::flex_self_block_size_resolution_space(
                        &sizing,
                        sized.sized_box(),
                        flex_self_resolution_space,
                        constraints,
                    )
                    .is_some()
            }
            _ => false,
        }
    }

    pub(crate) fn finalize_out_of_flow_root_after_inside_layout(
        &self,
        sized: OutOfFlowBox<'_>,
        inputs: &abspos_inputs::AbsposLayoutInputs,
        inside_layout: Option<InsideLayoutBlockSizes>,
    ) {
        let (available_space, constraints) = out_of_flow_root_space(inputs);
        let containing_block_size = geometry::LogicalSize {
            inline_size: available_space.inline_size.to_px_or_zero(),
            block_size: available_space.block_size.to_px_or_zero(),
        };
        let resolved = inputs.resolved_anchor_insets.as_ref();
        let style = self.style_of(sized).with_resolved_insets(resolved);
        if style.height().is_auto() {
            self.compute_block_size(
                sized,
                available_space,
                constraints,
                inputs,
                BlockSizePass::AfterInsideLayout { inside_layout },
                self.auto_margin_overflow(inputs.containing_block, SizingAxis::Block),
            );
        }

        {
            let used = sized.cells;
            let collapsed = used.uses_collapsing_borders_model();
            // https://drafts.csswg.org/css-position-3/#abspos-alignment
            // Otherwise, the box is aligned as specified by its self-alignment property in the relevant axis (as
            // defined by the writing mode of the containing block), using its margin box as the alignment subject and
            // the inset-modified containing block as the alignment container. However, if an explicit overflow
            // alignment is not specified and its margin box overflows the inset-modified containing block, its
            // alignment is adjusted to minimize overflow as specified in [[css-align-3#auto-safety-position]].
            let align_within_insets = |alignment: SelfAlignment,
                                       start_inset: CssPixels,
                                       end_inset: CssPixels,
                                       containing_block_size: CssPixels,
                                       margin_box_size: CssPixels| {
                let available = containing_block_size - start_inset - end_inset - margin_box_size;
                let offset = start_inset + edge_of_own_alignment(alignment).offset(available);
                if available >= CssPixels::default() || alignment.safety == OverflowSafety::Unsafe {
                    return offset;
                }
                let start_is_low_side = alignment.container_start == AxisEdge::Start;
                // https://drafts.csswg.org/css-align-3/#overflow-values
                // safe: If the size of the alignment subject overflows the alignment container, the alignment subject
                // is instead aligned as if the alignment mode were start.
                if alignment.safety == OverflowSafety::Safe {
                    return if start_is_low_side {
                        start_inset
                    } else {
                        start_inset + available
                    };
                }
                // NB: Where the box overflows its inset-modified containing block, it stays within the union of that
                //     and its containing block if it fits, and aligns with the start of the union otherwise.
                let region_start = start_inset.min(CssPixels::default());
                let region_end = (containing_block_size - end_inset).max(containing_block_size);
                if margin_box_size > region_end - region_start {
                    return if start_is_low_side {
                        region_start
                    } else {
                        region_end - margin_box_size
                    };
                }
                offset.max(region_start).min(region_end - margin_box_size)
            };
            // NB: A grid area, a position area or anchor-center aligns the box within its inset-modified containing block
            //     where its insets in the axis are both auto or both specified, and its own self-alignment does where
            //     they are both specified.
            // https://drafts.csswg.org/css-anchor-position-1/#position-area-alignment
            let aligned_start_inset = |placement: abspos_inputs::AxisPlacement,
                                       start: style_values::InsetValue<'_>,
                                       end: style_values::InsetValue<'_>,
                                       containing_block_size: CssPixels,
                                       margin_box_size: CssPixels,
                                       inline_axis: bool| {
                if start.is_auto() != end.is_auto() {
                    return None;
                }
                let (start_inset, end_inset) =
                    inset_modified_containing_block_insets(start, end, containing_block_size, None);
                let available = containing_block_size - start_inset - end_inset - margin_box_size;
                match placement.alignment {
                    None if start.is_auto() => None,
                    None => Some(align_within_insets(
                        self.own_self_alignment(sized, inputs.containing_block, inline_axis)?,
                        start_inset,
                        end_inset,
                        containing_block_size,
                        margin_box_size,
                    )),
                    // NB: By default, an anchor-centered box shifts to stay within its inset-modified containing
                    //     block.
                    Some(AbsposAxisAlignment::AnchorCenter {
                        anchor_start,
                        anchor_size,
                    }) => Some(
                        start_inset
                            + shift_into_containing_block(
                                anchor_start + (anchor_size - margin_box_size) / 2 - start_inset,
                                available,
                            ),
                    ),
                    Some(AbsposAxisAlignment::Edge(edge)) => Some(start_inset + edge.offset(available)),
                    Some(AbsposAxisAlignment::Stretch) => None,
                }
            };
            let info = &inputs.containing_block_info;
            let (inline_size, block_size) = (containing_block_size.inline_size, containing_block_size.block_size);
            let margin_box_inline_size = used.margin_box_inline_size(collapsed);
            if let Some(inset) = aligned_start_inset(
                info.inline_placement,
                style.inset_left(),
                style.inset_right(),
                inline_size,
                margin_box_inline_size,
                true,
            ) {
                Self::set_used_inline_insets(sized, inset, inline_size - inset - margin_box_inline_size);
            }
            let margin_box_block_size = used.margin_box_block_size(collapsed);
            if let Some(inset) = aligned_start_inset(
                info.block_placement,
                style.inset_top(),
                style.inset_bottom(),
                block_size,
                margin_box_block_size,
                false,
            ) {
                Self::set_used_block_insets(sized, inset, block_size - inset - margin_box_block_size);
            }
        }
    }

    /// The keyword and safety for ordinary self-alignment in an axis of the containing block's frame. Anchor-center falls
    /// back to center when no anchor placement is supplied. Stretch sizing is handled separately from self-alignment.
    /// https://drafts.csswg.org/css-align-3/#justify-abspos
    /// https://drafts.csswg.org/css-align-3/#align-abspos
    fn own_self_alignment_keyword(
        &self,
        sized: OutOfFlowBox<'_>,
        inline_axis: bool,
    ) -> Option<(SelfKeyword, OverflowSafety)> {
        let style = *self.style_of(sized);
        let (keyword, safety) = if inline_axis {
            SelfKeyword::justify_self_of(style, None)
        } else {
            SelfKeyword::align_self_of(style, None)
        };
        (!matches!(keyword, SelfKeyword::Normal | SelfKeyword::Stretch)).then_some((keyword, safety))
    }

    /// The box's own self-alignment in an axis of its containing block's frame, where it takes its fit-content size in
    /// its inset-modified containing block.
    fn own_self_alignment(
        &self,
        sized: OutOfFlowBox<'_>,
        containing_block: Node,
        inline_axis: bool,
    ) -> Option<SelfAlignment> {
        let (keyword, safety) = self.own_self_alignment_keyword(sized, inline_axis)?;
        let style = self.style_of(sized);
        let container_start_is_low =
            !inline_axis || containing_block.is_invalid() || !self.style(containing_block).inline_axis_is_reverse();
        Some(keyword.resolve(
            safety,
            AxisDirections {
                self_start_is_low: geometry::start_is_low_side_in_frame(
                    self.pass.frame,
                    style.writing_mode(),
                    style.direction(),
                    inline_axis,
                ),
                // NB: Along the inline axis of a frame, line-left is its left side.
                ..AxisDirections::of(container_start_is_low, Some(true))
            },
        ))
    }

    /// Lays out and places `node`, kept with the cells `cells` for its run, with the inputs `inputs`, where its insets
    /// compensate for the scroll shift `default_scroll_shift`, and returns the block end of its border box in its
    /// containing block.
    fn layout_element(
        &self,
        run: &FormattingContextRun<'pass>,
        node: Node,
        cells: used_values::RootInputCells,
        inputs: abspos_inputs::AbsposLayoutInputs,
        default_scroll_shift: Option<abspos_inputs::DefaultScrollShift>,
    ) -> CssPixels {
        assert!(!self.facts(node).is_svg_box());
        let (available_space, constraints) = out_of_flow_root_space(&inputs);
        let inputs = std::sync::Arc::new(inputs);

        let laid_out = formatting_context::layout_inside_child_with_input_cells(
            run,
            None,
            None,
            node,
            LayoutInput::new(
                available_space,
                constraints,
                ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs.clone()),
            ),
            cells,
            None,
            false,
            // https://drafts.csswg.org/css-sizing-3/#definite
            // "Additionally, the size of the containing block of an absolutely positioned element is always definite
            //  with respect to that element."
            true,
        );
        laid_out.report_percentage_block_size_dependency_of(run, node);
        let mut used_insets = laid_out
            .out_of_flow_root_used_insets()
            .expect("the layout of an absolutely positioned box gives it used insets");

        let metrics = laid_out.cells();
        let collapsed = metrics.uses_collapsing_borders_model();
        let static_offset = aligned_static_offset(
            inputs.static_position_rect,
            metrics.margin_box_inline_size(collapsed),
            metrics.margin_box_block_size(collapsed),
        );
        let mut used_offset = geometry::LogicalOffset {
            inline_offset: if inputs.containing_block_info.inline_placement.mode
                == abspos_inputs::AbsposAxisMode::StaticPosition
            {
                static_offset.inline_offset
            } else {
                inputs.containing_block_info.rect.offset.inline_offset + used_insets.left
            },
            block_offset: if inputs.containing_block_info.block_placement.mode
                == abspos_inputs::AbsposAxisMode::StaticPosition
            {
                static_offset.block_offset
            } else {
                inputs.containing_block_info.rect.offset.block_offset + used_insets.top
            },
        };
        used_offset.inline_offset += metrics.margin_left() + metrics.border_box_left(collapsed);
        used_offset.block_offset += metrics.margin_top() + metrics.border_box_top(collapsed);
        // https://drafts.csswg.org/css-anchor-position-1/#position-area
        // The used value of any auto inset properties and auto margin properties resolves to 0.
        // https://drafts.csswg.org/css-anchor-position-1/#anchor-center
        // The used value of any auto inset properties and auto margin properties resolves to 0.
        // NB: The box is aligned within its inset-modified containing block through its insets above. Now that it is
        //     placed, its used insets go back to the values of its inset properties.
        let (aligns_inline, aligns_block) = inputs.containing_block_info.anchor_aligned_axes();
        if aligns_inline || aligns_block {
            let style = self
                .style(node)
                .with_resolved_insets(inputs.resolved_anchor_insets.as_ref());
            let containing_block_size = inputs.containing_block_info.rect.size;
            if aligns_inline {
                used_insets.left = style.inset_left().to_px(containing_block_size.inline_size);
                used_insets.right = style.inset_right().to_px(containing_block_size.inline_size);
            }
            if aligns_block {
                used_insets.top = style.inset_top().to_px(containing_block_size.block_size);
                used_insets.bottom = style.inset_bottom().to_px(containing_block_size.block_size);
            }
        }
        let mut placed_child = laid_out.placed_child();
        placed_child.facts_decided_by_parent.used_insets = used_insets;
        placed_child.facts_decided_by_parent.default_scroll_shift = default_scroll_shift;
        placed_child.abspos_layout_inputs = Some(inputs);
        formatting_context::place_child(
            run,
            node,
            placed_child,
            FfiCssPixelPoint {
                x: used_offset.inline_offset,
                y: used_offset.block_offset,
            },
            None,
            laid_out.into_fragment_arrival(run),
        );
        used_offset.block_offset + metrics.content_block_size() + metrics.border_box_bottom(collapsed)
    }

    /// Lays out and places `child`, and returns the block end of its border box in its containing block.
    pub(crate) fn layout_pending_child(
        &self,
        run: &FormattingContextRun<'pass>,
        mut child: abspos_inputs::PendingAbsposChild,
    ) -> CssPixels {
        let child_box = child.child_box;
        let position_option = self.determine_position_fallback_styles(&child);
        self.with_position_option_style(child_box, position_option.style, |engine| {
            let run = run.with_pass(engine.pass);
            let cells =
                used_values::cells_before_sizing(&engine.pass, child_box, ContainingBlockConstraints::default());
            let (mut inputs, default_scroll_shift) = engine.pending_child_layout_inputs(&mut child);
            inputs.position_option = position_option.choice;
            engine.layout_element(&run, child_box, cells, inputs, Some(default_scroll_shift))
        })
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#determine-position-fallback-styles
    /// Chooses the position option the pending child `child` is laid out with, whose styles live until the answer is
    /// dropped.
    fn determine_position_fallback_styles(&self, child: &abspos_inputs::PendingAbsposChild) -> PositionOptionStyles {
        let node = child.child_box;
        let mut styles = PositionOptionStyles {
            choice: None,
            style: None,
            option_styles: Vec::new(),
        };
        let style = StyleValues::for_node(&self.pass, node);
        let anchor = style.anchor();
        let arena = self.pass.arena();
        if anchor.position_try_fallbacks.length == 0 || !style.is_absolutely_positioned() {
            return styles;
        }
        let Some(style_node) = arena.node_style_node(node) else {
            return styles;
        };
        let base_style_record = arena.node_style_record(node);
        // Logical directions are resolved against the writing mode of the containing block.
        let containing_block = child.containing_block();
        let (writing_mode, direction) = if containing_block.is_invalid() {
            (writing_mode::HORIZONTAL_TB, direction::LTR)
        } else {
            let containing_block_style = StyleValues::for_node(&self.pass, containing_block);
            (
                containing_block_style.writing_mode(),
                containing_block_style.direction(),
            )
        };

        // Implementations may choose to impose an implementation-defined limit on the length of position options
        // lists, to limit the amount of excess layout work that may be required. This limit must be at least five.
        const POSITION_OPTIONS_LIST_LIMIT: usize = 32;
        // NB: Each option is an entry of the position options list, the base styles first.
        let entries = arena.with_style_store(|engine| {
            engine.position_option_entries(style_node, base_style_record, POSITION_OPTIONS_LIST_LIMIT - 1)
        });
        if entries.is_empty() {
            return styles;
        }
        let entry_of = |option: u32| {
            option
                .checked_sub(1)
                .and_then(|index| entries.get(index as usize))
                .copied()
        };
        let mut options: Vec<u32> = (0..=entries.len() as u32).collect();

        // OPTIMIZATION: The styles of an option are made the first time it is tried, so that a box that fits with
        //               its current styles makes no other.
        let made_styles: std::cell::RefCell<Vec<(u32, Option<Option<node_data::LayoutStyleRef>>)>> =
            std::cell::RefCell::new(Vec::new());
        let option_styles = std::cell::RefCell::new(Vec::new());
        // The styles the option applies: none where the entry makes no option, and no styles of its own for the base
        // styles or an option that changes none of them.
        let option_style = |option: u32| -> Option<Option<node_data::LayoutStyleRef>> {
            let Some((index, _)) = entry_of(option) else {
                return Some(None);
            };
            let made = made_styles
                .borrow()
                .iter()
                .find(|&&(made_option, _)| made_option == option)
                .map(|&(_, style)| style);
            if let Some(style) = made {
                return style;
            }
            let style = arena
                .with_style_store(|engine| {
                    engine.position_option_style(style_node, base_style_record, index, writing_mode, direction)
                })
                .map(|style| match style {
                    PositionOptionStyle::SameAsBase => None,
                    PositionOptionStyle::Applied(style) => {
                        let payloads = style.payloads_for_style_reads();
                        option_styles.borrow_mut().push(style);
                        Some(payloads)
                    }
                });
            made_styles.borrow_mut().push((option, style));
            style
        };

        // https://drafts.csswg.org/css-anchor-position-1/#position-try-order-property
        // For each entry in the position options list, apply that position option to the box, and find the
        // inset-modified containing block size that results from those styles (treating auto inset values as zero).
        // Stably sort the position options list according to this size, with the largest coming first.
        if anchor.has_position_try_order {
            let try_order = anchor.position_try_order;
            let sized: Vec<(u32, CssPixels)> = options
                .iter()
                .filter_map(|&option| {
                    let style = option_style(option)?;
                    let size = self.with_position_option_style(node, style, |engine| {
                        let mut child = *child;
                        let (inputs, _) = engine.pending_child_layout_inputs(&mut child);
                        let size = engine.inset_modified_containing_block_size(node, &inputs);
                        let horizontal_is_frame_inline = !engine.pass.frame.is_transposed();
                        let inline_axis_is_horizontal = writing_mode == writing_mode::HORIZONTAL_TB;
                        let (horizontal, vertical) = if horizontal_is_frame_inline {
                            (size.inline_size, size.block_size)
                        } else {
                            (size.block_size, size.inline_size)
                        };
                        match try_order {
                            crate::css::css_enums::try_order::MOST_WIDTH => horizontal,
                            crate::css::css_enums::try_order::MOST_HEIGHT => vertical,
                            crate::css::css_enums::try_order::MOST_INLINE_SIZE if inline_axis_is_horizontal => {
                                horizontal
                            }
                            crate::css::css_enums::try_order::MOST_INLINE_SIZE => vertical,
                            _ if inline_axis_is_horizontal => vertical,
                            _ => horizontal,
                        }
                    });
                    Some((option, size))
                })
                .collect();
            let mut sorted = sized;
            sorted.sort_by_key(|a| core::cmp::Reverse(a.1));
            options = sorted.into_iter().map(|(option, _)| option).collect();
        }

        // 1. Let current styles be the current used styles of abspos, as derived from the computed base style (which
        //    might be the result of earlier fallback).
        // NB: A box whose @position-try rule of its last successful position option changed has no last successful
        //     position option.
        let last_successful_option = arena
            .last_successful_position_option(node)
            .filter(|choice| {
                entry_of(choice.option).map_or(crate::css::style::position_try::PositionTryRuleID::NONE, |(_, rule)| {
                    rule
                }) == choice.rule
            })
            .map_or(0, |choice| choice.option);
        let current = options
            .iter()
            .position(|&option| option == last_successful_option && option_style(option).is_some())
            .or_else(|| options.iter().position(|&option| option == 0))
            .expect("the base styles are a position option");
        let choice_of = |option: u32| abspos_inputs::PositionOptionChoice {
            option,
            rule: entry_of(option).map_or(crate::css::style::position_try::PositionTryRuleID::NONE, |(_, rule)| {
                rule
            }),
        };
        let fits = |index: usize| {
            let option = options[index];
            let Some(style) = option_style(option) else {
                return false;
            };
            self.with_position_option_style(node, style, |engine| {
                engine.position_option_fits(child, choice_of(option))
            })
        };
        // NB: The box determines its position fallback styles only where it overflows with its current styles.
        // INTEROP: Other browsers take the first option of a list that position-try-order sorted that fits, even
        //          where the base styles fit too.
        let chosen = if anchor.has_position_try_order {
            (0..options.len()).find(|&index| fits(index)).unwrap_or(current)
        } else if fits(current) {
            current
        } else {
            // 2. For each option in the position options list:
            //    1. If option is currently abspos's last successful position option, continue.
            // NB: position_option_fits() runs the other steps for each option.
            // 3. Assert: The previous step finished without finding a position option that avoids overflow.
            // 4. Return current styles.
            (0..options.len())
                .filter(|&index| index != current)
                .find(|&index| fits(index))
                .unwrap_or(current)
        };
        let option = options[chosen];
        styles.choice = Some(choice_of(option));
        styles.style = option_style(option).flatten();
        styles.option_styles = option_styles.into_inner();
        styles
    }

    /// Runs `callback` with this engine as it lays out `node` with `style`, which its style reads see, or with the
    /// style of its row for none.
    fn with_position_option_style<T>(
        &self,
        node: Node,
        style: Option<node_data::LayoutStyleRef>,
        callback: impl for<'option> FnOnce(&AbsposEngine<'option>) -> T,
    ) -> T {
        let Some(style) = style else {
            return callback(self);
        };
        self.pass.with_position_option_style(node, style, |pass| {
            callback(&AbsposEngine {
                state: self.state,
                pass,
                fragments: self.fragments,
            })
        })
    }

    /// The size of the inset-modified containing block that `inputs` give the box `node`, treating auto inset values
    /// as zero.
    fn inset_modified_containing_block_size(
        &self,
        node: Node,
        inputs: &abspos_inputs::AbsposLayoutInputs,
    ) -> geometry::LogicalSize {
        let style = self
            .style(node)
            .with_resolved_insets(inputs.resolved_anchor_insets.as_ref());
        let size = inputs.containing_block_info.rect.size;
        let inset_modified_size = |start, end, size| {
            let (start_inset, end_inset) = inset_modified_containing_block_insets(start, end, size, None);
            size - start_inset - end_inset
        };
        geometry::LogicalSize {
            inline_size: inset_modified_size(style.inset_left(), style.inset_right(), size.inline_size),
            block_size: inset_modified_size(style.inset_top(), style.inset_bottom(), size.block_size),
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#determine-position-fallback-styles
    /// Whether the margin box of the pending child `child`, laid out with the styles its style reads see, is fully
    /// contained within its inset-modified containing block.
    fn position_option_fits(
        &self,
        child: &abspos_inputs::PendingAbsposChild,
        choice: abspos_inputs::PositionOptionChoice,
    ) -> bool {
        let node = child.child_box;
        let mut child = *child;
        let (mut inputs, _) = self.pending_child_layout_inputs(&mut child);
        inputs.position_option = Some(choice);
        let inputs = std::sync::Arc::new(inputs);
        // 3. Let el rect be the size and position of abspos's margin box, and cb rect be the size and position of
        //    abspos's inset-modified containing block, when laid out with adjusted styles.
        let (available_space, constraints) = out_of_flow_root_space(&inputs);
        let laid_out = formatting_context::run_root_without_parent_fragments(
            self.pass,
            node,
            used_values::cells_before_sizing(&self.pass, node, ContainingBlockConstraints::default()),
            LayoutInput::new(
                available_space,
                constraints,
                ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs.clone()),
            ),
        );
        let used = laid_out.root_cells_left_by_run;
        let used_insets = laid_out
            .result
            .out_of_flow_root_used_insets
            .expect("the run of an absolutely positioned box gives it used insets");
        let style = self
            .style(node)
            .with_resolved_insets(inputs.resolved_anchor_insets.as_ref());
        let collapsed = used.uses_collapsing_borders_model();
        let margin_box_size = geometry::LogicalSize {
            inline_size: used.margin_box_inline_size(collapsed),
            block_size: used.margin_box_block_size(collapsed),
        };
        let static_offset = aligned_static_offset(
            inputs.static_position_rect,
            margin_box_size.inline_size,
            margin_box_size.block_size,
        );
        let containing_block = inputs.containing_block_info.rect;
        let axis_is_contained = |start: style_values::InsetValue<'_>,
                                 end: style_values::InsetValue<'_>,
                                 containing_block_size: CssPixels,
                                 margin_box_start: CssPixels,
                                 margin_box_size: CssPixels| {
            let (start_inset, end_inset) =
                inset_modified_containing_block_insets(start, end, containing_block_size, None);
            let inset_modified_start = start_inset;
            let inset_modified_end = containing_block_size - end_inset;
            // 4. If cb rect was negative-size in either axis and corrected into zero-size, continue.
            if inset_modified_end < inset_modified_start {
                return false;
            }
            // 5. If el rect is not fully contained within cb rect, continue.
            margin_box_start >= inset_modified_start && margin_box_start + margin_box_size <= inset_modified_end
        };
        let margin_box_start = |mode: abspos_inputs::AbsposAxisMode,
                                static_offset: CssPixels,
                                containing_block_offset: CssPixels,
                                start_inset: CssPixels| match mode {
            abspos_inputs::AbsposAxisMode::StaticPosition => static_offset - containing_block_offset,
            abspos_inputs::AbsposAxisMode::InsetFromRect => start_inset,
        };
        axis_is_contained(
            style.inset_left(),
            style.inset_right(),
            containing_block.size.inline_size,
            margin_box_start(
                inputs.containing_block_info.inline_placement.mode,
                static_offset.inline_offset,
                containing_block.offset.inline_offset,
                used_insets.left,
            ),
            margin_box_size.inline_size,
        ) && axis_is_contained(
            style.inset_top(),
            style.inset_bottom(),
            containing_block.size.block_size,
            margin_box_start(
                inputs.containing_block_info.block_placement.mode,
                static_offset.block_offset,
                containing_block.offset.block_offset,
                used_insets.top,
            ),
            margin_box_size.block_size,
        )
    }

    /// The layout inputs of the pending child `child`, as its style resolves them against its containing block and
    /// its anchors.
    fn pending_child_layout_inputs(
        &self,
        child: &mut abspos_inputs::PendingAbsposChild,
    ) -> (abspos_inputs::AbsposLayoutInputs, abspos_inputs::DefaultScrollShift) {
        let child_box = child.child_box;
        let containing_block_geometry = self.containing_block_geometry_for_pending_child(child);
        let inline_containing_block_rect =
            self.inline_containing_block_rect_for_pending_child(child, &containing_block_geometry);
        let translation_into_containing_block_space = formatting_context::point_sub(
            FfiCssPixelPoint::default(),
            containing_block_geometry.content_origin_in_entry_space,
        );
        child.static_position_rect = formatting_context::translate_static_position_rect(
            child.static_position_rect,
            translation_into_containing_block_space,
        );
        let containing_block_info_override = child.containing_block_info_override.or_else(|| {
            self.fragments
                .and_then(|fragments| fragments.find_abspos_containing_block_info(child_box))
        });
        let unresolved_containing_block_info = containing_block_info_override.unwrap_or_else(|| {
            self.base_containing_block_info(
                child_box,
                inline_containing_block_rect,
                &containing_block_geometry,
                None,
            )
        });
        let position_area_inputs = self.position_area_layout_inputs(
            child_box,
            child.containing_block(),
            &containing_block_geometry,
            child.coordinate_space_box,
            unresolved_containing_block_info,
        );
        let (containing_block_info, resolved, resolved_anchor_sizes, default_scroll_shift) = if let Some((
            position_area_containing_block_info,
            position_area_geometry,
            anchor_box,
            (stretches_inline, stretches_block),
        )) =
            position_area_inputs
        {
            let mut resolved_anchor_sizes = self.resolve_anchor_sizes(
                child_box,
                child.containing_block(),
                &position_area_geometry,
                child.coordinate_space_box,
            );
            self.zero_aligned_auto_margins(
                child_box,
                &mut resolved_anchor_sizes,
                &position_area_containing_block_info,
            );
            let (resolved, default_scroll_shift) = self.resolve_anchor_insets(
                child_box,
                child.containing_block(),
                &position_area_geometry,
                child.coordinate_space_box,
                Some(anchor_box),
            );
            let resolved = self.position_area_stretch_insets(child_box, resolved, stretches_inline, stretches_block);
            (
                position_area_containing_block_info,
                resolved,
                resolved_anchor_sizes,
                default_scroll_shift,
            )
        } else {
            let mut resolved_anchor_sizes = self.resolve_anchor_sizes(
                child_box,
                child.containing_block(),
                &containing_block_geometry,
                child.coordinate_space_box,
            );
            let (resolved, default_scroll_shift) = self.resolve_anchor_insets(
                child_box,
                child.containing_block(),
                &containing_block_geometry,
                child.coordinate_space_box,
                None,
            );
            let mut containing_block_info = containing_block_info_override.unwrap_or_else(|| {
                self.base_containing_block_info(
                    child_box,
                    inline_containing_block_rect,
                    &containing_block_geometry,
                    resolved.as_ref(),
                )
            });
            self.apply_anchor_center_alignment(
                child_box,
                &containing_block_geometry,
                child.coordinate_space_box,
                resolved.as_ref(),
                &mut containing_block_info,
            );
            self.zero_aligned_auto_margins(child_box, &mut resolved_anchor_sizes, &containing_block_info);
            (
                containing_block_info,
                resolved,
                resolved_anchor_sizes,
                default_scroll_shift,
            )
        };
        let inputs = abspos_inputs::AbsposLayoutInputs {
            containing_block: child.containing_block(),
            inline_containing_block: child.inline_containing_block(),
            static_position_rect: child.static_position_rect,
            containing_block_info,
            resolved_anchor_insets: resolved,
            resolved_anchor_sizes,
            position_option: None,
            frame: self.pass.frame,
        }
        .without_static_position_in_axes_placed_by_own_insets();
        (inputs, default_scroll_shift)
    }

    /// Joins the entry against the inline containing block rect its IFC
    /// contributed, folding the payload's resting space into the containing
    /// block's content-box space (no padding term, unlike anchor rects).
    fn inline_containing_block_rect_for_pending_child(
        &self,
        child: &abspos_inputs::PendingAbsposChild,
        containing_block_geometry: &ContainingBlockGeometry,
    ) -> Option<CssPixelRect> {
        if child.inline_containing_block().is_invalid() {
            return None;
        }
        let fragments = self.fragments?;
        let (rect, payload_space) = fragments.find_inline_containing_block_rect(child.inline_containing_block())?;
        let fold_into_entry_space =
            self.translation_between_payload_resting_spaces(payload_space, child.coordinate_space_box);
        Some(CssPixelRect {
            x: rect.x + fold_into_entry_space.x - containing_block_geometry.content_origin_in_entry_space.x,
            y: rect.y + fold_into_entry_space.y - containing_block_geometry.content_origin_in_entry_space.y,
            width: rect.width,
            height: rect.height,
        })
    }

    pub(super) fn replay(&self, run: &FormattingContextRun<'pass>, node: Node) {
        let mut inputs = self
            .pass
            .saved_abspos_layout_inputs(node)
            .expect("abspos relayout root must have committed layout inputs");
        if !inputs.containing_block_info.derives_from_own_computed_values() {
            let style = self
                .style(node)
                .with_resolved_insets(inputs.resolved_anchor_insets.as_ref());
            inputs.containing_block_info =
                abspos_inputs::AbsposContainingBlockInfo::of_own_insets(inputs.containing_block_info.rect, style);
            inputs = inputs.without_static_position_in_axes_placed_by_own_insets();
        }
        // Partial relayout uses a fresh state and keeps the replay root
        // exactly once.
        let cells = used_values::cells_before_sizing(&self.pass, node, ContainingBlockConstraints::default());
        let default_scroll_shift = self
            .pass
            .committed_fragment_link(node)
            .and_then(|link| link.placement.default_scroll_shift);
        self.layout_element(run, node, cells, inputs, default_scroll_shift);
    }

    fn compute_inset(
        &self,
        node: Node,
        containing_block: &impl BoxMetricCells,
        percentage_basis: geometry::LogicalSize,
        percentage_block_insets_have_definite_basis: bool,
    ) -> InFlowBoxInsets {
        // Most boxes are neither relatively positioned nor carry anchor()
        // insets, and both facts come straight from the node's style payload.
        let initial_style = self.style(node);
        let has_anchor_insets = initial_style.inset_top().contains_anchor_function()
            || initial_style.inset_right().contains_anchor_function()
            || initial_style.inset_bottom().contains_anchor_function()
            || initial_style.inset_left().contains_anchor_function();
        if !has_anchor_insets && initial_style.position() != positioning::RELATIVE {
            return InFlowBoxInsets::default();
        }
        let mut insets = InFlowBoxInsets::default();
        // Anchor resolution also refreshes the box's default scroll shift, so
        // an anchor-bearing box resolves its insets even when it turns out not
        // to be relatively positioned.
        let resolved = if has_anchor_insets {
            let in_flow_containing_block = self.pass.in_flow_containing_block(node);
            let containing_block_geometry =
                ContainingBlockGeometry::from_box_metrics(containing_block, FfiCssPixelPoint::default());
            let (resolved, default_scroll_shift) = self.resolve_anchor_insets(
                node,
                in_flow_containing_block,
                &containing_block_geometry,
                NodeSlotId::INVALID,
                None,
            );
            insets.default_scroll_shift = Some(default_scroll_shift);
            resolved
        } else {
            None
        };
        let style = initial_style.with_resolved_insets(resolved.as_ref());
        if style.position() != positioning::RELATIVE {
            return insets;
        }
        formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
            self.state, &self.pass, node, false,
        );

        let resolve_opposing = |first: style_values::InsetValue, second: style_values::InsetValue, basis: CssPixels| {
            let resolved_first = first.to_px(basis);
            let resolved_second = second.to_px(basis);
            if first.is_auto() && second.is_auto() {
                (CssPixels::default(), CssPixels::default())
            } else if first.is_auto() {
                (-resolved_second, resolved_second)
            } else {
                (resolved_first, -resolved_first)
            }
        };
        let (left, right) = resolve_opposing(style.inset_left(), style.inset_right(), percentage_basis.inline_size);

        let treat_block_axis_percentage_insets_as_auto = (style.inset_top().contains_percentage()
            || style.inset_bottom().contains_percentage())
            && !percentage_block_insets_have_definite_basis;
        fn block_axis_inset_value(
            value: style_values::InsetValue<'_>,
            treat_percentage_as_auto: bool,
        ) -> style_values::InsetValue<'_> {
            if treat_percentage_as_auto && value.contains_percentage() {
                style_values::InsetValue::auto_value()
            } else {
                value
            }
        }
        let (top, bottom) = resolve_opposing(
            block_axis_inset_value(style.inset_top(), treat_block_axis_percentage_insets_as_auto),
            block_axis_inset_value(style.inset_bottom(), treat_block_axis_percentage_insets_as_auto),
            percentage_basis.block_size,
        );
        insets.used_insets = Some(run_state::UsedInsets {
            left,
            right,
            top,
            bottom,
        });
        insets
    }
}

pub(crate) fn drain_abspos_with_placed_containing_blocks(
    state: &RunState,
    pass: LayoutPass<'_>,
    should_collect_devtools_layout_data: bool,
    entry_fragments: &fragment_tree::RunFragmentBuilder,
) {
    let accumulator_root = entry_fragments.root_node();
    let run = FormattingContextRun::new(
        state,
        accumulator_root,
        pass,
        formatting_context::RunMode::Commit {
            devtools: should_collect_devtools_layout_data,
        },
        false,
        None,
    );
    loop {
        let batch = entry_fragments.take_drainable_abspos(accumulator_root, &pass);
        if batch.is_empty() {
            break;
        }
        formatting_context::lay_out_pending_abspos_children(&run, batch);
    }
}

/// What the insets of an in-flow box resolve to: the used insets of a relatively positioned box, and the scroll shift
/// its anchor() insets compensate for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct InFlowBoxInsets {
    pub(crate) used_insets: Option<run_state::UsedInsets>,
    pub(crate) default_scroll_shift: Option<abspos_inputs::DefaultScrollShift>,
}

/// Resolves the insets of `node`, an in-flow box whose containing block has the metrics `containing_block`, with
/// percentages against `percentage_basis`, where block-axis percentages resolve against a definite block size if
/// `percentage_block_insets_have_definite_basis`.
pub(crate) fn compute_inset_native(
    run: &FormattingContextRun<'_>,
    node: Node,
    containing_block: &impl BoxMetricCells,
    percentage_basis: geometry::LogicalSize,
    percentage_block_insets_have_definite_basis: bool,
) -> InFlowBoxInsets {
    AbsposEngine::for_run(run).compute_inset(
        node,
        containing_block,
        percentage_basis,
        percentage_block_insets_have_definite_basis,
    )
}

/// The position option a box is laid out with and the styles it makes, if any, and the styles of the box's position
/// options, which the style reads of the box address while this lives.
struct PositionOptionStyles {
    choice: Option<abspos_inputs::PositionOptionChoice>,
    style: Option<node_data::LayoutStyleRef>,
    /// Boxed so that the payloads the style reads address stay in place as the list grows.
    #[expect(clippy::vec_box)]
    option_styles: Vec<Box<LayoutStyle>>,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn px(value: i64) -> CssPixels {
        CssPixels::from_integer(value)
    }

    fn equation(
        containing_block_size: i64,
        start: Option<i64>,
        end: Option<i64>,
        margin_start: Option<i64>,
        margin_end: Option<i64>,
        auto_margin_overflow: AutoMarginOverflow,
    ) -> AxisEquation {
        AxisEquation {
            containing_block_size: px(containing_block_size),
            start: start.map(px),
            end: end.map(px),
            margin_start: margin_start.map(px),
            margin_end: margin_end.map(px),
            borders_and_paddings: CssPixels::default(),
            auto_margin_overflow,
        }
    }

    #[test]
    fn axis_with_an_auto_inset_zeroes_auto_margins() {
        for available in [-50, 50] {
            for (start, end, margin_start, margin_end, expected) in [
                (None, Some(0), None, None, [available, 0, 0, 0]),
                (Some(0), None, None, None, [0, available, 0, 0]),
                (None, None, None, None, [17, available - 17, 0, 0]),
                (None, Some(0), Some(10), None, [available - 10, 0, 10, 0]),
                (None, Some(0), None, Some(10), [available - 10, 0, 0, 10]),
                (Some(0), None, Some(10), None, [0, available - 10, 10, 0]),
                (Some(0), None, None, Some(10), [0, available - 10, 0, 10]),
            ] {
                // NB: Each policy represents an axis/direction selected by the caller, including orthogonal boxes.
                for overflow in [
                    AutoMarginOverflow::Split,
                    AutoMarginOverflow::LowSide,
                    AutoMarginOverflow::HighSide,
                ] {
                    let actual = equation(available, start, end, margin_start, margin_end, overflow)
                        .solve(CssPixels::default(), || px(17));
                    assert_eq!(
                        actual,
                        AxisSolution {
                            start: px(expected[0]),
                            end: px(expected[1]),
                            margin_start: px(expected[2]),
                            margin_end: px(expected[3]),
                        },
                        "available={available}, start={start:?}, end={end:?}, margins={margin_start:?}/{margin_end:?}",
                    );
                }
            }
        }
    }

    #[test]
    fn axis_with_both_insets_distributes_auto_margins() {
        for (overflow, negative_start, negative_end) in [
            (AutoMarginOverflow::Split, -25, -25),
            (AutoMarginOverflow::LowSide, -50, 0),
            (AutoMarginOverflow::HighSide, 0, -50),
        ] {
            for (available, margin_start, margin_end) in [(-50, negative_start, negative_end), (50, 25, 25)] {
                assert_eq!(
                    equation(available, Some(0), Some(0), None, None, overflow).solve(CssPixels::default(), || px(17)),
                    AxisSolution {
                        start: px(0),
                        end: px(0),
                        margin_start: px(margin_start),
                        margin_end: px(margin_end),
                    },
                );
            }
        }
    }
}
