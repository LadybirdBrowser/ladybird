/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

mod host;
mod parser;
mod percent_encoding;
mod scheme;
#[allow(dead_code)]
mod serialize;
mod types;

pub(crate) use self::host::parse_host;
pub(crate) use self::scheme::default_port_for_scheme;
pub(crate) use self::scheme::is_special_scheme;
pub(crate) use self::scheme::special_schemes;
pub(crate) use self::types::ExcludeFragment;
pub use self::types::Host;
pub use self::types::State;
pub use self::types::Url;

#[derive(Debug, Default)]
pub struct BasicParseOptions<'a> {
    pub base_url: Option<&'a Url>,
    state_override: Option<State>,
    encoding: Option<&'a str>,
}

impl<'a> BasicParseOptions<'a> {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn base_url(mut self, base_url: &'a Url) -> Self {
        self.base_url = Some(base_url);
        self
    }

    pub fn state_override(mut self, state: impl Into<Option<State>>) -> Self {
        self.state_override = state.into();
        self
    }

    pub fn encoding(mut self, encoding: impl Into<Option<&'a str>>) -> Self {
        self.encoding = encoding.into();
        self
    }
}

pub fn basic_parse(input: &str, options: BasicParseOptions<'_>) -> Option<Url> {
    let mut url = Url::default();
    if parser::basic_parse_into(input, &mut url, &options, false) {
        Some(url)
    } else {
        None
    }
}

pub fn basic_parse_into(input: &str, url: &mut Url, options: &BasicParseOptions<'_>) -> bool {
    parser::basic_parse_into(input, url, options, true)
}
