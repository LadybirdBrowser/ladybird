/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

unsafe extern "C" {
    fn ladybird_gfx_font_snapshot(font: *const c_void, out_snapshot: *mut FfiFontSnapshot);
    fn ladybird_gfx_font_glyph_id(font: *const c_void, code_point: u32) -> u32;
    fn ladybird_gfx_font_contains_glyph(font: *const c_void, code_point: u32) -> bool;
    fn ladybird_gfx_font_is_emoji_font(font: *const c_void) -> bool;
    fn ladybird_gfx_font_measure_text_width(
        font: *const c_void,
        text_utf16: *const u16,
        length_in_code_units: usize,
    ) -> f32;
    fn ladybird_gfx_font_cascade_list_font_for_code_point(
        list: *const c_void,
        code_point: u32,
        emoji_presentation: bool,
        forced_presentation: bool,
    ) -> *const c_void;
    fn ladybird_gfx_font_ref(font: *const c_void);
    fn ladybird_gfx_font_unref(font: *const c_void);
    fn ladybird_gfx_font_cascade_list_ref(list: *const c_void);
    fn ladybird_gfx_font_cascade_list_unref(list: *const c_void);
    fn ladybird_gfx_font_cascade_list_equals(list: *const c_void, other: *const c_void) -> bool;
    fn ladybird_gfx_emoji_presentation_for_code_point(
        code_point: u32,
        next_code_point: u32,
        has_next_code_point: bool,
    ) -> u8;
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfiFontSnapshot {
    pub id: u64,
    pub ascent: f32,
    pub descent: f32,
    pub x_height: f32,
    pub zero_advance: f32,
    pub pixel_size: f32,
    pub point_size: f32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct FontId(pub u64);

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FontFacts {
    pub ascent: f32,
    pub descent: f32,
    pub x_height: f32,
    pub zero_advance: f32,
    pub pixel_size: f32,
    pub point_size: f32,
}

struct FontEntry {
    id: FontId,
    raw: NonNull<c_void>,
    facts: FontFacts,
}

// SAFETY: Gfx::Font is atomically reference counted, and the retained reference this entry owns
// keeps it live. Everything the entry answers with is fixed at construction: the snapshot it
// copied, and the typeface tables the glyph queries read. The font's lazily filled members are
// each internally synchronized - the HarfBuzz font behind `call_once`, the emoji verdict and the
// hinting memo behind an atomic word. NB: none of this holds for `Gfx::FontCascadeList`,
// which writes several unsynchronized caches from its `const` lookups; `FontCascadeListHandle`
// below is deliberately neither `Send` nor `Sync`.
unsafe impl Send for FontEntry {}
// SAFETY: See the Send implementation above.
unsafe impl Sync for FontEntry {}

impl Drop for FontEntry {
    fn drop(&mut self) {
        // SAFETY: FontHandle::intern took the reference this releases.
        unsafe { ladybird_gfx_font_unref(self.raw.as_ptr()) };
    }
}

#[derive(Clone)]
pub struct FontHandle(Arc<FontEntry>);

impl FontHandle {
    /// # Safety
    ///
    /// `raw` must point to a live `Gfx::Font`.
    pub unsafe fn intern(raw: *const c_void) -> Self {
        let raw = NonNull::new(raw.cast_mut()).expect("Gfx::Font pointer must not be null");
        let mut snapshot = FfiFontSnapshot::default();
        // SAFETY: The caller guarantees the font is live, and the out-pointer
        // addresses a local.
        unsafe { ladybird_gfx_font_snapshot(raw.as_ptr(), &raw mut snapshot) };
        // SAFETY: The caller guarantees the font is live; the reference taken
        // here keeps it that way until the entry drops.
        unsafe { ladybird_gfx_font_ref(raw.as_ptr()) };
        Self(Arc::new(FontEntry {
            id: FontId(snapshot.id),
            raw,
            facts: FontFacts {
                ascent: snapshot.ascent,
                descent: snapshot.descent,
                x_height: snapshot.x_height,
                zero_advance: snapshot.zero_advance,
                pixel_size: snapshot.pixel_size,
                point_size: snapshot.point_size,
            },
        }))
    }

    /// # Safety
    ///
    /// `raw` must point to a live `Gfx::Font` carrying one reference the caller gives up to the
    /// handle.
    pub unsafe fn adopt(raw: *const c_void) -> Self {
        // SAFETY: Guaranteed by the caller; interning takes a reference of its own.
        let handle = unsafe { Self::intern(raw) };
        // SAFETY: The reference the caller gave up, which the handle no longer needs.
        unsafe { ladybird_gfx_font_unref(raw) };
        handle
    }

    #[inline]
    pub fn id(&self) -> FontId {
        self.0.id
    }

    #[inline]
    pub fn facts(&self) -> &FontFacts {
        &self.0.facts
    }

    #[inline]
    pub fn as_raw(&self) -> *const c_void {
        self.0.raw.as_ptr()
    }

    pub fn is_emoji_font(&self) -> bool {
        // SAFETY: The entry's retained reference keeps the font live.
        unsafe { ladybird_gfx_font_is_emoji_font(self.as_raw()) }
    }

    pub fn glyph_width(&self, code_point: u32) -> f32 {
        let mut utf16 = [0u16; 2];
        let encoded = char::from_u32(code_point)
            .unwrap_or(char::REPLACEMENT_CHARACTER)
            .encode_utf16(&mut utf16);
        crate::text_layout::shaped_text_width(self, encoded, crate::text_layout::TextType::Common, 0.0, 0.0)
    }

    pub fn glyph_id_for_code_point(&self, code_point: u32) -> u32 {
        // SAFETY: The entry's retained reference keeps the font live.
        unsafe { ladybird_gfx_font_glyph_id(self.as_raw(), code_point) }
    }

    pub fn contains_glyph(&self, code_point: u32) -> bool {
        // SAFETY: The entry's retained reference keeps the font live.
        unsafe { ladybird_gfx_font_contains_glyph(self.as_raw(), code_point) }
    }

    pub fn measure_text_width(&self, text: &[u16]) -> f32 {
        // SAFETY: The entry's retained reference keeps the font live, and the
        // text slice stays valid for the synchronous measuring call.
        unsafe { ladybird_gfx_font_measure_text_width(self.as_raw(), text.as_ptr(), text.len()) }
    }
}

impl PartialEq for FontHandle {
    fn eq(&self, other: &Self) -> bool {
        self.0.id == other.0.id
    }
}

impl Eq for FontHandle {}

impl std::hash::Hash for FontHandle {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.0.id.hash(state);
    }
}

impl std::fmt::Debug for FontHandle {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_tuple("FontHandle").field(&self.0.id).finish()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EmojiPresentation {
    pub is_emoji: bool,
    pub forced: bool,
}

pub fn emoji_presentation_for_code_point(code_point: u32, next_code_point: Option<u32>) -> EmojiPresentation {
    // SAFETY: The lookup reads only immutable Unicode tables.
    let encoded = unsafe {
        ladybird_gfx_emoji_presentation_for_code_point(
            code_point,
            next_code_point.unwrap_or(0),
            next_code_point.is_some(),
        )
    };
    EmojiPresentation {
        is_emoji: encoded & 1 != 0,
        forced: encoded & 2 != 0,
    }
}

#[repr(C)]
pub struct FontCascadeListHandle {
    pointer: *const c_void,
}

impl FontCascadeListHandle {
    pub const fn null() -> Self {
        Self {
            pointer: std::ptr::null(),
        }
    }

    /// # Safety
    ///
    /// `list` must point to a live `Gfx::FontCascadeList`.
    #[inline]
    pub unsafe fn retain(list: *const c_void) -> Self {
        assert!(!list.is_null(), "Gfx::FontCascadeList pointer must not be null");
        // SAFETY: The caller guarantees the list is live; the reference taken
        // here keeps it that way until drop.
        unsafe { ladybird_gfx_font_cascade_list_ref(list) };
        Self { pointer: list }
    }

    /// # Safety
    ///
    /// `list` must point to a live `Gfx::FontCascadeList` carrying one
    /// reference the caller gives up to this handle.
    #[inline]
    pub unsafe fn adopt(list: *const c_void) -> Self {
        assert!(!list.is_null(), "Gfx::FontCascadeList pointer must not be null");
        Self { pointer: list }
    }

    #[inline]
    pub fn is_null(&self) -> bool {
        self.pointer.is_null()
    }

    #[inline]
    pub fn as_raw(&self) -> *const c_void {
        self.pointer
    }

    /// Whether both cascades resolve every code point to the same fonts, comparing their frozen selections when
    /// available. Pending selections retain the face and display state they were published with.
    /// This is not `==`, which compares the lists' identities.
    pub fn resolves_like(&self, other: &Self) -> bool {
        assert!(
            !self.pointer.is_null() && !other.pointer.is_null(),
            "Gfx::FontCascadeList pointer must not be null"
        );
        // SAFETY: Both handles keep their lists live.
        unsafe { ladybird_gfx_font_cascade_list_equals(self.pointer, other.pointer) }
    }

    pub fn font_for_code_point(
        &self,
        code_point: u32,
        presentation: EmojiPresentation,
        font_hint: Option<&FontHandle>,
    ) -> FontHandle {
        assert!(!self.pointer.is_null(), "Gfx::FontCascadeList pointer must not be null");
        // SAFETY: This handle keeps the list live, and the list owns every
        // font it resolves.
        let raw = unsafe {
            ladybird_gfx_font_cascade_list_font_for_code_point(
                self.pointer,
                code_point,
                presentation.is_emoji,
                presentation.forced,
            )
        };
        if let Some(font_hint) = font_hint
            && font_hint.as_raw() == raw
        {
            return font_hint.clone();
        }
        // SAFETY: The list keeps the resolved font live for the call.
        unsafe { FontHandle::intern(raw) }
    }
}

impl Clone for FontCascadeListHandle {
    #[inline]
    fn clone(&self) -> Self {
        if !self.pointer.is_null() {
            // SAFETY: This handle holds a reference, so the list is live.
            unsafe { ladybird_gfx_font_cascade_list_ref(self.pointer) };
        }
        Self { pointer: self.pointer }
    }
}

impl Drop for FontCascadeListHandle {
    #[inline]
    fn drop(&mut self) {
        if !self.pointer.is_null() {
            // SAFETY: Construction took the reference this releases.
            unsafe { ladybird_gfx_font_cascade_list_unref(self.pointer) };
        }
    }
}

impl PartialEq for FontCascadeListHandle {
    fn eq(&self, other: &Self) -> bool {
        self.pointer == other.pointer
    }
}

impl Eq for FontCascadeListHandle {}

impl std::fmt::Debug for FontCascadeListHandle {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_tuple("FontCascadeListHandle")
            .field(&self.pointer)
            .finish()
    }
}

unsafe extern "C" {
    fn ladybird_gfx_font_cascade_list_frozen(list: *const c_void) -> *const c_void;
    fn ladybird_gfx_resolve_pending_face(face_id: u64) -> bool;
    fn ladybird_gfx_cascade_snapshot_begin(list: *const c_void) -> *const c_void;
    fn ladybird_gfx_cascade_snapshot_header(snapshot: *const c_void, out_header: *mut FfiCascadeSnapshotHeader);
    fn ladybird_gfx_cascade_snapshot_fill(
        snapshot: *const c_void,
        fallback: bool,
        out_entries: *mut FfiCascadeSnapshotEntry,
        out_ranges: *mut FfiCascadeSnapshotRange,
    );
    fn ladybird_gfx_cascade_snapshot_end(snapshot: *const c_void);
    fn ladybird_gfx_system_fallback_font(
        code_point: u32,
        weight: u16,
        width: u16,
        slope: u8,
        prefer_color_emoji: bool,
        point_size: f32,
    ) -> *const c_void;
    fn ladybird_gfx_font_invisible_variant(font: *const c_void) -> *const c_void;

    // The process-wide state this crate is not allowed to hold; see `LibGfx/RustProcessState.cpp`.
    fn ladybird_gfx_process_note_wanted_pending_face(face_id: u64);
    fn ladybird_gfx_process_requeue_wanted_pending_face(face_id: u64);
    fn ladybird_gfx_process_take_wanted_pending_faces(
        context: *mut c_void,
        visit: extern "C" fn(*mut c_void, u64, bool),
    );
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FfiCascadeSnapshotHeader {
    entry_count: usize,
    range_count: usize,
    fallback_entry_count: usize,
    fallback_range_count: usize,
    last_resort_font: *const c_void,
    system_fallback_point_size: f32,
    system_fallback_weight: u16,
    system_fallback_width: u16,
    system_fallback_slope: u8,
    has_system_fallback: bool,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FfiCascadeSnapshotEntry {
    font: *const c_void,
    range_offset: usize,
    range_count: usize,
    pending_face_id: u64,
    pending_state: u8,
    source_face_id: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct FfiCascadeSnapshotRange {
    first_code_point: u32,
    last_code_point: u32,
}

/// Mirrors `Gfx::PendingFontState`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PendingFontState {
    Invisible,
    Visible,
}

impl PendingFontState {
    fn from_raw(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(Self::Invisible),
            1 => Some(Self::Visible),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct UnicodeRange {
    first: u32,
    last: u32,
}

/// One entry of a frozen cascade, in the order the lookup visits it.
struct FrozenEntry {
    /// The font this entry contributes, or `None` for a face still waiting on its load.
    font: Option<FontHandle>,
    /// The entry's `unicode-range` as an index range into [`FrozenFontList::ranges`]. An empty
    /// slice means the entry covers every code point.
    ranges: std::ops::Range<usize>,
    /// The union of `ranges`, so a code point outside it is rejected without a scan.
    enclosing: UnicodeRange,
    /// Set for a face that is still loading: which `font-display` period it is in, and the
    /// number the document knows it by.
    pending: Option<(u64, PendingFontState)>,
    /// The source face shared by independently resolved cascades; zero for an anonymous pending entry.
    source_face_id: u64,
    /// Set once a render pass has wanted this face, so the document requests its load only once
    /// per frozen cascade.
    wanted: std::sync::atomic::AtomicBool,
}

/// How this cascade asks for a font no listed family covers.
#[derive(Clone, Copy, Debug, PartialEq)]
struct SystemFallbackStyle {
    point_size: f32,
    weight: u16,
    width: u16,
    slope: u8,
}

/// A cascade list frozen at the moment the document published it: no caches to fill, no faces to
/// resolve, nothing to ask the document. Everything a render pass needs to pick a font for a code
/// point is here, and everything it decides that the document must act on is a number it leaves
/// behind in [`take_wanted_pending_faces`].
pub struct FrozenFontList {
    entries: Box<[FrozenEntry]>,
    fallback_entries: Box<[FrozenEntry]>,
    ranges: Box<[UnicodeRange]>,
    fallback_ranges: Box<[UnicodeRange]>,
    last_resort: Option<FontHandle>,
    system_fallback: Option<SystemFallbackStyle>,
    /// Every font reachable without asking the system-fallback service, so an ASCII answer can be
    /// remembered as an index rather than as a pointer.
    fonts: Box<[FontHandle]>,
    /// OPTIMIZATION: The list is immutable, so an answer for a code point is the same answer
    ///               forever; a racing write stores the value the other thread computed.
    ascii_cache: [std::sync::atomic::AtomicU32; 128],
    /// https://drafts.csswg.org/css-fonts-4/#invisible-fallback
    /// Built on demand, and only for a cascade that has a face in its block period.
    invisible_variants: std::sync::Mutex<std::collections::HashMap<FontId, FontHandle>>,
}

const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<FrozenFontList>();
};

impl UnicodeRange {
    const EVERYTHING: Self = Self {
        first: 0,
        last: u32::MAX,
    };

    fn contains(self, code_point: u32) -> bool {
        code_point >= self.first && code_point <= self.last
    }
}

impl FrozenEntry {
    fn covers(&self, code_point: u32, ranges: &[UnicodeRange]) -> bool {
        if self.ranges.is_empty() {
            return true;
        }
        if !self.enclosing.contains(code_point) {
            return false;
        }
        ranges[self.ranges.clone()]
            .iter()
            .any(|range| range.contains(code_point))
    }
}

/// Drains the faces render passes have wanted since the last call. The document turns each number
/// back into a face and resolves it, which is what starts the fetch and the display-period timer.
///
/// The list itself is LibGfx's C++ side's rather than a `static` here: this crate is compiled into
/// more than one library, and on a linker with two-level namespaces a list one copy pushed to would
/// not be the list the other copy drains. See `LibGfx/RustProcessState.cpp`.
pub fn take_wanted_pending_faces() -> Vec<(u64, bool)> {
    extern "C" fn visit(context: *mut c_void, face_id: u64, has_been_retried: bool) {
        // SAFETY: The context is the vector below, alive for the call.
        unsafe { &mut *context.cast::<Vec<(u64, bool)>>() }.push((face_id, has_been_retried));
    }
    let mut wanted = Vec::new();
    // SAFETY: The context outlives the call, and the callback only appends to it.
    unsafe { ladybird_gfx_process_take_wanted_pending_faces((&raw mut wanted).cast(), visit) };
    wanted
}

impl FrozenFontList {
    pub fn empty() -> Self {
        Self {
            entries: Box::new([]),
            fallback_entries: Box::new([]),
            ranges: Box::new([]),
            fallback_ranges: Box::new([]),
            last_resort: None,
            system_fallback: None,
            fonts: Box::new([]),
            ascii_cache: std::array::from_fn(|_| std::sync::atomic::AtomicU32::new(0)),
            invisible_variants: std::sync::Mutex::new(std::collections::HashMap::new()),
        }
    }

    /// # Safety
    ///
    /// `list` must point to a live, complete `Gfx::FontCascadeList`.
    pub unsafe fn from_cascade_list(list: *const c_void) -> Self {
        // SAFETY: The caller guarantees the list is live for this synchronous snapshot.
        let snapshot = unsafe { ladybird_gfx_cascade_snapshot_begin(list) };
        let mut header = FfiCascadeSnapshotHeader::default();
        // SAFETY: The snapshot is live until `..._end` below, and the out-pointer addresses a local.
        unsafe { ladybird_gfx_cascade_snapshot_header(snapshot, &raw mut header) };

        let mut ffi_entries = vec![FfiCascadeSnapshotEntry::default(); header.entry_count];
        let mut ffi_ranges = vec![FfiCascadeSnapshotRange::default(); header.range_count];
        let mut ffi_fallback_entries = vec![FfiCascadeSnapshotEntry::default(); header.fallback_entry_count];
        let mut ffi_fallback_ranges = vec![FfiCascadeSnapshotRange::default(); header.fallback_range_count];
        // SAFETY: The buffers are exactly the sizes the header asked for.
        unsafe {
            ladybird_gfx_cascade_snapshot_fill(snapshot, false, ffi_entries.as_mut_ptr(), ffi_ranges.as_mut_ptr());
            ladybird_gfx_cascade_snapshot_fill(
                snapshot,
                true,
                ffi_fallback_entries.as_mut_ptr(),
                ffi_fallback_ranges.as_mut_ptr(),
            );
        }
        let last_resort = (!header.last_resort_font.is_null()).then(|| {
            // SAFETY: The list owns its last-resort font for the snapshot's duration.
            unsafe { FontHandle::intern(header.last_resort_font) }
        });
        // SAFETY: Nothing above kept a pointer into the snapshot.
        unsafe { ladybird_gfx_cascade_snapshot_end(snapshot) };

        let convert_ranges = |ffi: &[FfiCascadeSnapshotRange]| -> Box<[UnicodeRange]> {
            ffi.iter()
                .map(|range| UnicodeRange {
                    first: range.first_code_point,
                    last: range.last_code_point,
                })
                .collect()
        };
        let ranges = convert_ranges(&ffi_ranges);
        let fallback_ranges = convert_ranges(&ffi_fallback_ranges);

        let mut fonts = Vec::new();
        let mut convert_entries = |ffi: &[FfiCascadeSnapshotEntry], ranges: &[UnicodeRange]| -> Box<[FrozenEntry]> {
            ffi.iter()
                .map(|entry| {
                    let font = (!entry.font.is_null()).then(|| {
                        // SAFETY: The cascade owns every font the snapshot named.
                        let handle = unsafe { FontHandle::intern(entry.font) };
                        fonts.push(handle.clone());
                        handle
                    });
                    let range_span = entry.range_offset..entry.range_offset + entry.range_count;
                    let enclosing = ranges[range_span.clone()].iter().fold(
                        UnicodeRange {
                            first: u32::MAX,
                            last: 0,
                        },
                        |enclosing, range| UnicodeRange {
                            first: enclosing.first.min(range.first),
                            last: enclosing.last.max(range.last),
                        },
                    );
                    FrozenEntry {
                        font,
                        ranges: range_span,
                        enclosing: if entry.range_count == 0 {
                            UnicodeRange::EVERYTHING
                        } else {
                            enclosing
                        },
                        pending: (entry.pending_face_id != 0)
                            .then(|| {
                                PendingFontState::from_raw(entry.pending_state)
                                    .map(|state| (entry.pending_face_id, state))
                            })
                            .flatten(),
                        source_face_id: entry.source_face_id,
                        wanted: std::sync::atomic::AtomicBool::new(false),
                    }
                })
                .collect()
        };
        let entries = convert_entries(&ffi_entries, &ranges);
        let fallback_entries = convert_entries(&ffi_fallback_entries, &fallback_ranges);
        if let Some(last_resort) = &last_resort {
            fonts.push(last_resort.clone());
        }

        Self {
            entries,
            fallback_entries,
            ranges,
            fallback_ranges,
            last_resort,
            system_fallback: header.has_system_fallback.then_some(SystemFallbackStyle {
                point_size: header.system_fallback_point_size,
                weight: header.system_fallback_weight,
                width: header.system_fallback_width,
                slope: header.system_fallback_slope,
            }),
            fonts: fonts.into_boxed_slice(),
            ascii_cache: std::array::from_fn(|_| std::sync::atomic::AtomicU32::new(0)),
            invisible_variants: std::sync::Mutex::new(std::collections::HashMap::new()),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty() && self.last_resort.is_none()
    }

    /// Compares immutable selections, including pending faces and their font-display periods, rather than the
    /// mutable faces those selections were built from. Load-request identities and lookup caches do not affect
    /// which glyphs a snapshot renders.
    pub fn resolves_like(&self, other: &Self) -> bool {
        let entries_equal = |entries: &[FrozenEntry],
                             ranges: &[UnicodeRange],
                             other_entries: &[FrozenEntry],
                             other_ranges: &[UnicodeRange]| {
            let mut has_earlier_pending_selection = false;
            entries.len() == other_entries.len()
                && entries.iter().zip(other_entries).all(|(entry, other_entry)| {
                    if entry.font != other_entry.font
                        || ranges[entry.ranges.clone()] != other_ranges[other_entry.ranges.clone()]
                    {
                        return false;
                    }
                    if entry.font.is_some() {
                        // A preceding unresolved face can start fallback, which skips resident pending faces
                        // but still considers loaded entries. Without one, both entries render the same font.
                        return !has_earlier_pending_selection
                            || entry.pending.is_some() == other_entry.pending.is_some();
                    }
                    has_earlier_pending_selection |= entry.pending.is_some();
                    match (entry.pending, other_entry.pending) {
                        (None, None) => true,
                        (Some((identity, state)), Some((other_identity, other_state))) => {
                            state == other_state
                                && if entry.source_face_id != 0 || other_entry.source_face_id != 0 {
                                    entry.source_face_id == other_entry.source_face_id
                                } else {
                                    identity == other_identity
                                }
                        }
                        _ => false,
                    }
                })
        };
        std::ptr::eq(self, other)
            || (self.last_resort == other.last_resort
                && self.system_fallback == other.system_fallback
                && entries_equal(&self.entries, &self.ranges, &other.entries, &other.ranges)
                && entries_equal(
                    &self.fallback_entries,
                    &self.fallback_ranges,
                    &other.fallback_entries,
                    &other.fallback_ranges,
                ))
    }

    pub fn has_pending_faces(&self) -> bool {
        self.entries.iter().any(|entry| entry.pending.is_some())
    }

    fn invisible_variant(&self, font: &FontHandle) -> FontHandle {
        let mut variants = self
            .invisible_variants
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        variants
            .entry(font.id())
            .or_insert_with(|| {
                // SAFETY: The handle keeps the visible font live, and the variant arrives with one
                // reference this handle takes over.
                unsafe { FontHandle::adopt(ladybird_gfx_font_invisible_variant(font.as_raw())) }
            })
            .clone()
    }

    fn system_fallback_font(&self, code_point: u32, presentation: EmojiPresentation) -> Option<FontHandle> {
        let style = self.system_fallback?;
        // SAFETY: The service answers with a font it transfers one reference to, or with null.
        let raw = unsafe {
            ladybird_gfx_system_fallback_font(
                code_point,
                style.weight,
                style.width,
                style.slope,
                presentation.is_emoji,
                style.point_size,
            )
        };
        // SAFETY: A non-null answer carries the reference this handle takes over.
        (!raw.is_null()).then(|| unsafe { FontHandle::adopt(raw) })
    }

    /// https://drafts.csswg.org/css-fonts-4/#font-matching-algorithm
    /// Mirrors `Gfx::FontCascadeList::font_for_code_point`, with every step that would have asked
    /// the document replaced by what the document decided before the pass began.
    pub fn font_for_code_point(&self, code_point: u32, presentation: EmojiPresentation) -> FontHandle {
        let use_ascii_cache = code_point < 128 && !presentation.is_emoji && !presentation.forced;
        if use_ascii_cache {
            let cached = self.ascii_cache[code_point as usize].load(std::sync::atomic::Ordering::Relaxed);
            if cached != 0 {
                return self.fonts[cached as usize - 1].clone();
            }
        }

        let presentation_matches = |font: &FontHandle| font.is_emoji_font() == presentation.is_emoji;
        let contains_glyph = |entry: &FrozenEntry, ranges: &[UnicodeRange]| -> bool {
            let Some(font) = &entry.font else { return false };
            if entry.ranges.is_empty() {
                return font.contains_glyph(code_point);
            }
            entry.covers(code_point, ranges) && font.contains_glyph(code_point)
        };

        let mut invisible = false;
        let mut rendering_with_fallback = false;
        let mut author_glyph_match: Option<&FontHandle> = None;
        let mut selected: Option<&FontHandle> = None;

        for entry in &self.entries {
            if let Some((face_id, state)) = entry.pending {
                if rendering_with_fallback || !entry.covers(code_point, &self.ranges) {
                    continue;
                }
                // NB: A local face that loading resolved gives its glyphs straight away.
                if let Some(font) = &entry.font {
                    if !contains_glyph(entry, &self.ranges) {
                        continue;
                    }
                    if !presentation.forced || presentation_matches(font) {
                        selected = Some(font);
                        break;
                    }
                    author_glyph_match = author_glyph_match.or(Some(font));
                    continue;
                }
                self.note_wanted_pending_face(entry, face_id);
                invisible = state == PendingFontState::Invisible;
                // https://drafts.csswg.org/css-fonts-4/#font-display-timeline
                // Doing this must not trigger loads of any of the fallback fonts.
                rendering_with_fallback = true;
                continue;
            }
            if !contains_glyph(entry, &self.ranges) {
                continue;
            }
            let font = entry.font.as_ref().expect("an entry with a glyph has a font");
            if !presentation.forced || presentation_matches(font) {
                selected = Some(font);
                break;
            }
            author_glyph_match = author_glyph_match.or(Some(font));
        }

        let mut fallback_glyph_match: Option<&FontHandle> = None;
        if selected.is_none() {
            for entry in &self.fallback_entries {
                if !contains_glyph(entry, &self.fallback_ranges) {
                    continue;
                }
                let font = entry.font.as_ref().expect("an entry with a glyph has a font");
                if presentation_matches(font) {
                    selected = Some(font);
                    break;
                }
                fallback_glyph_match = fallback_glyph_match.or(Some(font));
            }
        }

        if let Some(font) = selected {
            return self.finish(code_point, use_ascii_cache, invisible, font.clone(), true);
        }

        if let Some(fallback) = self.system_fallback_font(code_point, presentation)
            && (presentation_matches(&fallback) || (author_glyph_match.is_none() && fallback_glyph_match.is_none()))
        {
            return self.finish(code_point, use_ascii_cache, invisible, fallback, false);
        }

        let font = author_glyph_match
            .or(fallback_glyph_match)
            .or(self.last_resort.as_ref())
            .cloned();
        match font {
            Some(font) => self.finish(code_point, use_ascii_cache, invisible, font, true),
            // A cascade with no fonts at all cannot answer; only a list nobody published can be
            // in that state, and the layout stage never receives one.
            None => panic!("frozen font cascade has no font for U+{code_point:04X}"),
        }
    }

    fn note_wanted_pending_face(&self, entry: &FrozenEntry, face_id: u64) {
        if entry.wanted.swap(true, std::sync::atomic::Ordering::Relaxed) {
            return;
        }
        // SAFETY: The list is LibGfx's, and the number is all it takes.
        unsafe { ladybird_gfx_process_note_wanted_pending_face(face_id) };
    }

    fn finish(
        &self,
        code_point: u32,
        use_ascii_cache: bool,
        invisible: bool,
        font: FontHandle,
        is_own_font: bool,
    ) -> FontHandle {
        if invisible {
            // An invisible answer is never cached: it is the rare path, and caching it would make
            // the memo depend on which pending face the walk happened to reach first.
            return self.invisible_variant(&font);
        }
        if use_ascii_cache
            && is_own_font
            && let Some(index) = self.fonts.iter().position(|owned| owned == &font)
        {
            self.ascii_cache[code_point as usize].store(index as u32 + 1, std::sync::atomic::Ordering::Relaxed);
        }
        font
    }
}

impl std::fmt::Debug for FrozenFontList {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("FrozenFontList")
            .field("entries", &self.entries.len())
            .field("fallback_entries", &self.fallback_entries.len())
            .finish()
    }
}

/// An `Arc<FrozenFontList>` the computed font payload owns, erased so the C++ mirror of that
/// payload sees one opaque pointer. Rust names the list through [`FrozenFontListRef::list`].
#[repr(C)]
pub struct FrozenFontListRef {
    pointer: *const c_void,
}

impl FrozenFontListRef {
    pub const fn null() -> Self {
        Self {
            pointer: std::ptr::null(),
        }
    }

    pub fn new(list: Arc<FrozenFontList>) -> Self {
        Self {
            pointer: Arc::into_raw(list).cast(),
        }
    }

    #[inline]
    pub fn is_null(&self) -> bool {
        self.pointer.is_null()
    }

    #[inline]
    pub fn as_raw(&self) -> *const c_void {
        self.pointer
    }

    /// The list this reference owns, or `None` for a payload that never received one.
    #[inline]
    pub fn list(&self) -> Option<&FrozenFontList> {
        // SAFETY: A non-null pointer is one `Arc::into_raw` produced and this reference still owns.
        (!self.pointer.is_null()).then(|| unsafe { &*self.pointer.cast::<FrozenFontList>() })
    }

    /// A counted reference to the list, for a render row that must outlive the payload it came from.
    pub fn to_arc(&self) -> Option<Arc<FrozenFontList>> {
        if self.pointer.is_null() {
            return None;
        }
        // SAFETY: The pointer is one `Arc::into_raw` produced; the clone is balanced by the
        // `ManuallyDrop`, so this reference keeps its own count.
        unsafe {
            let borrowed = std::mem::ManuallyDrop::new(Arc::from_raw(self.pointer.cast::<FrozenFontList>()));
            Some(Arc::clone(&borrowed))
        }
    }
}

impl Clone for FrozenFontListRef {
    fn clone(&self) -> Self {
        match self.to_arc() {
            Some(list) => Self::new(list),
            None => Self::null(),
        }
    }
}

impl Drop for FrozenFontListRef {
    fn drop(&mut self) {
        if self.pointer.is_null() {
            return;
        }
        // SAFETY: Construction took the reference this releases.
        unsafe { drop(Arc::from_raw(self.pointer.cast::<FrozenFontList>())) };
    }
}

impl PartialEq for FrozenFontListRef {
    fn eq(&self, other: &Self) -> bool {
        self.pointer == other.pointer
    }
}

impl Eq for FrozenFontListRef {}

impl std::fmt::Debug for FrozenFontListRef {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_tuple("FrozenFontListRef").field(&self.pointer).finish()
    }
}

/// Freezes a completed cascade list. The returned pointer is an `Arc<FrozenFontList>` the caller
/// owns and gives back to `ladybird_gfx_frozen_font_list_release`.
///
/// # Safety
///
/// `list` must point to a live, complete `Gfx::FontCascadeList`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_frozen_font_list_build(list: *const c_void) -> *const c_void {
    assert!(!list.is_null(), "Gfx::FontCascadeList pointer must not be null");
    // SAFETY: The caller guarantees the list is live for this synchronous snapshot.
    let frozen = Arc::new(unsafe { FrozenFontList::from_cascade_list(list) });
    Arc::into_raw(frozen).cast()
}

/// # Safety
///
/// `frozen` must be a pointer `ladybird_gfx_frozen_font_list_build` returned and nobody released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_frozen_font_list_release(frozen: *const c_void) {
    if frozen.is_null() {
        return;
    }
    // SAFETY: The caller guarantees this is a live pointer from `..._build`.
    unsafe { drop(Arc::from_raw(frozen.cast::<FrozenFontList>())) };
}

/// # Safety
///
/// Both pointers must name live frozen lists returned by `ladybird_gfx_frozen_font_list_build`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_frozen_font_lists_equal(list: *const c_void, other: *const c_void) -> bool {
    assert!(
        !list.is_null() && !other.is_null(),
        "frozen font lists must not be null"
    );
    // SAFETY: Both snapshots are live for this call.
    unsafe { (&*list.cast::<FrozenFontList>()).resolves_like(&*other.cast::<FrozenFontList>()) }
}

