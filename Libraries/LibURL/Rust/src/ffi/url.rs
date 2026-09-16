/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::rust_panic::abort_on_panic;
use std::ffi::c_void;

use crate::url::BasicParseOptions;
use crate::url::HostKind;
use crate::url::State;
use crate::url::Url;
use crate::url::UrlComponents;
use crate::url::UrlRef;
use crate::url::basic_parse;
use crate::url::basic_parse_with_state_override;
use crate::url::{UrlInput, parse_host_into};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrlInput {
    pub utf8: *const u8,
    pub utf16: *const u16,
    pub length: usize,
}

impl RustUrlInput {
    /// # Safety
    /// The selected pointer must borrow length storage units. Byte input must be valid UTF-8.
    unsafe fn borrow<'a>(self) -> UrlInput<'a> {
        if self.length == 0 {
            return UrlInput::Utf8("");
        }
        if !self.utf16.is_null() {
            UrlInput::Utf16(unsafe { std::slice::from_raw_parts(self.utf16, self.length) })
        } else {
            UrlInput::Utf8(unsafe { std::str::from_utf8_unchecked(std::slice::from_raw_parts(self.utf8, self.length)) })
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrlByteSlice {
    pub data: *const u8,
    pub length: usize,
}

impl RustUrlByteSlice {
    fn from_str(string: &str) -> Self {
        Self {
            data: string.as_ptr(),
            length: string.len(),
        }
    }

    /// # Safety
    /// A non-null data pointer must borrow length bytes of valid UTF-8.
    unsafe fn borrow<'a>(self) -> Option<&'a str> {
        if self.data.is_null() {
            return None;
        }
        let bytes = unsafe { std::slice::from_raw_parts(self.data, self.length) };
        Some(std::str::from_utf8(bytes).expect("URL strings should be valid UTF-8"))
    }
}

/// A URL borrowed across the FFI boundary.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrl {
    pub serialization: RustUrlByteSlice,
    pub components: UrlComponents,
}

impl RustUrl {
    fn from_url(url: &Url) -> Self {
        Self {
            serialization: RustUrlByteSlice::from_str(url.as_str()),
            components: url.components(),
        }
    }

    /// # Safety
    /// The serialization must be borrowed for 'a, and the components must describe it.
    pub(crate) unsafe fn borrow<'a>(&self) -> UrlRef<'a> {
        Url {
            serialization: unsafe { self.serialization.borrow() }.unwrap_or(""),
            components: self.components,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiUrlHost {
    pub kind: HostKind,
    pub ipv4: [u8; 4],
    pub ipv6: [u8; 16],
    pub string_data: *const u8,
    pub string_length: usize,
}

impl FfiUrlHost {
    fn new(kind: HostKind, serialized_host: &str) -> Self {
        let mut host = Self {
            kind,
            ipv4: [0; 4],
            ipv6: [0; 16],
            string_data: serialized_host.as_ptr(),
            string_length: serialized_host.len(),
        };
        match kind {
            HostKind::Ipv4 => {
                host.ipv4 = serialized_host
                    .parse::<std::net::Ipv4Addr>()
                    .expect("serialized IPv4 address should parse")
                    .octets();
            }
            HostKind::Ipv6 => {
                host.ipv6 = serialized_host[1..serialized_host.len() - 1]
                    .parse::<std::net::Ipv6Addr>()
                    .expect("serialized IPv6 address should parse")
                    .octets();
            }
            HostKind::Null | HostKind::Domain | HostKind::Opaque => {}
        }
        host
    }
}

pub type FfiUrlResultFn = unsafe extern "C" fn(*mut c_void, *const RustUrl);
pub type FfiHostResultFn = unsafe extern "C" fn(*mut c_void, *const FfiUrlHost);

/// # Safety
/// `url` must be a valid URL borrowed for the duration of the call, and `on_complete` is called exactly once with the
/// modified URL.
unsafe fn modify_url(
    url: *const RustUrl,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
    modify: impl FnOnce(&mut Url),
) {
    let mut url = Url::from(unsafe { (*url).borrow() });
    modify(&mut url);
    let result = RustUrl::from_url(&url);
    // SAFETY: result borrows from url, which lives until the callback returns.
    unsafe { on_complete(ctx, &raw const result) };
}

/// # Safety
/// `input` must borrow its declared storage, with valid UTF-8 for byte input. `base_url` must be null or a valid URL,
/// and `encoding` must be a null or valid UTF-8 slice, borrowed for the duration of this call. `on_complete` is called
/// exactly once, with the parsed URL or null on failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_basic_parse(
    input: RustUrlInput,
    base_url: *const RustUrl,
    encoding: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) -> bool {
    abort_on_panic(|| {
        let input = unsafe { input.borrow() };
        let mut options = BasicParseOptions::new().encoding(unsafe { encoding.borrow() });
        if let Some(base_url) = unsafe { base_url.as_ref() } {
            options = options.base_url(unsafe { base_url.borrow() });
        }

        let Some(url) = basic_parse(input, options) else {
            // SAFETY: on_complete is a valid function pointer; ctx is caller-provided.
            unsafe { on_complete(ctx, std::ptr::null()) };
            return false;
        };

        let result = RustUrl::from_url(&url);
        // SAFETY: result borrows from url, which lives until the callback returns.
        unsafe { on_complete(ctx, &raw const result) };
        true
    })
}

/// # Safety
/// `input` must borrow its declared storage, with valid UTF-8 for byte input. `url` must be a valid URL, and `encoding`
/// must be a null or valid UTF-8 slice, borrowed for the duration of this call. `on_complete` is called exactly once,
/// with url as modified by the parser, including when parsing fails.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_basic_parse_with_state_override(
    input: RustUrlInput,
    url: *const RustUrl,
    state_override: State,
    encoding: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) -> bool {
    abort_on_panic(|| {
        let input = unsafe { input.borrow() };
        let encoding = unsafe { encoding.borrow() };
        let mut did_succeed = false;
        unsafe {
            modify_url(url, ctx, on_complete, |url| {
                did_succeed = basic_parse_with_state_override(input, url, state_override, encoding);
            })
        };
        did_succeed
    })
}

/// # Safety
/// `url` and `username` must be valid for the duration of this call. `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_username(
    url: *const RustUrl,
    username: RustUrlInput,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| url.set_username(username.borrow()));
    })
}

