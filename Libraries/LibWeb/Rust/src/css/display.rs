/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS `display` value and its predicates, mirroring `Web::CSS::Display`.

use crate::css::css_enums::{
    display_box, display_inside, display_internal, display_outside, keyword, keyword_to_display_internal,
};

const DISPLAY_BOX_CONTENTS: u8 = display_box::CONTENTS;
const DISPLAY_BOX_NONE: u8 = display_box::NONE;
const DISPLAY_INSIDE_FLOW: u8 = display_inside::FLOW;
const DISPLAY_INSIDE_FLOW_ROOT: u8 = display_inside::FLOW_ROOT;
const DISPLAY_INSIDE_TABLE: u8 = display_inside::TABLE;
const DISPLAY_INSIDE_FLEX: u8 = display_inside::FLEX;
const DISPLAY_INSIDE_GRID: u8 = display_inside::GRID;
const DISPLAY_INSIDE_RUBY: u8 = display_inside::RUBY;
const DISPLAY_INSIDE_MATH: u8 = display_inside::MATH;
const DISPLAY_INSIDE_WEBKIT_BOX: u8 = display_inside::_WEBKIT_BOX;
const DISPLAY_INTERNAL_TABLE_ROW_GROUP: u8 = display_internal::TABLE_ROW_GROUP;
const DISPLAY_INTERNAL_TABLE_HEADER_GROUP: u8 = display_internal::TABLE_HEADER_GROUP;
const DISPLAY_INTERNAL_TABLE_FOOTER_GROUP: u8 = display_internal::TABLE_FOOTER_GROUP;
const DISPLAY_INTERNAL_TABLE_ROW: u8 = display_internal::TABLE_ROW;
const DISPLAY_INTERNAL_TABLE_CELL: u8 = display_internal::TABLE_CELL;
const DISPLAY_INTERNAL_TABLE_COLUMN_GROUP: u8 = display_internal::TABLE_COLUMN_GROUP;
const DISPLAY_INTERNAL_TABLE_COLUMN: u8 = display_internal::TABLE_COLUMN;
const DISPLAY_INTERNAL_TABLE_CAPTION: u8 = display_internal::TABLE_CAPTION;
const DISPLAY_OUTSIDE_BLOCK: u8 = display_outside::BLOCK;
const DISPLAY_OUTSIDE_INLINE: u8 = display_outside::INLINE;

/// Mirror of the CSS Display value; the C++ tagged union crosses as explicit
/// fields, with the unused fields zeroed so equality is field-wise. `tag` uses
/// the same discriminants as Display::Type.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FfiDisplay {
    /// 0 = outside-and-inside, 1 = internal, 2 = box.
    pub tag: u8,
    pub outside: u8,
    pub inside: u8,
    pub list_item: bool,
    pub internal: u8,
    pub box_value: u8,
}

pub const DISPLAY_TAG_OUTSIDE_AND_INSIDE: u8 = 0;
pub const DISPLAY_TAG_INTERNAL: u8 = 1;
pub const DISPLAY_TAG_BOX: u8 = 2;

impl FfiDisplay {
    pub fn from_raw(raw: u32) -> Self {
        let [tag, first, second, third] = raw.to_le_bytes();
        match tag {
            DISPLAY_TAG_OUTSIDE_AND_INSIDE => Self::outside_and_inside(first, second, third != 0),
            DISPLAY_TAG_INTERNAL => Self::internal(first),
            DISPLAY_TAG_BOX => Self {
                tag,
                outside: 0,
                inside: 0,
                list_item: false,
                internal: 0,
                box_value: first,
            },
            _ => unreachable!("invalid display tag"),
        }
    }

    pub fn outside_and_inside(outside: u8, inside: u8, list_item: bool) -> Self {
        Self {
            tag: DISPLAY_TAG_OUTSIDE_AND_INSIDE,
            outside,
            inside,
            list_item,
            internal: 0,
            box_value: 0,
        }
    }

    pub fn internal(internal: u8) -> Self {
        Self {
            tag: DISPLAY_TAG_INTERNAL,
            outside: 0,
            inside: 0,
            list_item: false,
            internal,
            box_value: 0,
        }
    }

    pub fn block() -> Self {
        Self::outside_and_inside(DISPLAY_OUTSIDE_BLOCK, DISPLAY_INSIDE_FLOW, false)
    }

