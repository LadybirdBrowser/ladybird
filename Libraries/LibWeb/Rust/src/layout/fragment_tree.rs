/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::used_values::CommittedSvgFacts;
use smallvec::SmallVec;

/// Which border model a box follows, and in the collapsing borders model, whether the box is the table box or a cell:
/// the two own different parts of the collapsed borders at their edges (see `used_values::collapsed_border_share`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum CollapsingBorders {
    #[default]
    Separate,
    CollapsedCell,
    CollapsedTableBox,
}

impl CollapsingBorders {
    pub(crate) fn of(uses_collapsing_borders_model: bool, is_collapsed_borders_table_box: bool) -> Self {
        match (uses_collapsing_borders_model, is_collapsed_borders_table_box) {
            (false, _) => Self::Separate,
            (true, false) => Self::CollapsedCell,
            (true, true) => Self::CollapsedTableBox,
        }
    }

    pub(crate) fn uses_collapsing_borders_model(self) -> bool {
        self != Self::Separate
    }

    pub(crate) fn is_collapsed_borders_table_box(self) -> bool {
        self == Self::CollapsedTableBox
    }
}

/// The content size, borders and paddings a fragment shows of its box, in the physical plane.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct BoxSides {
    pub(crate) content_size: FfiCssPixelSize,
    pub(crate) border: geometry::Sides,
    pub(crate) padding: geometry::Sides,
    pub(crate) collapsing: CollapsingBorders,
}

impl BoxSides {
    /// The sides, which frame `from` holds, as frame `to` holds them.
    pub(crate) fn in_frame(self, from: geometry::Frame, to: geometry::Frame) -> Self {
        if from == to {
            return self;
        }
        let content_size = if from.is_orthogonal_to(to) {
            FfiCssPixelSize {
                width: self.content_size.height,
                height: self.content_size.width,
            }
        } else {
            self.content_size
        };
        Self {
            content_size,
            border: self.border.in_frame(from, to),
            padding: self.padding.in_frame(from, to),
            collapsing: self.collapsing,
        }
    }

    /// The widths of the borders that lie within the border box. In the collapsing borders model, a collapsed border is
    /// centered on the grid line between two boxes, so only a part of it belongs to each (see
    /// `used_values::collapsed_border_share`).
    pub(crate) fn border_box_edges(&self) -> geometry::Sides {
        if !self.collapsing.uses_collapsing_borders_model() {
            return self.border;
        }
        let is_table_box = self.collapsing.is_collapsed_borders_table_box();
        geometry::Sides {
            top: used_values::collapsed_border_share(self.border.top, true, is_table_box),
            right: used_values::collapsed_border_share(self.border.right, false, is_table_box),
            bottom: used_values::collapsed_border_share(self.border.bottom, false, is_table_box),
            left: used_values::collapsed_border_share(self.border.left, true, is_table_box),
        }
    }
}

/// Whether fragmentation broke the box, so that its fragment continues one in an earlier fragmentainer, or continues
/// in a later one.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct BreakFacts {
    pub(crate) continues_from_previous: bool,
    pub(crate) continues_in_next: bool,
}

impl BreakFacts {
    pub(crate) fn box_is_broken(self) -> bool {
        self.continues_from_previous || self.continues_in_next
    }
}

/// For a line-clamp container: whether its contents hold the clamp point, and whether what follows the clamp point
/// still counts as scrollable overflow.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LineClampFacts {
    pub(crate) has_line_clamp_point: bool,
    pub(crate) clamped_content_is_scrollable_overflow: bool,
}

/// What the layout of a box hands its fragment for painting and the devtools besides its geometry, by the kind of
/// layout that produced it.
#[derive(Clone)]
pub(crate) enum FcPayload {
    Table(std::sync::Arc<table_formatting_context::OwnedCollapsedTableBorders>),
    Grid(GridPayload),
    Flex(std::sync::Arc<formatting_context::FlexLayoutData>),
    Multicol(std::sync::Arc<fragmentation::MulticolLayoutData>),
    Math(std::sync::Arc<math_formatting_context::MathPaintData>),
    Svg(std::sync::Arc<SvgPayload>),
}

#[derive(Clone, Default)]
pub(crate) struct GridPayload {
    pub(crate) layout_data: Option<std::sync::Arc<grid_formatting_context::GridLayoutData>>,
    pub(crate) used_tracks: Option<std::sync::Arc<grid_formatting_context::OwnedUsedGridTracks>>,
}

#[derive(Clone, Default)]
pub(crate) struct SvgPayload {
    pub(crate) facts: CommittedSvgFacts,
    pub(crate) geometry: svg_formatting_context::SvgGeometry,
    pub(crate) path: Option<std::sync::Arc<libgfx_rust::path::Path>>,
    pub(crate) text_character_cells: Option<std::sync::Arc<Vec<svg_formatting_context::FfiSvgTextCharacterCell>>>,
}

/// Compares payloads as painting needs: the layout data painting reads through its allocation as the same allocations,
/// and the facts it copies as equal values.
impl PartialEq for FcPayload {
    fn eq(&self, other: &Self) -> bool {
        use std::sync::Arc;
        match (self, other) {
            (Self::Table(left), Self::Table(right)) => Arc::ptr_eq(left, right),
            (Self::Grid(left), Self::Grid(right)) => {
                same_allocation(left.layout_data.as_ref(), right.layout_data.as_ref())
                    && same_allocation(left.used_tracks.as_ref(), right.used_tracks.as_ref())
            }
            (Self::Flex(left), Self::Flex(right)) => Arc::ptr_eq(left, right),
            (Self::Multicol(left), Self::Multicol(right)) => left == right,
            (Self::Math(left), Self::Math(right)) => left == right,
            (Self::Svg(left), Self::Svg(right)) => {
                left.facts == right.facts
                    && left.geometry == right.geometry
                    && same_allocation(left.path.as_ref(), right.path.as_ref())
                    && same_allocation(left.text_character_cells.as_ref(), right.text_character_cells.as_ref())
            }
            _ => false,
        }
    }
}

impl FcPayload {
    /// Whether the payloads hold equal values, as two layouts of one box do.
    pub(crate) fn same_value(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Table(left), Self::Table(right)) => left == right,
            (Self::Grid(left), Self::Grid(right)) => {
                left.layout_data == right.layout_data && left.used_tracks == right.used_tracks
            }
            (Self::Flex(left), Self::Flex(right)) => left == right,
            (Self::Multicol(left), Self::Multicol(right)) => left == right,
            (Self::Math(left), Self::Math(right)) => left == right,
            (Self::Svg(left), Self::Svg(right)) => {
                left.facts == right.facts
                    && left.geometry == right.geometry
                    && left.path == right.path
                    && left.text_character_cells == right.text_character_cells
            }
            _ => false,
        }
    }

    fn holds_nothing(&self) -> bool {
        match self {
            Self::Grid(grid) => grid.layout_data.is_none() && grid.used_tracks.is_none(),
            Self::Svg(svg) => {
                svg.facts == CommittedSvgFacts::default()
                    && svg.geometry == Default::default()
                    && svg.path.is_none()
                    && svg.text_character_cells.is_none()
            }
            Self::Table(_) | Self::Flex(_) | Self::Multicol(_) | Self::Math(_) => false,
        }
    }
}

/// What the layout of a box hands the box's fragment besides its geometry, until the fragment is built.
#[derive(Clone, Default)]
pub(crate) struct PendingPayload {
    payload: Option<std::sync::Arc<FcPayload>>,
    pub(crate) line_clamp: LineClampFacts,
}

impl PendingPayload {
    fn of_fragment(fragment: &Fragment) -> Self {
        Self {
            payload: fragment.payload.clone(),
            line_clamp: fragment.line_clamp,
        }
    }

    fn is_empty(&self) -> bool {
        self.payload.is_none() && self.line_clamp == LineClampFacts::default()
    }

    pub(crate) fn set(&mut self, payload: FcPayload) {
        debug_assert!(
            self.payload
                .as_deref()
                .is_none_or(|held| std::mem::discriminant(held) == std::mem::discriminant(&payload)),
            "the fragment of a box gets payloads of two kinds"
        );
        self.payload = Some(std::sync::Arc::new(payload));
    }

