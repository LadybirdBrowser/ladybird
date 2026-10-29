/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::used_values::CommittedSvgFacts;
use smallvec::SmallVec;

pub(crate) struct Fragment {
    pub(crate) identity: u64,
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) content_inline_size: CssPixels,
    pub(crate) content_block_size: CssPixels,
    pub(crate) margin_left: CssPixels,
    pub(crate) margin_right: CssPixels,
    pub(crate) margin_top: CssPixels,
    pub(crate) margin_bottom: CssPixels,
    pub(crate) border_left: CssPixels,
    pub(crate) border_right: CssPixels,
    pub(crate) border_top: CssPixels,
    pub(crate) border_bottom: CssPixels,
    pub(crate) padding_left: CssPixels,
    pub(crate) padding_right: CssPixels,
    pub(crate) padding_top: CssPixels,
    pub(crate) padding_bottom: CssPixels,
    pub(crate) uses_collapsing_borders_model: bool,
    pub(crate) is_collapsed_borders_table_box: bool,
    pub(crate) table_column_index: u32,
    pub(crate) table_column_span: u32,
    pub(crate) hidden_by_collapsed_columns: bool,
    pub(crate) collapsed_table_borders: Option<std::sync::Arc<table_formatting_context::OwnedCollapsedTableBorders>>,
    pub(crate) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    pub(crate) grid_layout_data: Option<std::sync::Arc<grid_formatting_context::GridLayoutData>>,
    pub(crate) flex_layout_data: Option<std::sync::Arc<formatting_context::FlexLayoutData>>,
    pub(crate) multicol_layout_data: Option<std::sync::Arc<fragmentation::MulticolLayoutData>>,
    pub(crate) math_paint_data: Option<std::sync::Arc<math_formatting_context::MathPaintData>>,
    pub(crate) used_grid_tracks: Option<std::sync::Arc<grid_formatting_context::OwnedUsedGridTracks>>,
    /// Boxed, as only SVG boxes have any and the facts are larger than the rest of the fragment's box.
    pub(crate) svg: Option<Box<CommittedSvgFacts>>,
    pub(crate) computed_svg_path: Option<std::sync::Arc<libgfx_rust::path::Path>>,
    pub(crate) svg_text_character_cells: Option<std::sync::Arc<Vec<svg_formatting_context::FfiSvgTextCharacterCell>>>,
    pub(crate) has_line_clamp_point: bool,
    pub(crate) is_invisible_for_line_clamp: bool,
    pub(crate) clamped_content_is_scrollable_overflow: bool,
    pub(crate) continues_from_previous_fragment: bool,
    pub(crate) continues_in_next_fragment: bool,
    pub(crate) children: Vec<FragmentLink>,
}

#[derive(Clone)]
pub(crate) struct FragmentLink {
    pub(crate) fragment: std::sync::Arc<Fragment>,
    pub(crate) committed_offset: FfiCssPixelPoint,
    pub(crate) inset_left: CssPixels,
    pub(crate) inset_right: CssPixels,
    pub(crate) inset_top: CssPixels,
    pub(crate) inset_bottom: CssPixels,
    pub(crate) containing_line_box_index: Option<usize>,
    /// Shared, as few links place an absolutely positioned box, the inputs are larger than the rest of the link, and
    /// layout run cache entries keep copies of links.
    pub(crate) abspos_layout_inputs: Option<std::sync::Arc<abspos_inputs::AbsposLayoutInputs>>,
    /// The default scroll shift of a box whose anchor insets its layout resolved, which commit records for the box.
    pub(crate) default_scroll_shift: Option<abspos_inputs::DefaultScrollShift>,
    pub(crate) containing_block: crate::layout::node_data::NodeSlotId,
    /// The frame the box was laid out in, in which the link and the fragment hold its geometry.
    pub(crate) frame: geometry::Frame,
}

fn same_allocation<T>(left: Option<&std::sync::Arc<T>>, right: Option<&std::sync::Arc<T>>) -> bool {
    match (left, right) {
        (None, None) => true,
        (Some(left), Some(right)) => std::sync::Arc::ptr_eq(left, right),
        _ => false,
    }
}

/// How two fragments' shared payloads compare: as the same allocations, as painting needs, or as equal values, as
/// two layouts of one box do.
#[derive(Clone, Copy)]
enum PayloadComparison {
    SameAllocation,
    SameValue,
}

impl PayloadComparison {
    fn matches<T: PartialEq>(self, left: Option<&std::sync::Arc<T>>, right: Option<&std::sync::Arc<T>>) -> bool {
        match self {
            Self::SameAllocation => same_allocation(left, right),
            Self::SameValue => left.map(|left| &**left) == right.map(|right| &**right),
        }
    }
}

impl Fragment {
    fn builds_identically_to(&self, previous: &Fragment) -> bool {
        self.has_same_box_properties(previous)
            && same_allocation(self.line_data.as_ref(), previous.line_data.as_ref())
            && self.children.len() == previous.children.len()
            && self
                .children
                .iter()
                .zip(&previous.children)
                .all(|(link, previous_link)| link.places_same_fragment_identically_to(previous_link))
    }

    /// Whether the fragment holds nothing beyond a plain box's metrics, line data and children: no table, grid, flex,
    /// SVG or line clamp state.
    pub(crate) fn is_plain_box(&self) -> bool {
        !self.is_collapsed_borders_table_box
            && self.table_column_index == 0
            && self.table_column_span == 0
            && !self.hidden_by_collapsed_columns
            && self.collapsed_table_borders.is_none()
            && self.grid_layout_data.is_none()
            && self.flex_layout_data.is_none()
            && self.multicol_layout_data.is_none()
            && self.math_paint_data.is_none()
            && self.used_grid_tracks.is_none()
            && self.svg.is_none()
            && self.computed_svg_path.is_none()
            && self.svg_text_character_cells.is_none()
            && !self.has_line_clamp_point
            && !self.is_invisible_for_line_clamp
            && !self.clamped_content_is_scrollable_overflow
            && !self.continues_from_previous_fragment
            && !self.continues_in_next_fragment
    }

    /// Box properties excluding inline content and child fragments. Painting compares those
    /// separately: a new descendant fragment does not necessarily change this box's commands.
    pub(crate) fn has_same_box_properties(&self, previous: &Fragment) -> bool {
        self.has_same_box_properties_comparing_payloads(previous, PayloadComparison::SameAllocation)
    }

    /// Box properties excluding inline content and child fragments, with payloads compared by value.
    pub(crate) fn has_same_box_properties_comparing_payloads_by_value(&self, previous: &Fragment) -> bool {
        self.has_same_box_properties_comparing_payloads(previous, PayloadComparison::SameValue)
    }

    fn has_same_box_properties_comparing_payloads(&self, previous: &Fragment, payloads: PayloadComparison) -> bool {
        self.node == previous.node
            && self.content_inline_size == previous.content_inline_size
            && self.content_block_size == previous.content_block_size
            && self.margin_left == previous.margin_left
            && self.margin_right == previous.margin_right
            && self.margin_top == previous.margin_top
            && self.margin_bottom == previous.margin_bottom
            && self.border_left == previous.border_left
            && self.border_right == previous.border_right
            && self.border_top == previous.border_top
            && self.border_bottom == previous.border_bottom
            && self.padding_left == previous.padding_left
            && self.padding_right == previous.padding_right
            && self.padding_top == previous.padding_top
            && self.padding_bottom == previous.padding_bottom
            && self.uses_collapsing_borders_model == previous.uses_collapsing_borders_model
            && self.is_collapsed_borders_table_box == previous.is_collapsed_borders_table_box
            && self.table_column_index == previous.table_column_index
            && self.table_column_span == previous.table_column_span
            && self.hidden_by_collapsed_columns == previous.hidden_by_collapsed_columns
            && payloads.matches(
                self.collapsed_table_borders.as_ref(),
                previous.collapsed_table_borders.as_ref(),
            )
            && payloads.matches(self.grid_layout_data.as_ref(), previous.grid_layout_data.as_ref())
            && payloads.matches(self.flex_layout_data.as_ref(), previous.flex_layout_data.as_ref())
            && self.multicol_layout_data == previous.multicol_layout_data
            && self.math_paint_data == previous.math_paint_data
            && payloads.matches(self.used_grid_tracks.as_ref(), previous.used_grid_tracks.as_ref())
            && self.svg == previous.svg
            && payloads.matches(self.computed_svg_path.as_ref(), previous.computed_svg_path.as_ref())
            && payloads.matches(
                self.svg_text_character_cells.as_ref(),
                previous.svg_text_character_cells.as_ref(),
            )
            && self.has_line_clamp_point == previous.has_line_clamp_point
            && self.is_invisible_for_line_clamp == previous.is_invisible_for_line_clamp
            && self.clamped_content_is_scrollable_overflow == previous.clamped_content_is_scrollable_overflow
            && self.continues_from_previous_fragment == previous.continues_from_previous_fragment
            && self.continues_in_next_fragment == previous.continues_in_next_fragment
    }