    pub fn inline() -> Self {
        Self::outside_and_inside(DISPLAY_OUTSIDE_INLINE, DISPLAY_INSIDE_FLOW, false)
    }

    pub fn inline_block() -> Self {
        Self::outside_and_inside(DISPLAY_OUTSIDE_INLINE, DISPLAY_INSIDE_FLOW_ROOT, false)
    }

    pub fn flow_root() -> Self {
        Self::outside_and_inside(DISPLAY_OUTSIDE_BLOCK, DISPLAY_INSIDE_FLOW_ROOT, false)
    }

    pub fn table() -> Self {
        Self::outside_and_inside(DISPLAY_OUTSIDE_BLOCK, DISPLAY_INSIDE_TABLE, false)
    }

    pub fn inline_table() -> Self {
        Self::outside_and_inside(DISPLAY_OUTSIDE_INLINE, DISPLAY_INSIDE_TABLE, false)
    }

    pub fn encoded(&self) -> u32 {
        let (first, second, third) = match self.tag {
            DISPLAY_TAG_OUTSIDE_AND_INSIDE => (self.outside, self.inside, self.list_item as u8),
            DISPLAY_TAG_INTERNAL => (self.internal, 0, 0),
            DISPLAY_TAG_BOX => (self.box_value, 0, 0),
            _ => unreachable!("invalid display tag"),
        };
        self.tag as u32 | (first as u32) << 8 | (second as u32) << 16 | (third as u32) << 24
    }

    pub fn none() -> Self {
        Self {
            tag: DISPLAY_TAG_BOX,
            outside: 0,
            inside: 0,
            list_item: false,
            internal: 0,
            box_value: DISPLAY_BOX_NONE,
        }
    }

    pub fn contents() -> Self {
        Self {
            tag: DISPLAY_TAG_BOX,
            outside: 0,
            inside: 0,
            list_item: false,
            internal: 0,
            box_value: DISPLAY_BOX_CONTENTS,
        }
    }