    fn variant_mut<Variant>(
        &mut self,
        empty: fn() -> FcPayload,
        variant: fn(&mut FcPayload) -> Option<&mut Variant>,
    ) -> &mut Variant {
        let payload = std::sync::Arc::make_mut(self.payload.get_or_insert_with(|| std::sync::Arc::new(empty())));
        let holds_other_kind = variant(payload).is_none();
        debug_assert!(!holds_other_kind, "the fragment of a box gets payloads of two kinds");
        if holds_other_kind {
            *payload = empty();
        }
        variant(payload).expect("the payload is of the kind just made")
    }

    pub(crate) fn svg_mut(&mut self) -> &mut SvgPayload {
        self.variant_mut(
            || FcPayload::Svg(Default::default()),
            |payload| match payload {
                FcPayload::Svg(svg) => Some(std::sync::Arc::make_mut(svg)),
                _ => None,
            },
        )
    }
}

/// Whether a fragment holds links whose containing block is outside its box, as a table box holds the rows and cells
/// its wrapper contains, and whether their offsets are final.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum OuterLinks {
    #[default]
    None,
    /// The links of boxes whose containing block is that of the fragment's own box, offset as if that containing
    /// block's content box had no size, until the fragment of the containing block is built. Only a frame that reverses
    /// an axis leaves them so.
    AwaitingContainingBlockSize,
    Placed,
}

pub(crate) struct Fragment {
    pub(crate) identity: u64,
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) sides: BoxSides,
    pub(crate) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    pub(crate) payload: Option<std::sync::Arc<FcPayload>>,
    pub(crate) line_clamp: LineClampFacts,
    pub(crate) breaks: BreakFacts,
    pub(crate) scrollable_overflow: fragment_overflow::FragmentScrollableOverflow,
    pub(crate) outer_links: OuterLinks,
    pub(crate) children: Vec<FragmentLink>,
}

/// What the layout of its parent decides about how a box shows, which the link placing the box's fragment holds.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ParentShowFacts {
    /// For table cells and table-column(-group) boxes: the first grid column the box occupies and the number of grid
    /// columns it spans, so painting can find the cells that originate in a column (CSS 2.2 §17.5.1).
    pub(crate) table_column_index: u32,
    pub(crate) table_column_span: u32,
    /// For table cells: whether every column the cell spans has 'visibility: collapse', which removes the cell from
    /// the display along with the columns (CSS 2.2 §17.5.5).
    pub(crate) hidden_by_collapsed_columns: bool,
    /// Whether the box lies after the clamp point of a line-clamp container, which hides it.
    pub(crate) is_invisible_for_line_clamp: bool,
    /// The physical clip an ancestor's layout puts on the box and its contents, relative to the box's border box: empty
    /// for a box hidden after a clamp point, and the area before the block-end content edge of a line-clamp container
    /// for a float inside it.
    pub(crate) clip_decided_by_parent: Option<CssPixelRect>,
}

impl ParentShowFacts {
    pub(crate) fn hide_after_clamp_point(&mut self) {
        self.is_invisible_for_line_clamp = true;
        self.clip_decided_by_parent = Some(CssPixelRect::default());
    }
}

/// What a link shows of its box apart from the fragment, which change what painting shows of the box while the
/// fragment stays the same: the used margins, which the placement of the box decides, and what the parent decided
/// about how the box shows.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ShownAs {
    pub(crate) margins: geometry::Sides,
    pub(crate) facts: ParentShowFacts,
}

/// Where and how a link places its fragment, apart from its offset. Its insets and margins are physical.
#[derive(Clone, PartialEq)]
pub(crate) struct LinkPlacement {
    pub(crate) insets: geometry::Sides,
    pub(crate) shown_as: ShownAs,
    pub(crate) containing_line_box_index: Option<u32>,
    /// Shared, as few links place an absolutely positioned box, the inputs are larger than the rest of the link, and
    /// layout run cache entries keep copies of links.
    pub(crate) abspos_layout_inputs: Option<std::sync::Arc<abspos_inputs::AbsposLayoutInputs>>,
    /// The default scroll shift of a box whose anchor insets its layout resolved, which commit records for the box.
    pub(crate) default_scroll_shift: Option<abspos_inputs::DefaultScrollShift>,
    pub(crate) containing_block: crate::layout::node_data::NodeSlotId,
}

