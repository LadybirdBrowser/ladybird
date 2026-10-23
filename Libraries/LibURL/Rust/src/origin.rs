/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::url::{BasicParseOptions, HostKind, Url, basic_parse};

unsafe extern "C" {
    // NB: LibWeb's library links its own copy of this crate, so process-wide state lives in LibURL's C++.
    fn url_rust_file_scheme_urls_have_tuple_origins() -> bool;
}

#[derive(Clone, Debug, Eq, PartialEq, Hash)]
pub struct Host {
    pub kind: HostKind,
    pub serialization: String,
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
