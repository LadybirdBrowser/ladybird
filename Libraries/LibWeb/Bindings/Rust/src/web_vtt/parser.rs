/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebVTT parser, https://w3c.github.io/webvtt/#webvtt-parser

use ak::character_types::is_ascii_digit;
use ak::{GenericLexer, Utf16String};
use libwebcommon_rust::infra::character_types::is_ascii_whitespace;

const LINE_FEED: u16 = b'\n' as u16;
const CARRIAGE_RETURN: u16 = b'\r' as u16;
const REPLACEMENT_CHARACTER: u16 = 0xFFFD;

/// A cue that the parser collected, which LibWeb creates a VTTCue for.
pub(crate) struct ParsedCue {
    pub(crate) identifier: Utf16String,
    pub(crate) start_time: f64,
    pub(crate) end_time: f64,
    pub(crate) text: Utf16String,
}

fn contains(haystack: &[u16], needle: &[u16]) -> bool {
    haystack.windows(needle.len()).any(|window| window == needle)
}

fn utf16(string: &str) -> Vec<u16> {
    string.encode_utf16().collect()
}

fn skip_whitespace(lexer: &mut GenericLexer<'_, u16>) {
    lexer.ignore_while(|code_unit| is_ascii_whitespace(code_unit.into()));
}

fn collect_ascii_digits<'a>(lexer: &mut GenericLexer<'a, u16>) -> &'a [u16] {
    lexer.consume_while(|code_unit| is_ascii_digit(code_unit.into()))
}

fn interpret_as_base_ten_integer(digits: &[u16]) -> u64 {
    digits.iter().fold(0u64, |value, digit| {
        value.wrapping_mul(10).wrapping_add(u64::from(digit - u16::from(b'0')))
    })
}

// https://w3c.github.io/webvtt/#collect-a-webvtt-timestamp
fn collect_a_webvtt_timestamp(lexer: &mut GenericLexer<'_, u16>) -> Option<f64> {
    // 1. Let input and position be the same variables as those of the same name in the algorithm that invoked these
    //    steps.

    // 2. Let most significant units be minutes.
    let mut most_significant_units_are_hours = false;

    // 3. If position is past the end of input, return an error and abort these steps.
    let code_unit = lexer.peek()?;

    // 4. If the character indicated by position is not an ASCII digit, then return an error and abort these steps.
    if !is_ascii_digit(code_unit.into()) {
        return None;
    }

    // 5. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
    let mut string = collect_ascii_digits(lexer);

    // 6. Interpret string as a base-ten integer. Let value1 be that integer.
    let mut value1 = interpret_as_base_ten_integer(string);

    // 7. If string is not exactly two characters in length, or if value1 is greater than 59, let most significant units
    //    be hours.
    if string.len() != 2 || value1 > 59 {
        most_significant_units_are_hours = true;
    }

    // 8. If position is beyond the end of input or if the character at position is not a U+003A COLON character (:),
    //    then return an error and abort these steps. Otherwise, move position forwards one character.
    if !lexer.consume_specific(u16::from(b':')) {
        return None;
    }

    // 9. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
    string = collect_ascii_digits(lexer);

    // 10. If string is not exactly two characters in length, return an error and abort these steps.
    if string.len() != 2 {
        return None;
    }

    // 11. Interpret string as a base-ten integer. Let value2 be that integer.
    let mut value2 = interpret_as_base_ten_integer(string);
    let value3;

    // 12. If most significant units is hours, or if position is not beyond the end of input and the character at
    //     position is a U+003A COLON character (:), run these substeps:
    if most_significant_units_are_hours || lexer.peek() == Some(u16::from(b':')) {
        // 1. If position is beyond the end of input or if the character at position is not a U+003A COLON character
        //    (:), then return an error and abort these steps. Otherwise, move position forwards one character.
        if !lexer.consume_specific(u16::from(b':')) {
            return None;
        }

        // 2. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
        string = collect_ascii_digits(lexer);

        // 3. If string is not exactly two characters in length, return an error and abort these steps.
        if string.len() != 2 {
            return None;
        }

        // 4. Interpret string as a base-ten integer. Let value3 be that integer.
        value3 = interpret_as_base_ten_integer(string);
    }
    // Otherwise (if most significant units is not hours, and either position is beyond the end of input, or the
    // character at position is not a U+003A COLON character (:)), let value3 have the value of value2, then value2 have
    // the value of value1, then let value1 equal zero.
    else {
        value3 = value2;
        value2 = value1;
        value1 = 0;
    }

    // 13. If position is beyond the end of input or if the character at position is not a U+002E FULL STOP character
    //     (.), then return an error and abort these steps. Otherwise, move position forwards one character.
    if !lexer.consume_specific(u16::from(b'.')) {
        return None;
    }

    // 14. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
    string = collect_ascii_digits(lexer);

    // 15. If string is not exactly three characters in length, return an error and abort these steps.
    if string.len() != 3 {
        return None;
    }

    // 16. Interpret string as a base-ten integer. Let value4 be that integer.
    let value4 = interpret_as_base_ten_integer(string);

    // 17. If value2 is greater than 59 or if value3 is greater than 59, return an error and abort these steps.
    if value2 > 59 || value3 > 59 {
        return None;
    }

    // 18. Let result be value1×60×60 + value2×60 + value3 + value4∕1000.
    let result = (value1 as f64 * 60.0 * 60.0) + (value2 as f64 * 60.0) + value3 as f64 + (value4 as f64 / 1000.0);

    // 19. Return result.
    Some(result)
}

