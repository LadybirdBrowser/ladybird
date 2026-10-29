/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub use crate::css::css_pixels::{FfiCssPixelPoint, FfiCssPixelRect, FfiCssPixelSize};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct OptionalCssPixels {
    pub value: CssPixels,
    pub has_value: bool,
}

impl From<Option<CssPixels>> for OptionalCssPixels {
    fn from(value: Option<CssPixels>) -> Self {
        Self {
            value: value.unwrap_or_default(),
            has_value: value.is_some(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct OptionalCssPixelRect {
    pub value: FfiCssPixelRect,
    pub has_value: bool,
}

impl From<Option<FfiCssPixelRect>> for OptionalCssPixelRect {
    fn from(value: Option<FfiCssPixelRect>) -> Self {
        Self {
            value: value.unwrap_or_default(),
            has_value: value.is_some(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct OptionalIntRect {
    pub value: libgfx_rust::IntRect,
    pub has_value: bool,
}

impl From<Option<libgfx_rust::IntRect>> for OptionalIntRect {
    fn from(value: Option<libgfx_rust::IntRect>) -> Self {
        Self {
            value: value.unwrap_or_default(),
            has_value: value.is_some(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct LineBoxFragmentCoordinate {
    pub line_box_index: usize,
}

#[derive(Clone, Default, PartialEq)]
pub(crate) struct LineData {
    pub(crate) line_boxes: Vec<line_box::LineBoxData>,
    pub(crate) inline_box_pieces: Vec<inline_formatting_context::InlineBoxPieceData>,
    pub(crate) lines_after_clamp_point_rect: Option<Box<FfiCssPixelRect>>,
}

#[derive(Default)]
pub(crate) enum LineDataState {
    #[default]
    Empty,
    Building(Box<LineData>),
    Finished(std::sync::Arc<inline_content::InlineContent>),
}

impl LineDataState {
    /// The lines as inline content laid out at `content_inline_size` in `writing_mode`. Finishes lines still being
    /// built and keeps the result, so later calls return the same content.
    pub(crate) fn finish(
        &mut self,
        arena: &LayoutNodeArena,
        content_inline_size: CssPixels,
        writing_mode: u8,
    ) -> Option<std::sync::Arc<inline_content::InlineContent>> {
        let content = match self {
            Self::Empty => return None,
            Self::Finished(content) => return Some(content.clone()),
            Self::Building(data) => std::sync::Arc::new(inline_content::InlineContent::finish(
                std::mem::take(data.as_mut()),
                arena,
                content_inline_size,
                writing_mode,
            )),
        };
        *self = Self::Finished(content.clone());
        Some(content)
    }

    pub(crate) fn lines(&self) -> impl DoubleEndedIterator<Item = inline_content::LineRecord> + '_ {
        let (building, finished) = match self {
            Self::Empty => (&[][..], &[][..]),
            Self::Building(data) => (data.line_boxes.as_slice(), &[][..]),
            Self::Finished(data) => (&[][..], data.lines.as_slice()),
        };
        building
            .iter()
            .map(line_box::LineBoxData::retained_metrics)
            .chain(finished.iter().copied())
    }

    pub(crate) fn last_line(&self) -> Option<inline_content::LineRecord> {
        self.lines().next_back()
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct CommittedSvgFacts {
    pub(crate) viewport_transform: Option<svg_formatting_context::FfiAffineTransform>,
    pub(crate) viewport_size: Option<FfiCssPixelSize>,
    pub(crate) view_box: Option<svg_formatting_context::FfiSvgViewBox>,
    pub(crate) element_transform: Option<svg_formatting_context::FfiAffineTransform>,
    pub(crate) additional_element_transform: Option<svg_formatting_context::FfiAffineTransform>,
    pub(crate) mask_area_facts: Option<svg_formatting_context::SvgMaskAreaFacts>,
    pub(crate) viewport_percentage_basis: CssPixels,
    pub(crate) resource_content_units_are_object_bounding_box: bool,
}

/// The sizes, size constraints and box sides of a box.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct RootInputCells {
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

    pub(crate) has_definite_inline_size: bool,
    pub(crate) has_definite_block_size: bool,
    /// A button, or the anonymous wrapper of its content, whose automatic block size was made definite only so the
    /// content can be centered within a larger min-height. Percentages inside the button still treat it as indefinite.
    pub(crate) has_definite_block_size_only_for_button_content_alignment: bool,
    pub(crate) uses_collapsing_borders_model: bool,
    /// In the collapsing borders model, whether this is the table box rather than a cell. Both store the full widths
    /// of the collapsed borders at their edges, of which only a part lies inside the box (see border_left_collapsed()
    /// and friends): the table's border area lies outside the grid, so it owns the part of an outer border on the far
    /// side of the first or last grid line, while a cell owns the part on the inner side of its grid lines.
    pub(crate) is_collapsed_borders_table_box: bool,
}

impl RootInputCells {
    /// Resolves the left and right margins, borders and padding of `style`, with percentages against `basis`.
    pub(crate) fn resolve_horizontal_box_model(&mut self, style: &StyleValues<'_>, basis: CssPixels) {
        self.margin_left = style.margin_left().to_px(basis);
        self.margin_right = style.margin_right().to_px(basis);
        self.border_left = style.border_left_width();
        self.border_right = style.border_right_width();
        self.padding_left = style.padding_left().to_px(basis);
        self.padding_right = style.padding_right().to_px(basis);
    }

    /// Resolves the top and bottom margins, borders and padding of `style`, with percentages against `basis`.
    pub(crate) fn resolve_vertical_box_model(&mut self, style: &StyleValues<'_>, basis: CssPixels) {
        self.margin_top = style.margin_top().to_px(basis);
        self.margin_bottom = style.margin_bottom().to_px(basis);
        self.border_top = style.border_top_width();
        self.border_bottom = style.border_bottom_width();
        self.padding_top = style.padding_top().to_px(basis);
        self.padding_bottom = style.padding_bottom().to_px(basis);
    }

    pub(crate) fn set_content_inline_size(&mut self, value: CssPixels) {
        // Negative inline sizes are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        self.content_inline_size = clamp_to_max_dimension_value(value.max(CssPixels::default()));
        self.has_definite_inline_size = true;
    }

    pub(crate) fn set_content_block_size(&mut self, value: CssPixels) {
        // Negative block sizes are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        self.content_block_size = clamp_to_max_dimension_value(value.max(CssPixels::default()));
    }

    /// Resolves every margin, border and padding of `style`, with percentages against `basis`.
    pub(crate) fn resolve_box_model(&mut self, style: &StyleValues<'_>, basis: CssPixels) {
        self.resolve_horizontal_box_model(style, basis);
        self.resolve_vertical_box_model(style, basis);
    }

    /// The sizes and box sides `fragment` shows, with both sizes definite.
    pub(crate) fn of_committed_fragment(fragment: &fragment_tree::Fragment) -> Self {
        let mut cells = Self {
            has_definite_block_size: true,
            ..Self::default()
        };
        cells.take_box_metrics_of_fragment(fragment);
        cells
    }

    /// Takes the sizes and box sides `fragment` shows.
    pub(crate) fn take_box_metrics_of_fragment(&mut self, fragment: &fragment_tree::Fragment) {
        self.set_content_inline_size(fragment.content_inline_size);
        self.set_content_block_size(fragment.content_block_size);
        self.margin_left = fragment.margin_left;
        self.margin_right = fragment.margin_right;
        self.margin_top = fragment.margin_top;
        self.margin_bottom = fragment.margin_bottom;
        self.border_left = fragment.border_left;
        self.border_right = fragment.border_right;
        self.border_top = fragment.border_top;
        self.border_bottom = fragment.border_bottom;
        self.padding_left = fragment.padding_left;
        self.padding_right = fragment.padding_right;
        self.padding_top = fragment.padding_top;
        self.padding_bottom = fragment.padding_bottom;
    }

    /// The cells, with the sizes, size constraints and box sides they hold in frame `from` read in frame `to` instead.
    #[inline]
    pub(crate) fn in_frame(mut self, from: geometry::Frame, to: geometry::Frame) -> Self {
        if from == to {
            return self;
        }
        [self.margin_top, self.margin_right, self.margin_bottom, self.margin_left] = from.sides_in(
            [self.margin_top, self.margin_right, self.margin_bottom, self.margin_left],
            to,
        );
        [self.border_top, self.border_right, self.border_bottom, self.border_left] = from.sides_in(
            [self.border_top, self.border_right, self.border_bottom, self.border_left],
            to,
        );
        [
            self.padding_top,
            self.padding_right,
            self.padding_bottom,
            self.padding_left,
        ] = from.sides_in(
            [
                self.padding_top,
                self.padding_right,
                self.padding_bottom,
                self.padding_left,
            ],
            to,
        );
        if from.is_orthogonal_to(to) {
            std::mem::swap(&mut self.content_inline_size, &mut self.content_block_size);
            std::mem::swap(&mut self.has_definite_inline_size, &mut self.has_definite_block_size);
        }
        self
    }
}

macro_rules! box_metric_getters {
    ($($name:ident: $value:ty,)+) => {
        $(
            #[inline]
            fn $name(&self) -> $value {
                self.cells().$name
            }
        )+
    };
}

/// The sizes, size constraints and box sides of a box, with what derives from them, whether a cell holds them or they
/// are read as a value.
pub(crate) trait BoxMetricCells {
    /// The sizes, size constraints and box sides these metrics hold.
    fn cells(&self) -> RootInputCells;

    box_metric_getters! {
        content_inline_size: CssPixels,
        content_block_size: CssPixels,
        margin_left: CssPixels,
        margin_right: CssPixels,
        margin_top: CssPixels,
        margin_bottom: CssPixels,
        border_left: CssPixels,
        border_right: CssPixels,
        border_top: CssPixels,
        border_bottom: CssPixels,
        padding_left: CssPixels,
        padding_right: CssPixels,
        padding_top: CssPixels,
        padding_bottom: CssPixels,
        has_definite_inline_size: bool,
        has_definite_block_size: bool,
        has_definite_block_size_only_for_button_content_alignment: bool,
        uses_collapsing_borders_model: bool,
        is_collapsed_borders_table_box: bool,
    }

    fn border_left_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            collapsed_border_share(self.border_left(), true, self.is_collapsed_borders_table_box())
        } else {
            self.border_left()
        }
    }

    fn border_right_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            collapsed_border_share(self.border_right(), false, self.is_collapsed_borders_table_box())
        } else {
            self.border_right()
        }
    }

    fn border_top_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            collapsed_border_share(self.border_top(), true, self.is_collapsed_borders_table_box())
        } else {
            self.border_top()
        }
    }

    fn border_bottom_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            collapsed_border_share(self.border_bottom(), false, self.is_collapsed_borders_table_box())
        } else {
            self.border_bottom()
        }
    }

    fn border_box_left(&self, collapsed: bool) -> CssPixels {
        self.border_left_collapsed(collapsed) + self.padding_left()
    }

    fn border_box_right(&self, collapsed: bool) -> CssPixels {
        self.border_right_collapsed(collapsed) + self.padding_right()
    }

    fn border_box_top(&self, collapsed: bool) -> CssPixels {
        self.border_top_collapsed(collapsed) + self.padding_top()
    }

    fn border_box_bottom(&self, collapsed: bool) -> CssPixels {
        self.border_bottom_collapsed(collapsed) + self.padding_bottom()
    }

    fn horizontal_margin_border_padding(&self) -> CssPixels {
        self.margin_left()
            + self.border_left()
            + self.padding_left()
            + self.padding_right()
            + self.border_right()
            + self.margin_right()
    }

    fn border_box_inline_size(&self, collapsed: bool) -> CssPixels {
        self.border_box_left(collapsed) + self.content_inline_size() + self.border_box_right(collapsed)
    }

    fn border_box_block_size(&self, collapsed: bool) -> CssPixels {
        self.border_box_top(collapsed) + self.content_block_size() + self.border_box_bottom(collapsed)
    }

    fn margin_box_bottom(&self, collapsed: bool) -> CssPixels {
        self.margin_bottom() + self.border_box_bottom(collapsed)
    }

    fn margin_box_top(&self, collapsed: bool) -> CssPixels {
        self.margin_top() + self.border_box_top(collapsed)
    }

    fn margin_box_inline_size(&self, collapsed: bool) -> CssPixels {
        self.margin_left()
            + self.border_box_left(collapsed)
            + self.content_inline_size()
            + self.border_box_right(collapsed)
            + self.margin_right()
    }

    fn margin_box_block_size(&self, collapsed: bool) -> CssPixels {
        self.margin_top()
            + self.border_box_top(collapsed)
            + self.content_block_size()
            + self.margin_box_bottom(collapsed)
    }

    fn available_inner_space_or_constraints_from(&self, outer: AvailableSpace) -> AvailableSpace {
        let mut inline_size = if self.has_definite_inline_size() {
            AvailableSize::definite(self.content_inline_size())
        } else {
            AvailableSize::Indefinite
        };
        let block_size = if self.has_definite_block_size() {
            AvailableSize::definite(self.content_block_size())
        } else {
            AvailableSize::Indefinite
        };
        if inline_size == AvailableSize::Indefinite
            && matches!(outer.inline_size, AvailableSize::MinContent | AvailableSize::MaxContent)
        {
            inline_size = outer.inline_size;
        }
        AvailableSpace {
            inline_size,
            block_size,
        }
    }
}

impl BoxMetricCells for RootInputCells {
    #[inline]
    fn cells(&self) -> RootInputCells {
        *self
    }
}

impl BoxMetricCells for Cell<RootInputCells> {
    #[inline]
    fn cells(&self) -> RootInputCells {
        self.get()
    }
}

/// Writes the metrics a cell holds. Not `update`: the inherent `Cell::update` would shadow it.
pub(crate) trait BoxMetricCellsMut {
    /// Writes the metrics, which `write` sees and changes as the cell holds them.
    fn update_cells(&self, write: impl FnOnce(&mut RootInputCells));

    fn set_content_inline_size(&self, value: CssPixels) {
        self.update_cells(|cells| cells.set_content_inline_size(value));
    }

    fn set_content_block_size(&self, value: CssPixels) {
        self.update_cells(|cells| cells.set_content_block_size(value));
    }
}

impl BoxMetricCellsMut for Cell<RootInputCells> {
    fn update_cells(&self, write: impl FnOnce(&mut RootInputCells)) {
        let mut cells = self.get();
        write(&mut cells);
        self.set(cells);
    }
}

/// The part of a collapsed border that lies after its grid line (below or right of it), the rest lying before it.
/// A collapsed border "is centered on the grid line" (CSS 2.2 §17.6.2), but layout keeps box edges on whole CSS
/// pixels, so the border is split on whole pixels, an odd width giving its extra pixel to the part before the line.
/// Every box on a grid line splits the border the same way, so the shares of the boxes on both sides of the line add
/// up to the border width: a 1px border takes 1px of layout space, not a rounded-up half on each side.
/// https://www.w3.org/TR/CSS22/tables.html#collapsing-borders
pub(crate) fn collapsed_border_part_after_line(width: CssPixels) -> CssPixels {
    if width <= CssPixels::default() {
        return CssPixels::default();
    }
    (width / 2usize).floor()
}

/// The part of a collapsed border that lies before its grid line, see collapsed_border_part_after_line().
pub(crate) fn collapsed_border_part_before_line(width: CssPixels) -> CssPixels {
    if width <= CssPixels::default() {
        return CssPixels::default();
    }
    width - collapsed_border_part_after_line(width)
}

/// The part of a collapsed border of the given width that lies inside a box on the border's grid line; `start_edge`
/// says whether the line is at the box's top or left edge rather than its bottom or right one. A cell lies inside its
/// grid lines, so it owns the part after the line at its start edges and the part before the line at its end edges;
/// the table box lies around the grid and owns the opposite parts of its outer borders.
pub(crate) fn collapsed_border_share(width: CssPixels, start_edge: bool, is_table_box: bool) -> CssPixels {
    if start_edge != is_table_box {
        collapsed_border_part_after_line(width)
    } else {
        collapsed_border_part_before_line(width)
    }
}

/// The input cells of `node` before anything sizes the box: the sizes its style makes definite without layout.
pub(crate) fn cells_before_sizing(
    callbacks: &LayoutPass<'_>,
    node: Node,
    constraints: ContainingBlockConstraints,
) -> RootInputCells {
    assert!(!node.is_invalid());
    let facts = NodeFacts::new(callbacks, node);

    let style = StyleValues::for_node(callbacks, node);
    let percentage_basis_inline_size = constraints.percentage_basis_inline_size;
    let percentage_basis_block_size = constraints.percentage_basis_block_size;

    // NOTE: In the code below, we decide if `node` has definite inline
    // and/or block size. This attempts to cover all the *general* cases
    // where CSS considers sizes to be definite. If `node` has definite
    // values for min/max-width or min/max-height and a definite preferred
    // size in the same axis, we clamp the preferred size here as well.
    //
    // There are additional cases where CSS considers values to be
    // definite. We model all of those by considering sizes definite once
    // they are assigned through set_content_inline_size() or
    // set_content_block_size().
    let mut cells = RootInputCells::default();

    #[derive(Clone, Copy)]
    enum Axis {
        Inline,
        Block,
    }

    let containing_block_size_for_axis = |axis: Axis| match axis {
        Axis::Inline => percentage_basis_inline_size.unwrap_or_default(),
        Axis::Block => percentage_basis_block_size.unwrap_or_default(),
    };
    let containing_block_has_definite_size = |axis: Axis| match axis {
        Axis::Inline => percentage_basis_inline_size.is_some(),
        Axis::Block => percentage_basis_block_size.is_some(),
    };

    let adjust_for_box_sizing = |unadjusted: crate::layout::CssPixels, computed_size: &ComputedSize, axis: Axis| {
        // box-sizing: content-box and automatic sizes need no
        // adjustment.
        if style.box_sizing() == box_sizing::CONTENT_BOX || computed_size.is_auto() {
            return unadjusted;
        }

        // box-sizing: border-box subtracts the relevant border and
        // padding. Block-axis padding percentages also resolve against
        // the containing block's inline size.
        let inline_basis = percentage_basis_inline_size.unwrap_or_default();
        let border_and_padding = match axis {
            Axis::Inline => {
                style.border_left_width()
                    + style.padding_left().to_px(inline_basis)
                    + style.border_right_width()
                    + style.padding_right().to_px(inline_basis)
            }
            Axis::Block => {
                style.border_top_width()
                    + style.padding_top().to_px(inline_basis)
                    + style.border_bottom_width()
                    + style.padding_bottom().to_px(inline_basis)
            }
        };
        unadjusted - border_and_padding
    };

    let is_definite_size = |size: &ComputedSize, axis: Axis| -> Option<crate::layout::CssPixels> {
        // A definite size can be determined without performing
        // layout: a length, an initial-containing-block size, or a
        // percentage/formula resolved solely against definite sizes.
        if size.is_auto() {
            // The inline size of a non-flex-item block is definite when
            // it is auto and its containing block has a definite inline
            // size. This is the stretch-fit case from css-sizing-3.
            // Replaced boxes remain content-based until layout.
            if matches!(axis, Axis::Inline)
                && !facts.is_replaced_box()
                && !facts.is_floating()
                && !facts.is_absolutely_positioned()
                && facts.display().is_block_outside()
                && facts.parent_is_unfloated_flow_container()
                && containing_block_has_definite_size(Axis::Inline)
                && !formatting_context::frame_established_by(callbacks, node).is_orthogonal_to(callbacks.frame)
            {
                let available = containing_block_size_for_axis(Axis::Inline);
                return Some(clamp_to_max_dimension_value(
                    available - cells.horizontal_margin_border_padding(),
                ));
            }
            return None;
        }

        // https://drafts.csswg.org/css-sizing-3/#stretch-fit-sizing
        // NB: A stretch size is definite where percentages resolve to a definite value, and sizes the margin box to
        //     fill the containing block exactly, treating auto margins as zero.
        if size.is_stretch() && size.calc_size().is_none() {
            if !containing_block_has_definite_size(axis) {
                return None;
            }
            let inline_basis = percentage_basis_inline_size.unwrap_or_default();
            let margins_borders_and_padding = match axis {
                Axis::Inline => {
                    style.margin_left().to_px(inline_basis)
                        + style.border_left_width()
                        + style.padding_left().to_px(inline_basis)
                        + style.padding_right().to_px(inline_basis)
                        + style.border_right_width()
                        + style.margin_right().to_px(inline_basis)
                }
                Axis::Block => {
                    let (margin_top, margin_bottom) =
                        crate::layout::sizing_context::stretch_fit_block_margins(callbacks, node, inline_basis);
                    margin_top
                        + style.border_top_width()
                        + style.padding_top().to_px(inline_basis)
                        + style.padding_bottom().to_px(inline_basis)
                        + style.border_bottom_width()
                        + margin_bottom
                }
            };
            return Some(clamp_to_max_dimension_value(
                (containing_block_size_for_axis(axis) - margins_borders_and_padding)
                    .max(crate::layout::CssPixels::default()),
            ));
        }
        if !size.is_length_percentage() {
            return None;
        }
        if size.contains_percentage() && !containing_block_has_definite_size(axis) {
            return None;
        }
        let basis = if size.contains_percentage() {
            containing_block_size_for_axis(axis)
        } else {
            crate::layout::CssPixels::default()
        };
        Some(clamp_to_max_dimension_value(adjust_for_box_sizing(
            size.to_px(basis),
            size,
            axis,
        )))
    };

    let min_inline_size = is_definite_size(style.min_width(), Axis::Inline);
    let max_inline_size = is_definite_size(style.max_width(), Axis::Inline);
    let min_block_size = is_definite_size(style.min_height(), Axis::Block);
    let max_block_size = is_definite_size(style.max_height(), Axis::Block);
    let mut content_inline_size = is_definite_size(style.width(), Axis::Inline);
    let mut content_block_size = is_definite_size(style.height(), Axis::Block);

    cells.has_definite_inline_size = content_inline_size.is_some();
    cells.has_definite_block_size = content_block_size.is_some();
    if let Some(size) = content_inline_size.as_mut() {
        if let Some(minimum) = min_inline_size {
            *size = clamp_to_max_dimension_value((*size).max(minimum));
        }
        if let Some(maximum) = max_inline_size {
            *size = clamp_to_max_dimension_value((*size).min(maximum));
        }
    }
    if let Some(size) = content_block_size.as_mut() {
        if let Some(minimum) = min_block_size {
            *size = clamp_to_max_dimension_value((*size).max(minimum));
        }
        if let Some(maximum) = max_block_size {
            *size = clamp_to_max_dimension_value((*size).min(maximum));
        }
    }
    cells.content_inline_size = content_inline_size.unwrap_or_default();
    cells.content_block_size = content_block_size.unwrap_or_default();

    cells
}

/// The input cells of a box and what its parent decided about it, as its committed fragment link has them in the frame
/// of the link, which a run adopts as they are.
pub(crate) struct CommittedBox {
    pub(crate) cells: RootInputCells,
    pub(crate) facts_decided_by_parent: super::run_state::FactsDecidedByParent,
}

pub(crate) fn committed_box_from_fragment_link(callbacks: &LayoutPass<'_>, node: Node) -> Option<CommittedBox> {
    let link = callbacks.committed_fragment_link(node)?;
    let fragment = &link.fragment;

    Some(CommittedBox {
        cells: RootInputCells::of_committed_fragment(fragment),
        facts_decided_by_parent: super::run_state::FactsDecidedByParent {
            placement: Some(super::run_state::BoxPlacement {
                content_offset_in_containing_block: link.committed_offset,
                containing_block: link.containing_block,
            }),
            used_insets: super::run_state::UsedInsets {
                left: link.inset_left,
                right: link.inset_right,
                top: link.inset_top,
                bottom: link.inset_bottom,
            },
            default_scroll_shift: None,
            fragment_facts: super::run_state::FragmentFactsDecidedByParent {
                table_column_index: fragment.table_column_index,
                table_column_span: fragment.table_column_span,
                hidden_by_collapsed_columns: fragment.hidden_by_collapsed_columns,
                ..super::run_state::FragmentFactsDecidedByParent::default()
            },
        },
    })
}