    pub(crate) fn has_same_child_placements(&self, previous: &Fragment) -> bool {
        self.children.len() == previous.children.len()
            && self
                .children
                .iter()
                .zip(&previous.children)
                .all(|(link, previous_link)| link.has_same_placement(previous_link))
    }

    pub(crate) fn has_same_child_sequence(&self, previous: &Fragment) -> bool {
        self.children.len() == previous.children.len()
            && self
                .children
                .iter()
                .zip(&previous.children)
                .all(|(a, b)| a.fragment.node == b.fragment.node)
    }
}

impl FragmentLink {
    /// A link placing a fresh, empty fragment of `node` at the origin.
    #[cfg(test)]
    pub(crate) fn for_test(node: crate::layout::node_data::NodeSlotId) -> Self {
        Self {
            fragment: std::sync::Arc::new(Fragment {
                identity: 1,
                node,
                content_inline_size: CssPixels::default(),
                content_block_size: CssPixels::default(),
                margin_left: CssPixels::default(),
                margin_right: CssPixels::default(),
                margin_top: CssPixels::default(),
                margin_bottom: CssPixels::default(),
                border_left: CssPixels::default(),
                border_right: CssPixels::default(),
                border_top: CssPixels::default(),
                border_bottom: CssPixels::default(),
                padding_left: CssPixels::default(),
                padding_right: CssPixels::default(),
                padding_top: CssPixels::default(),
                padding_bottom: CssPixels::default(),
                uses_collapsing_borders_model: false,
                is_collapsed_borders_table_box: false,
                table_column_index: 0,
                table_column_span: 0,
                hidden_by_collapsed_columns: false,
                collapsed_table_borders: None,
                line_data: None,
                grid_layout_data: None,
                flex_layout_data: None,
                multicol_layout_data: None,
                math_paint_data: None,
                used_grid_tracks: None,
                svg: None,
                computed_svg_path: None,
                svg_text_character_cells: None,
                has_line_clamp_point: false,
                is_invisible_for_line_clamp: false,
                clamped_content_is_scrollable_overflow: false,
                continues_from_previous_fragment: false,
                continues_in_next_fragment: false,
                children: Vec::new(),
            }),
            committed_offset: Default::default(),
            inset_left: CssPixels::default(),
            inset_right: CssPixels::default(),
            inset_top: CssPixels::default(),
            inset_bottom: CssPixels::default(),
            containing_line_box_index: None,
            abspos_layout_inputs: None,
            default_scroll_shift: None,
            containing_block: crate::layout::node_data::NodeSlotId::INVALID,
            frame: geometry::Frame::PHYSICAL,
        }
    }

    pub(crate) fn places_same_fragment_identically_to(&self, previous: &FragmentLink) -> bool {
        std::sync::Arc::ptr_eq(&self.fragment, &previous.fragment) && self.has_same_placement(previous)
    }

    pub(crate) fn has_same_placement(&self, previous: &FragmentLink) -> bool {
        self.fragment.node == previous.fragment.node
            && self.committed_offset == previous.committed_offset
            && self.inset_left == previous.inset_left
            && self.inset_right == previous.inset_right
            && self.inset_top == previous.inset_top
            && self.inset_bottom == previous.inset_bottom
            && self.containing_line_box_index == previous.containing_line_box_index
            && self.abspos_layout_inputs == previous.abspos_layout_inputs
            && self.default_scroll_shift == previous.default_scroll_shift
            && self.containing_block == previous.containing_block
            && self.frame == previous.frame
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AnchorCandidate {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) border_box_rect: CssPixelRect,
    pub(crate) coordinate_space_box: crate::layout::node_data::NodeSlotId,
}

/// The padding-box rect of an inline box that acts as an abspos containing
/// block, spanning its first and last content lines.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct InlineContainingBlockRect {
    pub(crate) inline_box: crate::layout::node_data::NodeSlotId,
    pub(crate) rect: CssPixelRect,
    pub(crate) coordinate_space_box: crate::layout::node_data::NodeSlotId,
}

/// Containing block info a formatting context computed for a pending abspos
/// child it does not itself register (a grid supplying its grid area to deep
/// descendants). The rect is relative to the containing block's own content
/// origin, so the contribution never rebases; it travels as-is to whichever
/// run drains the child and is joined by child-box identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AbsposContainingBlockInfoContribution {
    pub(crate) child_box: crate::layout::node_data::NodeSlotId,
    pub(crate) info: abspos_inputs::AbsposContainingBlockInfo,
}

trait PropagatedPayload {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId;
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId);
    fn translate_by(&mut self, offset: FfiCssPixelPoint);
    /// Maps the payload from frame `from` to frame `to`, within a box whose content box has the physical size
    /// `container_size`.
    fn move_to_frame(&mut self, from: geometry::Frame, to: geometry::Frame, container_size: FfiCssPixelSize);
}

impl PropagatedPayload for abspos_inputs::PendingAbsposChild {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId {
        self.coordinate_space_box
    }
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId) {
        self.coordinate_space_box = node;
    }
    fn translate_by(&mut self, offset: FfiCssPixelPoint) {
        self.static_position_rect =
            formatting_context::translate_static_position_rect(self.static_position_rect, offset);
    }
    fn move_to_frame(&mut self, from: geometry::Frame, to: geometry::Frame, container_size: FfiCssPixelSize) {
        self.static_position_rect = self.static_position_rect.in_frame(from, to, container_size);
    }
}

impl PropagatedPayload for InlineContainingBlockRect {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId {
        self.coordinate_space_box
    }
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId) {
        self.coordinate_space_box = node;
    }
    fn translate_by(&mut self, offset: FfiCssPixelPoint) {
        self.rect.x += offset.x;
        self.rect.y += offset.y;
    }
    fn move_to_frame(&mut self, from: geometry::Frame, to: geometry::Frame, container_size: FfiCssPixelSize) {
        self.rect = from.rect_in(self.rect, to, container_size);
    }
}

impl PropagatedPayload for AnchorCandidate {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId {
        self.coordinate_space_box
    }
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId) {
        self.coordinate_space_box = node;
    }
    fn translate_by(&mut self, offset: FfiCssPixelPoint) {
        self.border_box_rect.x += offset.x;
        self.border_box_rect.y += offset.y;
    }
    fn move_to_frame(&mut self, from: geometry::Frame, to: geometry::Frame, container_size: FfiCssPixelSize) {
        self.border_box_rect = from.rect_in(self.border_box_rect, to, container_size);
    }
}

/// How payloads laid out in the frame a placed box establishes map into the frame of its containing block.
#[derive(Clone, Copy)]
struct PlacedBoxFrames {
    own: geometry::Frame,
    containing_block: geometry::Frame,
    physical_content_size: FfiCssPixelSize,
}

fn propagate_payload_into_containing_block_space<Payload: PropagatedPayload>(
    payload: &mut Payload,
    placed_box: crate::layout::node_data::NodeSlotId,
    containing_block: Option<crate::layout::node_data::NodeSlotId>,
    placed_box_content_offset: FfiCssPixelPoint,
    frames: Option<PlacedBoxFrames>,
) {
    if payload.coordinate_space_box() == placed_box
        && let Some(containing_block) = containing_block
    {
        if let Some(frames) = frames {
            payload.move_to_frame(frames.own, frames.containing_block, frames.physical_content_size);
        }
        payload.translate_by(placed_box_content_offset);
        payload.set_coordinate_space_box(containing_block);
    }
}

fn propagate_payload_toward_run_root_space<Payload: PropagatedPayload>(
    payload: &mut Payload,
    run_root: crate::layout::node_data::NodeSlotId,
    placed_boxes: &PlacedBoxes,
    callbacks: &LayoutPass<'_>,
) {
    while payload.coordinate_space_box() != run_root {
        let Some(placement) = placed_boxes.placement_of(payload.coordinate_space_box()) else {
            break;
        };
        payload.translate_by(placement.content_offset_in_containing_block);
        let containing_block =
            placed_boxes.placement_containing_block(callbacks, payload.coordinate_space_box(), run_root);
        if containing_block.is_invalid() {
            break;
        }
        payload.set_coordinate_space_box(containing_block);
    }
}

/// The sizes and box sides of a box, in the frame `frame`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct CellsInFrame {
    pub(crate) cells: used_values::RootInputCells,
    pub(crate) frame: geometry::Frame,
}