// https://w3c.github.io/webvtt/#collect-webvtt-cue-timings-and-settings
fn collect_webvtt_cue_timings_and_settings(input: &[u16], cue: &mut ParsedCue) -> bool {
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    let mut lexer = GenericLexer::new(input);

    // 3. Skip whitespace.
    skip_whitespace(&mut lexer);

    // 4. Collect a WebVTT timestamp. If that algorithm fails, then abort these steps and return failure. Otherwise, let
    //    cue's text track cue start time be the collected time.
    let Some(start_time) = collect_a_webvtt_timestamp(&mut lexer) else {
        return false;
    };
    cue.start_time = start_time;

    // 5. Skip whitespace.
    skip_whitespace(&mut lexer);

    // 6. If the character at position is not a U+002D HYPHEN-MINUS character (-) then abort these steps and return
    //    failure. Otherwise, move position forwards one character.
    // 7. If the character at position is not a U+002D HYPHEN-MINUS character (-) then abort these steps and return
    //    failure. Otherwise, move position forwards one character.
    // 8. If the character at position is not a U+003E GREATER-THAN SIGN character (>) then abort these steps and return
    //    failure. Otherwise, move position forwards one character.
    if !lexer.consume_specific(u16::from(b'-'))
        || !lexer.consume_specific(u16::from(b'-'))
        || !lexer.consume_specific(u16::from(b'>'))
    {
        return false;
    }

    // 9. Skip whitespace.
    skip_whitespace(&mut lexer);

    // 10. Collect a WebVTT timestamp. If that algorithm fails, then abort these steps and return failure. Otherwise, let
    //     cue's text track cue end time be the collected time.
    let Some(end_time) = collect_a_webvtt_timestamp(&mut lexer) else {
        return false;
    };
    cue.end_time = end_time;

    // FIXME: 11. Let remainder be the trailing substring of input starting at position.
    // FIXME: 12. Parse the WebVTT cue settings from remainder using regions for cue.
    true
}

fn is_block_keyword(buffer: &[u16], keyword: &str) -> bool {
    let keyword = utf16(keyword);
    let Some(remainder) = buffer.strip_prefix(keyword.as_slice()) else {
        return false;
    };
    remainder.iter().all(|&code_unit| is_ascii_whitespace(code_unit.into()))
}

