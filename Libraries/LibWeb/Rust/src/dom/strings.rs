/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The AK strings that the cores hold, as Rust borrows them where C++ keeps them.
//!
//! C++ constructs, assigns and destroys these fields, as the C++ types it sees them as. Rust never takes or drops a
//! reference to a string through them: it only reads the code units in place, for as long as it borrows the field.

use ak::Utf16StringUnits;
use core::cell::UnsafeCell;

/// A field that holds an interned string, laid out as C++'s AK::Utf16FlyString, which is what C++ sees it as. Two
/// interned strings are equal exactly when their raw identities are.
#[repr(transparent)]
pub struct FlyStringField(UnsafeCell<usize>);

impl FlyStringField {
    /// The word that names the string, which is the same for every AK::Utf16FlyString of the same string.
    #[inline(always)]
    pub fn raw_identity(&self) -> usize {
        // SAFETY: C++ does not write the field while Rust borrows it.
        unsafe { *self.0.get() }
    }

    /// The string's code units, where the string keeps them.
    #[inline(always)]
    pub fn units(&self) -> Utf16StringUnits<'_> {
        // SAFETY: C++ does not write the field while Rust borrows it, and the string it holds lives as long as it does.
        unsafe { ak::utf16_string_units(&*self.0.get()) }
    }

    #[inline(always)]
    pub fn is_empty(&self) -> bool {
        units_are_empty(self.units())
    }
}

/// A field that holds an interned string or none, laid out as C++'s AK::Optional<AK::Utf16FlyString>, which is what
/// C++ sees it as: the string's raw identity, or 0 for none.
#[repr(transparent)]
pub struct OptionalFlyStringField(UnsafeCell<usize>);

impl OptionalFlyStringField {
    #[inline(always)]
    pub fn get(&self) -> Option<&FlyStringField> {
        // SAFETY: C++ does not write the field while Rust borrows it.
        let raw = unsafe { *self.0.get() };
        // SAFETY: A field that holds a string has the layout of a FlyStringField.
        (raw != 0).then(|| unsafe { &*(self as *const Self).cast::<FlyStringField>() })
    }
}

/// A field that holds a string, laid out as C++'s AK::Utf16String, which is what C++ sees it as.
#[repr(transparent)]
pub struct StringField(UnsafeCell<usize>);

impl StringField {
    /// The string's code units, where the string keeps them.
    #[inline(always)]
    pub fn units(&self) -> Utf16StringUnits<'_> {
        // SAFETY: C++ does not write the field while Rust borrows it, and the string it holds lives as long as it does.
        unsafe { ak::utf16_string_units(&*self.0.get()) }
    }

    #[inline(always)]
    pub fn is_empty(&self) -> bool {
        units_are_empty(self.units())
    }

    /// The interned string with the same code units, as AK::Utf16FlyString's constructor from the string makes it.
    #[inline(always)]
    pub fn to_fly_string(&self) -> ak::Utf16FlyString {
        // SAFETY: C++ does not write the field while Rust borrows it, and the string it holds is a live AK::Utf16String,
        //         whose reference stays the field's, as the borrowed owner is never dropped.
        let string = core::mem::ManuallyDrop::new(unsafe { ak::Utf16String::from_raw_owned(*self.0.get()) });
        ak::Utf16FlyString::from_utf16_string(&string)
    }

    /// The string, as a reference of its own to it, which stays valid however C++ changes the field.
    #[inline(always)]
    pub fn to_utf16_string(&self) -> ak::Utf16String {
        // SAFETY: C++ does not write the field while Rust borrows it, and the string it holds is a live AK::Utf16String.
        unsafe { ak::Utf16String::from_raw_borrowed(*self.0.get()) }
    }
}

#[inline(always)]
fn units_are_empty(units: Utf16StringUnits<'_>) -> bool {
    match units {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.is_empty(),
        Utf16StringUnits::Utf16(units) => units.is_empty(),
    }
}