/// Where a run placed each box it placed so far, and the sizes and box sides of each placed box that may be the
/// containing block of absolutely positioned boxes, as the run placed it. Both are keyed by slot.
#[derive(Default)]
pub(crate) struct PlacedBoxes {
    placements: HashMap<u32, run_state::BoxPlacement>,
    cells_of_abspos_containing_blocks: HashMap<u32, CellsInFrame>,
}

impl PlacedBoxes {
    pub(crate) fn placement_of(&self, node: crate::layout::node_data::NodeSlotId) -> Option<run_state::BoxPlacement> {
        self.placements.get(&node.slot_index()).copied()
    }

    pub(crate) fn is_placed(&self, node: crate::layout::node_data::NodeSlotId) -> bool {
        self.placements.contains_key(&node.slot_index())
    }

    /// The containing block `node` is placed in, or for a box not placed in one yet, the containing block it is laid
    /// out in, within the run rooted at `run_root`.
    pub(crate) fn placement_containing_block(
        &self,
        callbacks: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
        run_root: crate::layout::node_data::NodeSlotId,
    ) -> crate::layout::node_data::NodeSlotId {
        match self.placement_of(node) {
            Some(placement) if !placement.containing_block.is_invalid() => placement.containing_block,
            _ => formatting_context::containing_block_laid_out_in(callbacks, node, run_root),
        }
    }
}

fn previously_committed_fragment_matching(
    callbacks: &LayoutPass<'_>,
    candidate: &Fragment,
) -> Option<std::sync::Arc<Fragment>> {
    if !callbacks.has_committed_fragment_link(candidate.node) {
        return None;
    }
    callbacks
        .arena()
        .with_committed_fragment_link(candidate.node, |previous_link| {
            previous_link
                .filter(|previous_link| candidate.builds_identically_to(&previous_link.fragment))
                .map(|previous_link| previous_link.fragment.clone())
        })
}

/// What the layout of a box hands its fragment for painting and the devtools besides its geometry.
#[derive(Clone, Default, PartialEq)]
pub(crate) struct FragmentPayloads {
    pub(crate) collapsed_table_borders: Option<std::sync::Arc<table_formatting_context::OwnedCollapsedTableBorders>>,
    pub(crate) grid_layout_data: Option<std::sync::Arc<grid_formatting_context::GridLayoutData>>,
    pub(crate) flex_layout_data: Option<std::sync::Arc<formatting_context::FlexLayoutData>>,
    pub(crate) multicol_layout_data: Option<std::sync::Arc<fragmentation::MulticolLayoutData>>,
    pub(crate) math_paint_data: Option<std::sync::Arc<math_formatting_context::MathPaintData>>,
    pub(crate) used_grid_tracks: Option<std::sync::Arc<grid_formatting_context::OwnedUsedGridTracks>>,
    pub(crate) svg: CommittedSvgFacts,
    pub(crate) computed_svg_path: Option<std::sync::Arc<libgfx_rust::path::Path>>,
    pub(crate) svg_text_character_cells: Option<std::sync::Arc<Vec<svg_formatting_context::FfiSvgTextCharacterCell>>>,
    /// For a line-clamp container: whether its contents hold the clamp point, and whether what follows the clamp
    /// point still counts as scrollable overflow.
    pub(crate) has_line_clamp_point: bool,
    pub(crate) clamped_content_is_scrollable_overflow: bool,
}

impl FragmentPayloads {
    fn of_fragment(fragment: &Fragment) -> Self {
        Self {
            collapsed_table_borders: fragment.collapsed_table_borders.clone(),
            grid_layout_data: fragment.grid_layout_data.clone(),
            flex_layout_data: fragment.flex_layout_data.clone(),
            multicol_layout_data: fragment.multicol_layout_data.clone(),
            math_paint_data: fragment.math_paint_data.clone(),
            used_grid_tracks: fragment.used_grid_tracks.clone(),
            svg: fragment.svg.as_deref().copied().unwrap_or_default(),
            computed_svg_path: fragment.computed_svg_path.clone(),
            svg_text_character_cells: fragment.svg_text_character_cells.clone(),
            has_line_clamp_point: fragment.has_line_clamp_point,
            clamped_content_is_scrollable_overflow: fragment.clamped_content_is_scrollable_overflow,
        }
    }
}

/// What the fragment of a placed box shows of the box's own layout: its sizes and box sides in the frame of the run that
/// places it, and its lines.
#[derive(Clone)]
pub(crate) struct FragmentGeometry {
    pub(crate) cells: used_values::RootInputCells,
    pub(crate) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
}

impl FragmentGeometry {
    pub(crate) fn shows_box_of_fragment(&self, fragment: &Fragment) -> bool {
        let cells = &self.cells;
        same_allocation(self.line_data.as_ref(), fragment.line_data.as_ref())
            && fragment.content_inline_size == cells.content_inline_size
            && fragment.content_block_size == cells.content_block_size
            && fragment.margin_left == cells.margin_left
            && fragment.margin_right == cells.margin_right
            && fragment.margin_top == cells.margin_top
            && fragment.margin_bottom == cells.margin_bottom
            && fragment.border_left == cells.border_left
            && fragment.border_right == cells.border_right
            && fragment.border_top == cells.border_top
            && fragment.border_bottom == cells.border_bottom
            && fragment.padding_left == cells.padding_left
            && fragment.padding_right == cells.padding_right
            && fragment.padding_top == cells.padding_top
            && fragment.padding_bottom == cells.padding_bottom
            && fragment.uses_collapsing_borders_model == cells.uses_collapsing_borders_model
    }
}

/// The committed fragment `fragment` of `node`, or a fragment of the box's `geometry` around the committed children
/// where the box is now hidden or shown for line clamping unlike the committed fragment.
pub(crate) fn committed_fragment_showing_box(
    callbacks: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    geometry: impl FnOnce() -> FragmentGeometry,
    fragment_facts: run_state::FragmentFactsDecidedByParent,
    fragment: std::sync::Arc<Fragment>,
) -> std::sync::Arc<Fragment> {
    if fragment_facts.is_invisible_for_line_clamp == fragment.is_invisible_for_line_clamp {
        return fragment;
    }
    snapshot_fragment(
        callbacks,
        node,
        fragment.children.clone(),
        geometry(),
        fragment_facts,
        Some(Box::new(FragmentPayloads::of_fragment(&fragment))),
    )
}

fn snapshot_fragment(
    callbacks: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
    geometry: FragmentGeometry,
    fragment_facts: run_state::FragmentFactsDecidedByParent,
    payloads: Option<Box<FragmentPayloads>>,
) -> std::sync::Arc<Fragment> {
    static NEXT_IDENTITY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let FragmentGeometry { cells, line_data } = geometry;
    let FragmentPayloads {
        collapsed_table_borders,
        grid_layout_data,
        flex_layout_data,
        multicol_layout_data,
        math_paint_data,
        used_grid_tracks,
        svg,
        computed_svg_path,
        svg_text_character_cells,
        has_line_clamp_point,
        clamped_content_is_scrollable_overflow,
    } = payloads.map(|payloads| *payloads).unwrap_or_default();
    let mut fragment = Fragment {
        identity: 0,
        node,
        content_inline_size: cells.content_inline_size,
        content_block_size: cells.content_block_size,
        margin_left: cells.margin_left,
        margin_right: cells.margin_right,
        margin_top: cells.margin_top,
        margin_bottom: cells.margin_bottom,
        border_left: cells.border_left,
        border_right: cells.border_right,
        border_top: cells.border_top,
        border_bottom: cells.border_bottom,
        padding_left: cells.padding_left,
        padding_right: cells.padding_right,
        padding_top: cells.padding_top,
        padding_bottom: cells.padding_bottom,
        uses_collapsing_borders_model: cells.uses_collapsing_borders_model,
        is_collapsed_borders_table_box: cells.is_collapsed_borders_table_box,
        table_column_index: fragment_facts.table_column_index,
        table_column_span: fragment_facts.table_column_span,
        hidden_by_collapsed_columns: fragment_facts.hidden_by_collapsed_columns,
        collapsed_table_borders,
        line_data,
        grid_layout_data,
        flex_layout_data,
        multicol_layout_data,
        math_paint_data,
        used_grid_tracks,
        svg: (svg != CommittedSvgFacts::default()).then(|| Box::new(svg)),
        computed_svg_path,
        svg_text_character_cells,
        has_line_clamp_point,
        is_invisible_for_line_clamp: fragment_facts.is_invisible_for_line_clamp,
        clamped_content_is_scrollable_overflow,
        continues_from_previous_fragment: fragment_facts.continues_from_previous_fragment,
        continues_in_next_fragment: fragment_facts.continues_in_next_fragment,
        children,
    };
    if !fragment.continues_from_previous_fragment
        && !fragment.continues_in_next_fragment
        && let Some(previous) = previously_committed_fragment_matching(callbacks, &fragment)
    {
        return previous;
    }
    fragment.identity = NEXT_IDENTITY.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    std::sync::Arc::new(fragment)
}

