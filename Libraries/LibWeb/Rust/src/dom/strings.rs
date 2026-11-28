/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The AK strings that the cores hold, as Rust borrows them where C++ keeps them.
//!
//! C++ constructs, assigns and destroys these fields, as the C++ types it sees them as. Rust reads the code units in
//! place, for as long as it borrows a field, and swaps the strings of the fields it writes, see their replace().

use ak::Utf16StringUnits;
use core::cell::UnsafeCell;

super::trace::fields_without_cells!(FlyStringField, OptionalFlyStringField, StringField);

/// A field that holds an interned string, laid out as C++'s AK::Utf16FlyString, which is what C++ sees it as. Two
/// interned strings are equal exactly when their raw identities are.
#[repr(transparent)]
pub struct FlyStringField(UnsafeCell<usize>);

impl FlyStringField {
    /// A field that holds `value`, whose reference passes to it.
    #[inline(always)]
    pub(super) fn from_owned(value: ak::Utf16FlyString) -> Self {
        Self(UnsafeCell::new(value.into_raw()))
    }

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

    /// Puts the string `value` in the field, and answers the string it held.
    ///
    /// # Safety
    ///
    /// The field must be one that holds a live AK::Utf16FlyString, and nothing may borrow its code units.
    #[inline(always)]
    pub(super) unsafe fn replace(&self, value: ak::Utf16FlyString) -> ak::Utf16FlyString {
        // SAFETY: Guaranteed by the caller: the field holds a reference to a live string, which passes to the caller,
        //         and takes over the reference of `value`.
        unsafe { ak::Utf16FlyString::from_raw_owned(core::ptr::replace(self.0.get(), value.into_raw())) }
    }
}

/// A field that holds an interned string or none, laid out as C++'s AK::Optional<AK::Utf16FlyString>, which is what
/// C++ sees it as: the string's raw identity, or 0 for none.
#[repr(transparent)]
pub struct OptionalFlyStringField(UnsafeCell<usize>);

impl OptionalFlyStringField {
    /// A field that holds `value` or none, whose reference passes to it.
    #[inline(always)]
    pub(super) fn from_owned(value: Option<ak::Utf16FlyString>) -> Self {
        Self(UnsafeCell::new(value.map_or(0, ak::Utf16FlyString::into_raw)))
    }

    #[inline(always)]
    pub fn get(&self) -> Option<&FlyStringField> {
        // SAFETY: C++ does not write the field while Rust borrows it.
        let raw = unsafe { *self.0.get() };
        // SAFETY: A field that holds a string has the layout of a FlyStringField.
        (raw != 0).then(|| unsafe { &*(self as *const Self).cast::<FlyStringField>() })
    }