/// A counted reference to the frozen snapshot `Gfx::FontCascadeList::freeze()` left on `list`, or
/// a null reference for a cascade that was never published to the render pipeline.
///
/// # Safety
///
/// `list` must point to a live `Gfx::FontCascadeList`.
pub unsafe fn frozen_font_list_of(list: *const c_void) -> FrozenFontListRef {
    // SAFETY: The caller guarantees the list is live for this call.
    let frozen = unsafe { ladybird_gfx_font_cascade_list_frozen(list) };
    if frozen.is_null() {
        return FrozenFontListRef::null();
    }
    // SAFETY: The cascade owns the reference this borrows from; the clone below is the caller's own.
    let borrowed = std::mem::ManuallyDrop::new(unsafe { Arc::from_raw(frozen.cast::<FrozenFontList>()) });
    FrozenFontListRef::new(Arc::clone(&borrowed))
}

/// Requests the load of a face a render pass wanted, and answers whether the face was still around
/// to request. This is the document-thread half of the pending-face handover: a pass records the
/// number, and resolving it here is what starts the fetch, arms the display-period timer and
/// engages the load-event delayer.
#[unsafe(no_mangle)]
pub extern "C" fn ladybird_gfx_request_wanted_pending_face(face_id: u64, has_been_retried: bool) -> bool {
    // SAFETY: The id names a face the document registered.
    if unsafe { ladybird_gfx_resolve_pending_face(face_id) } {
        return true;
    }
    if !has_been_retried {
        // A frozen cascade wants a face once and never again, so a want the document could not
        // act on is lost for good. Keep it for one more drain rather than drop it: the face may
        // only have been out of reach for this one. A want is offered exactly twice, so a face
        // that really is gone cannot make this spin.
        // SAFETY: The list is LibGfx's, and the number is all it takes.
        unsafe { ladybird_gfx_process_requeue_wanted_pending_face(face_id) };
    }
    false
}

