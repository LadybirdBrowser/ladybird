/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum SizeConstraint {
    #[default]
    None,
    MinContent,
    MaxContent,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCssPixelPoint {
    pub x: CssPixels,
    pub y: CssPixels,
}

impl Default for FfiCssPixelPoint {
    fn default() -> Self {
        Self {
            x: CssPixels::from_raw(0),
            y: CssPixels::from_raw(0),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCssPixelSize {
    pub width: CssPixels,
    pub height: CssPixels,
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

/// Lazily owns interior-mutable data without reserving space for `T` in every
/// `UsedValues` entry.
pub(crate) struct LazyRefCell<T> {
    value: OnceCell<Box<RefCell<T>>>,
}

impl<T> LazyRefCell<T> {
    pub(crate) const fn new() -> Self {
        Self { value: OnceCell::new() }
    }

    pub(crate) fn get(&self) -> Option<&RefCell<T>> {
        self.value.get().map(Box::as_ref)
    }

    pub(crate) fn get_or_init(&self, initialize: impl FnOnce() -> T) -> &RefCell<T> {
        self.value
            .get_or_init(|| Box::new(RefCell::new(initialize())))
            .as_ref()
    }
}

impl<T> std::fmt::Debug for LazyRefCell<T> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LazyRefCell")
            .field("initialized", &self.value.get().is_some())
            .finish()
    }
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

    pub(crate) fn is_sealed(&self) -> bool {
        self.sealed.get()
    }
}

#[derive(Default)]
pub(crate) struct LineData {
    pub(crate) line_boxes: Vec<LineBoxData>,
    pub(crate) inline_box_pieces: Vec<InlineBoxPieceData>,
}

#[derive(Default)]
pub(crate) struct UsedValuesRareData {
    pub(crate) table_cell_coordinates: Option<FfiTableCellCoordinates>,
    pub(crate) computed_svg_path: Option<libgfx_rust::path::OwnedPath>,
    pub(crate) computed_svg_transforms: Option<crate::layout::FfiSvgComputedTransforms>,
    pub(crate) svg_viewport_size: Option<crate::layout::FfiCssPixelSize>,
    pub(crate) grid_layout_data: Option<OwnedGridLayoutData>,
    pub(crate) flex_layout_data: Option<OwnedFlexLayoutData>,
    pub(crate) used_grid_tracks: Option<OwnedUsedGridTracks>,
    pub(crate) override_borders_data: Option<FfiBordersData>,
    pub(crate) abspos_layout_inputs: Option<AbsposLayoutInputs>,
}

impl UsedValuesRareData {
    pub(crate) fn install_present_payloads_into(self, record: &UsedValues) {
        let Self {
            table_cell_coordinates,
            computed_svg_path,
            computed_svg_transforms,
            svg_viewport_size,
            grid_layout_data,
            flex_layout_data,
            used_grid_tracks,
            override_borders_data,
            abspos_layout_inputs,
        } = self;
        debug_assert!(
            table_cell_coordinates.is_none() && override_borders_data.is_none() && abspos_layout_inputs.is_none(),
            "a run authored a parent-owned rare payload on its root record"
        );
        if computed_svg_path.is_none()
            && computed_svg_transforms.is_none()
            && svg_viewport_size.is_none()
            && grid_layout_data.is_none()
            && flex_layout_data.is_none()
            && used_grid_tracks.is_none()
        {
            return;
        }
        let mut rare = record.rare_data_mut();
        if let Some(path) = computed_svg_path {
            rare.computed_svg_path = Some(path);
        }
        if let Some(transforms) = computed_svg_transforms {
            rare.computed_svg_transforms = Some(transforms);
        }
        if let Some(size) = svg_viewport_size {
            rare.svg_viewport_size = Some(size);
        }
        if let Some(data) = grid_layout_data {
            rare.grid_layout_data = Some(data);
        }
        if let Some(data) = flex_layout_data {
            rare.flex_layout_data = Some(data);
        }
        if let Some(tracks) = used_grid_tracks {
            rare.used_grid_tracks = Some(tracks);
        }
    }
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

    pub inset_left: SealableCell<CssPixels>,
    pub inset_right: SealableCell<CssPixels>,
    pub inset_top: SealableCell<CssPixels>,
    pub inset_bottom: SealableCell<CssPixels>,

    pub has_definite_inline_size: Cell<bool>,
    pub has_definite_block_size: Cell<bool>,
    pub uses_collapsing_borders_model: Cell<bool>,

    pub inline_size_constraint: Cell<SizeConstraint>,
    pub block_size_constraint: Cell<SizeConstraint>,

    // Keep these as separate cells: content_offset is read by placement code
    // even where has_content_offset is false.
    pub has_content_offset: SealableCell<bool>,
    pub content_offset: SealableCell<FfiCssPixelPoint>,

    // Keep baseline payloads separate so resetting the presence bits does not
    // perturb the payloads observed by the existing derivation flow.
    pub has_first_baseline: Cell<bool>,
    pub first_baseline: Cell<CssPixels>,
    pub has_last_baseline: Cell<bool>,
    pub last_baseline: Cell<CssPixels>,


    pub(crate) line_data: LazyRefCell<LineData>,
    pub(crate) rare_data: LazyRefCell<UsedValuesRareData>,
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
            inset_left: SealableCell::new(zero),
            inset_right: SealableCell::new(zero),
            inset_top: SealableCell::new(zero),
            inset_bottom: SealableCell::new(zero),
            has_definite_inline_size: Cell::new(false),
            has_definite_block_size: Cell::new(false),
            uses_collapsing_borders_model: Cell::new(false),
            inline_size_constraint: Cell::new(SizeConstraint::None),
            block_size_constraint: Cell::new(SizeConstraint::None),
            has_content_offset: SealableCell::new(false),
            content_offset: SealableCell::new(FfiCssPixelPoint::default()),
            has_first_baseline: Cell::new(false),
            first_baseline: Cell::new(zero),
            has_last_baseline: Cell::new(false),
            last_baseline: Cell::new(zero),
            line_data: LazyRefCell::new(),
            rare_data: LazyRefCell::new(),
        }
    }
}

impl UsedValues {
    pub(crate) fn rare_data_mut(&self) -> RefMut<'_, UsedValuesRareData> {
        self.rare_data.get_or_init(UsedValuesRareData::default).borrow_mut()
    }

    pub(crate) fn line_data_ref(&self) -> Option<Ref<'_, LineData>> {
        self.line_data.get().map(RefCell::borrow)
    }

    pub(crate) fn line_data_cell(&self) -> &RefCell<LineData> {
        self.line_data.get_or_init(LineData::default)
    }

    pub(crate) fn content_baselines_from_cells(&self) -> crate::layout::DerivedBaselines {
        crate::layout::DerivedBaselines {
            first: self.has_first_baseline.get().then(|| self.first_baseline.get()),
            last: self.has_last_baseline.get().then(|| self.last_baseline.get()),
        }
    }

    /// Seals every field that commit emits as part of FfiCommittedBoxMetrics.
    /// Called when the box is placed: after placement, none of these may
    /// change again.
    pub(crate) fn seal_committed_box_metrics(&self) {
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
        self.inset_left.seal();
        self.inset_right.seal();
        self.inset_top.seal();
        self.inset_bottom.seal();
        self.has_content_offset.seal();
        self.content_offset.seal();
    }

    pub(crate) fn own_metrics_are_sealed(&self) -> bool {
        self.content_inline_size.is_sealed()
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

macro_rules! used_values_cell_state {
    ($($field:ident: $type:ty,)+) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub(crate) struct UsedValuesCellState {
            $(pub(crate) $field: $type,)+
        }

        impl UsedValuesCellState {
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

used_values_cell_state! {
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
    inset_left: CssPixels,
    inset_right: CssPixels,
    inset_top: CssPixels,
    inset_bottom: CssPixels,
    has_definite_inline_size: bool,
    has_definite_block_size: bool,
    uses_collapsing_borders_model: bool,
    inline_size_constraint: SizeConstraint,
    block_size_constraint: SizeConstraint,
    has_content_offset: bool,
    content_offset: FfiCssPixelPoint,
    has_first_baseline: bool,
    first_baseline: CssPixels,
    has_last_baseline: bool,
    last_baseline: CssPixels,
}

impl UsedValuesCellState {
    pub(crate) fn materialize_record(&self) -> UsedValues {
        let record = UsedValues::default();
        self.apply_to_record(&record);
        if self.has_content_offset {
            record.seal_committed_box_metrics();
        }
        record
    }
}

impl UsedValues {
    pub(crate) fn mirror_box_metrics_and_size_constraints_into(&self, scratch: &UsedValues) {
        scratch.margin_left.set(self.margin_left.get());
        scratch.margin_right.set(self.margin_right.get());
        scratch.margin_top.set(self.margin_top.get());
        scratch.margin_bottom.set(self.margin_bottom.get());
        scratch.border_left.set(self.border_left.get());
        scratch.border_right.set(self.border_right.get());
        scratch.border_top.set(self.border_top.get());
        scratch.border_bottom.set(self.border_bottom.get());
        scratch.padding_left.set(self.padding_left.get());
        scratch.padding_right.set(self.padding_right.get());
        scratch.padding_top.set(self.padding_top.get());
        scratch.padding_bottom.set(self.padding_bottom.get());
        scratch.content_inline_size.set(self.content_inline_size.get());
        scratch.content_block_size.set(self.content_block_size.get());
        scratch.inline_size_constraint.set(self.inline_size_constraint.get());
        scratch.block_size_constraint.set(self.block_size_constraint.get());
    }

    pub(crate) fn has_definite_inline_size(&self) -> bool {
        self.has_definite_inline_size.get() && self.inline_size_constraint.get() == SizeConstraint::None
    }

    pub(crate) fn has_definite_block_size(&self) -> bool {
        self.has_definite_block_size.get() && self.block_size_constraint.get() == SizeConstraint::None
    }

    pub(crate) fn set_content_inline_size(&self, value: CssPixels) {
        assert!(!self.has_content_offset.get());
        // Negative inline sizes are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        self.content_inline_size
            .set(clamp_to_max_dimension_value(value.max(CssPixels::default())));
        self.has_definite_inline_size.set(true);
    }

    pub(crate) fn set_content_block_size(&self, value: CssPixels) {
        assert!(!self.has_content_offset.get());
        // Negative block sizes are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        self.content_block_size
            .set(clamp_to_max_dimension_value(value.max(CssPixels::default())));
    }

    fn rounded_half_border(value: CssPixels) -> CssPixels {
        let value = CssPixels::from_raw(value.raw_value() / 2);
        let raw = value.raw_value();
        let rounded = if raw > 0 {
            (raw.saturating_add(32) & !63).min(i32::MAX & !63)
        } else if raw < 0 {
            let adjusted = raw.saturating_sub(32);
            let floor = adjusted & !63;
            floor.saturating_add(if adjusted & 63 != 0 { 64 } else { 0 })
        } else {
            0
        };
        CssPixels::from_raw(rounded)
    }

    pub(crate) fn border_left_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            Self::rounded_half_border(self.border_left.get())
        } else {
            self.border_left.get()
        }
    }

    pub(crate) fn border_right_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            Self::rounded_half_border(self.border_right.get())
        } else {
            self.border_right.get()
        }
    }

    pub(crate) fn border_top_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            Self::rounded_half_border(self.border_top.get())
        } else {
            self.border_top.get()
        }
    }

    pub(crate) fn border_bottom_collapsed(&self, collapsed: bool) -> CssPixels {
        if collapsed {
            Self::rounded_half_border(self.border_bottom.get())
        } else {
            self.border_bottom.get()
        }
    }

    pub(crate) fn border_box_left(&self, collapsed: bool) -> CssPixels {
        self.border_left_collapsed(collapsed) + self.padding_left.get()
    }

    pub(crate) fn border_box_right(&self, collapsed: bool) -> CssPixels {
        self.border_right_collapsed(collapsed) + self.padding_right.get()
    }

    pub(crate) fn border_box_top(&self, collapsed: bool) -> CssPixels {
        self.border_top_collapsed(collapsed) + self.padding_top.get()
    }

    pub(crate) fn border_box_bottom(&self, collapsed: bool) -> CssPixels {
        self.border_bottom_collapsed(collapsed) + self.padding_bottom.get()
    }

    pub(crate) fn border_box_inline_size(&self, collapsed: bool) -> CssPixels {
        self.border_box_left(collapsed) + self.content_inline_size.get() + self.border_box_right(collapsed)
    }

    pub(crate) fn border_box_block_size(&self, collapsed: bool) -> CssPixels {
        self.border_box_top(collapsed) + self.content_block_size.get() + self.border_box_bottom(collapsed)
    }

    pub(crate) fn margin_box_bottom(&self, collapsed: bool) -> CssPixels {
        self.margin_bottom.get() + self.border_box_bottom(collapsed)
    }

    pub(crate) fn margin_box_top(&self, collapsed: bool) -> CssPixels {
        self.margin_top.get() + self.border_box_top(collapsed)
    }

    pub(crate) fn margin_box_inline_size(&self, collapsed: bool) -> CssPixels {
        self.margin_left.get()
            + self.border_box_left(collapsed)
            + self.content_inline_size.get()
            + self.border_box_right(collapsed)
            + self.margin_right.get()
    }

    pub(crate) fn margin_box_block_size(&self, collapsed: bool) -> CssPixels {
        self.margin_top.get()
            + self.border_box_top(collapsed)
            + self.content_block_size.get()
            + self.margin_box_bottom(collapsed)
    }

    pub(crate) fn available_inner_space_or_constraints_from(
        &self,
        outer: crate::layout::AvailableSpace,
    ) -> crate::layout::AvailableSpace {
        use crate::layout::AvailableSize;

        let mut inline_size = match self.inline_size_constraint.get() {
            SizeConstraint::MinContent => AvailableSize::MinContent,
            SizeConstraint::MaxContent => AvailableSize::MaxContent,
            SizeConstraint::None if self.has_definite_inline_size.get() => {
                AvailableSize::definite(self.content_inline_size.get())
            }
            SizeConstraint::None => AvailableSize::Indefinite,
        };
        let mut block_size = match self.block_size_constraint.get() {
            SizeConstraint::MinContent => AvailableSize::MinContent,
            SizeConstraint::MaxContent => AvailableSize::MaxContent,
            SizeConstraint::None if self.has_definite_block_size.get() => {
                AvailableSize::definite(self.content_block_size.get())
            }
            SizeConstraint::None => AvailableSize::Indefinite,
        };
        if inline_size == AvailableSize::Indefinite
            && matches!(
                outer.inline_size,
                AvailableSize::MinContent | AvailableSize::MaxContent
            )
        {
            inline_size = outer.inline_size;
        }
        if block_size == AvailableSize::Indefinite
            && matches!(
                outer.block_size,
                AvailableSize::MinContent | AvailableSize::MaxContent
            )
        {
            block_size = outer.block_size;
        }
        crate::layout::AvailableSpace {
            inline_size,
            block_size,
        }
    }
}