    /// Puts the string `value`, or none, in the field, and answers what it held.
    ///
    /// # Safety
    ///
    /// The field must be one that holds a live AK::Optional<AK::Utf16FlyString>, and nothing may borrow its code units.
    #[inline(always)]
    pub(super) unsafe fn replace(&self, value: Option<ak::Utf16FlyString>) -> Option<ak::Utf16FlyString> {
        let raw = value.map_or(0, ak::Utf16FlyString::into_raw);
        // SAFETY: Guaranteed by the caller: the field holds a reference to a live string or none, which passes to the
        //         caller, and takes over the reference of `value`.
        let old_raw = unsafe { core::ptr::replace(self.0.get(), raw) };
        // SAFETY: As above.
        (old_raw != 0).then(|| unsafe { ak::Utf16FlyString::from_raw_owned(old_raw) })
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

impl StringField {
    /// The field's raw word, which names the string as C++'s AK::Utf16String::raw_identity() does.
    #[inline(always)]
    pub fn raw(&self) -> usize {
        // SAFETY: C++ does not write the field while Rust borrows it.
        unsafe { *self.0.get() }
    }

    /// Stores `value` in the field, and gives back the string it held, whose reference passes to the caller.
    ///
    /// # Safety
    ///
    /// The field must be one C++ holds a live AK::Utf16String in, and nothing may borrow its code units.
    #[inline(always)]
    pub(super) unsafe fn replace(&self, value: ak::Utf16String) -> ak::Utf16String {
        // SAFETY: Guaranteed by the caller: the field holds a reference to a live string, which passes to the caller,
        //         and takes over the reference of `value`.
        unsafe { ak::Utf16String::from_raw_owned(core::ptr::replace(self.0.get(), value.into_raw())) }
    }
}

/// A string C++ lends for a call, named by its raw word, as AK::Utf16String::raw_identity() and
/// AK::Utf16FlyString::raw_identity() name theirs alike.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct BorrowedString(usize);

impl BorrowedString {
    /// # Safety
    ///
    /// `raw` must be the raw word of a live AK::Utf16String or AK::Utf16FlyString, which lives for as long as the
    /// borrowed string is used.
    #[inline(always)]
    pub unsafe fn from_raw(raw: usize) -> Self {
        Self(raw)
    }

    #[inline(always)]
    pub fn raw(self) -> usize {
        self.0
    }

    #[inline(always)]
    pub fn units(&self) -> Utf16StringUnits<'_> {
        // SAFETY: The string lives for as long as it is borrowed, see from_raw().
        unsafe { ak::utf16_string_units(&self.0) }
    }

    /// The string as a reference of its own to it.
    #[inline(always)]
    pub fn to_utf16_string(self) -> ak::Utf16String {
        // SAFETY: The string is live, see from_raw().
        unsafe { ak::Utf16String::from_raw_borrowed(self.0) }
    }
}

/// Whether the raw word of a string holds its ASCII code units in place, as AK keeps a string of fewer than 8 of them.
/// AK keeps every interned string of that kind in place, so two such interned strings are equal exactly when their
/// raw words are.
#[inline(always)]
pub fn is_short_raw(raw: usize) -> bool {
    raw & ak::SHORT_STRING_FLAG != 0
}

/// Whether the strings whose raw words are `a` and `b` have the same code units.
///
/// # Safety
///
/// `a` and `b` must be the raw words of live strings, which live for the call.
#[inline(always)]
pub unsafe fn raw_strings_equal(a: usize, b: usize) -> bool {
    if a == b {
        return true;
    }
    // NB: A string of fewer than 8 ASCII code units that is not kept in place can only be one that AK made from wider
    //     storage, so only two strings kept in place compare by their raw words alone.
    if is_short_raw(a) && is_short_raw(b) {
        return false;
    }
    // SAFETY: Both words name live strings, see the safety section.
    unsafe { units_equal(ak::utf16_string_units(&a), ak::utf16_string_units(&b)) }
}

/// Whether two strings have the same code units, however AK stores them.
#[inline]
pub fn units_equal(a: Utf16StringUnits<'_>, b: Utf16StringUnits<'_>) -> bool {
    use Utf16StringUnits::*;
    match (a, b) {
        (Ascii(a) | Latin1(a), Ascii(b) | Latin1(b)) => a == b,
        (Utf16(a), Utf16(b)) => a == b,
        (Ascii(narrow) | Latin1(narrow), Utf16(wide)) | (Utf16(wide), Ascii(narrow) | Latin1(narrow)) => {
            narrow.len() == wide.len() && narrow.iter().zip(wide).all(|(&n, &w)| u16::from(n) == w)
        }
    }
}

/// Whether `string` has the code units of `ascii`.
#[inline(always)]
pub fn units_equal_ascii(string: Utf16StringUnits<'_>, ascii: &str) -> bool {
    units_equal(string, Utf16StringUnits::Ascii(ascii.as_bytes()))
}

/// Whether `string` starts with the code units of `ascii`.
#[inline]
pub fn units_start_with_ascii(string: Utf16StringUnits<'_>, ascii: &str) -> bool {
    let prefix = ascii.as_bytes();
    match string {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.starts_with(prefix),
        Utf16StringUnits::Utf16(units) => {
            units.len() >= prefix.len() && units.iter().zip(prefix).all(|(&unit, &byte)| unit == u16::from(byte))
        }
    }
}

/// Whether `string` has the code units of `other` in ASCII lowercase.
#[inline]
pub fn units_equal_ascii_lowercased(string: Utf16StringUnits<'_>, other: Utf16StringUnits<'_>) -> bool {
    fn lowercase(code_unit: u16) -> u16 {
        match u8::try_from(code_unit) {
            Ok(byte) => u16::from(byte.to_ascii_lowercase()),
            Err(_) => code_unit,
        }
    }
    let (string_length, string) = code_units(string);
    let (other_length, other) = code_units(other);
    string_length == other_length && string.eq(other.map(lowercase))
}

/// Whether `string` has an ASCII uppercase code unit.
#[inline]
pub fn units_contain_ascii_uppercase(string: Utf16StringUnits<'_>) -> bool {
    match string {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.iter().any(u8::is_ascii_uppercase),
        Utf16StringUnits::Utf16(units) => units
            .iter()
            .any(|&unit| (u16::from(b'A')..=u16::from(b'Z')).contains(&unit)),
    }
}

/// The raw word of the string kept in place whose raw word is `raw`, in ASCII lowercase.
#[inline(always)]
pub fn ascii_lowercase_short_raw(raw: usize) -> usize {
    debug_assert!(is_short_raw(raw));
    // NB: Every byte of the word is below 0x80: the tag byte, which holds the length and the flag and is never an ASCII
    //     uppercase letter, the ASCII code units, and the zero bytes after them. So adding to each byte carries into
    //     no other, and a byte's high bit after adding says whether it is at least the letter the addend makes 0x80.
    const ONES: usize = usize::MAX / 0xff;
    let at_least_upper_a = raw + ONES * (0x80 - b'A' as usize);
    let above_upper_z = raw + ONES * (0x80 - b'Z' as usize - 1);
    let is_upper = at_least_upper_a & !above_upper_z & (ONES * 0x80);
    raw | (is_upper >> 2)
}

/// The hash AK gives a string, as AK::Utf16String::hash() works it out: AK::string_hash() of its code units, which a
/// long string caches in its header once it is worked out.
///
/// # Safety
///
/// `raw` must be the raw word of a live string, which lives for the call.
pub unsafe fn string_hash(raw: usize) -> u32 {
    if raw != 0 && !is_short_raw(raw) {
        // SAFETY: The string is live, see the safety section, and its storage starts with its header.
        let header = unsafe { &*core::ptr::with_exposed_provenance::<ak::Utf16StringDataHeader>(raw) };
        let hash = header.hash.load(core::sync::atomic::Ordering::Relaxed);
        if hash != 0 {
            return hash;
        }
    }
    // SAFETY: The string is live, see the safety section.
    units_hash(&unsafe { ak::utf16_string_units(&raw) })
}

/// The hash AK gives a string with the code units `units`, as AK::Utf16View::hash() works it out.
pub fn units_hash(units: &Utf16StringUnits<'_>) -> u32 {
    let units = match units {
        Utf16StringUnits::Ascii(units) => Utf16StringUnits::Ascii(units),
        Utf16StringUnits::Latin1(units) => Utf16StringUnits::Latin1(units),
        Utf16StringUnits::Utf16(units) => Utf16StringUnits::Utf16(units),
    };
    let (length, units) = code_units(units);
    if length == 0 {
        return 0;
    }
    let mut hash: u32 = 0;
    for unit in units {
        hash = hash.wrapping_add(u32::from(unit));
        hash = hash.wrapping_add(hash << 10);
        hash ^= hash >> 6;
    }
    hash = hash.wrapping_add(hash << 3);
    hash ^= hash >> 11;
    hash.wrapping_add(hash << 15)
}

/// The number of code units of a string, and the code units.
#[inline(always)]
fn code_units(units: Utf16StringUnits<'_>) -> (usize, impl Iterator<Item = u16> + '_) {
    let (narrow, wide): (&[u8], &[u16]) = match units {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => (units, &[]),
        Utf16StringUnits::Utf16(units) => (&[], units),
    };
    let code_units = narrow.iter().copied().map(u16::from).chain(wide.iter().copied());
    (narrow.len() + wide.len(), code_units)
}

#[inline(always)]
pub(super) fn units_are_empty(units: Utf16StringUnits<'_>) -> bool {
    match units {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.is_empty(),
        Utf16StringUnits::Utf16(units) => units.is_empty(),
    }
}

/// Whether two strings are equal when ASCII letters compare without their case, as AK's equals_ignoring_ascii_case()
/// has it.
pub fn equals_ignoring_ascii_case(a: Utf16StringUnits<'_>, b: Utf16StringUnits<'_>) -> bool {
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

    #[test]
    fn short_strings_are_lowercased_in_place() {
        for (string, lowercase) in [("ID", "id"), ("aZ@[`{", "az@[`{"), ("FooBarB", "foobarb")] {
            let raw = ak::Utf16String::from_ascii(string.as_bytes()).into_raw();
            let expected = ak::Utf16String::from_ascii(lowercase.as_bytes()).into_raw();
            assert!(is_short_raw(raw));
            assert_eq!(ascii_lowercase_short_raw(raw), expected);
        }
    }

    #[test]
    fn strings_compare_however_they_are_stored() {
        let short = ak::Utf16String::from_ascii(b"abc");
        let wide = ak::Utf16String::from_utf16(&[0x100, 0x61]);
        let same_wide = ak::Utf16String::from_utf16(&[0x100, 0x61]);
        // SAFETY: The strings live for the test.
        unsafe {
            assert!(raw_strings_equal(short.raw_identity(), short.raw_identity()));
            assert!(raw_strings_equal(wide.raw_identity(), same_wide.raw_identity()));
            assert!(!raw_strings_equal(short.raw_identity(), wide.raw_identity()));
        }
        assert!(units_equal_ascii_lowercased(
            Utf16StringUnits::Ascii(b"aria-label"),
            ak::Utf16String::from_ascii(b"ARIA-Label").as_units()
        ));
    }

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
