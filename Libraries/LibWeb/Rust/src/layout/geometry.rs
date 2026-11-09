/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

// NB: Layout computes every box in the "frame" of the writing mode of the formatting context that lays it out: the
//     frame is that writing mode's flow-relative view of the plane, turned so that its line-left side is the frame's
//     left side and its block-start side is the frame's top side. Within a frame, the inline axis is the horizontal
//     one and blocks stack downwards, so every layout algorithm can work as if in horizontal-tb. The left and right
//     sides of a frame are line-relative rather than inline-start and inline-end, which keeps 'direction' working the
//     same in every writing mode. Painting maps the frame back onto the physical plane.
//     https://drafts.csswg.org/css-writing-modes-4/#logical-to-physical

/// A side of a box, in a frame or in the physical plane.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum Side {
    Top = 0,
    Right = 1,
    Bottom = 2,
    Left = 3,
}

impl Side {
    const ALL: [Side; 4] = [Side::Top, Side::Right, Side::Bottom, Side::Left];

    fn from_index(index: u8) -> Side {
        Self::ALL[index as usize & 3]
    }
}

/// The physical side each side of the frame of a writing mode lies on, indexed by the frame side.
const FRAME_SIDES_IN_PHYSICAL_PLANE: [[u8; 4]; 5] = [
    // horizontal-tb
    [0, 1, 2, 3],
    // vertical-rl: block-start is right, line-left is top.
    [1, 2, 3, 0],
    // vertical-lr: block-start is left, line-left is top.
    [3, 2, 1, 0],
    // sideways-rl: block-start is right, line-left is top.
    [1, 2, 3, 0],
    // sideways-lr: block-start is left, line-left is bottom.
    [3, 0, 1, 2],
];

/// The frame side that each physical side lies on, indexed by the physical side.
const PHYSICAL_SIDES_IN_FRAME: [[u8; 4]; 5] = [[0, 1, 2, 3], [3, 0, 1, 2], [3, 2, 1, 0], [3, 0, 1, 2], [1, 2, 3, 0]];

/// The widths of the sides of a box, such as its borders or paddings, in a frame or in the physical plane.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Sides {
    pub(crate) top: CssPixels,
    pub(crate) right: CssPixels,
    pub(crate) bottom: CssPixels,
    pub(crate) left: CssPixels,
}

impl Sides {
    /// The sides, which frame `from` holds, as frame `to` holds them.
    #[inline]
    pub(crate) fn in_frame(self, from: Frame, to: Frame) -> Self {
        let [top, right, bottom, left] = from.sides_in([self.top, self.right, self.bottom, self.left], to);
        Self {
            top,
            right,
            bottom,
            left,
        }
    }
}

/// The frame of a writing mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Frame(u8);

impl Default for Frame {
    fn default() -> Self {
        Self::PHYSICAL
    }
}

impl Frame {
    pub(crate) const PHYSICAL: Frame = Frame(writing_mode::HORIZONTAL_TB);

    #[inline]
    pub(crate) fn of(writing_mode: u8) -> Frame {
        Frame(if writing_mode <= writing_mode::SIDEWAYS_LR {
            writing_mode
        } else {
            writing_mode::HORIZONTAL_TB
        })
    }

    #[inline]
    pub(crate) fn writing_mode(self) -> u8 {
        self.0
    }

    #[inline]
    pub(crate) fn is_physical(self) -> bool {
        self.0 == writing_mode::HORIZONTAL_TB
    }

    /// Whether the frame's horizontal axis runs along the physical vertical axis.
    #[inline]
    pub(crate) fn is_transposed(self) -> bool {
        self.0 != writing_mode::HORIZONTAL_TB
    }

    /// Whether the frames run their inline axes along perpendicular physical axes.
    #[inline]
    pub(crate) fn is_orthogonal_to(self, other: Frame) -> bool {
        self.is_transposed() != other.is_transposed()
    }

