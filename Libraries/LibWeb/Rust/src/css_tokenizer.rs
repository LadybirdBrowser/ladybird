/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::char;
use std::ops::Range;
use std::ptr;

const REPLACEMENT_CHARACTER: u32 = 0xFFFD;
const TOKENIZER_EOF: u32 = u32::MAX;

// NB: Keep this in sync with Web::CSS::Parser::Token::Type in Token.h.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub enum CssTokenType {
    Invalid,
    EndOfFile,
    Ident,
    Function,
    AtKeyword,
    Hash,
    String,
    BadString,
    Url,
    BadUrl,
    Delim,
    Number,
    Percentage,
    Dimension,
    Whitespace,
    CDO,
    CDC,
    Colon,
    Semicolon,
    Comma,
    OpenSquare,
    CloseSquare,
    OpenParen,
    CloseParen,
    OpenCurly,
    CloseCurly,
}

// NB: Keep this in sync with Web::CSS::Parser::Token::HashType in Token.h.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub enum CssHashType {
    Id,
    Unrestricted,
}

// NB: Keep this in sync with Web::CSS::Number::Type in Number.h.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub enum CssNumberType {
    Number,
    IntegerWithExplicitSign,
    Integer,
}

#[repr(C)]
pub struct CssToken {
    pub token_type: CssTokenType,
    pub hash_type: CssHashType,
    pub number_type: CssNumberType,
    pub number_value: f64,
    pub delim: u32,
    pub value_ptr: *const u8,
    pub value_len: usize,
    pub original_source_ptr: *const u8,
    pub original_source_len: usize,
    pub start_line: usize,
    pub start_column: usize,
    pub end_line: usize,
    pub end_column: usize,
}

#[derive(Clone, Copy, Debug, Default)]
struct Position {
    line: usize,
    column: usize,
}

#[derive(Clone, Copy)]
struct NumericValue {
    number_type: CssNumberType,
    value: f64,
}

pub(crate) struct Token {
    token_type: TokenType,
    original_source_range: Range<usize>,
    range: Range<Position>,
}

enum TokenType {
    EndOfFile,
    Ident { value: String },
    Function { name: String },
    AtKeyword { name: String },
    Hash { hash_type: CssHashType, value: String },
    String { value: String },
    BadString,
    Url { value: String },
    BadUrl,
    Delim { value: u32 },
    Number { number: NumericValue },
    Percentage { number: NumericValue },
    Dimension { number: NumericValue, unit: String },
    Whitespace,
    Cdo,
    Cdc,
    Colon,
    Semicolon,
    Comma,
    OpenSquare,
    CloseSquare,
    OpenParen,
    CloseParen,
    OpenCurly,
    CloseCurly,
}

impl Token {
    fn create(token_type: TokenType, original_source_range: Range<usize>) -> Self {
        Self {
            token_type,
            original_source_range,
            range: Position::default()..Position::default(),
        }
    }

    pub(crate) fn as_ffi(&self, filtered_input: &str) -> CssToken {
        let original_source =
            &filtered_input.as_bytes()[self.original_source_range.start..self.original_source_range.end];
        let (original_source_ptr, original_source_len) = bytes_parts(original_source);

        let mut css_token = CssToken {
            token_type: CssTokenType::Invalid,
            hash_type: CssHashType::Id,
            number_type: CssNumberType::Number,
            number_value: 0.0,
            delim: 0,
            value_ptr: ptr::null(),
            value_len: 0,
            original_source_ptr,
            original_source_len,
            start_line: self.range.start.line,
            start_column: self.range.start.column,
            end_line: self.range.end.line,
            end_column: self.range.end.column,
        };

        match &self.token_type {
            TokenType::EndOfFile => css_token.token_type = CssTokenType::EndOfFile,
            TokenType::Ident { value } => {
                css_token.token_type = CssTokenType::Ident;
                (css_token.value_ptr, css_token.value_len) = string_parts(value);
            }
            TokenType::Function { name } => {
                css_token.token_type = CssTokenType::Function;
                (css_token.value_ptr, css_token.value_len) = string_parts(name);
            }
            TokenType::AtKeyword { name } => {
                css_token.token_type = CssTokenType::AtKeyword;
                (css_token.value_ptr, css_token.value_len) = string_parts(name);
            }
            TokenType::Hash { hash_type, value } => {
                css_token.token_type = CssTokenType::Hash;
                css_token.hash_type = *hash_type;
                (css_token.value_ptr, css_token.value_len) = string_parts(value);
            }
            TokenType::String { value } => {
                css_token.token_type = CssTokenType::String;
                (css_token.value_ptr, css_token.value_len) = string_parts(value);
            }
            TokenType::BadString => css_token.token_type = CssTokenType::BadString,
            TokenType::Url { value } => {
                css_token.token_type = CssTokenType::Url;
                (css_token.value_ptr, css_token.value_len) = string_parts(value);
            }
            TokenType::BadUrl => css_token.token_type = CssTokenType::BadUrl,
            TokenType::Delim { value } => {
                css_token.token_type = CssTokenType::Delim;
                css_token.delim = *value;
            }
            TokenType::Number { number } => {
                css_token.token_type = CssTokenType::Number;
                css_token.number_type = number.number_type;
                css_token.number_value = number.value;
            }
            TokenType::Percentage { number } => {
                css_token.token_type = CssTokenType::Percentage;
                css_token.number_type = number.number_type;
                css_token.number_value = number.value;
            }
            TokenType::Dimension { number, unit } => {
                css_token.token_type = CssTokenType::Dimension;
                css_token.number_type = number.number_type;
                css_token.number_value = number.value;
                (css_token.value_ptr, css_token.value_len) = string_parts(unit);
            }
            TokenType::Whitespace => css_token.token_type = CssTokenType::Whitespace,
            TokenType::Cdo => css_token.token_type = CssTokenType::CDO,
            TokenType::Cdc => css_token.token_type = CssTokenType::CDC,
            TokenType::Colon => css_token.token_type = CssTokenType::Colon,
            TokenType::Semicolon => css_token.token_type = CssTokenType::Semicolon,
            TokenType::Comma => css_token.token_type = CssTokenType::Comma,
            TokenType::OpenSquare => css_token.token_type = CssTokenType::OpenSquare,
            TokenType::CloseSquare => css_token.token_type = CssTokenType::CloseSquare,
            TokenType::OpenParen => css_token.token_type = CssTokenType::OpenParen,
            TokenType::CloseParen => css_token.token_type = CssTokenType::CloseParen,
            TokenType::OpenCurly => css_token.token_type = CssTokenType::OpenCurly,
            TokenType::CloseCurly => css_token.token_type = CssTokenType::CloseCurly,
        }

        css_token
    }
}

