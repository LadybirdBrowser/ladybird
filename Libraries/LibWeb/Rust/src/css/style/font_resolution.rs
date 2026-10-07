/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::HashMap;
use super::bridge::{FONT_RESOLUTION_FEATURE_INPUT_COUNT, FfiFontResolutionRequest, FfiHostHandle, FfiResolvedFont};
use super::tree::TreeScopeID;
use crate::css::host_shared::HostShared;
use crate::css::style_value::{RetainedStyleValueData, retain_style_value};
use std::ffi::c_void;

/// Resolves one request against the document's published `@font-face` table, through the memo of
/// what has been resolved from it. It is handed nothing else, so it cannot reach the document.
pub type ResolveFontCallback =
    unsafe extern "C" fn(memo: *const c_void, snapshot: *const c_void, FfiFontResolutionRequest) -> FfiResolvedFont;

unsafe extern "C" {
    fn web_css_resolve_font(
        memo: *const c_void,
        snapshot: *const c_void,
        request: FfiFontResolutionRequest,
    ) -> FfiResolvedFont;
    fn web_css_font_face_snapshot_reference(snapshot: *const c_void);
    fn web_css_font_face_snapshot_unreference(snapshot: *const c_void);
    fn web_css_font_cascade_memo_reference(memo: *const c_void);
    fn web_css_font_cascade_memo_unreference(memo: *const c_void);
}

#[derive(Clone, PartialEq, Eq, Hash)]
struct FontResolutionKey {
    font_family: usize,
    font_feature_values: [usize; FONT_RESOLUTION_FEATURE_INPUT_COUNT],
    font_size_raw: i32,
    font_slope: i32,
    font_weight: u64,
    font_width: u64,
    font_optical_sizing: u8,
    font_feature_values_scope: u32,
}

impl FontResolutionKey {
    fn new(request: FfiFontResolutionRequest) -> Self {
        Self {
            font_family: request.font_family.address,
            font_feature_values: request.font_feature_values.map(|value| value.address),
            font_size_raw: request.font_size_raw,
            font_slope: request.font_slope,
            font_weight: request.font_weight.to_bits(),
            font_width: request.font_width.to_bits(),
            font_optical_sizing: request.font_optical_sizing,
            font_feature_values_scope: request.font_feature_values_scope,
        }
    }
}

/// Own the request's family and the feature values it names until the boundary transfers them
/// into the prepared table.
#[derive(Clone)]
pub(super) struct FontRequest {
    ffi: FfiFontResolutionRequest,
    family: RetainedStyleValueData,
    feature_values: [Option<RetainedStyleValueData>; FONT_RESOLUTION_FEATURE_INPUT_COUNT],
}

impl FontRequest {
    pub fn new(ffi: FfiFontResolutionRequest) -> Self {
        let retain = |handle: FfiHostHandle| unsafe {
            RetainedStyleValueData::from_retained_pointer(retain_style_value(handle.as_pointer().cast()))
        };
        Self {
            ffi,
            family: retain(ffi.font_family),
            feature_values: ffi
                .font_feature_values
                .map(|value| (!value.as_pointer().is_null()).then(|| retain(value))),
        }
    }
}

#[derive(Clone)]

struct ResolvedFont {
    // Keep the family and feature values alive for the pointer identities in the cache key.
    _font_family: RetainedStyleValueData,
    _font_feature_values: [Option<RetainedStyleValueData>; FONT_RESOLUTION_FEATURE_INPUT_COUNT],
    ffi: FfiResolvedFont,
}

/// The resolutions this document has already been given, keyed by content and scoped to one
/// font-environment generation. This is retained engine state: an evaluation step reads it and
/// never calls the host.
///
/// It names the cascades it was given without holding a reference to any of them. The host's
/// cascade memo keeps every cascade it resolves alive until the font environment generation
/// changes, and a lookup answers only for the generation the cache was filled at. So the engine
/// never gives up the last reference to a `Gfx::FontCascadeList`, whose destructor releases fonts
/// into host caches, and nothing in the cache stops it from moving to another thread.
#[derive(Clone, Default)]
pub(super) struct FontResolutionCache {
    generation: Option<u64>,
    cache: HashMap<FontResolutionKey, ResolvedFont>,
    /// The shadow tree scopes whose own `@font-feature-values` the published table carries.
    feature_values_shadow_scopes: Box<[TreeScopeID]>,
}

impl FontResolutionCache {
    pub fn publish_feature_values_shadow_scopes(&mut self, scopes: Box<[TreeScopeID]>) {
        self.feature_values_shadow_scopes = scopes;
    }

    pub fn feature_values_shadow_scopes(&self) -> &[TreeScopeID] {
        &self.feature_values_shadow_scopes
    }

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
        // The host resolves every request, to the fallback font where nothing else matches, so the
        // resolution always names a list.
        debug_assert!(!ffi.font_cascade_list.is_none());
        self.cache.insert(
            FontResolutionKey::new(request.ffi),
            ResolvedFont {
                _font_family: request.family,
                _font_feature_values: request.feature_values,
                ffi,
            },
        );
    }
}

