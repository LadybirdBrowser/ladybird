/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::fmt;

use libunicode_rust::character_types::code_point_has_identifier_continue_property;
use libunicode_rust::character_types::code_point_has_identifier_start_property;

use crate::pattern::ErrorInfo;
use crate::pattern::PatternErrorOr;

// https://urlpattern.spec.whatwg.org/#token
// A token is a struct representing a single lexical token within a pattern string.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Token {
    // https://urlpattern.spec.whatwg.org/#token-type
    // A token has an associated type, a string, initially "invalid-char".
    pub r#type: Type,

    // https://urlpattern.spec.whatwg.org/#token-index
    // A token has an associated index, a number, initially 0. It is the position of the first code point in the pattern string represented by the token.
    pub index: u32,

    // https://urlpattern.spec.whatwg.org/#token-value
    // A token has an associated value, a string, initially the empty string. It contains the code points from the pattern string represented by the token.
    pub value: String,
}

// https://urlpattern.spec.whatwg.org/#token-type
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Type {
    // The token represents a U+007B ({) code point.
    Open,

    // The token represents a U+007D (}) code point.
    Close,

    // The token represents a string of the form "(<regular expression>)". The regular expression is required to consist of only ASCII code points.
    Regexp,

    // The token represents a string of the form ":<name>". The name value is restricted to code points that are consistent with JavaScript identifiers.
    Name,

    // The token represents a valid pattern code point without any special syntactical meaning.
    Char,

    // The token represents a code point escaped using a backslash like "\<char>".
    EscapedChar,

    // The token represents a matching group modifier that is either the U+003F (?) or U+002B (+) code points.
    OtherModifier,

    // The token represents a U+002A (*) code point that can be either a wildcard matching group or a matching group modifier.
    Asterisk,

    // The token represents the end of the pattern string.
    End,

    // The token represents a code point that is invalid in the pattern. This could be because of the code point value
    // itself or due to its location within the pattern relative to other syntactic elements.
    InvalidChar,
}

impl Token {
    pub fn type_to_string(r#type: Type) -> &'static str {
        match r#type {
            Type::Open => "Open",
            Type::Close => "Close",
            Type::Regexp => "Regexp",
            Type::Name => "Name",
            Type::Char => "Char",
            Type::EscapedChar => "EscapedChar",
            Type::OtherModifier => "OtherModifier",
            Type::Asterisk => "Asterisk",
            Type::End => "End",
            Type::InvalidChar => "InvalidChar",
        }
    }
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}, index: {}, value: '{}'",
            Self::type_to_string(self.r#type),
            self.index,
            self.value
        )
    }
}

// https://urlpattern.spec.whatwg.org/#tokenizer
// A tokenizer is a struct.
#[derive(Clone, Debug)]
pub struct Tokenizer {
    // https://urlpattern.spec.whatwg.org/#tokenizer-input
    // A tokenizer has an associated input, a pattern string, initially the empty string.
    pub input: String,

    // https://urlpattern.spec.whatwg.org/#tokenizer-policy
    // A tokenizer has an associated policy, a tokenize policy, initially "strict".
    pub policy: Policy,

    // https://urlpattern.spec.whatwg.org/#tokenizer-token-list
    // A tokenizer has an associated token list, a token list, initially an empty list.
    pub token_list: Vec<Token>,

    // https://urlpattern.spec.whatwg.org/#tokenizer-index
    // A tokenizer has an associated index, a number, initially 0.
    pub index: usize,

    // https://urlpattern.spec.whatwg.org/#tokenizer-next-index
    // A tokenizer has an associated next index, a number, initially 0.
    pub next_index: usize,

    // https://urlpattern.spec.whatwg.org/#tokenizer-code-point
    // A tokenizer has an associated code point, a Unicode code point, initially null.
    pub code_point: u32,

    input_code_points: Vec<char>,
}

// https://urlpattern.spec.whatwg.org/#tokenize-policy
// A tokenize policy is a string that must be either "strict" or "lenient".
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Policy {
    Strict,
    Lenient,
}

impl Tokenizer {
    pub(crate) fn new(input: &str, policy: Policy) -> Self {
        Self {
            input: input.to_string(),
            policy,
            token_list: Vec::new(),
            index: 0,
            next_index: 0,
            code_point: 0,
            input_code_points: input.chars().collect(),
        }
    }

