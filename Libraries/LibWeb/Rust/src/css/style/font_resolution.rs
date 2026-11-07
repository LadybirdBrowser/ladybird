/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::HashMap;
use super::bridge::{
    FONT_RESOLUTION_FEATURE_INPUT_COUNT, FfiFontResolutionRequest, FfiHostHandle, FfiResolvedFont, FontSizeAdjustKind,
};
use super::tree::TreeScopeID;
use crate::css::host_shared::HostShared;
use crate::css::style_value::{RetainedStyleValueData, retain_style_value};
use crate::fork::ForkShared;
use smallvec::SmallVec;
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
    fn web_css_resolve_font_for_fork(
        memo: *const c_void,
        snapshot: *const c_void,
        request: FfiFontResolutionRequest,
    ) -> FfiResolvedFont;
    fn web_css_font_face_snapshot_reference(snapshot: *const c_void);
    fn web_css_font_face_snapshot_unreference(snapshot: *const c_void);
    fn web_css_font_cascade_memo_reference(memo: *const c_void);
    fn web_css_font_cascade_memo_unreference(memo: *const c_void);
}

/// The scalars of a request, as bits: a request answers another exactly where both name the same
/// values and these are equal.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct FontRequestScalars {
    font_size_raw: i32,
    font_weight: u64,
    font_width: u64,
    font_size_adjust_value: u64,
    font_feature_values_scope: u32,
    // NB: An f32's bits, which keep the request within its size.
    font_oblique_angle: u32,
    font_optical_sizing: u8,
    /// The slope in the low two bits, and the synthesis bits above them, which keeps the request within its size.
    font_slope_and_synthesis: u8,
    font_size_adjust: FontSizeAdjustKind,
    font_size_adjust_metric: u8,
}

impl FontRequestScalars {
    fn of(request: &FfiFontResolutionRequest) -> Self {
        Self {
            font_size_raw: request.font_size_raw,
            font_weight: request.font_weight.to_bits(),
            font_width: request.font_width.to_bits(),
            font_size_adjust_value: request.font_size_adjust_value.to_bits(),
            font_oblique_angle: (request.font_oblique_angle as f32).to_bits(),
            font_feature_values_scope: request.font_feature_values_scope,
            font_optical_sizing: request.font_optical_sizing,
            font_slope_and_synthesis: (request.font_slope as u8 & 3) | (request.font_synthesis << 2),
            font_size_adjust: request.font_size_adjust,
            font_size_adjust_metric: request.font_size_adjust_metric,
        }
    }
}

/// A hash of the identities of the values `request` names and of its scalars, which the cache
/// finds its resolutions by.
fn request_identity_hash(request: &FfiFontResolutionRequest) -> u64 {
    use std::hash::{Hash, Hasher};
    let mut hasher = crate::fast_hash::fast_hasher();
    request.font_family.address.hash(&mut hasher);
    for value in request.font_feature_values {
        value.address.hash(&mut hasher);
    }
    FontRequestScalars::of(request).hash(&mut hasher);
    hasher.finish()
}

/// The value a request names by `handle`, or none for a null one.
fn requested_value(handle: FfiHostHandle) -> Option<&'static crate::css::style_value::StyleValueData> {
    // SAFETY: A request names live style values for as long as it is asked about.
    (!handle.as_pointer().is_null())
        .then(|| unsafe { &*handle.as_pointer().cast::<crate::css::style_value::StyleValueData>() })
}

/// A hash of the content of the values a request names.
fn request_content_hash(request: &FfiFontResolutionRequest) -> u64 {
    let mut hash =
        requested_value(request.font_family).map_or(0, crate::css::style_value::StyleValueData::content_hash);
    for value in request.font_feature_values {
        hash = hash.rotate_left(7)
            ^ requested_value(value).map_or(0, crate::css::style_value::StyleValueData::content_hash);
    }
    hash
}

