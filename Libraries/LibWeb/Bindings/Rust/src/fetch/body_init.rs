/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm};

use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, cell_into_abi};
use crate::domurl::url_search_params::URLSearchParams;
use crate::fetch::infrastructure::http::bodies::CppBody;
use crate::file_api::blob::AnyBlob;
use crate::xhr::form_data::FormData;

#[repr(u8)]
enum BodyInitKind {
    Blob = 0,
    Bytes = 1,
    FormData = 2,
    URLSearchParams = 3,
    String = 4,
}

#[repr(C)]
struct BodyInitAbi<'a> {
    kind: BodyInitKind,
    cell: *mut c_void,
    bytes: ak::Span<'a, u8>,
    string: usize,
}

unsafe extern "C" {
    fn web_fetch_safely_extract_body(
        realm: *mut c_void,
        object: *const BodyInitAbi<'_>,
        out_type: *mut usize,
    ) -> *mut c_void;
}

/// https://fetch.spec.whatwg.org/#typedefdef-xmlhttprequestbodyinit
pub(crate) enum XMLHttpRequestBodyInit {
    Blob(AnyBlob),
    Bytes(Vec<u8>),
    FormData(Ref<FormData>),
    URLSearchParams(Ref<URLSearchParams>),
    String(UsvString),
}

/// https://fetch.spec.whatwg.org/#body-with-type
pub(crate) struct BodyWithType {
    pub(crate) body: CppBody,
    pub(crate) r#type: Option<Utf16String>,
}

/// https://fetch.spec.whatwg.org/#bodyinit-safely-extract
pub(crate) fn safely_extract_body(realm: Gc<Realm>, object: &XMLHttpRequestBodyInit) -> BodyWithType {
    let mut abi = BodyInitAbi {
        kind: BodyInitKind::Bytes,
        cell: core::ptr::null_mut(),
        bytes: ak::Span::new(&[]),
        string: 0,
    };
    match object {
        XMLHttpRequestBodyInit::Blob(blob) => {
            abi.kind = BodyInitKind::Blob;
            abi.cell = blob.clone().cell_into_abi();
        }
        XMLHttpRequestBodyInit::Bytes(bytes) => abi.bytes = ak::Span::new(bytes),
        XMLHttpRequestBodyInit::FormData(form_data) => {
            abi.kind = BodyInitKind::FormData;
            abi.cell = cell_into_abi(*form_data);
        }
        XMLHttpRequestBodyInit::URLSearchParams(url_search_params) => {
            abi.kind = BodyInitKind::URLSearchParams;
            abi.cell = cell_into_abi(*url_search_params);
        }
        XMLHttpRequestBodyInit::String(string) => {
            abi.kind = BodyInitKind::String;
            abi.string = string.as_utf16_string().raw_identity();
        }
    }
    let mut r#type = 0;
    // SAFETY: The realm and the object are live, the object is lent for the call, and LibWeb returns a live body and
    //         hands over 0 or the type.
    unsafe {
        let body = CppBody::from_raw(web_fetch_safely_extract_body(
            realm.as_ptr().cast(),
            &raw const abi,
            &raw mut r#type,
        ));
        BodyWithType {
            body,
            r#type: (r#type != 0).then(|| Utf16String::from_raw_owned(r#type)),
        }
    }
}
