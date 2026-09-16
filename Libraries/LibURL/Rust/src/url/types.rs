/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ops::Range;

use super::UrlInput;
use super::percent_encoding::PercentEncodeSet;
use super::percent_encoding::percent_encode_input_into;
use super::scheme::SchemeType;

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

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExcludeFragment {
    No,
    Yes,
}

// https://url.spec.whatwg.org/#concept-host
// A host is a domain, an IP address, an opaque host, or an empty host. Typically a host serves as a network address,
// but it is sometimes used as opaque identifier in URLs where a network address is not necessary.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Hash)]
pub enum HostKind {
    #[default]
    Null,
    Domain,
    Ipv4,
    Ipv6,
    Opaque,
}

pub const URL_OFFSET_NONE: u32 = u32::MAX;

// Offsets into a URL's serialization, which is laid out as:
// scheme ":" ["//" [username [":" password] "@"] host [":" port]] ["/."] path ["?" query] ["#" fragment]
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub struct UrlComponents {
    pub scheme_end: u32,
    pub username_end: u32,
    pub host_start: u32,
    pub host_end: u32,
    pub path_start: u32,
    pub query_start: u32,
    pub fragment_start: u32,
    pub port: u16,
    pub has_port: bool,
    pub host_kind: HostKind,
    pub scheme_type: SchemeType,
    pub has_opaque_path: bool,
}

impl Default for UrlComponents {
    fn default() -> Self {
        Self {
            scheme_end: 0,
            username_end: 0,
            host_start: 0,
            host_end: 0,
            path_start: 0,
            query_start: URL_OFFSET_NONE,
            fragment_start: URL_OFFSET_NONE,
            port: 0,
            has_port: false,
            host_kind: HostKind::Null,
            scheme_type: SchemeType::NotSpecial,
            has_opaque_path: false,
        }
    }
}

impl UrlComponents {
    fn query_start(&self) -> Option<u32> {
        (self.query_start != URL_OFFSET_NONE).then_some(self.query_start)
    }

    fn set_query_start(&mut self, query_start: Option<u32>) {
        self.query_start = query_start.unwrap_or(URL_OFFSET_NONE);
    }

    fn fragment_start(&self) -> Option<u32> {
        (self.fragment_start != URL_OFFSET_NONE).then_some(self.fragment_start)
    }

    fn set_fragment_start(&mut self, fragment_start: Option<u32>) {
        self.fragment_start = fragment_start.unwrap_or(URL_OFFSET_NONE);
    }
}

#[derive(Clone, Copy)]
enum Offset {
    SchemeEnd,
    UsernameEnd,
    HostStart,
    HostEnd,
    PathStart,
    QueryStart,
    FragmentStart,
}

// https://url.spec.whatwg.org/#url-representation
// A URL is a struct that represents a universal identifier.
// To disambiguate from a valid URL string it can also be referred to as a URL record.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Hash)]
pub struct Url<Serialization = String> {
    pub(crate) serialization: Serialization,
    pub(crate) components: UrlComponents,
}

pub type UrlRef<'a> = Url<&'a str>;

impl<Serialization: AsRef<str>> Url<Serialization> {
    fn slice(&self, start: u32, end: u32) -> &str {
        &self.as_str()[start as usize..end as usize]
    }

    pub(crate) fn end_offset(&self) -> u32 {
        self.as_str().len() as u32
    }

    pub fn as_str(&self) -> &str {
        self.serialization.as_ref()
    }