struct Parser<'a> {
    input: &'a [u16],
    lexer: GenericLexer<'a, u16>,
    seen_cue: bool,
}

impl<'a> Parser<'a> {
    fn new(input: &'a [u16]) -> Self {
        Self {
            input,
            lexer: GenericLexer::new(input),
            seen_cue: false,
        }
    }

    fn collect_a_sequence_of_code_points_that_are_not_line_feeds(&mut self) -> &'a [u16] {
        self.lexer.consume_while(|code_unit| code_unit != LINE_FEED)
    }

    fn collect_a_sequence_of_line_feeds(&mut self) {
        self.lexer.ignore_while(|code_unit| code_unit == LINE_FEED);
    }

    // https://w3c.github.io/webvtt/#collect-a-webvtt-block
    fn collect_a_webvtt_block(&mut self, in_header: bool) -> Option<ParsedCue> {
        // 1. Let input, position, seen cue and regions be the same variables as those of the same name in the algorithm
        //    that invoked these steps.

        // 2. Let line count be zero.
        let mut line_count = 0usize;

        // 3. Let previous position be position.
        let mut previous_position = self.lexer.tell();

        // 4. Let line be the empty string.
        // 5. Let buffer be the empty string.
        let mut buffer: Vec<u16> = Vec::new();

        // 6. Let seen EOF be false.
        let mut seen_eof = false;

        // 7. Let seen arrow be false.
        let mut seen_arrow = false;

        // 8. Let cue be null.
        let mut cue: Option<ParsedCue> = None;

        // FIXME: 9. Let stylesheet be null.
        // FIXME: 10. Let region be null.

        let arrow = utf16("-->");

        // 11. Loop: Run these substeps in a loop:
        loop {
            // 1. Collect a sequence of code points that are not U+000A LINE FEED (LF) characters. Let line be those
            //    characters, if any.
            let line = self.collect_a_sequence_of_code_points_that_are_not_line_feeds();

            // 2. Increment line count by 1.
            line_count += 1;

            // 3. If position is past the end of input, let seen EOF be true. Otherwise, the character indicated by
            //    position is a U+000A LINE FEED (LF) character; advance position to the next character in input.
            if self.lexer.is_eof() {
                seen_eof = true;
            } else {
                self.lexer.ignore(1);
            }

            // 4. If line contains the three-character substring "-->" (U+002D HYPHEN-MINUS, U+002D HYPHEN-MINUS, U+003E
            //    GREATER-THAN SIGN), then run these substeps:
            if contains(line, &arrow) {
                // 1. If in header is not set and at least one of the following conditions are true:
                //    - line count is 1
                //    - line count is 2 and seen arrow is false
                //    ...then run these substeps:
                if !in_header && (line_count == 1 || (line_count == 2 && !seen_arrow)) {
                    // 1. Let seen arrow be true.
                    seen_arrow = true;

                    // 2. Let previous position be position.
                    previous_position = self.lexer.tell();

                    // 3. Cue creation: Let cue be a new WebVTT cue and initialize it as follows:
                    //    Let cue's text track cue identifier be buffer.
                    //    FIXME: Initialize the remaining cue fields once cue settings are supported.
                    let mut new_cue = ParsedCue {
                        identifier: Utf16String::from_utf16(&buffer),
                        start_time: 0.0,
                        end_time: 0.0,
                        text: Utf16String::from_utf8(""),
                    };

                    // 4. Collect WebVTT cue timings and settings from line using regions for cue. If that fails, let
                    //    cue be null. Otherwise, let buffer be the empty string and let seen cue be true.
                    if collect_webvtt_cue_timings_and_settings(line, &mut new_cue) {
                        cue = Some(new_cue);
                        buffer.clear();
                        self.seen_cue = true;
                    } else {
                        cue = None;
                    }
                }
                // Otherwise, let position be previous position and break out of loop.
                else {
                    self.lexer.retreat(self.lexer.tell() - previous_position);
                    break;
                }
            }
            // 5. Otherwise, if line is the empty string, break out of loop.
            else if line.is_empty() {
                break;
            }
            // 6. Otherwise, run these substeps:
            else {
                // 1. If in header is not set and line count is 2, run these substeps:
                if !in_header && line_count == 2 {
                    // 1. If seen cue is false and buffer starts with the substring "STYLE" (U+0053 LATIN CAPITAL LETTER
                    //    S, U+0054 LATIN CAPITAL LETTER T, U+0059 LATIN CAPITAL LETTER Y, U+004C LATIN CAPITAL LETTER
                    //    L, U+0045 LATIN CAPITAL LETTER E), and the remaining characters in buffer (if any) are all
                    //    ASCII whitespace, then run these substeps:
                    // 2. Otherwise, if seen cue is false and buffer starts with the substring "REGION" (U+0052 LATIN
                    //    CAPITAL LETTER R, U+0045 LATIN CAPITAL LETTER E, U+0047 LATIN CAPITAL LETTER G, U+0049 LATIN
                    //    CAPITAL LETTER I, U+004F LATIN CAPITAL LETTER O, U+004E LATIN CAPITAL LETTER N), and the
                    //    remaining characters in buffer (if any) are all ASCII whitespace, then run these substeps:
                    if !self.seen_cue && (is_block_keyword(&buffer, "STYLE") || is_block_keyword(&buffer, "REGION")) {
                        // FIXME: Create the CSS style sheet or WebVTT region.
                        // Let buffer be the empty string.
                        buffer.clear();
                    }
                }

                // 2. If buffer is not the empty string, append a U+000A LINE FEED (LF) character to buffer.
                if !buffer.is_empty() {
                    buffer.push(LINE_FEED);
                }

                // 3. Append line to buffer.
                buffer.extend_from_slice(line);

                // 4. Let previous position be position.
                previous_position = self.lexer.tell();
            }

            // 7. If seen EOF is true, break out of loop.
            if seen_eof {
                break;
            }
        }

        // 12. If cue is not null, let the cue text of cue be buffer, and return cue.
        if let Some(mut cue) = cue {
            cue.text = Utf16String::from_utf16(&buffer);
            return Some(cue);
        }

        // FIXME: 13. Otherwise, if stylesheet is not null, then Parse a stylesheet from buffer. If it returned a list of
        //            rules, assign the list as stylesheet's CSS rules; otherwise, set stylesheet's CSS rules to an
        //            empty list. Finally, return stylesheet.
        // FIXME: 14. Otherwise, if region is not null, then collect WebVTT region settings from buffer using region for
        //            the results. Construct a WebVTT Region Object from region, and return it.

        // 15. Otherwise, return null.
        None
    }

    // https://w3c.github.io/webvtt/#webvtt-parser-algorithm
    fn parse(&mut self) -> Option<Vec<ParsedCue>> {
        let mut output = Vec::new();

        // 2. Let position be a pointer into input, initially pointing at the start of the string.
        // 3. Let seen cue be false.

        // 4. If input is less than six characters long, then abort these steps. The file does not start with the
        //    correct WebVTT file signature and was therefore not successfully processed.
        if self.input.len() < 6 {
            return None;
        }

        // 5. If input is exactly six characters long but does not exactly equal "WEBVTT", then abort these steps. The
        //    file does not start with the correct WebVTT file signature and was therefore not successfully processed.
        // 6. If input is more than six characters long but the first six characters do not exactly equal "WEBVTT", or
        //    the seventh character is not a U+0020 SPACE character, a U+0009 CHARACTER TABULATION (tab) character, or a
        //    U+000A LINE FEED (LF) character, then abort these steps. The file does not start with the correct WebVTT
        //    file signature and was therefore not successfully processed.
        if !self.input.starts_with(&utf16("WEBVTT")) {
            return None;
        }
        if self.input.len() > 6 && !matches!(self.input[6], 0x20 | 0x09 | LINE_FEED) {
            return None;
        }

        // 7. Collect a sequence of code points that are not U+000A LINE FEED (LF) characters.
        self.collect_a_sequence_of_code_points_that_are_not_line_feeds();

        // 8. If position is past the end of input, then abort these steps. The file was successfully processed, but it
        //    contains no useful data and so no WebVTT cues were added to output.
        if self.lexer.is_eof() {
            return Some(output);
        }

        // 9. The character indicated by position is a U+000A LINE FEED (LF) character. Advance position to the next
        //    character in input.
        self.lexer.ignore(1);

        // 10. If position is past the end of input, then abort these steps. The file was successfully processed, but it
        //     contains no useful data and so no WebVTT cues were added to output.
        if self.lexer.is_eof() {
            return Some(output);
        }

        // 11. Header: If the character indicated by position is not a U+000A LINE FEED (LF) character, then collect a
        //     WebVTT block with the in header flag set. Otherwise, advance position to the next character in input.
        if self.lexer.peek() == Some(LINE_FEED) {
            self.lexer.ignore(1);
        } else {
            self.collect_a_webvtt_block(true);
        }

        // 12. Collect a sequence of code points that are U+000A LINE FEED (LF) characters.
        self.collect_a_sequence_of_line_feeds();

        // FIXME: 13. Let regions be an empty text track list of regions.

        // 14. Block loop: While position doesn't point past the end of input:
        while !self.lexer.is_eof() {
            // 1. Collect a WebVTT block, and let block be the returned value.
            let block = self.collect_a_webvtt_block(false);

            // 2. If block is a WebVTT cue, add block to the text track list of cues output.
            if let Some(block) = block {
                output.push(block);
            }

            // FIXME: 3. Otherwise, if block is a CSS style sheet, add block to stylesheets.
            // FIXME: 4. Otherwise, if block is a WebVTT region object, add block to regions.

            // 5. Collect a sequence of code points that are U+000A LINE FEED (LF) characters.
            self.collect_a_sequence_of_line_feeds();
        }

        // 15. End: The file has ended. Abort these steps. The WebVTT parser has finished. The file was successfully
        //     processed.
        Some(output)
    }
}

