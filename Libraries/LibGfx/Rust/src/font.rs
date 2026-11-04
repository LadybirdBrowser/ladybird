/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::borrow::Cow;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

unsafe extern "C" {
    fn ladybird_gfx_font_snapshot(font: *const c_void, out_snapshot: *mut FfiFontSnapshot);
    fn ladybird_gfx_font_glyph_id(font: *const c_void, code_point: u32) -> u32;
    fn ladybird_gfx_font_contains_glyph(font: *const c_void, code_point: u32) -> bool;
    fn ladybird_gfx_font_is_emoji_font(font: *const c_void) -> bool;
    fn ladybird_gfx_font_is_invisible(font: *const c_void) -> bool;
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
    fn ladybird_gfx_font_cascade_list_first(list: *const c_void) -> *const c_void;
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

// NB: Layout threads read every entry while they count references to it. The alignment puts the reference counts in
//     front of the entry on a cache line of their own, so counting does not evict what the readers read.
#[repr(align(128))]
struct FontEntry {
    id: FontId,
    raw: NonNull<c_void>,
    facts: FontFacts,
    // What the font answers about itself that text chunking asks for every character: whether it is an emoji font,
    // and whether it has a glyph for U+0020 SPACE. Each is unknown until first asked.
    is_emoji_font: LazyVerdict,
    contains_space_glyph: LazyVerdict,
}

/// A yes or no answer that never changes, asked for once: zero until then.
#[derive(Default)]
struct LazyVerdict(std::sync::atomic::AtomicU8);

impl LazyVerdict {
    const NO: u8 = 1;
    const YES: u8 = 2;

    fn get(&self, ask: impl FnOnce() -> bool) -> bool {
        use std::sync::atomic::Ordering;
        match self.0.load(Ordering::Relaxed) {
            Self::NO => false,
            Self::YES => true,
            _ => {
                let answer = ask();
                self.0
                    .store(if answer { Self::YES } else { Self::NO }, Ordering::Relaxed);
                answer
            }
        }
    }
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
            is_emoji_font: LazyVerdict::default(),
            contains_space_glyph: LazyVerdict::default(),
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
        self.0.is_emoji_font.get(|| {
            // SAFETY: The entry's retained reference keeps the font live.
            unsafe { ladybird_gfx_font_is_emoji_font(self.as_raw()) }
        })
    }

    /// Whether the font stands in for one that is still loading, whose glyphs draw nothing.
    pub fn is_invisible(&self) -> bool {
        // SAFETY: The entry's retained reference keeps the font live.
        unsafe { ladybird_gfx_font_is_invisible(self.as_raw()) }
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
        let ask = || {
            // SAFETY: The entry's retained reference keeps the font live.
            unsafe { ladybird_gfx_font_contains_glyph(self.as_raw(), code_point) }
        };
        if code_point == u32::from(' ') {
            return self.0.contains_space_glyph.get(ask);
        }
        ask()
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

    /// The first font of the cascade, or its last resort font if it has none, as Gfx::FontCascadeList::first().
    pub fn first(&self) -> FontHandle {
        assert!(!self.pointer.is_null(), "Gfx::FontCascadeList pointer must not be null");
        // SAFETY: This handle keeps the list live, and the list keeps its fonts live for the call.
        unsafe { FontHandle::intern(ladybird_gfx_font_cascade_list_first(self.pointer)) }
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
    /// The font a pending face's load filled in, after which the entry selects exactly as a loaded
    /// face's would. A cascade keeps its identity across the load, so the styles naming it stay
    /// the same and only what reads its fonts runs again.
    loaded: std::sync::OnceLock<FontHandle>,
}

/// An entry as it selects now: its own font or the one a load filled in, and whether it still
/// waits on a load.
#[derive(Clone, Copy)]
struct EntrySelection<'a> {
    font: Option<&'a FontHandle>,
    pending: Option<(u64, PendingFontState)>,
    source_face_id: u64,
    ranges: &'a [UnicodeRange],
}

/// How this cascade asks for a font no listed family covers.
#[derive(Clone, Copy, Debug, PartialEq)]
struct SystemFallbackStyle {
    point_size: f32,
    weight: u16,
    width: u16,
    slope: u8,
}

/// The index of the first entry with a font and the ASCII code points it resolves, where one does.
type FirstFontAsciiCodePoints = Option<(usize, u128)>;

/// OPTIMIZATION: What text chunking asks of a cascade every time it starts, remembered for a fill generation without a
///               lock. The answer takes more than one word, so its header says when the words after it are being
///               written, and a read that finds the header moved under it counts as a miss. Only one thread at a time
///               writes; another keeps the answer it computed to itself.
#[derive(Default)]
struct FirstFontAsciiCodePointsMemo {
    /// The fill generation in the high half, and in the low half the bits below with the index of the entry.
    header: std::sync::atomic::AtomicU64,
    code_points: [std::sync::atomic::AtomicU64; 2],
}

impl FirstFontAsciiCodePointsMemo {
    const ANSWERED: u64 = 1 << 31;
    const WRITING: u64 = 1 << 30;
    const HAS_FIRST_FONT: u64 = 1 << 29;
    const ENTRY_INDEX_MASK: u64 = Self::HAS_FIRST_FONT - 1;

    fn answer_at(&self, generation: u32) -> Option<FirstFontAsciiCodePoints> {
        use std::sync::atomic::{Ordering, fence};
        let header = self.header.load(Ordering::Acquire);
        if header & (Self::ANSWERED | Self::WRITING) != Self::ANSWERED || (header >> 32) as u32 != generation {
            return None;
        }
        if header & Self::HAS_FIRST_FONT == 0 {
            return Some(None);
        }
        let low = self.code_points[0].load(Ordering::Relaxed);
        let high = self.code_points[1].load(Ordering::Relaxed);
        fence(Ordering::Acquire);
        if self.header.load(Ordering::Relaxed) != header {
            return None;
        }
        let index = (header & Self::ENTRY_INDEX_MASK) as usize;
        Some(Some((index, (u128::from(high) << 64) | u128::from(low))))
    }

    fn remember(&self, generation: u32, answer: FirstFontAsciiCodePoints) {
        use std::sync::atomic::{Ordering, fence};
        let header = self.header.load(Ordering::Relaxed);
        if header & Self::WRITING != 0
            || self
                .header
                .compare_exchange(header, header | Self::WRITING, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
        {
            return;
        }
        fence(Ordering::Release);
        let mut new_header = (u64::from(generation) << 32) | Self::ANSWERED;
        if let Some((index, code_points)) = answer {
            new_header |= Self::HAS_FIRST_FONT | (index as u64 & Self::ENTRY_INDEX_MASK);
            self.code_points[0].store(code_points as u64, Ordering::Relaxed);
            self.code_points[1].store((code_points >> 64) as u64, Ordering::Relaxed);
        }
        self.header.store(new_header, Ordering::Release);
    }
}

/// A font a cascade answered with, named by where the cascade keeps it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RememberedFont {
    OwnFont(usize),
    FilledEntry(usize),
    SystemFallbackFont(usize),
}

impl RememberedFont {
    const INDEX_BITS: u32 = 7;
    const KIND_SHIFT: u32 = Self::INDEX_BITS;
    const PRESENTATION_SHIFT: u32 = Self::KIND_SHIFT + 2;
    const CODE_POINT_SHIFT: u32 = Self::PRESENTATION_SHIFT + 2;
    const INDEX_MASK: u32 = (1 << Self::INDEX_BITS) - 1;
    const KIND_OWN_FONT: u32 = 1;
    const KIND_FILLED_ENTRY: u32 = 2;
    const KIND_SYSTEM_FALLBACK_FONT: u32 = 3;

    fn presentation_bits(presentation: EmojiPresentation) -> u32 {
        u32::from(presentation.is_emoji) | u32::from(presentation.forced) << 1
    }

    /// The answer word of [`BeyondAsciiAnswers`]: the fill generation in the high half, and in the low half the code
    /// point, its presentation and the font. None for a font whose index does not fit.
    fn answer_word(self, generation: u32, code_point: u32, presentation: EmojiPresentation) -> Option<u64> {
        let (kind, index) = match self {
            Self::OwnFont(index) => (Self::KIND_OWN_FONT, index),
            Self::FilledEntry(index) => (Self::KIND_FILLED_ENTRY, index),
            Self::SystemFallbackFont(index) => (Self::KIND_SYSTEM_FALLBACK_FONT, index),
        };
        let index = u32::try_from(index).ok().filter(|index| *index <= Self::INDEX_MASK)?;
        let low = code_point << Self::CODE_POINT_SHIFT
            | Self::presentation_bits(presentation) << Self::PRESENTATION_SHIFT
            | kind << Self::KIND_SHIFT
            | index;
        Some(u64::from(generation) << 32 | u64::from(low))
    }

    /// The font an answer word remembers for `code_point` in `presentation`, if it remembers one found at `generation`.
    fn from_answer_word(word: u64, generation: u32, code_point: u32, presentation: EmojiPresentation) -> Option<Self> {
        let low = word as u32;
        if (word >> 32) as u32 != generation
            || low >> Self::CODE_POINT_SHIFT != code_point
            || (low >> Self::PRESENTATION_SHIFT) & 0b11 != Self::presentation_bits(presentation)
        {
            return None;
        }
        let index = (low & Self::INDEX_MASK) as usize;
        match (low >> Self::KIND_SHIFT) & 0b11 {
            Self::KIND_OWN_FONT => Some(Self::OwnFont(index)),
            Self::KIND_FILLED_ENTRY => Some(Self::FilledEntry(index)),
            Self::KIND_SYSTEM_FALLBACK_FONT => Some(Self::SystemFallbackFont(index)),
            _ => None,
        }
    }
}

const BEYOND_ASCII_ANSWER_SLOTS: usize = 512;
const MAX_REMEMBERED_SYSTEM_FALLBACK_FONTS: usize = 32;

/// OPTIMIZATION: What a cascade answered for the code points its ASCII cache does not take: other code points, and
///               ASCII ones in emoji or forced presentation. Each answer sits in the slot its code point maps to, and a
///               later answer for another code point there replaces it, so reading takes no lock. The fonts the system
///               fallback service answered with are kept here too, once each, so an answer can name them.
struct BeyondAsciiAnswers {
    words: [std::sync::atomic::AtomicU64; BEYOND_ASCII_ANSWER_SLOTS],
    system_fallback_fonts: [std::sync::OnceLock<FontHandle>; MAX_REMEMBERED_SYSTEM_FALLBACK_FONTS],
    system_fallback_font_appending: std::sync::Mutex<()>,
}

impl Default for BeyondAsciiAnswers {
    fn default() -> Self {
        Self {
            words: [const { std::sync::atomic::AtomicU64::new(0) }; BEYOND_ASCII_ANSWER_SLOTS],
            system_fallback_fonts: [const { std::sync::OnceLock::new() }; MAX_REMEMBERED_SYSTEM_FALLBACK_FONTS],
            system_fallback_font_appending: std::sync::Mutex::new(()),
        }
    }
}

impl BeyondAsciiAnswers {
    fn word_for(&self, code_point: u32, presentation: EmojiPresentation) -> &std::sync::atomic::AtomicU64 {
        let slot = (code_point ^ RememberedFont::presentation_bits(presentation) << 8) as usize;
        &self.words[slot % BEYOND_ASCII_ANSWER_SLOTS]
    }

    fn system_fallback_font_index(&self, font: &FontHandle) -> Option<usize> {
        self.system_fallback_fonts
            .iter()
            .map_while(std::sync::OnceLock::get)
            .position(|kept| kept == font)
    }

    /// The kept copy of a font the system fallback service answered with, keeping `font` if it is the first copy, or
    /// `font` itself back once there is no room left.
    fn keep_system_fallback_font(&self, font: FontHandle) -> Result<&FontHandle, FontHandle> {
        let _appending = self
            .system_fallback_font_appending
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if let Some(index) = self.system_fallback_font_index(&font) {
            return Ok(self.system_fallback_fonts[index].get().expect("a found slot is filled"));
        }
        let Some(slot) = self.system_fallback_fonts.iter().find(|slot| slot.get().is_none()) else {
            return Err(font);
        };
        Ok(slot.get_or_init(|| font))
    }
}

/// A cascade list frozen at the moment the document published it: no caches to fill, no faces to
/// resolve, nothing to ask the document. Everything a render pass needs to pick a font for a code
/// point is here, and everything it decides that the document must act on is a number it leaves
/// behind in [`take_wanted_pending_faces`].
// NB: Aligned as FontEntry is, which keeps the reference counts off the line of the entry pointers every lookup reads.
#[repr(align(128))]
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
    /// OPTIMIZATION: An answer for a code point is the same answer until a load fills a face in, so
    ///               each is remembered with the fill generation it was found at; a racing write
    ///               stores the value the other thread computed.
    ascii_cache: [std::sync::atomic::AtomicU64; 128],
    /// The first font with a face and the ASCII code points it resolves.
    ascii_code_points_resolved_by_first_font: FirstFontAsciiCodePointsMemo,
    /// https://drafts.csswg.org/css-fonts-4/#invisible-fallback
    /// Built on demand, and only for a cascade that has a face in its block period.
    invisible_variants: std::sync::Mutex<std::collections::HashMap<FontId, FontHandle>>,
    /// The first available font, as an ASCII cache answer is remembered.
    first_available: std::sync::atomic::AtomicU64,
    /// Made on the first answer the ASCII cache does not take.
    beyond_ascii_answers: std::sync::OnceLock<Box<BeyondAsciiAnswers>>,
    /// How many times loads filled pending faces in, so a memo of what the cascade answered can
    /// tell an answer from before a fill.
    fill_generation: std::sync::atomic::AtomicU32,
}

