/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiReplacedElementDisplayAdjustment {
    None,
    Inline,
    Block,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_adjusted_table_display_for_replaced_element(
    is_table_inside: bool,
    is_block_outside: bool,
    is_internal_table: bool,
    is_table_caption: bool,
) -> FfiReplacedElementDisplayAdjustment {
    abort_on_panic(|| {
        // https://drafts.csswg.org/css-display-3/#outer-role
        // Note: Outer display types do affect replaced elements.
        if is_table_inside {
            if is_block_outside {
                return FfiReplacedElementDisplayAdjustment::Block;
            }
            return FfiReplacedElementDisplayAdjustment::Inline;
        }

        // https://drafts.csswg.org/css-display-3/#layout-specific-display
        // When the 'display' property of a replaced element computes to one of the layout-internal values, it is
        // handled as having a used value of 'display: inline'.
        if is_internal_table || is_table_caption {
            return FfiReplacedElementDisplayAdjustment::Inline;
        }
        FfiReplacedElementDisplayAdjustment::None
    })
}

#[cfg(test)]
mod tests {
    use super::{FfiReplacedElementDisplayAdjustment, rust_adjusted_table_display_for_replaced_element};

    #[test]
    fn replaced_table_display_adjustments() {
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(true, true, false, false),
            FfiReplacedElementDisplayAdjustment::Block
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(true, false, false, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, true, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, false, true),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, false, false),
            FfiReplacedElementDisplayAdjustment::None
        );
    }
}
