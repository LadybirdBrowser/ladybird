/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::pattern::ErrorInfo;
use crate::pattern::Options;
use crate::pattern::Part;
use crate::pattern::PatternErrorOr;
use crate::pattern::Token;
use crate::pattern::Tokenizer;
use crate::pattern::full_wildcard_regexp_value;
use crate::pattern::generate_a_segment_wildcard_regexp;
use crate::pattern::part::Modifier as PartModifier;
use crate::pattern::part::Type as PartType;
use crate::pattern::tokenizer::Policy as TokenizerPolicy;
use crate::pattern::tokenizer::Type as TokenType;

// https://urlpattern.spec.whatwg.org/#pattern-parser
pub struct PatternParser {
    // https://urlpattern.spec.whatwg.org/#pattern-parser-token-list
    // A pattern parser has an associated token list, a token list, initially an empty list.
    pub token_list: Vec<Token>,

    // https://urlpattern.spec.whatwg.org/#pattern-parser-encoding-callback
    // A pattern parser has an associated encoding callback, a encoding callback, that must be set upon creation.
    pub encoding_callback: EncodingCallback,

    // https://urlpattern.spec.whatwg.org/#pattern-parser-segment-wildcard-regexp
    // A pattern parser has an associated segment wildcard regexp, a string, that must be set upon creation.
    pub segment_wildcard_regexp: String,

    // https://urlpattern.spec.whatwg.org/#pattern-parser-part-list
    // A pattern parser has an associated part list, a part list, initially an empty list.
    pub part_list: Vec<Part>,

    // https://urlpattern.spec.whatwg.org/#pattern-parser-pending-fixed-value
    // A pattern parser has an associated pending fixed value, a string, initially the empty string.
    pub pending_fixed_value: String,

    // https://urlpattern.spec.whatwg.org/#pattern-parser-index
    // A pattern parser has an associated index, a number, initially 0.
    pub index: usize,

    // https://urlpattern.spec.whatwg.org/#pattern-parser-next-numeric-name
    // A pattern parser has an associated next numeric name, a number, initially 0.
    pub next_numeric_name: usize,
}

// https://urlpattern.spec.whatwg.org/#encoding-callback
// An encoding callback is an abstract algorithm that takes a given string input. The input will be a simple text
// piece of a pattern string. An implementing algorithm will validate and encode the input. It must return the
// encoded string or throw an exception.
pub type EncodingCallback = Box<dyn Fn(&str) -> PatternErrorOr<String>>;

impl PatternParser {
    pub(crate) fn new(encoding_callback: EncodingCallback, segment_wildcard_regexp: String) -> Self {
        Self {
            token_list: Vec::new(),
            encoding_callback,
            segment_wildcard_regexp,
            part_list: Vec::new(),
            pending_fixed_value: String::new(),
            index: 0,
            next_numeric_name: 0,
        }
    }

