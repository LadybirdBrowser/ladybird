/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::rc::Rc;

unsafe extern "C" {
    fn ladybird_gfx_font_snapshot(font: *const c_void, out_snapshot: *mut FfiFontSnapshot);
    fn ladybird_gfx_font_id(font: *const c_void) -> u64;
    fn ladybird_gfx_font_glyph_width(font: *const c_void, code_point: u32) -> f32;
    fn ladybird_gfx_font_glyph_id(font: *const c_void, code_point: u32) -> u32;
    fn ladybird_gfx_font_contains_glyph(font: *const c_void, code_point: u32) -> bool;
    fn ladybird_gfx_font_is_emoji_font(font: *const c_void) -> bool;
    fn ladybird_gfx_font_pixel_metrics(font: *const c_void, ascent: *mut f32, descent: *mut f32);
    fn ladybird_gfx_font_pixel_size(font: *const c_void) -> f32;
    fn ladybird_gfx_font_x_height(font: *const c_void) -> f32;
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
    fn ladybird_gfx_emoji_presentation_for_code_point(
        code_point: u32,
        next_code_point: u32,
        has_next_code_point: bool,
    ) -> u8;
}

#[derive(Clone, Copy)]
pub struct FontRef<'a> {
    raw: NonNull<c_void>,
    _lifetime: PhantomData<&'a c_void>,
}

impl PartialEq for FontRef<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
    }
}

impl Eq for FontRef<'_> {}

impl<'a> FontRef<'a> {
    /// # Safety
    ///
    /// `raw` must point to a live `Gfx::Font` for the returned reference's
    /// lifetime.
    #[inline]
    pub unsafe fn from_raw(raw: *const c_void) -> Self {
        Self {
            raw: NonNull::new(raw.cast_mut()).expect("Gfx::Font pointer must not be null"),
            _lifetime: PhantomData,
        }
    }

    /// The font's process-unique id. Ids are never recycled, so a dead font's
    /// id can never alias a live font's.
    #[inline]
    pub fn id(self) -> u64 {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_id(self.raw.as_ptr()) }
    }

    #[inline]
    pub fn glyph_width(self, code_point: u32) -> f32 {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_glyph_width(self.raw.as_ptr(), code_point) }
    }

    #[inline]
    pub fn glyph_id_for_code_point(self, code_point: u32) -> u32 {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_glyph_id(self.raw.as_ptr(), code_point) }
    }

    #[inline]
    pub fn contains_glyph(self, code_point: u32) -> bool {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_contains_glyph(self.raw.as_ptr(), code_point) }
    }

    #[inline]
    pub fn pixel_metrics_ascent_descent(self) -> (f32, f32) {
        let mut ascent = 0.0f32;
        let mut descent = 0.0f32;
        // SAFETY: The font is live for the lifetime of this reference; the out-pointers address
        // local floats.
        unsafe { ladybird_gfx_font_pixel_metrics(self.raw.as_ptr(), &raw mut ascent, &raw mut descent) };
        (ascent, descent)
    }

    #[inline]
    pub fn pixel_size(self) -> f32 {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_pixel_size(self.raw.as_ptr()) }
    }

    #[inline]
    pub fn x_height(self) -> f32 {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_x_height(self.raw.as_ptr()) }
    }

    pub fn measure_text_width(self, text: &[u16]) -> f32 {
        // SAFETY: The font is live for this reference's lifetime, and the text
        // slice stays valid for the synchronous measuring call.
        unsafe { ladybird_gfx_font_measure_text_width(self.raw.as_ptr(), text.as_ptr(), text.len()) }
    }

    pub fn is_emoji_font(self) -> bool {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_is_emoji_font(self.raw.as_ptr()) }
    }

    #[inline]
    pub fn as_raw(self) -> *const c_void {
        self.raw.as_ptr()
    }
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
    retained: RetainedFont,
    facts: FontFacts,
}

#[derive(Clone)]
pub struct FontHandle(Rc<FontEntry>);

