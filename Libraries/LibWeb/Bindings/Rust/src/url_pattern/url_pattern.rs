/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! URLPattern, https://urlpattern.spec.whatwg.org/#urlpattern

use ak::Utf16String;
use libjs_rust::api::Trace;
use liburl_rust::pattern::{
    ComponentResult, ErrorInfo, GroupMatch, IgnoreCase, Init, Input, MatchInput, Pattern, Result as MatchResult,
};

use crate::bindings::conversions::UsvString;
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The union URLPatternInput of URLPattern/URLPattern.idl, whose members the bindings convert.
pub(crate) enum URLPatternInput {
    USVString(UsvString),
    URLPatternInit(URLPatternInit),
}

/// The dictionary URLPatternInit of URLPattern/URLPattern.idl, whose members the bindings convert.
pub(crate) struct URLPatternInit {
    pub(crate) base_url: Option<UsvString>,
    pub(crate) hash: Option<UsvString>,
    pub(crate) hostname: Option<UsvString>,
    pub(crate) password: Option<UsvString>,
    pub(crate) pathname: Option<UsvString>,
    pub(crate) port: Option<UsvString>,
    pub(crate) protocol: Option<UsvString>,
    pub(crate) search: Option<UsvString>,
    pub(crate) username: Option<UsvString>,
}

/// The dictionary URLPatternOptions of URLPattern/URLPattern.idl, whose members the bindings convert.
pub(crate) struct URLPatternOptions {
    pub(crate) ignore_case: bool,
}

/// The dictionary URLPatternResult of URLPattern/URLPattern.idl, whose members the bindings convert.
pub(crate) struct URLPatternResult {
    pub(crate) hash: Option<URLPatternComponentResult>,
    pub(crate) hostname: Option<URLPatternComponentResult>,
    pub(crate) inputs: Option<Vec<URLPatternInput>>,
    pub(crate) password: Option<URLPatternComponentResult>,
    pub(crate) pathname: Option<URLPatternComponentResult>,
    pub(crate) port: Option<URLPatternComponentResult>,
    pub(crate) protocol: Option<URLPatternComponentResult>,
    pub(crate) search: Option<URLPatternComponentResult>,
    pub(crate) username: Option<URLPatternComponentResult>,
}

/// The dictionary URLPatternComponentResult of URLPattern/URLPattern.idl, whose members the bindings convert.
pub(crate) struct URLPatternComponentResult {
    pub(crate) groups: Option<Vec<(UsvString, Option<UsvString>)>>,
    pub(crate) input: Option<UsvString>,
}

fn optional_utf8(string: Option<UsvString>) -> Option<String> {
    string.map(|string| string.to_utf8())
}

fn optional_usv_string(string: Option<String>) -> Option<UsvString> {
    string.as_deref().map(UsvString::from_utf8)
}

impl From<URLPatternInit> for Init {
    fn from(init: URLPatternInit) -> Self {
        Self {
            protocol: optional_utf8(init.protocol),
            username: optional_utf8(init.username),
            password: optional_utf8(init.password),
            hostname: optional_utf8(init.hostname),
            port: optional_utf8(init.port),
            pathname: optional_utf8(init.pathname),
            search: optional_utf8(init.search),
            hash: optional_utf8(init.hash),
            base_url: optional_utf8(init.base_url),
        }
    }
}

impl From<Input> for URLPatternInput {
    fn from(input: Input) -> Self {
        match input {
            Input::String(string) => Self::USVString(UsvString::from_utf8(&string)),
            Input::Init(init) => Self::URLPatternInit(URLPatternInit {
                base_url: optional_usv_string(init.base_url),
                hash: optional_usv_string(init.hash),
                hostname: optional_usv_string(init.hostname),
                password: optional_usv_string(init.password),
                pathname: optional_usv_string(init.pathname),
                port: optional_usv_string(init.port),
                protocol: optional_usv_string(init.protocol),
                search: optional_usv_string(init.search),
                username: optional_usv_string(init.username),
            }),
        }
    }
}

impl From<ComponentResult> for URLPatternComponentResult {
    fn from(result: ComponentResult) -> Self {
        let groups = result.groups.into_iter().map(|(name, group)| {
            let group = match group {
                GroupMatch::String(string) => Some(UsvString::from_utf8(&string)),
                GroupMatch::Empty => None,
            };
            (UsvString::from_utf8(&name), group)
        });
        Self {
            groups: Some(groups.collect()),
            input: Some(UsvString::from_utf8(&result.input)),
        }
    }
}

