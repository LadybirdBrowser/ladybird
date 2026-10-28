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
    pub fragment_index: usize,
}

pub(crate) struct SealableCell<T> {
    value: Cell<T>,
    sealed: Cell<bool>,
}

impl<T: Copy + std::fmt::Debug> std::fmt::Debug for SealableCell<T> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.value.get().fmt(formatter)
    }
}

impl<T: Copy> SealableCell<T> {
    pub(crate) fn new(value: T) -> Self {
        Self {
            value: Cell::new(value),
            sealed: Cell::new(false),
        }
    }

    #[inline]
    pub(crate) fn get(&self) -> T {
        self.value.get()
    }

    #[track_caller]
    #[inline]
    pub(crate) fn set(&self, value: T) {
        assert!(!self.sealed.get(), "write to a sealed committed box metric");
        self.value.set(value);
    }

    pub(crate) fn seal(&self) {
        self.sealed.set(true);
    }
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

impl std::fmt::Debug for LineDataState {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            Self::Empty => "Empty",
            Self::Building(_) => "Building",
            Self::Finished(_) => "Finished",
        })
    }
}

impl LineDataState {
    pub(crate) fn building_mut(&mut self) -> &mut LineData {
        match self {
            Self::Building(data) => data,
            Self::Empty => panic!("line building mutated line data before it began"),
            Self::Finished(_) => panic!("line building mutated finalized inline content"),
        }
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

/// The per-box geometry stored in a Rust-owned layout pass.
#[derive(Debug)]
pub(crate) struct UsedValues {
    pub content_inline_size: SealableCell<CssPixels>,
    pub content_block_size: SealableCell<CssPixels>,

    pub margin_left: SealableCell<CssPixels>,
    pub margin_right: SealableCell<CssPixels>,
    pub margin_top: SealableCell<CssPixels>,
    pub margin_bottom: SealableCell<CssPixels>,

    pub border_left: SealableCell<CssPixels>,
    pub border_right: SealableCell<CssPixels>,
    pub border_top: SealableCell<CssPixels>,
    pub border_bottom: SealableCell<CssPixels>,

    pub padding_left: SealableCell<CssPixels>,
    pub padding_right: SealableCell<CssPixels>,
    pub padding_top: SealableCell<CssPixels>,
    pub padding_bottom: SealableCell<CssPixels>,

    pub has_definite_inline_size: Cell<bool>,
    pub has_definite_block_size: Cell<bool>,
    /// A button, or the anonymous wrapper of its content, whose automatic block size was made definite only so the
    /// content can be centered within a larger min-height. Percentages inside the button still treat it as indefinite.
    pub has_definite_block_size_only_for_button_content_alignment: Cell<bool>,
    pub uses_collapsing_borders_model: Cell<bool>,
    /// In the collapsing borders model, whether this is the table box rather than a cell. Both store the full widths
    /// of the collapsed borders at their edges, of which only a part lies inside the box (see border_left_collapsed()
    /// and friends): the table's border area lies outside the grid, so it owns the part of an outer border on the far
    /// side of the first or last grid line, while a cell owns the part on the inner side of its grid lines.
    pub is_collapsed_borders_table_box: Cell<bool>,

    // Keep baseline payloads separate so resetting the presence bits does not
    // perturb the payloads observed by the existing derivation flow.
    pub has_first_baseline: Cell<bool>,
    pub first_baseline: Cell<CssPixels>,
    pub has_last_baseline: Cell<bool>,
    pub last_baseline: Cell<CssPixels>,
    pub inline_alignment_baseline: Cell<Option<CssPixels>>,

    pub(crate) line_data: RefCell<LineDataState>,
    /// The frame the box's sizes, size constraints and box sides are held in.
    pub(crate) frame: Cell<geometry::Frame>,
    /// The anchor-size() resolutions of an absolutely positioned box, set once as the abspos engine places it.
    pub(crate) anchor_resolved_values: std::cell::OnceCell<Box<style_values::AnchorResolvedValues>>,
}

impl Default for UsedValues {
    fn default() -> Self {
        let zero = CssPixels::from_raw(0);
        Self {
            content_inline_size: SealableCell::new(zero),
            content_block_size: SealableCell::new(zero),
            margin_left: SealableCell::new(zero),
            margin_right: SealableCell::new(zero),
            margin_top: SealableCell::new(zero),
            margin_bottom: SealableCell::new(zero),
            border_left: SealableCell::new(zero),
            border_right: SealableCell::new(zero),
            border_top: SealableCell::new(zero),
            border_bottom: SealableCell::new(zero),
            padding_left: SealableCell::new(zero),
            padding_right: SealableCell::new(zero),
            padding_top: SealableCell::new(zero),
            padding_bottom: SealableCell::new(zero),
            has_definite_inline_size: Cell::new(false),
            has_definite_block_size: Cell::new(false),
            has_definite_block_size_only_for_button_content_alignment: Cell::new(false),
            uses_collapsing_borders_model: Cell::new(false),
            is_collapsed_borders_table_box: Cell::new(false),
            has_first_baseline: Cell::new(false),
            first_baseline: Cell::new(zero),
            has_last_baseline: Cell::new(false),
            last_baseline: Cell::new(zero),
            inline_alignment_baseline: Cell::new(None),
            line_data: RefCell::new(LineDataState::Empty),
            frame: Cell::new(geometry::Frame::PHYSICAL),
            anchor_resolved_values: std::cell::OnceCell::new(),
        }
    }
}

impl UsedValues {
    /// Resolves every margin, border and padding of `style`, with percentages against `basis`.
    pub(crate) fn resolve_box_model(&self, style: &StyleValues<'_>, basis: CssPixels) {
        RecordInFrame::in_its_own_frame(self).resolve_box_model(style, basis);
    }

    pub(crate) fn line_data_ref(&self) -> Option<Ref<'_, LineDataState>> {
        let state = self.line_data.borrow();
        (!matches!(*state, LineDataState::Empty)).then_some(state)
    }

    pub(crate) fn ensure_line_data(&self) {
        let mut state = self.line_data.borrow_mut();
        if matches!(*state, LineDataState::Empty) {
            *state = LineDataState::Building(Box::default());
        }
    }

    /// Takes the lines built in this record so far, for an inline formatting context to build on.
    pub(crate) fn take_building_line_data(&self) -> LineData {
        let mut data = RefMut::map(self.line_data.borrow_mut(), LineDataState::building_mut);
        std::mem::take(&mut *data)
    }

    /// Keeps the lines an inline formatting context built in this record.
    pub(crate) fn set_building_line_data(&self, data: LineData) {
        *RefMut::map(self.line_data.borrow_mut(), LineDataState::building_mut) = data;
    }

    pub(crate) fn set_finished_line_data(&self, content: std::sync::Arc<inline_content::InlineContent>) {
        *self.line_data.borrow_mut() = LineDataState::Finished(content);
    }

    pub(crate) fn finish_line_data(
        &self,
        callbacks: &LayoutPass<'_>,
    ) -> Option<std::sync::Arc<inline_content::InlineContent>> {
        let mut state = self.line_data.borrow_mut();
        let content = match &mut *state {
            LineDataState::Empty => return None,
            LineDataState::Finished(content) => return Some(content.clone()),
            LineDataState::Building(data) => std::sync::Arc::new(inline_content::InlineContent::finish(
                std::mem::take(data.as_mut()),
                callbacks.arena(),
                self.content_inline_size.get(),
                self.frame.get().writing_mode(),
            )),
        };
        *state = LineDataState::Finished(content.clone());
        Some(content)
    }

    pub(crate) fn content_baselines_from_cells(&self) -> DerivedBaselines {
        DerivedBaselines {
            inline_alignment: self.inline_alignment_baseline.get(),
            first: self.has_first_baseline.get().then(|| self.first_baseline.get()),
            last: self.has_last_baseline.get().then(|| self.last_baseline.get()),
        }
    }

    pub(crate) fn seal_own_metrics(&self) {
        self.content_inline_size.seal();
        self.content_block_size.seal();
        self.margin_left.seal();
        self.margin_right.seal();
        self.margin_top.seal();
        self.margin_bottom.seal();
        self.border_left.seal();
        self.border_right.seal();
        self.border_top.seal();
        self.border_bottom.seal();
        self.padding_left.seal();
        self.padding_right.seal();
        self.padding_top.seal();
        self.padding_bottom.seal();
    }
}

macro_rules! used_values_cells {
    ($name:ident { $($field:ident: $type:ty,)+ }) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub(crate) struct $name {
            $(pub(crate) $field: $type,)+
        }

        impl $name {
            pub(crate) fn capture(used: &UsedValues) -> Self {
                Self {
                    $($field: used.$field.get(),)+
                }
            }

            pub(crate) fn apply_to_record(&self, used: &UsedValues) {
                $(
                    if used.$field.get() != self.$field {
                        used.$field.set(self.$field);
                    }
                )+
            }
        }
    };
}