/// A `Web::CSS::FontFaceSnapshot`, which Rust only names by pointer.
#[repr(C)]
#[derive(Clone)]
struct FontFaceSnapshotObject {
    _opaque: [u8; 0],
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

// SAFETY: The snapshot is immutable once built and counts its references atomically, so a shared
// reference to one may be read, taken and given up on any thread.
unsafe impl Sync for FontFaceSnapshotObject {}

/// A `Web::CSS::FontCascadeMemo`, which Rust only names by pointer.
#[repr(C)]
#[derive(Clone)]
struct FontCascadeMemoObject {
    _opaque: [u8; 0],
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

// SAFETY: The memo counts its references atomically, so a shared reference to it may be taken and
// given up on any thread. Resolving through it reads the document's font face state, which the
// memo's lock does not cover, so only `FontResolverHost::refill` resolves, and only the document
// thread reaches the host.
unsafe impl Sync for FontCascadeMemoObject {}

/// One reference to a host object, given up on drop. It may cross threads exactly when the object
/// may be shared between them.
struct HostReference<T> {
    object: HostShared<T>,
    reference: unsafe extern "C" fn(*const c_void),
    unreference: unsafe extern "C" fn(*const c_void),
}

impl<T> HostReference<T> {
    /// # Safety
    /// `object` must be a live `T`, with one reference this takes over and `unreference` gives up.
    unsafe fn adopt(
        object: *const c_void,
        reference: unsafe extern "C" fn(*const c_void),
        unreference: unsafe extern "C" fn(*const c_void),
    ) -> Self {
        Self {
            object: HostShared::new(object.cast()),
            reference,
            unreference,
        }
    }

    fn as_ptr(&self) -> *const c_void {
        self.object.as_ptr().cast()
    }
}

/// A fork holds its own reference.
impl<T> Clone for HostReference<T> {
    fn clone(&self) -> Self {
        // SAFETY: This owns one reference, so the object is live.
        unsafe { (self.reference)(self.as_ptr()) };
        Self {
            object: self.object,
            reference: self.reference,
            unreference: self.unreference,
        }
    }
}

impl<T> Drop for HostReference<T> {
    fn drop(&mut self) {
        // SAFETY: This owns one reference, taken in `adopt`.
        unsafe { (self.unreference)(self.as_ptr()) };
    }
}

/// The document's `@font-face` table as published, and the memo of the cascades resolved from it:
/// one reference to each host object.
#[derive(Clone)]
pub(crate) struct PublishedFontFaces {
    snapshot: HostReference<FontFaceSnapshotObject>,
    memo: HostReference<FontCascadeMemoObject>,
}

impl PublishedFontFaces {
    /// # Safety
    /// `snapshot` and `memo` must be a live `Web::CSS::FontFaceSnapshot` and `FontCascadeMemo`, one
    /// reference to each of which this takes over.
    pub(super) unsafe fn adopt(snapshot: *const c_void, memo: *const c_void) -> Self {
        unsafe {
            Self {
                snapshot: HostReference::adopt(
                    snapshot,
                    web_css_font_face_snapshot_reference,
                    web_css_font_face_snapshot_unreference,
                ),
                memo: HostReference::adopt(
                    memo,
                    web_css_font_cascade_memo_reference,
                    web_css_font_cascade_memo_unreference,
                ),
            }
        }
    }
}

/// The host's synchronous font resolver and the table it resolves against. This is host state, and
/// only a round between evaluation passes may call it.
#[derive(Clone)]
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

    /// Service a synchronous request between evaluation passes, on the document thread: a memo miss
    /// builds and freezes a cascade from the document's font face state. Pending web faces remain
    /// in the returned cascade and retain the host's rendering-triggered loading behavior.
    pub fn refill(&self, cache: &mut FontResolutionCache, request: FontRequest) {
        cache.prepare(request.ffi.font_environment_generation);
        let ffi = unsafe {
            (self.resolve)(
                self.font_faces.memo.as_ptr(),
                self.font_faces.snapshot.as_ptr(),
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
        _memo: *const c_void,
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
            font_feature_values: [crate::css::style::bridge::FfiHostHandle::from_pointer(std::ptr::null());
                FONT_RESOLUTION_FEATURE_INPUT_COUNT],
            font_size_raw: 1024,
            font_slope: 0,
            font_weight: 400.0,
            font_width: 100.0,
            font_optical_sizing: 0,
            font_feature_values_scope: 0,
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
        assert_eq!(font_cascade_list_unref_count(), unrefs_before);
        host.refill(&mut resolver, FontRequest::new(request));
        resolver.lookup(request).unwrap();
        assert_eq!(RESOLVES.load(Ordering::Relaxed), 2);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before);

        // The host's memo owns the cascades, so neither a new generation nor dropping the cache
        // gives up a reference.
        drop(resolver);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before);
    }
}