pub(crate) fn create_used_values(
    callbacks: &FfiLayoutFcCallbacks,
    node: Node,
    constraints: ContainingBlockConstraints,
) -> std::rc::Rc<UsedValues> {
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

    let parent = callbacks.parent(node);
    let parent_facts = (!parent.is_invalid()).then(|| NodeFacts::new(callbacks, parent));
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
                && parent_facts.is_some_and(|parent| {
                    !parent.is_floating()
                        && (parent.display().is_flow_root_inside() || parent.display().is_flow_inside())
                })
                && containing_block_has_definite_size(Axis::Inline)
            {
                let available = containing_block_size_for_axis(Axis::Inline);
                return Some(clamp_to_max_dimension_value(
                    available
                        - used.margin_left.get()
                        - used.margin_right.get()
                        - used.padding_left.get()
                        - used.padding_right.get()
                        - used.border_left.get()
                        - used.border_right.get(),
                ));
            }
            return None;
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

    std::rc::Rc::new(used)
}

pub(crate) fn used_values_from_paintable(
    callbacks: &FfiLayoutFcCallbacks,
    node: Node,
    paintable: *mut c_void,
) -> Option<std::rc::Rc<UsedValues>> {
    let mut geometry = FfiPaintableGeometry::default();
    let found =
        unsafe {
            (callbacks.read_paintable_geometry)(callbacks.context, callbacks.shell(node), paintable, &raw mut geometry)
        };
    if !found {
        return None;
    }

    // Skip normal node initialization: resolving computed sizes requires
    // percentage bases, and every resulting geometry field is replaced by
    // the previous paintable's committed value immediately.
    let used = UsedValues::default();
    used.set_content_inline_size(geometry.content_inline_size);
    used.set_content_block_size(geometry.content_block_size);
    used.has_definite_inline_size.set(true);
    used.has_definite_block_size.set(true);
    used.content_offset.set(geometry.content_offset);
    used.margin_left.set(geometry.margin_left);
    used.margin_right.set(geometry.margin_right);
    used.margin_top.set(geometry.margin_top);
    used.margin_bottom.set(geometry.margin_bottom);
    used.border_left.set(geometry.border_left);
    used.border_right.set(geometry.border_right);
    used.border_top.set(geometry.border_top);
    used.border_bottom.set(geometry.border_bottom);
    used.padding_left.set(geometry.padding_left);
    used.padding_right.set(geometry.padding_right);
    used.padding_top.set(geometry.padding_top);
    used.padding_bottom.set(geometry.padding_bottom);
    used.inset_left.set(geometry.inset_left);
    used.inset_right.set(geometry.inset_right);
    used.inset_top.set(geometry.inset_top);
    used.inset_bottom.set(geometry.inset_bottom);
    // Materialization is this box's placement: the previous paintable's
    // committed geometry is final from the moment it is adopted.
    used.has_content_offset.set(true);
    used.seal_committed_box_metrics();

    if NodeFacts::new(callbacks, node).is_svg_svg_box() {
        used.rare_data_mut().svg_viewport_size = Some(geometry.svg_viewport_size);
    }
    Some(std::rc::Rc::new(used))
}
