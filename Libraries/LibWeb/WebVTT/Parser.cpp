/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/WebVTT/Parser.h>
#include <LibWebCommon/Infra/CharacterTypes.h>

namespace Web::WebVTT {

namespace {

class Parser {
public:
    explicit Parser(Utf16View input)
        : m_input(input)
    {
    }

    Optional<Vector<ParsedCue>> parse();

private:
    bool is_past_end() const { return m_position >= m_input.length_in_code_units(); }
    char16_t current() const { return m_input.code_unit_at(m_position); }

    Utf16View collect_a_sequence_of_code_points_that_are_not_line_feeds();
    void collect_a_sequence_of_line_feeds();
    Optional<ParsedCue> collect_a_webvtt_block(bool in_header);

    Utf16View m_input;
    size_t m_position { 0 };
    bool m_seen_cue { false };
};

Utf16View Parser::collect_a_sequence_of_code_points_that_are_not_line_feeds()
{
    auto start = m_position;
    while (!is_past_end() && current() != '\n')
        ++m_position;
    return m_input.substring_view(start, m_position - start);
}

void Parser::collect_a_sequence_of_line_feeds()
{
    while (!is_past_end() && current() == '\n')
        ++m_position;
}

class StringScanner {
public:
    explicit StringScanner(Utf16View input)
        : m_input(input)
    {
    }

    bool is_past_end() const { return m_position >= m_input.length_in_code_units(); }
    char16_t current() const { return m_input.code_unit_at(m_position); }
    void advance() { ++m_position; }

    void skip_whitespace()
    {
        while (!is_past_end() && Infra::is_ascii_whitespace(current()))
            ++m_position;
    }

    Utf16View collect_ascii_digits()
    {
        auto start = m_position;
        while (!is_past_end() && is_ascii_digit(current()))
            ++m_position;
        return m_input.substring_view(start, m_position - start);
    }