    pub fn as_url_ref(&self) -> UrlRef<'_> {
        Url {
            serialization: self.as_str(),
            components: self.components,
        }
    }

    pub fn components(&self) -> UrlComponents {
        self.components
    }

    // https://url.spec.whatwg.org/#concept-url-serializer
    pub fn serialize(&self, exclude_fragment: ExcludeFragment) -> &str {
        match (exclude_fragment, self.components.fragment_start()) {
            (ExcludeFragment::Yes, Some(fragment_start)) => self.slice(0, fragment_start),
            _ => self.as_str(),
        }
    }

    pub fn scheme(&self) -> &str {
        self.slice(0, self.components.scheme_end)
    }

    pub fn scheme_type(&self) -> SchemeType {
        self.components.scheme_type
    }

    // https://url.spec.whatwg.org/#is-special
    pub fn is_special(&self) -> bool {
        self.components.scheme_type.is_special()
    }

    pub fn username(&self) -> &str {
        if self.components.host_kind == HostKind::Null {
            return "";
        }
        self.slice(self.components.scheme_end + 3, self.components.username_end)
    }

    pub fn password(&self) -> &str {
        let components = self.components;
        if components.host_kind == HostKind::Null || components.username_end + 1 >= components.host_start {
            return "";
        }
        self.slice(components.username_end + 1, components.host_start - 1)
    }

    // https://url.spec.whatwg.org/#include-credentials
    pub fn includes_credentials(&self) -> bool {
        self.components.host_kind != HostKind::Null && self.components.host_start > self.components.scheme_end + 3
    }

    pub fn host_kind(&self) -> HostKind {
        self.components.host_kind
    }

    // https://url.spec.whatwg.org/#concept-host-serializer
    pub fn serialized_host(&self) -> Option<&str> {
        (self.components.host_kind != HostKind::Null)
            .then(|| self.slice(self.components.host_start, self.components.host_end))
    }

    // https://url.spec.whatwg.org/#empty-host
    pub fn has_empty_host(&self) -> bool {
        self.components.host_kind != HostKind::Null && self.components.host_start == self.components.host_end
    }

    pub fn port(&self) -> Option<u16> {
        self.components.has_port.then_some(self.components.port)
    }

    // https://url.spec.whatwg.org/#url-opaque-path
    pub fn has_opaque_path(&self) -> bool {
        self.components.has_opaque_path
    }

    // https://url.spec.whatwg.org/#url-path-serializer
    pub fn serialize_path(&self) -> &str {
        self.slice(self.components.path_start, self.path_end())
    }

    pub fn path_segments(&self) -> impl Iterator<Item = &str> + Clone {
        let path = self.serialize_path();
        let (opaque_path, segments) = if self.components.has_opaque_path {
            (Some(path), None)
        } else {
            (None, path.strip_prefix('/').map(|segments| segments.split('/')))
        };
        opaque_path.into_iter().chain(segments.into_iter().flatten())
    }

    pub fn path_segment_count(&self) -> usize {
        if self.components.has_opaque_path {
            return 1;
        }
        self.serialize_path().bytes().filter(|byte| *byte == b'/').count()
    }

    pub fn query(&self) -> Option<&str> {
        let query_start = self.components.query_start()?;
        let query_end = self.components.fragment_start().unwrap_or(self.end_offset());
        Some(self.slice(query_start + 1, query_end))
    }

    pub fn fragment(&self) -> Option<&str> {
        let fragment_start = self.components.fragment_start()?;
        Some(self.slice(fragment_start + 1, self.end_offset()))
    }

    fn path_end(&self) -> u32 {
        self.components
            .query_start()
            .or(self.components.fragment_start())
            .unwrap_or(self.end_offset())
    }
}

impl<'a> From<&'a Url> for UrlRef<'a> {
    fn from(url: &'a Url) -> Self {
        url.as_url_ref()
    }
}

impl From<UrlRef<'_>> for Url {
    fn from(url: UrlRef<'_>) -> Self {
        Url {
            serialization: url.serialization.to_owned(),
            components: url.components,
        }
    }
}

impl From<Url> for String {
    fn from(url: Url) -> Self {
        url.serialization
    }
}

impl Url {
    fn shift_offsets(&mut self, from: Offset, delta: isize) {
        let components = &mut self.components;
        let offsets = [
            &mut components.scheme_end,
            &mut components.username_end,
            &mut components.host_start,
            &mut components.host_end,
            &mut components.path_start,
            &mut components.query_start,
            &mut components.fragment_start,
        ];
        for offset in offsets.into_iter().skip(from as usize) {
            if *offset != URL_OFFSET_NONE {
                *offset = (*offset as isize + delta) as u32;
            }
        }
    }

    fn replace_range(&mut self, range: Range<u32>, replacement: &str, shift_from: Offset) {
        let delta = replacement.len() as isize - (range.end - range.start) as isize;
        self.serialization
            .replace_range(range.start as usize..range.end as usize, replacement);
        self.shift_offsets(shift_from, delta);
    }

    pub fn set_scheme(&mut self, scheme: &str) {
        self.replace_range(0..self.components.scheme_end, scheme, Offset::SchemeEnd);
        self.components.scheme_type = SchemeType::from_scheme(scheme);
    }

