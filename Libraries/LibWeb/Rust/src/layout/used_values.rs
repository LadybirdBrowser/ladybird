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
        assert!(
            !self.sealed.get(),
            "write to a sealed committed box metric after placement"
        );
        self.value.set(value);
    }

    pub(crate) fn seal(&self) {
        self.sealed.set(true);
    }
}

/// The per-box geometry stored in a Rust-owned layout pass.
#[derive(Debug)]
pub(crate) struct UsedValues {
    pub node: crate::layout::node_data::NodeSlotId,

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
    pub materialized_from_paintable: Cell<bool>,
    pub uses_collapsing_borders_model: Cell<bool>,

    pub inline_size_constraint: Cell<SizeConstraint>,
    pub block_size_constraint: Cell<SizeConstraint>,

    // Keep these as separate cells: content_offset is read by placement code
    // even where has_content_offset is false.
    pub has_content_offset: SealableCell<bool>,
    pub content_offset: SealableCell<FfiCssPixelPoint>,

    pub committed_offset_delta: SealableCell<FfiCssPixelPoint>,

    // Keep baseline payloads separate so resetting the presence bits does not
    // perturb the payloads observed by the existing derivation flow.
    pub has_first_baseline: Cell<bool>,
    pub first_baseline: Cell<CssPixels>,
    pub has_last_baseline: Cell<bool>,
    pub last_baseline: Cell<CssPixels>,

    // Commit copies the coordinate payload even when the presence bit is
    // false, so this cannot be represented as Cell<Option<_>>.
    pub has_containing_line_box_fragment: SealableCell<bool>,
    pub containing_line_box_fragment: SealableCell<LineBoxFragmentCoordinate>,

    pub(crate) line_data: LazyRefCell<LineData>,
    pub(crate) rare_data: LazyRefCell<UsedValuesRareData>,
}

impl Default for UsedValues {
    fn default() -> Self {
        let zero = CssPixels::from_raw(0);
        Self {
            node: crate::layout::node_data::NodeSlotId::INVALID,
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
            materialized_from_paintable: Cell::new(false),
            uses_collapsing_borders_model: Cell::new(false),
            inline_size_constraint: Cell::new(SizeConstraint::None),
            block_size_constraint: Cell::new(SizeConstraint::None),
            has_content_offset: SealableCell::new(false),
            content_offset: SealableCell::new(FfiCssPixelPoint::default()),
            committed_offset_delta: SealableCell::new(FfiCssPixelPoint::default()),
            has_first_baseline: Cell::new(false),
            first_baseline: Cell::new(zero),
            has_last_baseline: Cell::new(false),
            last_baseline: Cell::new(zero),
            has_containing_line_box_fragment: SealableCell::new(false),
            containing_line_box_fragment: SealableCell::new(LineBoxFragmentCoordinate::default()),
            line_data: LazyRefCell::new(),
            rare_data: LazyRefCell::new(),
        }
    }
}

impl UsedValues {
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
        self.committed_offset_delta.seal();
        self.has_containing_line_box_fragment.seal();
        self.containing_line_box_fragment.seal();
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