impl FontHandle {
    /// # Safety
    ///
    /// `raw` must point to a live `Gfx::Font`.
    pub unsafe fn intern(raw: *const c_void) -> Self {
        let mut snapshot = FfiFontSnapshot::default();
        // SAFETY: The caller guarantees the font is live, and the out-pointer
        // addresses a local.
        unsafe { ladybird_gfx_font_snapshot(raw, &raw mut snapshot) };
        // SAFETY: The caller guarantees the font is live; the retained
        // reference keeps it that way for the entry's lifetime.
        let retained = unsafe { RetainedFont::retain(raw) };
        Self(Rc::new(FontEntry {
            id: FontId(snapshot.id),
            retained,
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
        self.0.retained.as_raw()
    }

    pub fn is_emoji_font(&self) -> bool {
        // SAFETY: The entry's retained reference keeps the font live.
        unsafe { ladybird_gfx_font_is_emoji_font(self.as_raw()) }
    }

    pub fn glyph_width(&self, code_point: u32) -> f32 {
        // SAFETY: The entry's retained reference keeps the font live.
        unsafe { ladybird_gfx_font_glyph_width(self.as_raw(), code_point) }
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

#[derive(Clone, Copy)]
pub struct FontCascadeListRef<'a> {
    raw: NonNull<c_void>,
    _lifetime: PhantomData<&'a c_void>,
}

impl<'a> FontCascadeListRef<'a> {
    /// # Safety
    ///
    /// `raw` must point to a live `Gfx::FontCascadeList` for the returned
    /// reference's lifetime.
    #[inline]
    pub unsafe fn from_raw(raw: *const c_void) -> Self {
        Self {
            raw: NonNull::new(raw.cast_mut()).expect("Gfx::FontCascadeList pointer must not be null"),
            _lifetime: PhantomData,
        }
    }

    pub fn font_for_code_point(self, code_point: u32, presentation: EmojiPresentation) -> FontRef<'a> {
        // SAFETY: The constructor requires the Gfx::FontCascadeList to remain
        // live for this reference's lifetime, and the fonts it resolves are
        // owned by it.
        unsafe {
            FontRef::from_raw(ladybird_gfx_font_cascade_list_font_for_code_point(
                self.raw.as_ptr(),
                code_point,
                presentation.is_emoji,
                presentation.forced,
            ))
        }
    }

    pub fn first(self) -> FontRef<'a> {
        // SAFETY: The constructor requires the Gfx::FontCascadeList to remain
        // live for this reference's lifetime.
        unsafe { FontRef::from_raw(ladybird_gfx_font_cascade_list_first(self.raw.as_ptr())) }
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

    #[inline]
    pub fn as_ref(&self) -> FontCascadeListRef<'_> {
        // SAFETY: A non-null handle holds a reference that keeps the list live
        // for the lifetime of the returned borrow.
        unsafe { FontCascadeListRef::from_raw(self.pointer) }
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

/// Generates a strong-reference handle over a C++ ref/unref FFI pair:
/// retain-on-construct, release-on-drop.
macro_rules! retained_ffi_handle {
    ($(#[$documentation:meta])* $name:ident, $ref_function:ident, $unref_function:ident, $type_name:literal) => {
        $(#[$documentation])*
        pub struct $name {
            raw: NonNull<c_void>,
        }

        impl $name {
            /// # Safety
            ///
            /// `raw` must point to a live object at the time of the call.
            pub unsafe fn retain(raw: *const c_void) -> Self {
                let raw = NonNull::new(raw.cast_mut()).expect(concat!($type_name, " pointer must not be null"));
                // SAFETY: The caller guarantees the object is live, and the
                // reference taken here keeps it that way until drop.
                unsafe { $ref_function(raw.as_ptr()) };
                Self { raw }
            }

            pub fn as_raw(&self) -> *const c_void {
                self.raw.as_ptr()
            }
        }

        impl Drop for $name {
            fn drop(&mut self) {
                // SAFETY: retain() took a strong reference on construction.
                unsafe { $unref_function(self.raw.as_ptr()) };
            }
        }
    };
}

retained_ffi_handle!(
    /// A strong reference to a single `Gfx::Font`, keeping it alive until dropped.
    RetainedFont,
    ladybird_gfx_font_ref,
    ladybird_gfx_font_unref,
    "Gfx::Font"
);