/// A request for a font, which holds the values it names alive for their identities, with its
/// scalars and the generation of the font environment it was made for.
#[derive(Clone)]
pub(super) struct FontRequest {
    family: RetainedStyleValueData,
    /// The feature values, each empty where the property has its initial value, boxed only where
    /// any is set: most requests name none.
    feature_values: Option<Box<[RetainedStyleValueData; FONT_RESOLUTION_FEATURE_INPUT_COUNT]>>,
    scalars: FontRequestScalars,
    generation: u64,
}

const _: () = assert!(size_of::<FontRequest>() <= 64);

impl FontRequest {
    pub fn new(ffi: FfiFontResolutionRequest) -> Self {
        let retain = |handle: FfiHostHandle| {
            if handle.as_pointer().is_null() {
                return RetainedStyleValueData::none();
            }
            // SAFETY: A request names live style values, a reference to which this takes.
            unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(handle.as_pointer().cast())) }
        };
        let names_feature_values = ffi
            .font_feature_values
            .iter()
            .any(|value| !value.as_pointer().is_null());
        Self {
            family: retain(ffi.font_family),
            feature_values: names_feature_values.then(|| Box::new(ffi.font_feature_values.map(retain))),
            scalars: FontRequestScalars::of(&ffi),
            generation: ffi.font_environment_generation,
        }
    }

    /// The request, made for the font environment of `generation`.
    pub fn for_generation(mut self, generation: u64) -> Self {
        self.generation = generation;
        self
    }

    /// The generation of the font environment the request was made for.
    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// The request as the host resolves it, naming the values this holds.
    fn ffi(&self) -> FfiFontResolutionRequest {
        let handle = |value: &RetainedStyleValueData| FfiHostHandle::from_pointer(value.pointer().cast());
        let scalars = self.scalars;
        FfiFontResolutionRequest {
            font_family: handle(&self.family),
            font_feature_values: match &self.feature_values {
                Some(values) => std::array::from_fn(|index| handle(&values[index])),
                None => [FfiHostHandle::from_pointer(std::ptr::null()); FONT_RESOLUTION_FEATURE_INPUT_COUNT],
            },
            font_size_raw: scalars.font_size_raw,
            font_slope: i32::from(scalars.font_slope_and_synthesis & 3),
            font_weight: f64::from_bits(scalars.font_weight),
            font_width: f64::from_bits(scalars.font_width),
            font_optical_sizing: scalars.font_optical_sizing,
            font_oblique_angle: f64::from(f32::from_bits(scalars.font_oblique_angle)),
            font_synthesis: scalars.font_slope_and_synthesis >> 2,
            font_size_adjust: scalars.font_size_adjust,
            font_size_adjust_metric: scalars.font_size_adjust_metric,
            font_size_adjust_value: f64::from_bits(scalars.font_size_adjust_value),
            font_feature_values_scope: scalars.font_feature_values_scope,
            font_environment_generation: self.generation,
        }
    }

    /// The feature value at `index`, or none where the request names none.
    fn feature_value(&self, index: usize) -> Option<&crate::css::style_value::StyleValueData> {
        self.feature_values.as_ref()?[index].optional_data()
    }

    /// Whether the request names the values `request` names, by identity, and has its scalars:
    /// it is the same request, whatever generation either was made for.
    fn is(&self, request: &FfiFontResolutionRequest) -> bool {
        self.scalars == FontRequestScalars::of(request)
            && self.family.pointer() as usize == request.font_family.address
            && (0..FONT_RESOLUTION_FEATURE_INPUT_COUNT).all(|index| {
                self.feature_values
                    .as_ref()
                    .map_or(0, |values| values[index].pointer() as usize)
                    == request.font_feature_values[index].address
            })
    }

    /// Whether the request names values equal to those `request` names, by whatever identities,
    /// and has its scalars.
    fn names_values_equal_to(&self, request: &FfiFontResolutionRequest) -> bool {
        self.scalars == FontRequestScalars::of(request)
            && self.family.optional_data() == requested_value(request.font_family)
            && (0..FONT_RESOLUTION_FEATURE_INPUT_COUNT)
                .all(|index| self.feature_value(index) == requested_value(request.font_feature_values[index]))
    }
}

/// One resolution: the request it answers and what the host resolved it to.
#[derive(Clone)]
struct FontResolution {
    request: FontRequest,
    ffi: FfiResolvedFont,
}

