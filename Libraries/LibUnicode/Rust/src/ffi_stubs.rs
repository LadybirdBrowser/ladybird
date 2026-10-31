/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Standalone implementations of the `unicode_*` FFI callbacks that
//! `character_types` normally resolves against LibUnicode's C++ (and thus ICU)
//! implementation.

/// Returns the single `char` an iterator yields, or `None` if it yields any other number of them.
fn single_char(mut chars: impl Iterator<Item = char>) -> Option<u32> {
    let first = chars.next()?;
    chars.next().is_none().then_some(first as u32)
}

/// Simple enough casefolding to make regex happy.
fn fold(code_point: u32) -> u32 {
    match code_point {
        0x017F => 's' as u32, // LATIN SMALL LETTER LONG S
        0x0345 => 0x03B9,     // COMBINING GREEK YPOGEGRAMMENI
        0x03C2 => 0x03C3,     // GREEK SMALL LETTER FINAL SIGMA
        0x1E9E => 0x00DF,     // LATIN CAPITAL LETTER SHARP S
        _ => char::from_u32(code_point)
            .and_then(|c| single_char(c.to_lowercase()))
            .unwrap_or(code_point),
    }
}

fn extra_case_equivalents(canonical: u32) -> &'static [u32] {
    match canonical {
        0x73 => &[0x017F], // s
        0x6B => &[0x212A], // k
        0x00DF => &[0x1E9E],
        0x03B9 => &[0x0345, 0x1FBE],
        0x03C3 => &[0x03C2],
        _ => &[],
    }
}

fn case_closure(code_point: u32) -> Vec<u32> {
    let canonical = fold(code_point);

    let mut closure = vec![code_point, canonical];
    closure.extend(extra_case_equivalents(canonical));
    if let Some(uppercased) = char::from_u32(canonical).and_then(|c| single_char(c.to_uppercase())) {
        closure.push(uppercased);
    }

    closure.retain(|candidate| fold(*candidate) == canonical);
    closure.sort_unstable();
    closure.dedup();
    closure
}

// 22.2.2.7.3 Canonicalize ( rer, ch ), https://tc39.es/ecma262/#sec-runtime-semantics-canonicalize-ch
fn canonicalize(code_point: u32, unicode_mode: bool) -> u32 {
    if unicode_mode {
        return fold(code_point);
    }

    if code_point < 128 {
        return u32::from((code_point as u8).to_ascii_uppercase());
    }

    let Some(uppercased) = char::from_u32(code_point).and_then(|c| single_char(c.to_uppercase())) else {
        return code_point;
    };

    if uppercased < 128 { code_point } else { uppercased }
}

#[unsafe(no_mangle)]
extern "C" fn unicode_simple_case_fold(code_point: u32, unicode_mode: bool) -> u32 {
    canonicalize(code_point, unicode_mode)
}

#[unsafe(no_mangle)]
extern "C" fn unicode_code_point_matches_range_ignoring_case(
    code_point: u32,
    from: u32,
    to: u32,
    unicode_mode: bool,
) -> bool {
    if code_point >= from && code_point <= to {
        return true;
    }

    let canonical = canonicalize(code_point, unicode_mode);
    case_closure(code_point)
        .into_iter()
        .filter(|candidate| *candidate >= from && *candidate <= to)
        .any(|candidate| canonicalize(candidate, unicode_mode) == canonical)
}

#[unsafe(no_mangle)]
unsafe extern "C" fn unicode_get_case_closure(code_point: u32, out_buffer: *mut u32, buffer_capacity: u32) -> u32 {
    let closure = case_closure(code_point);
    let count = closure.len().min(buffer_capacity as usize);

    for (index, candidate) in closure.into_iter().take(count).enumerate() {
        // SAFETY: The caller guarantees `out_buffer` is writable for `buffer_capacity` elements.
        unsafe { out_buffer.add(index).write(candidate) };
    }

    count as u32
}