used_values_cells! {
    RootInputCells {
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
}

impl RootInputCells {
    /// The input cells of `node` as its record starts out, before anything sizes the box.
    pub(crate) fn of_box_before_sizing(
        callbacks: &LayoutPass<'_>,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> Self {
        Self::capture(&create_used_values(callbacks, node, constraints))
    }

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
        self.content_inline_size = clamp_to_max_dimension_value(value.max(CssPixels::default()));
        self.has_definite_inline_size = true;
    }

    pub(crate) fn set_content_block_size(&mut self, value: CssPixels) {
        self.content_block_size = clamp_to_max_dimension_value(value.max(CssPixels::default()));
    }

    /// A record holding the cells, which hold the box's metrics in `frame`.
    pub(crate) fn materialize_record(&self, frame: geometry::Frame) -> UsedValues {
        let record = UsedValues::default();
        record.frame.set(frame);
        self.apply_to_record(&record);
        record
    }

    /// The cells, with the sizes, size constraints and box sides they hold in frame `from` read in frame `to` instead.
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct UsedValuesCellState {
    pub(crate) inputs: RootInputCells,
    pub(crate) baselines: DerivedBaselines,
}

impl UsedValuesCellState {
    pub(crate) fn capture(used: &UsedValues) -> Self {
        Self {
            inputs: RootInputCells::capture(used),
            baselines: used.content_baselines_from_cells(),
        }
    }

    pub(crate) fn apply_to_record(&self, used: &UsedValues) {
        self.inputs.apply_to_record(used);
        formatting_context::store_derived_baselines(used, self.baselines);
    }
}

impl UsedValues {
    pub(crate) fn set_content_inline_size(&self, value: CssPixels) {
        // Negative inline sizes are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        self.content_inline_size
            .set(clamp_to_max_dimension_value(value.max(CssPixels::default())));
        self.has_definite_inline_size.set(true);
    }

    pub(crate) fn set_content_block_size(&self, value: CssPixels) {
        // Negative block sizes are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        self.content_block_size
            .set(clamp_to_max_dimension_value(value.max(CssPixels::default())));
    }

    fn margin_cell(&self, side: geometry::Side) -> &SealableCell<CssPixels> {
        match side {
            geometry::Side::Top => &self.margin_top,
            geometry::Side::Right => &self.margin_right,
            geometry::Side::Bottom => &self.margin_bottom,
            geometry::Side::Left => &self.margin_left,
        }
    }

    fn border_cell(&self, side: geometry::Side) -> &SealableCell<CssPixels> {
        match side {
            geometry::Side::Top => &self.border_top,
            geometry::Side::Right => &self.border_right,
            geometry::Side::Bottom => &self.border_bottom,
            geometry::Side::Left => &self.border_left,
        }
    }

    fn padding_cell(&self, side: geometry::Side) -> &SealableCell<CssPixels> {
        match side {
            geometry::Side::Top => &self.padding_top,
            geometry::Side::Right => &self.padding_right,
            geometry::Side::Bottom => &self.padding_bottom,
            geometry::Side::Left => &self.padding_left,
        }
    }

    #[track_caller]
    pub(crate) fn set_margin(&self, side: geometry::Side, value: CssPixels) {
        self.margin_cell(side).set(value);
    }

    #[track_caller]
    pub(crate) fn set_border(&self, side: geometry::Side, value: CssPixels) {
        self.border_cell(side).set(value);
    }

    #[track_caller]
    pub(crate) fn set_padding(&self, side: geometry::Side, value: CssPixels) {
        self.padding_cell(side).set(value);
    }

    pub(crate) fn set_has_definite_inline_size(&self, value: bool) {
        self.has_definite_inline_size.set(value);
    }

    pub(crate) fn set_has_definite_block_size(&self, value: bool) {
        self.has_definite_block_size.set(value);
    }

    pub(crate) fn set_has_definite_block_size_only_for_button_content_alignment(&self, value: bool) {
        self.has_definite_block_size_only_for_button_content_alignment
            .set(value);
    }

    pub(crate) fn set_box_metrics_from_fragment(&self, fragment: &fragment_tree::Fragment) {
        RecordInFrame::in_its_own_frame(self).set_box_metrics_from_fragment(fragment);
    }

    /// Whether `fragment` shows this record's box as it stands: its metrics, and the very line data it holds.
    pub(crate) fn shows_box_of_fragment(&self, fragment: &fragment_tree::Fragment) -> bool {
        let same_line_data = match (&fragment.line_data, &*self.line_data.borrow()) {
            (None, LineDataState::Empty) => true,
            (Some(left), LineDataState::Finished(right)) => std::sync::Arc::ptr_eq(left, right),
            _ => false,
        };
        same_line_data
            && fragment.content_inline_size == self.content_inline_size.get()
            && fragment.content_block_size == self.content_block_size.get()
            && fragment.margin_left == self.margin_left.get()
            && fragment.margin_right == self.margin_right.get()
            && fragment.margin_top == self.margin_top.get()
            && fragment.margin_bottom == self.margin_bottom.get()
            && fragment.border_left == self.border_left.get()
            && fragment.border_right == self.border_right.get()
            && fragment.border_top == self.border_top.get()
            && fragment.border_bottom == self.border_bottom.get()
            && fragment.padding_left == self.padding_left.get()
            && fragment.padding_right == self.padding_right.get()
            && fragment.padding_top == self.padding_top.get()
            && fragment.padding_bottom == self.padding_bottom.get()
            && fragment.uses_collapsing_borders_model == self.uses_collapsing_borders_model.get()
    }
}

/// The sizes, size constraints and box sides of a box, with what derives from them, whether a run holds them in the box's
/// record or its parent holds them as a value.
pub(crate) trait BoxMetricCells {
    fn content_inline_size(&self) -> CssPixels;
    fn content_block_size(&self) -> CssPixels;
    fn margin_left(&self) -> CssPixels;
    fn margin_right(&self) -> CssPixels;
    fn margin_top(&self) -> CssPixels;
    fn margin_bottom(&self) -> CssPixels;
    fn border_left(&self) -> CssPixels;
    fn border_right(&self) -> CssPixels;
    fn border_top(&self) -> CssPixels;
    fn border_bottom(&self) -> CssPixels;
    fn padding_left(&self) -> CssPixels;
    fn padding_right(&self) -> CssPixels;
    fn padding_top(&self) -> CssPixels;
    fn padding_bottom(&self) -> CssPixels;
    fn has_definite_inline_size(&self) -> bool;
    fn has_definite_block_size(&self) -> bool;
    fn has_definite_block_size_only_for_button_content_alignment(&self) -> bool;
    fn uses_collapsing_borders_model(&self) -> bool;
    fn is_collapsed_borders_table_box(&self) -> bool;

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

macro_rules! impl_box_metric_cells {
    ($type:ty, $read:ident) => {
        impl BoxMetricCells for $type {
            impl_box_metric_cells!(@getters $read;
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
            );
        }
    };
    (@getters cell; $($name:ident: $value:ty,)+) => {
        $(
            #[inline]
            fn $name(&self) -> $value {
                self.$name.get()
            }
        )+
    };
    (@getters value; $($name:ident: $value:ty,)+) => {
        $(
            #[inline]
            fn $name(&self) -> $value {
                self.$name
            }
        )+
    };
}

impl_box_metric_cells!(UsedValues, cell);
impl_box_metric_cells!(RootInputCells, value);

/// A record as a reader in frame `reader` sees it: the sizes, size constraints and box sides the record holds in its own
/// frame, read and written in the reader's frame.
#[derive(Clone, Copy)]
pub(crate) struct RecordInFrame<'record> {
    record: &'record UsedValues,
    reader: geometry::Frame,
}

impl<'record> RecordInFrame<'record> {
    /// The record, read in frame `reader`.
    #[inline]
    pub(crate) fn new(record: &'record UsedValues, reader: geometry::Frame) -> Self {
        Self { record, reader }
    }

    /// The record, read in the frame it holds its metrics in.
    #[inline]
    pub(crate) fn in_its_own_frame(record: &'record UsedValues) -> Self {
        Self {
            record,
            reader: record.frame.get(),
        }
    }

    /// The record, for what it holds besides the metrics this view projects.
    #[inline]
    pub(crate) fn record(self) -> &'record UsedValues {
        self.record
    }

    #[inline]
    fn sizes_swap_axes(self) -> bool {
        self.record.frame.get().is_orthogonal_to(self.reader)
    }

    /// The side the record holds the given side of the reader's frame on.
    #[inline]
    fn stored_side(self, side: geometry::Side) -> geometry::Side {
        let record_frame = self.record.frame.get();
        if record_frame == self.reader {
            return side;
        }
        self.reader.side_in(side, record_frame)
    }

    /// The record's input cells, read in the reader's frame.
    pub(crate) fn cells(self) -> RootInputCells {
        RootInputCells::capture(self.record).in_frame(self.record.frame.get(), self.reader)
    }

    pub(crate) fn set_content_inline_size(self, value: CssPixels) {
        if self.sizes_swap_axes() {
            self.record.set_content_block_size(value);
            self.record.set_has_definite_block_size(true);
        } else {
            self.record.set_content_inline_size(value);
        }
    }

    pub(crate) fn set_content_block_size(self, value: CssPixels) {
        if self.sizes_swap_axes() {
            self.record
                .content_inline_size
                .set(clamp_to_max_dimension_value(value.max(CssPixels::default())));
        } else {
            self.record.set_content_block_size(value);
        }
    }

    #[track_caller]
    pub(crate) fn set_margin(self, side: geometry::Side, value: CssPixels) {
        self.record.set_margin(self.stored_side(side), value);
    }

    #[track_caller]
    pub(crate) fn set_border(self, side: geometry::Side, value: CssPixels) {
        self.record.set_border(self.stored_side(side), value);
    }

    #[track_caller]
    pub(crate) fn set_padding(self, side: geometry::Side, value: CssPixels) {
        self.record.set_padding(self.stored_side(side), value);
    }

    pub(crate) fn set_has_definite_inline_size(self, value: bool) {
        if self.sizes_swap_axes() {
            self.record.set_has_definite_block_size(value);
        } else {
            self.record.set_has_definite_inline_size(value);
        }
    }

    pub(crate) fn set_has_definite_block_size(self, value: bool) {
        if self.sizes_swap_axes() {
            self.record.set_has_definite_inline_size(value);
        } else {
            self.record.set_has_definite_block_size(value);
        }
    }

    pub(crate) fn set_has_definite_block_size_only_for_button_content_alignment(self, value: bool) {
        self.record
            .set_has_definite_block_size_only_for_button_content_alignment(value);
    }

    /// Resolves every margin, border and padding of `style`, with percentages against `basis`.
    pub(crate) fn resolve_box_model(self, style: &StyleValues<'_>, basis: CssPixels) {
        self.set_margin(geometry::Side::Left, style.margin_left().to_px(basis));
        self.set_margin(geometry::Side::Right, style.margin_right().to_px(basis));
        self.set_border(geometry::Side::Left, style.border_left_width());
        self.set_border(geometry::Side::Right, style.border_right_width());
        self.set_padding(geometry::Side::Left, style.padding_left().to_px(basis));
        self.set_padding(geometry::Side::Right, style.padding_right().to_px(basis));
        self.set_margin(geometry::Side::Top, style.margin_top().to_px(basis));
        self.set_margin(geometry::Side::Bottom, style.margin_bottom().to_px(basis));
        self.set_border(geometry::Side::Top, style.border_top_width());
        self.set_border(geometry::Side::Bottom, style.border_bottom_width());
        self.set_padding(geometry::Side::Top, style.padding_top().to_px(basis));
        self.set_padding(geometry::Side::Bottom, style.padding_bottom().to_px(basis));
    }

    pub(crate) fn set_box_metrics_from_fragment(self, fragment: &fragment_tree::Fragment) {
        self.set_content_inline_size(fragment.content_inline_size);
        self.set_content_block_size(fragment.content_block_size);
        self.set_margin(geometry::Side::Left, fragment.margin_left);
        self.set_margin(geometry::Side::Right, fragment.margin_right);
        self.set_margin(geometry::Side::Top, fragment.margin_top);
        self.set_margin(geometry::Side::Bottom, fragment.margin_bottom);
        self.set_border(geometry::Side::Left, fragment.border_left);
        self.set_border(geometry::Side::Right, fragment.border_right);
        self.set_border(geometry::Side::Top, fragment.border_top);
        self.set_border(geometry::Side::Bottom, fragment.border_bottom);
        self.set_padding(geometry::Side::Left, fragment.padding_left);
        self.set_padding(geometry::Side::Right, fragment.padding_right);
        self.set_padding(geometry::Side::Top, fragment.padding_top);
        self.set_padding(geometry::Side::Bottom, fragment.padding_bottom);
    }
}