impl LinkPlacement {
    /// The placement of a box that `facts` and `cells` describe in `frame`.
    fn of_box(
        facts: &run_state::FactsDecidedByParent,
        cells: &used_values::RootInputCells,
        abspos_layout_inputs: Option<std::sync::Arc<abspos_inputs::AbsposLayoutInputs>>,
        containing_line_box_index: Option<u32>,
        frame: geometry::Frame,
    ) -> Self {
        let insets = facts.used_insets;
        Self {
            insets: geometry::Sides {
                top: insets.top,
                right: insets.right,
                bottom: insets.bottom,
                left: insets.left,
            }
            .in_frame(frame, geometry::Frame::PHYSICAL),
            shown_as: ShownAs {
                margins: cells.margins().in_frame(frame, geometry::Frame::PHYSICAL),
                facts: facts.fragment_facts.show,
            },
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
}

#[derive(Clone)]
pub(crate) struct FragmentLink {
    pub(crate) fragment: std::sync::Arc<Fragment>,
    /// The physical offset of the content box of the fragment's box in the content box of its containing block.
    pub(crate) offset: FfiCssPixelPoint,
    pub(crate) placement: LinkPlacement,
}

/// The physical offset of a content box of physical size `content_size` that lies at `offset_in_frame` in the content
/// box of a containing block of physical size `containing_block_size`, which lays out its contents in `frame`.
fn physical_offset(
    offset_in_frame: FfiCssPixelPoint,
    frame: geometry::Frame,
    content_size: FfiCssPixelSize,
    containing_block_size: FfiCssPixelSize,
) -> FfiCssPixelPoint {
    if frame.is_physical() {
        return offset_in_frame;
    }
    let size_in_frame = frame.size_from_physical(content_size);
    frame
        .rect_to_physical(
            CssPixelRect::new(
                offset_in_frame.x,
                offset_in_frame.y,
                size_in_frame.width,
                size_in_frame.height,
            ),
            containing_block_size,
        )
        .location()
        .into()
}

/// A link the builder holds until the physical size of its containing block is known, with its offset in `frame`, the
/// frame in which the containing block lays out its contents.
pub(crate) struct PendingLink {
    fragment: std::sync::Arc<Fragment>,
    offset_in_frame: FfiCssPixelPoint,
    frame: geometry::Frame,
    placement: LinkPlacement,
    /// The physical content size of the containing block, once the builder knows it.
    containing_block_size: Option<FfiCssPixelSize>,
}

impl PendingLink {
    fn into_link(self, containing_block_size: FfiCssPixelSize) -> FragmentLink {
        FragmentLink {
            offset: physical_offset(
                self.offset_in_frame,
                self.frame,
                self.fragment.sides.content_size,
                containing_block_size,
            ),
            fragment: self.fragment,
            placement: self.placement,
        }
    }
}

/// The physical content size `node` committed, or none for a box without a committed fragment.
pub(crate) fn committed_content_size_of(
    pass: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
) -> FfiCssPixelSize {
    if node.is_invalid() {
        return FfiCssPixelSize::default();
    }
    crate::painting::paint_read::GeometryRead::with_committed_fragment_link(pass.arena(), node, |link| {
        link.map(|link| link.fragment.sides.content_size).unwrap_or_default()
    })
}

fn same_allocation<T>(left: Option<&std::sync::Arc<T>>, right: Option<&std::sync::Arc<T>>) -> bool {
    match (left, right) {
        (None, None) => true,
        (Some(left), Some(right)) => std::sync::Arc::ptr_eq(left, right),
        _ => false,
    }
}

impl Fragment {
    pub(crate) fn collapsed_table_borders(
        &self,
    ) -> Option<&std::sync::Arc<table_formatting_context::OwnedCollapsedTableBorders>> {
        match self.payload.as_deref()? {
            FcPayload::Table(borders) => Some(borders),
            _ => None,
        }
    }

    pub(crate) fn grid(&self) -> Option<&GridPayload> {
        match self.payload.as_deref()? {
            FcPayload::Grid(grid) => Some(grid),
            _ => None,
        }
    }

    pub(crate) fn flex_layout_data(&self) -> Option<&std::sync::Arc<formatting_context::FlexLayoutData>> {
        match self.payload.as_deref()? {
            FcPayload::Flex(data) => Some(data),
            _ => None,
        }
    }

    pub(crate) fn multicol_layout_data(&self) -> Option<&std::sync::Arc<fragmentation::MulticolLayoutData>> {
        match self.payload.as_deref()? {
            FcPayload::Multicol(data) => Some(data),
            _ => None,
        }
    }

    pub(crate) fn math_paint_data(&self) -> Option<&std::sync::Arc<math_formatting_context::MathPaintData>> {
        match self.payload.as_deref()? {
            FcPayload::Math(data) => Some(data),
            _ => None,
        }
    }

    pub(crate) fn svg(&self) -> Option<&SvgPayload> {
        match self.payload.as_deref()? {
            FcPayload::Svg(svg) => Some(svg.as_ref()),
            _ => None,
        }
    }

    fn builds_identically_to(&self, previous: &Fragment) -> bool {
        std::ptr::eq(self, previous)
            || self.scrollable_overflow == previous.scrollable_overflow
                && self.builds_like_apart_from_overflow(previous)
    }

    fn builds_like_apart_from_overflow(&self, previous: &Fragment) -> bool {
        self.has_same_box_properties(previous)
            && self.outer_links == previous.outer_links
            && self.holds_same_contents_as(previous)
    }

    pub(crate) fn with_scrollable_overflow(
        &self,
        scrollable_overflow: fragment_overflow::FragmentScrollableOverflow,
    ) -> Self {
        self.with_scrollable_overflow_and_children(scrollable_overflow, self.children.clone())
    }

    fn with_scrollable_overflow_and_children(
        &self,
        scrollable_overflow: fragment_overflow::FragmentScrollableOverflow,
        children: Vec<FragmentLink>,
    ) -> Self {
        Self {
            identity: self.identity,
            node: self.node,
            sides: self.sides,
            line_data: self.line_data.clone(),
            payload: self.payload.clone(),
            line_clamp: self.line_clamp,
            breaks: self.breaks,
            scrollable_overflow,
            outer_links: self.outer_links,
            children,
        }
    }

    /// Whether the fragment holds links of boxes whose containing block is `node`.
    fn holds_links_contained_by(&self, node: crate::layout::node_data::NodeSlotId) -> bool {
        self.outer_links != OuterLinks::None && self.children.iter().any(|link| link.placement.containing_block == node)
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
        self.has_same_box_properties_comparing_payloads(previous, FcPayload::eq)
    }

    /// Box properties excluding inline content and child fragments, with payloads compared by value.
    pub(crate) fn has_same_box_properties_comparing_payloads_by_value(&self, previous: &Fragment) -> bool {
        self.has_same_box_properties_comparing_payloads(previous, FcPayload::same_value)
    }

    /// Whether the layout handed the fragment payloads of the same values as `previous`.
    pub(crate) fn has_payloads_of_same_value_as(&self, previous: &Fragment) -> bool {
        self.has_payloads_matching(previous, FcPayload::same_value)
    }

    fn has_payloads_matching(&self, previous: &Fragment, same_payload: fn(&FcPayload, &FcPayload) -> bool) -> bool {
        self.line_clamp == previous.line_clamp
            && match (self.payload.as_deref(), previous.payload.as_deref()) {
                (None, None) => true,
                (Some(payload), Some(previous_payload)) => same_payload(payload, previous_payload),
                _ => false,
            }
    }

    fn has_same_box_properties_comparing_payloads(
        &self,
        previous: &Fragment,
        same_payload: fn(&FcPayload, &FcPayload) -> bool,
    ) -> bool {
        self.node == previous.node
            && self.sides == previous.sides
            && self.breaks == previous.breaks
            && self.has_payloads_matching(previous, same_payload)
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
                sides: BoxSides::default(),
                line_data: None,
                payload: None,
                line_clamp: LineClampFacts::default(),
                breaks: BreakFacts::default(),
                scrollable_overflow: Default::default(),
                outer_links: OuterLinks::None,
                children: Vec::new(),
            }),
            offset: Default::default(),
            placement: LinkPlacement {
                insets: geometry::Sides::default(),
                shown_as: ShownAs::default(),
                containing_line_box_index: None,
                abspos_layout_inputs: None,
                default_scroll_shift: None,
                containing_block: crate::layout::node_data::NodeSlotId::INVALID,
            },
        }
    }

    /// The offset of the link's content box in the content box of its containing block, read in `frame`, where the
    /// containing block's content box has the physical size `containing_block_size`.
    pub(crate) fn offset_in_frame(
        &self,
        frame: geometry::Frame,
        containing_block_size: FfiCssPixelSize,
    ) -> FfiCssPixelPoint {
        if frame.is_physical() {
            return self.offset;
        }
        frame
            .rect_from_physical(
                CssPixelRect::new(
                    self.offset.x,
                    self.offset.y,
                    self.fragment.sides.content_size.width,
                    self.fragment.sides.content_size.height,
                ),
                containing_block_size,
            )
            .location()
            .into()
    }

    pub(crate) fn places_same_fragment_identically_to(&self, previous: &FragmentLink) -> bool {
        std::sync::Arc::ptr_eq(&self.fragment, &previous.fragment) && self.has_same_placement(previous)
    }

    pub(crate) fn has_same_placement(&self, previous: &FragmentLink) -> bool {
        self.fragment.node == previous.fragment.node
            && self.offset == previous.offset
            && self.placement == previous.placement
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
    placed_boxes: &PlacementsOfBuilder<'_>,
    pass: &LayoutPass<'_>,
) {
    while payload.coordinate_space_box() != run_root {
        let Some(placement) = placed_boxes.placement_of(payload.coordinate_space_box()) else {
            break;
        };
        payload.translate_by(placement.content_offset_in_containing_block);
        let containing_block = placed_boxes.placement_containing_block(pass, payload.coordinate_space_box(), run_root);
        if containing_block.is_invalid() {
            break;
        }
        payload.set_coordinate_space_box(containing_block);
    }
}

/// Moves by `offset` the payloads among `payloads` that are in the coordinate space of `space`.
fn translate_payloads_in_space_of<Payload: PropagatedPayload>(
    payloads: &mut [Payload],
    space: crate::layout::node_data::NodeSlotId,
    offset: FfiCssPixelPoint,
) {
    for payload in payloads
        .iter_mut()
        .filter(|payload| payload.coordinate_space_box() == space)
    {
        payload.translate_by(offset);
    }
}