impl From<MatchResult> for URLPatternResult {
    fn from(result: MatchResult) -> Self {
        Self {
            hash: Some(result.hash.into()),
            hostname: Some(result.hostname.into()),
            inputs: Some(result.inputs.into_iter().map(Into::into).collect()),
            password: Some(result.password.into()),
            pathname: Some(result.pathname.into()),
            port: Some(result.port.into()),
            protocol: Some(result.protocol.into()),
            search: Some(result.search.into()),
            username: Some(result.username.into()),
        }
    }
}

/// Every error of a URL pattern is a TypeError.
fn type_error(error: ErrorInfo) -> Exception {
    Exception::TypeError(Utf16String::from_utf8(&error.message))
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct URLPattern {
    /// The associated URL pattern, https://urlpattern.spec.whatwg.org/#urlpattern-associated-url-pattern
    #[gc(untraced)]
    pattern: Pattern,
}

impl Wrappable for URLPattern {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl URLPattern {
    // https://urlpattern.spec.whatwg.org/#urlpattern-initialize
    fn initialize(
        input: URLPatternInput,
        base_url: Option<UsvString>,
        options: &URLPatternOptions,
    ) -> WebIdlResult<Self> {
        // 1. Set this's associated URL pattern to the result of create given input, baseURL, and options.
        let input = match input {
            URLPatternInput::USVString(string) => Input::String(string.to_utf8()),
            URLPatternInput::URLPatternInit(init) => Input::Init(init.into()),
        };
        let ignore_case = if options.ignore_case {
            IgnoreCase::Yes
        } else {
            IgnoreCase::No
        };
        let pattern = Pattern::create(&input, &optional_utf8(base_url), ignore_case).map_err(type_error)?;
        Ok(Self { pattern })
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-urlpattern
    pub(crate) fn construct_with_base_url(
        _: &mut CallContext,
        input: URLPatternInput,
        base_url: UsvString,
        options: URLPatternOptions,
    ) -> WebIdlResult<Self> {
        // 1. Run initialize given this, input, baseURL, and options.
        Self::initialize(input, Some(base_url), &options)
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-urlpattern-input-options
    pub(crate) fn construct(
        _: &mut CallContext,
        input: URLPatternInput,
        options: URLPatternOptions,
    ) -> WebIdlResult<Self> {
        // 1. Run initialize given this, input, null, and options.
        Self::initialize(input, None, &options)
    }

    // https://urlpattern.spec.whatwg.org/#url-pattern-match
    fn match_(&self, input: URLPatternInput, base_url: Option<UsvString>) -> WebIdlResult<Option<MatchResult>> {
        let input = match input {
            URLPatternInput::USVString(string) => MatchInput::String(string.to_utf8()),
            URLPatternInput::URLPatternInit(init) => MatchInput::Init(init.into()),
        };
        self.pattern
            .r#match(&input, &optional_utf8(base_url))
            .map_err(type_error)
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-test
    pub(crate) fn test(
        &self,
        _: &mut CallContext,
        input: URLPatternInput,
        base_url: Option<UsvString>,
    ) -> WebIdlResult<bool> {
        // 1. Let result be the result of match given this's associated URL pattern, input, and baseURL if given.
        let result = self.match_(input, base_url)?;

        // 2. If result is null, return false.
        // 3. Return true.
        Ok(result.is_some())
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-exec
    pub(crate) fn exec(
        &self,
        _: &mut CallContext,
        input: URLPatternInput,
        base_url: Option<UsvString>,
    ) -> WebIdlResult<Option<URLPatternResult>> {
        // 1. Return the result of match given this's associated URL pattern, input, and baseURL if given.
        Ok(self.match_(input, base_url)?.map(Into::into))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-protocol
    pub(crate) fn protocol(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's protocol component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.protocol_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-username
    pub(crate) fn username(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's username component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.username_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-password
    pub(crate) fn password(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's password component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.password_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-hostname
    pub(crate) fn hostname(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's hostname component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.hostname_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-port
    pub(crate) fn port(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's port component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.port_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-pathname
    pub(crate) fn pathname(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's pathname component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.pathname_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-search
    pub(crate) fn search(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's search component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.search_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-hash
    pub(crate) fn hash(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Return this's associated URL pattern's hash component's pattern string.
        Ok(UsvString::from_utf8(&self.pattern.hash_component().pattern_string))
    }

    // https://urlpattern.spec.whatwg.org/#dom-urlpattern-hasregexpgroups
    pub(crate) fn has_reg_exp_groups(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // 1. If this's associated URL pattern's has regexp groups, then return true.
        // 2. Return false.
        Ok(self.pattern.has_regexp_groups())
    }
}
