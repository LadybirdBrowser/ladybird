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
}

#[derive(Clone, Copy)]
pub struct FontRef<'a> {
    raw: NonNull<c_void>,
    _lifetime: PhantomData<&'a c_void>,
}

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
    pub(crate) fn as_ptr(self) -> *const c_void {
        self.raw.as_ptr()
    }
}