    // https://urlpattern.spec.whatwg.org/#tokenize
    pub fn tokenize(input: &str, policy: Policy) -> PatternErrorOr<Vec<Token>> {
        // 1. Let tokenizer be a new tokenizer.
        // 2. Set tokenizer’s input to input.
        // 3. Set tokenizer’s policy to policy.
        let mut tokenizer = Self::new(input, policy);

        // 4. While tokenizer’s index is less than tokenizer’s input's code point length:
        while tokenizer.index < tokenizer.input_code_points.len() {
            // 1. Run seek and get the next code point given tokenizer and tokenizer’s index.
            tokenizer.seek_and_get_the_next_code_point(tokenizer.index as u32);

            // 2. If tokenizer’s code point is U+002A (*):
            if tokenizer.code_point == '*' as u32 {
                // 1. Run add a token with default position and length given tokenizer and "asterisk".
                tokenizer.add_a_token_with_default_position_and_length(Type::Asterisk);

                // 2. Continue.
                continue;
            }

            // 3. If tokenizer’s code point is U+002B (+) or U+003F (?):
            if tokenizer.code_point == '+' as u32 || tokenizer.code_point == '?' as u32 {
                // 1. Run add a token with default position and length given tokenizer and "other-modifier".
                tokenizer.add_a_token_with_default_position_and_length(Type::OtherModifier);

                // 2. Continue.
                continue;
            }

            // 4. If tokenizer’s code point is U+005C (\):
            if tokenizer.code_point == '\\' as u32 {
                // 1. If tokenizer’s index is equal to tokenizer’s input's code point length − 1:
                if tokenizer.index == tokenizer.input_code_points.len() - 1 {
                    // 1. Run process a tokenizing error given tokenizer, tokenizer’s next index, and tokenizer’s index.
                    tokenizer.process_a_tokenizing_error(tokenizer.next_index as u32, tokenizer.index as u32)?;

                    // 2. Continue.
                    continue;
                }

                // 2. Let escaped index be tokenizer’s next index.
                let escaped_index = tokenizer.next_index;

                // 3. Run get the next code point given tokenizer.
                tokenizer.get_the_next_code_point();

                // 4. Run add a token with default length given tokenizer, "escaped-char", tokenizer’s next index, and escaped index.
                tokenizer.add_a_token_with_default_length(
                    Type::EscapedChar,
                    tokenizer.next_index as u32,
                    escaped_index as u32,
                );

                // 5. Continue.
                continue;
            }

            // 5. If tokenizer’s code point is U+007B ({):
            if tokenizer.code_point == '{' as u32 {
                // 1. Run add a token with default position and length given tokenizer and "open".
                tokenizer.add_a_token_with_default_position_and_length(Type::Open);

                // 2. Continue.
                continue;
            }

            // 6. If tokenizer’s code point is U+007D (}):
            if tokenizer.code_point == '}' as u32 {
                // 1. Run add a token with default position and length given tokenizer and "close".
                tokenizer.add_a_token_with_default_position_and_length(Type::Close);

                // 2. Continue.
                continue;
            }

            // 1. If tokenizer’s code point is U+003A (:):
            if tokenizer.code_point == ':' as u32 {
                // 1. Let name position be tokenizer’s next index.
                let mut name_position = tokenizer.next_index;

                // 2. Let name start be name position.
                let name_start = name_position;

                // 3. While name position is less than tokenizer’s input's code point length:
                while name_position < tokenizer.input_code_points.len() {
                    // 1. Run seek and get the next code point given tokenizer and name position.
                    tokenizer.seek_and_get_the_next_code_point(name_position as u32);

                    // 2. Let first code point be true if name position equals name start and false otherwise.
                    let first_code_point = name_position == name_start;

                    // 3. Let valid code point be the result of running is a valid name code point given tokenizer’s code point and first code point.
                    let valid_code_point = Self::is_a_valid_name_code_point(tokenizer.code_point, first_code_point);

                    // 4. If valid code point is false break.
                    if !valid_code_point {
                        break;
                    }

                    // 5. Set name position to tokenizer’s next index.
                    name_position = tokenizer.next_index;
                }

                // 4. If name position is less than or equal to name start:
                if name_position <= name_start {
                    // 1. Run process a tokenizing error given tokenizer, name start, and tokenizer’s index.
                    tokenizer.process_a_tokenizing_error(name_start as u32, tokenizer.index as u32)?;

                    // 2. Continue.
                    continue;
                }

                // 5. Run add a token with default length given tokenizer, "name", name position, and name start.
                tokenizer.add_a_token_with_default_length(Type::Name, name_position as u32, name_start as u32);

                // 6. Continue.
                continue;
            }

            // 8. If tokenizer’s code point is U+0028 (():
            if tokenizer.code_point == '(' as u32 {
                // 1. Let depth be 1.
                let mut depth = 1u32;

                // 2. Let regexp position be tokenizer’s next index.
                let mut regexp_position = tokenizer.next_index;

                // 3. Let regexp start be regexp position.
                let regexp_start = regexp_position;

                // 4. Let error be false.
                let mut error = false;

                // 5. While regexp position is less than tokenizer’s input's code point length:
                while regexp_position < tokenizer.input_code_points.len() {
                    // 1. Run seek and get the next code point given tokenizer and regexp position.
                    tokenizer.seek_and_get_the_next_code_point(regexp_position as u32);

                    // 2. If the result of running is ASCII given tokenizer’s code point is false:
                    if tokenizer.code_point > 0x7f {
                        // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                        tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                        // 2. Set error to true.
                        error = true;

                        // 3. Break.
                        break;
                    }

                    // 3. If regexp position equals regexp start and tokenizer’s code point is U+003F (?):
                    if regexp_position == regexp_start && tokenizer.code_point == '?' as u32 {
                        // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                        tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                        // 2. Set error to true.
                        error = true;

                        // 3. Break.
                        break;
                    }

                    // 4. If tokenizer’s code point is U+005C (\):
                    if tokenizer.code_point == '\\' as u32 {
                        // 1. If regexp position equals tokenizer’s input's code point length − 1:
                        if regexp_position == tokenizer.input_code_points.len() - 1 {
                            // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                            tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                            // 2. Set error to true.
                            error = true;

                            // 3. Break
                            break;
                        }

                        // 2. Run get the next code point given tokenizer.
                        tokenizer.get_the_next_code_point();

                        // 3. If the result of running is ASCII given tokenizer’s code point is false:
                        if tokenizer.code_point > 0x7f {
                            // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                            tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                            // 2. Set error to true.
                            error = true;

                            // 3. Break.
                            break;
                        }

                        // 4. Set regexp position to tokenizer’s next index.
                        regexp_position = tokenizer.next_index;

                        // 5. Continue.
                        continue;
                    }

                    // 5. If tokenizer’s code point is U+0029 ()):
                    if tokenizer.code_point == ')' as u32 {
                        // 1. Decrement depth by 1.
                        depth -= 1;

                        // 1. If depth is 0:
                        if depth == 0 {
                            // 1. Set regexp position to tokenizer’s next index.
                            regexp_position = tokenizer.next_index;

                            // 2. Break.
                            break;
                        }
                    }
                    // 6. Otherwise if tokenizer’s code point is U+0028 (():
                    else if tokenizer.code_point == '(' as u32 {
                        // 1. Increment depth by 1.
                        depth += 1;

                        // 2. If regexp position equals tokenizer’s input's code point length − 1:
                        if regexp_position == tokenizer.input_code_points.len() - 1 {
                            // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                            tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                            // 2. Set error to true.
                            error = true;

                            // 3. Break
                            break;
                        }

                        // 3. Let temporary position be tokenizer’s next index.
                        let temporary_position = tokenizer.next_index;

                        // 4. Run get the next code point given tokenizer.
                        tokenizer.get_the_next_code_point();

                        // 5. If tokenizer’s code point is not U+003F (?):
                        if tokenizer.code_point != '?' as u32 {
                            // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                            tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                            // 2. Set error to true.
                            error = true;

                            // 3. Break.
                            break;
                        }

                        // 6. Set tokenizer’s next index to temporary position.
                        tokenizer.next_index = temporary_position;
                    }

                    // 7. Set regexp position to tokenizer’s next index.
                    regexp_position = tokenizer.next_index;
                }

                // 6. If error is true continue.
                if error {
                    continue;
                }

                // 7. If depth is not zero:
                if depth != 0 {
                    // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                    tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                    // 2. Continue.
                    continue;
                }

                // 8. Let regexp length be regexp position − regexp start − 1.
                let regexp_length = regexp_position - regexp_start - 1;

                // 9. If regexp length is zero:
                if regexp_length == 0 {
                    // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                    tokenizer.process_a_tokenizing_error(regexp_start as u32, tokenizer.index as u32)?;

                    // 2. Continue.
                    continue;
                }

                // 10. Run add a token given tokenizer, "regexp", regexp position, regexp start, and regexp length.
                tokenizer.add_a_token(
                    Type::Regexp,
                    regexp_position as u32,
                    regexp_start as u32,
                    regexp_length as u32,
                );

                // 11. Continue.
                continue;
            }

            // 9. Run add a token with default position and length given tokenizer and "char".
            tokenizer.add_a_token_with_default_position_and_length(Type::Char);
        }

        // 5. Run add a token with default length given tokenizer, "end", tokenizer’s index, and tokenizer’s index.
        tokenizer.add_a_token_with_default_length(Type::End, tokenizer.index as u32, tokenizer.index as u32);

        // 6. Return tokenizer’s token list.
        Ok(tokenizer.token_list)
    }

