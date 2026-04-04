/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::net::Ipv4Addr;
use std::net::Ipv6Addr;

use super::percent_encoding::PercentEncodeSet;
use super::percent_encoding::percent_encode;
use super::scheme::is_special_scheme;

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum State {
    SchemeStart,
    Scheme,
    NoScheme,
    SpecialRelativeOrAuthority,
    PathOrAuthority,
    Relative,
    RelativeSlash,
    SpecialAuthoritySlashes,
    SpecialAuthorityIgnoreSlashes,
    Authority,
    Host,
    Hostname,
    Port,
    File,
    FileSlash,
    FileHost,
    PathStart,
    Path,
    OpaquePath,
    Query,
    Fragment,
}

#[allow(dead_code)]
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ExcludeFragment {
    No,
    Yes,
}

impl Url {
    pub(crate) fn set_scheme(&mut self, scheme: String) {
        self.scheme = scheme;
    }

    // https://url.spec.whatwg.org/#set-the-username
    pub(crate) fn set_username(&mut self, username: &str) {
        // To set the username given a url and username, set url’s username to the result of running UTF-8 percent-encode
        // on username using the userinfo percent-encode set.
        self.username = percent_encode(username, PercentEncodeSet::Userinfo, false);
    }

    // https://url.spec.whatwg.org/#set-the-password
    pub(crate) fn set_password(&mut self, password: &str) {
        // To set the password given a url and password, set url’s password to the result of running UTF-8 percent-encode
        // on password using the userinfo percent-encode set.
        self.password = percent_encode(password, PercentEncodeSet::Userinfo, false);
    }

    pub(crate) fn serialized_host(&self) -> String {
        self.host.as_ref().expect("host should be present").serialize()
    }

    pub(crate) fn set_paths(&mut self, paths: &[&str]) {
        self.path.clear();
        self.path.reserve(paths.len());
        for segment in paths {
            self.path.push(percent_encode(segment, PercentEncodeSet::Path, false));
        }
    }

    pub(crate) fn set_query(&mut self, query: Option<String>) {
        self.query = query;
    }

    pub(crate) fn set_fragment(&mut self, fragment: Option<String>) {
        self.fragment = fragment;
    }

    pub(crate) fn set_has_an_opaque_path(&mut self, value: bool) {
        self.has_opaque_path = value;
    }

    // https://url.spec.whatwg.org/#is-special
    pub(crate) fn is_special(&self) -> bool {
        is_special_scheme(self.scheme.as_bytes())
    }
}

// https://url.spec.whatwg.org/#concept-host
// A host is a domain, an IP address, an opaque host, or an empty host. Typically a host serves as a network address,
// but it is sometimes used as opaque identifier in URLs where a network address is not necessary.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Host {
    Domain(String),
    Ipv4(Ipv4Addr),
    Ipv6(Ipv6Addr),
    Opaque(String),
}

impl Host {
    pub(crate) fn is_empty_host(&self) -> bool {
        matches!(self, Self::Domain(host) if host.is_empty())
    }
}

// https://url.spec.whatwg.org/#url-representation
// A URL is a struct that represents a universal identifier.
// To disambiguate from a valid URL string it can also be referred to as a URL record.
#[derive(Clone, Debug, Default)]
pub struct Url {
    // A URL’s scheme is an ASCII string that identifies the type of URL and can be used to dispatch a URL for further
    // processing after parsing. It is initially the empty string.
    pub(crate) scheme: String,

    // A URL’s username is an ASCII string identifying a username. It is initially the empty string.
    pub(crate) username: String,

    // A URL’s password is an ASCII string identifying a password. It is initially the empty string.
    pub(crate) password: String,

    // A URL’s host is null or a host. It is initially null.
    pub(crate) host: Option<Host>,

    // A URL’s port is either null or a 16-bit unsigned integer that identifies a networking port. It is initially null.
    pub(crate) port: Option<u16>,

    // A URL’s path is either a URL path segment or a list of zero or more URL path segments, usually identifying a location. It is initially « ».
    // A URL path segment is an ASCII string. It commonly refers to a directory or a file, but has no predefined meaning.
    pub(crate) path: Vec<String>,
    pub(crate) has_opaque_path: bool,

    // A URL’s query is either null or an ASCII string. It is initially null.
    pub(crate) query: Option<String>,

    // A URL’s fragment is either null or an ASCII string that can be used for further processing on the resource the
    // URL’s other components identify. It is initially null.
    pub(crate) fragment: Option<String>,
}