    fn replace_userinfo(&mut self, username: &str, password: &str) {
        // NB: A URL without a host has no userinfo in its serialization, so it can only have empty credentials.
        if self.components.host_kind == HostKind::Null {
            assert!(username.is_empty() && password.is_empty());
            return;
        }

        let userinfo_start = self.components.scheme_end + 3;
        let mut userinfo = String::with_capacity(username.len() + password.len() + 2);
        userinfo.push_str(username);
        if !password.is_empty() {
            userinfo.push(':');
            userinfo.push_str(password);
        }
        if !userinfo.is_empty() {
            userinfo.push('@');
        }

        self.replace_range(userinfo_start..self.components.host_start, &userinfo, Offset::HostStart);
        self.components.username_end = userinfo_start + username.len() as u32;
    }

    // https://url.spec.whatwg.org/#set-the-username
    pub fn set_username<'a>(&mut self, username: impl Into<UrlInput<'a>>) {
        // To set the username given a url and username, set url’s username to the result of running UTF-8 percent-encode
        // on username using the userinfo percent-encode set.
        let mut encoded_username = String::new();
        percent_encode_input_into(
            username.into(),
            PercentEncodeSet::Userinfo,
            false,
            &mut encoded_username,
        );
        let password = self.password().to_owned();
        self.replace_userinfo(&encoded_username, &password);
    }

    // https://url.spec.whatwg.org/#set-the-password
    pub fn set_password<'a>(&mut self, password: impl Into<UrlInput<'a>>) {
        // To set the password given a url and password, set url’s password to the result of running UTF-8 percent-encode
        // on password using the userinfo percent-encode set.
        let mut encoded_password = String::new();
        percent_encode_input_into(
            password.into(),
            PercentEncodeSet::Userinfo,
            false,
            &mut encoded_password,
        );
        let username = self.username().to_owned();
        self.replace_userinfo(&username, &encoded_password);
    }

    // Sets url's host to an already serialized host of the given kind.
    pub fn set_host(&mut self, kind: HostKind, host: &str) {
        assert!(kind != HostKind::Null);

        if self.components.host_kind == HostKind::Null {
            let authority_start = self.components.scheme_end + 1;
            let mut authority = String::with_capacity(host.len() + 2);
            authority.push_str("//");
            authority.push_str(host);
            self.replace_range(authority_start..authority_start, &authority, Offset::UsernameEnd);
            self.components.username_end = authority_start + 2;
            self.components.host_start = authority_start + 2;
            self.components.host_end = authority_start + 2 + host.len() as u32;
        } else {
            self.replace_range(
                self.components.host_start..self.components.host_end,
                host,
                Offset::HostEnd,
            );
        }

        self.components.host_kind = kind;
        self.update_empty_first_segment_marker();
    }

    pub(crate) fn set_host_to_null(&mut self) {
        let authority_start = self.components.scheme_end + 1;
        self.replace_range(authority_start..self.components.path_start, "", Offset::PathStart);
        self.components.username_end = authority_start;
        self.components.host_start = authority_start;
        self.components.host_end = authority_start;
        self.components.host_kind = HostKind::Null;
        self.components.port = 0;
        self.components.has_port = false;
        self.update_empty_first_segment_marker();
    }

    pub fn set_port(&mut self, port: Option<u16>) {
        // NB: A URL without a host has no port in its serialization, so it can only have a null port.
        if self.components.host_kind == HostKind::Null {
            assert!(port.is_none());
            return;
        }

        let serialized_port = port.map(|port| format!(":{port}")).unwrap_or_default();
        self.replace_range(
            self.components.host_end..self.components.path_start,
            &serialized_port,
            Offset::PathStart,
        );
        self.components.port = port.unwrap_or(0);
        self.components.has_port = port.is_some();
    }

    // Sets url's path to a list of already percent-encoded URL path segments.
    pub fn set_path<'a>(&mut self, segments: impl IntoIterator<Item = &'a str>) {
        let mut path = String::new();
        for segment in segments {
            path.push('/');
            path.push_str(segment);
        }
        self.replace_path(&path, false);
    }

    // Sets url's path to an already percent-encoded opaque path.
    pub fn set_opaque_path(&mut self, path: &str) {
        self.replace_path(path, true);
    }

    fn replace_path(&mut self, path: &str, has_opaque_path: bool) {
        let path_end = self.path_end();
        self.replace_range(self.components.path_start..path_end, path, Offset::QueryStart);
        self.components.has_opaque_path = has_opaque_path;
        self.update_empty_first_segment_marker();
    }

    // Sets url's query to an already percent-encoded query, or null.
    pub fn set_query(&mut self, query: Option<&str>) {
        let query_start = self.components.query_start().unwrap_or(self.path_end());
        let query_end = self.components.fragment_start().unwrap_or(self.end_offset());
        let serialized_query = query.map(|query| format!("?{query}")).unwrap_or_default();
        self.replace_range(query_start..query_end, &serialized_query, Offset::FragmentStart);
        self.components.set_query_start(query.is_some().then_some(query_start));
    }

    // Sets url's fragment to an already percent-encoded fragment, or null.
    pub fn set_fragment(&mut self, fragment: Option<&str>) {
        let fragment_start = self.components.fragment_start().unwrap_or(self.end_offset());
        self.serialization.truncate(fragment_start as usize);
        self.components.set_fragment_start(None);
        if let Some(fragment) = fragment {
            self.set_fragment_to_empty();
            self.serialization.push_str(fragment);
        }
    }

    // https://url.spec.whatwg.org/#concept-url-serializer
    // 3. If url’s host is null, url does not have an opaque path, url’s path’s size is greater than 1, and url’s path[0]
    //    is the empty string, then append U+002F (/) followed by U+002E (.) to output.
    // NB: A path whose size is greater than 1 and whose path[0] is the empty string serializes as starting with "//".
    pub(crate) fn update_empty_first_segment_marker(&mut self) {
        let components = self.components;
        let has_marker = self.slice(components.host_end, components.path_start) == "/.";
        let needs_marker = components.host_kind == HostKind::Null
            && !components.has_opaque_path
            && self.serialize_path().starts_with("//");

        if has_marker && !needs_marker {
            self.replace_range(components.host_end..components.path_start, "", Offset::PathStart);
        } else if !has_marker && needs_marker {
            self.replace_range(components.path_start..components.path_start, "/.", Offset::PathStart);
        }
    }

    // NB: The following append to a URL whose serialization ends with the component being parsed. The offsets of all
    //     later components are kept at the end, so that the setters above can also be used while parsing.

    pub(crate) fn append_scheme(&mut self, scheme: &str) {
        self.serialization.push_str(scheme);
        self.components.scheme_end = self.end_offset();
        self.components.scheme_type = SchemeType::from_scheme(scheme);
        self.serialization.push(':');
        self.components.username_end = self.end_offset();
        self.components.host_start = self.end_offset();
        self.components.host_end = self.end_offset();
        self.components.path_start = self.end_offset();
    }

    pub(crate) fn append_authority_start(&mut self) {
        self.serialization.push_str("//");
        self.components.username_end = self.end_offset();
        self.start_host();
        self.components.host_kind = HostKind::Domain;
    }

    pub(crate) fn start_host(&mut self) {
        self.components.host_start = self.end_offset();
        self.components.host_end = self.end_offset();
        self.components.path_start = self.end_offset();
    }

    pub(crate) fn start_path(&mut self) {
        self.components.path_start = self.end_offset();
    }

    pub(crate) fn start_opaque_path(&mut self) {
        self.start_path();
        self.components.has_opaque_path = true;
    }

    pub(crate) fn set_query_to_empty(&mut self) {
        self.components.set_query_start(Some(self.end_offset()));
        self.serialization.push('?');
    }

    pub(crate) fn set_fragment_to_empty(&mut self) {
        self.components.set_fragment_start(Some(self.end_offset()));
        self.serialization.push('#');
    }

    pub(crate) fn copy_credentials_host_and_port_from(&mut self, base_url: UrlRef<'_>) {
        if base_url.host_kind() == HostKind::Null {
            return;
        }

        let base_components = base_url.components;
        assert_eq!(self.components.scheme_end, base_components.scheme_end);
        self.serialization
            .push_str(base_url.slice(base_components.scheme_end + 1, base_components.path_start));
        self.components.username_end = base_components.username_end;
        self.components.host_start = base_components.host_start;
        self.components.host_end = base_components.host_end;
        self.components.path_start = base_components.path_start;
        self.components.host_kind = base_components.host_kind;
        self.components.port = base_components.port;
        self.components.has_port = base_components.has_port;
    }

    pub(crate) fn copy_host_from(&mut self, base_url: UrlRef<'_>) {
        match base_url.serialized_host() {
            Some(host) => self.set_host(base_url.host_kind(), host),
            None => self.set_host_to_null(),
        }
    }

    pub(crate) fn copy_path_from(&mut self, base_url: UrlRef<'_>) {
        self.serialization.push_str(base_url.serialize_path());
    }

    pub(crate) fn copy_query_from(&mut self, base_url: UrlRef<'_>) {
        if let Some(query) = base_url.query() {
            self.set_query_to_empty();
            self.serialization.push_str(query);
        }
    }

    pub(crate) fn detach_query_and_fragment(&mut self) -> DetachedTail {
        let fragment = self.take_fragment();
        let query = self.take_query();
        DetachedTail { query, fragment }
    }

    pub(crate) fn detach_fragment(&mut self) -> DetachedTail {
        DetachedTail {
            query: None,
            fragment: self.take_fragment(),
        }
    }

    fn take_query(&mut self) -> Option<String> {
        let query_start = self.components.query_start()?;
        self.components.set_query_start(None);
        Some(self.serialization.split_off(query_start as usize))
    }

    fn take_fragment(&mut self) -> Option<String> {
        let fragment_start = self.components.fragment_start()?;
        self.components.set_fragment_start(None);
        Some(self.serialization.split_off(fragment_start as usize))
    }

    // Restores a detached query and fragment, unless they were replaced since being detached.
    pub(crate) fn reattach(&mut self, tail: DetachedTail) {
        let fragment = self.take_fragment().or(tail.fragment);

        if self.components.query_start().is_none()
            && let Some(query) = tail.query
        {
            self.components.set_query_start(Some(self.end_offset()));
            self.serialization.push_str(&query);
        }

        if let Some(fragment) = fragment {
            self.components.set_fragment_start(Some(self.end_offset()));
            self.serialization.push_str(&fragment);
        }

        self.update_empty_first_segment_marker();
    }

    #[cfg(debug_assertions)]
    pub(crate) fn verify_invariants(&self) {
        let components = self.components;
        let bytes = self.serialization.as_bytes();
        let invariant = |holds: bool, description: &str| assert!(holds, "{description}: {self:?}");

        invariant(
            bytes.get(components.scheme_end as usize) == Some(&b':'),
            "scheme ends with ':'",
        );
        invariant(
            components.scheme_type == SchemeType::from_scheme(self.scheme()),
            "scheme type matches scheme",
        );

        let between_host_and_path = self.slice(components.host_end, components.path_start);
        if components.host_kind == HostKind::Null {
            invariant(
                components.username_end == components.scheme_end + 1
                    && components.host_start == components.username_end
                    && components.host_end == components.host_start,
                "null host has no authority",
            );
            invariant(!components.has_port, "null host has no port");
        } else {
            invariant(
                &bytes[components.scheme_end as usize + 1..components.scheme_end as usize + 3] == b"//",
                "host follows '//'",
            );
            invariant(
                components.scheme_end + 3 <= components.username_end
                    && components.username_end <= components.host_start
                    && components.host_start <= components.host_end
                    && components.host_end <= components.path_start,
                "authority offsets are ordered",
            );
            invariant(
                !self.includes_credentials() || bytes[components.host_start as usize - 1] == b'@',
                "credentials end with '@'",
            );
            let serialized_port = self.port().map(|port| format!(":{port}")).unwrap_or_default();
            invariant(between_host_and_path == serialized_port, "port follows host");
        }

        invariant(
            components.path_start <= self.path_end() && self.path_end() <= self.end_offset(),
            "path is in bounds",
        );
        if let Some(query_start) = components.query_start() {
            invariant(bytes[query_start as usize] == b'?', "query starts with '?'");
        }
        if let Some(fragment_start) = components.fragment_start() {
            invariant(bytes[fragment_start as usize] == b'#', "fragment starts with '#'");
            invariant(
                components
                    .query_start()
                    .is_none_or(|query_start| query_start < fragment_start),
                "query precedes fragment",
            );
        }

        let needs_marker = components.host_kind == HostKind::Null
            && !components.has_opaque_path
            && self.serialize_path().starts_with("//");
        invariant(
            (between_host_and_path == "/.") == needs_marker,
            "empty first segment marker is present exactly when needed",
        );
    }
}

pub(crate) struct DetachedTail {
    query: Option<String>,
    fragment: Option<String>,
}
