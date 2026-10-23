/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use crate::ffi::url::{FfiUrlHost, RustUrl, RustUrlByteSlice};
use crate::origin::{Host, Nonce, OpaqueOriginKind, Origin};
use crate::rust_panic::abort_on_panic;
use crate::url::HostKind;

/// An origin borrowed across the FFI boundary. A tuple origin without a domain has a domain of kind Null.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiOrigin {
    pub is_opaque: bool,
    pub opaque_kind: OpaqueOriginKind,
    pub nonce: Nonce,
    pub scheme: RustUrlByteSlice,
    pub host: FfiUrlHost,
    pub has_port: bool,
    pub port: u16,
    pub domain: FfiUrlHost,
}

impl FfiOrigin {
    fn from_origin(origin: &Origin) -> Self {
        let null_host = FfiUrlHost::new(HostKind::Null, "");
        match origin {
            Origin::Opaque { nonce, kind } => Self {
                is_opaque: true,
                opaque_kind: *kind,
                nonce: *nonce,
                scheme: RustUrlByteSlice::from_str(""),
                host: null_host,
                has_port: false,
                port: 0,
                domain: null_host,
            },
            Origin::Tuple {
                scheme,
                host,
                port,
                domain,
            } => Self {
                is_opaque: false,
                opaque_kind: OpaqueOriginKind::Standard,
                nonce: Nonce::default(),
                scheme: RustUrlByteSlice::from_str(scheme),
                host: FfiUrlHost::new(host.kind, &host.serialization),
                has_port: port.is_some(),
                port: port.unwrap_or(0),
                domain: domain
                    .as_ref()
                    .map_or(null_host, |domain| FfiUrlHost::new(domain.kind, &domain.serialization)),
            },
        }
    }

    /// Lends the origin to `f` across the FFI boundary.
    pub fn with_origin<R>(origin: &Origin, f: impl FnOnce(&FfiOrigin) -> R) -> R {
        f(&Self::from_origin(origin))
    }

    /// # Safety
    /// The strings of the origin must be valid UTF-8 for the duration of this call. The string of a host is its
    /// serialization, whatever its kind.
    pub unsafe fn to_origin(&self) -> Origin {
        if self.is_opaque {
            return Origin::Opaque {
                nonce: self.nonce,
                kind: self.opaque_kind,
            };
        }
        let host_from_ffi = |host: &FfiUrlHost| Host {
            kind: host.kind,
            serialization: unsafe {
                RustUrlByteSlice {
                    data: host.string_data,
                    length: host.string_length,
                }
                .borrow()
            }
            .unwrap_or("")
            .to_owned(),
        };
        Origin::Tuple {
            scheme: unsafe { self.scheme.borrow() }.unwrap_or("").to_owned(),
            host: host_from_ffi(&self.host),
            port: self.has_port.then_some(self.port),
            domain: (self.domain.kind != HostKind::Null).then(|| host_from_ffi(&self.domain)),
        }
    }
}

pub type FfiOriginResultFn = unsafe extern "C" fn(*mut c_void, *const FfiOrigin);

/// # Safety
/// `url` must be valid for the duration of this call. `on_complete` is called exactly once, with the origin of the
/// URL, which it borrows for the duration of that call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_url_origin(url: *const RustUrl, ctx: *mut c_void, on_complete: FfiOriginResultFn) {
    abort_on_panic(|| {
        let origin = unsafe { (*url).borrow() }.origin();
        FfiOrigin::with_origin(&origin, |ffi_origin| {
            // SAFETY: on_complete is a valid function pointer; ctx is caller-provided.
            unsafe { on_complete(ctx, ffi_origin) };
        });
    })
}