/// A reference to a cascade, which a cache only holds.
#[derive(Clone)]
struct HeldFontCascadeList(#[allow(dead_code)] libgfx_rust::font::FontCascadeListHandle);

// SAFETY: A `Gfx::FontCascadeList` counts its references atomically, and the holder never reads it: the reference only
// keeps it alive, and is taken and given up on any thread.
unsafe impl Send for HeldFontCascadeList {}
unsafe impl Sync for HeldFontCascadeList {}

/// The resolutions of one generation, with the indices a lookup finds them by.
#[derive(Clone, Default)]
struct FontResolutions {
    /// Each holding its request once.
    resolutions: Vec<FontResolution>,
    /// The resolutions by the hash of their requests' identities.
    by_identity: HashMap<u64, SmallVec<[u32; 1]>>,
    /// The resolutions by the content of the values their requests name, for a request naming
    /// equal values by other identities.
    by_content: HashMap<u64, SmallVec<[u32; 1]>>,
}

impl FontResolutions {
    fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, small_vec_bytes, vec_bytes};
        vec_bytes(&self.resolutions)
            + self
                .resolutions
                .iter()
                .filter(|resolution| resolution.request.feature_values.is_some())
                .count()
                * size_of::<[RetainedStyleValueData; FONT_RESOLUTION_FEATURE_INPUT_COUNT]>()
            + map_bytes(&self.by_identity)
            + self.by_identity.values().map(small_vec_bytes).sum::<usize>()
            + map_bytes(&self.by_content)
            + self.by_content.values().map(small_vec_bytes).sum::<usize>()
    }

    /// The resolution of `request`, made for its very values.
    fn find(&self, request: &FfiFontResolutionRequest) -> Option<&FontResolution> {
        self.by_identity
            .get(&request_identity_hash(request))?
            .iter()
            .map(|&index| &self.resolutions[index as usize])
            .find(|resolution| resolution.request.is(request))
    }
}

/// The resolutions this document has already been given, keyed by content and scoped to one
/// font-environment generation. This is retained engine state: an evaluation step reads it and
/// never calls the host.
///
/// It names the cascades it was given without holding a reference to any of them. The host's
/// cascade memo keeps every cascade it resolves alive until it publishes the font faces of a later
/// generation, which empties the cache, and a lookup answers only for the generation the cache was
/// filled at. So the engine never gives up the last reference to a `Gfx::FontCascadeList`, whose
/// destructor releases fonts into host caches, and nothing in the cache stops it from moving to
/// another thread.
///
/// A fork of the render state shares the resolutions until either side writes, and sees no
/// publication, so its cache holds a reference of its own to each cascade they name instead. See
/// [`Self::hold_cascades_for_fork`].
#[derive(Clone, Default)]
pub(super) struct FontResolutionCache {
    generation: Option<u64>,
    resolutions: ForkShared<FontResolutions>,
    /// Whether a lookup finds a resolution made for equal values by other identities, which a
    /// style pass needs: it computes the values its requests name anew each time.
    matches_by_content: bool,
    /// The references a fork's cache holds to the cascades its resolutions name, and none in the
    /// cache of the render state the host writes.
    held_cascades: Option<Vec<HeldFontCascadeList>>,
    /// The requests the earlier generations resolved, which the next one is likely to be asked
    /// for again.
    carried: Vec<FontRequest>,
    /// The shadow tree scopes whose own `@font-feature-values` the published table carries.
    feature_values_shadow_scopes: Box<[TreeScopeID]>,
}

impl FontResolutionCache {
    /// The bytes the cache holds on the heap, but for the values its requests name, which their
    /// declarations and records hold too.
    pub(super) fn heap_size(&self) -> usize {
        use crate::heap_size::vec_bytes;
        self.resolutions.heap_size()
            + self.held_cascades.as_ref().map_or(0, vec_bytes)
            + vec_bytes(&self.carried)
            + size_of_val::<[TreeScopeID]>(&self.feature_values_shadow_scopes)
    }

    pub fn matches_by_content(&self) -> bool {
        self.matches_by_content
    }

