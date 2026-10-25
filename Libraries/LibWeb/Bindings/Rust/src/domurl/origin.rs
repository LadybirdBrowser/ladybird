/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Origin, https://html.spec.whatwg.org/multipage/browsers.html#the-origin-interface

use ak::Utf16String;
use libjs_rust::api::{Trace, Value, is_host_instance_of};
use liburl_rust::origin::{OpaqueOriginKind, Origin as OriginValue};
use liburl_rust::{BasicParseOptions, FfiOrigin, basic_parse};

use super::url::URL;
use super::url_input;
use crate::bindings::conversions::to_dom_string;
use crate::bindings::interface::{Interface, Ref, Wrappable, allocate, implementation_of};
use crate::bindings::services::web_bindings_extract_an_origin;
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Origin {
    // https://html.spec.whatwg.org/multipage/browsers.html#concept-origin-origin
    // Origin objects have an associated origin, which holds an origin.
    #[gc(untraced)]
    origin: OriginValue,
}

impl Wrappable for Origin {}

impl Origin {
    // https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-constructor
    pub(crate) fn construct(_: &mut CallContext) -> WebIdlResult<Self> {
        // The new Origin() constructor steps are to set this's origin to a unique opaque origin.
        Ok(Self {
            origin: OriginValue::new_opaque(OpaqueOriginKind::Standard),
        })
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-from
    pub(crate) fn from(cx: &mut CallContext, value: Value) -> WebIdlResult<Ref<Self>> {
        // 1. If value is a platform object:
        //     1. Let origin be the result of executing value's extract an origin operation.
        //     2. If origin is not null, then return a new Origin object whose origin is origin.
        if let Some(origin) = extract_an_origin(value) {
            return Ok(allocate(cx.vm(), |_| Self { origin }));
        }

        // 2. If value is a string:
        if value.is_string() {
            // 1. Let parsedURL be the result of basic URL parsing value.
            let string = to_dom_string(cx.vm(), value)?;
            let parsed_url = basic_parse(url_input(string.as_units()), BasicParseOptions::new());

            // 2. If parsedURL is not failure, then return a new Origin object whose origin is set to parsedURL's
            //    origin.
            if let Some(parsed_url) = parsed_url {
                let origin = parsed_url.origin();
                return Ok(allocate(cx.vm(), |_| Self { origin }));
            }
        }

        // 3. Throw a TypeError.
        Err(Exception::TypeError(Utf16String::from_utf8(
            "Value is not a valid Origin",
        )))
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-opaque
    pub(crate) fn opaque(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The opaque getter steps are to return true if this's origin is an opaque origin; otherwise false.
        Ok(self.origin.is_opaque())
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-issameorigin
    pub(crate) fn is_same_origin(&self, _: &mut CallContext, other: Ref<Self>) -> WebIdlResult<bool> {
        // The isSameOrigin(other) method steps are to return true if this's origin is same origin with other's
        // origin; otherwise false.
        Ok(self.origin.is_same_origin(&other.origin))
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#dom-origin-issamesite
    pub(crate) fn is_same_site(&self, _: &mut CallContext, other: Ref<Self>) -> WebIdlResult<bool> {
        // The isSameSite(other) method steps are to return true if this's origin is same site with other's origin;
        // otherwise false.
        Ok(self.origin.is_same_site(&other.origin))
    }
}

// https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin
fn extract_an_origin(value: Value) -> Option<OriginValue> {
    if !value.is_object() {
        return None;
    }
    let object = value.as_object();

    // Objects implementing the Origin interface's extract an origin steps are to return this's origin.
    if is_host_instance_of(&object, Origin::wrapper_class()) {
        return Some(implementation_of::<Origin>(object).origin.clone());
    }
    if is_host_instance_of(&object, URL::wrapper_class()) {
        return Some(implementation_of::<URL>(object).extract_an_origin());
    }

    unsafe extern "C" fn on_origin(ctx: *mut core::ffi::c_void, ffi_origin: *const FfiOrigin) {
        // SAFETY: ctx is the result below, and LibWeb lends the origin for the call.
        unsafe { *ctx.cast::<Option<OriginValue>>() = Some((*ffi_origin).to_origin()) };
    }

    let mut origin = None;
    // SAFETY: The object is live, and the result outlives the call.
    unsafe { web_bindings_extract_an_origin(object.as_ptr().cast(), (&raw mut origin).cast(), on_origin) };
    origin
}