    // https://urlpattern.spec.whatwg.org/#consume-a-required-token
    pub(crate) fn consume_a_required_token(&mut self, r#type: TokenType) -> PatternErrorOr<()> {
        // 1. Let result be the result of running try to consume a token given parser and type.
        let result = self.try_to_consume_a_token(r#type);

        // 2. If result is null, then throw a TypeError.
        if result.is_none() {
            return Err(ErrorInfo::new(format!(
                "Missing required token '{}' in URL pattern",
                Token::type_to_string(r#type)
            )));
        }

        // 3. Return result.
        // NOTE: No caller actually needs the result, so we just ignore it.
        Ok(())
    }

    // https://urlpattern.spec.whatwg.org/#consume-text
    pub(crate) fn consume_text(&mut self) -> String {
        // 1. Let result be the empty string.
        let mut result = String::new();

        // 1. While true:
        loop {
            // 1. Let token be the result of running try to consume a token given parser and "char".
            let mut token = self.try_to_consume_a_token(TokenType::Char);

            // 2. If token is null, then set token to the result of running try to consume a token given parser and "escaped-char".
            if token.is_none() {
                token = self.try_to_consume_a_token(TokenType::EscapedChar);
            }

            // 3. If token is null, then break.
            let Some(token) = token else {
                break;
            };

            // 4. Append token’s value to the end of result.
            result.push_str(&token.value);
        }

        // 2. Return result.
        result
    }

    // https://urlpattern.spec.whatwg.org/#maybe-add-a-part-from-the-pending-fixed-value
    pub(crate) fn maybe_add_a_part_from_the_pending_fixed_value(&mut self) -> PatternErrorOr<()> {
        // 1. If parser’s pending fixed value is the empty string, then return.
        if self.pending_fixed_value.is_empty() {
            return Ok(());
        }

        // 2. Let encoded value be the result of running parser’s encoding callback given parser’s pending fixed value.
        let encoded_value = (self.encoding_callback)(&self.pending_fixed_value)?;

        // 3. Set parser’s pending fixed value to the empty string.
        self.pending_fixed_value.clear();

        // 4. Let part be a new part whose type is "fixed-text", value is encoded value, and modifier is "none".
        // 5. Append part to parser’s part list.
        self.part_list
            .push(Part::new(PartType::FixedText, encoded_value, PartModifier::None));

        Ok(())
    }

    // https://urlpattern.spec.whatwg.org/#is-a-duplicate-name
    pub(crate) fn is_a_duplicate_name(&self, name: &str) -> bool {
        // 1. For each part of parser’s part list:
        for part in &self.part_list {
            // 1. If part’s name is name, then return true.
            if part.name == name {
                return true;
            }
        }

        // 2. Return false.
        false
    }

    // https://urlpattern.spec.whatwg.org/#add-a-part
    pub(crate) fn add_a_part(
        &mut self,
        prefix: &str,
        name_token: Option<Token>,
        regexp_or_wildcard_token: Option<Token>,
        suffix: &str,
        modifier_token: Option<Token>,
    ) -> PatternErrorOr<()> {
        // 1. Let modifier be "none".
        let mut modifier = PartModifier::None;

        // 2. If modifier token is not null:
        if let Some(modifier_token) = modifier_token {
            // 1. If modifier token’s value is "?" then set modifier to "optional".
            if modifier_token.value == "?" {
                modifier = PartModifier::Optional;
            }
            // 2. Otherwise if modifier token’s value is "*" then set modifier to "zero-or-more".
            else if modifier_token.value == "*" {
                modifier = PartModifier::ZeroOrMore;
            }
            // 3. Otherwise if modifier token’s value is "+" then set modifier to "one-or-more".
            else if modifier_token.value == "+" {
                modifier = PartModifier::OneOrMore;
            }
        }

        // 3. If name token is null and regexp or wildcard token is null and modifier is "none":
        // NOTE: This was a "{foo}" grouping. We add this to the pending fixed value so that it will be combined with
        //       any previous or subsequent text.
        if name_token.is_none() && regexp_or_wildcard_token.is_none() && modifier == PartModifier::None {
            // 1. Append prefix to the end of parser’s pending fixed value.
            self.pending_fixed_value.push_str(prefix);

            // 2. Return.
            return Ok(());
        }

        // 4. Run maybe add a part from the pending fixed value given parser.
        self.maybe_add_a_part_from_the_pending_fixed_value()?;

        // 5. If name token is null and regexp or wildcard token is null:
        // NOTE: This was a "{foo}?" grouping. The modifier means we cannot combine it with other text. Therefore we
        //       add it as a part immediately.
        if name_token.is_none() && regexp_or_wildcard_token.is_none() {
            // 1. Assert: suffix is the empty string.
            assert!(suffix.is_empty());

            // 2. If prefix is the empty string, then return.
            if prefix.is_empty() {
                return Ok(());
            }

            // 3. Let encoded value be the result of running parser’s encoding callback given prefix.
            let encoded_value = (self.encoding_callback)(prefix)?;

            // 4. Let part be a new part whose type is "fixed-text", value is encoded value, and modifier is modifier.
            // 5. Append part to parser’s part list.
            self.part_list
                .push(Part::new(PartType::FixedText, encoded_value, modifier));

            // 6. Return.
            return Ok(());
        }

        // 6. Let regexp value be the empty string.
        // NOTE: Next, we convert the regexp or wildcard token into a regular expression.
        let mut regexp_value =
            // 7. If regexp or wildcard token is null, then set regexp value to parser’s segment wildcard regexp.
            if let Some(regexp_or_wildcard_token) = regexp_or_wildcard_token.as_ref() {
                // 8. Otherwise if regexp or wildcard token’s type is "asterisk", then set regexp value to the full wildcard regexp value.
                if regexp_or_wildcard_token.r#type == TokenType::Asterisk {
                    full_wildcard_regexp_value.to_string()
                }
                // 9. Otherwise set regexp value to regexp or wildcard token’s value.
                else {
                    regexp_or_wildcard_token.value.clone()
                }
            } else {
                self.segment_wildcard_regexp.clone()
            };

        // 10. Let type be "regexp".
        // NOTE: Next, we convert regexp value into a part type. We make sure to go to a regular expression first so
        //       that an equivalent "regexp" token will be treated the same as a "name" or "asterisk" token.
        let mut r#type = PartType::Regexp;

        // 11. If regexp value is parser’s segment wildcard regexp:
        if regexp_value == self.segment_wildcard_regexp {
            // 1. Set type to "segment-wildcard".
            r#type = PartType::SegmentWildcard;

            // 2. Set regexp value to the empty string.
            regexp_value = String::new();
        }
        // 12. Otherwise if regexp value is the full wildcard regexp value:
        else if regexp_value == full_wildcard_regexp_value {
            // 1. Set type to "full-wildcard".
            r#type = PartType::FullWildcard;

            // 2. Set regexp value to the empty string.
            regexp_value = String::new();
        }

        // 13. Let name be the empty string.
        // NOTE: Next, we determine the part name. This can be explicitly provided by a "name" token or be automatically assigned.
        let mut name = String::new();

        // 14. If name token is not null, then set name to name token’s value.
        if let Some(name_token) = name_token {
            name = name_token.value;
        }
        // 15. Otherwise if regexp or wildcard token is not null:
        else if regexp_or_wildcard_token.is_some() {
            // 1. Set name to parser’s next numeric name, serialized.
            name = self.next_numeric_name.to_string();

            // 2. Increment parser’s next numeric name by 1.
            self.next_numeric_name += 1;
        }

        // 16. If the result of running is a duplicate name given parser and name is true, then throw a TypeError.
        if self.is_a_duplicate_name(&name) {
            return Err(ErrorInfo::new(format!(
                "Duplicate name '{name}' provided in URL pattern"
            )));
        }

        // 17. Let encoded prefix be the result of running parser’s encoding callback given prefix.
        // NOTE: Finally, we encode the fixed text values and create the part.
        let encoded_prefix = (self.encoding_callback)(prefix)?;

        // 18. Let encoded suffix be the result of running parser’s encoding callback given suffix.
        let encoded_suffix = (self.encoding_callback)(suffix)?;

        // 19. Let part be a new part whose type is type, value is regexp value, modifier is modifier, name is name, prefix
        //     is encoded prefix, and suffix is encoded suffix.
        // 20. Append part to parser’s part list.
        self.part_list.push(Part::new_with_name(
            r#type,
            regexp_value,
            modifier,
            name,
            encoded_prefix,
            encoded_suffix,
        ));

        Ok(())
    }

    // https://urlpattern.spec.whatwg.org/#try-to-consume-a-modifier-token
    pub(crate) fn try_to_consume_a_modifier_token(&mut self) -> Option<Token> {
        // 1. Let token be the result of running try to consume a token given parser and "other-modifier".
        let mut token = self.try_to_consume_a_token(TokenType::OtherModifier);

        // 2. If token is not null, then return token.
        if token.is_some() {
            return token;
        }

        // 3. Set token to the result of running try to consume a token given parser and "asterisk".
        token = self.try_to_consume_a_token(TokenType::Asterisk);

        // 4. Return token.
        token
    }

    // https://urlpattern.spec.whatwg.org/#try-to-consume-a-regexp-or-wildcard-token
    pub(crate) fn try_to_consume_a_regexp_or_wildcard_token(&mut self, name_token: Option<Token>) -> Option<Token> {
        // 1. Let token be the result of running try to consume a token given parser and "regexp".
        let mut token = self.try_to_consume_a_token(TokenType::Regexp);

        // 2. If name token is null and token is null, then set token to the result of running try to consume a token given
        //    parser and "asterisk".
        if name_token.is_none() && token.is_none() {
            token = self.try_to_consume_a_token(TokenType::Asterisk);
        }

        // 3. Return token.
        token
    }

    // https://urlpattern.spec.whatwg.org/#try-to-consume-a-token
    pub(crate) fn try_to_consume_a_token(&mut self, r#type: TokenType) -> Option<Token> {
        // 1. Assert: parser’s index is less than parser’s token list size.
        assert!(self.index < self.token_list.len());

        // 2. Let next token be parser’s token list[parser’s index].
        let next_token = self.token_list[self.index].clone();

        // 3. If next token’s type is not type return null.
        if next_token.r#type != r#type {
            return None;
        }

        // 4. Increment parser’s index by 1.
        self.index += 1;

        // 5. Return next token.
        Some(next_token)
    }