/// The fragment a placement built, and whether anything from its subtree went on to its containing block.
pub(crate) struct PlacedFragment {
    pub(crate) fragment: std::sync::Arc<Fragment>,
    pub(crate) exports_payloads: bool,
    /// The absolutely positioned boxes laid out and placed as the box was placed.
    pub(crate) abspos_boxes_placed_with_it: Vec<PlacedAbsposBox>,
}

/// An absolutely positioned box laid out and placed in its containing block.
pub(crate) struct PlacedAbsposBox {
    pub(crate) box_: crate::layout::node_data::NodeSlotId,
    /// The block end of the border box in the containing block.
    pub(crate) border_box_block_end: CssPixels,
}

pub(crate) struct PlacementData {
    pub(crate) committed_offset: FfiCssPixelPoint,
    pub(crate) inset_left: CssPixels,
    pub(crate) inset_right: CssPixels,
    pub(crate) inset_top: CssPixels,
    pub(crate) inset_bottom: CssPixels,
    pub(crate) containing_line_box_index: Option<usize>,
    pub(crate) abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
    pub(crate) default_scroll_shift: Option<abspos_inputs::DefaultScrollShift>,
    pub(crate) containing_block: crate::layout::node_data::NodeSlotId,
    pub(crate) frame: geometry::Frame,
}

impl PlacementData {
    fn of_box(
        facts: &run_state::FactsDecidedByParent,
        abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
        containing_line_box_index: Option<usize>,
        committed_offset: FfiCssPixelPoint,
        frame: geometry::Frame,
    ) -> Self {
        Self {
            frame,
            committed_offset,
            inset_left: facts.used_insets.left,
            inset_right: facts.used_insets.right,
            inset_top: facts.used_insets.top,
            inset_bottom: facts.used_insets.bottom,
            containing_line_box_index,
            abspos_layout_inputs,
            default_scroll_shift: facts.default_scroll_shift,
            containing_block: facts
                .placement
                .map_or(crate::layout::node_data::NodeSlotId::INVALID, |placement| {
                    placement.containing_block
                }),
        }
    }

    /// The placement data of a child the run left unplaced, which `close()` hands on as `unplaced_child` shows it.
    fn of_unplaced_child(
        unplaced_child: &crate::layout::formatting_context::PlacedChild,
        frame: geometry::Frame,
    ) -> Self {
        Self::of_box(
            &unplaced_child.facts_decided_by_parent,
            None,
            None,
            FfiCssPixelPoint::default(),
            frame,
        )
    }
}

fn link_fragment(fragment: std::sync::Arc<Fragment>, placement: PlacementData) -> FragmentLink {
    FragmentLink {
        fragment,
        committed_offset: placement.committed_offset,
        inset_left: placement.inset_left,
        inset_right: placement.inset_right,
        inset_top: placement.inset_top,
        inset_bottom: placement.inset_bottom,
        containing_line_box_index: placement.containing_line_box_index,
        abspos_layout_inputs: placement.abspos_layout_inputs.map(std::sync::Arc::new),
        default_scroll_shift: placement.default_scroll_shift,
        containing_block: placement.containing_block,
        frame: placement.frame,
    }
}

/// A child the run laid out but does not place: what its layout handed its placement, and what placing it would have
/// taken.
struct ChildLeftUnplaced {
    arrival: ChildFragmentArrival,
    unplaced_child: crate::layout::formatting_context::PlacedChild,
}

#[derive(Clone)]
pub(crate) struct UnplacedRootFragment {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) scoped_descendants: Vec<FragmentLink>,
    pub(crate) reused_subtree_roots: HashSet<u32>,
    pub(crate) propagated_pending_abspos: Vec<abspos_inputs::PendingAbsposChild>,
    pub(crate) propagated_anchor_candidates: Vec<AnchorCandidate>,
    pub(crate) propagated_inline_containing_block_rects: Vec<InlineContainingBlockRect>,
    pub(crate) propagated_abspos_containing_block_info: Vec<AbsposContainingBlockInfoContribution>,
    /// What the run's layout of its root hands the root's fragment.
    pub(crate) root_payloads: Option<Box<FragmentPayloads>>,
}

impl UnplacedRootFragment {
    /// Whether the root hands its ancestors no payload.
    pub(crate) fn propagates_nothing(&self) -> bool {
        self.propagated_pending_abspos.is_empty()
            && self.propagated_anchor_candidates.is_empty()
            && self.propagated_inline_containing_block_rects.is_empty()
            && self.propagated_abspos_containing_block_info.is_empty()
    }
}

/// What the layout of a child hands the child's placement: the unplaced root fragment of the child's run, or a
/// committed fragment the child is placed with as it is.
#[must_use = "a child's fragments are lost unless its placement takes them"]
pub(crate) enum ChildFragmentArrival {
    RunRoot {
        root: UnplacedRootFragment,
        reuses_committed_subtree: bool,
    },
    CommittedFragment {
        fragment: std::sync::Arc<Fragment>,
        reuses_subtree: bool,
    },
}

impl ChildFragmentArrival {
    pub(crate) fn node(&self) -> crate::layout::node_data::NodeSlotId {
        match self {
            Self::RunRoot { root, .. } => root.node,
            Self::CommittedFragment { fragment, .. } => fragment.node,
        }
    }
}

#[derive(Clone, Copy)]
pub(crate) struct SlotFragment<'tree> {
    pub(crate) link: &'tree FragmentLink,
    pub(crate) containing_block_fragment: Option<u32>,
    pub(crate) containing_block_line_index_offset: usize,
}

pub(crate) type SlotFragments<'tree> = SmallVec<[SlotFragment<'tree>; 1]>;

pub(crate) struct LinksBySlot<'tree> {
    in_first_containing_block_fragment: HashMap<u32, &'tree FragmentLink>,
    other: HashMap<u32, (SlotFragments<'tree>, usize)>,
}

impl<'tree> LinksBySlot<'tree> {
    pub(crate) fn fragments(&self, slot: u32) -> SlotFragments<'tree> {
        if let Some(link) = self.in_first_containing_block_fragment.get(&slot) {
            return smallvec::smallvec![SlotFragment {
                link,
                containing_block_fragment: Some(0),
                containing_block_line_index_offset: 0,
            }];
        }
        self.other
            .get(&slot)
            .map(|(fragments, _)| fragments.clone())
            .unwrap_or_default()
    }

    fn push(&mut self, slot: u32, fragment: SlotFragment<'tree>) -> (u32, usize) {
        let line_count = |fragment: &SlotFragment<'_>| {
            fragment
                .link
                .fragment
                .line_data
                .as_ref()
                .map_or(0, |line_data| line_data.lines.len())
        };
        if let Some(link) = self.in_first_containing_block_fragment.remove(&slot) {
            let first = SlotFragment {
                link,
                containing_block_fragment: Some(0),
                containing_block_line_index_offset: 0,
            };
            let first_line_count = line_count(&first);
            self.other.insert(slot, (smallvec::smallvec![first], first_line_count));
        }
        let Some((fragments, line_count_before)) = self.other.get_mut(&slot) else {
            if fragment.containing_block_fragment == Some(0) && fragment.containing_block_line_index_offset == 0 {
                self.in_first_containing_block_fragment.insert(slot, fragment.link);
            } else {
                let fragment_line_count = line_count(&fragment);
                self.other
                    .insert(slot, (smallvec::smallvec![fragment], fragment_line_count));
            }
            return (0, 0);
        };
        let fragment_index = fragments.len() as u32;
        let line_index_offset = *line_count_before;
        *line_count_before += line_count(&fragment);
        fragments.push(fragment);
        (fragment_index, line_index_offset)
    }
}

pub(crate) struct CompletedPassFragments {
    pub(crate) roots: Vec<FragmentLink>,
    pub(crate) reused_subtree_roots: HashSet<u32>,
}

