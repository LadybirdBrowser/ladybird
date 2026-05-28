/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::net::Ipv4Addr;
use std::net::Ipv6Addr;
use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

use crate::url::BasicParseOptions;
use crate::url::Host;
use crate::url::State;
use crate::url::Url;
use crate::url::basic_parse;
use crate::url::basic_parse_into;
use crate::url::is_special_scheme;
use crate::url::parse_host;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RustUrlByteSlice {
    pub data: *const u8,
    pub length: usize,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustUrlHostKind {
    String,
    Ipv4,
    Ipv6,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiUrlHost {
    pub has_host: bool,
    pub kind: RustUrlHostKind,
    pub ipv4: [u8; 4],
    pub ipv6: [u8; 16],
    pub string_data: *const u8,
    pub string_length: usize,
}

impl Default for FfiUrlHost {
    fn default() -> Self {
        Self {
            has_host: false,
            kind: RustUrlHostKind::String,
            ipv4: [0; 4],
            ipv6: [0; 16],
            string_data: std::ptr::null(),
            string_length: 0,
        }
    }
}

#[repr(C)]
pub struct RustFfiUrl {
    pub scheme: RustUrlByteSlice,
    pub username: RustUrlByteSlice,
    pub password: RustUrlByteSlice,
    pub host: FfiUrlHost,
    pub has_port: bool,
    pub port: u16,
    pub path_segments: *const RustUrlByteSlice,
    pub path_segment_count: usize,
    pub has_opaque_path: bool,
    pub has_query: bool,
    pub query: RustUrlByteSlice,
    pub has_fragment: bool,
    pub fragment: RustUrlByteSlice,
}

/// FFI parse options borrowed from C++ storage.
///
/// The embedded `RustFfiUrl` values contain raw pointers into `UrlFfiStorage`
/// objects on the C++ side. Those storage objects must outlive the call to
/// `rust_url_basic_parse`.
#[repr(C)]
pub struct RustBasicParseOptions {
    pub has_base_url: bool,
    pub has_url: bool,
    pub base_url: RustFfiUrl,
    pub url: RustFfiUrl,
    pub has_state_override: bool,
    pub state_override: State,
    pub encoding: RustUrlByteSlice,
}

pub type FfiUrlResultFn = unsafe extern "C" fn(*mut c_void, *const RustFfiUrl);
pub type FfiHostResultFn = unsafe extern "C" fn(*mut c_void, *const FfiUrlHost);

fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(_) => std::process::abort(),
    }
}

fn decode_utf8(slice: RustUrlByteSlice) -> String {
    if slice.data.is_null() {
        return String::new();
    }
    // SAFETY: slice.data is valid for slice.length bytes (contract with C++ caller).
    let bytes = unsafe { std::slice::from_raw_parts(slice.data, slice.length) };
    std::str::from_utf8(bytes)
        .expect("URL fields are valid UTF-8")
        .to_owned()
}

fn host_from_ffi(ffi: &FfiUrlHost, scheme: &str) -> Option<Host> {
    if !ffi.has_host {
        return None;
    }
    Some(match ffi.kind {
        RustUrlHostKind::String => {
            let s = decode_utf8(RustUrlByteSlice {
                data: ffi.string_data,
                length: ffi.string_length,
            });
            if is_special_scheme(scheme.as_bytes()) {
                Host::Domain(s)
            } else {
                Host::Opaque(s)
            }
        }
        RustUrlHostKind::Ipv4 => Host::Ipv4(Ipv4Addr::new(ffi.ipv4[0], ffi.ipv4[1], ffi.ipv4[2], ffi.ipv4[3])),
        RustUrlHostKind::Ipv6 => Host::Ipv6(Ipv6Addr::from(ffi.ipv6)),
    })
}

fn host_to_ffi(host: Option<&Host>) -> FfiUrlHost {
    let Some(host) = host else {
        return FfiUrlHost::default();
    };
    match host {
        Host::Domain(s) | Host::Opaque(s) => FfiUrlHost {
            has_host: true,
            kind: RustUrlHostKind::String,
            string_data: s.as_ptr(),
            string_length: s.len(),
            ..FfiUrlHost::default()
        },
        Host::Ipv4(addr) => FfiUrlHost {
            has_host: true,
            kind: RustUrlHostKind::Ipv4,
            ipv4: addr.octets(),
            ..FfiUrlHost::default()
        },
        Host::Ipv6(addr) => FfiUrlHost {
            has_host: true,
            kind: RustUrlHostKind::Ipv6,
            ipv6: addr.octets(),
            ..FfiUrlHost::default()
        },
    }
}

pub(crate) fn url_from_ffi(ffi: &RustFfiUrl) -> Url {
    let scheme = decode_utf8(ffi.scheme);
    let path = if !ffi.path_segments.is_null() {
        // SAFETY: path_segments is valid for path_segment_count elements.
        let segments = unsafe { std::slice::from_raw_parts(ffi.path_segments, ffi.path_segment_count) };
        segments.iter().map(|s| decode_utf8(*s)).collect()
    } else {
        vec![]
    };
    Url {
        scheme: scheme.clone(),
        username: decode_utf8(ffi.username),
        password: decode_utf8(ffi.password),
        host: host_from_ffi(&ffi.host, &scheme),
        port: ffi.has_port.then_some(ffi.port),
        path,
        has_opaque_path: ffi.has_opaque_path,
        query: ffi.has_query.then(|| decode_utf8(ffi.query)),
        fragment: ffi.has_fragment.then(|| decode_utf8(ffi.fragment)),
    }
}

