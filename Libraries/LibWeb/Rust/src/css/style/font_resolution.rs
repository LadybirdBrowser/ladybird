/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::HashMap;
use super::bridge::{FfiFontResolutionRequest, FfiResolvedFont};
use crate::css::style_value::{RetainedStyleValueData, retain_style_value};
use std::ffi::c_void;

pub type ResolveFontCallback = unsafe extern "C" fn(*mut c_void, FfiFontResolutionRequest) -> FfiResolvedFont;
pub type RetainFontCallback = unsafe extern "C" fn(*const c_void);
pub type ReleaseFontCallback = unsafe extern "C" fn(*const c_void);

#[derive(PartialEq, Eq, Hash)]
struct FontResolutionKey {
    font_family: usize,
    font_size_raw: i32,
    font_slope: i32,
    font_weight: u64,
    font_width: u64,
    font_optical_sizing: u8,
}

struct ResolvedFont {
    // Keep the family alive for the pointer identity in the cache key.
    _font_family: RetainedStyleValueData,
    ffi: FfiResolvedFont,
    release: ReleaseFontCallback,
}

impl Drop for ResolvedFont {
    fn drop(&mut self) {
        unsafe { (self.release)(self.ffi.handle) };
    }
}

pub(super) struct FontResolver {
    context: *mut c_void,
    resolve: ResolveFontCallback,
    retain: RetainFontCallback,
    release: ReleaseFontCallback,
    generation: Option<u64>,
    cache: HashMap<FontResolutionKey, ResolvedFont>,
}

impl FontResolver {
    pub fn new(
        context: *mut c_void,
        resolve: ResolveFontCallback,
        retain: RetainFontCallback,
        release: ReleaseFontCallback,
    ) -> Self {
        Self {
            context,
            resolve,
            retain,
            release,
            generation: None,
            cache: HashMap::default(),
        }
    }

    #[allow(dead_code)]
    pub fn resolve(&mut self, request: FfiFontResolutionRequest) -> Option<FfiResolvedFont> {
        if self.generation != Some(request.font_environment_generation) {
            self.cache.clear();
            self.generation = Some(request.font_environment_generation);
        }
        let key = FontResolutionKey {
            font_family: request.font_family as usize,
            font_size_raw: request.font_size_raw,
            font_slope: request.font_slope,
            font_weight: request.font_weight.to_bits(),
            font_width: request.font_width.to_bits(),
            font_optical_sizing: request.font_optical_sizing,
        };
        if let Some(resolved) = self.cache.get(&key) {
            return Some(resolved.ffi);
        }
        let font_family =
            unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(request.font_family.cast())) };
        let resolved = unsafe { (self.resolve)(self.context, request) };
        if resolved.handle.is_null() {
            return None;
        }
        let ffi = resolved;
        self.cache.insert(
            key,
            ResolvedFont {
                _font_family: font_family,
                ffi,
                release: self.release,
            },
        );
        Some(ffi)
    }

    #[allow(dead_code)]
    pub fn retain(&self, handle: *const c_void) {
        unsafe { (self.retain)(handle) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style_value::StyleValueData;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static RESOLVES: AtomicUsize = AtomicUsize::new(0);
    static RETAINS: AtomicUsize = AtomicUsize::new(0);
    static RELEASES: AtomicUsize = AtomicUsize::new(0);

    unsafe extern "C" fn resolve_font(_context: *mut c_void, _request: FfiFontResolutionRequest) -> FfiResolvedFont {
        RESOLVES.fetch_add(1, Ordering::Relaxed);
        FfiResolvedFont {
            handle: std::ptr::dangling(),
            font_cascade_list: std::ptr::dangling(),
            ..Default::default()
        }
    }

    unsafe extern "C" fn retain_font(_handle: *const c_void) {
        RETAINS.fetch_add(1, Ordering::Relaxed);
    }

    unsafe extern "C" fn release_font(_handle: *const c_void) {
        RELEASES.fetch_add(1, Ordering::Relaxed);
    }

    #[test]
    fn font_resolution_cache_is_engine_owned_and_generation_scoped() {
        RESOLVES.store(0, Ordering::Relaxed);
        RETAINS.store(0, Ordering::Relaxed);
        RELEASES.store(0, Ordering::Relaxed);
        let family = RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: 1 });
        let mut resolver = FontResolver::new(std::ptr::null_mut(), resolve_font, retain_font, release_font);
        let mut request = FfiFontResolutionRequest {
            font_family: family.pointer().cast(),
            font_size_raw: 1024,
            font_slope: 0,
            font_weight: 400.0,
            font_width: 100.0,
            font_optical_sizing: 0,
            font_environment_generation: 1,
        };

        let first = resolver.resolve(request).unwrap();
        assert_eq!(resolver.resolve(request).unwrap().handle, first.handle);
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 1);

        resolver.retain(first.handle);
        assert_eq!(RETAINS.load(Ordering::Relaxed), 1);
        request.font_environment_generation = 2;
        resolver.resolve(request).unwrap();
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 2);
        assert_eq!(RELEASES.load(Ordering::Relaxed), 1);

        drop(resolver);
        assert_eq!(RELEASES.load(Ordering::Relaxed), 2);
    }
}
