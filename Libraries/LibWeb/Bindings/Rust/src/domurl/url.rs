/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! URL, https://url.spec.whatwg.org/#url-class

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use liburl_rust::origin::Origin;
use liburl_rust::url::{ExcludeFragment, State};
use liburl_rust::{BasicParseOptions, Url, basic_parse, basic_parse_with_state_override};

use super::url_input;
use super::url_search_params::{URLSearchParams, parse_query};
use crate::bindings::conversions::{CppInterface, UsvString};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::file_api::blob::AnyBlob;

unsafe extern "C" {
    /// Adds an entry to the blob URL store for the Blob or File whose cell the call borrows, and returns the URL as a
    /// raw AK::Utf16String that the caller owns.
    fn web_url_create_object_url_for_blob(blob: *mut c_void) -> usize;

    /// Adds an entry to the blob URL store for the MediaSource that the wrapper wraps, and returns the URL as a raw
    /// AK::Utf16String that the caller owns.
    fn web_url_create_object_url_for_media_source(media_source: *mut c_void) -> usize;

    /// Runs the revokeObjectURL(url) steps for the URL, a raw AK::Utf16String that the call borrows.
    fn web_url_revoke_object_url(url: usize);
}

/// The union (Blob or MediaSource) of DOMURL/URL.idl, whose members the bindings convert.
pub(crate) enum BlobOrMediaSource {
    Blob(AnyBlob),
    MediaSource(CppInterface),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
#[expect(
    clippy::upper_case_acronyms,
    reason = "the bindings name the implementation after the interface"
)]
pub(crate) struct URL {
    // https://url.spec.whatwg.org/#concept-url-url
    #[gc(untraced)]
    url: RefCell<Url>,
    // https://url.spec.whatwg.org/#concept-url-query-object
    query: Cell<Option<Ref<URLSearchParams>>>,
}

impl Wrappable for URL {}

// https://url.spec.whatwg.org/#api-url-parser
fn parse_api_url(url: &UsvString, base: Option<&UsvString>) -> Option<Url> {
    // 1. Let parsedBase be null.
    // 2. If base is non-null:
    let parsed_base = match base {
        // 1. Set parsedBase to the result of running the basic URL parser on base.
        // 2. If parsedBase is failure, then return failure.
        Some(base) => Some(basic_parse(url_input(base.units()), BasicParseOptions::new())?),
        None => None,
    };

    // 3. Return the result of running the basic URL parser on url with parsedBase.
    let mut options = BasicParseOptions::new();
    if let Some(parsed_base) = &parsed_base {
        options = options.base_url(parsed_base);
    }
    basic_parse(url_input(url.units()), options)
}

impl URL {
    // https://url.spec.whatwg.org/#url-initialize
    fn initialize(url_record: Url) -> Self {
        // OPTIMIZATION: Defer creating and initializing the query object until searchParams is accessed.
        // 2. Set url's URL to urlRecord.
        Self {
            url: RefCell::new(url_record),
            query: Cell::new(None),
        }
    }

    // https://url.spec.whatwg.org/#url-initialize
    // NB: The steps of initialize that searchParams defers.
    fn initialize_query_object(this: &Ref<Self>, vm: &Vm) -> Ref<URLSearchParams> {
        // 1. Let query be urlRecord's query, if that is non-null; otherwise the empty string.
        let url = this.url.borrow();
        let query = url.query().unwrap_or("");

        // 3. Set url's query object to a new URLSearchParams object.
        // 4. Initialize url's query object with query.
        // 5. Set url's query object's URL object to url.
        let query_object = allocate(vm, |_| URLSearchParams::new(parse_query(query), Some(*this)));
        this.query.set(Some(query_object));
        query_object
    }

    // https://url.spec.whatwg.org/#dom-url-url
    pub(crate) fn construct(_: &mut CallContext, url: UsvString, base: Option<UsvString>) -> WebIdlResult<Self> {
        // 1. Let parsedURL be the result of running the API URL parser on url with base, if given.
        // 2. If parsedURL is failure, then throw a TypeError.
        let parsed_url = parse_api_url(&url, base.as_ref())
            .ok_or_else(|| Exception::TypeError(Utf16String::from_utf8("Invalid URL")))?;

        // 3. Initialize this with parsedURL.
        Ok(Self::initialize(parsed_url))
    }