impl CompletedPassFragments {
    pub(crate) fn links_by_slot(&self) -> LinksBySlot<'_> {
        fn insert_links<'tree>(
            links: &'tree [FragmentLink],
            parent: Option<(crate::layout::node_data::NodeSlotId, u32, usize)>,
            reused_subtree_roots: &HashSet<u32>,
            links_by_slot: &mut LinksBySlot<'tree>,
        ) {
            for link in links {
                let slot = link.fragment.node.slot_index();
                let (containing_block_fragment, containing_block_line_index_offset) = match parent {
                    None => (None, 0),
                    Some((parent_node, parent_fragment, parent_line_index_offset))
                        if parent_node == link.containing_block =>
                    {
                        (Some(parent_fragment), parent_line_index_offset)
                    }
                    Some(_) => (Some(0), 0),
                };
                let (fragment_index, line_index_offset) = links_by_slot.push(
                    slot,
                    SlotFragment {
                        link,
                        containing_block_fragment,
                        containing_block_line_index_offset,
                    },
                );
                // NB: Commit stops at a reused subtree's root, so nothing looks up the links below it.
                if !reused_subtree_roots.contains(&slot) {
                    insert_links(
                        &link.fragment.children,
                        Some((link.fragment.node, fragment_index, line_index_offset)),
                        reused_subtree_roots,
                        links_by_slot,
                    );
                }
            }
        }
        fn count_links(links: &[FragmentLink], reused_subtree_roots: &HashSet<u32>) -> usize {
            links
                .iter()
                .map(|link| {
                    let slot = link.fragment.node.slot_index();
                    1 + if reused_subtree_roots.contains(&slot) {
                        0
                    } else {
                        count_links(&link.fragment.children, reused_subtree_roots)
                    }
                })
                .sum()
        }
        let mut links_by_slot = LinksBySlot {
            in_first_containing_block_fragment: HashMap::with_capacity_and_hasher(
                count_links(&self.roots, &self.reused_subtree_roots),
                Default::default(),
            ),
            other: HashMap::default(),
        };
        insert_links(&self.roots, None, &self.reused_subtree_roots, &mut links_by_slot);
        links_by_slot
    }

    pub(crate) fn subtree_was_reused(&self, slot: u32) -> bool {
        self.reused_subtree_roots.contains(&slot)
    }
}

struct PendingFragment {
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
    pending_abspos: Vec<abspos_inputs::PendingAbsposChild>,
    anchor_candidates: Vec<AnchorCandidate>,
    inline_containing_block_rects: Vec<InlineContainingBlockRect>,
    payloads: Option<Box<FragmentPayloads>>,
}

impl PendingFragment {
    fn new(node: crate::layout::node_data::NodeSlotId) -> Self {
        Self {
            node,
            children: Vec::new(),
            pending_abspos: Vec::new(),
            anchor_candidates: Vec::new(),
            inline_containing_block_rects: Vec::new(),
            payloads: None,
        }
    }
}

pub(crate) struct RunFragmentBuilder {
    root_node: crate::layout::node_data::NodeSlotId,
    root_containing_block_slot: Option<u32>,
    is_entry_accumulator: bool,
    inner: std::cell::RefCell<RunFragmentBuilderInner>,
}

#[derive(Default)]
struct RunFragmentBuilderInner {
    pending_fragments: HashMap<u32, PendingFragment>,
    pending_abspos_at_root: Vec<abspos_inputs::PendingAbsposChild>,
    anchor_candidates_at_root: Vec<AnchorCandidate>,
    inline_containing_block_rects_at_root: Vec<InlineContainingBlockRect>,
    abspos_containing_block_info_contributions: Vec<AbsposContainingBlockInfoContribution>,
    /// How many abspos containing block info contributions the run has received, whether or not they are still listed.
    abspos_containing_block_info_contributions_received: u64,
    top_scope_links: Vec<FragmentLink>,
    reused_subtree_roots: HashSet<u32>,
    /// What the layouts of children the run laid out but never placed handed their placements, which close() links at
    /// the top of the run's fragments.
    children_left_unplaced: Vec<ChildLeftUnplaced>,
    /// What the run's layout of its root hands the root's fragment.
    root_payloads: Option<Box<FragmentPayloads>>,
    /// Where the run placed each box it placed so far, from the moment it places the box, before the placement drains
    /// the absolutely positioned boxes the box contains.
    placed_boxes: PlacedBoxes,
}

impl RunFragmentBuilderInner {
    fn receive_abspos_containing_block_info(&mut self, contributions: Vec<AbsposContainingBlockInfoContribution>) {
        self.abspos_containing_block_info_contributions_received += contributions.len() as u64;
        self.abspos_containing_block_info_contributions.extend(contributions);
    }
}

impl RunFragmentBuilderInner {
    fn iter_pending_abspos(&self) -> impl Iterator<Item = &abspos_inputs::PendingAbsposChild> {
        self.pending_fragments
            .values()
            .flat_map(|pending_fragment| pending_fragment.pending_abspos.iter())
            .chain(self.pending_abspos_at_root.iter())
    }

    fn iter_anchor_candidates(&self) -> impl Iterator<Item = &AnchorCandidate> {
        self.pending_fragments
            .values()
            .flat_map(|pending_fragment| pending_fragment.anchor_candidates.iter())
            .chain(self.anchor_candidates_at_root.iter())
    }

    fn iter_inline_containing_block_rects(&self) -> impl Iterator<Item = &InlineContainingBlockRect> {
        self.pending_fragments
            .values()
            .flat_map(|pending_fragment| pending_fragment.inline_containing_block_rects.iter())
            .chain(self.inline_containing_block_rects_at_root.iter())
    }
}

impl RunFragmentBuilder {
    pub(crate) fn new(
        root_node: crate::layout::node_data::NodeSlotId,
        root_containing_block: Option<crate::layout::node_data::NodeSlotId>,
    ) -> Self {
        Self {
            root_node,
            root_containing_block_slot: root_containing_block.map(|node| node.slot_index()),
            is_entry_accumulator: false,
            inner: std::cell::RefCell::new(RunFragmentBuilderInner::default()),
        }
    }

    pub(crate) fn new_entry_accumulator(root_node: crate::layout::node_data::NodeSlotId) -> Self {
        Self {
            root_node,
            root_containing_block_slot: None,
            is_entry_accumulator: true,
            inner: std::cell::RefCell::new(RunFragmentBuilderInner::default()),
        }
    }

    pub(crate) fn root_node(&self) -> crate::layout::node_data::NodeSlotId {
        self.root_node
    }

    /// Notes that the run placed `node` in `placement`, which is final, with the sizes and box sides
    /// `cells_for_contained_abspos` of a box that may be the containing block of absolutely positioned boxes.
    pub(crate) fn note_placed_box(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        placement: run_state::BoxPlacement,
        cells_for_contained_abspos: Option<CellsInFrame>,
    ) {
        let placed_boxes = &mut self.inner.borrow_mut().placed_boxes;
        let previous = placed_boxes.placements.insert(node.slot_index(), placement);
        debug_assert!(previous.is_none(), "a box is placed once");
        if let Some(cells) = cells_for_contained_abspos {
            placed_boxes
                .cells_of_abspos_containing_blocks
                .insert(node.slot_index(), cells);
        }
    }

    /// The sizes and box sides of `containing_block`, a box the run placed that may be the containing block of
    /// absolutely positioned boxes, read in frame `reader`.
    pub(crate) fn cells_of_placed_containing_block(
        &self,
        containing_block: crate::layout::node_data::NodeSlotId,
        reader: geometry::Frame,
    ) -> Option<used_values::RootInputCells> {
        let kept = *self
            .inner
            .borrow()
            .placed_boxes
            .cells_of_abspos_containing_blocks
            .get(&containing_block.slot_index())?;
        Some(kept.cells.in_frame(kept.frame, reader))
    }

    /// Where the run placed `node`, if it placed the box.
    pub(crate) fn placement_of(&self, node: crate::layout::node_data::NodeSlotId) -> Option<run_state::BoxPlacement> {
        self.inner.borrow().placed_boxes.placement_of(node)
    }

    /// The containing block `node` is placed in, or for a box the run did not place yet, the containing block it is
    /// laid out in.
    pub(crate) fn placement_containing_block(
        &self,
        callbacks: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
    ) -> crate::layout::node_data::NodeSlotId {
        self.inner
            .borrow()
            .placed_boxes
            .placement_containing_block(callbacks, node, self.root_node)
    }

    pub(crate) fn register_pending_abspos(
        &self,
        coordinate_space_box: crate::layout::node_data::NodeSlotId,
        entry: abspos_inputs::PendingAbsposChild,
    ) {
        let mut inner = self.inner.borrow_mut();
        if coordinate_space_box == self.root_node {
            inner.pending_abspos_at_root.push(entry);
            return;
        }
        match inner.pending_fragments.get_mut(&coordinate_space_box.slot_index()) {
            Some(pending_fragment) => pending_fragment.pending_abspos.push(entry),
            None => {
                if self.is_entry_accumulator {
                    inner.pending_abspos_at_root.push(entry);
                } else {
                    let mut pending_fragment = PendingFragment::new(coordinate_space_box);
                    pending_fragment.pending_abspos.push(entry);
                    inner
                        .pending_fragments
                        .insert(coordinate_space_box.slot_index(), pending_fragment);
                }
            }
        }
    }