    pub fn set_matches_by_content(&mut self, matches_by_content: bool) {
        self.matches_by_content = matches_by_content;
    }

    pub fn publish_feature_values_shadow_scopes(&mut self, scopes: Box<[TreeScopeID]>) {
        self.feature_values_shadow_scopes = scopes;
    }

    pub fn feature_values_shadow_scopes(&self) -> &[TreeScopeID] {
        &self.feature_values_shadow_scopes
    }

    /// Has the cache of a fork of the render state hold a reference to each cascade it names, the shared resolutions'
    /// and those it resolves from here on: the host frees the cascades of a generation once it publishes the next,
    /// which the fork never sees. The fork resolves what the state it forked resolved as that state did, at the
    /// generation of its inputs, whatever the host's font environment moved to since.
    pub fn hold_cascades_for_fork(&mut self) {
        if self.held_cascades.is_some() {
            return;
        }
        let held = self
            .resolutions
            .resolutions
            .iter()
            .map(|resolution| {
                // SAFETY: The host keeps the cascades of the generation the cache was filled at alive until it
                //         publishes the font faces of the next, which empties the cache first.
                HeldFontCascadeList(unsafe {
                    libgfx_rust::font::FontCascadeListHandle::retain(resolution.ffi.font_cascade_list.as_pointer())
                })
            })
            .collect();
        self.held_cascades = Some(held);
    }

    /// Forget every resolution, as the host published the font faces of a later generation: it frees the cascades of
    /// the earlier ones from here on.
    pub fn forget_earlier_generations(&mut self) {
        self.generation = None;
        self.carry_requests();
    }

    pub fn prepare(&mut self, generation: u64) {
        if self.generation != Some(generation) {
            self.carry_requests();
            self.generation = Some(generation);
        }
    }

    /// Forget every resolution, keeping the requests they answered.
    fn carry_requests(&mut self) {
        let resolutions = self.resolutions.take();
        self.carried
            .extend(resolutions.resolutions.into_iter().map(|resolution| resolution.request));
        if let Some(held) = &mut self.held_cascades {
            held.clear();
        }
    }

    /// Take the requests the earlier generations resolved, which no resolution of this one
    /// answers yet.
    pub fn take_carried_requests(&mut self) -> Vec<FontRequest> {
        // NB: A request carried over several generations is carried once for each, the latest last.
        let mut taken: HashMap<u64, SmallVec<[usize; 1]>> = HashMap::default();
        let mut requests: Vec<FontRequest> = Vec::new();
        for request in std::mem::take(&mut self.carried).into_iter().rev() {
            let ffi = request.ffi();
            if self.resolutions.find(&ffi).is_some() {
                continue;
            }
            let same = taken.entry(request_identity_hash(&ffi)).or_default();
            if same.iter().any(|&index| requests[index].is(&ffi)) {
                continue;
            }
            same.push(requests.len());
            requests.push(request);
        }
        requests.reverse();
        requests
    }

    pub fn lookup(&self, request: FfiFontResolutionRequest) -> Option<FfiResolvedFont> {
        if self.generation != Some(request.font_environment_generation) {
            return None;
        }
        if let Some(resolution) = self.resolutions.find(&request) {
            return Some(resolution.ffi);
        }
        if !self.matches_by_content {
            return None;
        }
        // A family or feature value equal to one a resolution was made for resolves the same.
        let candidates = self.resolutions.by_content.get(&request_content_hash(&request))?;
        candidates.iter().find_map(|&index| {
            let resolution = &self.resolutions.resolutions[index as usize];
            resolution
                .request
                .names_values_equal_to(&request)
                .then_some(resolution.ffi)
        })
    }

    /// Takes in what the threads of a pass resolved. Nothing waits on the requests carried from the generations before
    /// any more: a pass resolves what it asks for. Answers how many fonts they resolved.
    pub(super) fn take_in_pass_fonts(&mut self, pass_fonts: PassFonts) -> usize {
        let mut resolved = pass_fonts.into_resolved();
        let Some(generation) = resolved.generation else {
            return 0;
        };
        self.prepare(generation);
        self.carried.clear();
        if let (Some(held), Some(resolved_held)) = (&mut self.held_cascades, resolved.held_cascades) {
            held.extend(resolved_held);
        }
        let resolutions = resolved.resolutions.take().resolutions;
        let count = resolutions.len();
        for resolution in resolutions {
            self.insert(resolution.request, resolution.ffi, None);
        }
        count
    }