pub(crate) struct TokenizationResult {
    pub filtered_input: String,
    pub tokens: Vec<Token>,
}

pub fn tokenize(filtered_input: &[u8]) -> TokenizationResult {
    let filtered_input = std::str::from_utf8(filtered_input)
        .expect("rust_css_tokenize received non-UTF-8 input after C++ decoding")
        .to_owned();
    Tokenizer::new(filtered_input).tokenize()
}

struct Tokenizer {
    input: String,
    code_points: Vec<(usize, u32)>,
    index: usize,
    prev_index: usize,
    position: Position,
    prev_position: Position,
}

impl Tokenizer {
    fn new(input: String) -> Self {
        let code_points = input
            .char_indices()
            .map(|(offset, code_point)| (offset, code_point as u32))
            .collect();

        Self {
            input,
            code_points,
            index: 0,
            prev_index: 0,
            position: Position::default(),
            prev_position: Position::default(),
        }
    }

    fn tokenize(mut self) -> TokenizationResult {
        let mut tokens = Vec::new();

        loop {
            let token_start = self.position;
            let mut token = self.consume_a_token();
            token.range = token_start..self.position;
            let is_eof = matches!(token.token_type, TokenType::EndOfFile);
            tokens.push(token);

            if is_eof {
                return TokenizationResult {
                    filtered_input: self.input,
                    tokens,
                };
            }
        }
    }

    fn current_byte_offset(&self) -> usize {
        if let Some((offset, _)) = self.code_points.get(self.index) {
            *offset
        } else {
            self.input.len()
        }
    }

    fn current_code_point(&self) -> u32 {
        self.code_points[self.prev_index].1
    }

    fn consume_code_point(&mut self) -> u32 {
        if self.index >= self.code_points.len() {
            return TOKENIZER_EOF;
        }

        self.prev_index = self.index;
        self.prev_position = self.position;

        let (_, code_point) = self.code_points[self.index];
        self.index += 1;

        if is_newline(code_point) {
            self.position.line += 1;
            self.position.column = 0;
        } else {
            self.position.column += 1;
        }

        code_point
    }

    fn peek_code_point(&self, offset: usize) -> u32 {
        self.code_points
            .get(self.index + offset)
            .map(|(_, code_point)| *code_point)
            .unwrap_or(TOKENIZER_EOF)
    }

    fn peek_twin(&self) -> (u32, u32) {
        (self.peek_code_point(0), self.peek_code_point(1))
    }

    fn peek_triplet(&self) -> (u32, u32, u32) {
        (
            self.peek_code_point(0),
            self.peek_code_point(1),
            self.peek_code_point(2),
        )
    }

    fn start_of_input_stream_twin(&mut self) -> (u32, u32) {
        (self.current_code_point(), self.peek_code_point(0))
    }

    fn start_of_input_stream_triplet(&mut self) -> (u32, u32, u32) {
        (
            self.current_code_point(),
            self.peek_code_point(0),
            self.peek_code_point(1),
        )
    }