    bool consume(char16_t code_unit)
    {
        if (is_past_end() || current() != code_unit)
            return false;
        ++m_position;
        return true;
    }

private:
    Utf16View m_input;
    size_t m_position { 0 };
};

u64 interpret_as_base_ten_integer(Utf16View digits)
{
    u64 value = 0;
    for (size_t i = 0; i < digits.length_in_code_units(); ++i)
        value = (value * 10) + (digits.code_unit_at(i) - '0');
    return value;
}

// https://w3c.github.io/webvtt/#collect-a-webvtt-timestamp
Optional<double> collect_a_webvtt_timestamp(StringScanner& scanner)
{
    // 1. Let input and position be the same variables as those of the same name in the algorithm that invoked these
    //    steps.

    // 2. Let most significant units be minutes.
    bool most_significant_units_are_hours = false;

    // 3. If position is past the end of input, return an error and abort these steps.
    if (scanner.is_past_end())
        return {};

    // 4. If the character indicated by position is not an ASCII digit, then return an error and abort these steps.
    if (!is_ascii_digit(scanner.current()))
        return {};

    // 5. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
    auto string = scanner.collect_ascii_digits();

    // 6. Interpret string as a base-ten integer. Let value1 be that integer.
    auto value1 = interpret_as_base_ten_integer(string);

    // 7. If string is not exactly two characters in length, or if value1 is greater than 59, let most significant units
    //    be hours.
    if (string.length_in_code_units() != 2 || value1 > 59)
        most_significant_units_are_hours = true;

    // 8. If position is beyond the end of input or if the character at position is not a U+003A COLON character (:),
    //    then return an error and abort these steps. Otherwise, move position forwards one character.
    if (!scanner.consume(':'))
        return {};

    // 9. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
    string = scanner.collect_ascii_digits();

    // 10. If string is not exactly two characters in length, return an error and abort these steps.
    if (string.length_in_code_units() != 2)
        return {};

    // 11. Interpret string as a base-ten integer. Let value2 be that integer.
    auto value2 = interpret_as_base_ten_integer(string);
    u64 value3 = 0;

    // 12. If most significant units is hours, or if position is not beyond the end of input and the character at
    //     position is a U+003A COLON character (:), run these substeps:
    if (most_significant_units_are_hours || (!scanner.is_past_end() && scanner.current() == ':')) {
        // 1. If position is beyond the end of input or if the character at position is not a U+003A COLON character
        //    (:), then return an error and abort these steps. Otherwise, move position forwards one character.
        if (!scanner.consume(':'))
            return {};

        // 2. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
        string = scanner.collect_ascii_digits();

        // 3. If string is not exactly two characters in length, return an error and abort these steps.
        if (string.length_in_code_units() != 2)
            return {};

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
    if (!scanner.consume('.'))
        return {};

    // 14. Collect a sequence of code points that are ASCII digits, and let string be the collected substring.
    string = scanner.collect_ascii_digits();

    // 15. If string is not exactly three characters in length, return an error and abort these steps.
    if (string.length_in_code_units() != 3)
        return {};

    // 16. Interpret string as a base-ten integer. Let value4 be that integer.
    auto value4 = interpret_as_base_ten_integer(string);

    // 17. If value2 is greater than 59 or if value3 is greater than 59, return an error and abort these steps.
    if (value2 > 59 || value3 > 59)
        return {};

    // 18. Let result be value1×60×60 + value2×60 + value3 + value4∕1000.
    auto result = (static_cast<double>(value1) * 60 * 60) + (static_cast<double>(value2) * 60) + static_cast<double>(value3) + (static_cast<double>(value4) / 1000);

    // 19. Return result.
    return result;
}

// https://w3c.github.io/webvtt/#collect-webvtt-cue-timings-and-settings
bool collect_webvtt_cue_timings_and_settings(Utf16View input, ParsedCue& cue)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    StringScanner scanner { input };

    // 3. Skip whitespace.
    scanner.skip_whitespace();

    // 4. Collect a WebVTT timestamp. If that algorithm fails, then abort these steps and return failure. Otherwise, let
    //    cue's text track cue start time be the collected time.
    auto start_time = collect_a_webvtt_timestamp(scanner);
    if (!start_time.has_value())
        return false;
    cue.start_time = *start_time;

    // 5. Skip whitespace.
    scanner.skip_whitespace();

    // 6. If the character at position is not a U+002D HYPHEN-MINUS character (-) then abort these steps and return
    //    failure. Otherwise, move position forwards one character.
    // 7. If the character at position is not a U+002D HYPHEN-MINUS character (-) then abort these steps and return
    //    failure. Otherwise, move position forwards one character.
    // 8. If the character at position is not a U+003E GREATER-THAN SIGN character (>) then abort these steps and return
    //    failure. Otherwise, move position forwards one character.
    if (!scanner.consume('-') || !scanner.consume('-') || !scanner.consume('>'))
        return false;

    // 9. Skip whitespace.
    scanner.skip_whitespace();

    // 10. Collect a WebVTT timestamp. If that algorithm fails, then abort these steps and return failure. Otherwise, let
    //     cue's text track cue end time be the collected time.
    auto end_time = collect_a_webvtt_timestamp(scanner);
    if (!end_time.has_value())
        return false;
    cue.end_time = *end_time;

    // FIXME: 11. Let remainder be the trailing substring of input starting at position.
    // FIXME: 12. Parse the WebVTT cue settings from remainder using regions for cue.
    return true;
}

bool is_block_keyword(Utf16View buffer, Utf16View keyword)
{
    if (!buffer.starts_with(keyword))
        return false;
    auto remainder = buffer.substring_view(keyword.length_in_code_units());
    for (size_t i = 0; i < remainder.length_in_code_units(); ++i) {
        if (!Infra::is_ascii_whitespace(remainder.code_unit_at(i)))
            return false;
    }
    return true;
}

// https://w3c.github.io/webvtt/#collect-a-webvtt-block
Optional<ParsedCue> Parser::collect_a_webvtt_block(bool in_header)
{
    // 1. Let input, position, seen cue and regions be the same variables as those of the same name in the algorithm
    //    that invoked these steps.

    // 2. Let line count be zero.
    size_t line_count = 0;

    // 3. Let previous position be position.
    auto previous_position = m_position;

    // 4. Let line be the empty string.
    Utf16View line;

    // 5. Let buffer be the empty string.
    Utf16StringBuilder buffer;

    // 6. Let seen EOF be false.
    bool seen_eof = false;

    // 7. Let seen arrow be false.
    bool seen_arrow = false;

    // 8. Let cue be null.
    Optional<ParsedCue> cue;

    // FIXME: 9. Let stylesheet be null.
    // FIXME: 10. Let region be null.

    // 11. Loop: Run these substeps in a loop:
    while (true) {
        // 1. Collect a sequence of code points that are not U+000A LINE FEED (LF) characters. Let line be those
        //    characters, if any.
        line = collect_a_sequence_of_code_points_that_are_not_line_feeds();

        // 2. Increment line count by 1.
        ++line_count;

        // 3. If position is past the end of input, let seen EOF be true. Otherwise, the character indicated by position
        //    is a U+000A LINE FEED (LF) character; advance position to the next character in input.
        if (is_past_end())
            seen_eof = true;
        else
            ++m_position;

        // 4. If line contains the three-character substring "-->" (U+002D HYPHEN-MINUS, U+002D HYPHEN-MINUS, U+003E
        //    GREATER-THAN SIGN), then run these substeps:
        if (line.contains(u"-->"sv)) {
            // 1. If in header is not set and at least one of the following conditions are true:
            //    - line count is 1
            //    - line count is 2 and seen arrow is false
            //    ...then run these substeps:
            if (!in_header && (line_count == 1 || (line_count == 2 && !seen_arrow))) {
                // 1. Let seen arrow be true.
                seen_arrow = true;

                // 2. Let previous position be position.
                previous_position = m_position;

                // 3. Cue creation: Let cue be a new WebVTT cue and initialize it as follows:
                //    Let cue's text track cue identifier be buffer.
                //    FIXME: Initialize the remaining cue fields once cue settings are supported.
                cue = ParsedCue {};
                cue->identifier = buffer.to_string();

                // 4. Collect WebVTT cue timings and settings from line using regions for cue. If that fails, let cue be
                //    null. Otherwise, let buffer be the empty string and let seen cue be true.
                if (!collect_webvtt_cue_timings_and_settings(line, *cue)) {
                    cue.clear();
                } else {
                    buffer.clear();
                    m_seen_cue = true;
                }
            }
            // Otherwise, let position be previous position and break out of loop.
            else {
                m_position = previous_position;
                break;
            }
        }
        // 5. Otherwise, if line is the empty string, break out of loop.
        else if (line.is_empty()) {
            break;
        }
        // 6. Otherwise, run these substeps:
        else {
            // 1. If in header is not set and line count is 2, run these substeps:
            if (!in_header && line_count == 2) {
                // 1. If seen cue is false and buffer starts with the substring "STYLE" (U+0053 LATIN CAPITAL LETTER S,
                //    U+0054 LATIN CAPITAL LETTER T, U+0059 LATIN CAPITAL LETTER Y, U+004C LATIN CAPITAL LETTER L, U+0045
                //    LATIN CAPITAL LETTER E), and the remaining characters in buffer (if any) are all ASCII whitespace,
                //    then run these substeps:
                // 2. Otherwise, if seen cue is false and buffer starts with the substring "REGION" (U+0052 LATIN CAPITAL
                //    LETTER R, U+0045 LATIN CAPITAL LETTER E, U+0047 LATIN CAPITAL LETTER G, U+0049 LATIN CAPITAL LETTER
                //    I, U+004F LATIN CAPITAL LETTER O, U+004E LATIN CAPITAL LETTER N), and the remaining characters in
                //    buffer (if any) are all ASCII whitespace, then run these substeps:
                if (!m_seen_cue && (is_block_keyword(buffer.view(), u"STYLE"sv) || is_block_keyword(buffer.view(), u"REGION"sv))) {
                    // FIXME: Create the CSS style sheet or WebVTT region.
                    // Let buffer be the empty string.
                    buffer.clear();
                }
            }

            // 2. If buffer is not the empty string, append a U+000A LINE FEED (LF) character to buffer.
            if (!buffer.is_empty())
                buffer.append_code_unit('\n');

            // 3. Append line to buffer.
            buffer.append(line);

            // 4. Let previous position be position.
            previous_position = m_position;
        }

        // 7. If seen EOF is true, break out of loop.
        if (seen_eof)
            break;
    }

    // 12. If cue is not null, let the cue text of cue be buffer, and return cue.
    if (cue.has_value()) {
        cue->text = buffer.to_string();
        return cue;
    }

    // FIXME: 13. Otherwise, if stylesheet is not null, then Parse a stylesheet from buffer. If it returned a list of
    //            rules, assign the list as stylesheet's CSS rules; otherwise, set stylesheet's CSS rules to an empty
    //            list. Finally, return stylesheet.
    // FIXME: 14. Otherwise, if region is not null, then collect WebVTT region settings from buffer using region for the
    //            results. Construct a WebVTT Region Object from region, and return it.

    // 15. Otherwise, return null.
    return {};
}

// https://w3c.github.io/webvtt/#webvtt-parser-algorithm
Optional<Vector<ParsedCue>> Parser::parse()
{
    Vector<ParsedCue> output;

    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    // 3. Let seen cue be false.

    // 4. If input is less than six characters long, then abort these steps. The file does not start with the correct
    //    WebVTT file signature and was therefore not successfully processed.
    if (m_input.length_in_code_units() < 6)
        return {};

    // 5. If input is exactly six characters long but does not exactly equal "WEBVTT", then abort these steps. The file
    //    does not start with the correct WebVTT file signature and was therefore not successfully processed.
    // 6. If input is more than six characters long but the first six characters do not exactly equal "WEBVTT", or the
    //    seventh character is not a U+0020 SPACE character, a U+0009 CHARACTER TABULATION (tab) character, or a U+000A
    //    LINE FEED (LF) character, then abort these steps. The file does not start with the correct WebVTT file
    //    signature and was therefore not successfully processed.
    if (!m_input.starts_with(u"WEBVTT"sv))
        return {};
    if (m_input.length_in_code_units() > 6 && !first_is_one_of(m_input.code_unit_at(6), ' ', '\t', '\n'))
        return {};

    // 7. Collect a sequence of code points that are not U+000A LINE FEED (LF) characters.
    (void)collect_a_sequence_of_code_points_that_are_not_line_feeds();

    // 8. If position is past the end of input, then abort these steps. The file was successfully processed, but it
    //    contains no useful data and so no WebVTT cues were added to output.
    if (is_past_end())
        return output;

    // 9. The character indicated by position is a U+000A LINE FEED (LF) character. Advance position to the next
    //    character in input.
    ++m_position;

    // 10. If position is past the end of input, then abort these steps. The file was successfully processed, but it
    //     contains no useful data and so no WebVTT cues were added to output.
    if (is_past_end())
        return output;

    // 11. Header: If the character indicated by position is not a U+000A LINE FEED (LF) character, then collect a WebVTT
    //     block with the in header flag set. Otherwise, advance position to the next character in input.
    if (current() != '\n')
        (void)collect_a_webvtt_block(true);
    else
        ++m_position;

    // 12. Collect a sequence of code points that are U+000A LINE FEED (LF) characters.
    collect_a_sequence_of_line_feeds();

    // FIXME: 13. Let regions be an empty text track list of regions.

    // 14. Block loop: While position doesn't point past the end of input:
    while (!is_past_end()) {
        // 1. Collect a WebVTT block, and let block be the returned value.
        auto block = collect_a_webvtt_block(false);

        // 2. If block is a WebVTT cue, add block to the text track list of cues output.
        if (block.has_value())
            output.append(block.release_value());

        // FIXME: 3. Otherwise, if block is a CSS style sheet, add block to stylesheets.
        // FIXME: 4. Otherwise, if block is a WebVTT region object, add block to regions.

        // 5. Collect a sequence of code points that are U+000A LINE FEED (LF) characters.
        collect_a_sequence_of_line_feeds();
    }

    // 15. End: The file has ended. Abort these steps. The WebVTT parser has finished. The file was successfully
    //     processed.
    return output;
}

}

// https://w3c.github.io/webvtt/#webvtt-parser
Optional<Vector<ParsedCue>> parse_webvtt_file(ReadonlyBytes bytes)
{
    // A WebVTT parser, given an input byte stream, a text track list of cues output, and a collection of CSS style
    // sheets stylesheets, must decode the byte stream using the UTF-8 decode algorithm, and then must parse the resulting
    // string according to the WebVTT parser algorithm below.
    auto decoded = Utf16String::from_utf8_with_replacement_character(StringView { bytes }, Utf16String::WithBOMHandling::Yes);

    // 1. Let input be the string being parsed, after conversion to Unicode, and with the following transformations
    //    applied:
    //    - Replace all U+0000 NULL characters by U+FFFD REPLACEMENT CHARACTERs.
    //    - Replace each U+000D CARRIAGE RETURN U+000A LINE FEED (CRLF) character pair by a single U+000A LINE FEED (LF)
    //      character.
    //    - Replace all remaining U+000D CARRIAGE RETURN characters by U+000A LINE FEED (LF) characters.
    auto view = decoded.utf16_view();
    Utf16StringBuilder builder;
    for (size_t i = 0; i < view.length_in_code_units(); ++i) {
        auto code_unit = view.code_unit_at(i);
        if (code_unit == 0) {
            builder.append_code_unit(0xFFFD);
        } else if (code_unit == '\r') {
            if (i + 1 < view.length_in_code_units() && view.code_unit_at(i + 1) == '\n')
                ++i;
            builder.append_code_unit('\n');
        } else {
            builder.append_code_unit(code_unit);
        }
    }
    auto input = builder.to_string();

    Parser parser { input.utf16_view() };
    return parser.parse();
}

}