/// The pending faces of a frozen cascade that loads filled in, each with the font it now selects:
/// what a cascade resolved against the newer @font-face table answers in their places. Made where
/// both cascades are live, and applied wherever the cascade's readers expect its change.
pub struct FrozenFontFills {
    list: Arc<FrozenFontList>,
    fills: Vec<(usize, FontHandle)>,
}

/// A cached answer naming the font a load filled into an entry, by the entry's index, rather than
/// one of [`FrozenFontList::fonts`].
const CACHED_FILLED_ENTRY: u32 = 1 << 31;

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

    /// The font the entry selects with: its own, or the one a load filled in.
    fn font(&self) -> Option<&FontHandle> {
        self.font.as_ref().or_else(|| self.loaded.get())
    }

    /// The face's load state while it still waits on its load.
    fn pending(&self) -> Option<(u64, PendingFontState)> {
        self.pending.filter(|_| self.loaded.get().is_none())
    }

    /// Whether the entry waits on a load with no font to select yet, which a fill can change.
    fn awaits_fill(&self) -> bool {
        self.font.is_none() && self.pending().is_some()
    }

    fn selection<'a>(&'a self, ranges: &'a [UnicodeRange]) -> EntrySelection<'a> {
        EntrySelection {
            font: self.font(),
            pending: self.pending(),
            source_face_id: self.source_face_id,
            ranges: &ranges[self.ranges.clone()],
        }
    }
}

