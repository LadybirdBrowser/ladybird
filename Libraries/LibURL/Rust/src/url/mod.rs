/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

mod host;
mod input;
mod parser;
mod percent_encoding;
mod scheme;
mod serialize;
mod types;

pub(crate) use self::host::parse_host_into;
pub use self::input::UrlInput;
pub use self::scheme::SchemeType;
pub(crate) use self::scheme::default_port_for_scheme;
pub(crate) use self::scheme::is_special_scheme;
pub(crate) use self::scheme::special_schemes;
pub use self::types::ExcludeFragment;
pub use self::types::HostKind;
pub use self::types::State;
pub use self::types::URL_OFFSET_NONE;
pub use self::types::Url;
pub use self::types::UrlComponents;
pub use self::types::UrlRef;

#[derive(Debug, Default)]
pub struct BasicParseOptions<'a> {
    base_url: Option<UrlRef<'a>>,
    encoding: Option<&'a str>,
}

impl<'a> BasicParseOptions<'a> {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn base_url(mut self, base_url: impl Into<UrlRef<'a>>) -> Self {
        self.base_url = Some(base_url.into());
        self
    }

    pub fn encoding(mut self, encoding: impl Into<Option<&'a str>>) -> Self {
        self.encoding = encoding.into();
        self
    }
}

// https://url.spec.whatwg.org/#concept-basic-url-parser
pub fn basic_parse<'a>(input: impl Into<UrlInput<'a>>, options: BasicParseOptions<'_>) -> Option<Url> {
    parser::basic_parse(input.into(), options.base_url, options.encoding)
}

// https://url.spec.whatwg.org/#concept-basic-url-parser
pub fn basic_parse_with_state_override<'a>(
    input: impl Into<UrlInput<'a>>,
    url: &mut Url,
    state_override: State,
    encoding: Option<&str>,
) -> bool {
    parser::basic_parse_with_state_override(input.into(), url, state_override, encoding)
}