    // https://urlpattern.spec.whatwg.org/#parse-a-pattern-string
    pub fn parse(input: &str, options: &Options, encoding_callback: EncodingCallback) -> PatternErrorOr<Vec<Part>> {
        // 1. Let parser be a new pattern parser whose encoding callback is encoding callback and segment wildcard regexp
        //    is the result of running generate a segment wildcard regexp given options.
        let mut parser = Self::new(encoding_callback, generate_a_segment_wildcard_regexp(options));

        // 2. Set parser’s token list to the result of running tokenize given input and "strict".
        parser.token_list = Tokenizer::tokenize(input, TokenizerPolicy::Strict)?;

        // 3. While parser’s index is less than parser’s token list's size:
        while parser.index < parser.token_list.len() {
            // 1. Let char token be the result of running try to consume a token given parser and "char".
            let char_token = parser.try_to_consume_a_token(TokenType::Char);

            // 2. Let name token be the result of running try to consume a token given parser and "name".
            let mut name_token = parser.try_to_consume_a_token(TokenType::Name);

            // 3. Let regexp or wildcard token be the result of running try to consume a regexp or wildcard token given
            //    parser and name token.
            let mut regexp_or_wildcard_token = parser.try_to_consume_a_regexp_or_wildcard_token(name_token.clone());

            // 4. If name token is not null or regexp or wildcard token is not null:
            // NOTE: If there is a matching group, we need to add the part immediately.
            if name_token.is_some() || regexp_or_wildcard_token.is_some() {
                // 1. Let prefix be the empty string.
                let mut prefix = String::new();

                // 2. If char token is not null then set prefix to char token’s value.
                if let Some(char_token) = char_token {
                    prefix = char_token.value;
                }

                // 3. If prefix is not the empty string and not options’s prefix code point:
                if !prefix.is_empty()
                    && (options.prefix_code_point.is_none() || prefix != options.prefix_code_point.unwrap().to_string())
                {
                    // 1. Append prefix to the end of parser’s pending fixed value.
                    parser.pending_fixed_value.push_str(&prefix);

                    // 2. Set prefix to the empty string.
                    prefix.clear();
                }

                // 4. Run maybe add a part from the pending fixed value given parser.
                parser.maybe_add_a_part_from_the_pending_fixed_value()?;

                // 5. Let modifier token be the result of running try to consume a modifier token given parser.
                let modifier_token = parser.try_to_consume_a_modifier_token();

                // 6. Run add a part given parser, prefix, name token, regexp or wildcard token, the empty string,
                //    and modifier token.
                parser.add_a_part(&prefix, name_token, regexp_or_wildcard_token, "", modifier_token)?;

                // 7. Continue.
                continue;
            }

            // 5. Let fixed token be char token.
            // NOTE: If there was no matching group, then we need to buffer any fixed text. We want to collect as
            //       much text as possible before adding it as a "fixed-text" part.
            let mut fixed_token = char_token;

            // 6. If fixed token is null, then set fixed token to the result of running try to consume a token given
            //     parser and "escaped-char".
            if fixed_token.is_none() {
                fixed_token = parser.try_to_consume_a_token(TokenType::EscapedChar);
            }

            // 7. If fixed token is not null:
            if let Some(fixed_token) = fixed_token {
                // 1. Append fixed token’s value to parser’s pending fixed value.
                parser.pending_fixed_value.push_str(&fixed_token.value);

                // 2. Continue.
                continue;
            }

            // 8. Let open token be the result of running try to consume a token given parser and "open".
            let open_token = parser.try_to_consume_a_token(TokenType::Open);

            // 9. If open token is not null:
            if open_token.is_some() {
                // 1. Let prefix be the result of running consume text given parser.
                let prefix = parser.consume_text();

                // 2. Set name token to the result of running try to consume a token given parser and "name".
                name_token = parser.try_to_consume_a_token(TokenType::Name);

                // 3. Set regexp or wildcard token to the result of running try to consume a regexp or wildcard token
                //    given parser and name token.
                regexp_or_wildcard_token = parser.try_to_consume_a_regexp_or_wildcard_token(name_token.clone());

                // 4. Let suffix be the result of running consume text given parser.
                let suffix = parser.consume_text();

                // 5. Run consume a required token given parser and "close".
                parser.consume_a_required_token(TokenType::Close)?;

                // 6. Let modifier token to the result of running try to consume a modifier token given parser.
                let modifier_token = parser.try_to_consume_a_modifier_token();

                // 7. Run add a part given parser, prefix, name token, regexp or wildcard token, suffix, and modifier token.
                parser.add_a_part(&prefix, name_token, regexp_or_wildcard_token, &suffix, modifier_token)?;

                // 8. Continue.
                continue;
            }

            // 10. Run maybe add a part from the pending fixed value given parser.
            parser.maybe_add_a_part_from_the_pending_fixed_value()?;

            // 11. Run consume a required token given parser and "end".
            parser.consume_a_required_token(TokenType::End)?;
        }

        // 4. Return parser’s part list.
        Ok(parser.part_list)
    }
}