    #[inline]
    pub(crate) fn physical_side(self, side: Side) -> Side {
        Side::from_index(FRAME_SIDES_IN_PHYSICAL_PLANE[self.0 as usize][side as usize])
    }

    #[inline]
    pub(crate) fn frame_side(self, physical_side: Side) -> Side {
        Side::from_index(PHYSICAL_SIDES_IN_FRAME[self.0 as usize][physical_side as usize])
    }

    /// The side of `other` that lies on the same physical side as the given side of this frame.
    #[inline]
    pub(crate) fn side_in(self, side: Side, other: Frame) -> Side {
        other.frame_side(self.physical_side(side))
    }

    /// Maps per-side values, indexed top, right, bottom, left, from this frame to `other`.
    #[inline]
    pub(crate) fn sides_in<T: Copy>(self, values: [T; 4], other: Frame) -> [T; 4] {
        if self == other {
            return values;
        }
        Side::ALL.map(|side| values[other.side_in(side, self) as usize])
    }

    /// Whether the frame's horizontal axis runs opposite to the physical axis it lies on.
    pub(crate) fn horizontal_axis_is_reversed(self) -> bool {
        matches!(self.physical_side(Side::Left), Side::Right | Side::Bottom)
    }

    /// Whether the frame's vertical axis runs opposite to the physical axis it lies on.
    pub(crate) fn vertical_axis_is_reversed(self) -> bool {
        matches!(self.physical_side(Side::Top), Side::Right | Side::Bottom)
    }

    /// Maps a rect in this frame to the physical plane, within a box whose physical size is `container_size`, both
    /// relative to that box's top-left corner and its frame origin respectively.
    pub(crate) fn rect_to_physical(self, rect: CssPixelRect, container_size: FfiCssPixelSize) -> CssPixelRect {
        if self.is_physical() {
            return rect;
        }
        // NB: In a transposed frame, the horizontal frame axis runs along the physical vertical axis.
        let (mut x, mut y, width, height) = (rect.y, rect.x, rect.height, rect.width);
        if self.vertical_axis_is_reversed() {
            x = container_size.width - x - width;
        }
        if self.horizontal_axis_is_reversed() {
            y = container_size.height - y - height;
        }
        CssPixelRect { x, y, width, height }
    }

    /// Maps a physical rect within a box of physical size `container_size` to this frame.
    pub(crate) fn rect_from_physical(self, rect: CssPixelRect, container_size: FfiCssPixelSize) -> CssPixelRect {
        if self.is_physical() {
            return rect;
        }
        let (mut x, mut y) = (rect.x, rect.y);
        if self.vertical_axis_is_reversed() {
            x = container_size.width - x - rect.width;
        }
        if self.horizontal_axis_is_reversed() {
            y = container_size.height - y - rect.height;
        }
        CssPixelRect {
            x: y,
            y: x,
            width: rect.height,
            height: rect.width,
        }
    }

    /// Maps a rect within a box from this frame to `other`, where the box has the physical size `container_size`.
    pub(crate) fn rect_in(self, rect: CssPixelRect, other: Frame, container_size: FfiCssPixelSize) -> CssPixelRect {
        if self == other {
            return rect;
        }
        other.rect_from_physical(self.rect_to_physical(rect, container_size), container_size)
    }

    /// Maps an offset in this frame to the physical plane, as the corner of an empty rect at the origin of an empty box
    /// maps.
    pub(crate) fn offset_to_physical(self, offset: FfiCssPixelPoint) -> FfiCssPixelPoint {
        let corner = self.rect_to_physical(
            CssPixelRect::new(offset.x, offset.y, CssPixels::default(), CssPixels::default()),
            FfiCssPixelSize::default(),
        );
        FfiCssPixelPoint {
            x: corner.x,
            y: corner.y,
        }
    }

    /// Maps a physical offset to this frame.
    pub(crate) fn offset_from_physical(self, offset: FfiCssPixelPoint) -> FfiCssPixelPoint {
        let corner = self.rect_from_physical(
            CssPixelRect::new(offset.x, offset.y, CssPixels::default(), CssPixels::default()),
            FfiCssPixelSize::default(),
        );
        FfiCssPixelPoint {
            x: corner.x,
            y: corner.y,
        }
    }

