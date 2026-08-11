/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;

unsafe extern "C" {
    fn ladybird_gfx_font_glyph_width(font: *const c_void, code_point: u32) -> f32;
    fn ladybird_gfx_font_glyph_id(font: *const c_void, code_point: u32) -> u32;
    fn ladybird_gfx_font_contains_glyph(font: *const c_void, code_point: u32) -> bool;
    fn ladybird_gfx_font_is_emoji_font(font: *const c_void) -> bool;
    fn ladybird_gfx_font_cascade_list_font_for_code_point(
        list: *const c_void,
        code_point: u32,
        trigger_pending_loads: bool,
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
    pub fn is_emoji_font(self) -> bool {
        // SAFETY: FontRef's constructor requires the Gfx::Font to remain live
        // for this reference's lifetime.
        unsafe { ladybird_gfx_font_is_emoji_font(self.raw.as_ptr()) }
    }

    #[inline]
    pub(crate) fn as_ptr(self) -> *const c_void {
        self.raw.as_ptr()
    }

    #[inline]
    pub fn as_raw(self) -> *const c_void {
        self.raw.as_ptr()
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

    pub fn font_for_code_point(
        self,
        code_point: u32,
        trigger_pending_loads: bool,
        presentation: EmojiPresentation,
    ) -> FontRef<'a> {
        // SAFETY: The constructor requires the Gfx::FontCascadeList to remain
        // live for this reference's lifetime, and the fonts it resolves are
        // owned by it.
        unsafe {
            FontRef::from_raw(ladybird_gfx_font_cascade_list_font_for_code_point(
                self.raw.as_ptr(),
                code_point,
                trigger_pending_loads,
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

retained_ffi_handle!(
    /// A strong reference to a `Gfx::FontCascadeList`, keeping the list and every
    /// font it can resolve alive until dropped.
    RetainedFontCascadeList,
    ladybird_gfx_font_cascade_list_ref,
    ladybird_gfx_font_cascade_list_unref,
    "Gfx::FontCascadeList"
);
