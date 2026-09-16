/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::HashMap;
use super::bridge::{FfiFontResolutionRequest, FfiResolvedFont};
use crate::css::style_value::{RetainedStyleValueData, retain_style_value};
use libgfx_rust::font::FontCascadeListHandle;
use std::ffi::c_void;

pub type ResolveFontCallback = unsafe extern "C" fn(*mut c_void, FfiFontResolutionRequest) -> FfiResolvedFont;

#[derive(PartialEq, Eq, Hash)]
struct FontResolutionKey {
    font_family: usize,
    font_size_raw: i32,
    font_slope: i32,
    font_weight: u64,
    font_width: u64,
    font_optical_sizing: u8,
}

impl FontResolutionKey {
    fn new(request: FfiFontResolutionRequest) -> Self {
        Self {
            font_family: request.font_family as usize,
            font_size_raw: request.font_size_raw,
            font_slope: request.font_slope,
            font_weight: request.font_weight.to_bits(),
            font_width: request.font_width.to_bits(),
            font_optical_sizing: request.font_optical_sizing,
        }
    }
}

/// Own the request's family until the boundary transfers it into the prepared table.
pub(super) struct FontRequest {
    ffi: FfiFontResolutionRequest,
    family: RetainedStyleValueData,
}

impl FontRequest {
    pub fn new(ffi: FfiFontResolutionRequest) -> Self {
        let family =
            unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(ffi.font_family.cast())) };
        Self { ffi, family }
    }
}

struct ResolvedFont {
    // Keep the family alive for the pointer identity in the cache key.
    _font_family: RetainedStyleValueData,
    _font_cascade_list: Option<FontCascadeListHandle>,
    ffi: FfiResolvedFont,
}

/// The resolutions this document has already been given, keyed by content and scoped to one
/// font-environment generation. This is retained engine state: an evaluation step reads it and
/// never calls the host.
#[derive(Default)]
pub(super) struct FontResolutionCache {
    generation: Option<u64>,
    cache: HashMap<FontResolutionKey, ResolvedFont>,
}

impl FontResolutionCache {
    pub fn prepare(&mut self, generation: u64) {
        if self.generation != Some(generation) {
            self.cache.clear();
            self.generation = Some(generation);
        }
    }

    pub fn lookup(&self, request: FfiFontResolutionRequest) -> Option<FfiResolvedFont> {
        if self.generation != Some(request.font_environment_generation) {
            return None;
        }
        self.cache
            .get(&FontResolutionKey::new(request))
            .map(|resolved| resolved.ffi)
    }

    fn insert(&mut self, request: FontRequest, ffi: FfiResolvedFont) {
        // A null result is a completed, unsupported host resolution, not another cache miss.
        let font_cascade_list = (!ffi.font_cascade_list.is_null()).then(|| {
            // SAFETY: The callback transfers one reference to a live list.
            unsafe { FontCascadeListHandle::adopt(ffi.font_cascade_list) }
        });
        self.cache.insert(
            FontResolutionKey::new(request.ffi),
            ResolvedFont {
                _font_family: request.family,
                _font_cascade_list: font_cascade_list,
                ffi,
            },
        );
    }
}

/// The host's synchronous font resolver. This is host state: it holds a C++ context pointer and
/// the callback into it, and only a round between evaluation passes may call it.
pub(super) struct FontResolverHost {
    context: *mut c_void,
    resolve: ResolveFontCallback,
}

impl FontResolverHost {
    pub fn new(context: *mut c_void, resolve: ResolveFontCallback) -> Self {
        Self { context, resolve }
    }

    /// Service a synchronous request between evaluation passes. Pending web faces remain in
    /// the returned cascade and retain the host's rendering-triggered loading behavior.
    pub fn refill(&self, cache: &mut FontResolutionCache, request: FontRequest) {
        cache.prepare(request.ffi.font_environment_generation);
        let ffi = unsafe { (self.resolve)(self.context, request.ffi) };
        cache.insert(request, ffi);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style_compute::ffi_test_stubs::font_cascade_list_unref_count;
    use crate::css::style_value::StyleValueData;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static RESOLVES: AtomicUsize = AtomicUsize::new(0);

    unsafe extern "C" fn resolve_font(_context: *mut c_void, _request: FfiFontResolutionRequest) -> FfiResolvedFont {
        RESOLVES.fetch_add(1, Ordering::Relaxed);
        FfiResolvedFont {
            first_available_font: std::ptr::dangling(),
            font_cascade_list: std::ptr::dangling(),
            ..Default::default()
        }
    }

    #[test]
    fn font_resolution_cache_is_engine_owned_and_generation_scoped() {
        RESOLVES.store(0, Ordering::Relaxed);
        let unrefs_before = font_cascade_list_unref_count();
        let family = RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: 1 });
        let host = FontResolverHost::new(std::ptr::null_mut(), resolve_font);
        let mut resolver = FontResolutionCache::default();
        let mut request = FfiFontResolutionRequest {
            font_family: family.pointer().cast(),
            font_size_raw: 1024,
            font_slope: 0,
            font_weight: 400.0,
            font_width: 100.0,
            font_optical_sizing: 0,
            font_environment_generation: 1,
        };

        resolver.prepare(1);
        assert!(resolver.lookup(request).is_none());
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 0);
        host.refill(&mut resolver, FontRequest::new(request));
        let first = resolver.lookup(request).unwrap();
        assert_eq!(
            resolver.lookup(request).unwrap().font_cascade_list,
            first.font_cascade_list
        );
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 1);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before);

        request.font_environment_generation = 2;
        assert!(resolver.lookup(request).is_none());
        resolver.prepare(2);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before + 1);
        host.refill(&mut resolver, FontRequest::new(request));
        resolver.lookup(request).unwrap();
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 2);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before + 1);

        drop(resolver);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before + 2);
    }

    #[test]
    fn unavailable_resolution_is_a_completed_answer_until_the_environment_changes() {
        unsafe extern "C" fn unavailable(_: *mut c_void, _: FfiFontResolutionRequest) -> FfiResolvedFont {
            FfiResolvedFont::default()
        }
        let family = RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: 1 });
        let request = FfiFontResolutionRequest {
            font_family: family.pointer().cast(),
            font_size_raw: 1024,
            font_slope: 0,
            font_weight: 400.0,
            font_width: 100.0,
            font_optical_sizing: 0,
            font_environment_generation: 1,
        };
        let host = FontResolverHost::new(std::ptr::null_mut(), unavailable);
        let mut resolver = FontResolutionCache::default();
        resolver.prepare(1);
        let owned = FontRequest::new(request);
        drop(family);
        assert!(resolver.lookup(request).is_none());
        host.refill(&mut resolver, owned);
        assert!(resolver.lookup(request).unwrap().font_cascade_list.is_null());
        assert!(resolver.lookup(request).unwrap().font_cascade_list.is_null());
        // A failed synchronous result must not cause an endless refill loop.
        let next = FfiFontResolutionRequest {
            font_environment_generation: 2,
            ..request
        };
        assert!(resolver.lookup(next).is_none());
        resolver.prepare(2);
    }
}