    // https://urlpattern.spec.whatwg.org/#get-the-next-code-point
    pub(crate) fn get_the_next_code_point(&mut self) {
        // 1. Set tokenizer’s code point to the Unicode code point in tokenizer’s input at the position indicated by tokenizer’s next index.
        self.code_point = self.input_code_points[self.next_index] as u32;

        // 2. Increment tokenizer’s next index by 1.
        self.next_index += 1;
    }

    // https://urlpattern.spec.whatwg.org/#seek-and-get-the-next-code-point
    pub(crate) fn seek_and_get_the_next_code_point(&mut self, index: u32) {
        // 1. Set tokenizer’s next index to index.
        self.next_index = index as usize;

        // 2. Run get the next code point given tokenizer.
        self.get_the_next_code_point();
    }

    // https://urlpattern.spec.whatwg.org/#add-a-token
    pub(crate) fn add_a_token(&mut self, r#type: Type, next_position: u32, value_position: u32, value_length: u32) {
        // 1. Let token be a new token.
        let token = Token {
            // 2. Set token’s type to type.
            r#type,

            // 3. Set token’s index to tokenizer’s index.
            index: self.index as u32,

            // 4. Set token’s value to the code point substring from value position with length value length within tokenizer’s input.
            value: self
                .input_code_points
                .iter()
                .skip(value_position as usize)
                .take(value_length as usize)
                .collect(),
        };

        // 5. Append token to the back of tokenizer’s token list.
        self.token_list.push(token);

        // 5. Set tokenizer’s index to next position.
        self.index = next_position as usize;
    }