fn sort_in_tree_order(entries: &mut [abspos_inputs::PendingAbsposChild], pass: &LayoutPass<'_>) {
    entries.sort_by(|left, right| {
        if left.child_box == right.child_box {
            std::cmp::Ordering::Equal
        } else if pass.is_before(left.child_box, right.child_box) {
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

/// The slots a run placed so far, and the sizes and box sides of each placed box that may be the containing block of
/// absolutely positioned boxes, as the run placed it, keyed by slot.
#[derive(Default)]
pub(crate) struct PlacedBoxes {
    placed_slots: Vec<u32>,
    cells_of_abspos_containing_blocks: HashMap<u32, CellsInFrame>,
}

#[derive(Clone, Copy)]
struct PlacedBoxSlot {
    builder: u64,
    placement: run_state::BoxPlacement,
    /// The physical size of the box's content box, once the builder built the box's fragment.
    physical_content_size: Option<FfiCssPixelSize>,
}

impl Default for PlacedBoxSlot {
    fn default() -> Self {
        Self {
            builder: 0,
            placement: run_state::BoxPlacement {
                content_offset_in_containing_block: FfiCssPixelPoint::default(),
                containing_block: crate::layout::node_data::NodeSlotId::INVALID,
            },
            physical_content_size: None,
        }
    }
}

#[derive(Default)]
pub(super) struct FragmentBuilderBuffers {
    pending_fragments: HashMap<u32, PendingFragment>,
    placed_boxes: PlacedBoxes,
}

#[derive(Default)]
pub(crate) struct FragmentBuilderScratch {
    placed_box_slots: super::pass_column::PassColumn<PlacedBoxSlot>,
}

fn next_unique_identity(worker: &super::layout_pass::WorkerScratch) -> u64 {
    const IDENTITIES_PER_RESERVATION: u64 = 1024;
    static NEXT_UNRESERVED_IDENTITY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let reserved = &worker.reserved_unique_identities;
    let mut identities = reserved.replace(0..0);
    if identities.is_empty() {
        let first =
            NEXT_UNRESERVED_IDENTITY.fetch_add(IDENTITIES_PER_RESERVATION, std::sync::atomic::Ordering::Relaxed);
        identities = first..first + IDENTITIES_PER_RESERVATION;
    }
    let identity = identities.start;
    reserved.set(identity + 1..identities.end);
    identity
}

impl FragmentBuilderScratch {
    fn place(
        &self,
        builder: u64,
        node: crate::layout::node_data::NodeSlotId,
        placement: run_state::BoxPlacement,
    ) -> bool {
        self.placed_box_slots.with_row_mut(node.slot_index() as usize, |slot| {
            let was_placed_by_builder = slot.builder == builder;
            *slot = PlacedBoxSlot {
                builder,
                placement,
                physical_content_size: None,
            };
            was_placed_by_builder
        })
    }

    fn note_physical_content_size(
        &self,
        builder: u64,
        node: crate::layout::node_data::NodeSlotId,
        physical_content_size: FfiCssPixelSize,
    ) {
        self.placed_box_slots.with_row_mut(node.slot_index() as usize, |slot| {
            if slot.builder == builder {
                slot.physical_content_size = Some(physical_content_size);
            }
        });
    }

    fn placement_by(
        &self,
        builder: u64,
        node: crate::layout::node_data::NodeSlotId,
    ) -> Option<run_state::BoxPlacement> {
        self.placed_box_slots.with_row(node.slot_index() as usize, |slot| {
            slot.filter(|slot| slot.builder == builder).map(|slot| slot.placement)
        })
    }

    pub(super) fn cover_slots(&mut self, slot_count: usize) {
        self.placed_box_slots.cover_slots(slot_count);
    }
}

fn take_spare_fragment_builder_buffers(worker: &super::layout_pass::WorkerScratch) -> FragmentBuilderBuffers {
    worker
        .spare_fragment_builder_buffers
        .borrow_mut()
        .pop()
        .unwrap_or_default()
}

fn return_spare_fragment_builder_buffers(worker: &super::layout_pass::WorkerScratch, buffers: FragmentBuilderBuffers) {
    worker.spare_fragment_builder_buffers.borrow_mut().push(buffers);
}

pub(crate) struct PlacementsOfBuilder<'scratch> {
    scratch: &'scratch FragmentBuilderScratch,
    builder: u64,
}

impl PlacementsOfBuilder<'_> {
    pub(crate) fn placement_of(&self, node: crate::layout::node_data::NodeSlotId) -> Option<run_state::BoxPlacement> {
        self.scratch.placement_by(self.builder, node)
    }

    pub(crate) fn is_placed(&self, node: crate::layout::node_data::NodeSlotId) -> bool {
        self.placement_of(node).is_some()
    }

    /// The physical size of the content box of `node`, where the builder placed the box and built its fragment.
    fn physical_content_size_of(&self, node: crate::layout::node_data::NodeSlotId) -> Option<FfiCssPixelSize> {
        self.scratch
            .placed_box_slots
            .with_row(node.slot_index() as usize, |slot| {
                slot.filter(|slot| slot.builder == self.builder)
                    .and_then(|slot| slot.physical_content_size)
            })
    }

    /// The containing block `node` is placed in, or for a box not placed in one yet, the containing block it is laid
    /// out in, within the run rooted at `run_root`.
    pub(crate) fn placement_containing_block(
        &self,
        pass: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
        run_root: crate::layout::node_data::NodeSlotId,
    ) -> crate::layout::node_data::NodeSlotId {
        match self.placement_of(node) {
            Some(placement) if !placement.containing_block.is_invalid() => placement.containing_block,
            _ => formatting_context::containing_block_laid_out_in(pass, node, run_root),
        }
    }

    /// Whether the placements of the boxes from the coordinate space box `space` up lead to `run_root`.
    fn chain_of_space_reaches(
        &self,
        mut space: crate::layout::node_data::NodeSlotId,
        run_root: crate::layout::node_data::NodeSlotId,
        pass: &LayoutPass<'_>,
    ) -> bool {
        while space != run_root {
            if !self.is_placed(space) {
                return false;
            }
            space = self.placement_containing_block(pass, space, run_root);
        }
        true
    }
}

/// The fragment the box of `candidate` committed, where `candidate` builds identically to it. A fragment that continues
/// in another fragment of its box matches none.
pub(crate) fn previously_committed_fragment_matching(
    pass: &LayoutPass<'_>,
    candidate: &Fragment,
) -> Option<std::sync::Arc<Fragment>> {
    previously_committed_fragment_where(pass, candidate, Fragment::builds_identically_to)
}

fn previously_committed_fragment_like(pass: &LayoutPass<'_>, candidate: &Fragment) -> Option<std::sync::Arc<Fragment>> {
    previously_committed_fragment_where(pass, candidate, Fragment::builds_like_apart_from_overflow)
}

fn previously_committed_fragment_where(
    pass: &LayoutPass<'_>,
    candidate: &Fragment,
    matches: impl FnOnce(&Fragment, &Fragment) -> bool,
) -> Option<std::sync::Arc<Fragment>> {
    if candidate.breaks.box_is_broken() || !pass.has_committed_fragment_link(candidate.node) {
        return None;
    }
    pass.arena()
        .with_committed_fragment_link(candidate.node, |previous_link| {
            previous_link
                .filter(|previous_link| matches(candidate, &previous_link.fragment))
                .map(|previous_link| previous_link.fragment.clone())
        })
}

/// What the placement of a box shows of the box's own layout, in the frame of the run that places it: the fragment shows
/// its sizes, borders, paddings and lines, and the link shows its margins.
#[derive(Clone)]
pub(crate) struct FragmentGeometry {
    pub(crate) cells: used_values::RootInputCells,
    pub(crate) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
}

impl FragmentGeometry {
    /// Whether `fragment` shows the box as these cells, which `frame` holds, and lines do.
    pub(crate) fn shows_box_of_fragment(&self, fragment: &Fragment, frame: geometry::Frame) -> bool {
        fragment.sides == self.cells.box_sides().in_frame(frame, geometry::Frame::PHYSICAL)
            && same_allocation(self.line_data.as_ref(), fragment.line_data.as_ref())
    }
}

/// How the physical offsets of boxes in a content box whose contents use `frame` move as the content box goes from
/// the physical size `from` to `to`: along each axis the frame reverses, they keep their distance from the far edge.
fn shift_of_contents(frame: geometry::Frame, from: FfiCssPixelSize, to: FfiCssPixelSize) -> FfiCssPixelPoint {
    FfiCssPixelPoint {
        x: if frame.vertical_axis_is_reversed() {
            to.width - from.width
        } else {
            CssPixels::default()
        },
        y: if frame.horizontal_axis_is_reversed() {
            to.height - from.height
        } else {
            CssPixels::default()
        },
    }
}

/// A fragment like `fragment` whose links of boxes contained by `containing_block` are moved by `shift`, and whose
/// outer links are then `outer_links`.
fn fragment_with_contained_links_moved(
    pass: &LayoutPass<'_>,
    fragment: &Fragment,
    containing_block: crate::layout::node_data::NodeSlotId,
    shift: FfiCssPixelPoint,
    outer_links: OuterLinks,
) -> std::sync::Arc<Fragment> {
    let children = fragment
        .children
        .iter()
        .map(|link| {
            let mut link = link.clone();
            if link.placement.containing_block == containing_block {
                link.offset = formatting_context::point_add(link.offset, shift);
            }
            link
        })
        .collect();
    let mut rebuilt = fragment.with_scrollable_overflow_and_children(fragment.scrollable_overflow, children);
    rebuilt.outer_links = outer_links;
    rebuilt_fragment(pass, rebuilt)
}

/// The links of the fragment `arrived` of `node`, for a fragment of the box at the physical content size
/// `content_size` whose contents its parent moves by the physical offset `contents_shift`. Beside that, the boxes the
/// box contains keep their offsets in the frame it lays out its contents in.
fn links_moved_to_content_size(
    pass: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    arrived: &Fragment,
    content_size: FfiCssPixelSize,
    contents_shift: FfiCssPixelPoint,
) -> Vec<FragmentLink> {
    let shift_of_box_sides = shift_of_contents(
        formatting_context::frame_established_by(pass, node),
        arrived.sides.content_size,
        content_size,
    );
    let shift = formatting_context::point_add(shift_of_box_sides, contents_shift);
    if shift == FfiCssPixelPoint::default() {
        return arrived.children.clone();
    }
    // NB: The rendered legend of a fieldset sits on the fieldset's border, which the contents shift leaves in place.
    let rendered_legend = NodeFacts::new(pass, node).rendered_legend();
    arrived
        .children
        .iter()
        .map(|link| {
            let mut link = link.clone();
            if link.fragment.node == rendered_legend {
                link.offset = formatting_context::point_add(link.offset, shift_of_box_sides);
            } else if link.placement.containing_block == node {
                link.offset = formatting_context::point_add(link.offset, shift);
            }
            if link.fragment.outer_links == OuterLinks::Placed && link.fragment.holds_links_contained_by(node) {
                link.fragment =
                    fragment_with_contained_links_moved(pass, &link.fragment, node, shift, OuterLinks::Placed);
            }
            link
        })
        .collect()
}

/// The links `children` of a fragment of `node` whose content box has the physical size `content_size`, where the
/// fragments of the boxes it contains place the links of its other boxes that awaited its size.
fn place_links_awaiting_size_of(
    pass: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    content_size: FfiCssPixelSize,
    mut children: Vec<FragmentLink>,
) -> Vec<FragmentLink> {
    for link in &mut children {
        if link.placement.containing_block != node
            || link.fragment.outer_links != OuterLinks::AwaitingContainingBlockSize
        {
            continue;
        }
        let shift = shift_of_contents(
            formatting_context::frame_established_by(pass, node),
            FfiCssPixelSize::default(),
            content_size,
        );
        link.fragment = fragment_with_contained_links_moved(pass, &link.fragment, node, shift, OuterLinks::Placed);
    }
    children
}

/// Builds the fragment of `node` around `children`, whose offsets are physical. Where `awaits_containing_block_size`,
/// the links it holds of boxes contained by its own containing block await that block's size.
fn snapshot_fragment(
    pass: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
    geometry: FragmentGeometry,
    breaks: BreakFacts,
    payload: PendingPayload,
    awaits_containing_block_size: bool,
) -> std::sync::Arc<Fragment> {
    let FragmentGeometry { cells, line_data } = geometry;
    let PendingPayload { payload, line_clamp } = payload;
    let sides = cells.box_sides().in_frame(pass.frame, geometry::Frame::PHYSICAL);
    let mut children = place_links_awaiting_size_of(pass, node, sides.content_size, children);
    if line_clamp.has_line_clamp_point && !breaks.continues_in_next {
        line_clamp_edge::clip_floats_at_block_end_content_edge(
            pass,
            node,
            formatting_context::frame_established_by(pass, node),
            sides.content_size,
            &mut children,
        );
    }
    let outer_links = if !fragment_overflow::holds_links_of_outer_containing_blocks(node, &children) {
        OuterLinks::None
    } else if awaits_containing_block_size {
        OuterLinks::AwaitingContainingBlockSize
    } else {
        OuterLinks::Placed
    };
    let mut fragment = Fragment {
        identity: 0,
        node,
        sides,
        line_data,
        payload: payload.filter(|payload| !payload.holds_nothing()),
        line_clamp,
        breaks,
        scrollable_overflow: Default::default(),
        outer_links,
        children,
    };
    fragment.scrollable_overflow = fragment_overflow::scrollable_overflow_of(
        pass.arena(),
        &fragment,
        fragment_overflow::contained_links(&fragment),
    );
    if let Some(previous) = previously_committed_fragment_like(pass, &fragment) {
        if previous.scrollable_overflow == fragment.scrollable_overflow {
            return previous;
        }
        return std::sync::Arc::new(previous.with_scrollable_overflow(fragment.scrollable_overflow));
    }
    fragment.identity = next_unique_identity(pass.worker_scratch());
    std::sync::Arc::new(fragment)
}

/// A fragment like `fragment` that holds `children` in place of its own: the committed fragment of the box where that
/// holds them the same way, or else a new one. It never takes the identity of `fragment`, which caches may hold.
pub(crate) fn fragment_with_children(
    pass: &LayoutPass<'_>,
    fragment: &Fragment,
    children: Vec<FragmentLink>,
) -> std::sync::Arc<Fragment> {
    rebuilt_fragment(
        pass,
        fragment.with_scrollable_overflow_and_children(fragment.scrollable_overflow, children),
    )
}

/// The committed fragment of the box of `rebuilt` where that builds identically to it, or else `rebuilt` with an
/// identity of its own.
fn rebuilt_fragment(pass: &LayoutPass<'_>, mut rebuilt: Fragment) -> std::sync::Arc<Fragment> {
    if let Some(previous) = previously_committed_fragment_matching(pass, &rebuilt) {
        return previous;
    }
    rebuilt.identity = next_unique_identity(pass.worker_scratch());
    std::sync::Arc::new(rebuilt)
}

/// The lines `lines` of `node` moved by `offset`: those the committed fragment of the box holds where they are the
/// same, so that moving the lines as the last layout of the box did keeps the box's fragment.
pub(crate) fn lines_translated_by(
    pass: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    lines: std::sync::Arc<inline_content::InlineContent>,
    offset: FfiCssPixelPoint,
) -> std::sync::Arc<inline_content::InlineContent> {
    let translated = inline_content::InlineContent::concatenate_translated(&[(lines, offset)]);
    if !pass.has_committed_fragment_link(node) {
        return translated;
    }
    pass.arena()
        .with_committed_fragment_link(node, |link| {
            link.and_then(|link| link.fragment.line_data.clone())
                .filter(|committed| **committed == *translated)
        })
        .unwrap_or(translated)
}

/// The fragment of a placed box: `arrived`, the fragment its own run built or the committed one, where that shows the
/// box as placed and nothing else was placed in it, or else a fragment of the box's `geometry` around the arrived
/// fragment's children, moved by `contents_shift`, a physical offset by which the box's parent moves its contents, and
/// the `children` placed in it here.
#[allow(clippy::too_many_arguments)]
fn fragment_of_placed_box(
    pass: &LayoutPass<'_>,
    node: crate::layout::node_data::NodeSlotId,
    arrived: Option<std::sync::Arc<Fragment>>,
    children: Vec<PendingLink>,
    geometry: FragmentGeometry,
    breaks: BreakFacts,
    payload: PendingPayload,
    contents_shift: FfiCssPixelPoint,
) -> std::sync::Arc<Fragment> {
    let content_size = geometry
        .cells
        .box_sides()
        .in_frame(pass.frame, geometry::Frame::PHYSICAL)
        .content_size;
    let placed_children = |children: Vec<PendingLink>| {
        children.into_iter().map(move |child| {
            debug_assert!(
                child.placement.containing_block == node,
                "a box holds the pending links of the boxes it contains"
            );
            child.into_link(content_size)
        })
    };
    let Some(arrived) = arrived else {
        return snapshot_fragment(
            pass,
            node,
            placed_children(children).collect(),
            geometry,
            breaks,
            payload,
            false,
        );
    };
    let shows_box_as_placed = children.is_empty()
        && contents_shift == FfiCssPixelPoint::default()
        && geometry.shows_box_of_fragment(&arrived, pass.frame)
        && arrived.breaks == breaks;
    if shows_box_as_placed {
        return arrived;
    }
    let mut all_children = links_moved_to_content_size(pass, node, &arrived, content_size, contents_shift);
    all_children.extend(placed_children(children));
    snapshot_fragment(
        pass,
        node,
        all_children,
        geometry,
        breaks,
        PendingPayload::of_fragment(&arrived),
        arrived.outer_links == OuterLinks::AwaitingContainingBlockSize,
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
/// fragments and what goes past the run. A normal run turns them into its root's fragment, a fragmentainer's run hands
/// them to the run of the box it belongs to, and a whole layout pass hands its links to commit.
pub(crate) struct RunContents {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) scoped_descendants: Vec<PendingLink>,
    pub(crate) holds_links_beside_their_sealed_containing_blocks: bool,
    pub(crate) payloads_past_run: PayloadsPastRun,
}

impl RunContents {
    /// The root's fragment around what the run laid out inside it, showing the root's own layout `geometry` and what
    /// the formatting context of the root handed it as `root_payload`.
    pub(crate) fn into_run_root_fragment(
        self,
        pass: &LayoutPass<'_>,
        geometry: FragmentGeometry,
        root_payload: PendingPayload,
    ) -> RunRootFragment {
        let Self {
            node,
            scoped_descendants,
            holds_links_beside_their_sealed_containing_blocks,
            payloads_past_run,
        } = self;
        let content_size = geometry
            .cells
            .box_sides()
            .in_frame(pass.frame, geometry::Frame::PHYSICAL)
            .content_size;
        let mut awaits_containing_block_size = false;
        let mut links: Vec<FragmentLink> = scoped_descendants
            .into_iter()
            .map(|link| {
                let containing_block_size = match link.containing_block_size {
                    Some(size) => size,
                    None if effective_containing_block(pass, &link) == node => content_size,
                    None => {
                        // NB: A table box holds the rows and cells its wrapper contains, whose fragment is built after
                        //     the table box's.
                        awaits_containing_block_size |= !link.placement.containing_block.is_invalid()
                            && (link.frame.vertical_axis_is_reversed() || link.frame.horizontal_axis_is_reversed());
                        FfiCssPixelSize::default()
                    }
                };
                link.into_link(containing_block_size)
            })
            .collect();
        if holds_links_beside_their_sealed_containing_blocks {
            fragment_overflow::include_links_held_beside_their_containing_blocks(pass.arena(), node, &mut links);
        }
        RunRootFragment::new(
            snapshot_fragment(
                pass,
                node,
                links,
                geometry,
                BreakFacts::default(),
                root_payload,
                awaits_containing_block_size,
            ),
            payloads_past_run,
        )
    }
}

/// The containing block of the box `link` places, or for a link without one, the containing block found among the
/// box's ancestors, as painting finds it.
fn effective_containing_block(pass: &LayoutPass<'_>, link: &PendingLink) -> crate::layout::node_data::NodeSlotId {
    let containing_block = link.placement.containing_block;
    if containing_block.is_invalid() {
        return pass.arena().containing_block_by_walking_ancestors(link.fragment.node);
    }
    containing_block
}

/// What a run hands its parent of its root: the root's fragment, and what goes past the run toward boxes outside it.
/// A box its parent takes as it was committed hands its committed fragment the same way, with nothing past it.
#[derive(Clone)]
#[must_use = "a child's fragments are lost unless its placement takes them"]
pub(crate) struct RunRootFragment {
    pub(crate) fragment: std::sync::Arc<Fragment>,
    pub(crate) payloads_past_run: Option<Box<PayloadsPastRun>>,
}

static NO_PAYLOADS_PAST_RUN: PayloadsPastRun = PayloadsPastRun {
    pending_abspos: Vec::new(),
    anchor_candidates: Vec::new(),
    inline_containing_block_rects: Vec::new(),
    abspos_containing_block_info: Vec::new(),
};

impl RunRootFragment {
    pub(crate) fn new(fragment: std::sync::Arc<Fragment>, payloads_past_run: PayloadsPastRun) -> Self {
        Self {
            fragment,
            payloads_past_run: (!payloads_past_run.is_empty()).then(|| Box::new(payloads_past_run)),
        }
    }

    /// The root fragment of a run that hands nothing past `fragment`.
    pub(crate) fn propagating_nothing(fragment: std::sync::Arc<Fragment>) -> Self {
        Self {
            fragment,
            payloads_past_run: None,
        }
    }

    pub(crate) fn payloads_past_run(&self) -> &PayloadsPastRun {
        self.payloads_past_run.as_deref().unwrap_or(&NO_PAYLOADS_PAST_RUN)
    }

    /// The root fragment of a run whose root's contents its parent moves by `offset`, in the frame the root lays out
    /// its contents in, where what the run hands past the root moves with them. The links and lines of the root's
    /// fragment move as the root's placement builds its fragment (see fragment_of_placed_box()).
    pub(crate) fn translated_by(mut self, offset: FfiCssPixelPoint) -> Self {
        let root = self.fragment.node;
        if let Some(payloads) = self.payloads_past_run.as_deref_mut() {
            translate_payloads_in_space_of(&mut payloads.pending_abspos, root, offset);
            translate_payloads_in_space_of(&mut payloads.anchor_candidates, root, offset);
            translate_payloads_in_space_of(&mut payloads.inline_containing_block_rects, root, offset);
        }
        self
    }
}

#[derive(Clone, Copy)]
pub(crate) struct SlotFragment<'tree> {
    pub(crate) link: &'tree FragmentLink,
    pub(crate) containing_block_fragment: Option<u32>,
    pub(crate) containing_block_line_index_offset: usize,
}

pub(crate) type SlotFragments<'tree> = SmallVec<[SlotFragment<'tree>; 1]>;

pub(crate) struct CompletedPassFragments {
    pub(crate) roots: Vec<FragmentLink>,
}

struct PendingFragment {
    node: crate::layout::node_data::NodeSlotId,
    /// The fragment the box's layout handed its placement: the root fragment of its own run, or its committed fragment.
    arrived: Option<std::sync::Arc<Fragment>>,
    children: Vec<PendingLink>,
    pending_abspos: Vec<abspos_inputs::PendingAbsposChild>,
    anchor_candidates: Vec<AnchorCandidate>,
    inline_containing_block_rects: Vec<InlineContainingBlockRect>,
    payload: PendingPayload,
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
            payload: PendingPayload::default(),
        }
    }
}

