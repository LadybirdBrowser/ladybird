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
pub(crate) struct LineBoxFragmentCoordinate {
    pub line_box_index: usize,
    pub fragment_index: usize,
}

/// The per-box geometry stored in a Rust-owned layout pass.
#[derive(Debug)]
pub(crate) struct UsedValues {
    pub node: crate::layout::node_data::NodeSlotId,

    pub content_inline_size: Cell<CssPixels>,
    pub content_block_size: Cell<CssPixels>,

    pub margin_left: Cell<CssPixels>,
    pub margin_right: Cell<CssPixels>,
    pub margin_top: Cell<CssPixels>,
    pub margin_bottom: Cell<CssPixels>,

    pub border_left: Cell<CssPixels>,
    pub border_right: Cell<CssPixels>,
    pub border_top: Cell<CssPixels>,
    pub border_bottom: Cell<CssPixels>,

    pub padding_left: Cell<CssPixels>,
    pub padding_right: Cell<CssPixels>,
    pub padding_top: Cell<CssPixels>,
    pub padding_bottom: Cell<CssPixels>,

    pub inset_left: Cell<CssPixels>,
    pub inset_right: Cell<CssPixels>,
    pub inset_top: Cell<CssPixels>,
    pub inset_bottom: Cell<CssPixels>,

    pub has_definite_inline_size: Cell<bool>,
    pub has_definite_block_size: Cell<bool>,
    pub materialized_from_paintable: Cell<bool>,
    pub uses_collapsing_borders_model: Cell<bool>,

    pub inline_size_constraint: Cell<SizeConstraint>,
    pub block_size_constraint: Cell<SizeConstraint>,

    // Keep these as separate cells: content_offset is read by placement code
    // even where has_content_offset is false.
    pub has_content_offset: Cell<bool>,
    pub content_offset: Cell<FfiCssPixelPoint>,

    // Keep baseline payloads separate so resetting the presence bits does not
    // perturb the payloads observed by the existing derivation flow.
    pub has_first_baseline: Cell<bool>,
    pub first_baseline: Cell<CssPixels>,
    pub has_last_baseline: Cell<bool>,
    pub last_baseline: Cell<CssPixels>,

    // Commit copies the coordinate payload even when the presence bit is
    // false, so this cannot be represented as Cell<Option<_>>.
    pub has_containing_line_box_fragment: Cell<bool>,
    pub containing_line_box_fragment: Cell<LineBoxFragmentCoordinate>,
}

impl Default for UsedValues {
    fn default() -> Self {
        let zero = CssPixels::from_raw(0);
        Self {
            node: crate::layout::node_data::NodeSlotId::INVALID,
            content_inline_size: Cell::new(zero),
            content_block_size: Cell::new(zero),
            margin_left: Cell::new(zero),
            margin_right: Cell::new(zero),
            margin_top: Cell::new(zero),
            margin_bottom: Cell::new(zero),
            border_left: Cell::new(zero),
            border_right: Cell::new(zero),
            border_top: Cell::new(zero),
            border_bottom: Cell::new(zero),
            padding_left: Cell::new(zero),
            padding_right: Cell::new(zero),
            padding_top: Cell::new(zero),
            padding_bottom: Cell::new(zero),
            inset_left: Cell::new(zero),
            inset_right: Cell::new(zero),
            inset_top: Cell::new(zero),
            inset_bottom: Cell::new(zero),
            has_definite_inline_size: Cell::new(false),
            has_definite_block_size: Cell::new(false),
            materialized_from_paintable: Cell::new(false),
            uses_collapsing_borders_model: Cell::new(false),
            inline_size_constraint: Cell::new(SizeConstraint::None),
            block_size_constraint: Cell::new(SizeConstraint::None),
            has_content_offset: Cell::new(false),
            content_offset: Cell::new(FfiCssPixelPoint::default()),
            has_first_baseline: Cell::new(false),
            first_baseline: Cell::new(zero),
            has_last_baseline: Cell::new(false),
            last_baseline: Cell::new(zero),
            has_containing_line_box_fragment: Cell::new(false),
            containing_line_box_fragment: Cell::new(LineBoxFragmentCoordinate::default()),
        }
    }
}

impl UsedValues {
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