    pub(crate) fn register_inline_anchor_candidate(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        border_box_rect: CssPixelRect,
        coordinate_space_box: crate::layout::node_data::NodeSlotId,
    ) {
        let mut inner = self.inner.borrow_mut();
        let candidate = AnchorCandidate {
            node,
            border_box_rect,
            coordinate_space_box,
        };
        if coordinate_space_box == self.root_node {
            inner.anchor_candidates_at_root.push(candidate);
            return;
        }
        match inner.pending_fragments.get_mut(&coordinate_space_box.slot_index()) {
            Some(pending_fragment) => pending_fragment.anchor_candidates.push(candidate),
            None => {
                if self.is_entry_accumulator {
                    inner.anchor_candidates_at_root.push(candidate);
                } else {
                    let mut pending_fragment = PendingFragment::new(coordinate_space_box);
                    pending_fragment.anchor_candidates.push(candidate);
                    inner
                        .pending_fragments
                        .insert(coordinate_space_box.slot_index(), pending_fragment);
                }
            }
        }
    }

    pub(crate) fn register_inline_containing_block_rect(
        &self,
        inline_box: crate::layout::node_data::NodeSlotId,
        rect: CssPixelRect,
        coordinate_space_box: crate::layout::node_data::NodeSlotId,
    ) {
        let mut inner = self.inner.borrow_mut();
        debug_assert!(
            !inner
                .iter_inline_containing_block_rects()
                .any(|payload| payload.inline_box == inline_box),
            "an inline box's containing block rect was registered twice"
        );
        let payload = InlineContainingBlockRect {
            inline_box,
            rect,
            coordinate_space_box,
        };
        if coordinate_space_box == self.root_node {
            inner.inline_containing_block_rects_at_root.push(payload);
            return;
        }
        match inner.pending_fragments.get_mut(&coordinate_space_box.slot_index()) {
            Some(pending_fragment) => pending_fragment.inline_containing_block_rects.push(payload),
            None => {
                if self.is_entry_accumulator {
                    inner.inline_containing_block_rects_at_root.push(payload);
                } else {
                    let mut pending_fragment = PendingFragment::new(coordinate_space_box);
                    pending_fragment.inline_containing_block_rects.push(payload);
                    inner
                        .pending_fragments
                        .insert(coordinate_space_box.slot_index(), pending_fragment);
                }
            }
        }
    }

    pub(crate) fn find_inline_containing_block_rect(
        &self,
        inline_box: crate::layout::node_data::NodeSlotId,
    ) -> Option<(CssPixelRect, crate::layout::node_data::NodeSlotId)> {
        self.inner
            .borrow()
            .iter_inline_containing_block_rects()
            .find(|payload| payload.inline_box == inline_box)
            .map(|payload| (payload.rect, payload.coordinate_space_box))
    }

    /// Lists pending abspos children whose containing block is the given box
    /// and whose registration did not already resolve containing block info.
    /// Only root-resting entries are considered: by the time a containing
    /// block asks (its run's tail), every interior box is placed, so entries
    /// it contains have propagated to the root list.
    pub(crate) fn pending_abspos_children_awaiting_containing_block_info(
        &self,
        containing_block: crate::layout::node_data::NodeSlotId,
    ) -> Vec<crate::layout::node_data::NodeSlotId> {
        let inner = self.inner.borrow();
        let awaits_info = |entry: &abspos_inputs::PendingAbsposChild| {
            entry.containing_block_info_override.is_none() && entry.containing_block() == containing_block
        };
        debug_assert!(
            !inner
                .pending_fragments
                .values()
                .flat_map(|pending_fragment| pending_fragment.pending_abspos.iter())
                .any(awaits_info),
            "a pending abspos child awaiting containing block info rests below the root list"
        );
        inner
            .pending_abspos_at_root
            .iter()
            .filter(|entry| awaits_info(entry))
            .map(|entry| entry.child_box)
            .collect()
    }

    pub(crate) fn register_abspos_containing_block_info(
        &self,
        child_box: crate::layout::node_data::NodeSlotId,
        info: abspos_inputs::AbsposContainingBlockInfo,
    ) {
        let mut inner = self.inner.borrow_mut();
        debug_assert!(
            !inner
                .abspos_containing_block_info_contributions
                .iter()
                .any(|contribution| contribution.child_box == child_box),
            "an abspos child's containing block info was contributed twice"
        );
        inner
            .abspos_containing_block_info_contributions
            .push(AbsposContainingBlockInfoContribution { child_box, info });
        inner.abspos_containing_block_info_contributions_received += 1;
    }

    pub(crate) fn find_abspos_containing_block_info(
        &self,
        child_box: crate::layout::node_data::NodeSlotId,
    ) -> Option<abspos_inputs::AbsposContainingBlockInfo> {
        self.inner
            .borrow()
            .abspos_containing_block_info_contributions
            .iter()
            .find(|contribution| contribution.child_box == child_box)
            .map(|contribution| contribution.info)
    }

    pub(crate) fn any_pending_abspos_has_inline_containing_block(&self) -> bool {
        self.inner
            .borrow()
            .iter_pending_abspos()
            .any(|entry| !entry.inline_containing_block().is_invalid())
    }

    pub(crate) fn any_pending_abspos_names_inline_containing_block(
        &self,
        inline_box: crate::layout::node_data::NodeSlotId,
    ) -> bool {
        self.inner
            .borrow()
            .iter_pending_abspos()
            .any(|entry| entry.inline_containing_block() == inline_box)
    }

    pub(crate) fn find_anchor_candidate(
        &self,
        node: crate::layout::node_data::NodeSlotId,
    ) -> Option<(CssPixelRect, crate::layout::node_data::NodeSlotId)> {
        self.inner
            .borrow()
            .iter_anchor_candidates()
            .find(|candidate| candidate.node == node)
            .map(|candidate| (candidate.border_box_rect, candidate.coordinate_space_box))
    }

    pub(crate) fn take_drainable_abspos(
        &self,
        placed: crate::layout::node_data::NodeSlotId,
        callbacks: &LayoutPass<'_>,
    ) -> Vec<abspos_inputs::PendingAbsposChild> {
        let mut inner = self.inner.borrow_mut();
        let RunFragmentBuilderInner {
            pending_fragments,
            pending_abspos_at_root,
            placed_boxes,
            ..
        } = &mut *inner;
        let containing_block_is_owned_and_placed = |entry: &abspos_inputs::PendingAbsposChild| {
            let containing_block = entry.containing_block();
            !containing_block.is_invalid()
                && (self.is_entry_accumulator || containing_block != self.root_node)
                && placed_boxes.is_placed(containing_block)
        };
        let mut taken: Vec<abspos_inputs::PendingAbsposChild> = pending_abspos_at_root
            .extract_if(.., |entry| containing_block_is_owned_and_placed(entry))
            .collect();
        if let Some(pending_fragment) = pending_fragments.get_mut(&placed.slot_index()) {
            taken.extend(
                pending_fragment
                    .pending_abspos
                    .extract_if(.., |entry| containing_block_is_owned_and_placed(entry)),
            );
        }
        taken.sort_by(|left, right| {
            if left.child_box == right.child_box {
                std::cmp::Ordering::Equal
            } else if callbacks.is_before(left.child_box, right.child_box) {
                std::cmp::Ordering::Less
            } else {
                std::cmp::Ordering::Greater
            }
        });
        taken
    }

    /// Keeps what the layout of a child the run laid out but does not place handed its placement, with what placing the
    /// child would have taken.
    pub(crate) fn leave_child_unplaced(
        &self,
        arrival: ChildFragmentArrival,
        unplaced_child: crate::layout::formatting_context::PlacedChild,
    ) {
        debug_assert!(
            !self.left_child_unplaced(arrival.node()),
            "a child was left unplaced twice"
        );
        self.inner.borrow_mut().children_left_unplaced.push(ChildLeftUnplaced {
            arrival,
            unplaced_child,
        });
    }

    pub(crate) fn left_child_unplaced(&self, node: crate::layout::node_data::NodeSlotId) -> bool {
        self.inner
            .borrow()
            .children_left_unplaced
            .iter()
            .any(|child| child.arrival.node() == node)
    }

