/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The locales of LibUnicode (Libraries/LibUnicode/Locale.h), through its C exports.

use ak::Utf16String;

use super::{UnicodeTextMappingOutput, collect_text};

unsafe extern "C" {
    fn unicode_default_locale(output: UnicodeTextMappingOutput);
}

/// Unicode::default_locale.
pub fn default_locale() -> Utf16String {
    // SAFETY: The output writes into a Vec that outlives the call.
    let ((), locale) = collect_text(|output| unsafe { unicode_default_locale(output) });
    Utf16String::from_utf16(&locale)
}