    /// The physical half-plane before the block-end edge of a content box whose contents use this frame and whose
    /// physical size is `content_size`, relative to that content box.
    pub(crate) fn half_plane_before_block_end(self, content_size: FfiCssPixelSize) -> CssPixelRect {
        let extent = CssPixels::from_integer((crate::css::css_pixels::MAX_INTEGER_VALUE / 4) as i64);
        let block_size = if self.is_transposed() {
            content_size.width
        } else {
            content_size.height
        };
        self.rect_to_physical(
            CssPixelRect::new(-extent, -extent, extent * 2, extent + block_size),
            content_size,
        )
    }

    /// Maps a physical size to this frame.
    #[inline]
    pub(crate) fn size_from_physical(self, size: FfiCssPixelSize) -> FfiCssPixelSize {
        self.size_to_physical(size.width, size.height)
    }

    /// Maps a size in this frame to the physical plane.
    #[inline]
    pub(crate) fn size_to_physical(self, width: CssPixels, height: CssPixels) -> FfiCssPixelSize {
        if self.is_transposed() {
            FfiCssPixelSize {
                width: height,
                height: width,
            }
        } else {
            FfiCssPixelSize { width, height }
        }
    }
}

pub(crate) fn to_physical<T>(writing_mode: u8, inline: T, block: T) -> (T, T) {
    if writing_mode == writing_mode::HORIZONTAL_TB {
        (inline, block)
    } else {
        (block, inline)
    }
}

/// Whether the start side, in the given writing mode and direction, of the axis that runs along the given physical
/// axis is its left (horizontal) or top (vertical) side.
pub(crate) fn start_is_left_or_top(writing_mode: u8, direction: u8, horizontal: bool) -> bool {
    let is_ltr = direction == direction::LTR;
    match (writing_mode, horizontal) {
        (writing_mode::HORIZONTAL_TB, true) => is_ltr,
        (writing_mode::HORIZONTAL_TB, false) => true,
        (writing_mode::VERTICAL_RL | writing_mode::SIDEWAYS_RL, true) => false,
        (writing_mode::VERTICAL_LR | writing_mode::SIDEWAYS_LR, true) => true,
        (writing_mode::SIDEWAYS_LR, false) => !is_ltr,
        (_, false) => is_ltr,
        _ => true,
    }
}

