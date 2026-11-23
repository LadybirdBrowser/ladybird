/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A borrowed string of code units, as `AK::Utf16View` is in C++.

/// A borrowed string of code units, in either of the two forms `AK::Utf16String` stores them in: one byte per code
/// unit, all of them below U+0100, or UTF-16.
#[derive(Clone, Copy, Debug)]
pub enum Utf16View<'a> {
    Latin1(&'a [u8]),
    Utf16(&'a [u16]),
}

impl PartialEq for Utf16View<'_> {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Latin1(a), Self::Latin1(b)) => a == b,
            (Self::Utf16(a), Self::Utf16(b)) => a == b,
            (Self::Latin1(narrow), Self::Utf16(wide)) | (Self::Utf16(wide), Self::Latin1(narrow)) => {
                narrow.len() == wide.len() && narrow.iter().zip(wide.iter()).all(|(&a, &b)| u16::from(a) == b)
            }
        }
    }
}

impl PartialEq<str> for Utf16View<'_> {
    fn eq(&self, other: &str) -> bool {
        self.code_units().eq(other.encode_utf16())
    }
}

impl PartialEq<&str> for Utf16View<'_> {
    fn eq(&self, other: &&str) -> bool {
        *self == **other
    }
}

impl<'a> Utf16View<'a> {
    pub fn code_units(self) -> impl Iterator<Item = u16> + 'a {
        let (narrow, wide): (&[u8], &[u16]) = match self {
            Self::Latin1(code_units) => (code_units, &[]),
            Self::Utf16(code_units) => (&[], code_units),
        };
        narrow.iter().copied().map(u16::from).chain(wide.iter().copied())
    }

    /// The code units as ASCII bytes, if that is all they are.
    pub fn as_ascii(self) -> Option<&'a [u8]> {
        match self {
            Self::Latin1(code_units) if code_units.is_ascii() => Some(code_units),
            _ => None,
        }
    }

    pub fn eq_ignore_ascii_case(self, other: &str) -> bool {
        self.len() == other.len() && self.starts_with_ignore_ascii_case(other)
    }

    /// Whether this starts with `prefix`, which must be ASCII, in any mix of ASCII upper and lower case.
    pub fn starts_with_ignore_ascii_case(self, prefix: &str) -> bool {
        debug_assert!(prefix.is_ascii());
        self.len() >= prefix.len()
            && self.code_units().zip(prefix.bytes()).all(|(code_unit, byte)| {
                u8::try_from(code_unit).is_ok_and(|code_unit| code_unit.eq_ignore_ascii_case(&byte))
            })
    }

    pub fn len(&self) -> usize {
        match self {
            Self::Latin1(code_units) => code_units.len(),
            Self::Utf16(code_units) => code_units.len(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn to_utf16(&self) -> Vec<u16> {
        let mut code_units = Vec::with_capacity(self.len());
        self.append_to(&mut code_units);
        code_units
    }

    pub fn append_to(&self, output: &mut Vec<u16>) {
        match self {
            Self::Latin1(code_units) => output.extend(code_units.iter().copied().map(u16::from)),
            Self::Utf16(code_units) => output.extend_from_slice(code_units),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn code_units(input: &str) -> Vec<u16> {
        input.encode_utf16().collect()
    }

    #[test]
    fn views_compare_by_code_unit_whatever_form_they_are_in() {
        let wide = code_units("caf\u{e9}");
        let narrow = Utf16View::Latin1(b"caf\xe9");
        let wide = Utf16View::Utf16(&wide);
        assert_eq!(narrow, wide);
        assert_eq!(narrow, "caf\u{e9}");
        assert_ne!(narrow, "cafe");
        assert_ne!(wide, "caf");
        assert_eq!(wide.to_utf16(), code_units("caf\u{e9}"));
        assert_eq!(narrow.len(), 4);
    }

    #[test]
    fn views_compare_with_ascii_in_any_case() {
        assert!(Utf16View::Latin1(b"Text/HTML").eq_ignore_ascii_case("text/html"));
        assert!(!Utf16View::Latin1(b"Text/HTML5").eq_ignore_ascii_case("text/html"));
        assert!(Utf16View::Utf16(&code_units("-//W3C//dtd html")).starts_with_ignore_ascii_case("-//w3c//DTD"));
        assert!(!Utf16View::Latin1(b"-//W3").starts_with_ignore_ascii_case("-//w3c//DTD"));
        assert!(!Utf16View::Utf16(&code_units("\u{212a}")).eq_ignore_ascii_case("k"));
    }

    #[test]
    fn only_one_byte_ascii_views_are_ascii_bytes() {
        assert_eq!(Utf16View::Latin1(b"div").as_ascii(), Some(b"div" as &[u8]));
        assert_eq!(Utf16View::Latin1(b"d\xefv").as_ascii(), None);
        assert_eq!(Utf16View::Utf16(&code_units("div")).as_ascii(), None);
    }
}
