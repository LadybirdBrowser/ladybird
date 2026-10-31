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
    /// The used margins, which the placement of the box decides, so that a change to them alone keeps the fragment.
    pub(crate) margin_left: CssPixels,
    pub(crate) margin_right: CssPixels,
    pub(crate) margin_top: CssPixels,
    pub(crate) margin_bottom: CssPixels,
    /// What the parent decided about how the box shows, as `run_state::FragmentFactsDecidedByParent` describes it.
    pub(crate) table_column_index: u32,
    pub(crate) table_column_span: u32,
    pub(crate) hidden_by_collapsed_columns: bool,
    pub(crate) is_invisible_for_line_clamp: bool,
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
        std::ptr::eq(self, previous) || self.has_same_box_properties(previous) && self.holds_same_contents_as(previous)
    }

    /// Whether the fragment holds the very lines and child fragments `previous` holds, placed the same way.
    pub(crate) fn holds_same_contents_as(&self, previous: &Fragment) -> bool {
        same_allocation(self.line_data.as_ref(), previous.line_data.as_ref())
            && self.children.len() == previous.children.len()
            && self
                .children
                .iter()
                .zip(&previous.children)
                .all(|(link, previous_link)| link.places_same_fragment_identically_to(previous_link))
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
            margin_left: CssPixels::default(),
            margin_right: CssPixels::default(),
            margin_top: CssPixels::default(),
            margin_bottom: CssPixels::default(),
            table_column_index: 0,
            table_column_span: 0,
            hidden_by_collapsed_columns: false,
            is_invisible_for_line_clamp: false,
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

    /// Whether the link shows its fragment with the same margins and the same facts its parent decided about the box,
    /// which change what painting shows of the box while the fragment stays the same.
    pub(crate) fn shows_box_as(&self, previous: &FragmentLink) -> bool {
        self.margin_left == previous.margin_left
            && self.margin_right == previous.margin_right
            && self.margin_top == previous.margin_top
            && self.margin_bottom == previous.margin_bottom
            && self.table_column_index == previous.table_column_index
            && self.table_column_span == previous.table_column_span
            && self.hidden_by_collapsed_columns == previous.hidden_by_collapsed_columns
            && self.is_invisible_for_line_clamp == previous.is_invisible_for_line_clamp
    }

    pub(crate) fn has_same_placement(&self, previous: &FragmentLink) -> bool {
        self.fragment.node == previous.fragment.node
            && self.committed_offset == previous.committed_offset
            && self.inset_left == previous.inset_left
            && self.inset_right == previous.inset_right
            && self.inset_top == previous.inset_top
            && self.inset_bottom == previous.inset_bottom
            && self.shows_box_as(previous)
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

fn sort_in_tree_order(entries: &mut [abspos_inputs::PendingAbsposChild], callbacks: &LayoutPass<'_>) {
    entries.sort_by(|left, right| {
        if left.child_box == right.child_box {
            std::cmp::Ordering::Equal
        } else if callbacks.is_before(left.child_box, right.child_box) {
            std::cmp::Ordering::Less
        } else {
            std::cmp::Ordering::Greater
        }
    });
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

/// The fragment the box of `candidate` committed, where `candidate` builds identically to it. A fragment that continues
/// in another fragment of its box matches none.
pub(crate) fn previously_committed_fragment_matching(
    callbacks: &LayoutPass<'_>,
    candidate: &Fragment,
) -> Option<std::sync::Arc<Fragment>> {
    if candidate.continues_from_previous_fragment
        || candidate.continues_in_next_fragment
        || !callbacks.has_committed_fragment_link(candidate.node)
    {
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
    pub(crate) fn of_fragment(fragment: &Fragment) -> Self {
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

/// What the placement of a box shows of the box's own layout, in the frame of the run that places it: the fragment shows
/// its sizes, borders, paddings and lines, and the link shows its margins.
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
        clamped_content_is_scrollable_overflow,
        continues_from_previous_fragment: fragment_facts.continues_from_previous_fragment,
        continues_in_next_fragment: fragment_facts.continues_in_next_fragment,
        children,
    };
    if let Some(previous) = previously_committed_fragment_matching(callbacks, &fragment) {
        return previous;
    }
    fragment.identity = NEXT_IDENTITY.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    std::sync::Arc::new(fragment)
}

/// The fragment of a placed box: `arrived`, the fragment its own run built or the committed one, where that shows the
/// box as placed and nothing else was placed in it, or else a fragment of the box's `geometry` around the arrived
/// fragment's children and the `children` placed in it here.
fn fragment_of_placed_box(
    callbacks: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    arrived: Option<std::sync::Arc<Fragment>>,
    children: Vec<FragmentLink>,
    geometry: FragmentGeometry,
    fragment_facts: run_state::FragmentFactsDecidedByParent,
    payloads: Option<Box<FragmentPayloads>>,
) -> std::sync::Arc<Fragment> {
    let Some(arrived) = arrived else {
        return snapshot_fragment(callbacks, node, children, geometry, fragment_facts, payloads);
    };
    let shows_box_as_placed = children.is_empty()
        && geometry.shows_box_of_fragment(&arrived)
        && arrived.continues_from_previous_fragment == fragment_facts.continues_from_previous_fragment
        && arrived.continues_in_next_fragment == fragment_facts.continues_in_next_fragment;
    if shows_box_as_placed {
        return arrived;
    }
    let mut all_children = arrived.children.clone();
    all_children.extend(children);
    snapshot_fragment(
        callbacks,
        node,
        all_children,
        geometry,
        fragment_facts,
        Some(Box::new(FragmentPayloads::of_fragment(&arrived))),
    )
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
    pub(crate) margin_left: CssPixels,
    pub(crate) margin_right: CssPixels,
    pub(crate) margin_top: CssPixels,
    pub(crate) margin_bottom: CssPixels,
    pub(crate) table_column_index: u32,
    pub(crate) table_column_span: u32,
    pub(crate) hidden_by_collapsed_columns: bool,
    pub(crate) is_invisible_for_line_clamp: bool,
    pub(crate) containing_line_box_index: Option<usize>,
    pub(crate) abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
    pub(crate) default_scroll_shift: Option<abspos_inputs::DefaultScrollShift>,
    pub(crate) containing_block: crate::layout::node_data::NodeSlotId,
    pub(crate) frame: geometry::Frame,
}

impl PlacementData {
    fn of_box(
        facts: &run_state::FactsDecidedByParent,
        cells: &used_values::RootInputCells,
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
            margin_left: cells.margin_left,
            margin_right: cells.margin_right,
            margin_top: cells.margin_top,
            margin_bottom: cells.margin_bottom,
            table_column_index: facts.fragment_facts.table_column_index,
            table_column_span: facts.fragment_facts.table_column_span,
            hidden_by_collapsed_columns: facts.fragment_facts.hidden_by_collapsed_columns,
            is_invisible_for_line_clamp: facts.fragment_facts.is_invisible_for_line_clamp,
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
            &unplaced_child.geometry.cells,
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
        margin_left: placement.margin_left,
        margin_right: placement.margin_right,
        margin_top: placement.margin_top,
        margin_bottom: placement.margin_bottom,
        table_column_index: placement.table_column_index,
        table_column_span: placement.table_column_span,
        hidden_by_collapsed_columns: placement.hidden_by_collapsed_columns,
        is_invisible_for_line_clamp: placement.is_invisible_for_line_clamp,
        containing_line_box_index: placement.containing_line_box_index,
        abspos_layout_inputs: placement.abspos_layout_inputs.map(std::sync::Arc::new),
        default_scroll_shift: placement.default_scroll_shift,
        containing_block: placement.containing_block,
        frame: placement.frame,
    }
}

/// A child the run laid out but does not place: the root fragment its layout handed its placement, and what placing
/// it would have taken.
struct ChildLeftUnplaced {
    root: RunRootFragment,
    unplaced_child: crate::layout::formatting_context::PlacedChild,
}

/// What a run hands past its root toward the boxes outside it: the absolutely positioned boxes whose containing block
/// is outside the run, the anchor candidates and inline containing block rects they are positioned against, and the
/// containing block info of absolutely positioned boxes.
#[derive(Clone, Default)]
pub(crate) struct PayloadsPastRun {
    pub(crate) pending_abspos: Vec<abspos_inputs::PendingAbsposChild>,
    pub(crate) anchor_candidates: Vec<AnchorCandidate>,
    pub(crate) inline_containing_block_rects: Vec<InlineContainingBlockRect>,
    pub(crate) abspos_containing_block_info: Vec<AbsposContainingBlockInfoContribution>,
}

impl PayloadsPastRun {
    pub(crate) fn is_empty(&self) -> bool {
        self.pending_abspos.is_empty()
            && self.anchor_candidates.is_empty()
            && self.inline_containing_block_rects.is_empty()
            && self.abspos_containing_block_info.is_empty()
    }
}

/// What a run laid out inside its root, before the root's fragment exists: the links at the top of the run's
/// fragments, what goes past the run, and what the run's layout of its root hands the root's fragment. A normal run
/// turns them into its root's fragment, a fragmentainer's run hands them to the run of the box it belongs to, and a
/// whole layout pass hands its links to commit.
pub(crate) struct RunContents {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) scoped_descendants: Vec<FragmentLink>,
    pub(crate) payloads_past_run: PayloadsPastRun,
    /// What the run's layout of its root hands the root's fragment.
    pub(crate) root_payloads: Option<Box<FragmentPayloads>>,
}

impl RunContents {
    /// The root's fragment around what the run laid out inside it, showing the root's own layout `geometry`.
    pub(crate) fn into_run_root_fragment(
        self,
        callbacks: &LayoutPass<'_>,
        geometry: FragmentGeometry,
    ) -> RunRootFragment {
        let Self {
            node,
            scoped_descendants,
            payloads_past_run,
            root_payloads,
        } = self;
        RunRootFragment {
            fragment: snapshot_fragment(
                callbacks,
                node,
                scoped_descendants,
                geometry,
                run_state::FragmentFactsDecidedByParent::default(),
                root_payloads,
            ),
            payloads_past_run,
        }
    }
}

/// What a run hands its parent of its root: the root's fragment, and what goes past the run toward boxes outside it.
/// A box its parent takes as it was committed hands its committed fragment the same way, with nothing past it.
#[derive(Clone)]
#[must_use = "a child's fragments are lost unless its placement takes them"]
pub(crate) struct RunRootFragment {
    pub(crate) fragment: std::sync::Arc<Fragment>,
    pub(crate) payloads_past_run: PayloadsPastRun,
}

impl RunRootFragment {
    /// The root fragment of a run that hands nothing past `fragment`.
    pub(crate) fn propagating_nothing(fragment: std::sync::Arc<Fragment>) -> Self {
        Self {
            fragment,
            payloads_past_run: PayloadsPastRun::default(),
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
    kept_subtree_roots: HashSet<u32>,
}

impl<'tree> LinksBySlot<'tree> {
    /// Whether the box in `slot` keeps its committed subtree, which commit then leaves as it is below the box.
    pub(crate) fn keeps_committed_subtree(&self, slot: u32) -> bool {
        self.kept_subtree_roots.contains(&slot)
    }

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
}

impl CompletedPassFragments {
    /// The links of the pass by the slot of their box, below `pass_root`. A box keeps its committed subtree where
    /// `keeps_committed_subtree` holds for its link and the fragment of no box around it holds the link of a box inside
    /// it, which the box's own fragment would then not show. Commit looks up nothing below such a box.
    pub(crate) fn links_by_slot(
        &self,
        pass_root: crate::layout::node_data::NodeSlotId,
        parent_of: impl Fn(crate::layout::node_data::NodeSlotId) -> crate::layout::node_data::NodeSlotId,
        keeps_committed_subtree: impl Fn(&FragmentLink) -> bool,
    ) -> LinksBySlot<'_> {
        struct KeptSubtrees<ParentOf, KeepsCommittedSubtree> {
            parent_of: ParentOf,
            keeps_committed_subtree: KeepsCommittedSubtree,
            boxes_around_links_held_outside_them: HashSet<u32>,
            kept_subtree_roots: HashSet<u32>,
        }
        impl<ParentOf, KeepsCommittedSubtree> KeptSubtrees<ParentOf, KeepsCommittedSubtree>
        where
            ParentOf: Fn(crate::layout::node_data::NodeSlotId) -> crate::layout::node_data::NodeSlotId,
            KeepsCommittedSubtree: Fn(&FragmentLink) -> bool,
        {
            /// Decides which of `links`, held by the fragment of `holder`, keep their committed subtrees, and answers
            /// how many links commit looks up among them.
            fn decide(&mut self, links: &[FragmentLink], holder: crate::layout::node_data::NodeSlotId) -> usize {
                for link in links {
                    if link.fragment.node == holder {
                        continue;
                    }
                    let mut ancestor = (self.parent_of)(link.fragment.node);
                    while !ancestor.is_invalid() && ancestor != holder {
                        self.boxes_around_links_held_outside_them.insert(ancestor.slot_index());
                        ancestor = (self.parent_of)(ancestor);
                    }
                }
                links
                    .iter()
                    .map(|link| {
                        let node = link.fragment.node;
                        if !self.boxes_around_links_held_outside_them.contains(&node.slot_index())
                            && (self.keeps_committed_subtree)(link)
                        {
                            self.kept_subtree_roots.insert(node.slot_index());
                            return 1;
                        }
                        1 + self.decide(&link.fragment.children, node)
                    })
                    .sum()
            }
        }
        fn insert_links<'tree>(
            links: &'tree [FragmentLink],
            parent: Option<(crate::layout::node_data::NodeSlotId, u32, usize)>,
            kept_subtree_roots: &HashSet<u32>,
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
                if !kept_subtree_roots.contains(&slot) {
                    insert_links(
                        &link.fragment.children,
                        Some((link.fragment.node, fragment_index, line_index_offset)),
                        kept_subtree_roots,
                        links_by_slot,
                    );
                }
            }
        }
        let mut kept_subtrees = KeptSubtrees {
            parent_of,
            keeps_committed_subtree,
            boxes_around_links_held_outside_them: HashSet::default(),
            kept_subtree_roots: HashSet::default(),
        };
        let link_count = kept_subtrees.decide(&self.roots, pass_root);
        let kept_subtree_roots = kept_subtrees.kept_subtree_roots;
        let mut links_by_slot = LinksBySlot {
            in_first_containing_block_fragment: HashMap::with_capacity_and_hasher(link_count, Default::default()),
            other: HashMap::default(),
            kept_subtree_roots: HashSet::default(),
        };
        insert_links(&self.roots, None, &kept_subtree_roots, &mut links_by_slot);
        links_by_slot.kept_subtree_roots = kept_subtree_roots;
        links_by_slot
    }
}

struct PendingFragment {
    node: crate::layout::node_data::NodeSlotId,
    /// The fragment the box's layout handed its placement: the root fragment of its own run, or its committed fragment.
    arrived: Option<std::sync::Arc<Fragment>>,
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
            arrived: None,
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
        sort_in_tree_order(&mut taken, callbacks);
        taken
    }

    /// Keeps the sizes and box sides of the run's root, in the frame its contents are laid out in, for the absolutely
    /// positioned boxes the run lays out against the root at its end.
    pub(crate) fn keep_root_cells_for_contained_abspos(&self, cells: CellsInFrame) {
        debug_assert!(
            !self.is_entry_accumulator,
            "an entry accumulator has no root of its own"
        );
        self.inner
            .borrow_mut()
            .placed_boxes
            .cells_of_abspos_containing_blocks
            .insert(self.root_node.slot_index(), cells);
    }

    /// Takes the absolutely positioned boxes whose containing block is the run's root and whose positions the run
    /// knows, in tree order. Those inside a child the run left unplaced stay for the run's parent.
    pub(crate) fn take_abspos_contained_by_root(
        &self,
        callbacks: &LayoutPass<'_>,
    ) -> Vec<abspos_inputs::PendingAbsposChild> {
        let root = self.root_node;
        let mut taken: Vec<abspos_inputs::PendingAbsposChild> = self
            .inner
            .borrow_mut()
            .pending_abspos_at_root
            .extract_if(.., |entry| entry.containing_block() == root)
            .collect();
        sort_in_tree_order(&mut taken, callbacks);
        taken
    }

    /// Keeps what the layout of a child the run laid out but does not place handed its placement, with what placing the
    /// child would have taken.
    pub(crate) fn leave_child_unplaced(
        &self,
        root: RunRootFragment,
        unplaced_child: crate::layout::formatting_context::PlacedChild,
    ) {
        debug_assert!(
            !self.left_child_unplaced(root.fragment.node),
            "a child was left unplaced twice"
        );
        self.inner
            .borrow_mut()
            .children_left_unplaced
            .push(ChildLeftUnplaced { root, unplaced_child });
    }

    pub(crate) fn left_child_unplaced(&self, node: crate::layout::node_data::NodeSlotId) -> bool {
        self.inner
            .borrow()
            .children_left_unplaced
            .iter()
            .any(|child| child.root.fragment.node == node)
    }

    /// Takes in what the run of a fragmentainer of this run's root laid out, `offset` to where the fragmentainer
    /// sits, with its lines after the `root_line_index_offset` lines of the root's earlier fragmentainers.
    pub(crate) fn adopt_fragmentainer_contents(
        &self,
        contents: RunContents,
        offset: FfiCssPixelPoint,
        root_line_index_offset: usize,
    ) {
        debug_assert!(
            contents.node == self.root_node,
            "a fragmentainer run's root is the box of the run that adopts it"
        );
        let mut inner = self.inner.borrow_mut();
        for mut link in contents.scoped_descendants {
            link.committed_offset = formatting_context::point_add(link.committed_offset, offset);
            if link.containing_block == self.root_node
                && let Some(line_index) = link.containing_line_box_index.as_mut()
            {
                *line_index += root_line_index_offset;
            }
            inner.top_scope_links.push(link);
        }
        for mut entry in contents.payloads_past_run.pending_abspos {
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
        for mut candidate in contents.payloads_past_run.anchor_candidates {
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
        for mut payload in contents.payloads_past_run.inline_containing_block_rects {
            payload.translate_by(offset);
            inner.inline_containing_block_rects_at_root.push(payload);
        }
        inner.receive_abspos_containing_block_info(contents.payloads_past_run.abspos_containing_block_info);
    }

    /// Takes in the root fragment of the run that laid out `node`, which is being placed, and what goes past that run.
    pub(crate) fn take_in_child_root_at_placement(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        root: RunRootFragment,
        callbacks: &LayoutPass<'_>,
    ) {
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
        fragment_arrival: Option<RunRootFragment>,
    ) {
        debug_assert!(!self.left_child_unplaced(node), "a child left unplaced is discarded");
        let arrived_payloads = fragment_arrival.map(|root| Box::new(FragmentPayloads::of_fragment(&root.fragment)));
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let mut payloads_of_box = arrived_payloads;
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
        root: RunRootFragment,
        callbacks: &LayoutPass<'_>,
    ) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let pending_fragment = inner
            .pending_fragments
            .entry(slot)
            .or_insert_with(|| PendingFragment::new(node));
        debug_assert!(
            root.fragment.node == node,
            "a box took in the root of another box's run"
        );
        debug_assert!(
            pending_fragment.children.is_empty() || root.fragment.children.is_empty(),
            "a child run's root and an open pending fragment both carry children for slot {slot}"
        );
        pending_fragment.arrived = Some(root.fragment);
        pending_fragment
            .pending_abspos
            .extend(root.payloads_past_run.pending_abspos.into_iter().map(|mut entry| {
                formatting_context::resolve_pending_abspos_containing_block(callbacks, &mut entry, self.root_node);
                entry
            }));
        pending_fragment
            .anchor_candidates
            .extend(root.payloads_past_run.anchor_candidates);
        pending_fragment
            .inline_containing_block_rects
            .extend(root.payloads_past_run.inline_containing_block_rects);
        inner.receive_abspos_containing_block_info(root.payloads_past_run.abspos_containing_block_info);
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
            arrived,
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
        let link_placement = PlacementData::of_box(
            &facts,
            &geometry.cells,
            abspos_layout_inputs,
            containing_line_box_index,
            committed_offset,
            callbacks.frame,
        );
        let fragment = fragment_of_placed_box(
            callbacks,
            node,
            arrived,
            children,
            geometry,
            facts.fragment_facts,
            payloads,
        );
        let link = link_fragment(fragment.clone(), link_placement);
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

    pub(crate) fn take_run_contents(&self, callbacks: &LayoutPass<'_>) -> RunContents {
        debug_assert!(
            !self.is_entry_accumulator,
            "an entry accumulator closes as a pass, not a run"
        );
        self.close(callbacks)
    }

    pub(crate) fn take_completed_pass(&self, callbacks: &LayoutPass<'_>) -> CompletedPassFragments {
        debug_assert!(
            self.is_entry_accumulator,
            "an ordinary run closes into the contents of its root"
        );
        let root = self.close(callbacks);
        CompletedPassFragments {
            roots: root.scoped_descendants,
        }
    }

    fn close(&self, callbacks: &LayoutPass<'_>) -> RunContents {
        let mut inner = self.inner.take();
        let mut payloads_past_run = PayloadsPastRun {
            pending_abspos: std::mem::take(&mut inner.pending_abspos_at_root),
            anchor_candidates: std::mem::take(&mut inner.anchor_candidates_at_root),
            inline_containing_block_rects: std::mem::take(&mut inner.inline_containing_block_rects_at_root),
            abspos_containing_block_info: std::mem::take(&mut inner.abspos_containing_block_info_contributions),
        };
        let pending_fragments = std::mem::take(&mut inner.pending_fragments);
        for (_, pending_fragment) in pending_fragments {
            payloads_past_run.pending_abspos.extend(pending_fragment.pending_abspos);
            payloads_past_run
                .anchor_candidates
                .extend(pending_fragment.anchor_candidates);
            payloads_past_run
                .inline_containing_block_rects
                .extend(pending_fragment.inline_containing_block_rects);
            // NB: Every formatting context places the containing blocks of the children it places, or leaves them
            //     unplaced with the children they contain.
            assert!(
                pending_fragment.children.is_empty(),
                "{:?} contains placed children but was never placed",
                pending_fragment.node
            );
        }
        // NB: A child run's root that the run never placed keeps its fragment at the top of the run's fragments, as its
        //     subtree goes with it.
        for ChildLeftUnplaced { root, unplaced_child } in std::mem::take(&mut inner.children_left_unplaced) {
            let node = root.fragment.node;
            let placement = PlacementData::of_unplaced_child(&unplaced_child, callbacks.frame);
            inner.top_scope_links.push(link_fragment(
                fragment_of_placed_box(
                    callbacks,
                    node,
                    Some(root.fragment),
                    Vec::new(),
                    unplaced_child.geometry,
                    unplaced_child.facts_decided_by_parent.fragment_facts,
                    None,
                ),
                placement,
            ));
        }
        let placed_boxes = &inner.placed_boxes;
        for entry in &mut payloads_past_run.pending_abspos {
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
        for candidate in &mut payloads_past_run.anchor_candidates {
            propagate_payload_toward_run_root_space(candidate, self.root_node, placed_boxes, callbacks);
        }
        for payload in &mut payloads_past_run.inline_containing_block_rects {
            propagate_payload_toward_run_root_space(payload, self.root_node, placed_boxes, callbacks);
        }
        RunContents {
            node: self.root_node,
            scoped_descendants: inner.top_scope_links,
            payloads_past_run,
            root_payloads: inner.root_payloads,
        }
    }
}

// A pass's fragments are an immutable publication that later passes and paintable rows share.
const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<CompletedPassFragments>();
};