    fn insert(&mut self, request: FontRequest, ffi: FfiResolvedFont, font_cascade_list: Option<HeldFontCascadeList>) {
        // The host resolves every request, to the fallback font where nothing else matches, so the
        // resolution always names a list.
        debug_assert!(!ffi.font_cascade_list.is_none());
        if let (Some(held), Some(font_cascade_list)) = (&mut self.held_cascades, font_cascade_list) {
            held.push(font_cascade_list);
        }
        let request_ffi = request.ffi();
        let identity_hash = request_identity_hash(&request_ffi);
        let resolutions = &mut *self.resolutions;
        let resolution = FontResolution { request, ffi };
        let same_request = resolutions.by_identity.get(&identity_hash).and_then(|indices| {
            indices
                .iter()
                .copied()
                .find(|&index| resolutions.resolutions[index as usize].request.is(&request_ffi))
        });
        if let Some(index) = same_request {
            resolutions.resolutions[index as usize] = resolution;
            return;
        }
        let index = u32::try_from(resolutions.resolutions.len()).expect("a document resolves fewer than 2^32 fonts");
        resolutions.resolutions.push(resolution);
        resolutions.by_identity.entry(identity_hash).or_default().push(index);
        resolutions
            .by_content
            .entry(request_content_hash(&request_ffi))
            .or_default()
            .push(index);
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
// memo's lock does not cover, so only `FontResolverHost::refill` resolves, on one thread at a time:
// the one running the rounds of a pass, or a thread of a pass holding the lock of `PassFonts`.
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

/// The host's synchronous font resolver and the table it resolves against. This is host state, which
/// one thread at a time may call.
#[derive(Clone)]
pub(super) struct FontResolverHost {
    resolve: ResolveFontCallback,
    /// Resolves for a fork's cache, handing over a reference to the cascade.
    resolve_for_fork: ResolveFontCallback,
    font_faces: PublishedFontFaces,
}

impl FontResolverHost {
    pub(super) fn new(font_faces: PublishedFontFaces) -> Self {
        Self::with_callbacks(web_css_resolve_font, web_css_resolve_font_for_fork, font_faces)
    }

    pub(super) fn with_callbacks(
        resolve: ResolveFontCallback,
        resolve_for_fork: ResolveFontCallback,
        font_faces: PublishedFontFaces,
    ) -> Self {
        Self {
            resolve,
            resolve_for_fork,
            font_faces,
        }
    }

    /// The table the document published last, which every later resolution reads.
    pub(super) fn publish(&mut self, font_faces: PublishedFontFaces) {
        self.font_faces = font_faces;
    }

    /// Service a synchronous request: a memo miss
    /// builds and freezes a cascade from the document's font face state. Pending web faces remain
    /// in the returned cascade and retain the host's rendering-triggered loading behavior.
    pub fn refill(&self, cache: &mut FontResolutionCache, request: FontRequest) {
        cache.prepare(request.generation);
        let resolve = if cache.held_cascades.is_some() {
            self.resolve_for_fork
        } else {
            self.resolve
        };
        let ffi = unsafe {
            resolve(
                self.font_faces.memo.as_ptr(),
                self.font_faces.snapshot.as_ptr(),
                request.ffi(),
            )
        };
        // SAFETY: A fork's resolution hands over a reference to the cascade it names.
        let font_cascade_list = cache.held_cascades.is_some().then(|| {
            HeldFontCascadeList(unsafe {
                libgfx_rust::font::FontCascadeListHandle::adopt(ffi.font_cascade_list.as_pointer())
            })
        });
        cache.insert(request, ffi, font_cascade_list);
    }
}

/// The fonts the threads of a style pass resolve as they first ask for them, one thread at a time under the lock, the
/// others going on with what they have. What they resolve joins the cache as the pass ends.
pub(super) struct PassFonts {
    resolver: FontResolverHost,
    resolved: std::sync::RwLock<FontResolutionCache>,
}

impl PassFonts {
    /// What a pass reading `cache` resolves through `resolver`.
    pub(super) fn new(resolver: &FontResolverHost, cache: &FontResolutionCache) -> Self {
        Self {
            resolver: resolver.clone(),
            resolved: std::sync::RwLock::new(FontResolutionCache {
                matches_by_content: cache.matches_by_content,
                held_cascades: cache.held_cascades.as_ref().map(|_| Vec::new()),
                ..Default::default()
            }),
        }
    }

    /// The resolution of `request`, resolved now where no thread of the pass did yet.
    pub(super) fn resolve(&self, request: FfiFontResolutionRequest) -> FfiResolvedFont {
        let lock_failed = "no thread panicked while resolving a font";
        if let Some(resolved) = self.resolved.read().expect(lock_failed).lookup(request) {
            return resolved;
        }
        let mut resolved = self.resolved.write().expect(lock_failed);
        if let Some(resolution) = resolved.lookup(request) {
            return resolution;
        }
        self.resolver.refill(&mut resolved, FontRequest::new(request));
        resolved.lookup(request).expect("a resolved request has its resolution")
    }

    /// What the pass resolved.
    fn into_resolved(self) -> FontResolutionCache {
        self.resolved
            .into_inner()
            .expect("no thread panicked while resolving a font")
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
        let host = FontResolverHost::with_callbacks(resolve_font, resolve_font, unsafe {
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
            font_oblique_angle: 14.0,
            font_synthesis: 0,
            font_size_adjust: FontSizeAdjustKind::None,
            font_size_adjust_metric: 0,
            font_size_adjust_value: 0.0,
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

    unsafe extern "C" fn resolve_font_uncounted(
        _memo: *const c_void,
        _snapshot: *const c_void,
        _request: FfiFontResolutionRequest,
    ) -> FfiResolvedFont {
        FfiResolvedFont {
            first_available_font: crate::css::style::bridge::FfiHostHandle::from_pointer(std::ptr::dangling()),
            font_cascade_list: crate::css::style::bridge::FfiHostHandle::from_pointer(std::ptr::dangling()),
            ..Default::default()
        }
    }

    #[test]
    fn a_forks_cache_answers_what_it_copied_and_holds_the_cascades() {
        let unrefs_before = font_cascade_list_unref_count();
        let family = RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: 2 });
        let host = FontResolverHost::with_callbacks(resolve_font_uncounted, resolve_font_uncounted, unsafe {
            PublishedFontFaces::adopt(std::ptr::dangling(), std::ptr::dangling())
        });
        let mut resolver = FontResolutionCache::default();
        let request = FfiFontResolutionRequest {
            font_family: crate::css::style::bridge::FfiHostHandle::from_pointer(family.pointer().cast()),
            font_feature_values: [crate::css::style::bridge::FfiHostHandle::from_pointer(std::ptr::null());
                FONT_RESOLUTION_FEATURE_INPUT_COUNT],
            font_size_raw: 2048,
            font_slope: 0,
            font_weight: 700.0,
            font_width: 100.0,
            font_optical_sizing: 0,
            font_oblique_angle: 14.0,
            font_synthesis: 0,
            font_size_adjust: FontSizeAdjustKind::None,
            font_size_adjust_metric: 0,
            font_size_adjust_value: 0.0,
            font_feature_values_scope: 0,
            font_environment_generation: 1,
        };
        host.refill(&mut resolver, FontRequest::new(request));
        assert!(resolver.lookup(request).is_some());

        // The fork answers what the cache it copied answered, holding a reference to each cascade, which the host frees
        // once it publishes another generation.
        let mut fork = resolver.clone();
        fork.hold_cascades_for_fork();
        assert!(fork.lookup(request).is_some());
        assert_eq!(font_cascade_list_unref_count(), unrefs_before);
        drop(fork);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before + 1);
        drop(resolver);
        assert_eq!(font_cascade_list_unref_count(), unrefs_before + 1);
    }
}