    pub(crate) fn adopt_fragmentainer_root(
        &self,
        root: UnplacedRootFragment,
        offset: FfiCssPixelPoint,
        root_line_index_offset: usize,
    ) {
        debug_assert!(
            root.node == self.root_node,
            "a fragmentainer run's root is the box of the run that adopts it"
        );
        let mut inner = self.inner.borrow_mut();
        inner.reused_subtree_roots.extend(root.reused_subtree_roots);
        for mut link in root.scoped_descendants {
            link.committed_offset = formatting_context::point_add(link.committed_offset, offset);
            if link.containing_block == self.root_node
                && let Some(line_index) = link.containing_line_box_index.as_mut()
            {
                *line_index += root_line_index_offset;
            }
            inner.top_scope_links.push(link);
        }
        for mut entry in root.propagated_pending_abspos {
            entry.translate_by(offset);
            inner.pending_abspos_at_root.push(entry);
        }
        // Lookups outside the fragmentation context use the bounding rectangle of all fragments.
        // Combine candidates only after translating them into the multicol container's coordinate space.
        let mut anchor_indices: HashMap<_, _> = inner
            .anchor_candidates_at_root
            .iter()
            .enumerate()
            .map(|(index, candidate)| ((candidate.node, candidate.coordinate_space_box), index))
            .collect();
        for mut candidate in root.propagated_anchor_candidates {
            candidate.translate_by(offset);
            let key = (candidate.node, candidate.coordinate_space_box);
            if let Some(&index) = anchor_indices.get(&key) {
                inner.anchor_candidates_at_root[index]
                    .border_box_rect
                    .unite(candidate.border_box_rect);
            } else {
                anchor_indices.insert(key, inner.anchor_candidates_at_root.len());
                inner.anchor_candidates_at_root.push(candidate);
            }
        }
        for mut payload in root.propagated_inline_containing_block_rects {
            payload.translate_by(offset);
            inner.inline_containing_block_rects_at_root.push(payload);
        }
        inner.receive_abspos_containing_block_info(root.propagated_abspos_containing_block_info);
    }

    /// Takes in the unplaced root of the run that laid out `node`, which is being placed, so that its fragment is built
    /// around the root's descendants.
    pub(crate) fn take_in_child_root_at_placement(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        root: UnplacedRootFragment,
        reuses_committed_subtree: bool,
        callbacks: &LayoutPass<'_>,
    ) {
        {
            let mut inner = self.inner.borrow_mut();
            inner
                .reused_subtree_roots
                .extend(root.reused_subtree_roots.iter().copied());
            if reuses_committed_subtree {
                inner.reused_subtree_roots.insert(node.slot_index());
            } else {
                inner.reused_subtree_roots.remove(&node.slot_index());
            }
        }
        self.merge_child_root_into_pending_fragment(node, root, callbacks);
    }

    /// Opens the fragment of `node`, which is being placed without fragments handed to it, so that the fragments of
    /// its absolutely positioned descendants laid out at its placement go inside it.
    pub(crate) fn open_fragment_of_placed_box(&self, node: crate::layout::node_data::NodeSlotId) {
        self.inner
            .borrow_mut()
            .pending_fragments
            .entry(node.slot_index())
            .or_insert_with(|| PendingFragment::new(node));
    }

    /// Drops the fragments laid out inside `node`. What the box's own layout handed its fragment stays for the box,
    /// which may still be placed.
    pub(crate) fn discard_unplaced_subtree(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        fragment_arrival: Option<ChildFragmentArrival>,
    ) {
        debug_assert!(!self.left_child_unplaced(node), "a child left unplaced is discarded");
        let arrived_payloads = fragment_arrival.and_then(|arrival| match arrival {
            ChildFragmentArrival::RunRoot { root, .. } => root.root_payloads,
            ChildFragmentArrival::CommittedFragment { fragment, .. } => {
                Some(Box::new(FragmentPayloads::of_fragment(&fragment)))
            }
        });
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let mut payloads_of_box = arrived_payloads;
        inner.reused_subtree_roots.remove(&slot);
        if let Some(pending_fragment) = inner.pending_fragments.remove(&slot) {
            payloads_of_box = payloads_of_box.or(pending_fragment.payloads);
        }
        if payloads_of_box.is_some() {
            inner.pending_fragments.insert(
                slot,
                PendingFragment {
                    payloads: payloads_of_box,
                    ..PendingFragment::new(node)
                },
            );
        }
    }

    /// Hands what the layout of `node` produced besides its geometry to the box's fragment.
    pub(crate) fn update_fragment_payloads_of(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        update: impl FnOnce(&mut FragmentPayloads),
    ) {
        let mut inner = self.inner.borrow_mut();
        let payloads = if node == self.root_node && !self.is_entry_accumulator {
            &mut inner.root_payloads
        } else {
            &mut inner
                .pending_fragments
                .entry(node.slot_index())
                .or_insert_with(|| PendingFragment::new(node))
                .payloads
        };
        update(payloads.get_or_insert_with(Default::default));
    }

    fn merge_child_root_into_pending_fragment(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        root: UnplacedRootFragment,
        callbacks: &LayoutPass<'_>,
    ) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let pending_fragment = inner
            .pending_fragments
            .entry(slot)
            .or_insert_with(|| PendingFragment::new(node));
        debug_assert!(root.node == node, "a box took in the root of another box's run");
        debug_assert!(
            pending_fragment.children.is_empty() || root.scoped_descendants.is_empty(),
            "a child run's root and an open pending fragment both carry children for slot {slot}"
        );
        if pending_fragment.children.is_empty() {
            pending_fragment.children = root.scoped_descendants;
        } else {
            pending_fragment.children.extend(root.scoped_descendants);
        }
        pending_fragment
            .pending_abspos
            .extend(root.propagated_pending_abspos.into_iter().map(|mut entry| {
                formatting_context::resolve_pending_abspos_containing_block(callbacks, &mut entry, self.root_node);
                entry
            }));
        pending_fragment
            .anchor_candidates
            .extend(root.propagated_anchor_candidates);
        pending_fragment
            .inline_containing_block_rects
            .extend(root.propagated_inline_containing_block_rects);
        pending_fragment.payloads = root.root_payloads;
        inner.receive_abspos_containing_block_info(root.propagated_abspos_containing_block_info);
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn build_fragment_for_placed_box(
        &self,
        callbacks: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
        geometry: FragmentGeometry,
        facts: run_state::FactsDecidedByParent,
        abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
        containing_line_box_index: Option<usize>,
        committed_offset: FfiCssPixelPoint,
        own_anchor_candidate_border_box_rect: Option<CssPixelRect>,
    ) -> PlacedFragment {
        let placement = facts.placement.expect("a box is placed before its fragment is built");
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let PendingFragment {
            node: _,
            children,
            pending_abspos: pending_abspos_from_placed_box,
            anchor_candidates: anchor_candidates_from_placed_box,
            inline_containing_block_rects: inline_containing_block_rects_from_placed_box,
            payloads,
        } = inner
            .pending_fragments
            .remove(&slot)
            .unwrap_or_else(|| PendingFragment::new(node));
        let content_size = (geometry.cells.content_inline_size, geometry.cells.content_block_size);
        let fragment = snapshot_fragment(callbacks, node, children, geometry, facts.fragment_facts, payloads);
        let link = link_fragment(
            fragment.clone(),
            PlacementData::of_box(
                &facts,
                abspos_layout_inputs,
                containing_line_box_index,
                committed_offset,
                callbacks.frame,
            ),
        );
        self.attach(&mut inner, link, containing_block);
        let exports_payloads = !pending_abspos_from_placed_box.is_empty()
            || !anchor_candidates_from_placed_box.is_empty()
            || !inline_containing_block_rects_from_placed_box.is_empty()
            || own_anchor_candidate_border_box_rect.is_some();
        let content_offset = placement.content_offset_in_containing_block;
        let frames = exports_payloads
            .then(|| {
                let own = formatting_context::frame_established_by(callbacks, node);
                (own != callbacks.frame).then(|| PlacedBoxFrames {
                    own,
                    containing_block: callbacks.frame,
                    physical_content_size: callbacks.frame.size_to_physical(content_size.0, content_size.1),
                })
            })
            .flatten();
        for mut entry in pending_abspos_from_placed_box {
            debug_assert!(
                entry.containing_block() != node,
                "an abspos registration outlived its containing block's placement drain (slot {})",
                entry.child_box.slot_index()
            );
            propagate_payload_into_containing_block_space(&mut entry, node, containing_block, content_offset, frames);
            match inner
                .pending_fragments
                .get_mut(&entry.coordinate_space_box.slot_index())
            {
                Some(pending_fragment) => pending_fragment.pending_abspos.push(entry),
                None => inner.pending_abspos_at_root.push(entry),
            }
        }
        let own_candidate = own_anchor_candidate_border_box_rect.map(|border_box_rect| AnchorCandidate {
            node,
            border_box_rect,
            coordinate_space_box: containing_block.unwrap_or(node),
        });
        for mut candidate in anchor_candidates_from_placed_box.into_iter().chain(own_candidate) {
            propagate_payload_into_containing_block_space(
                &mut candidate,
                node,
                containing_block,
                content_offset,
                frames,
            );
            match inner
                .pending_fragments
                .get_mut(&candidate.coordinate_space_box.slot_index())
            {
                Some(pending_fragment) => pending_fragment.anchor_candidates.push(candidate),
                None => inner.anchor_candidates_at_root.push(candidate),
            }
        }
        for mut payload in inline_containing_block_rects_from_placed_box {
            propagate_payload_into_containing_block_space(&mut payload, node, containing_block, content_offset, frames);
            match inner
                .pending_fragments
                .get_mut(&payload.coordinate_space_box.slot_index())
            {
                Some(pending_fragment) => pending_fragment.inline_containing_block_rects.push(payload),
                None => inner.inline_containing_block_rects_at_root.push(payload),
            }
        }
        PlacedFragment {
            fragment,
            exports_payloads,
            abspos_boxes_placed_with_it: Vec::new(),
        }
    }

