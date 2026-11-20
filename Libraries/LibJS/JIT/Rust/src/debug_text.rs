/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text of a debug dump or of an error that a verifier found, with its bytes in a zone.

use core::fmt;
use core::ops::Deref;

use ak::Zone;
use ak::ZoneVec;

/// Text for a person to read: a debug dump, or what a verifier found. It grows in a zone (see `ak::Zone`), so it makes
/// no block of the heap.
///
/// NB: This is not a string of the engine, and no string of a script is kept in one. It is UTF-8 because its text
///     comes from the formatting of Rust (`write!`, `Display`, `Debug`), which gives UTF-8, and goes to the terminal.
///
/// `write!` and `writeln!` add formatted text to it and give no result to check, because adding to it cannot fail:
///
/// ```ignore
/// let mut text = DebugText::new_in(&zone);
/// write!(text, "v{} = {}", 1, "Add");
/// ```
pub struct DebugText<'zone> {
    // NB: The bytes are UTF-8 at all times: text goes in as whole `str`s and `char`s, and comes out at the
    //     boundaries of characters only.
    bytes: ZoneVec<'zone, u8>,
}

impl<'zone> DebugText<'zone> {
    /// A text with no characters, which takes no memory of the zone before it gets one.
    pub fn new_in(zone: &'zone Zone) -> Self {
        Self {
            bytes: ZoneVec::new_in(zone),
        }
    }

    /// A text with no characters and room for `capacity` bytes.
    pub fn with_capacity_in(capacity: usize, zone: &'zone Zone) -> Self {
        Self {
            bytes: ZoneVec::with_capacity_in(capacity, zone),
        }
    }

    /// A copy of `text`.
    pub fn from_str_in(text: &str, zone: &'zone Zone) -> Self {
        Self {
            bytes: ZoneVec::from_slice_in(text.as_bytes(), zone),
        }
    }

    pub fn zone(&self) -> &'zone Zone {
        self.bytes.zone()
    }

    /// The length in bytes.
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    pub fn as_str(&self) -> &str {
        // SAFETY: The bytes are UTF-8 (see the field).
        unsafe { core::str::from_utf8_unchecked(&self.bytes) }
    }

    /// The text as it is now, for as long as the zone holds it. This copies nothing.
    pub fn into_str(self) -> &'zone str {
        // SAFETY: The bytes are UTF-8 (see the field).
        unsafe { core::str::from_utf8_unchecked(self.bytes.into_slice()) }
    }

    pub fn push(&mut self, character: char) {
        self.push_str(character.encode_utf8(&mut [0; 4]));
    }

    pub fn push_str(&mut self, text: &str) {
        self.bytes.extend_from_slice(text.as_bytes());
    }

    /// Adds formatted text. This is what `write!` and `writeln!` call.
    pub fn write_fmt(&mut self, arguments: fmt::Arguments<'_>) {
        // NB: `write_str()` does not fail, so this fails only if a `Display` or `Debug` implementation reports an
        //     error of its own, which is a bug of that implementation, as it is for `format!`.
        fmt::Write::write_fmt(self, arguments).expect("a formatting trait implementation returned an error");
    }

    /// Removes the last character and returns it.
    pub fn pop(&mut self) -> Option<char> {
        let character = self.as_str().chars().next_back()?;
        self.bytes.truncate(self.len() - character.len_utf8());
        Some(character)
    }

    /// Keeps the first `len` bytes, which must end at the boundary of a character. Does nothing if the text is not
    /// longer than that.
    pub fn truncate(&mut self, len: usize) {
        if len < self.len() {
            assert!(
                self.as_str().is_char_boundary(len),
                "a text ends at the boundary of a character"
            );
            self.bytes.truncate(len);
        }
    }

    /// Removes all characters and keeps the memory.
    pub fn clear(&mut self) {
        self.bytes.clear();
    }
}

impl fmt::Write for DebugText<'_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        self.push_str(text);
        Ok(())
    }

    fn write_char(&mut self, character: char) -> fmt::Result {
        self.push(character);
        Ok(())
    }
}

impl Deref for DebugText<'_> {
    type Target = str;

    fn deref(&self) -> &str {
        self.as_str()
    }
}

impl AsRef<str> for DebugText<'_> {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}

impl fmt::Display for DebugText<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self.as_str(), formatter)
    }
}

impl fmt::Debug for DebugText<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self.as_str(), formatter)
    }
}

impl PartialEq for DebugText<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.as_str() == other.as_str()
    }
}

impl Eq for DebugText<'_> {}

impl PartialEq<str> for DebugText<'_> {
    fn eq(&self, other: &str) -> bool {
        self.as_str() == other
    }
}

impl PartialEq<&str> for DebugText<'_> {
    fn eq(&self, other: &&str) -> bool {
        self.as_str() == *other
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_grows_by_characters_strings_and_formatted_values() {
        let zone = Zone::new(1 << 20, c"test");
        let mut text = DebugText::new_in(&zone);
        assert!(text.is_empty());
        assert_eq!(zone.used(), 0);
        text.push('v');
        text.push_str("1 = ");
        let name = "Add";
        write!(text, "{name} ({:>4}, {:#x})", 7, 255);
        writeln!(text);
        assert_eq!(text, "v1 = Add (   7, 0xff)\n");
        assert_eq!(text.len(), 22);
        assert_eq!(text.lines().next(), Some("v1 = Add (   7, 0xff)"));
        assert_eq!(
            format!("{text}{text:?}"),
            "v1 = Add (   7, 0xff)\n\"v1 = Add (   7, 0xff)\\n\""
        );
    }

    #[test]
    fn text_ends_at_the_boundaries_of_characters() {
        let zone = Zone::new(1 << 20, c"test");
        let mut text = DebugText::from_str_in("aä€😀", &zone);
        assert_eq!(text.len(), 10);
        assert_eq!(text.pop(), Some('😀'));
        assert_eq!(text.pop(), Some('€'));
        assert_eq!(text, "aä");
        text.truncate(8);
        assert_eq!(text, "aä");
        text.truncate(1);
        assert_eq!(text, "a");
        text.clear();
        assert_eq!(text.pop(), None);
        assert_eq!(text, "");
    }

    #[test]
    #[should_panic(expected = "boundary of a character")]
    fn text_cannot_end_inside_a_character() {
        let zone = Zone::new(1 << 20, c"test");
        let mut text = DebugText::from_str_in("ä", &zone);
        text.truncate(1);
    }

    #[test]
    fn text_keeps_its_bytes_when_it_moves_in_the_zone() {
        let zone = Zone::new(1 << 20, c"test");
        let mut text = DebugText::with_capacity_in(4, &zone);
        text.push_str("abcd");
        // NB: Something else is now the last thing that the zone gave out, so the text must move to grow.
        let other = zone.alloc(7u64);
        for index in 0..100 {
            write!(text, "{index},");
        }
        assert_eq!(*other, 7);
        assert!(text.starts_with("abcd0,1,2,"));
        assert!(text.ends_with(",98,99,"));
        let frozen: &str = text.into_str();
        assert_eq!(frozen.len(), 4 + 2 * 10 + 3 * 90);
    }
}