/// Whether the start side, in the given writing mode and direction, of the axis that runs along the given axis of
/// `frame` is that axis's left (horizontal) or top (vertical) side in the frame.
pub(crate) fn start_is_low_side_in_frame(
    frame: Frame,
    writing_mode: u8,
    direction: u8,
    frame_horizontal: bool,
) -> bool {
    let physical_horizontal = frame_horizontal != frame.is_transposed();
    let start_is_physical_low_side = start_is_left_or_top(writing_mode, direction, physical_horizontal);
    let physical_low_side = if physical_horizontal { Side::Left } else { Side::Top };
    matches!(frame.frame_side(physical_low_side), Side::Left | Side::Top) == start_is_physical_low_side
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AvailableSize {
    Definite(CssPixels),
    #[default]
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PackedOptionalCssPixels(i32);

impl PackedOptionalCssPixels {
    const NONE_RAW_VALUE: i32 = i32::MIN;

    pub(crate) fn get(self) -> Option<CssPixels> {
        (self.0 != Self::NONE_RAW_VALUE).then(|| CssPixels::from_raw(self.0))
    }

    pub(crate) fn is_none(self) -> bool {
        self.0 == Self::NONE_RAW_VALUE
    }
}

impl Default for PackedOptionalCssPixels {
    fn default() -> Self {
        Self(Self::NONE_RAW_VALUE)
    }
}

impl From<Option<CssPixels>> for PackedOptionalCssPixels {
    fn from(value: Option<CssPixels>) -> Self {
        match value {
            Some(pixels) => Self(pixels.raw_value().max(Self::NONE_RAW_VALUE + 1)),
            None => Self::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct RootSizingDirectives {
    // Input-only: a fieldset's content wrapper fills the fieldset's definite content block size.
    pub(crate) forced_content_block_size: PackedOptionalCssPixels,
    pub(crate) forced_min_border_box_block_size: PackedOptionalCssPixels,
    // Input-only: the wrapper's BFC tells the table formatting context where the table box's
    // content box sits in the wrapper, so rows and row groups (whose containing block is the
    // wrapper) can be placed in wrapper coordinates.
    pub(crate) table_box_content_block_offset_in_wrapper: PackedOptionalCssPixels,
    pub(crate) content_block_size_after_run: ContentBlockSizeAfterRun,
    // Input-only: how far the run of a table cell moves the contents of its root along the block axis, to align them
    // in the block size the rows it spans give it after the run (see ContentBlockSizeAfterRun::Given).
    pub(crate) table_cell_contents_block_offset: CssPixels,
    pub(crate) outer_float_intrusion_before_list_item_children: inline_formatting_context::SpaceUsedByFloats,
    pub(crate) treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    pub(crate) root_is_measured_for_its_content_block_size: bool,
    // Input-only: MathML layout lays out an embellished operator with the size its core operator should stretch to
    // cover.
    pub(crate) math_stretch_size_constraint: Option<MathStretchSizeConstraint>,
}

/// The content block size the root of an item's run takes once its contents are laid out.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum ContentBlockSizeAfterRun {
    /// The one its container handed it.
    #[default]
    HandedOver,
    /// Its automatic content block size.
    Automatic,
    /// This one, once its contents are laid out against the one its container handed it: a flex or grid item laid out
    /// against the block size transferred through its aspect ratio takes the one its automatic minimum size grew to
    /// fit the content, and a table cell takes the block size of the rows it spans.
    Given(CssPixels),
}

/// The size that MathML layout asks the core operator of an embellished operator to stretch to cover.
// https://w3c.github.io/mathml-core/#box-model
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum MathStretchSizeConstraint {
    /// The ink line-ascent and ink line-descent that a core operator stretched along the block axis should cover.
    Block { ascent: CssPixels, descent: CssPixels },
    /// The target size that a core operator stretched along the inline axis should cover.
    Inline(CssPixels),
    /// AD-HOC: A block stretch size constraint that also covers the in-flow children of the embellished operators
    ///         it passes through. It stands in for laying out an embellished operator with the block stretch size
    ///         constraint (0, 0) and then with its own ink line-ascent and ink line-descent, as a box is laid out
    ///         only once in a run.
    BlockCoveringContent { ascent: CssPixels, descent: CssPixels },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum ParticipationInParentFormattingContext {
    BlockLevel,
    /// A float, which its parent block formatting context sizes against the floats beside it before the run and places
    /// after it. The run lays the float out from its own origin.
    Float {
        /// Whether the float is a flex container that resolves its own automatic block size in its available space,
        /// which its parent finds ahead of the float's inline size.
        flex_container_resolves_its_own_automatic_block_size: bool,
    },
    AtomicInline,
    AbsolutelyPositioned(std::sync::Arc<abspos_inputs::AbsposLayoutInputs>),
    Item,
    Root,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct LayoutInput {
    pub(crate) available_space: AvailableSpace,
    pub(crate) containing_block_constraints: ContainingBlockConstraints,
    pub(crate) sizing: RootSizingDirectives,
    pub(crate) participation: ParticipationInParentFormattingContext,
}

const _: () = assert!(size_of::<LayoutInput>() <= 144);

impl LayoutInput {
    pub(crate) fn new(
        available_space: AvailableSpace,
        containing_block_constraints: ContainingBlockConstraints,
        participation: ParticipationInParentFormattingContext,
    ) -> Self {
        Self {
            available_space,
            containing_block_constraints,
            sizing: RootSizingDirectives::default(),
            participation,
        }
    }
}