fn url_to_ffi_result<'a>(url: &'a Url, path_slices: &'a [RustUrlByteSlice]) -> RustFfiUrl {
    let null_slice = RustUrlByteSlice {
        data: std::ptr::null(),
        length: 0,
    };
    RustFfiUrl {
        scheme: RustUrlByteSlice {
            data: url.scheme.as_ptr(),
            length: url.scheme.len(),
        },
        username: RustUrlByteSlice {
            data: url.username.as_ptr(),
            length: url.username.len(),
        },
        password: RustUrlByteSlice {
            data: url.password.as_ptr(),
            length: url.password.len(),
        },
        host: host_to_ffi(url.host.as_ref()),
        has_port: url.port.is_some(),
        port: url.port.unwrap_or(0),
        path_segments: path_slices.as_ptr(),
        path_segment_count: path_slices.len(),
        has_opaque_path: url.has_opaque_path,
        has_query: url.query.is_some(),
        query: url.query.as_deref().map_or(null_slice, |s: &str| RustUrlByteSlice {
            data: s.as_ptr(),
            length: s.len(),
        }),
        has_fragment: url.fragment.is_some(),
        fragment: url.fragment.as_deref().map_or(null_slice, |s: &str| RustUrlByteSlice {
            data: s.as_ptr(),
            length: s.len(),
        }),
    }
}

/// # Safety
/// `input` must be valid for `input_length` bytes. `options` must be a valid pointer
/// whose embedded URL pointers remain valid for the duration of this call. `on_complete`
/// is called exactly once with the parse result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_basic_parse(
    input: *const u8,
    input_length: usize,
    options: *const RustBasicParseOptions,
    ctx: *mut c_void,
    on_complete: FfiUrlResultFn,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: caller guarantees input is scalar-value UTF-8 and valid for input_length bytes.
        let input_str = unsafe { std::str::from_utf8_unchecked(std::slice::from_raw_parts(input, input_length)) };

        let options = unsafe { options.as_ref() };

        let base_url = options.filter(|o| o.has_base_url).map(|o| url_from_ffi(&o.base_url));

        let existing_url = options.filter(|o| o.has_url).map(|o| url_from_ffi(&o.url));

        let state_override = options.filter(|o| o.has_state_override).map(|o| o.state_override);

        let encoding = options.and_then(|o| {
            if o.encoding.data.is_null() {
                None
            } else {
                Some(decode_utf8(o.encoding))
            }
        });

        let mut parse_options = BasicParseOptions::new()
            .state_override(state_override)
            .encoding(encoding.as_deref());
        parse_options.base_url = base_url.as_ref();

        let (did_succeed, maybe_url) = if let Some(mut existing_url) = existing_url {
            let did_succeed = basic_parse_into(input_str, &mut existing_url, &parse_options);
            (did_succeed, Some(existing_url))
        } else if let Some(parsed) = basic_parse(input_str, parse_options) {
            (true, Some(parsed))
        } else {
            (false, None)
        };

        let Some(url) = maybe_url else {
            // SAFETY: on_complete is a valid function pointer; ctx is caller-provided.
            unsafe { on_complete(ctx, std::ptr::null()) };
            return false;
        };

        // Build path slices borrowing from url.path — all live until end of closure.
        let path_slices: Vec<RustUrlByteSlice> = url
            .path
            .iter()
            .map(|s: &String| RustUrlByteSlice {
                data: s.as_ptr(),
                length: s.len(),
            })
            .collect();

        let ffi_result = url_to_ffi_result(&url, &path_slices);

        // SAFETY: ffi_result borrows from url and path_slices, both live here.
        unsafe { on_complete(ctx, &raw const ffi_result) };
        did_succeed
    })
}

/// # Safety
/// `input` must be valid for `input_length` bytes.
/// `on_complete` is called exactly once with either a host result or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_parse_host(
    input: *const u8,
    input_length: usize,
    is_opaque: bool,
    ctx: *mut c_void,
    on_complete: FfiHostResultFn,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: caller guarantees input is scalar-value UTF-8 and valid for input_length bytes.
        let input_str = unsafe { std::str::from_utf8_unchecked(std::slice::from_raw_parts(input, input_length)) };

        let Some(host) = parse_host(input_str, is_opaque) else {
            // SAFETY: on_complete is a valid function pointer; ctx is caller-provided.
            unsafe { on_complete(ctx, std::ptr::null()) };
            return false;
        };

        let ffi_result = host_to_ffi(Some(&host));

        // SAFETY: ffi_result borrows from host, which lives until the callback returns.
        unsafe { on_complete(ctx, &raw const ffi_result) };
        true
    })
}
