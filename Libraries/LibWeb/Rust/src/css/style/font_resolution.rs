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

struct ResolvedFont {
    // Keep the family alive for the pointer identity in the cache key.
    _font_family: RetainedStyleValueData,
    _font_cascade_list: FontCascadeListHandle,
    ffi: FfiResolvedFont,
}

pub(super) struct FontResolver {
    context: *mut c_void,
    resolve: ResolveFontCallback,
    generation: Option<u64>,
    cache: HashMap<FontResolutionKey, ResolvedFont>,
}

impl FontResolver {
    pub fn new(context: *mut c_void, resolve: ResolveFontCallback) -> Self {
        Self {
            context,
            resolve,
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
        let ffi = unsafe { (self.resolve)(self.context, request) };
        if ffi.font_cascade_list.is_null() {
            return None;
        }
        // SAFETY: The resolver callback hands over one reference to a live list.
        let font_cascade_list = unsafe { FontCascadeListHandle::adopt(ffi.font_cascade_list) };
        self.cache.insert(
            key,
            ResolvedFont {
                _font_family: font_family,
                _font_cascade_list: font_cascade_list,
                ffi,
            },
        );
        Some(ffi)
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
        let mut resolver = FontResolver::new(std::ptr::null_mut(), resolve_font);
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
        assert_eq!(
            resolver.resolve(request).unwrap().font_cascade_list,
            first.font_cascade_list
        );
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 1);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before);

        request.font_environment_generation = 2;
        resolver.resolve(request).unwrap();
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 2);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before + 1);

        drop(resolver);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before + 2);
    }
}
