/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The strings that LibWeb's C++ code lends the exports of the crate for as long as they run, as the raw
//! representation of an AK::Utf16String or an AK::Utf16FlyString. An export takes one of these where C++ passes such a
//! string, as it takes a `&Vm` where C++ passes the VM, so a value of one is always a live string. Its lifetime is that
//! of the call, which keeps the string from outliving it.

use core::marker::PhantomData;
use core::num::NonZeroUsize;

use ak::{Utf16FlyString, Utf16String};

/// An AK::Utf16String that C++ lends for the call.
#[repr(transparent)]
pub(crate) struct LentUtf16String<'a>(NonZeroUsize, PhantomData<&'a ()>);

impl LentUtf16String<'_> {
    /// The string, with a reference of its own.
    pub(crate) fn to_utf16_string(&self) -> Utf16String {
        // SAFETY: A LentUtf16String is a live string.
        unsafe { Utf16String::from_raw_borrowed(self.0.get()) }
    }
}

/// An AK::Utf16FlyString that C++ lends for the call.
#[repr(transparent)]
pub(crate) struct LentUtf16FlyString<'a>(NonZeroUsize, PhantomData<&'a ()>);

impl LentUtf16FlyString<'_> {
    /// The string, with a reference of its own.
    pub(crate) fn to_utf16_fly_string(&self) -> Utf16FlyString {
        // SAFETY: A LentUtf16FlyString is a live fly string.
        unsafe { Utf16FlyString::from_raw_borrowed(self.0.get()) }
    }
}