    // https://url.spec.whatwg.org/#dom-url-parse
    pub(crate) fn parse(
        cx: &mut CallContext,
        url: UsvString,
        base: Option<UsvString>,
    ) -> WebIdlResult<Option<Ref<Self>>> {
        // 1. Let parsedURL be the result of running the API URL parser on url with base, if given.
        // 2. If parsedURL is failure, then return null.
        let Some(parsed_url) = parse_api_url(&url, base.as_ref()) else {
            return Ok(None);
        };

        // 3. Let url be a new URL object.
        // 4. Initialize url with parsedURL.
        // 5. Return url.
        Ok(Some(allocate(cx.vm(), |_| Self::initialize(parsed_url))))
    }

    // https://url.spec.whatwg.org/#dom-url-canparse
    pub(crate) fn can_parse(_: &mut CallContext, url: UsvString, base: Option<UsvString>) -> WebIdlResult<bool> {
        // 1. Let parsedURL be the result of running the API URL parser on url with base, if given.
        // 2. If parsedURL is failure, then return false.
        // 3. Return true.
        Ok(parse_api_url(&url, base.as_ref()).is_some())
    }

    // https://w3c.github.io/FileAPI/#dfn-createObjectURL
    pub(crate) fn create_object_url(_: &mut CallContext, obj: BlobOrMediaSource) -> WebIdlResult<Utf16String> {
        // The createObjectURL(obj) static method must return the result of adding an entry to the blob URL store for
        // obj.
        // SAFETY: The object is live, and LibWeb returns a string that the caller owns.
        Ok(unsafe {
            Utf16String::from_raw_owned(match obj {
                BlobOrMediaSource::Blob(blob) => web_url_create_object_url_for_blob(blob.cell_into_abi()),
                BlobOrMediaSource::MediaSource(media_source) => {
                    web_url_create_object_url_for_media_source(media_source.wrapper().as_ptr().cast())
                }
            })
        })
    }

    // https://w3c.github.io/FileAPI/#dfn-revokeObjectURL
    pub(crate) fn revoke_object_url(_: &mut CallContext, url: Utf16String) -> WebIdlResult<()> {
        // SAFETY: The string outlives the call, which borrows it.
        unsafe { web_url_revoke_object_url(url.raw_identity()) };
        Ok(())
    }

    // https://url.spec.whatwg.org/#extract-an-origin
    pub(crate) fn extract_an_origin(&self) -> Origin {
        // Objects implementing the URL interface's extract an origin steps are to return this's URL's origin.
        self.url.borrow().origin()
    }

    pub(crate) fn set_query(&self, query: Option<&str>) {
        self.url.borrow_mut().set_query(query);
    }

    // https://url.spec.whatwg.org/#dom-url-href
    pub(crate) fn href(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The href getter steps and the toJSON() method steps are to return the serialization of this's URL.
        Ok(UsvString::from_ascii(
            self.url.borrow().serialize(ExcludeFragment::No).as_bytes(),
        ))
    }

