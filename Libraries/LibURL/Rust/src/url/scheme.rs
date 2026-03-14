/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://url.spec.whatwg.org/#special-scheme
pub(crate) fn special_schemes() -> &'static [&'static str] {
    &["ftp", "file", "http", "https", "ws", "wss"]
}

// https://url.spec.whatwg.org/#is-special
pub(crate) fn is_special_scheme(scheme: &[u8]) -> bool {
    special_schemes()
        .iter()
        .any(|special_scheme| scheme == special_scheme.as_bytes())
}

// https://url.spec.whatwg.org/#default-port
pub(crate) fn default_port_for_scheme(scheme: &str) -> Option<u16> {
    match scheme {
        "ftp" => Some(21),
        "http" => Some(80),
        "https" => Some(443),
        "ws" => Some(80),
        "wss" => Some(443),
        _ => None,
    }
}