/// Requests the loads render passes wanted since the last call, and answers how many faces were
/// still around to request.
#[unsafe(no_mangle)]
pub extern "C" fn ladybird_gfx_request_wanted_pending_faces() -> usize {
    take_wanted_pending_faces()
        .into_iter()
        .filter(|(face_id, has_been_retried)| ladybird_gfx_request_wanted_pending_face(*face_id, *has_been_retried))
        .count()
}

/// Looks a code point up in a frozen cascade. `Gfx::FontCascadeList` keeps this for its unit
/// tests; the pipeline calls [`FrozenFontList::font_for_code_point`] directly.
///
/// # Safety
///
/// `frozen` must be a pointer `ladybird_gfx_frozen_font_list_build` returned and nobody released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_frozen_font_list_font_for_code_point(
    frozen: *const c_void,
    code_point: u32,
    is_emoji: bool,
    forced: bool,
) -> *const c_void {
    assert!(!frozen.is_null(), "FrozenFontList pointer must not be null");
    // SAFETY: The caller guarantees this is a live pointer from `..._build`.
    let list = unsafe { &*frozen.cast::<FrozenFontList>() };
    let font = list.font_for_code_point(code_point, EmojiPresentation { is_emoji, forced });
    // The frozen list owns every font it can answer with, so the borrowed pointer stays live.
    font.as_raw()
}
