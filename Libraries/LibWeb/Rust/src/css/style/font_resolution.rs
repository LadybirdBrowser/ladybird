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

/// Resolves one request against the document's published `@font-face` table, through the memo of
/// what has been resolved from it. It is handed nothing else, so it cannot reach the document.
pub type ResolveFontCallback =
    unsafe extern "C" fn(memo: *mut c_void, snapshot: *const c_void, FfiFontResolutionRequest) -> FfiResolvedFont;

unsafe extern "C" {
    fn web_css_resolve_font(
        memo: *mut c_void,
        snapshot: *const c_void,
        request: FfiFontResolutionRequest,
    ) -> FfiResolvedFont;
    fn web_css_font_face_snapshot_unreference(snapshot: *const c_void);
    fn web_css_font_cascade_memo_unreference(memo: *const c_void);
}

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
            font_family: request.font_family.address,
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
        let family = unsafe {
            RetainedStyleValueData::from_retained_pointer(retain_style_value(ffi.font_family.as_pointer().cast()))
        };
        Self { ffi, family }
    }
}

/// One reference to a host `Gfx::FontCascadeList`, held so the list the engine names stays alive
/// while a resolution names it.
struct SharedFontCascadeList(#[expect(dead_code, reason = "held for the reference it owns")] FontCascadeListHandle);

// SAFETY: An evaluation step reaches this type only through `&FontResolutionCache`, and `lookup`
// copies the `FfiResolvedFont` out without ever naming the handle, so no worker can move or drop
// one. That is exactly `Sync` and deliberately not `Send`: the handle is borrowed by a walk,
// never given to it. What matters is which thread performs the *final* release, because that runs
// `~FontCascadeList`. This handle is the reason it is never a worker: the cache holds one reference
// per cached resolution for the whole font-environment generation, taken here and given up only in
// `FontResolutionCache::prepare` or when the cache is dropped - a host round on the engine's thread.
// A step that builds a font style group takes a second reference to the same list
// (`build_font_group` in `table_group_builder.rs`) and may give it up again when the rebuilt payload
// is canonicalized away. `Gfx::FontCascadeList`, `Gfx::Font`, and `Gfx::Typeface` are all atomically
// reference-counted, so that pair is safe. Keeping final destruction on the host also keeps it away
// from concurrently used mutable font and typeface caches.
unsafe impl Sync for SharedFontCascadeList {}

struct ResolvedFont {
    // Keep the family alive for the pointer identity in the cache key.
    _font_family: RetainedStyleValueData,
    _font_cascade_list: Option<SharedFontCascadeList>,
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
        let font_cascade_list = (!ffi.font_cascade_list.is_none()).then(|| {
            // SAFETY: The callback transfers one reference to a live list.
            SharedFontCascadeList(unsafe { FontCascadeListHandle::adopt(ffi.font_cascade_list.as_pointer()) })
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

/// One reference to a host object, handed back when this is dropped. The raw pointer keeps it
/// neither `Send` nor `Sync`: the host counts its references without atomics, and the memo behind
/// one changes on every resolution, so it stays on the thread that published it.
struct HostReference {
    object: *mut c_void,
    unreference: unsafe extern "C" fn(*const c_void),
}

impl HostReference {
    /// # Safety
    /// `object` must be live, with one reference this takes over and `unreference` gives up.
    unsafe fn adopt(object: *const c_void, unreference: unsafe extern "C" fn(*const c_void)) -> Self {
        Self {
            object: object.cast_mut(),
            unreference,
        }
    }
}

impl Drop for HostReference {
    fn drop(&mut self) {
        // SAFETY: This owns one reference, taken in `adopt`.
        unsafe { (self.unreference)(self.object) };
    }
}

/// The document's `@font-face` table as published, and the memo of the cascades resolved from it:
/// one reference to each host object.
pub(super) struct PublishedFontFaces {
    snapshot: HostReference,
    memo: HostReference,
}

impl PublishedFontFaces {
    /// # Safety
    /// `snapshot` and `memo` must be a live `Web::CSS::FontFaceSnapshot` and `FontCascadeMemo`, one
    /// reference to each of which this takes over.
    pub(super) unsafe fn adopt(snapshot: *const c_void, memo: *const c_void) -> Self {
        unsafe {
            Self {
                snapshot: HostReference::adopt(snapshot, web_css_font_face_snapshot_unreference),
                memo: HostReference::adopt(memo, web_css_font_cascade_memo_unreference),
            }
        }
    }
}

/// The host's synchronous font resolver and the table it resolves against. This is host state, and
/// only a round between evaluation passes may call it.
pub(super) struct FontResolverHost {
    resolve: ResolveFontCallback,
    font_faces: PublishedFontFaces,
}

impl FontResolverHost {
    pub(super) fn new(font_faces: PublishedFontFaces) -> Self {
        Self::with_callback(web_css_resolve_font, font_faces)
    }

    fn with_callback(resolve: ResolveFontCallback, font_faces: PublishedFontFaces) -> Self {
        Self { resolve, font_faces }
    }

    /// The table the document published last, which every later resolution reads.
    pub(super) fn publish(&mut self, font_faces: PublishedFontFaces) {
        self.font_faces = font_faces;
    }

    /// Service a synchronous request between evaluation passes. Pending web faces remain in
    /// the returned cascade and retain the host's rendering-triggered loading behavior.
    pub fn refill(&self, cache: &mut FontResolutionCache, request: FontRequest) {
        cache.prepare(request.ffi.font_environment_generation);
        let ffi = unsafe {
            (self.resolve)(
                self.font_faces.memo.object,
                self.font_faces.snapshot.object,
                request.ffi,
            )
        };
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

    unsafe extern "C" fn resolve_font(
        _memo: *mut c_void,
        _snapshot: *const c_void,
        _request: FfiFontResolutionRequest,
    ) -> FfiResolvedFont {
        RESOLVES.fetch_add(1, Ordering::Relaxed);
        FfiResolvedFont {
            first_available_font: crate::css::style::bridge::FfiHostHandle::from_pointer(std::ptr::dangling()),
            font_cascade_list: crate::css::style::bridge::FfiHostHandle::from_pointer(std::ptr::dangling()),
            ..Default::default()
        }
    }

    #[test]
    fn font_resolution_cache_is_engine_owned_and_generation_scoped() {
        RESOLVES.store(0, Ordering::Relaxed);
        let unrefs_before = font_cascade_list_unref_count();
        let family = RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: 1 });
        let host = FontResolverHost::with_callback(resolve_font, unsafe {
            PublishedFontFaces::adopt(std::ptr::dangling(), std::ptr::dangling())
        });
        let mut resolver = FontResolutionCache::default();
        let mut request = FfiFontResolutionRequest {
            font_family: crate::css::style::bridge::FfiHostHandle::from_pointer(family.pointer().cast()),
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
        unsafe extern "C" fn unavailable(
            _: *mut c_void,
            _: *const c_void,
            _: FfiFontResolutionRequest,
        ) -> FfiResolvedFont {
            FfiResolvedFont::default()
        }
        let family = RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: 1 });
        let request = FfiFontResolutionRequest {
            font_family: crate::css::style::bridge::FfiHostHandle::from_pointer(family.pointer().cast()),
            font_size_raw: 1024,
            font_slope: 0,
            font_weight: 400.0,
            font_width: 100.0,
            font_optical_sizing: 0,
            font_environment_generation: 1,
        };
        let host = FontResolverHost::with_callback(unavailable, unsafe {
            PublishedFontFaces::adopt(std::ptr::dangling(), std::ptr::dangling())
        });
        let mut resolver = FontResolutionCache::default();
        resolver.prepare(1);
        let owned = FontRequest::new(request);
        drop(family);
        assert!(resolver.lookup(request).is_none());
        host.refill(&mut resolver, owned);
        assert!(resolver.lookup(request).unwrap().font_cascade_list.is_none());
        assert!(resolver.lookup(request).unwrap().font_cascade_list.is_none());
        // A failed synchronous result must not cause an endless refill loop.
        let next = FfiFontResolutionRequest {
            font_environment_generation: 2,
            ..request
        };
        assert!(resolver.lookup(next).is_none());
        resolver.prepare(2);
    }
}