/// Whether two strings are equal when ASCII letters compare without their case, as AK's equals_ignoring_ascii_case()
/// has it.
pub fn equals_ignoring_ascii_case(a: Utf16StringUnits<'_>, b: Utf16StringUnits<'_>) -> bool {
    fn code_units(units: Utf16StringUnits<'_>) -> (usize, impl Iterator<Item = u16> + '_) {
        let (narrow, wide): (&[u8], &[u16]) = match units {
            Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => (units, &[]),
            Utf16StringUnits::Utf16(units) => (&[], units),
        };
        let code_units = narrow.iter().copied().map(u16::from).chain(wide.iter().copied());
        (narrow.len() + wide.len(), code_units)
    }
    fn lowercase(code_unit: u16) -> u16 {
        match u8::try_from(code_unit) {
            Ok(byte) => u16::from(byte.to_ascii_lowercase()),
            Err(_) => code_unit,
        }
    }
    let (a_length, a) = code_units(a);
    let (b_length, b) = code_units(b);
    a_length == b_length && a.map(lowercase).eq(b.map(lowercase))
}

/// A field that holds the raw word of a test string, which the test leaks.
#[cfg(test)]
impl FlyStringField {
    pub(crate) fn for_test(string: &str) -> Self {
        Self(UnsafeCell::new(ak::Utf16FlyString::from_utf8(string).into_raw()))
    }
}

#[cfg(test)]
impl StringField {
    pub(crate) fn for_test(string: &str) -> Self {
        let units: Vec<u16> = string.encode_utf16().collect();
        Self(UnsafeCell::new(ak::Utf16String::from_utf16(&units).into_raw()))
    }
}

#[cfg(test)]
impl OptionalFlyStringField {
    pub(crate) fn for_test(string: Option<&str>) -> Self {
        Self(UnsafeCell::new(
            string.map_or(0, |string| ak::Utf16FlyString::from_utf8(string).into_raw()),
        ))
    }
}

// NB: The C++ types these fields are, are one word each.
const _: () = assert!(size_of::<FlyStringField>() == 8 && align_of::<FlyStringField>() == 8);
const _: () = assert!(size_of::<OptionalFlyStringField>() == 8 && align_of::<OptionalFlyStringField>() == 8);
const _: () = assert!(size_of::<StringField>() == 8 && align_of::<StringField>() == 8);

#[cfg(test)]
mod tests {
    use super::*;

    fn as_vec(units: Utf16StringUnits<'_>) -> Vec<u16> {
        match units {
            Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => {
                units.iter().map(|&unit| unit.into()).collect()
            }
            Utf16StringUnits::Utf16(units) => units.to_vec(),
        }
    }

    #[test]
    fn a_field_reads_its_string_in_place() {
        let short = FlyStringField::for_test("div");
        assert_eq!(as_vec(short.units()), "div".encode_utf16().collect::<Vec<_>>());
        let long = FlyStringField::for_test("a much longer interned name");
        assert_eq!(
            as_vec(long.units()),
            "a much longer interned name".encode_utf16().collect::<Vec<_>>()
        );
    }

    #[test]
    fn a_string_field_reads_its_string_in_place() {
        for string in ["", "short", "a string too long to be stored in place", "\u{3b1}\u{3b2}"] {
            assert_eq!(
                as_vec(StringField::for_test(string).units()),
                string.encode_utf16().collect::<Vec<_>>()
            );
        }
    }

    #[test]
    fn an_interned_string_has_one_identity() {
        let a = FlyStringField::for_test("section");
        let b = FlyStringField::for_test("section");
        let c = FlyStringField::for_test("article");
        assert_eq!(a.raw_identity(), b.raw_identity());
        assert_ne!(a.raw_identity(), c.raw_identity());
    }

    #[test]
    fn an_optional_field_holds_a_string_or_none() {
        assert!(OptionalFlyStringField::for_test(None).get().is_none());
        let field = OptionalFlyStringField::for_test(Some("main"));
        assert_eq!(
            field.get().map(FlyStringField::raw_identity),
            Some(FlyStringField::for_test("main").raw_identity())
        );
    }

    #[test]
    fn strings_compare_ignoring_the_case_of_ascii_letters_only() {
        let compare = |a: &str, b: &str| {
            equals_ignoring_ascii_case(FlyStringField::for_test(a).units(), FlyStringField::for_test(b).units())
        };
        assert!(compare("Foo", "fOO"));
        assert!(compare("", ""));
        assert!(!compare("foo", "food"));
        assert!(!compare("foo", "fob"));
        // NB: Only ASCII letters fold, and a Latin-1 string compares with a UTF-16 one by code unit.
        assert!(!compare("\u{e9}", "\u{c9}"));
        assert!(compare("\u{e9}\u{3b1}", "\u{e9}\u{3b1}"));
        assert!(compare("A\u{3b1}", "a\u{3b1}"));
        assert!(!compare("\u{3b1}", "\u{391}"));
    }
}