impl BoxMetricCells for RecordInFrame<'_> {
    #[inline]
    fn content_inline_size(&self) -> CssPixels {
        if self.sizes_swap_axes() {
            self.record.content_block_size.get()
        } else {
            self.record.content_inline_size.get()
        }
    }

    #[inline]
    fn content_block_size(&self) -> CssPixels {
        if self.sizes_swap_axes() {
            self.record.content_inline_size.get()
        } else {
            self.record.content_block_size.get()
        }
    }

    #[inline]
    fn margin_left(&self) -> CssPixels {
        self.record.margin_cell(self.stored_side(geometry::Side::Left)).get()
    }

    #[inline]
    fn margin_right(&self) -> CssPixels {
        self.record.margin_cell(self.stored_side(geometry::Side::Right)).get()
    }

    #[inline]
    fn margin_top(&self) -> CssPixels {
        self.record.margin_cell(self.stored_side(geometry::Side::Top)).get()
    }

    #[inline]
    fn margin_bottom(&self) -> CssPixels {
        self.record.margin_cell(self.stored_side(geometry::Side::Bottom)).get()
    }

    #[inline]
    fn border_left(&self) -> CssPixels {
        self.record.border_cell(self.stored_side(geometry::Side::Left)).get()
    }

    #[inline]
    fn border_right(&self) -> CssPixels {
        self.record.border_cell(self.stored_side(geometry::Side::Right)).get()
    }

    #[inline]
    fn border_top(&self) -> CssPixels {
        self.record.border_cell(self.stored_side(geometry::Side::Top)).get()
    }

    #[inline]
    fn border_bottom(&self) -> CssPixels {
        self.record.border_cell(self.stored_side(geometry::Side::Bottom)).get()
    }

    #[inline]
    fn padding_left(&self) -> CssPixels {
        self.record.padding_cell(self.stored_side(geometry::Side::Left)).get()
    }

    #[inline]
    fn padding_right(&self) -> CssPixels {
        self.record.padding_cell(self.stored_side(geometry::Side::Right)).get()
    }

    #[inline]
    fn padding_top(&self) -> CssPixels {
        self.record.padding_cell(self.stored_side(geometry::Side::Top)).get()
    }

    #[inline]
    fn padding_bottom(&self) -> CssPixels {
        self.record.padding_cell(self.stored_side(geometry::Side::Bottom)).get()
    }

    #[inline]
    fn has_definite_inline_size(&self) -> bool {
        if self.sizes_swap_axes() {
            self.record.has_definite_block_size.get()
        } else {
            self.record.has_definite_inline_size.get()
        }
    }

    #[inline]
    fn has_definite_block_size(&self) -> bool {
        if self.sizes_swap_axes() {
            self.record.has_definite_inline_size.get()
        } else {
            self.record.has_definite_block_size.get()
        }
    }

    #[inline]
    fn has_definite_block_size_only_for_button_content_alignment(&self) -> bool {
        self.record
            .has_definite_block_size_only_for_button_content_alignment
            .get()
    }

    #[inline]
    fn uses_collapsing_borders_model(&self) -> bool {
        self.record.uses_collapsing_borders_model.get()
    }

    #[inline]
    fn is_collapsed_borders_table_box(&self) -> bool {
        self.record.is_collapsed_borders_table_box.get()
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

pub(crate) fn create_used_values(
    callbacks: &LayoutPass<'_>,
    node: Node,
    constraints: ContainingBlockConstraints,
) -> UsedValues {
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
    let used = UsedValues::default();
    used.frame.set(callbacks.frame);

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
                    available - used.horizontal_margin_border_padding(),
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

    used.has_definite_inline_size.set(content_inline_size.is_some());
    used.has_definite_block_size.set(content_block_size.is_some());
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
    used.content_inline_size.set(content_inline_size.unwrap_or_default());
    used.content_block_size.set(content_block_size.unwrap_or_default());

    used
}

/// The record of a box and what its parent decided about it, as its committed fragment link has them, which a run
/// adopts as they are.
pub(crate) struct CommittedBox {
    pub(crate) record: UsedValues,
    pub(crate) facts_decided_by_parent: super::run_records::FactsDecidedByParent,
}

pub(crate) fn committed_box_from_fragment_link(callbacks: &LayoutPass<'_>, node: Node) -> Option<CommittedBox> {
    let link = callbacks.committed_fragment_link(node)?;
    let fragment = &link.fragment;

    // Skip normal node initialization: resolving computed sizes requires
    // percentage bases, and every resulting geometry field is replaced by
    // the previously committed value immediately.
    let used = UsedValues::default();
    used.frame.set(link.frame);
    used.set_box_metrics_from_fragment(fragment);
    used.has_definite_inline_size.set(true);
    used.has_definite_block_size.set(true);

    Some(CommittedBox {
        record: used,
        facts_decided_by_parent: super::run_records::FactsDecidedByParent {
            placement: Some(super::run_records::BoxPlacement {
                content_offset_in_containing_block: link.committed_offset,
                containing_block: link.containing_block,
            }),
            used_insets: super::run_records::UsedInsets {
                left: link.inset_left,
                right: link.inset_right,
                top: link.inset_top,
                bottom: link.inset_bottom,
            },
            default_scroll_shift: None,
            fragment_facts: super::run_records::FragmentFactsDecidedByParent {
                table_column_index: fragment.table_column_index,
                table_column_span: fragment.table_column_span,
                hidden_by_collapsed_columns: fragment.hidden_by_collapsed_columns,
                ..super::run_records::FragmentFactsDecidedByParent::default()
            },
        },
    })
}
