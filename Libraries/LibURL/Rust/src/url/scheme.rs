/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Hash)]
pub enum SchemeType {
    #[default]
    NotSpecial,
    Ftp,
    File,
    Http,
    Https,
    Ws,
    Wss,
}

impl SchemeType {
    pub(crate) fn from_scheme(scheme: &str) -> Self {
        match scheme {
            "ftp" => Self::Ftp,
            "file" => Self::File,
            "http" => Self::Http,
            "https" => Self::Https,
            "ws" => Self::Ws,
            "wss" => Self::Wss,
            _ => Self::NotSpecial,
        }
    }

    // https://url.spec.whatwg.org/#is-special
    pub(crate) fn is_special(self) -> bool {
        self != Self::NotSpecial
    }

    // https://url.spec.whatwg.org/#default-port
    pub(crate) fn default_port(self) -> Option<u16> {
        match self {
            Self::Ftp => Some(21),
            Self::Http | Self::Ws => Some(80),
            Self::Https | Self::Wss => Some(443),
            Self::File | Self::NotSpecial => None,
        }
    }
}

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
    SchemeType::from_scheme(scheme).default_port()
}