    // https://urlpattern.spec.whatwg.org/#add-a-token-with-default-length
    pub(crate) fn add_a_token_with_default_length(&mut self, r#type: Type, next_position: u32, value_position: u32) {
        // 1. Let computed length be next position − value position.
        let computed_length = next_position - value_position;

        // 2. Run add a token given tokenizer, type, next position, value position, and computed length.
        self.add_a_token(r#type, next_position, value_position, computed_length);
    }

    // https://urlpattern.spec.whatwg.org/#add-a-token-with-default-position-and-length
    pub(crate) fn add_a_token_with_default_position_and_length(&mut self, r#type: Type) {
        // 1. Run add a token with default length given tokenizer, type, tokenizer’s next index, and tokenizer’s index.
        self.add_a_token_with_default_length(r#type, self.next_index as u32, self.index as u32);
    }

    // https://urlpattern.spec.whatwg.org/#process-a-tokenizing-error
    pub(crate) fn process_a_tokenizing_error(&mut self, next_position: u32, value_position: u32) -> PatternErrorOr<()> {
        // 1. If tokenizer’s policy is "strict", then throw a TypeError.
        if self.policy == Policy::Strict {
            return Err(ErrorInfo::new("Error processing a token"));
        }

        // 2. Assert: tokenizer’s policy is "lenient".
        assert!(self.policy == Policy::Lenient);

        // 3. Run add a token with default length given tokenizer, "invalid-char", next position, and value position.
        self.add_a_token_with_default_length(Type::InvalidChar, next_position, value_position);

        Ok(())
    }

    // https://urlpattern.spec.whatwg.org/#is-a-valid-name-code-point
    pub fn is_a_valid_name_code_point(code_point: u32, first: bool) -> bool {
        // 1. If first is true return the result of checking if code point is contained in the IdentifierStart set of code points.
        if first {
            return code_point == '$' as u32
                || code_point == '_' as u32
                || code_point_has_identifier_start_property(code_point);
        }

        // 2. Otherwise return the result of checking if code point is contained in the IdentifierPart set of code points.
        code_point == '$' as u32 || code_point_has_identifier_continue_property(code_point)
    }
}
