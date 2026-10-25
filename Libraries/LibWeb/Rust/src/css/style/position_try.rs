/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The position options of an absolutely positioned box: the styles that the entries of its `position-try-fallbacks`
//! make of its base styles, which layout tries when the box overflows its inset-modified containing block.
//! https://drafts.csswg.org/css-anchor-position-1/#fallback

use super::HashMap;
use super::StyleEngine;
use super::layout_style::{DerivedStyleRecord, LayoutStyle};
use super::tree::{StyleNodeID, TreeScopeID};
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::computed_value_types::{AnchorValues, FontValues};
use crate::css::computed_values::InheritedBoxValues;
use crate::css::css_enums::{anchor_size, keyword, position_area, try_tactic, writing_mode};
use crate::css::declaration_block::DeclarationBlockData;
use crate::css::property_metadata::property_id;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use crate::css::table_group_builder::group_index;
use std::sync::Arc;

/// One style scope's `@position-try` rules: the declarations of the rule each name resolves to.
pub(crate) type PositionTryRow = HashMap<Box<[u16]>, Arc<DeclarationBlockData>>;

/// The `@position-try` rules every style scope of the document defines, as each scope's rule cache resolved them.
#[derive(Clone, Default)]
pub(crate) struct PositionTryRules {
    scopes: HashMap<TreeScopeID, PositionTryRow>,
}

impl PositionTryRules {
    /// The row of the names, packed into one buffer of code units with a length each, and the declarations of each.
    ///
    /// # Safety
    /// Each declaration pointer must be a live `Arc<DeclarationBlockData>`.
    pub(crate) unsafe fn row(
        name_lengths: &[u32],
        name_units: &[u16],
        declarations: &[*const std::ffi::c_void],
    ) -> PositionTryRow {
        debug_assert_eq!(name_lengths.len(), declarations.len());
        let mut offset = 0;
        name_lengths
            .iter()
            .zip(declarations)
            .map(|(&length, &declarations)| {
                let units = &name_units[offset..offset + length as usize];
                offset += length as usize;
                // SAFETY: Guaranteed by the caller; the row takes a reference of its own.
                let declarations = unsafe {
                    let declarations = declarations.cast::<DeclarationBlockData>();
                    Arc::increment_strong_count(declarations);
                    Arc::from_raw(declarations)
                };
                (Box::from(units), declarations)
            })
            .collect()
    }

    /// Replaces one scope's row. An empty row gives the scope's row up.
    pub(crate) fn set(&mut self, tree_scope: TreeScopeID, row: PositionTryRow) {
        if row.is_empty() {
            self.scopes.remove(&tree_scope);
            return;
        }
        self.scopes.insert(tree_scope, row);
    }

    /// The declarations of the `@position-try` rule of this name that the tree scope `scope` defines.
    fn in_scope(&self, scope: TreeScopeID, name: &[u16]) -> Option<&Arc<DeclarationBlockData>> {
        self.scopes.get(&scope)?.get(name)
    }
}

/// https://drafts.csswg.org/css-anchor-position-1/#accepted-position-try-properties
/// The physical longhands of the accepted @position-try properties.
const ACCEPTED_POSITION_TRY_LONGHANDS: [u16; 18] = [
    property_id::TOP,
    property_id::RIGHT,
    property_id::BOTTOM,
    property_id::LEFT,
    property_id::MARGIN_TOP,
    property_id::MARGIN_RIGHT,
    property_id::MARGIN_BOTTOM,
    property_id::MARGIN_LEFT,
    property_id::WIDTH,
    property_id::HEIGHT,
    property_id::MIN_WIDTH,
    property_id::MIN_HEIGHT,
    property_id::MAX_WIDTH,
    property_id::MAX_HEIGHT,
    property_id::ALIGN_SELF,
    property_id::JUSTIFY_SELF,
    property_id::POSITION_ANCHOR,
    property_id::POSITION_AREA,
];

