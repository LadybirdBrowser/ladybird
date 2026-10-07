/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The display names of LibUnicode (Libraries/LibUnicode/DisplayNames.h), through its C exports.

use ak::Utf16String;

use super::time_zone::InDST;
use super::{UnicodeTextMappingOutput, collect_text};

unsafe extern "C" {
    fn unicode_time_zone_display_name(
        locale: *const u8,
        locale_length: usize,
        time_zone: *const u8,
        time_zone_length: usize,
        in_dst: bool,
        time: f64,
        output: UnicodeTextMappingOutput,
    ) -> bool;
}

/// Unicode::time_zone_display_name: the long name of `time_zone_identifier` in `locale` at `time`, the standard or
/// the daylight saving one as `in_dst` says.
pub fn time_zone_display_name(
    locale: &str,
    time_zone_identifier: &str,
    in_dst: InDST,
    time: f64,
) -> Option<Utf16String> {
    let (found, name) = collect_text(|output| {
        // SAFETY: The locale and time zone buffers are valid for their lengths, and the output writes into a Vec.
        unsafe {
            unicode_time_zone_display_name(
                locale.as_ptr(),
                locale.len(),
                time_zone_identifier.as_ptr(),
                time_zone_identifier.len(),
                in_dst == InDST::Yes,
                time,
                output,
            )
        }
    });
    found.then(|| Utf16String::from_utf16(&name))
}