/// Whether two cascades' entries, as they select now, render every code point the same.
fn selections_resolve_alike<'a>(
    entries: impl ExactSizeIterator<Item = EntrySelection<'a>>,
    other_entries: impl ExactSizeIterator<Item = EntrySelection<'a>>,
) -> bool {
    let mut has_earlier_pending_selection = false;
    entries.len() == other_entries.len()
        && entries.zip(other_entries).all(|(entry, other_entry)| {
            if entry.font != other_entry.font || entry.ranges != other_entry.ranges {
                return false;
            }
            if entry.font.is_some() {
                // A preceding unresolved face can start fallback, which skips resident pending faces
                // but still considers loaded entries. Without one, both entries render the same font.
                return !has_earlier_pending_selection || entry.pending.is_some() == other_entry.pending.is_some();
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
}

impl FrozenFontFills {
    /// The fills that make `list` select as `resolved` does, where loads of faces `list` still
    /// waits on are all that tells them apart; none otherwise.
    pub fn toward(list: Arc<FrozenFontList>, resolved: &FrozenFontList) -> Option<Self> {
        if list.entries.len() != resolved.entries.len() {
            return None;
        }
        let fills: Vec<(usize, FontHandle)> = list
            .entries
            .iter()
            .zip(&resolved.entries)
            .enumerate()
            .filter_map(|(index, (entry, resolved_entry))| {
                let font = resolved_entry.font()?;
                (entry.awaits_fill() && resolved_entry.pending().is_none()).then(|| (index, font.clone()))
            })
            .collect();
        if fills.is_empty() {
            return None;
        }
        let filled = list.entries.iter().enumerate().map(|(index, entry)| {
            match fills.binary_search_by_key(&index, |(filled_index, _)| *filled_index) {
                Ok(fill) => EntrySelection {
                    font: Some(&fills[fill].1),
                    pending: None,
                    ..entry.selection(&list.ranges)
                },
                Err(_) => entry.selection(&list.ranges),
            }
        });
        let resolves_alike = list.last_resort == resolved.last_resort
            && list.system_fallback == resolved.system_fallback
            && selections_resolve_alike(
                filled,
                resolved.entries.iter().map(|entry| entry.selection(&resolved.ranges)),
            )
            && selections_resolve_alike(
                list.fallback_entries
                    .iter()
                    .map(|entry| entry.selection(&list.fallback_ranges)),
                resolved
                    .fallback_entries
                    .iter()
                    .map(|entry| entry.selection(&resolved.fallback_ranges)),
            );
        resolves_alike.then_some(Self { list, fills })
    }

    /// Fills the faces in. A face some earlier fill already filled keeps the font it was given.
    pub fn apply(self) {
        for (index, font) in self.fills {
            let _ = self.list.entries[index].loaded.set(font);
        }
        self.list
            .fill_generation
            .fetch_add(1, std::sync::atomic::Ordering::Release);
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
            ascii_cache: std::array::from_fn(|_| std::sync::atomic::AtomicU64::new(0)),
            ascii_code_points_resolved_by_first_font: FirstFontAsciiCodePointsMemo::default(),
            invisible_variants: std::sync::Mutex::new(std::collections::HashMap::new()),
            first_available: std::sync::atomic::AtomicU64::new(0),
            beyond_ascii_answers: std::sync::OnceLock::new(),
            fill_generation: std::sync::atomic::AtomicU32::new(0),
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
                        loaded: std::sync::OnceLock::new(),
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
            ascii_cache: std::array::from_fn(|_| std::sync::atomic::AtomicU64::new(0)),
            ascii_code_points_resolved_by_first_font: FirstFontAsciiCodePointsMemo::default(),
            invisible_variants: std::sync::Mutex::new(std::collections::HashMap::new()),
            first_available: std::sync::atomic::AtomicU64::new(0),
            beyond_ascii_answers: std::sync::OnceLock::new(),
            fill_generation: std::sync::atomic::AtomicU32::new(0),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty() && self.last_resort.is_none()
    }

    /// Compares immutable selections, including pending faces and their font-display periods, rather than the
    /// mutable faces those selections were built from. Load-request identities and lookup caches do not affect
    /// which glyphs a snapshot renders.
    pub fn resolves_like(&self, other: &Self) -> bool {
        std::ptr::eq(self, other)
            || (self.last_resort == other.last_resort
                && self.system_fallback == other.system_fallback
                && selections_resolve_alike(
                    self.entries.iter().map(|entry| entry.selection(&self.ranges)),
                    other.entries.iter().map(|entry| entry.selection(&other.ranges)),
                )
                && selections_resolve_alike(
                    self.fallback_entries
                        .iter()
                        .map(|entry| entry.selection(&self.fallback_ranges)),
                    other
                        .fallback_entries
                        .iter()
                        .map(|entry| entry.selection(&other.fallback_ranges)),
                ))
    }

    /// How many times loads filled pending faces of the cascade in. What it answers for a code
    /// point, and its first available font, only move when this does.
    pub fn fill_generation(&self) -> u32 {
        self.fill_generation.load(std::sync::atomic::Ordering::Acquire)
    }

    pub fn has_pending_faces(&self) -> bool {
        self.entries.iter().any(|entry| entry.pending().is_some())
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
        self.font_for_code_point_borrowed(code_point, presentation).into_owned()
    }

    #[inline]
    pub fn font_for_code_point_borrowed(
        &self,
        code_point: u32,
        presentation: EmojiPresentation,
    ) -> Cow<'_, FontHandle> {
        let remembered = if Self::ascii_cache_takes(code_point, presentation) {
            self.cached_font(&self.ascii_cache[code_point as usize])
        } else {
            self.remembered_beyond_ascii_font(code_point, presentation)
        };
        match remembered {
            Some(font) => Cow::Borrowed(font),
            None => self.resolve_font_for_code_point(code_point, presentation),
        }
    }

    #[inline]
    fn ascii_cache_takes(code_point: u32, presentation: EmojiPresentation) -> bool {
        code_point < 128 && !presentation.is_emoji && !presentation.forced
    }

    #[inline]
    fn remembered_beyond_ascii_font(&self, code_point: u32, presentation: EmojiPresentation) -> Option<&FontHandle> {
        let answers = self.beyond_ascii_answers.get()?;
        let word = answers
            .word_for(code_point, presentation)
            .load(std::sync::atomic::Ordering::Relaxed);
        match RememberedFont::from_answer_word(word, self.fill_generation(), code_point, presentation)? {
            RememberedFont::OwnFont(index) => self.fonts.get(index),
            RememberedFont::FilledEntry(index) => self.entries.get(index)?.loaded.get(),
            RememberedFont::SystemFallbackFont(index) => answers.system_fallback_fonts.get(index)?.get(),
        }
    }

    fn remember_beyond_ascii(
        &self,
        code_point: u32,
        presentation: EmojiPresentation,
        generation: u32,
        font: &FontHandle,
    ) {
        let answers = self.beyond_ascii_answers.get_or_init(Box::default);
        let remembered = if let Some(index) = self.fonts.iter().position(|owned| owned == font) {
            RememberedFont::OwnFont(index)
        } else if let Some(index) = self.entries.iter().position(|entry| entry.loaded.get() == Some(font)) {
            RememberedFont::FilledEntry(index)
        } else if let Some(index) = answers.system_fallback_font_index(font) {
            RememberedFont::SystemFallbackFont(index)
        } else {
            return;
        };
        if let Some(word) = remembered.answer_word(generation, code_point, presentation) {
            answers
                .word_for(code_point, presentation)
                .store(word, std::sync::atomic::Ordering::Relaxed);
        }
    }

    pub fn first_font_and_ascii_code_points_it_resolves_in_text_presentation(&self) -> Option<(&FontHandle, u128)> {
        let generation = self.fill_generation();
        // NB: Filling a face in moves which font resolves a code point, so an answer holds for its generation.
        let memo = &self.ascii_code_points_resolved_by_first_font;
        let (index, code_points) = memo.answer_at(generation).unwrap_or_else(|| {
            let answer = self.ascii_code_points_resolved_by_the_first_font();
            memo.remember(generation, answer);
            answer
        })?;
        self.entries[index].font().map(|font| (font, code_points))
    }

    fn ascii_code_points_resolved_by_the_first_font(&self) -> Option<(usize, u128)> {
        let index = self.entries.iter().position(|entry| entry.font().is_some())?;
        let entry = &self.entries[index];
        let font = entry.font()?;
        if font.is_emoji_font() {
            return None;
        }
        let is_resolved_by_first_font = |code_point: u32| {
            entry.covers(code_point, &self.ranges)
                && font.contains_glyph(code_point)
                && !self.entries[..index]
                    .iter()
                    .any(|earlier| earlier.pending.is_some() && earlier.covers(code_point, &self.ranges))
        };
        let code_points = (0..128)
            .filter(|&code_point| is_resolved_by_first_font(code_point))
            .fold(0u128, |code_points, code_point| code_points | 1 << code_point);
        Some((index, code_points))
    }

    #[inline(never)]
    fn resolve_font_for_code_point(&self, code_point: u32, presentation: EmojiPresentation) -> Cow<'_, FontHandle> {
        let generation = self.fill_generation();
        let presentation_matches = |font: &FontHandle| font.is_emoji_font() == presentation.is_emoji;
        let contains_glyph = |entry: &FrozenEntry, ranges: &[UnicodeRange]| -> bool {
            let Some(font) = entry.font() else { return false };
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
            if let Some((face_id, state)) = entry.pending() {
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
            let font = entry.font().expect("an entry with a glyph has a font");
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
                let font = entry.font().expect("an entry with a glyph has a font");
                if presentation_matches(font) {
                    selected = Some(font);
                    break;
                }
                fallback_glyph_match = fallback_glyph_match.or(Some(font));
            }
        }

        if let Some(font) = selected {
            return self.finish(code_point, presentation, generation, invisible, Cow::Borrowed(font));
        }

        if let Some(fallback) = self.system_fallback_font(code_point, presentation)
            && (presentation_matches(&fallback) || (author_glyph_match.is_none() && fallback_glyph_match.is_none()))
        {
            return self.finish(code_point, presentation, generation, invisible, Cow::Owned(fallback));
        }

        let font = author_glyph_match
            .or(fallback_glyph_match)
            .or(self.last_resort.as_ref());
        match font {
            Some(font) => self.finish(code_point, presentation, generation, invisible, Cow::Borrowed(font)),
            // A cascade with no fonts at all cannot answer; only a list nobody published can be
            // in that state, and the layout stage never receives one.
            None => panic!("frozen font cascade has no font for U+{code_point:04X}"),
        }
    }

    /// https://drafts.csswg.org/css-fonts/#first-available-font
    /// Mirrors `Gfx::FontCascadeList::first_available_font`: the first font whose `unicode-range`
    /// does not exclude U+0020 SPACE, skipping faces still waiting on their loads, or the last
    /// resort font.
    pub fn first_available_font(&self) -> &FontHandle {
        if let Some(font) = self.cached_font(&self.first_available) {
            return font;
        }
        const SPACE: u32 = 0x20;
        let generation = self.fill_generation();
        let font = self
            .entries
            .iter()
            .filter(|entry| entry.covers(SPACE, &self.ranges))
            .find_map(FrozenEntry::font)
            .or(self.last_resort.as_ref())
            .expect("a published font cascade has a first available font");
        self.remember(&self.first_available, generation, font);
        font
    }

    /// The font a cache word remembers, if it remembers one found since the last fill.
    fn cached_font(&self, cache: &std::sync::atomic::AtomicU64) -> Option<&FontHandle> {
        let word = cache.load(std::sync::atomic::Ordering::Relaxed);
        if (word >> 32) as u32 != self.fill_generation() {
            return None;
        }
        match word as u32 {
            0 => None,
            token if token & CACHED_FILLED_ENTRY != 0 => {
                self.entries[(token & !CACHED_FILLED_ENTRY) as usize].loaded.get()
            }
            token => Some(&self.fonts[token as usize - 1]),
        }
    }

    /// Remembers `font`, found at the fill generation `generation`, in a cache word, where it is one
    /// of the cascade's own fonts or a font a load filled in.
    fn remember(&self, cache: &std::sync::atomic::AtomicU64, generation: u32, font: &FontHandle) {
        if let Some(token) = self.cache_token(font) {
            cache.store(
                (u64::from(generation) << 32) | u64::from(token),
                std::sync::atomic::Ordering::Relaxed,
            );
        }
    }

    /// How a cache word remembers one of the cascade's own fonts, or a font a load filled in.
    fn cache_token(&self, font: &FontHandle) -> Option<u32> {
        if let Some(index) = self.fonts.iter().position(|owned| owned == font) {
            return Some(index as u32 + 1);
        }
        self.entries
            .iter()
            .position(|entry| entry.loaded.get() == Some(font))
            .map(|index| index as u32 | CACHED_FILLED_ENTRY)
    }

    fn note_wanted_pending_face(&self, entry: &FrozenEntry, face_id: u64) {
        if entry.wanted.swap(true, std::sync::atomic::Ordering::Relaxed) {
            return;
        }
        // SAFETY: The list is LibGfx's, and the number is all it takes.
        unsafe { ladybird_gfx_process_note_wanted_pending_face(face_id) };
    }

    /// Remembers the answer `font`, which is one of the cascade's own fonts, borrowed, or one the system fallback
    /// service answered with, owned.
    fn finish<'list>(
        &'list self,
        code_point: u32,
        presentation: EmojiPresentation,
        generation: u32,
        invisible: bool,
        font: Cow<'list, FontHandle>,
    ) -> Cow<'list, FontHandle> {
        if invisible {
            // An invisible answer is never cached: it is the rare path, and caching it would make
            // the memo depend on which pending face the walk happened to reach first.
            return Cow::Owned(self.invisible_variant(&font));
        }
        let font = match font {
            Cow::Borrowed(own_font) => own_font,
            Cow::Owned(system_fallback_font) => {
                match self
                    .beyond_ascii_answers
                    .get_or_init(Box::default)
                    .keep_system_fallback_font(system_fallback_font)
                {
                    Ok(kept) => kept,
                    Err(not_kept) => return Cow::Owned(not_kept),
                }
            }
        };
        if Self::ascii_cache_takes(code_point, presentation) {
            self.remember(&self.ascii_cache[code_point as usize], generation, font);
        } else {
            self.remember_beyond_ascii(code_point, presentation, generation, font);
        }
        Cow::Borrowed(font)
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

impl FrozenFontFills {
    /// [`FrozenFontFills::toward`] for the frozen snapshots of two `Gfx::FontCascadeList`s.
    ///
    /// # Safety
    ///
    /// Both pointers must name live `Gfx::FontCascadeList`s.
    pub unsafe fn toward_cascade(list: *const c_void, resolved: *const c_void) -> Option<Self> {
        // SAFETY: The caller guarantees both cascades are live for this call.
        let (frozen, resolved) = unsafe { (frozen_font_list_of(list), frozen_font_list_of(resolved)) };
        let fills = Self::toward(frozen.to_arc()?, resolved.list()?)?;
        // The cascade's own pending faces must resolve to the fonts filled in, so what it answers on the document
        // thread agrees with what its frozen list answers once filled. A face sized by font-size-adjust, which the
        // load moves, does not.
        // SAFETY: As above.
        let live = unsafe { FrozenFontList::from_cascade_list(list) };
        fills
            .fills
            .iter()
            .all(|(index, font)| live.entries.get(*index).and_then(|entry| entry.font.as_ref()) == Some(font))
            .then_some(fills)
    }
}

/// Whether loads of the faces the cascade `list` waits on are all that tells it apart from
/// `resolved`, so filling them in makes it select as `resolved` does.
///
/// # Safety
///
/// Both pointers must name live `Gfx::FontCascadeList`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_font_cascade_list_fills_toward(
    list: *const c_void,
    resolved: *const c_void,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { FrozenFontFills::toward_cascade(list, resolved) }.is_some()
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

#[cfg(test)]
mod first_font_ascii_code_points_memo_tests {
    use super::FirstFontAsciiCodePointsMemo;

    #[test]
    fn an_answer_holds_for_its_fill_generation() {
        let memo = FirstFontAsciiCodePointsMemo::default();
        assert_eq!(memo.answer_at(0), None);

        let code_points = (7_u128 << 64) | 9;
        memo.remember(3, Some((5, code_points)));
        assert_eq!(memo.answer_at(3), Some(Some((5, code_points))));
        assert_eq!(memo.answer_at(4), None);

        memo.remember(4, None);
        assert_eq!(memo.answer_at(4), Some(None));
        assert_eq!(memo.answer_at(3), None);
    }
}

#[cfg(test)]
mod beyond_ascii_answer_word_tests {
    use super::{EmojiPresentation, RememberedFont};

    const TEXT: EmojiPresentation = EmojiPresentation {
        is_emoji: false,
        forced: false,
    };
    const FORCED_EMOJI: EmojiPresentation = EmojiPresentation {
        is_emoji: true,
        forced: true,
    };

    #[test]
    fn an_answer_holds_for_its_code_point_presentation_and_fill_generation() {
        for font in [
            RememberedFont::OwnFont(0),
            RememberedFont::FilledEntry(5),
            RememberedFont::SystemFallbackFont(127),
        ] {
            let word = font.answer_word(3, 0x4e2d, TEXT).unwrap();
            assert_eq!(RememberedFont::from_answer_word(word, 3, 0x4e2d, TEXT), Some(font));
            assert_eq!(RememberedFont::from_answer_word(word, 4, 0x4e2d, TEXT), None);
            assert_eq!(RememberedFont::from_answer_word(word, 3, 0x4e2d + 512, TEXT), None);
            assert_eq!(RememberedFont::from_answer_word(word, 3, 0x4e2d, FORCED_EMOJI), None);
        }

        let word = RememberedFont::OwnFont(1)
            .answer_word(0, 0x10ffff, FORCED_EMOJI)
            .unwrap();
        assert_eq!(
            RememberedFont::from_answer_word(word, 0, 0x10ffff, FORCED_EMOJI),
            Some(RememberedFont::OwnFont(1))
        );
    }

    #[test]
    fn an_empty_word_or_a_font_index_that_does_not_fit_remembers_nothing() {
        assert_eq!(RememberedFont::from_answer_word(0, 0, 0, TEXT), None);
        assert_eq!(RememberedFont::OwnFont(128).answer_word(0, 0x4e2d, TEXT), None);
    }
}
