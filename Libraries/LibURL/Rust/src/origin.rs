/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use crate::url::{BasicParseOptions, HostKind, Url, basic_parse};

unsafe extern "C" {
    // NB: LibWeb's library links its own copy of this crate, so process-wide state lives in LibURL's C++.
    fn url_rust_file_scheme_urls_have_tuple_origins() -> bool;

    fn url_rust_registrable_domain(
        domain: *const u8,
        domain_length: usize,
        ctx: *mut c_void,
        on_success: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    );
}

#[derive(Clone, Debug, Eq, PartialEq, Hash)]
pub struct Host {
    pub kind: HostKind,
    pub serialization: String,
}

impl Host {
    // https://url.spec.whatwg.org/#host-registrable-domain
    pub fn registrable_domain(&self) -> Option<String> {
        if self.kind != HostKind::Domain || self.serialization.is_empty() {
            return None;
        }

        unsafe extern "C" fn on_success(ctx: *mut c_void, data: *const u8, length: usize) {
            // SAFETY: ctx is the Option<String> below, and LibURL passes an ASCII string.
            let result = unsafe { &mut *ctx.cast::<Option<String>>() };
            let bytes = unsafe { ak::slice_from_raw_parts(data, length) };
            *result = Some(String::from_utf8(bytes.to_vec()).expect("a registrable domain is ASCII"));
        }

        let mut result: Option<String> = None;
        // SAFETY: The domain and the result outlive the call.
        unsafe {
            url_rust_registrable_domain(
                self.serialization.as_ptr(),
                self.serialization.len(),
                (&raw mut result).cast(),
                on_success,
            );
        }
        result
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum OpaqueOriginKind {
    #[default]
    Standard,
    File,
    SandboxedFile,
}

pub type Nonce = [u8; 16];

// https://html.spec.whatwg.org/multipage/browsers.html#concept-origin
#[derive(Clone, Debug)]
pub enum Origin {
    // https://html.spec.whatwg.org/multipage/browsers.html#concept-origin-opaque
    Opaque {
        nonce: Nonce,
        kind: OpaqueOriginKind,
    },
    // https://html.spec.whatwg.org/multipage/browsers.html#concept-origin-tuple
    Tuple {
        scheme: String,
        host: Host,
        port: Option<u16>,
        domain: Option<Host>,
    },
}

// https://html.spec.whatwg.org/multipage/browsers.html#site
enum Site<'a> {
    Opaque(&'a Nonce),
    SchemeAndHost(&'a str, Host),
}

impl Origin {
    pub fn new_opaque(kind: OpaqueOriginKind) -> Self {
        let mut nonce = Nonce::default();
        getrandom::fill(&mut nonce).expect("the platform CSPRNG fills buffers");
        Self::Opaque { nonce, kind }
    }

    fn new_tuple(scheme: &str, host: Host, port: Option<u16>) -> Self {
        Self::Tuple {
            scheme: scheme.to_owned(),
            host,
            port,
            domain: None,
        }
    }

    pub fn is_opaque(&self) -> bool {
        matches!(self, Self::Opaque { .. })
    }

    // https://html.spec.whatwg.org/multipage/origin.html#same-origin
    pub fn is_same_origin(&self, other: &Self) -> bool {
        match (self, other) {
            // 1. If A and B are the same opaque origin, then return true.
            (Self::Opaque { nonce: a, .. }, Self::Opaque { nonce: b, .. }) => a == b,
            // 2. If A and B are both tuple origins, their schemes and ports are identical, and their hosts are equal, then
            //    return true.
            (
                Self::Tuple {
                    scheme: scheme_a,
                    host: host_a,
                    port: port_a,
                    ..
                },
                Self::Tuple {
                    scheme: scheme_b,
                    host: host_b,
                    port: port_b,
                    ..
                },
            ) => scheme_a == scheme_b && port_a == port_b && host_a == host_b,
            // 3. Return false.
            _ => false,
        }
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#obtain-a-site
    fn site(&self) -> Site<'_> {
        match self {
            // 1. If origin is an opaque origin, then return origin.
            Self::Opaque { nonce, .. } => Site::Opaque(nonce),
            Self::Tuple { scheme, host, .. } => match host.registrable_domain() {
                // 2. If origin's host's registrable domain is null, then return (origin's scheme, origin's host).
                None => Site::SchemeAndHost(scheme, host.clone()),
                // 3. Return (origin's scheme, origin's host's registrable domain).
                Some(registrable_domain) => Site::SchemeAndHost(
                    scheme,
                    Host {
                        kind: HostKind::Domain,
                        serialization: registrable_domain,
                    },
                ),
            },
        }
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#same-site
    pub fn is_same_site(&self, other: &Self) -> bool {
        // 1. Let siteA be the result of obtaining a site given A.
        let site_a = self.site();

        // 2. Let siteB be the result of obtaining a site given B.
        let site_b = other.site();

        // 3. If siteA is same site with siteB, then return true.
        // 4. Return false.
        // https://html.spec.whatwg.org/multipage/browsers.html#concept-site-same-site
        match (site_a, site_b) {
            // 1. If A and B are the same opaque origin, then return true.
            (Site::Opaque(a), Site::Opaque(b)) => a == b,
            // 2. If A or B is an opaque origin, then return false.
            (Site::Opaque(_), _) | (_, Site::Opaque(_)) => false,
            // 3. If A's and B's scheme values are different, then return false.
            // 4. If A's and B's host values are not equal, then return false.
            // 5. Return true.
            (Site::SchemeAndHost(scheme_a, host_a), Site::SchemeAndHost(scheme_b, host_b)) => {
                scheme_a == scheme_b && host_a == host_b
            }
        }
    }

    // https://html.spec.whatwg.org/multipage/origin.html#ascii-serialisation-of-an-origin
    pub fn serialize(&self) -> String {
        match self {
            // 1. If origin is an opaque origin, then return "null"
            Self::Opaque { .. } => "null".to_owned(),
            Self::Tuple { scheme, host, port, .. } => {
                // 2. Otherwise, let result be origin's scheme.
                let mut result = scheme.clone();

                // 3. Append "://" to result.
                result.push_str("://");

                // 4. Append origin's host, serialized, to result.
                result.push_str(&host.serialization);

                // 5. If origin's port is non-null, append a U+003A COLON character (:), and origin's port, serialized,
                //    to result.
                if let Some(port) = port {
                    result.push(':');
                    result.push_str(&port.to_string());
                }

                // 6. Return result
                result
            }
        }
    }
}

impl<Serialization: AsRef<str>> Url<Serialization> {
    fn host(&self) -> Host {
        Host {
            kind: self.host_kind(),
            serialization: self.serialized_host().unwrap_or("").to_owned(),
        }
    }

    // https://url.spec.whatwg.org/#concept-url-origin
    // NB: A URL of this crate has no blob URL entry, so the caller runs step 1 of "blob".
    pub fn origin(&self) -> Origin {
        // The origin of a URL url is the origin returned by running these steps, switching on url’s scheme:
        match self.scheme() {
            // -> "blob"
            "blob" => {
                // 2. Let pathURL be the result of parsing the result of URL path serializing url.
                let Some(path_url) = basic_parse(self.serialize_path(), BasicParseOptions::new()) else {
                    // 3. If pathURL is failure, then return a new opaque origin.
                    return Origin::new_opaque(OpaqueOriginKind::Standard);
                };

                // 4. If pathURL’s scheme is "http", "https", or "file", then return pathURL’s origin.
                if matches!(path_url.scheme(), "http" | "https" | "file") {
                    return path_url.origin();
                }

                // 5. Return a new opaque origin.
                Origin::new_opaque(OpaqueOriginKind::Standard)
            }
            // -> "ftp"
            // -> "http"
            // -> "https"
            // -> "ws"
            // -> "wss"
            // Return the tuple origin (url’s scheme, url’s host, url’s port, null).
            "ftp" | "http" | "https" | "ws" | "wss" => Origin::new_tuple(self.scheme(), self.host(), self.port()),
            // AD-HOC: resource:// URLs are internal browser resources; give them a shared tuple origin
            // so that same-origin checks pass between any two resource:// documents or worker scripts.
            "resource" => Origin::new_tuple("resource", empty_host(), None),
            // -> "file"
            "file" => {
                // Unfortunate as it is, this is left as an exercise to the reader. When in doubt, return a new opaque origin.

                // Our implementation-defined behavior is to return an opaque origin for file:// URLs,
                // tagged explicitly as a "file" opaque origin rather than a fully anonymous one.
                //
                // This keeps file:// URLs opaque by default while still allowing downstream code to
                // identify and special-case them where needed - for example, to match cases where
                // other browsers treat a file:// origin as if it were a tuple origin.
                //
                // A process-wide flag can opt into tuple origins for file:// URLs instead. This is
                // intended for development/testing scenarios where web features requiring a non-opaque
                // origin (such as localStorage) need to work with file:// pages.
                // SAFETY: LibURL defines the function, which only reads a flag.
                if unsafe { url_rust_file_scheme_urls_have_tuple_origins() } {
                    return Origin::new_tuple("file", empty_host(), None);
                }

                Origin::new_opaque(OpaqueOriginKind::File)
            }
            // -> Otherwise
            // Return a new opaque origin.
            _ => Origin::new_opaque(OpaqueOriginKind::Standard),
        }
    }
}

fn empty_host() -> Host {
    Host {
        kind: HostKind::Domain,
        serialization: String::new(),
    }
}
