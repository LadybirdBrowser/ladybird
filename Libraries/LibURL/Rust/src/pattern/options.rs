/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://urlpattern.spec.whatwg.org/#options
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Options {
    // https://urlpattern.spec.whatwg.org/#options-delimiter-code-point
    pub delimiter_code_point: Option<char>,

    // https://urlpattern.spec.whatwg.org/#options-prefix-code-point
    pub prefix_code_point: Option<char>,

    // https://urlpattern.spec.whatwg.org/#options-ignore-case
    pub ignore_case: bool,
}

impl Options {
    // https://urlpattern.spec.whatwg.org/#default-options
    pub fn default_() -> Self {
        // The default options is an options struct with delimiter code point set to the empty string and prefix code point set to the empty string.
        Self {
            delimiter_code_point: None,
            prefix_code_point: None,
            ignore_case: false,
        }
    }

    // https://urlpattern.spec.whatwg.org/#hostname-options
    pub fn hostname() -> Self {
        // The hostname options is an options struct with delimiter code point set "." and prefix code point set to the empty string.
        Self {
            delimiter_code_point: Some('.'),
            prefix_code_point: None,
            ignore_case: false,
        }
    }

    // https://urlpattern.spec.whatwg.org/#pathname-options
    pub fn pathname() -> Self {
        // The pathname options is an options struct with delimiter code point set "/" and prefix code point set to "/".
        Self {
            delimiter_code_point: Some('/'),
            prefix_code_point: Some('/'),
            ignore_case: false,
        }
    }
}
