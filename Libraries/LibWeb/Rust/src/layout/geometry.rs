/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(crate) fn to_physical<T>(writing_mode: u8, inline: T, block: T) -> (T, T) {
    if writing_mode == writing_mode::HORIZONTAL_TB {
        (inline, block)
    } else {
        (block, inline)
    }
}

pub(crate) fn to_logical<T>(writing_mode: u8, horizontal: T, vertical: T) -> (T, T) {
    if writing_mode == writing_mode::HORIZONTAL_TB {
        (horizontal, vertical)
    } else {
        (vertical, horizontal)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AvailableSize {
    Definite(CssPixels),
    Indefinite,
    MinContent,
    MaxContent,
}

impl AvailableSize {
    pub fn definite(value: CssPixels) -> Self {
        assert!(!matches!(value.raw_value(), i32::MIN | i32::MAX));
        Self::Definite(value)
    }

    pub(crate) fn is_intrinsic_sizing_constraint(self) -> bool {
        matches!(self, Self::MinContent | Self::MaxContent)
    }

    pub(crate) fn to_px_or_zero(self) -> CssPixels {
        match self {
            Self::Definite(value) => value,
            _ => CssPixels::default(),
        }
    }

    pub(crate) fn pixels_greater_than(self, pixels: CssPixels) -> bool {
        match self {
            Self::MaxContent | Self::Indefinite => false,
            Self::MinContent => true,
            Self::Definite(value) => pixels > value,
        }
    }

    pub(crate) fn pixels_less_than(self, pixels: CssPixels) -> bool {
        match self {
            Self::MaxContent | Self::Indefinite => true,
            Self::MinContent => false,
            Self::Definite(value) => pixels < value,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct AvailableSpace {
    pub inline_size: AvailableSize,
    pub block_size: AvailableSize,
}

impl Default for AvailableSpace {
    fn default() -> Self {
        Self {
            inline_size: AvailableSize::Indefinite,
            block_size: AvailableSize::Indefinite,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LogicalSize {
    pub(crate) inline_size: CssPixels,
    pub(crate) block_size: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LogicalOffset {
    pub(crate) inline_offset: CssPixels,
    pub(crate) block_offset: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LogicalRect {
    pub(crate) offset: LogicalOffset,
    pub(crate) size: LogicalSize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ContainingBlockConstraints {
    pub(crate) percentage_basis_inline_size: Option<CssPixels>,
    pub(crate) percentage_basis_block_size: Option<CssPixels>,
    pub(crate) quirks_mode_percentage_basis_block_size: Option<CssPixels>,
}

impl ContainingBlockConstraints {
    pub(crate) fn inline_basis(self) -> CssPixels {
        self.percentage_basis_inline_size.unwrap_or_default()
    }

    pub(crate) fn block_basis(self) -> CssPixels {
        self.percentage_basis_block_size.unwrap_or_default()
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct RootSizingDirectives {
    pub(crate) forced_content_inline_size: Option<CssPixels>,
    pub(crate) forced_content_block_size: Option<CssPixels>,
    pub(crate) forced_min_border_box_block_size: Option<CssPixels>,
    // Input-only: the table formatting context supplies the cell's intrinsic block padding
    // (the vertical-alignment stretch) before laying out the cell's contents.
    pub(crate) table_cell_intrinsic_block_padding: Option<(CssPixels, CssPixels)>,
    // Input-only: the wrapper's BFC tells the table formatting context where the table box's
    // content box sits in the wrapper, so rows and row groups (whose containing block is the
    // wrapper) can be placed in wrapper coordinates.
    pub(crate) table_box_content_block_offset_in_wrapper: Option<CssPixels>,
    pub(crate) adopt_automatic_content_block_size: bool,
    pub(crate) flex_self_block_size_resolution_space: Option<AvailableSpace>,
    pub(crate) float_avoidance_inline_size: Option<CssPixels>,
    pub(crate) outer_float_intrusion_before_list_item_children: inline_formatting_context::SpaceUsedByFloats,
    pub(crate) treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ParticipationInParentFormattingContext {
    BlockLevel,
    Float,
    AtomicInline,
    AbsolutelyPositioned(abspos_inputs::AbsposLayoutInputs),
    Item,
    Root,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct LayoutInput {
    pub(crate) available_space: AvailableSpace,
    pub(crate) containing_block_constraints: ContainingBlockConstraints,
    pub(crate) content_box_position_in_bfc_root: Option<FfiCssPixelPoint>,
    pub(crate) sizing: RootSizingDirectives,
    pub(crate) participation: ParticipationInParentFormattingContext,
}

impl LayoutInput {
    pub(crate) fn new(
        available_space: AvailableSpace,
        containing_block_constraints: ContainingBlockConstraints,
        participation: ParticipationInParentFormattingContext,
    ) -> Self {
        Self {
            available_space,
            containing_block_constraints,
            content_box_position_in_bfc_root: None,
            sizing: RootSizingDirectives::default(),
            participation,
        }
    }

    pub(crate) fn with_forced_sizes(
        mut self,
        forced_content_inline_size: CssPixels,
        forced_content_block_size: CssPixels,
    ) -> Self {
        self.sizing.forced_content_inline_size = Some(forced_content_inline_size);
        self.sizing.forced_content_block_size = Some(forced_content_block_size);
        self
    }
}
