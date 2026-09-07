/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;

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

impl Drop for FontEntry {
    fn drop(&mut self) {
        // SAFETY: FontHandle::intern took the reference this releases.
        unsafe { ladybird_gfx_font_unref(self.raw.as_ptr()) };
    }
}

#[derive(Clone)]
pub struct FontHandle(Rc<FontEntry>);

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
        Self(Rc::new(FontEntry {
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