/// # Safety
/// `url` and `password` must be valid for the duration of this call. `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_password(
    url: *const RustUrl,
    password: RustUrlInput,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| url.set_password(password.borrow()));
    })
}

/// # Safety
/// `url` must be valid for the duration of this call. `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_port(
    url: *const RustUrl,
    has_port: bool,
    port: u16,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| url.set_port(has_port.then_some(port)));
    })
}

/// # Safety
/// `url` and `query` must be valid for the duration of this call, with a null `query` setting url's query to null.
/// `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_query(
    url: *const RustUrl,
    query: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| url.set_query(query.borrow()));
    })
}

/// # Safety
/// `url` and `fragment` must be valid for the duration of this call, with a null `fragment` setting url's fragment to
/// null. `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_fragment(
    url: *const RustUrl,
    fragment: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| url.set_fragment(fragment.borrow()));
    })
}

/// # Safety
/// `url` and `scheme` must be valid for the duration of this call. `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_scheme(
    url: *const RustUrl,
    scheme: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| {
            url.set_scheme(scheme.borrow().unwrap_or(""))
        });
    })
}

/// # Safety
/// `url` and `host` must be valid for the duration of this call, with `host` holding a serialized host of the given
/// kind. `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_host(
    url: *const RustUrl,
    kind: HostKind,
    host: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| {
            url.set_host(kind, host.borrow().unwrap_or(""))
        });
    })
}

/// # Safety
/// `url` must be valid for the duration of this call, and `segments` must point to `segment_count` percent-encoded
/// URL path segments. `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_path(
    url: *const RustUrl,
    segments: *const RustUrlByteSlice,
    segment_count: usize,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        let segments = if segment_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(segments, segment_count)
        };
        modify_url(url, ctx, on_complete, |url| {
            url.set_path(segments.iter().map(|segment| segment.borrow().unwrap_or("")))
        });
    })
}

/// # Safety
/// `url` and `path` must be valid for the duration of this call, with `path` holding a percent-encoded opaque path.
/// `on_complete` is called exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_set_opaque_path(
    url: *const RustUrl,
    path: RustUrlByteSlice,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) {
    abort_on_panic(|| unsafe {
        modify_url(url, ctx, on_complete, |url| {
            url.set_opaque_path(path.borrow().unwrap_or(""))
        });
    })
}

/// # Safety
/// `url` must be valid for the duration of this call. The returned host borrows url's serialization.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_host(url: *const RustUrl) -> FfiUrlHost {
    abort_on_panic(|| {
        let url = unsafe { (*url).borrow() };
        FfiUrlHost::new(url.host_kind(), url.serialized_host().unwrap_or(""))
    })
}

/// # Safety
/// `input` must borrow its declared storage, with valid UTF-8 for byte input.
/// `on_complete` is called exactly once with either a host result or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_parse_host(
    input: RustUrlInput,
    is_opaque: bool,
    ctx: *mut c_void,
    on_complete: FfiHostResultFn,
) -> bool {
    abort_on_panic(|| {
        let input_str = unsafe { input.borrow() };

        let mut host = String::new();
        let Some(kind) = parse_host_into(input_str, is_opaque, &mut host) else {
            // SAFETY: on_complete is a valid function pointer; ctx is caller-provided.
            unsafe { on_complete(ctx, std::ptr::null()) };
            return false;
        };

        let ffi_result = FfiUrlHost::new(kind, &host);

        // SAFETY: ffi_result borrows from host, which lives until the callback returns.
        unsafe { on_complete(ctx, &raw const ffi_result) };
        true
    })
}
