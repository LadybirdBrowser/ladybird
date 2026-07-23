/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS `display` value and its predicates, mirroring `Web::CSS::Display`.

use crate::css_enums::{display_box, display_inside, display_internal, display_outside};

/// Mirror of the CSS Display value; the C++ tagged union crosses as explicit
/// fields, with the unused fields zeroed so equality is field-wise. `tag` uses
/// the same discriminants as Display::Type.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
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
        Self::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, false)
    }

    pub fn inline() -> Self {
        Self::outside_and_inside(display_outside::INLINE, display_inside::FLOW, false)
    }

    pub fn inline_block() -> Self {
        Self::outside_and_inside(display_outside::INLINE, display_inside::FLOW_ROOT, false)
    }

    pub fn flow_root() -> Self {
        Self::outside_and_inside(display_outside::BLOCK, display_inside::FLOW_ROOT, false)
    }

    pub fn table() -> Self {
        Self::outside_and_inside(display_outside::BLOCK, display_inside::TABLE, false)
    }

    pub fn inline_table() -> Self {
        Self::outside_and_inside(display_outside::INLINE, display_inside::TABLE, false)
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
            box_value: display_box::NONE,
        }
    }

    pub fn contents() -> Self {
        Self {
            tag: DISPLAY_TAG_BOX,
            outside: 0,
            inside: 0,
            list_item: false,
            internal: 0,
            box_value: display_box::CONTENTS,
        }
    }

    pub fn is_outside_and_inside(&self) -> bool {
        self.tag == DISPLAY_TAG_OUTSIDE_AND_INSIDE
    }

    pub fn is_internal(&self) -> bool {
        self.tag == DISPLAY_TAG_INTERNAL
    }

    pub fn is_none(&self) -> bool {
        self.tag == DISPLAY_TAG_BOX && self.box_value == display_box::NONE
    }

    pub fn is_contents(&self) -> bool {
        self.tag == DISPLAY_TAG_BOX && self.box_value == display_box::CONTENTS
    }

    pub fn is_block_outside(&self) -> bool {
        self.is_outside_and_inside() && self.outside == display_outside::BLOCK
    }

    pub fn is_inline_outside(&self) -> bool {
        self.is_outside_and_inside() && self.outside == display_outside::INLINE
    }

    pub fn is_inline_block(&self) -> bool {
        self.is_inline_outside() && self.is_flow_root_inside()
    }

    pub fn is_list_item(&self) -> bool {
        self.is_outside_and_inside() && self.list_item
    }

    pub fn is_flow_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::FLOW
    }

    pub fn is_flow_root_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::FLOW_ROOT
    }

    pub fn is_table_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::TABLE
    }

    pub fn is_flex_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::FLEX
    }

    pub fn is_grid_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::GRID
    }

    pub fn is_ruby_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::RUBY
    }

    pub fn is_math_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::MATH
    }

    pub fn is_table_row(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_ROW
    }

    pub fn is_table_cell(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_CELL
    }

    pub fn is_table_column(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_COLUMN
    }

    pub fn is_table_column_group(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_COLUMN_GROUP
    }

    pub fn is_table_row_group(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_ROW_GROUP
    }

    pub fn is_table_header_group(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_HEADER_GROUP
    }

    pub fn is_table_footer_group(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_FOOTER_GROUP
    }

    pub fn is_table_caption(&self) -> bool {
        self.is_internal() && self.internal == display_internal::TABLE_CAPTION
    }

    // https://drafts.csswg.org/css-display-3/#internal-table-element
    pub fn is_internal_table(&self) -> bool {
        self.is_internal()
            && (self.internal == display_internal::TABLE_ROW_GROUP
                || self.internal == display_internal::TABLE_HEADER_GROUP
                || self.internal == display_internal::TABLE_FOOTER_GROUP
                || self.internal == display_internal::TABLE_ROW
                || self.internal == display_internal::TABLE_CELL
                || self.internal == display_internal::TABLE_COLUMN_GROUP
                || self.internal == display_internal::TABLE_COLUMN)
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
}