    pub(crate) fn from_single_keyword(value: u16) -> Option<Self> {
        let display = match value {
            keyword::NONE => Self::none(),
            keyword::CONTENTS => Self::contents(),
            keyword::BLOCK | keyword::FLOW => Self::block(),
            keyword::FLOW_ROOT => Self::flow_root(),
            keyword::INLINE => Self::inline(),
            keyword::INLINE_BLOCK => Self::inline_block(),
            keyword::RUN_IN => Self::outside_and_inside(display_outside::RUN_IN, display_inside::FLOW, false),
            keyword::LIST_ITEM => Self::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, true),
            keyword::FLEX | keyword::_WEBKIT_FLEX => {
                Self::outside_and_inside(display_outside::BLOCK, display_inside::FLEX, false)
            }
            keyword::INLINE_FLEX | keyword::_WEBKIT_INLINE_FLEX => {
                Self::outside_and_inside(display_outside::INLINE, display_inside::FLEX, false)
            }
            keyword::GRID => Self::outside_and_inside(display_outside::BLOCK, display_inside::GRID, false),
            keyword::INLINE_GRID => Self::outside_and_inside(display_outside::INLINE, display_inside::GRID, false),
            keyword::RUBY => Self::outside_and_inside(display_outside::INLINE, display_inside::RUBY, false),
            keyword::TABLE => Self::table(),
            keyword::INLINE_TABLE => Self::inline_table(),
            keyword::MATH => Self::outside_and_inside(display_outside::INLINE, display_inside::MATH, false),
            keyword::_WEBKIT_BOX => {
                Self::outside_and_inside(display_outside::BLOCK, display_inside::_WEBKIT_BOX, false)
            }
            keyword::_WEBKIT_INLINE_BOX => {
                Self::outside_and_inside(display_outside::INLINE, display_inside::_WEBKIT_BOX, false)
            }
            _ => Self::internal(keyword_to_display_internal(value)?),
        };
        Some(display)
    }

    pub fn is_outside_and_inside(&self) -> bool {
        self.tag == DISPLAY_TAG_OUTSIDE_AND_INSIDE
    }

    pub fn is_internal(&self) -> bool {
        self.tag == DISPLAY_TAG_INTERNAL
    }

    pub fn is_none(&self) -> bool {
        self.tag == DISPLAY_TAG_BOX && self.box_value == DISPLAY_BOX_NONE
    }

    pub fn is_contents(&self) -> bool {
        self.tag == DISPLAY_TAG_BOX && self.box_value == DISPLAY_BOX_CONTENTS
    }

    pub fn is_block_outside(&self) -> bool {
        self.is_outside_and_inside() && self.outside == DISPLAY_OUTSIDE_BLOCK
    }

    pub fn is_inline_outside(&self) -> bool {
        self.is_outside_and_inside() && self.outside == DISPLAY_OUTSIDE_INLINE
    }

    pub fn is_inline_block(&self) -> bool {
        self.is_inline_outside() && self.is_flow_root_inside()
    }

    pub fn is_list_item(&self) -> bool {
        self.is_outside_and_inside() && self.list_item
    }

    pub fn is_flow_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == DISPLAY_INSIDE_FLOW
    }

    pub fn is_flow_root_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == DISPLAY_INSIDE_FLOW_ROOT
    }

    pub fn is_table_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == DISPLAY_INSIDE_TABLE
    }

    pub fn is_flex_inside(&self) -> bool {
        self.is_outside_and_inside() && (self.inside == DISPLAY_INSIDE_FLEX || self.inside == DISPLAY_INSIDE_WEBKIT_BOX)
    }

    pub fn is_webkit_box_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == DISPLAY_INSIDE_WEBKIT_BOX
    }

    pub fn is_grid_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == DISPLAY_INSIDE_GRID
    }

    pub fn is_ruby_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == DISPLAY_INSIDE_RUBY
    }

    pub fn is_math_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == DISPLAY_INSIDE_MATH
    }

    pub fn is_table_row(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_ROW
    }

    pub fn is_table_cell(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_CELL
    }

    pub fn is_table_column(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_COLUMN
    }

    pub fn is_table_column_group(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_COLUMN_GROUP
    }

    pub fn is_table_row_group(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_ROW_GROUP
    }

    pub fn is_table_header_group(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_HEADER_GROUP
    }

    pub fn is_table_footer_group(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_FOOTER_GROUP
    }

    // https://drafts.csswg.org/css-tables-3/#terminology
    // Mentions of table-row-groups in the tables spec also encompass the
    // specialized table-header-groups and table-footer-groups.
    pub fn is_table_row_group_kind(&self) -> bool {
        self.is_table_row_group() || self.is_table_header_group() || self.is_table_footer_group()
    }

    pub fn is_table_caption(&self) -> bool {
        self.is_internal() && self.internal == DISPLAY_INTERNAL_TABLE_CAPTION
    }

    // https://drafts.csswg.org/css-display-3/#internal-table-element
    pub fn is_internal_table(&self) -> bool {
        self.is_internal()
            && (self.internal == DISPLAY_INTERNAL_TABLE_ROW_GROUP
                || self.internal == DISPLAY_INTERNAL_TABLE_HEADER_GROUP
                || self.internal == DISPLAY_INTERNAL_TABLE_FOOTER_GROUP
                || self.internal == DISPLAY_INTERNAL_TABLE_ROW
                || self.internal == DISPLAY_INTERNAL_TABLE_CELL
                || self.internal == DISPLAY_INTERNAL_TABLE_COLUMN_GROUP
                || self.internal == DISPLAY_INTERNAL_TABLE_COLUMN)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display_raw_value_decodes_for_box_type_transformation() {
        let inline_flex = FfiDisplay::from_raw(u32::from_ne_bytes([
            DISPLAY_TAG_OUTSIDE_AND_INSIDE,
            display_outside::INLINE,
            display_inside::FLEX,
            0,
        ]));
        assert!(inline_flex.is_inline_outside());
        assert!(inline_flex.is_flex_inside());

        let none = FfiDisplay::from_raw(u32::from_ne_bytes([DISPLAY_TAG_BOX, display_box::NONE, 0, 0]));
        assert!(none.is_none());
    }

    #[test]
    fn display_encodes_for_adjustment_store() {
        let list_item = FfiDisplay::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, true);
        assert_eq!(
            list_item.encoded(),
            DISPLAY_TAG_OUTSIDE_AND_INSIDE as u32
                | (display_outside::BLOCK as u32) << 8
                | (display_inside::FLOW as u32) << 16
                | 1 << 24
        );

        assert_eq!(
            FfiDisplay::internal(display_internal::TABLE_ROW).encoded(),
            DISPLAY_TAG_INTERNAL as u32 | (display_internal::TABLE_ROW as u32) << 8
        );
        assert_eq!(
            FfiDisplay::none().encoded(),
            DISPLAY_TAG_BOX as u32 | (display_box::NONE as u32) << 8
        );
    }

    #[test]
    fn encoding_round_trips() {
        let values = [
            FfiDisplay::block(),
            FfiDisplay::inline(),
            FfiDisplay::inline_block(),
            FfiDisplay::flow_root(),
            FfiDisplay::table(),
            FfiDisplay::inline_table(),
            FfiDisplay::none(),
            FfiDisplay::contents(),
            FfiDisplay::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, true),
            FfiDisplay::internal(display_internal::TABLE_CELL),
        ];
        for value in values {
            assert!(FfiDisplay::from_raw(value.encoded()) == value);
        }
    }

    #[test]
    fn internal_table_predicates_match_each_internal_value() {
        let row = FfiDisplay::internal(display_internal::TABLE_ROW);
        assert!(row.is_table_row() && row.is_internal_table() && !row.is_table_cell());

        let cell = FfiDisplay::internal(display_internal::TABLE_CELL);
        assert!(cell.is_table_cell() && cell.is_internal_table());

        let column = FfiDisplay::internal(display_internal::TABLE_COLUMN);
        assert!(column.is_table_column() && column.is_internal_table());

        let column_group = FfiDisplay::internal(display_internal::TABLE_COLUMN_GROUP);
        assert!(column_group.is_table_column_group() && column_group.is_internal_table());

        let row_group = FfiDisplay::internal(display_internal::TABLE_ROW_GROUP);
        assert!(row_group.is_table_row_group() && row_group.is_internal_table());

        let header_group = FfiDisplay::internal(display_internal::TABLE_HEADER_GROUP);
        assert!(header_group.is_table_header_group() && header_group.is_internal_table());

        let footer_group = FfiDisplay::internal(display_internal::TABLE_FOOTER_GROUP);
        assert!(footer_group.is_table_footer_group() && footer_group.is_internal_table());

        // table-caption is internal but not an internal table element.
        let caption = FfiDisplay::internal(display_internal::TABLE_CAPTION);
        assert!(caption.is_table_caption() && !caption.is_internal_table());
    }

    #[test]
    fn outside_and_inside_predicates() {
        let table = FfiDisplay::table();
        assert!(table.is_table_inside() && table.is_block_outside() && !table.is_internal());

        let inline_table = FfiDisplay::inline_table();
        assert!(inline_table.is_table_inside() && inline_table.is_inline_outside());

        let list_item = FfiDisplay::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, true);
        assert!(list_item.is_list_item() && list_item.is_flow_inside());
        assert!(!FfiDisplay::block().is_list_item());

        assert!(FfiDisplay::block().is_flow_inside());
        assert!(FfiDisplay::flow_root().is_flow_root_inside());
        assert!(FfiDisplay::inline_block().is_inline_block());

        let ruby = FfiDisplay::outside_and_inside(display_outside::INLINE, display_inside::RUBY, false);
        assert!(ruby.is_ruby_inside());

        // Box-tag values answer no outside/inside predicate.
        assert!(!FfiDisplay::none().is_flow_inside());
        assert!(!FfiDisplay::none().is_block_outside());
    }

    #[test]
    fn creates_single_keyword_display_values() {
        assert_eq!(FfiDisplay::from_single_keyword(keyword::NONE), Some(FfiDisplay::none()));
        assert_eq!(
            FfiDisplay::from_single_keyword(keyword::FLOW),
            Some(FfiDisplay::block())
        );
        assert_eq!(
            FfiDisplay::from_single_keyword(keyword::_WEBKIT_FLEX),
            Some(FfiDisplay::outside_and_inside(
                display_outside::BLOCK,
                display_inside::FLEX,
                false
            ))
        );
        assert_eq!(
            FfiDisplay::from_single_keyword(keyword::TABLE_ROW),
            Some(FfiDisplay::internal(display_internal::TABLE_ROW))
        );
        assert_eq!(FfiDisplay::from_single_keyword(keyword::AUTO), None);
    }
}
