/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Writes control characters other than a newline or a tab as escapes (\x1b). What a page logs also goes to the
/// terminal Ladybird runs in, and written as they are, they would take effect there, e.g. clearing the screen.
pub fn escape_for_terminal(text: &str) -> String {
    let mut escaped = String::with_capacity(text.len());
    for character in text.chars() {
        let code_point = u32::from(character);
        let is_control =
            (code_point < 0x20 && character != '\n' && character != '\t') || (0x7f..0xa0).contains(&code_point);
        if is_control {
            escaped.push_str(&format!("\\x{code_point:02x}"));
        } else {
            escaped.push(character);
        }
    }
    escaped
}

#[cfg(test)]
mod tests {
    use super::escape_for_terminal;

    #[test]
    fn control_characters_come_out_as_escapes() {
        assert_eq!(escape_for_terminal("a\u{1b}[2Jb"), "a\\x1b[2Jb");
        assert_eq!(escape_for_terminal("\r\u{7f}\u{9b}"), "\\x0d\\x7f\\x9b");
    }

    #[test]
    fn newlines_tabs_and_other_text_pass_through() {
        assert_eq!(
            escape_for_terminal("one\ntwo\tthree \u{a0}\u{e9}\u{1f600}"),
            "one\ntwo\tthree \u{a0}\u{e9}\u{1f600}"
        );
    }
}