/// https://w3c.github.io/webvtt/#webvtt-parser
///
/// Returns None if the file does not start with the WebVTT file signature.
pub(crate) fn parse_webvtt_file(bytes: &[u8]) -> Option<Vec<ParsedCue>> {
    // A WebVTT parser, given an input byte stream, a text track list of cues output, and a collection of CSS style
    // sheets stylesheets, must decode the byte stream using the UTF-8 decode algorithm, and then must parse the
    // resulting string according to the WebVTT parser algorithm below.
    let decoded = String::from_utf8_lossy(bytes);
    let decoded = decoded.strip_prefix('\u{FEFF}').unwrap_or(&decoded);

    // 1. Let input be the string being parsed, after conversion to Unicode, and with the following transformations
    //    applied:
    //    - Replace all U+0000 NULL characters by U+FFFD REPLACEMENT CHARACTERs.
    //    - Replace each U+000D CARRIAGE RETURN U+000A LINE FEED (CRLF) character pair by a single U+000A LINE FEED
    //      (LF) character.
    //    - Replace all remaining U+000D CARRIAGE RETURN characters by U+000A LINE FEED (LF) characters.
    let mut input = Vec::with_capacity(decoded.len());
    let mut code_units = decoded.encode_utf16().peekable();
    while let Some(code_unit) = code_units.next() {
        match code_unit {
            0 => input.push(REPLACEMENT_CHARACTER),
            CARRIAGE_RETURN => {
                code_units.next_if_eq(&LINE_FEED);
                input.push(LINE_FEED);
            }
            _ => input.push(code_unit),
        }
    }

    Parser::new(&input).parse()
}