/// The style groups that hold the accepted @position-try properties.
const POSITION_OPTION_GROUPS: [usize; 4] = [
    group_index::SURROUND,
    group_index::SIZING,
    group_index::ALIGNMENT,
    group_index::ANCHOR,
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Side {
    Top,
    Right,
    Bottom,
    Left,
}

impl Side {
    const ALL: [Self; 4] = [Self::Top, Self::Right, Self::Bottom, Self::Left];

    fn index(self) -> usize {
        self as usize
    }

    fn opposite(self) -> Self {
        match self {
            Self::Top => Self::Bottom,
            Self::Right => Self::Left,
            Self::Bottom => Self::Top,
            Self::Left => Self::Right,
        }
    }

    fn is_horizontal(self) -> bool {
        matches!(self, Self::Left | Self::Right)
    }

    fn inset_property(self) -> u16 {
        match self {
            Self::Top => property_id::TOP,
            Self::Right => property_id::RIGHT,
            Self::Bottom => property_id::BOTTOM,
            Self::Left => property_id::LEFT,
        }
    }

    fn margin_property(self) -> u16 {
        match self {
            Self::Top => property_id::MARGIN_TOP,
            Self::Right => property_id::MARGIN_RIGHT,
            Self::Bottom => property_id::MARGIN_BOTTOM,
            Self::Left => property_id::MARGIN_LEFT,
        }
    }

    fn keyword(self) -> u16 {
        match self {
            Self::Top => keyword::TOP,
            Self::Right => keyword::RIGHT,
            Self::Bottom => keyword::BOTTOM,
            Self::Left => keyword::LEFT,
        }
    }

    fn from_keyword(code: u16) -> Option<Self> {
        match code {
            keyword::TOP => Some(Self::Top),
            keyword::RIGHT => Some(Self::Right),
            keyword::BOTTOM => Some(Self::Bottom),
            keyword::LEFT => Some(Self::Left),
            _ => None,
        }
    }
}

/// The physical start sides of a writing mode.
#[derive(Clone, Copy)]
struct WritingModeSides {
    writing_mode: u8,
    direction: u8,
}

impl WritingModeSides {
    /// The start side of the horizontal or vertical axis.
    fn start_of_axis(self, horizontal: bool) -> Side {
        let left_or_top = crate::layout::geometry::start_is_left_or_top(self.writing_mode, self.direction, horizontal);
        match (horizontal, left_or_top) {
            (true, true) => Side::Left,
            (true, false) => Side::Right,
            (false, true) => Side::Top,
            (false, false) => Side::Bottom,
        }
    }

    fn inline_axis_is_horizontal(self) -> bool {
        self.writing_mode == writing_mode::HORIZONTAL_TB
    }

    fn inline_start(self) -> Side {
        self.start_of_axis(self.inline_axis_is_horizontal())
    }

    fn block_start(self) -> Side {
        self.start_of_axis(!self.inline_axis_is_horizontal())
    }
}

/// https://drafts.csswg.org/css-anchor-position-1/#typedef-position-try-fallbacks-try-tactic
/// The try tactics of a position option, composed in order: for each physical side, the side its values move to.
#[derive(Clone, Copy, PartialEq, Eq)]
struct SideMapping([Side; 4]);

impl SideMapping {
    const IDENTITY: Self = Self(Side::ALL);

    fn of_tactics(tactics: &[u8], containing_block: WritingModeSides) -> Self {
        let mut mapping = Self::IDENTITY;
        for &tactic in tactics {
            let (block_start, inline_start) = (containing_block.block_start(), containing_block.inline_start());
            mapping = match tactic {
                // swaps the values in the block axis
                try_tactic::FLIP_BLOCK => mapping.then_swapping(block_start, block_start.opposite()),
                // swaps the values in the inline axis
                try_tactic::FLIP_INLINE => mapping.then_swapping(inline_start, inline_start.opposite()),
                // swaps the values in the horizontal axis
                try_tactic::FLIP_X => mapping.then_swapping(Side::Left, Side::Right),
                // swaps the values in the vertical axis
                try_tactic::FLIP_Y => mapping.then_swapping(Side::Top, Side::Bottom),
                // swaps the values of the start properties with each other, and the end properties with each other
                try_tactic::FLIP_START => mapping
                    .then_swapping(block_start, inline_start)
                    .then_swapping(block_start.opposite(), inline_start.opposite()),
                _ => mapping,
            };
        }
        mapping
    }

    fn then_swapping(self, first: Side, second: Side) -> Self {
        Self(self.0.map(|side| {
            if side == first {
                second
            } else if side == second {
                first
            } else {
                side
            }
        }))
    }

    fn get(self, side: Side) -> Side {
        self.0[side.index()]
    }

    /// Whether the values of each axis move to the other axis.
    fn transposes(self) -> bool {
        self.get(Side::Top).is_horizontal()
    }
}

/// What executing a position option's try tactics on a box needs to know.
#[derive(Clone, Copy)]
struct TacticContext {
    mapping: SideMapping,
    containing_block: WritingModeSides,
    own: WritingModeSides,
}

impl TacticContext {
    /// https://drafts.csswg.org/css-anchor-position-1/#valdef-anchor-percentage
    /// Whether a position along the axis of `side`, measured from the start side of that axis, is measured from the
    /// end side of the axis its values move to.
    fn reverses_axis_of(self, side: Side) -> bool {
        let start = self.containing_block.start_of_axis(side.is_horizontal());
        let moved_start = self.mapping.get(start);
        moved_start != self.containing_block.start_of_axis(moved_start.is_horizontal())
    }

    /// The anchor() side `side`, in an inset property on the physical side `inset_side`, changed to maintain the same
    /// relative relationship to the side its value moves to.
    fn anchor_side(self, side: &StyleValueData, inset_side: Side) -> Option<StyleValueData> {
        let horizontal = inset_side.is_horizontal();
        let physical = match side {
            StyleValueData::Keyword { keyword: code } => match *code {
                keyword::START => self.containing_block.start_of_axis(horizontal),
                keyword::END => self.containing_block.start_of_axis(horizontal).opposite(),
                keyword::SELF_START => self.own.start_of_axis(horizontal),
                keyword::SELF_END => self.own.start_of_axis(horizontal).opposite(),
                code => Side::from_keyword(code)?,
            },
            // If a <percentage> is used, and directions are opposing, change it to 100% minus the original
            // percentage.
            StyleValueData::Percentage { value } => {
                return self
                    .reverses_axis_of(inset_side)
                    .then(|| StyleValueData::Percentage { value: 100.0 - value });
            }
            StyleValueData::Calculated { .. } => {
                // NB: An <anchor-side> calculation holds percentages alone, which resolve against a basis of 100 to
                //     the percentage they add up to.
                let value = crate::css::calc::resolve_calculated_percentage_without_context(side)
                    .or_else(|| crate::css::calc::resolve_calculated_length_without_context(side, 100.0))?;
                return Some(StyleValueData::Percentage {
                    value: if self.reverses_axis_of(inset_side) {
                        100.0 - value
                    } else {
                        value
                    },
                });
            }
            _ => return None,
        };
        Some(StyleValueData::Keyword {
            keyword: self.mapping.get(physical).keyword(),
        })
    }

    /// `value`, moving from a property of the physical side `side` (or of its axis), with the anchor() and
    /// anchor-size() functions in it changed to maintain the same relative relationship to the new direction.
    fn moved_value(self, value: &RetainedStyleValueData, side: Side) -> RetainedStyleValueData {
        match value.optional_data().and_then(|data| self.moved_function(data, side)) {
            Some(moved) => RetainedStyleValueData::from_arc(Arc::new(moved)),
            None => value.clone_retained(),
        }
    }

    fn moved_function(self, data: &StyleValueData, side: Side) -> Option<StyleValueData> {
        match data {
            StyleValueData::Anchor {
                has_anchor_name,
                anchor_name,
                anchor_side,
                fallback_value,
            } => Some(StyleValueData::Anchor {
                has_anchor_name: *has_anchor_name,
                anchor_name: anchor_name.clone(),
                anchor_side: match anchor_side
                    .optional_data()
                    .and_then(|data| self.anchor_side(data, side))
                {
                    Some(moved) => RetainedStyleValueData::from_arc(Arc::new(moved)),
                    None => anchor_side.clone_retained(),
                },
                fallback_value: fallback_value.clone_retained(),
            }),
            StyleValueData::AnchorSize {
                has_anchor_name,
                anchor_name,
                has_anchor_size,
                anchor_size: size,
                fallback_value,
            } if *has_anchor_size && self.mapping.transposes() => Some(StyleValueData::AnchorSize {
                has_anchor_name: *has_anchor_name,
                anchor_name: anchor_name.clone(),
                has_anchor_size: true,
                anchor_size: match *size {
                    anchor_size::WIDTH => anchor_size::HEIGHT,
                    anchor_size::HEIGHT => anchor_size::WIDTH,
                    anchor_size::BLOCK => anchor_size::INLINE,
                    anchor_size::INLINE => anchor_size::BLOCK,
                    anchor_size::SELF_BLOCK => anchor_size::SELF_INLINE,
                    anchor_size::SELF_INLINE => anchor_size::SELF_BLOCK,
                    size => size,
                },
                fallback_value: fallback_value.clone_retained(),
            }),
            StyleValueData::Calculated {
                resolve_as_is_number,
                resolve_as_base,
                has_percentages_resolve_as,
                percentages_resolve_as,
                resolve_numbers_as_integers,
                rust_calculation,
                resolved_type,
                accepted_ranges,
            } => {
                let root = crate::css::calc::map_calc_size_leaves(&rust_calculation.node_arc(), &|leaf| match leaf {
                    crate::css::calc::CalcNode::NonMathFunction { value, numeric_type } => {
                        let moved = self.moved_function(value.optional_data()?, side)?;
                        Some(Arc::new(crate::css::calc::CalcNode::NonMathFunction {
                            value: RetainedStyleValueData::from_arc(Arc::new(moved)),
                            numeric_type: *numeric_type,
                        }))
                    }
                    _ => None,
                })?;
                if Arc::ptr_eq(&root, &rust_calculation.node_arc()) {
                    return None;
                }
                Some(StyleValueData::Calculated {
                    resolve_as_is_number: *resolve_as_is_number,
                    resolve_as_base: *resolve_as_base,
                    has_percentages_resolve_as: *has_percentages_resolve_as,
                    percentages_resolve_as: *percentages_resolve_as,
                    resolve_numbers_as_integers: *resolve_numbers_as_integers,
                    rust_calculation: crate::css::calc::CalcNodeHandle::from_arc(root),
                    resolved_type: *resolved_type,
                    accepted_ranges: accepted_ranges.clone(),
                })
            }
            _ => None,
        }
    }

    /// A self-alignment value of the axis whose start side is `start`, moved to the axis that side moves to, where it
    /// starts at `destination_start`: its <self-position> keywords changed to maintain the same relative relationship
    /// to the new direction.
    fn moved_self_alignment(
        self,
        value: &RetainedStyleValueData,
        start: Side,
        destination_start: Side,
    ) -> RetainedStyleValueData {
        let moved_start = self.mapping.get(start);
        let reversed = moved_start != destination_start;
        let destination_is_horizontal = destination_start.is_horizontal();
        let destination_is_inline = destination_is_horizontal == self.containing_block.inline_axis_is_horizontal();
        let moved_keyword = |code: u16| -> u16 {
            match code {
                keyword::START
                | keyword::END
                | keyword::SELF_START
                | keyword::SELF_END
                | keyword::FLEX_START
                | keyword::FLEX_END
                    if reversed =>
                {
                    match code {
                        keyword::START => keyword::END,
                        keyword::END => keyword::START,
                        keyword::SELF_START => keyword::SELF_END,
                        keyword::SELF_END => keyword::SELF_START,
                        keyword::FLEX_START => keyword::FLEX_END,
                        _ => keyword::FLEX_START,
                    }
                }
                // https://drafts.csswg.org/css-align-3/#valdef-justify-self-left
                // The alignment subject is flush with the alignment container's line-left or physical left edge,
                // whichever is in the appropriate axis.
                keyword::LEFT | keyword::RIGHT => {
                    let line_left = WritingModeSides {
                        writing_mode: self.containing_block.writing_mode,
                        direction: crate::css::css_enums::direction::LTR,
                    }
                    .inline_start();
                    let side = if code == keyword::LEFT {
                        line_left
                    } else {
                        line_left.opposite()
                    };
                    let moved = self.mapping.get(side);
                    if destination_is_inline && moved == line_left {
                        keyword::LEFT
                    } else if destination_is_inline && moved == line_left.opposite() {
                        keyword::RIGHT
                    } else if moved == destination_start {
                        keyword::START
                    } else {
                        keyword::END
                    }
                }
                code => code,
            }
        };
        let moved = match value.optional_data() {
            Some(StyleValueData::Keyword { keyword: code }) => {
                let moved = moved_keyword(*code);
                (moved != *code).then_some(StyleValueData::Keyword { keyword: moved })
            }
            Some(StyleValueData::ValueList {
                values,
                separator,
                collapsible,
            }) => {
                let mut changed = false;
                let items = values
                    .as_slice()
                    .iter()
                    .map(|item| match item.optional_data() {
                        Some(StyleValueData::Keyword { keyword: code }) if moved_keyword(*code) != *code => {
                            changed = true;
                            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Keyword {
                                keyword: moved_keyword(*code),
                            }))
                        }
                        _ => item.clone_retained(),
                    })
                    .collect();
                changed.then(|| StyleValueData::ValueList {
                    values: crate::css::style_value::RetainedStyleValueDataList::from_retained_values(items),
                    separator: *separator,
                    collapsible: *collapsible,
                })
            }
            _ => None,
        };
        match moved {
            Some(moved) => RetainedStyleValueData::from_arc(Arc::new(moved)),
            None => value.clone_retained(),
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#execute-a-try-tactic
    /// Swap the values of `table` between the associated properties corresponding to the directions, and modify the
    /// values of the properties as they swap to match the new directions.
    fn execute(self, base: &ComputedLonghandTable, table: &mut ComputedLonghandTable) {
        let value = |property: u16| base.get(property).map(RetainedStyleValueData::clone_retained);
        for side in Side::ALL {
            let destination = self.mapping.get(side);
            if let Some(inset) = value(side.inset_property()) {
                table.set(destination.inset_property(), self.moved_value(&inset, side), -1);
            }
            if let Some(margin) = value(side.margin_property()) {
                table.set(destination.margin_property(), self.moved_value(&margin, side), -1);
            }
        }
        let size_pairs = [
            (property_id::WIDTH, property_id::HEIGHT),
            (property_id::MIN_WIDTH, property_id::MIN_HEIGHT),
            (property_id::MAX_WIDTH, property_id::MAX_HEIGHT),
        ];
        for (horizontal, vertical) in size_pairs {
            let (horizontal_destination, vertical_destination) = if self.mapping.transposes() {
                (vertical, horizontal)
            } else {
                (horizontal, vertical)
            };
            if let Some(size) = value(horizontal) {
                table.set(horizontal_destination, self.moved_value(&size, Side::Left), -1);
            }
            if let Some(size) = value(vertical) {
                table.set(vertical_destination, self.moved_value(&size, Side::Top), -1);
            }
        }
        // NB: justify-self aligns in the inline axis of the containing block, and align-self in its block axis.
        let inline_start = self.containing_block.inline_start();
        let block_start = self.containing_block.block_start();
        let (justify_destination, align_destination) = if self.mapping.transposes() {
            (property_id::ALIGN_SELF, property_id::JUSTIFY_SELF)
        } else {
            (property_id::JUSTIFY_SELF, property_id::ALIGN_SELF)
        };
        let destination_start = |property: u16| {
            if property == property_id::JUSTIFY_SELF {
                inline_start
            } else {
                block_start
            }
        };
        if let Some(justify_self) = value(property_id::JUSTIFY_SELF) {
            let moved = self.moved_self_alignment(&justify_self, inline_start, destination_start(justify_destination));
            table.set(justify_destination, moved, -1);
        }
        if let Some(align_self) = value(property_id::ALIGN_SELF) {
            let moved = self.moved_self_alignment(&align_self, block_start, destination_start(align_destination));
            table.set(align_destination, moved, -1);
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#execute-a-try-tactic
    /// For position-area, change the value so that the selected rows/columns of the position-area grid maintain the
    /// same relative relationship to the new direction that they had to the old.
    fn moved_position_area(self, codes: &[u8]) -> Option<Vec<u8>> {
        let tracks = crate::layout::abspos_engine::resolve_position_area_tracks(
            codes,
            crate::layout::abspos_engine::PositionAreaWritingMode {
                writing_mode: self.containing_block.writing_mode,
                direction: self.containing_block.direction,
            },
            crate::layout::abspos_engine::PositionAreaWritingMode {
                writing_mode: self.own.writing_mode,
                direction: self.own.direction,
            },
        )?;
        // NB: A range counts tracks from the left or top side of its axis.
        let moved = |range: crate::layout::abspos_engine::PositionAreaTrackRange, low_side: Side| {
            let destination = self.mapping.get(low_side);
            let range = if matches!(destination, Side::Right | Side::Bottom) {
                range.reversed()
            } else {
                range
            };
            (destination.is_horizontal(), range)
        };
        let mut horizontal = tracks.horizontal;
        let mut vertical = tracks.vertical;
        for (is_horizontal, range) in [moved(tracks.horizontal, Side::Left), moved(tracks.vertical, Side::Top)] {
            if is_horizontal {
                horizontal = range;
            } else {
                vertical = range;
            }
        }
        let code = |range: crate::layout::abspos_engine::PositionAreaTrackRange, horizontal: bool| match (
            range.first,
            range.last,
            horizontal,
        ) {
            (0, 0, true) => position_area::LEFT,
            (2, 2, true) => position_area::RIGHT,
            (0, 1, true) => position_area::SPAN_LEFT,
            (1, 2, true) => position_area::SPAN_RIGHT,
            (0, 0, false) => position_area::TOP,
            (2, 2, false) => position_area::BOTTOM,
            (0, 1, false) => position_area::SPAN_TOP,
            (1, 2, false) => position_area::SPAN_BOTTOM,
            (1, 1, _) => position_area::CENTER,
            _ => position_area::SPAN_ALL,
        };
        let horizontal_code = code(horizontal, true);
        let vertical_code = code(vertical, false);
        let is_ambiguous = |code: u8| matches!(code, position_area::CENTER | position_area::SPAN_ALL);
        // NB: Where both keywords are ambiguous about their axis, the first refers to the block axis of the
        //     containing block.
        Some(
            if is_ambiguous(horizontal_code)
                && is_ambiguous(vertical_code)
                && self.containing_block.inline_axis_is_horizontal()
            {
                vec![vertical_code, horizontal_code]
            } else {
                vec![horizontal_code, vertical_code]
            },
        )
    }
}

impl StyleEngine {
    /// https://drafts.csswg.org/css-anchor-position-1/#position-options-list
    /// The first `limit` entries of the `position-try-fallbacks` of the box of `node`, whose record is `record`, that
    /// add a position option to its position options list: the index of each, and the identity of the `@position-try`
    /// rule it names, or 0 for an entry that names none.
    pub(crate) fn position_option_entries(&self, node: StyleNodeID, record: u64, limit: usize) -> Vec<(usize, usize)> {
        let Some(view) = self.computed_group_sets.style_record_view(record) else {
            return Vec::new();
        };
        // SAFETY: A live record's payloads are live payloads of their groups.
        let anchor = unsafe { view.payloads[group_index::ANCHOR].cast::<AnchorValues>().deref() };
        anchor
            .position_try_fallbacks
            .as_slice()
            .iter()
            .enumerate()
            .filter_map(|(index, fallback)| {
                let name = fallback.name.to_utf16();
                if name.is_empty() {
                    return Some((index, 0));
                }
                // <dashed-ident>: If there is a @position-try rule with the given name, its associated position
                // option is added to the position options list. Otherwise, this value has no effect.
                let declarations = self.resolve_position_try_rule(node, &name)?;
                Some((index, Arc::as_ptr(&declarations) as usize))
            })
            .take(limit)
            .collect()
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#apply-a-position-option
    /// The style record of the box of `node`, whose record is `record`, with the position option that the entry
    /// `index` of its `position-try-fallbacks` makes applied, pinned for a layout row. Logical directions are resolved
    /// against the writing mode of the containing block, `containing_block_writing_mode` and
    /// `containing_block_direction`. Answers none where the entry adds no position option to the box's position
    /// options list.
    pub(crate) fn position_option_record(
        &mut self,
        node: StyleNodeID,
        record: u64,
        index: usize,
        containing_block_writing_mode: u8,
        containing_block_direction: u8,
    ) -> Option<DerivedStyleRecord> {
        let view = self.computed_group_sets.style_record_view(record)?;
        let base = view.longhand_table?;
        // SAFETY: A live record's payloads are live payloads of their groups.
        let (anchor, inherited_box) = unsafe {
            (
                view.payloads[group_index::ANCHOR].cast::<AnchorValues>().deref(),
                view.payloads[group_index::INHERITED_BOX]
                    .cast::<InheritedBoxValues>()
                    .deref(),
            )
        };
        let fallback = anchor.position_try_fallbacks.as_slice().get(index)?;
        let own = WritingModeSides {
            writing_mode: inherited_box.writing_mode,
            direction: inherited_box.direction,
        };
        let context = TacticContext {
            mapping: SideMapping::of_tactics(
                &fallback.tactics[..fallback.tactic_count],
                WritingModeSides {
                    writing_mode: containing_block_writing_mode,
                    direction: containing_block_direction,
                },
            ),
            containing_block: WritingModeSides {
                writing_mode: containing_block_writing_mode,
                direction: containing_block_direction,
            },
            own,
        };

        let name = fallback.name.to_utf16();
        // <dashed-ident>: If there is a @position-try rule with the given name, its associated position option is
        // added to the position options list. Otherwise, this value has no effect.
        let declarations = if name.is_empty() {
            None
        } else {
            Some(self.resolve_position_try_rule(node, &name)?)
        };
        let mut table = ComputedLonghandTable::copied_for_partial_drive(base);
        if let Some(declarations) = declarations {
            self.apply_position_try_declarations(node, record, own, &declarations, &mut table)?;
        }
        if context.mapping != SideMapping::IDENTITY {
            let mut transformed = ComputedLonghandTable::copied_for_partial_drive(&table);
            context.execute(&table, &mut transformed);
            table = transformed;
        }
        table.freeze();

        // NB: The Animation Origin lies above the Position Fallback Origin.
        let animated_overlay = self
            .computed_group_sets
            .style_record_view(record)?
            .animated_overlay
            .cloned();
        let mut style = LayoutStyle::from_record(self, record);
        style.replace_groups_from_table(&table, animated_overlay.as_ref(), &POSITION_OPTION_GROUPS);
        // <position-area>: Automatically creates a position option composed solely of a position-area property with
        // the given value.
        let position_area = if fallback.has_position_area {
            Some(fallback.position_area.as_slice().to_vec())
        } else if context.mapping != SideMapping::IDENTITY {
            let codes = style.position_area();
            (!codes.is_empty())
                .then(|| context.moved_position_area(&codes))
                .flatten()
        } else {
            None
        };
        if let Some(codes) = position_area {
            style.set_position_area(codes);
        }
        if style.is_unchanged() {
            return Some(DerivedStyleRecord::pin(self, record));
        }
        Some(style.intern(self))
    }

    /// https://drafts.csswg.org/css-shadow-1/#tree-scoped-name
    /// The declarations of the `@position-try` rule of this name that the element of `node` refers to: the one of the
    /// closest tree scope around the element that defines one.
    fn resolve_position_try_rule(&self, node: StyleNodeID, name: &[u16]) -> Option<Arc<DeclarationBlockData>> {
        let mut scope = self.tree.tree_scope(node);
        loop {
            if let Some(declarations) = self.position_try_rules.in_scope(scope, name) {
                return Some(declarations.clone());
            }
            if scope == TreeScopeID::DOCUMENT {
                return None;
            }
            scope = self
                .scope_root(scope)
                .and_then(|root| self.tree.host_of(root))
                .map_or(TreeScopeID::DOCUMENT, |host| self.tree.tree_scope(host));
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#fallback-rule
    /// All of the properties in a @position-try are applied to the box as part of the Position Fallback Origin, a new
    /// cascade origin that lies between the Author Origin and the Animation Origin.
    /// Computes the accepted properties `declarations` declares for the box of `node`, whose record is `record`, and
    /// stores them in `table`, unless an important declaration of the base styles wins over them.
    fn apply_position_try_declarations(
        &self,
        node: StyleNodeID,
        record: u64,
        own: WritingModeSides,
        declarations: &DeclarationBlockData,
        table: &mut ComputedLonghandTable,
    ) -> Option<()> {
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
        use crate::css::style_compute::{
            FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiInputLineHeightMetrics, FfiLengthResolutionContext,
            FfiStyleComputationEnvironment, LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation,
            empty_longhand_driver_results, expand_shorthands_with, is_required_driver_input,
            map_logical_alias_to_physical, parent_snapshot_for_style_record,
        };

        let mut declared: Vec<(u16, RetainedStyleValueData)> = Vec::new();
        // NB: Arbitrary substitution functions substitute against the custom properties of the element.
        let custom_property_store = |node: Option<StyleNodeID>| {
            node.and_then(|node| self.computed_group_sets.custom_property_environment_identity(node))
                .and_then(|identity| self.custom_property_environments.store(identity))
                .map_or(std::ptr::null(), |store| Arc::as_ptr(store).cast::<std::ffi::c_void>())
        };
        let store = custom_property_store(Some(node));
        let inheritance_store = custom_property_store(self.tree.inheritance_parent(node));
        for declaration in &declarations.properties {
            let mut value = RetainedStyleValueData::from_arc(declaration.value.clone());
            if matches!(value.optional_data(), Some(StyleValueData::Unresolved { .. })) {
                value = self.substitute_keyframe_value(
                    node,
                    None,
                    store,
                    inheritance_store,
                    declaration.property_id,
                    &[],
                    &value,
                    &std::cell::RefCell::new(None),
                );
            }
            expand_shorthands_with(declaration.property_id, &value, false, &mut |longhand, data, _| {
                let longhand = map_logical_alias_to_physical(longhand, own.writing_mode, own.direction);
                if !ACCEPTED_POSITION_TRY_LONGHANDS.contains(&longhand) {
                    return;
                }
                let data = match data.optional_data() {
                    None => return,
                    // https://drafts.csswg.org/css-variables-2/#invalid-at-computed-value-time
                    // NB: A value that did not substitute is invalid at computed-value time, and computes as if
                    //     unset.
                    Some(
                        StyleValueData::Unresolved { .. }
                        | StyleValueData::PendingSubstitution { .. }
                        | StyleValueData::GuaranteedInvalid,
                    ) => RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Keyword {
                        keyword: keyword::UNSET,
                    })),
                    // As with the Animation Origin, revert-layer has no special behavior and acts as specified,
                    // reverting to the base styles.
                    Some(StyleValueData::Keyword {
                        keyword: keyword::REVERT_LAYER,
                    }) => return,
                    // Similar to the Animation Origin, use of the revert value acts as if the property was part of
                    // the Author Origin, so that it instead reverts back to the User Origin.
                    // NB: No user style sheet sets an accepted @position-try property, so this is unset.
                    Some(StyleValueData::Keyword {
                        keyword: keyword::REVERT,
                    }) => RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Keyword {
                        keyword: keyword::UNSET,
                    })),
                    Some(_) => data.clone_retained(),
                };
                declared.retain(|(property, _)| *property != longhand);
                declared.push((longhand, data));
            });
        }
        let base = self.computed_group_sets.style_record_view(record)?.longhand_table?;
        declared.retain(|(property, _)| !base.is_important(*property));
        if declared.is_empty() {
            return Some(());
        }

        let view = self.computed_group_sets.style_record_view(record)?;
        let snapshot = self
            .tree
            .inheritance_parent(node)
            .and_then(|parent| self.computed_group_sets.assigned_style_record(parent))
            .and_then(|parent_record| {
                let parent_view = self.computed_group_sets.style_record_view(parent_record.raw())?;
                Some(parent_snapshot_for_style_record(
                    self,
                    parent_record.raw(),
                    parent_view.animated_overlay,
                ))
            });
        // SAFETY: A live record's payloads are live payloads of their groups.
        let font = unsafe { view.payloads[group_index::FONT].cast::<FontValues>().deref() };
        let inputs = &self.document_style_computation_inputs;
        let font_metric = |value: f32| crate::css::css_pixels::CssPixels::nearest_value_for_f32(value).to_double();
        let mut resolved_viewport_relative_length = false;
        let mut length = FfiLengthResolutionContext {
            viewport_width: inputs.viewport_width,
            viewport_height: inputs.viewport_height,
            font_metrics: FfiFontMetrics {
                font_size: font.font_size.to_double(),
                x_height: font_metric(font.font_x_height),
                // The C++ metrics approximate the cap height with the ascent.
                cap_height: font_metric(font.font_ascent),
                zero_advance: font_metric(font.font_zero_advance),
                line_height: font.line_height_used.to_double(),
            },
            root_font_metrics: FfiFontMetrics {
                font_size: inputs.root_font_size,
                x_height: inputs.root_font_x_height,
                cap_height: inputs.root_font_cap_height,
                zero_advance: inputs.root_font_zero_advance,
                line_height: inputs.root_line_height,
            },
            font_metrics_depend_on_viewport_metrics: false,
            root_font_metrics_depend_on_viewport_metrics: inputs.root_font_metrics_depend_on_viewport_metrics,
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: own.inline_axis_is_horizontal(),
            resolved_viewport_relative_length: &raw mut resolved_viewport_relative_length,
        };
        self.container_unit_bases(node).apply_to(&mut length);
        let environment = FfiStyleComputationEnvironment {
            box_type_input: self.box_type_transformation_input(
                self.computed_group_sets.adjustment_facts(node),
                crate::css::style_compute::FfiStyleAdjustmentTarget::Element,
                self.tree.inheritance_parent(node),
            ),
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: 0,
                has_document_supported_schemes: false,
                document_supported_scheme_codes: std::ptr::null(),
                document_supported_scheme_count: 0,
            },
            is_th_element: false,
            has_new_font_size: false,
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: self.document_resource_contexts.document_base_url.as_ptr(),
            document_base_url_length: self.document_resource_contexts.document_base_url.len(),
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
        };

        const LONGHAND_WORD_COUNT: usize = crate::css::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES.div_ceil(64);
        let mut store = crate::css::cascaded_properties::CascadedPropertyStore::new();
        for property in FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID {
            if !is_required_driver_input(property) {
                continue;
            }
            if let Some(value) = base.get(property) {
                store.seed_retained_property(property, value.clone_retained(), false, false);
            }
        }
        let mut selected = [0u64; LONGHAND_WORD_COUNT];
        for (property, value) in &declared {
            store.seed_retained_property(*property, value.clone_retained(), false, false);
            let index = usize::from(*property - FIRST_LONGHAND_PROPERTY_ID);
            selected[index / 64] |= 1 << (index % 64);
        }
        let mut driven = view.longhand_table_for_partial_drive();
        let mut results = empty_longhand_driver_results();
        let mut effective_color_scheme = base.effective_color_scheme();
        let input_line_height_metrics = FfiInputLineHeightMetrics {
            current_line_height: 0.0,
            minimum_line_height: 0.0,
        };
        // SAFETY: Every pointer the environment and length context hold stays live across the drive.
        unsafe {
            drive_property_computation(
                &mut driven,
                &store,
                snapshot.as_ref(),
                None,
                &environment,
                Some(&selected),
                LONGHAND_DRIVE_PHASE_REMAINING,
                Some(&length),
                Some(&input_line_height_metrics),
                &mut results,
                &mut effective_color_scheme,
                false,
            );
        }
        for (property, _) in &declared {
            if let Some(value) = driven.get(*property) {
                table.set(*property, value.clone_retained(), -1);
            }
        }
        Some(())
    }
}