    /// Places the committed fragment of a box as it is. Where the box's subtree is reused as it is, commit stops at the
    /// box, so its committed descendants stay as they are.
    #[expect(clippy::too_many_arguments)]
    pub(crate) fn attach_committed_fragment(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        fragment: std::sync::Arc<Fragment>,
        reuses_subtree: bool,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
        facts: run_state::FactsDecidedByParent,
        abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
        containing_line_box_index: Option<usize>,
        committed_offset: FfiCssPixelPoint,
        frame: geometry::Frame,
    ) {
        assert!(
            facts.placement.is_some(),
            "a box is placed before its fragment is attached"
        );
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        debug_assert!(
            !inner.pending_fragments.contains_key(&slot),
            "a reused subtree's box has fragments of its own in this run (slot {slot})"
        );
        let link = link_fragment(
            fragment,
            PlacementData::of_box(
                &facts,
                abspos_layout_inputs,
                containing_line_box_index,
                committed_offset,
                frame,
            ),
        );
        self.attach(&mut inner, link, containing_block);
        if reuses_subtree {
            inner.reused_subtree_roots.insert(slot);
        }
    }

    /// How many abspos containing block contributions the run has collected so far.
    /// How many abspos containing block info contributions the run has received so far, which only grows.
    pub(crate) fn abspos_containing_block_info_contributions_received(&self) -> u64 {
        self.inner.borrow().abspos_containing_block_info_contributions_received
    }

    fn attach(
        &self,
        inner: &mut RunFragmentBuilderInner,
        link: FragmentLink,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
    ) {
        let Some(containing_block) = containing_block else {
            inner.top_scope_links.push(link);
            return;
        };
        if containing_block == self.root_node || Some(containing_block.slot_index()) == self.root_containing_block_slot
        {
            inner.top_scope_links.push(link);
            return;
        }
        if let Some(pending_fragment) = inner.pending_fragments.get_mut(&containing_block.slot_index()) {
            pending_fragment.children.push(link);
            return;
        }
        // NB: A containing block the run placed already is sealed, so the box goes to the top of the run's fragments.
        if self.is_entry_accumulator || inner.placed_boxes.is_placed(containing_block) {
            inner.top_scope_links.push(link);
            return;
        }
        let mut pending_fragment = PendingFragment::new(containing_block);
        pending_fragment.children.push(link);
        inner
            .pending_fragments
            .insert(containing_block.slot_index(), pending_fragment);
    }

    pub(crate) fn take_unplaced_root(&self, callbacks: &LayoutPass<'_>) -> UnplacedRootFragment {
        debug_assert!(
            !self.is_entry_accumulator,
            "an entry accumulator closes as a pass, not a run"
        );
        self.close(callbacks)
    }

    pub(crate) fn take_completed_pass(&self, callbacks: &LayoutPass<'_>) -> CompletedPassFragments {
        debug_assert!(
            self.is_entry_accumulator,
            "an ordinary run closes as a singular unplaced root"
        );
        let root = self.close(callbacks);
        CompletedPassFragments {
            roots: root.scoped_descendants,
            reused_subtree_roots: root.reused_subtree_roots,
        }
    }

    fn close(&self, callbacks: &LayoutPass<'_>) -> UnplacedRootFragment {
        let mut inner = self.inner.take();
        let mut propagated_pending_abspos = std::mem::take(&mut inner.pending_abspos_at_root);
        let mut propagated_anchor_candidates = std::mem::take(&mut inner.anchor_candidates_at_root);
        let mut propagated_inline_containing_block_rects =
            std::mem::take(&mut inner.inline_containing_block_rects_at_root);
        let propagated_abspos_containing_block_info =
            std::mem::take(&mut inner.abspos_containing_block_info_contributions);
        let pending_fragments = std::mem::take(&mut inner.pending_fragments);
        for (_, pending_fragment) in pending_fragments {
            propagated_pending_abspos.extend(pending_fragment.pending_abspos);
            propagated_anchor_candidates.extend(pending_fragment.anchor_candidates);
            propagated_inline_containing_block_rects.extend(pending_fragment.inline_containing_block_rects);
            // NB: Every formatting context places the containing blocks of the children it places, or leaves them
            //     unplaced with the children they contain.
            assert!(
                pending_fragment.children.is_empty(),
                "{:?} contains placed children but was never placed",
                pending_fragment.node
            );
        }
        // NB: A reused subtree root its run never placed has no children of its own in this run, as an empty child run
        //     root has, and closes the same way.
        // An empty box that was never placed has no fragment in this run.
        let mut child_run_roots_left_unplaced = Vec::new();
        for ChildLeftUnplaced {
            arrival,
            unplaced_child,
        } in std::mem::take(&mut inner.children_left_unplaced)
        {
            match arrival {
                ChildFragmentArrival::CommittedFragment {
                    fragment,
                    reuses_subtree: true,
                } => {
                    let node = fragment.node;
                    inner.reused_subtree_roots.insert(node.slot_index());
                    let placement = PlacementData::of_unplaced_child(&unplaced_child, callbacks.frame);
                    inner.top_scope_links.push(link_fragment(
                        snapshot_fragment(
                            callbacks,
                            node,
                            Vec::new(),
                            unplaced_child.geometry,
                            unplaced_child.facts_decided_by_parent.fragment_facts,
                            Some(Box::new(FragmentPayloads::of_fragment(&fragment))),
                        ),
                        placement,
                    ));
                }
                ChildFragmentArrival::CommittedFragment {
                    reuses_subtree: false, ..
                } => {}
                ChildFragmentArrival::RunRoot {
                    root,
                    reuses_committed_subtree,
                } => child_run_roots_left_unplaced.push((root, reuses_committed_subtree, unplaced_child)),
            }
        }
        for (root, reuses_committed_subtree, unplaced_child) in child_run_roots_left_unplaced {
            inner
                .reused_subtree_roots
                .extend(root.reused_subtree_roots.iter().copied());
            if reuses_committed_subtree {
                inner.reused_subtree_roots.insert(root.node.slot_index());
            }
            let placement = PlacementData::of_unplaced_child(&unplaced_child, callbacks.frame);
            inner.top_scope_links.push(link_fragment(
                snapshot_fragment(
                    callbacks,
                    root.node,
                    root.scoped_descendants,
                    unplaced_child.geometry,
                    unplaced_child.facts_decided_by_parent.fragment_facts,
                    root.root_payloads,
                ),
                placement,
            ));
        }
        let placed_boxes = &inner.placed_boxes;
        for entry in &mut propagated_pending_abspos {
            debug_assert!(
                {
                    let containing_block = entry.containing_block();
                    containing_block.is_invalid()
                        || containing_block == self.root_node
                        || !placed_boxes.is_placed(containing_block)
                },
                "a drainable abspos registration survived its containing block's run (slot {})",
                entry.child_box.slot_index()
            );
            propagate_payload_toward_run_root_space(entry, self.root_node, placed_boxes, callbacks);
        }
        for candidate in &mut propagated_anchor_candidates {
            propagate_payload_toward_run_root_space(candidate, self.root_node, placed_boxes, callbacks);
        }
        for payload in &mut propagated_inline_containing_block_rects {
            propagate_payload_toward_run_root_space(payload, self.root_node, placed_boxes, callbacks);
        }
        UnplacedRootFragment {
            node: self.root_node,
            scoped_descendants: inner.top_scope_links,
            reused_subtree_roots: inner.reused_subtree_roots,
            propagated_pending_abspos,
            propagated_anchor_candidates,
            propagated_inline_containing_block_rects,
            propagated_abspos_containing_block_info,
            root_payloads: inner.root_payloads,
        }
    }
}

// A pass's fragments are an immutable publication that later passes and paintable rows share.
const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<CompletedPassFragments>();
};
