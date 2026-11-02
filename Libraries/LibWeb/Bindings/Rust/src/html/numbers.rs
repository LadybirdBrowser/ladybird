/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

unsafe extern "C" {
    fn web_html_parse_floating_point_number(string: ak::Span<'_, u8>, value: *mut f64) -> bool;
}

/// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-floating-point-number-values
pub(crate) fn parse_floating_point_number(string: &str) -> Option<f64> {
    let mut value = 0.0;
    // SAFETY: The string and the value outlive the call.
    unsafe { web_html_parse_floating_point_number(ak::Span::new(string.as_bytes()), &raw mut value) }.then_some(value)
}