pub(crate) struct RunFragmentBuilder {
    root_node: crate::layout::node_data::NodeSlotId,
    root_containing_block_slot: Option<u32>,
    is_entry_accumulator: bool,
    builder: u64,
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
    top_scope_links: Vec<PendingLink>,
    /// What the layouts of children the run laid out but never placed handed their placements, which close() links at
    /// the top of the run's fragments.
    children_left_unplaced: Vec<ChildLeftUnplaced>,
    /// Where the run placed each box it placed so far, from the moment it places the box, before the placement drains
    /// the absolutely positioned boxes the box contains.
    placed_boxes: PlacedBoxes,
    holds_links_beside_their_sealed_containing_blocks: bool,
    holds_pending_abspos_below_root: bool,
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
        pass: &LayoutPass<'_>,
        root_node: crate::layout::node_data::NodeSlotId,
        root_containing_block: Option<crate::layout::node_data::NodeSlotId>,
    ) -> Self {
        Self::with_spare_buffers(
            pass.worker_scratch(),
            root_node,
            root_containing_block.map(|node| node.slot_index()),
            false,
        )
    }

    pub(crate) fn new_entry_accumulator(
        pass: &LayoutPass<'_>,
        root_node: crate::layout::node_data::NodeSlotId,
    ) -> Self {
        Self::with_spare_buffers(pass.worker_scratch(), root_node, None, true)
    }

    fn with_spare_buffers(
        worker: &super::layout_pass::WorkerScratch,
        root_node: crate::layout::node_data::NodeSlotId,
        root_containing_block_slot: Option<u32>,
        is_entry_accumulator: bool,
    ) -> Self {
        let builder = next_unique_identity(worker);
        let FragmentBuilderBuffers {
            pending_fragments,
            placed_boxes,
        } = take_spare_fragment_builder_buffers(worker);
        Self {
            root_node,
            root_containing_block_slot,
            is_entry_accumulator,
            builder,
            inner: std::cell::RefCell::new(RunFragmentBuilderInner {
                pending_fragments,
                placed_boxes,
                ..RunFragmentBuilderInner::default()
            }),
        }
    }

    fn with_placements<R>(&self, pass: &LayoutPass<'_>, read: impl FnOnce(&PlacementsOfBuilder<'_>) -> R) -> R {
        read(&PlacementsOfBuilder {
            scratch: pass.fragment_builder_scratch(),
            builder: self.builder,
        })
    }

    pub(crate) fn root_node(&self) -> crate::layout::node_data::NodeSlotId {
        self.root_node
    }

    /// Notes that the run placed `node` in `placement`, which is final, with the sizes and box sides
    /// `cells_for_contained_abspos` of a box that may be the containing block of absolutely positioned boxes.
    pub(crate) fn note_placed_box(
        &self,
        pass: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
        placement: run_state::BoxPlacement,
        cells_for_contained_abspos: Option<CellsInFrame>,
    ) {
        let was_placed = pass.fragment_builder_scratch().place(self.builder, node, placement);
        debug_assert!(!was_placed, "a box is placed once");
        let placed_boxes = &mut self.inner.borrow_mut().placed_boxes;
        placed_boxes.placed_slots.push(node.slot_index());
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
    pub(crate) fn placement_of(
        &self,
        pass: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
    ) -> Option<run_state::BoxPlacement> {
        self.with_placements(pass, |placements| placements.placement_of(node))
    }

    /// The containing block `node` is placed in, or for a box the run did not place yet, the containing block it is
    /// laid out in.
    pub(crate) fn placement_containing_block(
        &self,
        pass: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
    ) -> crate::layout::node_data::NodeSlotId {
        self.with_placements(pass, |placements| {
            placements.placement_containing_block(pass, node, self.root_node)
        })
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
            Some(pending_fragment) => {
                pending_fragment.pending_abspos.push(entry);
                inner.holds_pending_abspos_below_root = true;
            }
            None => {
                if self.is_entry_accumulator {
                    inner.pending_abspos_at_root.push(entry);
                } else {
                    let mut pending_fragment = PendingFragment::new(coordinate_space_box);
                    pending_fragment.pending_abspos.push(entry);
                    inner
                        .pending_fragments
                        .insert(coordinate_space_box.slot_index(), pending_fragment);
                    inner.holds_pending_abspos_below_root = true;
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
        pass: &LayoutPass<'_>,
    ) -> Vec<abspos_inputs::PendingAbsposChild> {
        let mut inner = self.inner.borrow_mut();
        let RunFragmentBuilderInner {
            pending_fragments,
            pending_abspos_at_root,
            holds_pending_abspos_below_root,
            ..
        } = &mut *inner;
        if pending_abspos_at_root.is_empty() && !*holds_pending_abspos_below_root {
            return Vec::new();
        }
        let mut taken = self.with_placements(pass, |placements| {
            let containing_block_is_owned_and_placed = |entry: &abspos_inputs::PendingAbsposChild| {
                let containing_block = entry.containing_block();
                !containing_block.is_invalid()
                    && (self.is_entry_accumulator || containing_block != self.root_node)
                    && placements.is_placed(containing_block)
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
            taken
        });
        sort_in_tree_order(&mut taken, pass);
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
    /// knows, in tree order. Those inside a child the run left unplaced stay for the run's parent, as do those whose
    /// positions stand in a space beyond the root, which only the placement of the root relates to the root: a table
    /// places its cells in the space of its wrapper.
    pub(crate) fn take_abspos_contained_by_root(
        &self,
        pass: &LayoutPass<'_>,
    ) -> Vec<abspos_inputs::PendingAbsposChild> {
        let root = self.root_node;
        let mut inner = self.inner.borrow_mut();
        let mut taken: Vec<abspos_inputs::PendingAbsposChild> = self.with_placements(pass, |placements| {
            inner
                .pending_abspos_at_root
                .extract_if(.., |entry| {
                    entry.containing_block() == root
                        && placements.chain_of_space_reaches(entry.coordinate_space_box, root, pass)
                })
                .collect()
        });
        sort_in_tree_order(&mut taken, pass);
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

    /// Whether what the run laid out rests where the placement of its root finds it, for a run whose parent places its
    /// root. A child it left unplaced or a box beside its sealed containing block belongs at the top of the fragments of
    /// the run its root is in, and only placing a box in this builder lays out an absolutely positioned box whose
    /// containing block this builder placed.
    pub(crate) fn hands_everything_to_the_placement_of_its_root(&self, pass: &LayoutPass<'_>) -> bool {
        let inner = self.inner.borrow();
        inner.children_left_unplaced.is_empty()
            && !inner.holds_links_beside_their_sealed_containing_blocks
            && self.with_placements(pass, |placements| {
                !inner.iter_pending_abspos().any(|entry| {
                    let containing_block = entry.containing_block();
                    !containing_block.is_invalid() && placements.is_placed(containing_block)
                })
            })
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
        inner.holds_links_beside_their_sealed_containing_blocks |=
            contents.holds_links_beside_their_sealed_containing_blocks;
        for mut link in contents.scoped_descendants {
            link.offset_in_frame = formatting_context::point_add(link.offset_in_frame, offset);
            if link.placement.containing_block == self.root_node
                && let Some(line_index) = link.placement.containing_line_box_index.as_mut()
            {
                *line_index += u32::try_from(root_line_index_offset).expect("a line box index fits in 32 bits");
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

    pub(crate) fn translate_root_contents(&self, pass: &LayoutPass<'_>, offset: FfiCssPixelPoint) {
        let root = self.root_node;
        let mut inner = self.inner.borrow_mut();
        for link in inner
            .top_scope_links
            .iter_mut()
            .filter(|link| link.placement.containing_block == root)
        {
            link.offset_in_frame = formatting_context::point_add(link.offset_in_frame, offset);
        }
        {
            let scratch = pass.fragment_builder_scratch();
            for &slot in &inner.placed_boxes.placed_slots {
                scratch.placed_box_slots.with_row_mut(slot as usize, |placed| {
                    debug_assert!(
                        placed.builder == self.builder,
                        "a run's placements stay with its builder"
                    );
                    if placed.placement.containing_block == root {
                        placed.placement.content_offset_in_containing_block =
                            formatting_context::point_add(placed.placement.content_offset_in_containing_block, offset);
                    }
                });
            }
        }
        translate_payloads_in_space_of(&mut inner.pending_abspos_at_root, root, offset);
        translate_payloads_in_space_of(&mut inner.anchor_candidates_at_root, root, offset);
        translate_payloads_in_space_of(&mut inner.inline_containing_block_rects_at_root, root, offset);
    }

    /// Takes in the root fragment of the run that laid out `node`, which is being placed, and what goes past that run.
    pub(crate) fn take_in_child_root_at_placement(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        root: RunRootFragment,
        pass: &LayoutPass<'_>,
    ) {
        self.merge_child_root_into_pending_fragment(node, root, pass);
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
        let arrived_payload = fragment_arrival.map(|root| PendingPayload::of_fragment(&root.fragment));
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let pending_payload = inner
            .pending_fragments
            .remove(&slot)
            .map(|pending_fragment| pending_fragment.payload)
            .filter(|payload| !payload.is_empty());
        if let Some(payload_of_box) = arrived_payload.or(pending_payload) {
            inner.pending_fragments.insert(
                slot,
                PendingFragment {
                    payload: payload_of_box,
                    ..PendingFragment::new(node)
                },
            );
        }
    }

    /// Hands what the layout of `node` produced besides its geometry to the box's fragment.
    pub(crate) fn update_fragment_payload_of(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        update: impl FnOnce(&mut PendingPayload),
    ) {
        debug_assert!(
            node != self.root_node || self.is_entry_accumulator,
            "a run's root takes its payload from its formatting context"
        );
        let mut inner = self.inner.borrow_mut();
        update(
            &mut inner
                .pending_fragments
                .entry(node.slot_index())
                .or_insert_with(|| PendingFragment::new(node))
                .payload,
        );
    }

    fn merge_child_root_into_pending_fragment(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        root: RunRootFragment,
        pass: &LayoutPass<'_>,
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
        let Some(payloads_past_run) = root.payloads_past_run else {
            return;
        };
        let PayloadsPastRun {
            pending_abspos,
            anchor_candidates,
            inline_containing_block_rects,
            abspos_containing_block_info,
        } = *payloads_past_run;
        let receives_pending_abspos = !pending_abspos.is_empty();
        pending_fragment
            .pending_abspos
            .extend(pending_abspos.into_iter().map(|mut entry| {
                formatting_context::resolve_pending_abspos_containing_block(pass, &mut entry, self.root_node);
                entry
            }));
        pending_fragment.anchor_candidates.extend(anchor_candidates);
        pending_fragment
            .inline_containing_block_rects
            .extend(inline_containing_block_rects);
        inner.receive_abspos_containing_block_info(abspos_containing_block_info);
        inner.holds_pending_abspos_below_root |= receives_pending_abspos;
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn build_fragment_for_placed_box(
        &self,
        pass: &LayoutPass<'_>,
        node: crate::layout::node_data::NodeSlotId,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
        geometry: FragmentGeometry,
        facts: run_state::FactsDecidedByParent,
        abspos_layout_inputs: Option<std::sync::Arc<abspos_inputs::AbsposLayoutInputs>>,
        containing_line_box_index: Option<u32>,
        offset_in_frame: FfiCssPixelPoint,
        own_anchor_candidate_border_box_rect: Option<CssPixelRect>,
        physical_contents_shift: FfiCssPixelPoint,
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
            payload,
        } = inner
            .pending_fragments
            .remove(&slot)
            .unwrap_or_else(|| PendingFragment::new(node));
        let content_size = (geometry.cells.content_inline_size, geometry.cells.content_block_size);
        let physical_content_size = pass.frame.size_to_physical(content_size.0, content_size.1);
        pass.fragment_builder_scratch()
            .note_physical_content_size(self.builder, node, physical_content_size);
        let link_placement = LinkPlacement::of_box(
            &facts,
            &geometry.cells,
            abspos_layout_inputs,
            containing_line_box_index,
            pass.frame,
        );
        let fragment = fragment_of_placed_box(
            pass,
            node,
            arrived,
            children,
            geometry,
            facts.fragment_facts.breaks,
            payload,
            physical_contents_shift,
        );
        let link = PendingLink {
            fragment: fragment.clone(),
            offset_in_frame,
            frame: pass.frame,
            placement: link_placement,
            containing_block_size: None,
        };
        self.attach(pass, &mut inner, link, containing_block);
        let exports_payloads = !pending_abspos_from_placed_box.is_empty()
            || !anchor_candidates_from_placed_box.is_empty()
            || !inline_containing_block_rects_from_placed_box.is_empty()
            || own_anchor_candidate_border_box_rect.is_some();
        let content_offset = placement.content_offset_in_containing_block;
        let frames = exports_payloads
            .then(|| {
                let own = formatting_context::frame_established_by(pass, node);
                (own != pass.frame).then_some(PlacedBoxFrames {
                    own,
                    containing_block: pass.frame,
                    physical_content_size,
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
                Some(pending_fragment) => {
                    pending_fragment.pending_abspos.push(entry);
                    inner.holds_pending_abspos_below_root = true;
                }
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
        pass: &LayoutPass<'_>,
        inner: &mut RunFragmentBuilderInner,
        link: PendingLink,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
    ) {
        let Some(containing_block) = containing_block else {
            self.push_to_top_scope(pass, inner, link);
            return;
        };
        if containing_block == self.root_node || Some(containing_block.slot_index()) == self.root_containing_block_slot
        {
            self.push_to_top_scope(pass, inner, link);
            return;
        }
        if let Some(pending_fragment) = inner.pending_fragments.get_mut(&containing_block.slot_index()) {
            pending_fragment.children.push(link);
            return;
        }
        // NB: A containing block the run placed already is sealed, so the box goes to the top of the run's fragments.
        if self.is_entry_accumulator || self.with_placements(pass, |placements| placements.is_placed(containing_block))
        {
            inner.holds_links_beside_their_sealed_containing_blocks |= !self.is_entry_accumulator;
            self.push_to_top_scope(pass, inner, link);
            return;
        }
        let mut pending_fragment = PendingFragment::new(containing_block);
        pending_fragment.children.push(link);
        inner
            .pending_fragments
            .insert(containing_block.slot_index(), pending_fragment);
    }

    /// Puts `link` at the top of the run's fragments, with the physical content size of its containing block where the
    /// run placed that block already.
    fn push_to_top_scope(&self, pass: &LayoutPass<'_>, inner: &mut RunFragmentBuilderInner, mut link: PendingLink) {
        if link.containing_block_size.is_none() {
            let containing_block = effective_containing_block(pass, &link);
            link.containing_block_size =
                self.with_placements(pass, |placements| placements.physical_content_size_of(containing_block));
        }
        inner.top_scope_links.push(link);
    }

    pub(crate) fn take_run_contents(&self, pass: &LayoutPass<'_>) -> RunContents {
        debug_assert!(
            !self.is_entry_accumulator,
            "an entry accumulator closes as a pass, not a run"
        );
        self.close(pass)
    }

    pub(crate) fn take_completed_pass(&self, pass: &LayoutPass<'_>) -> CompletedPassFragments {
        debug_assert!(
            self.is_entry_accumulator,
            "an ordinary run closes into the contents of its root"
        );
        let root = self.close(pass);
        // NB: A link at the top of a pass whose containing block the pass did not place, such as the containing block of
        //     a partial relayout's root, keeps the containing block it committed.
        CompletedPassFragments {
            roots: root
                .scoped_descendants
                .into_iter()
                .map(|link| {
                    let containing_block_size = link
                        .containing_block_size
                        .unwrap_or_else(|| committed_content_size_of(pass, effective_containing_block(pass, &link)));
                    link.into_link(containing_block_size)
                })
                .collect(),
        }
    }

    fn close(&self, pass: &LayoutPass<'_>) -> RunContents {
        let mut inner = self.inner.take();
        let mut payloads_past_run = PayloadsPastRun {
            pending_abspos: std::mem::take(&mut inner.pending_abspos_at_root),
            anchor_candidates: std::mem::take(&mut inner.anchor_candidates_at_root),
            inline_containing_block_rects: std::mem::take(&mut inner.inline_containing_block_rects_at_root),
            abspos_containing_block_info: std::mem::take(&mut inner.abspos_containing_block_info_contributions),
        };
        let mut pending_fragments = std::mem::take(&mut inner.pending_fragments);
        for (_, pending_fragment) in pending_fragments.drain() {
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
            let facts = unplaced_child.facts_decided_by_parent;
            let placement = LinkPlacement::of_box(&facts, &unplaced_child.geometry.cells, None, None, pass.frame);
            let link = PendingLink {
                fragment: fragment_of_placed_box(
                    pass,
                    node,
                    Some(root.fragment),
                    Vec::new(),
                    unplaced_child.geometry,
                    facts.fragment_facts.breaks,
                    PendingPayload::default(),
                    FfiCssPixelPoint::default(),
                ),
                offset_in_frame: FfiCssPixelPoint::default(),
                frame: pass.frame,
                placement,
                containing_block_size: None,
            };
            self.push_to_top_scope(pass, &mut inner, link);
        }
        self.with_placements(pass, |placements| {
            for entry in &mut payloads_past_run.pending_abspos {
                debug_assert!(
                    {
                        let containing_block = entry.containing_block();
                        containing_block.is_invalid()
                            || containing_block == self.root_node
                            || !placements.is_placed(containing_block)
                    },
                    "a drainable abspos registration survived its containing block's run (slot {})",
                    entry.child_box.slot_index()
                );
                propagate_payload_toward_run_root_space(entry, self.root_node, placements, pass);
            }
            for candidate in &mut payloads_past_run.anchor_candidates {
                propagate_payload_toward_run_root_space(candidate, self.root_node, placements, pass);
            }
            for payload in &mut payloads_past_run.inline_containing_block_rects {
                propagate_payload_toward_run_root_space(payload, self.root_node, placements, pass);
            }
        });
        let mut placed_boxes = std::mem::take(&mut inner.placed_boxes);
        placed_boxes.placed_slots.clear();
        placed_boxes.cells_of_abspos_containing_blocks.clear();
        return_spare_fragment_builder_buffers(
            pass.worker_scratch(),
            FragmentBuilderBuffers {
                pending_fragments,
                placed_boxes,
            },
        );
        RunContents {
            node: self.root_node,
            scoped_descendants: inner.top_scope_links,
            holds_links_beside_their_sealed_containing_blocks: inner.holds_links_beside_their_sealed_containing_blocks,
            payloads_past_run,
        }
    }
}

// A pass's fragments are an immutable publication that later passes and paintable rows share.
const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<CompletedPassFragments>();
};