    fn reconsume_current_input_code_point(&mut self) {
        self.index = self.prev_index;
        self.position = self.prev_position;
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-comment
    fn consume_comments(&mut self) {
        // This section describes how to consume comments from a stream of code points.
        // It returns nothing.

        loop {
            // If the next two input code point are U+002F SOLIDUS (/) followed by a U+002A ASTERISK (*),
            // consume them and all following code points up to and including the first U+002A ASTERISK (*)
            // followed by a U+002F SOLIDUS (/), or up to an EOF code point. Return to the start of this step.
            //
            // If the preceding paragraph ended by consuming an EOF code point, this is a parse error.
            //
            // Return nothing.
            let (first, second) = self.peek_twin();
            if !(is_solidus(first) && is_asterisk(second)) {
                return;
            }

            self.consume_code_point();
            self.consume_code_point();

            loop {
                let (first, second) = self.peek_twin();
                if is_eof(first) || is_eof(second) {
                    return;
                }

                if is_asterisk(first) && is_solidus(second) {
                    self.consume_code_point();
                    self.consume_code_point();
                    break;
                }

                self.consume_code_point();
            }
        }
    }

    fn consume_as_much_whitespace_as_possible(&mut self) {
        while is_whitespace(self.peek_code_point(0)) {
            self.consume_code_point();
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-escaped-code-point
    fn consume_escaped_code_point(&mut self) -> u32 {
        // This section describes how to consume an escaped code point.
        // It assumes that the U+005C REVERSE SOLIDUS (\) has already been consumed and that the next
        // input code point has already been verified to be part of a valid escape.
        // It will return a code point.

        // Consume the next input code point.
        let input = self.consume_code_point();

        // hex digit
        if is_hex_digit(input) {
            let mut repr = String::new();
            append_code_point(&mut repr, input);

            // Consume as many hex digits as possible, but no more than 5.
            // Note that this means 1-6 hex digits have been consumed in total.
            let mut counter = 0usize;
            while is_hex_digit(self.peek_code_point(0)) && counter < 5 {
                counter += 1;
                append_code_point(&mut repr, self.consume_code_point());
            }

            // If the next input code point is whitespace, consume it as well.
            if is_whitespace(self.peek_code_point(0)) {
                self.consume_code_point();
            }

            // Interpret the hex digits as a hexadecimal number.
            let unhexed = u32::from_str_radix(&repr, 16).unwrap_or(0);
            // If this number is zero, or is for a surrogate, or is greater than the maximum allowed
            // code point, return U+FFFD REPLACEMENT CHARACTER (�).
            if unhexed == 0 || is_unicode_surrogate(unhexed) || is_greater_than_maximum_allowed_code_point(unhexed) {
                return REPLACEMENT_CHARACTER;
            }

            // Otherwise, return the code point with that value.
            return unhexed;
        }

        // EOF
        if is_eof(input) {
            // This is a parse error. Return U+FFFD REPLACEMENT CHARACTER (�).
            return REPLACEMENT_CHARACTER;
        }

        // anything else
        // Return the current input code point.
        input
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-ident-like-token
    fn consume_an_ident_like_token(&mut self) -> Token {
        // This section describes how to consume an ident-like token from a stream of code points.
        // It returns an <ident-token>, <function-token>, <url-token>, or <bad-url-token>.

        // Consume an ident sequence, and let string be the result.
        let start_byte_offset = self.current_byte_offset();
        let string = self.consume_an_ident_sequence();

        // If string’s value is an ASCII case-insensitive match for "url", and the next input code
        // point is U+0028 LEFT PARENTHESIS ((), consume it.
        if string.eq_ignore_ascii_case("url") && is_left_paren(self.peek_code_point(0)) {
            self.consume_code_point();

            // While the next two input code points are whitespace, consume the next input code point.
            loop {
                let (first, second) = self.peek_twin();
                if !(is_whitespace(first) && is_whitespace(second)) {
                    break;
                }
                self.consume_code_point();
            }

            // If the next one or two input code points are U+0022 QUOTATION MARK ("), U+0027 APOSTROPHE ('),
            // or whitespace followed by U+0022 QUOTATION MARK (") or U+0027 APOSTROPHE ('), then create a
            // <function-token> with its value set to string and return it.
            let (first, second) = self.peek_twin();
            if is_quotation_mark(first)
                || is_apostrophe(first)
                || (is_whitespace(first) && (is_quotation_mark(second) || is_apostrophe(second)))
            {
                return Token::create(
                    TokenType::Function { name: string },
                    start_byte_offset..self.current_byte_offset(),
                );
            }

            // Otherwise, consume a url token, and return it.
            return self.consume_a_url_token(start_byte_offset);
        }

        // Otherwise, if the next input code point is U+0028 LEFT PARENTHESIS ((), consume it.
        if is_left_paren(self.peek_code_point(0)) {
            self.consume_code_point();

            // Create a <function-token> with its value set to string and return it.
            return Token::create(
                TokenType::Function { name: string },
                start_byte_offset..self.current_byte_offset(),
            );
        }

        // Otherwise, create an <ident-token> with its value set to string and return it.
        Token::create(
            TokenType::Ident { value: string },
            start_byte_offset..self.current_byte_offset(),
        )
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-number
    fn consume_a_number(&mut self) -> NumericValue {
        // This section describes how to consume a number from a stream of code points.
        // It returns a numeric value, and a type which is either "integer" or "number".
        //
        // Note: This algorithm does not do the verification of the first few code points
        // that are necessary to ensure a number can be obtained from the stream. Ensure
        // that the stream starts with a number before calling this algorithm.

        // Execute the following steps in order:

        // 1. Initially set type to "integer". Let repr be the empty string.
        let mut repr = String::new();
        let mut number_type = CssNumberType::Integer;

        // 2. If the next input code point is U+002B PLUS SIGN (+) or U+002D HYPHEN-MINUS (-),
        // consume it and append it to repr.
        let mut has_explicit_sign = false;
        let next_input = self.peek_code_point(0);
        if is_plus_sign(next_input) || is_hyphen_minus(next_input) {
            has_explicit_sign = true;
            append_code_point(&mut repr, self.consume_code_point());
        }

        // 3. While the next input code point is a digit, consume it and append it to repr.
        while is_digit(self.peek_code_point(0)) {
            append_code_point(&mut repr, self.consume_code_point());
        }

        // 4. If the next 2 input code points are U+002E FULL STOP (.) followed by a digit, then:
        let (first, second) = self.peek_twin();
        if is_full_stop(first) && is_digit(second) {
            // 1. Consume them.
            // 2. Append them to repr.
            append_code_point(&mut repr, self.consume_code_point());
            append_code_point(&mut repr, self.consume_code_point());

            // 3. Set type to "number".
            number_type = CssNumberType::Number;

            // 4. While the next input code point is a digit, consume it and append it to repr.
            while is_digit(self.peek_code_point(0)) {
                append_code_point(&mut repr, self.consume_code_point());
            }
        }

        // 5. If the next 2 or 3 input code points are U+0045 LATIN CAPITAL LETTER E (E) or
        // U+0065 LATIN SMALL LETTER E (e), optionally followed by U+002D HYPHEN-MINUS (-)
        // or U+002B PLUS SIGN (+), followed by a digit, then:
        let (first, second, third) = self.peek_triplet();
        if (is_e(first) || is_uppercase_e(first))
            && (((is_plus_sign(second) || is_hyphen_minus(second)) && is_digit(third)) || is_digit(second))
        {
            // 1. Consume them.
            // 2. Append them to repr.
            if (is_plus_sign(second) || is_hyphen_minus(second)) && is_digit(third) {
                append_code_point(&mut repr, self.consume_code_point());
                append_code_point(&mut repr, self.consume_code_point());
                append_code_point(&mut repr, self.consume_code_point());
            } else if is_digit(second) {
                append_code_point(&mut repr, self.consume_code_point());
                append_code_point(&mut repr, self.consume_code_point());
            }

            // 3. Set type to "number".
            number_type = CssNumberType::Number;

            // 4. While the next input code point is a digit, consume it and append it to repr.
            while is_digit(self.peek_code_point(0)) {
                append_code_point(&mut repr, self.consume_code_point());
            }
        }

        // 6. Convert repr to a number, and set the value to the returned value.
        let value = repr.parse::<f64>().unwrap();

        // 7. Return value and type.
        if number_type == CssNumberType::Integer && has_explicit_sign {
            return NumericValue {
                number_type: CssNumberType::IntegerWithExplicitSign,
                value,
            };
        }

        NumericValue { number_type, value }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-name
    fn consume_an_ident_sequence(&mut self) -> String {
        // This section describes how to consume an ident sequence from a stream of code points.
        // It returns a string containing the largest name that can be formed from adjacent
        // code points in the stream, starting from the first.
        //
        // Note: This algorithm does not do the verification of the first few code points that
        // are necessary to ensure the returned code points would constitute an <ident-token>.
        // If that is the intended use, ensure that the stream starts with an ident sequence before
        // calling this algorithm.

        // Let result initially be an empty string.
        let mut result = String::new();

        // Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            if is_eof(input) {
                break;
            }

            // name code point
            if is_ident_code_point(input) {
                // Append the code point to result.
                append_code_point(&mut result, input);
                continue;
            }

            // the stream starts with a valid escape
            if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                // Consume an escaped code point. Append the returned code point to result.
                append_code_point(&mut result, self.consume_escaped_code_point());
                continue;
            }

            // anything else
            // Reconsume the current input code point. Return result.
            self.reconsume_current_input_code_point();
            break;
        }

        result
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-url-token
    fn consume_a_url_token(&mut self, start_byte_offset: usize) -> Token {
        // This section describes how to consume a url token from a stream of code points.
        // It returns either a <url-token> or a <bad-url-token>.
        //
        // Note: This algorithm assumes that the initial "url(" has already been consumed.
        // This algorithm also assumes that it’s being called to consume an "unquoted" value,
        // like url(foo). A quoted value, like url("foo"), is parsed as a <function-token>.
        // Consume an ident-like token automatically handles this distinction; this algorithm
        // shouldn’t be called directly otherwise.

        // 1. Initially create a <url-token> with its value set to the empty string.
        let mut value = String::new();

        // 2. Consume as much whitespace as possible.
        self.consume_as_much_whitespace_as_possible();

        // 3. Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            // U+0029 RIGHT PARENTHESIS ())
            if is_right_paren(input) {
                // Return the <url-token>.
                return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
            }

            // EOF
            if is_eof(input) {
                // This is a parse error. Return the <url-token>.
                return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
            }

            // whitespace
            if is_whitespace(input) {
                // Consume as much whitespace as possible.
                self.consume_as_much_whitespace_as_possible();
                let next_input = self.peek_code_point(0);

                // If the next input code point is U+0029 RIGHT PARENTHESIS ()) or EOF, consume it
                // and return the <url-token> (if EOF was encountered, this is a parse error);
                if is_right_paren(next_input) {
                    self.consume_code_point();
                    return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
                }

                if is_eof(next_input) {
                    self.consume_code_point();
                    return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
                }

                // otherwise, consume the remnants of a bad url, create a <bad-url-token>, and return it.
                self.consume_the_remnants_of_a_bad_url();
                return Token::create(TokenType::BadUrl, start_byte_offset..self.current_byte_offset());
            }

            // U+0022 QUOTATION MARK (")
            // U+0027 APOSTROPHE (')
            // U+0028 LEFT PARENTHESIS (()
            // non-printable code point
            if is_quotation_mark(input)
                || is_apostrophe(input)
                || is_left_paren(input)
                || is_non_printable_code_point(input)
            {
                // This is a parse error. Consume the remnants of a bad url, create a <bad-url-token>, and return it.
                self.consume_the_remnants_of_a_bad_url();
                return Token::create(TokenType::BadUrl, start_byte_offset..self.current_byte_offset());
            }

            // U+005C REVERSE SOLIDUS (\)
            if is_reverse_solidus(input) {
                // If the stream starts with a valid escape,
                if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                    // consume an escaped code point and append the returned code point to the <url-token>’s value.
                    append_code_point(&mut value, self.consume_escaped_code_point());
                    continue;
                }

                // Otherwise, this is a parse error.
                // Consume the remnants of a bad url, create a <bad-url-token>, and return it.
                self.consume_the_remnants_of_a_bad_url();
                return Token::create(TokenType::BadUrl, start_byte_offset..self.current_byte_offset());
            }

            // anything else
            // Append the current input code point to the <url-token>’s value.
            append_code_point(&mut value, input);
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-remnants-of-bad-url
    fn consume_the_remnants_of_a_bad_url(&mut self) {
        // This section describes how to consume the remnants of a bad url from a stream of code points,
        // "cleaning up" after the tokenizer realizes that it’s in the middle of a <bad-url-token> rather
        // than a <url-token>. It returns nothing; its sole use is to consume enough of the input stream
        // to reach a recovery point where normal tokenizing can resume.

        // Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            // U+0029 RIGHT PARENTHESIS ())
            // EOF
            if is_eof(input) || is_right_paren(input) {
                // Return.
                return;
            }

            // the input stream starts with a valid escape
            if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                // Consume an escaped code point.
                // This allows an escaped right parenthesis ("\)") to be encountered without ending
                // the <bad-url-token>. This is otherwise identical to the "anything else" clause.
                self.consume_escaped_code_point();
            }

            // anything else
            // Do nothing.
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-numeric-token
    fn consume_a_numeric_token(&mut self) -> Token {
        // This section describes how to consume a numeric token from a stream of code points.
        // It returns either a <number-token>, <percentage-token>, or <dimension-token>.

        let start_byte_offset = self.current_byte_offset();

        // Consume a number and let number be the result.
        let number = self.consume_a_number();

        // If the next 3 input code points would start an ident sequence, then:
        if would_start_an_ident_sequence(self.peek_triplet()) {
            // 1. Create a <dimension-token> with the same value and type flag as number,
            //    and a unit set initially to the empty string.

            // 2. Consume an ident sequence. Set the <dimension-token>’s unit to the returned value.
            let unit = self.consume_an_ident_sequence();

            // 3. Return the <dimension-token>.
            return Token::create(
                TokenType::Dimension { number, unit },
                start_byte_offset..self.current_byte_offset(),
            );
        }

        // Otherwise, if the next input code point is U+0025 PERCENTAGE SIGN (%), consume it.
        if is_percent(self.peek_code_point(0)) {
            self.consume_code_point();

            // Create a <percentage-token> with the same value as number, and return it.
            return Token::create(
                TokenType::Percentage { number },
                start_byte_offset..self.current_byte_offset(),
            );
        }

        // Otherwise, create a <number-token> with the same value and type flag as number, and return it.
        Token::create(
            TokenType::Number { number },
            start_byte_offset..self.current_byte_offset(),
        )
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-string-token
    fn consume_string_token(&mut self, ending_code_point: u32) -> Token {
        // This section describes how to consume a string token from a stream of code points.
        // It returns either a <string-token> or <bad-string-token>.
        //
        // This algorithm may be called with an ending code point, which denotes the code point
        // that ends the string. If an ending code point is not specified, the current input
        // code point is used.

        // Initially create a <string-token> with its value set to the empty string.
        let start_byte_offset = self.current_byte_offset() - 1;
        let mut value = String::new();

        // Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            // ending code point
            if input == ending_code_point {
                // Return the <string-token>.
                return Token::create(
                    TokenType::String { value },
                    start_byte_offset..self.current_byte_offset(),
                );
            }

            // EOF
            if is_eof(input) {
                // This is a parse error. Return the <string-token>.
                return Token::create(
                    TokenType::String { value },
                    start_byte_offset..self.current_byte_offset(),
                );
            }

            // newline
            if is_newline(input) {
                // This is a parse error. Reconsume the current input code point, create a
                // <bad-string-token>, and return it.
                self.reconsume_current_input_code_point();
                return Token::create(TokenType::BadString, start_byte_offset..self.current_byte_offset());
            }

            // U+005C REVERSE SOLIDUS (\)
            if is_reverse_solidus(input) {
                // If the next input code point is EOF, do nothing.
                let next_input = self.peek_code_point(0);
                if is_eof(next_input) {
                    continue;
                }

                // Otherwise, if the next input code point is a newline, consume it.
                if is_newline(next_input) {
                    self.consume_code_point();
                    continue;
                }

                // Otherwise, (the stream starts with a valid escape) consume an escaped code
                // point and append the returned code point to the <string-token>’s value.
                append_code_point(&mut value, self.consume_escaped_code_point());
                continue;
            }

            // anything else
            // Append the current input code point to the <string-token>’s value.
            append_code_point(&mut value, input);
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-token
    fn consume_a_token(&mut self) -> Token {
        // This section describes how to consume a token from a stream of code points.
        // It will return a single token of any type.

        let start_byte_offset = self.current_byte_offset();

        // Consume comments.
        self.consume_comments();

        // AD-HOC: Preserve comments as whitespace tokens, for serializing custom properties.
        let after_comments_byte_offset = self.current_byte_offset();
        if after_comments_byte_offset != start_byte_offset {
            return Token::create(TokenType::Whitespace, start_byte_offset..self.current_byte_offset());
        }

        // Consume the next input code point.
        let input = self.consume_code_point();

        match input {
            // whitespace
            c if is_whitespace(c) => {
                // Consume as much whitespace as possible. Return a <whitespace-token>.
                self.consume_as_much_whitespace_as_possible();
                Token::create(TokenType::Whitespace, start_byte_offset..self.current_byte_offset())
            }

            // U+0022 QUOTATION MARK (")
            0x22 => {
                // Consume a string token and return it.
                self.consume_string_token(input)
            }

            // U+0023 NUMBER SIGN (#)
            0x23 => {
                // If the next input code point is an ident code point or the next two input code points
                // are a valid escape, then:
                if is_ident_code_point(self.peek_code_point(0)) || is_valid_escape_sequence(self.peek_twin()) {
                    // 1. Create a <hash-token>.
                    // 2. If the next 3 input code points would start an ident sequence, set the <hash-token>’s
                    //    type flag to "id".
                    let hash_type = if would_start_an_ident_sequence(self.peek_triplet()) {
                        CssHashType::Id
                    } else {
                        CssHashType::Unrestricted
                    };

                    // 3. Consume an ident sequence, and set the <hash-token>’s value to the returned string.
                    let value = self.consume_an_ident_sequence();

                    // 4. Return the <hash-token>.
                    Token::create(
                        TokenType::Hash { hash_type, value },
                        start_byte_offset..self.current_byte_offset(),
                    )
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+0027 APOSTROPHE (')
            0x27 => {
                // Consume a string token and return it.
                self.consume_string_token(input)
            }

            // U+0028 LEFT PARENTHESIS (()
            0x28 => {
                // Return a <(-token>.
                Token::create(TokenType::OpenParen, start_byte_offset..self.current_byte_offset())
            }

            // U+0029 RIGHT PARENTHESIS ())
            0x29 => {
                // Return a <)-token>.
                Token::create(TokenType::CloseParen, start_byte_offset..self.current_byte_offset())
            }

            // U+002B PLUS SIGN (+)
            0x2B => {
                // If the input stream starts with a number, reconsume the current input code point,
                // consume a numeric token and return it.
                if would_start_a_number(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    self.consume_a_numeric_token()
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+002C COMMA (,)
            0x2C => {
                // Return a <comma-token>.
                Token::create(TokenType::Comma, start_byte_offset..self.current_byte_offset())
            }

            // U+002D HYPHEN-MINUS (-)
            0x2D => {
                // If the input stream starts with a number, reconsume the current input code point,
                // consume a numeric token, and return it.
                if would_start_a_number(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    return self.consume_a_numeric_token();
                }

                // Otherwise, if the next 2 input code points are U+002D HYPHEN-MINUS U+003E
                // GREATER-THAN SIGN (->), consume them and return a <CDC-token>.
                let (first, second) = self.peek_twin();
                if is_hyphen_minus(first) && is_greater_than_sign(second) {
                    self.consume_code_point();
                    self.consume_code_point();
                    return Token::create(TokenType::Cdc, start_byte_offset..self.current_byte_offset());
                }

                // Otherwise, if the input stream starts with an identifier, reconsume the current
                // input code point, consume an ident-like token, and return it.
                if would_start_an_ident_sequence(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    return self.consume_an_ident_like_token();
                }

                // Otherwise, return a <delim-token> with its value set to the current input code point.
                Token::create(
                    TokenType::Delim { value: input },
                    start_byte_offset..self.current_byte_offset(),
                )
            }

            // U+002E FULL STOP (.)
            0x2E => {
                // If the input stream starts with a number, reconsume the current input code point,
                // consume a numeric token, and return it.
                if would_start_a_number(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    self.consume_a_numeric_token()
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+003A COLON (:)
            0x3A => {
                // Return a <colon-token>.
                Token::create(TokenType::Colon, start_byte_offset..self.current_byte_offset())
            }

            // U+003B SEMICOLON (;)
            0x3B => {
                // Return a <semicolon-token>.
                Token::create(TokenType::Semicolon, start_byte_offset..self.current_byte_offset())
            }

            // U+003C LESS-THAN SIGN (<)
            0x3C => {
                // If the next 3 input code points are U+0021 EXCLAMATION MARK U+002D HYPHEN-MINUS
                // U+002D HYPHEN-MINUS (!--), consume them and return a <CDO-token>.
                let (first, second, third) = self.peek_triplet();
                if is_exclamation_mark(first) && is_hyphen_minus(second) && is_hyphen_minus(third) {
                    self.consume_code_point();
                    self.consume_code_point();
                    self.consume_code_point();
                    Token::create(TokenType::Cdo, start_byte_offset..self.current_byte_offset())
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+0040 COMMERCIAL AT (@)
            0x40 => {
                // If the next 3 input code points would start an ident sequence, consume an ident sequence, create
                // an <at-keyword-token> with its value set to the returned value, and return it.
                if would_start_an_ident_sequence(self.peek_triplet()) {
                    let name = self.consume_an_ident_sequence();
                    Token::create(
                        TokenType::AtKeyword { name },
                        start_byte_offset..self.current_byte_offset(),
                    )
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+005B LEFT SQUARE BRACKET ([)
            0x5B => {
                // Return a <[-token>.
                Token::create(TokenType::OpenSquare, start_byte_offset..self.current_byte_offset())
            }

            // U+005C REVERSE SOLIDUS (\)
            0x5C => {
                // If the input stream starts with a valid escape, reconsume the current input code point,
                // consume an ident-like token, and return it.
                if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                    self.reconsume_current_input_code_point();
                    self.consume_an_ident_like_token()
                } else {
                    // Otherwise, this is a parse error. Return a <delim-token> with its value set to the
                    // current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+005D RIGHT SQUARE BRACKET (])
            0x5D => {
                // Return a <]-token>.
                Token::create(TokenType::CloseSquare, start_byte_offset..self.current_byte_offset())
            }

            // U+007B LEFT CURLY BRACKET ({)
            0x7B => {
                // Return a <{-token>.
                Token::create(TokenType::OpenCurly, start_byte_offset..self.current_byte_offset())
            }

            // U+007D RIGHT CURLY BRACKET (})
            0x7D => {
                // Return a <}-token>.
                Token::create(TokenType::CloseCurly, start_byte_offset..self.current_byte_offset())
            }

            // digit
            0x30..=0x39 => {
                // Reconsume the current input code point, consume a numeric token, and return it.
                self.reconsume_current_input_code_point();
                self.consume_a_numeric_token()
            }

            // EOF
            TOKENIZER_EOF => {
                // Return an <EOF-token>.
                Token::create(TokenType::EndOfFile, start_byte_offset..self.current_byte_offset())
            }

            // name-start code point
            c if is_ident_start_code_point(c) => {
                // Reconsume the current input code point, consume an ident-like token, and return it.
                self.reconsume_current_input_code_point();
                self.consume_an_ident_like_token()
            }

            // anything else
            _ => {
                // Return a <delim-token> with its value set to the current input code point.
                Token::create(
                    TokenType::Delim { value: input },
                    start_byte_offset..self.current_byte_offset(),
                )
            }
        }
    }
}

fn string_parts(string: &str) -> (*const u8, usize) {
    bytes_parts(string.as_bytes())
}

fn bytes_parts(bytes: &[u8]) -> (*const u8, usize) {
    if bytes.is_empty() {
        (ptr::null(), 0)
    } else {
        (bytes.as_ptr(), bytes.len())
    }
}

fn append_code_point(builder: &mut String, code_point: u32) {
    builder.push(char::from_u32(code_point).unwrap_or(char::REPLACEMENT_CHARACTER));
}

fn is_eof(code_point: u32) -> bool {
    code_point == TOKENIZER_EOF
}

fn is_ascii(code_point: u32) -> bool {
    code_point <= 0x7F
}

fn is_ascii_alpha(code_point: u32) -> bool {
    (0x41..=0x5A).contains(&code_point) || (0x61..=0x7A).contains(&code_point)
}

fn is_unicode(code_point: u32) -> bool {
    code_point <= 0x10FFFF
}

fn is_unicode_surrogate(code_point: u32) -> bool {
    (0xD800..=0xDFFF).contains(&code_point)
}

fn is_digit(code_point: u32) -> bool {
    (0x30..=0x39).contains(&code_point)
}

fn is_hex_digit(code_point: u32) -> bool {
    is_digit(code_point) || (0x41..=0x46).contains(&code_point) || (0x61..=0x66).contains(&code_point)
}

fn is_ident_start_code_point(code_point: u32) -> bool {
    is_ascii_alpha(code_point) || (!is_ascii(code_point) && is_unicode(code_point)) || code_point == '_' as u32
}

fn is_ident_code_point(code_point: u32) -> bool {
    is_ident_start_code_point(code_point) || is_digit(code_point) || code_point == '-' as u32
}

fn is_non_printable_code_point(code_point: u32) -> bool {
    code_point <= 0x8 || code_point == 0xB || (0xE..=0x1F).contains(&code_point) || code_point == 0x7F
}

fn is_newline(code_point: u32) -> bool {
    code_point == 0x0A
}

fn is_whitespace(code_point: u32) -> bool {
    is_newline(code_point) || code_point == '\t' as u32 || code_point == ' ' as u32
}

fn is_greater_than_maximum_allowed_code_point(code_point: u32) -> bool {
    code_point > 0x10FFFF
}

fn is_quotation_mark(code_point: u32) -> bool {
    code_point == 0x22
}

fn is_hyphen_minus(code_point: u32) -> bool {
    code_point == 0x2D
}

fn is_reverse_solidus(code_point: u32) -> bool {
    code_point == 0x5C
}

fn is_apostrophe(code_point: u32) -> bool {
    code_point == 0x27
}

fn is_left_paren(code_point: u32) -> bool {
    code_point == 0x28
}

fn is_right_paren(code_point: u32) -> bool {
    code_point == 0x29
}

fn is_plus_sign(code_point: u32) -> bool {
    code_point == 0x2B
}

fn is_full_stop(code_point: u32) -> bool {
    code_point == 0x2E
}

fn is_asterisk(code_point: u32) -> bool {
    code_point == 0x2A
}

fn is_solidus(code_point: u32) -> bool {
    code_point == 0x2F
}

fn is_greater_than_sign(code_point: u32) -> bool {
    code_point == 0x3E
}

fn is_percent(code_point: u32) -> bool {
    code_point == 0x25
}

fn is_exclamation_mark(code_point: u32) -> bool {
    code_point == 0x21
}

fn is_e(code_point: u32) -> bool {
    code_point == 0x65
}

fn is_uppercase_e(code_point: u32) -> bool {
    code_point == 0x45
}

// https://www.w3.org/TR/css-syntax-3/#starts-with-a-valid-escape
fn is_valid_escape_sequence((first, second): (u32, u32)) -> bool {
    // This section describes how to check if two code points are a valid escape.
    // The algorithm described here can be called explicitly with two code points,
    // or can be called with the input stream itself. In the latter case, the two
    // code points in question are the current input code point and the next input
    // code point, in that order.
    //
    // Note: This algorithm will not consume any additional code point.

    // If the first code point is not U+005C REVERSE SOLIDUS (\), return false.
    if !is_reverse_solidus(first) {
        return false;
    }

    // Otherwise, if the second code point is a newline, return false.
    if is_newline(second) {
        return false;
    }

    // Otherwise, return true.
    true
}

// https://www.w3.org/TR/css-syntax-3/#would-start-an-identifier
fn would_start_an_ident_sequence((first, second, third): (u32, u32, u32)) -> bool {
    // This section describes how to check if three code points would start an ident sequence.
    // The algorithm described here can be called explicitly with three code points, or
    // can be called with the input stream itself. In the latter case, the three code
    // points in question are the current input code point and the next two input code
    // points, in that order.
    //
    // Note: This algorithm will not consume any additional code points.

    // Look at the first code point:

    // U+002D HYPHEN-MINUS
    if is_hyphen_minus(first) {
        // If the second code point is a name-start code point or a U+002D HYPHEN-MINUS,
        // or the second and third code points are a valid escape, return true.
        if is_ident_start_code_point(second) || is_hyphen_minus(second) || is_valid_escape_sequence((second, third)) {
            return true;
        }
        // Otherwise, return false.
        return false;
    }

    // name-start code point
    if is_ident_start_code_point(first) {
        // Return true.
        return true;
    }

    // U+005C REVERSE SOLIDUS (\)
    if is_reverse_solidus(first) {
        // If the first and second code points are a valid escape, return true.
        if is_valid_escape_sequence((first, second)) {
            return true;
        }
        // Otherwise, return false.
        return false;
    }

    // anything else
    // Return false.
    false
}

// https://www.w3.org/TR/css-syntax-3/#starts-with-a-number
fn would_start_a_number((first, second, third): (u32, u32, u32)) -> bool {
    // This section describes how to check if three code points would start a number.
    // The algorithm described here can be called explicitly with three code points,
    // or can be called with the input stream itself. In the latter case, the three
    // code points in question are the current input code point and the next two input
    // code points, in that order.
    //
    // Note: This algorithm will not consume any additional code points.

    // Look at the first code point:

    // U+002B PLUS SIGN (+)
    // U+002D HYPHEN-MINUS (-)
    if is_plus_sign(first) || is_hyphen_minus(first) {
        // If the second code point is a digit, return true.
        if is_digit(second) {
            return true;
        }

        // Otherwise, if the second code point is a U+002E FULL STOP (.) and the third
        // code point is a digit, return true.
        if is_full_stop(second) && is_digit(third) {
            return true;
        }

        // Otherwise, return false.
        return false;
    }

    // U+002E FULL STOP (.)
    if is_full_stop(first) {
        // If the second code point is a digit, return true. Otherwise, return false.
        return is_digit(second);
    }

    // digit
    if is_digit(first) {
        // Return true.
        return true;
    }

    // anything else
    // Return false.
    false
}