#[unsafe(no_mangle)]
extern "C" fn unicode_code_point_has_space_separator_general_category(code_point: u32) -> bool {
    // General_Category=Zs.
    matches!(
        code_point,
        0x20 | 0xA0 | 0x1680 | 0x2000..=0x200A | 0x202F | 0x205F | 0x3000
    )
}

#[unsafe(no_mangle)]
extern "C" fn unicode_code_point_has_identifier_start_property(code_point: u32) -> bool {
    char::from_u32(code_point).is_some_and(|c| c.is_alphabetic() || c == '$' || c == '_')
}

#[unsafe(no_mangle)]
extern "C" fn unicode_code_point_has_identifier_continue_property(code_point: u32) -> bool {
    char::from_u32(code_point).is_some_and(|c| c.is_alphanumeric() || c == '$' || c == '_')
}

#[unsafe(no_mangle)]
extern "C" fn unicode_property_matches(_: u32, _: *const u8, _: usize, _: *const u8, _: usize) -> bool {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[unsafe(no_mangle)]
extern "C" fn unicode_property_matches_case_insensitive(
    _: u32,
    _: *const u8,
    _: usize,
    _: *const u8,
    _: usize,
) -> bool {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[unsafe(no_mangle)]
extern "C" fn unicode_property_all_case_equivalents_match(
    _: u32,
    _: *const u8,
    _: usize,
    _: *const u8,
    _: usize,
) -> bool {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[unsafe(no_mangle)]
extern "C" fn unicode_resolve_property(
    _: *const u8,
    _: usize,
    _: *const u8,
    _: usize,
    _: *mut u8,
    _: *mut u32,
) -> bool {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[unsafe(no_mangle)]
extern "C" fn unicode_resolved_property_matches(_: u32, _: u8, _: u32) -> bool {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[unsafe(no_mangle)]
extern "C" fn unicode_is_string_property(_: *const u8, _: usize) -> bool {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[unsafe(no_mangle)]
extern "C" fn unicode_is_valid_ecma262_property(_: *const u8, _: usize, _: *const u8, _: usize) -> bool {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[unsafe(no_mangle)]
extern "C" fn unicode_get_string_property_data(_: *const u8, _: usize, _: *mut u32, _: u32) -> u32 {
    unimplemented!("Unicode property lookups are unavailable in Rust-only test binaries")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn folding_matches_the_cases_the_regex_tests_rely_on() {
        assert_eq!(fold(u32::from(b'A')), u32::from(b'a'));
        assert_eq!(fold(0x212A), 'k' as u32); // KELVIN SIGN
        assert_eq!(fold(0x017F), 's' as u32); // LATIN SMALL LETTER LONG S
        assert_eq!(fold(0x0130), 0x0130); // LATIN CAPITAL LETTER I WITH DOT ABOVE

        // Non-unicode mode canonicalizes by uppercasing, but never out of the non-ASCII range and into ASCII.
        assert_eq!(canonicalize('a' as u32, false), 'A' as u32);
        assert_eq!(canonicalize(0x212A, false), 0x212A);
        assert_eq!(canonicalize(0x017F, false), 0x017F);
    }

    #[test]
    fn case_closures_include_the_compatibility_code_points() {
        assert_eq!(case_closure('k' as u32), vec!['K' as u32, 'k' as u32, 0x212A]);
        assert_eq!(case_closure(0x212A), vec!['K' as u32, 'k' as u32, 0x212A]);
        assert_eq!(case_closure('s' as u32), vec!['S' as u32, 's' as u32, 0x017F]);
        assert!(unicode_code_point_matches_range_ignoring_case(
            0x212A, 'a' as u32, 'z' as u32, true
        ));
        assert!(!unicode_code_point_matches_range_ignoring_case(
            0x212A, 'a' as u32, 'z' as u32, false
        ));
    }
}
