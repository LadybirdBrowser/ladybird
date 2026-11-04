/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::GenericLexer;

// https://fetch.spec.whatwg.org/#http-tab-or-space
// An HTTP tab or space is U+0009 TAB or U+0020 SPACE.
pub fn is_http_tab_or_space(code_point: impl Into<u32>) -> bool {
    matches!(code_point.into(), 0x09 | 0x20)
}

// https://fetch.spec.whatwg.org/#http-whitespace
// HTTP whitespace is U+000A LF, U+000D CR, or an HTTP tab or space.
pub fn is_http_whitespace(code_point: impl Into<u32>) -> bool {
    let code_point = code_point.into();
    matches!(code_point, 0x0A | 0x0D) || is_http_tab_or_space(code_point)
}

// https://mimesniff.spec.whatwg.org/#http-token-code-point
// An HTTP token code point is U+0021 (!), U+0023 (#), U+0024 ($), U+0025 (%), U+0026 (&), U+0027 ('), U+002A (*),
// U+002B (+), U+002D (-), U+002E (.), U+005E (^), U+005F (_), U+0060 (`), U+007C (|), U+007E (~), or an ASCII alphanumeric.
pub fn is_http_token_code_point(code_point: impl Into<u32>) -> bool {
    char::from_u32(code_point.into()).is_some_and(|code_point| {
        matches!(
            code_point,
            '!' | '#' | '$' | '%' | '&' | '\'' | '*' | '+' | '-' | '.' | '^' | '_' | '`' | '|' | '~'
        ) || code_point.is_ascii_alphanumeric()
    })
}

pub struct HttpQuotedString<T> {
    pub value: Vec<T>,
    pub closed: bool,
}

// https://fetch.spec.whatwg.org/#collect-an-http-quoted-string
pub fn collect_an_http_quoted_string_extracting_the_value<T: Copy + PartialEq + From<u8>>(
    lexer: &mut GenericLexer<'_, T>,
) -> HttpQuotedString<T> {
    let quote = T::from(b'"');
    let backslash = T::from(b'\\');

    // 1. Let positionStart be position.
    // NB: Skipped, as every caller uses the extract-value flag.

    // 2. Let value be the empty string.
    let mut value = Vec::new();
    let mut closed = false;

    // 3. Assert: the code point at position within input is U+0022 (").
    // 4. Advance position by 1.
    assert!(lexer.consume_specific(quote));

    // 5. While true:
    loop {
        // 1. Append the result of collecting a sequence of code points that are not U+0022 (") or U+005C (\) from
        //    input, given position, to value.
        value.extend_from_slice(lexer.consume_while(|code_point| code_point != quote && code_point != backslash));

        // 2. If position is past the end of input, then break.
        // 3. Let quoteOrBackslash be the code point at position within input.
        // 4. Advance position by 1.
        let Some(quote_or_backslash) = lexer.consume() else {
            break;
        };

        // 5. If quoteOrBackslash is U+005C (\), then:
        if quote_or_backslash == backslash {
            // 1. If position is past the end of input, then append U+005C (\) to value and break.
            // 2. Append the code point at position within input to value.
            // 3. Advance position by 1.
            let Some(code_point) = lexer.consume() else {
                value.push(backslash);
                break;
            };
            value.push(code_point);
        }
        // 6. Otherwise:
        else {
            // 1. Assert: quoteOrBackslash is U+0022 (").
            assert!(quote_or_backslash == quote);

            // 2. Break.
            closed = true;
            break;
        }
    }

    // 6. If the extract-value flag is set, then return value.
    HttpQuotedString { value, closed }
}