    // https://url.spec.whatwg.org/#dom-url-tojson
    pub(crate) fn to_json(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The href getter steps and the toJSON() method steps are to return the serialization of this's URL.
        Ok(UsvString::from_ascii(
            self.url.borrow().serialize(ExcludeFragment::No).as_bytes(),
        ))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-href②
    pub(crate) fn set_href(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        // 1. Let parsedURL be the result of running the basic URL parser on the given value.
        // 2. If parsedURL is failure, then throw a TypeError.
        let parsed_url = basic_parse(url_input(value.units()), BasicParseOptions::new())
            .ok_or_else(|| Exception::TypeError(Utf16String::from_utf8("Invalid URL")))?;

        // 3. Set this's URL to parsedURL.
        *self.url.borrow_mut() = parsed_url;

        if let Some(query_object) = self.query.get() {
            // 4. Empty this's query object's list.
            // 5. Let query be this's URL's query.
            // 6. If query is non-null, then set this's query object's list to the result of parsing query.
            query_object.set_list(self.url.borrow().query().map(parse_query).unwrap_or_default());
        }
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-origin
    pub(crate) fn origin(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The origin getter steps are to return the serialization of this's URL's origin. [HTML]
        Ok(UsvString::from_ascii(self.url.borrow().origin().serialize().as_bytes()))
    }

    // https://url.spec.whatwg.org/#dom-url-protocol
    pub(crate) fn protocol(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The protocol getter steps are to return this's URL's scheme, followed by U+003A (:).
        Ok(UsvString::from_ascii(
            format!("{}:", self.url.borrow().scheme()).as_bytes(),
        ))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-protocol%E2%91%A0
    pub(crate) fn set_protocol(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        // The protocol setter steps are to basic URL parse the given value, followed by U+003A (:), with this's URL as
        // url and scheme start state as state override.
        let value_with_colon = format!("{}:", value.to_utf8());
        basic_parse_with_state_override(
            value_with_colon.as_str(),
            &mut self.url.borrow_mut(),
            State::SchemeStart,
            None,
        );
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-username
    pub(crate) fn username(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The username getter steps are to return this's URL's username.
        Ok(UsvString::from_ascii(self.url.borrow().username().as_bytes()))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-username%E2%91%A0
    pub(crate) fn set_username(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        let mut url = self.url.borrow_mut();

        // 1. If this's URL cannot have a username/password/port, then return.
        if url.cannot_have_a_username_or_password_or_port() {
            return Ok(());
        }

        // 2. Set the username given this's URL and the given value.
        url.set_username(url_input(value.units()));
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-password
    pub(crate) fn password(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The password getter steps are to return this's URL's password.
        Ok(UsvString::from_ascii(self.url.borrow().password().as_bytes()))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-password%E2%91%A0
    pub(crate) fn set_password(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        let mut url = self.url.borrow_mut();

        // 1. If this's URL cannot have a username/password/port, then return.
        if url.cannot_have_a_username_or_password_or_port() {
            return Ok(());
        }

        // 2. Set the password given this's URL and the given value.
        url.set_password(url_input(value.units()));
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-host
    pub(crate) fn host(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. Let url be this's URL.
        let url = self.url.borrow();

        // 2. If url's host is null, then return the empty string.
        let Some(host) = url.serialized_host() else {
            return Ok(UsvString::from_ascii(b""));
        };

        // 3. If url's port is null, return url's host, serialized.
        let Some(port) = url.port() else {
            return Ok(UsvString::from_ascii(host.as_bytes()));
        };

        // 4. Return url's host, serialized, followed by U+003A (:) and url's port, serialized.
        Ok(UsvString::from_ascii(format!("{host}:{port}").as_bytes()))
    }

    // https://url.spec.whatwg.org/#dom-url-host
    pub(crate) fn set_host(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        let mut url = self.url.borrow_mut();

        // 1. If this's URL has an opaque path, then return.
        if url.has_opaque_path() {
            return Ok(());
        }

        // 2. Basic URL parse the given value with this's URL as url and host state as state override.
        basic_parse_with_state_override(url_input(value.units()), &mut url, State::Host, None);
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-hostname
    pub(crate) fn hostname(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. If this's URL's host is null, then return the empty string.
        // 2. Return this's URL's host, serialized.
        Ok(UsvString::from_ascii(
            self.url.borrow().serialized_host().unwrap_or("").as_bytes(),
        ))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-hostname①
    pub(crate) fn set_hostname(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        let mut url = self.url.borrow_mut();

        // 1. If this's URL has an opaque path, then return.
        if url.has_opaque_path() {
            return Ok(());
        }

        // 2. Basic URL parse the given value with this's URL as url and hostname state as state override.
        basic_parse_with_state_override(url_input(value.units()), &mut url, State::Hostname, None);
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-port
    pub(crate) fn port(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. If this's URL's port is null, then return the empty string.
        // 2. Return this's URL's port, serialized.
        let port = self
            .url
            .borrow()
            .port()
            .map(|port| port.to_string())
            .unwrap_or_default();
        Ok(UsvString::from_ascii(port.as_bytes()))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-port%E2%91%A0
    pub(crate) fn set_port(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        let mut url = self.url.borrow_mut();

        // 1. If this's URL cannot have a username/password/port, then return.
        if url.cannot_have_a_username_or_password_or_port() {
            return Ok(());
        }

        // 2. If the given value is the empty string, then set this's URL's port to null.
        if value.length_in_code_units() == 0 {
            url.set_port(None);
        }
        // 3. Otherwise, basic URL parse the given value with this's URL as url and port state as state override.
        else {
            basic_parse_with_state_override(url_input(value.units()), &mut url, State::Port, None);
        }
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-pathname
    pub(crate) fn pathname(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // The pathname getter steps are to return the result of URL path serializing this's URL.
        Ok(UsvString::from_ascii(self.url.borrow().serialize_path().as_bytes()))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-pathname%E2%91%A0
    pub(crate) fn set_pathname(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        let mut url = self.url.borrow_mut();

        // 1. If this's URL has an opaque path, then return.
        if url.has_opaque_path() {
            return Ok(());
        }

        // 2. Empty this's URL's path.
        url.set_path(core::iter::empty());

        // 3. Basic URL parse the given value with this's URL as url and path start state as state override.
        basic_parse_with_state_override(url_input(value.units()), &mut url, State::PathStart, None);
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-search
    pub(crate) fn search(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. If this's URL's query is either null or the empty string, then return the empty string.
        // 2. Return U+003F (?), followed by this's URL's query.
        let query = match self.url.borrow().query() {
            None | Some("") => String::new(),
            Some(query) => format!("?{query}"),
        };
        Ok(UsvString::from_ascii(query.as_bytes()))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-search%E2%91%A0
    pub(crate) fn set_search(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        // 1. Let url be this's URL.
        let mut url = self.url.borrow_mut();

        // 2. If the given value is the empty string, then set url's query to null, empty this's query object's list,
        //    and return.
        if value.length_in_code_units() == 0 {
            url.set_query(None);
            if let Some(query_object) = self.query.get() {
                query_object.set_list(Vec::new());
            }
            return Ok(());
        }

        // 3. Let input be the given value with a single leading U+003F (?) removed, if any.
        let input = value.strip_ascii_prefix(b'?').unwrap_or(value);

        // 4. Set url's query to the empty string.
        url.set_query(Some(""));

        // 5. Basic URL parse input with url as url and query state as state override.
        basic_parse_with_state_override(url_input(input.units()), &mut url, State::Query, None);

        // 6. Set this's query object's list to the result of parsing url's query.
        if let Some(query_object) = self.query.get() {
            query_object.set_list(url.query().map(parse_query).unwrap_or_default());
        }
        Ok(())
    }

    // https://url.spec.whatwg.org/#dom-url-searchparams
    pub(crate) fn search_params(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<Ref<URLSearchParams>> {
        // The searchParams getter steps are to return this's query object.
        Ok(this
            .query
            .get()
            .unwrap_or_else(|| Self::initialize_query_object(this, cx.vm())))
    }

    // https://url.spec.whatwg.org/#dom-url-hash
    pub(crate) fn hash(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        // 1. If this's URL's fragment is either null or the empty string, then return the empty string.
        // 2. Return U+0023 (#), followed by this's URL's fragment.
        let fragment = match self.url.borrow().fragment() {
            None | Some("") => String::new(),
            Some(fragment) => format!("#{fragment}"),
        };
        Ok(UsvString::from_ascii(fragment.as_bytes()))
    }

    // https://url.spec.whatwg.org/#ref-for-dom-url-hash%E2%91%A0
    pub(crate) fn set_hash(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        let mut url = self.url.borrow_mut();

        // 1. If the given value is the empty string, then set this's URL's fragment to null and return.
        if value.length_in_code_units() == 0 {
            url.set_fragment(None);
            return Ok(());
        }

        // 2. Let input be the given value with a single leading U+0023 (#) removed, if any.
        let input = value.strip_ascii_prefix(b'#').unwrap_or(value);

        // 3. Set this's URL's fragment to the empty string.
        url.set_fragment(Some(""));

        // 4. Basic URL parse input with this's URL as url and fragment state as state override.
        basic_parse_with_state_override(url_input(input.units()), &mut url, State::Fragment, None);
        Ok(())
    }
}
