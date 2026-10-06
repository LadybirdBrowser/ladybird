/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The date string parser of Date.parse and the Date constructor.

// Parse simplified ISO8601 and non-standard date formats to milliseconds
// from epoch (double). Synopsis:
// (1) Try to parse the string as simplified ISO8601 (case sensitive).
//   - if that worked, assemble result (4 below)
//   - hard fail (return NAN) if the input string "looks like" ISO8601,
//     but deviates.
// (2) If parsing ISO8601 "soft" fails (unlike "hard" above), continue
//     shallow parsing (case insensitive) date string components: time,
//     timezone, keywords, numbers...
// (3) Guess ambiguous date parts, like "1/2/3" --> Jan 2, 2003
// (4) Assemble result from parts (year, month,...) and convert to milliseconds
//     from epoch.
//
// Overall objectives:
// - compliance with ECMA date time string format, incl. ISO8601 extensions
//   for signed 6-digit year and extended time offset format (:SS.nanosecs)
//   https://tc39.es/ecma262/#sec-date-time-string-format
// - Support for Date.toString and Date.toUTCString formats.
// - Compatible with Mozilla Firefox 134.0.1 and Chromium 131.0.6778.264. Within
//   reason. Differences indicated in comments.
//   - Where Firefox and Chrome agree on a parse, support it.
//   - Where they disagree, support the one that seems more sane.
//   - In very limited cases, pick our own way. Example: "<number> Month":
//     - Firefox fails on all "<number> Month" date strings.
//     - In most cases, Chrome interprets "<number> Month" as "Month 01, Year".
//     - Chrome parses "7 Feb" as "Feb 7, 2001".
//     - We always parse as "Month 01, Year".
// - Support Firefox less permissive punctuation but more permissive punctuation
//   syntax.

use crate::runtime::date::{days_since_epoch, time_clip, utc_time};
use crate::utf16::Utf16View;

/// A hard failure, after which the parse returns NaN.
type ParseError = &'static str;

#[derive(Clone, Copy)]
struct ParsedNumber {
    number: u64,
    digits: usize,
}

fn is_ascii_digit(character: u8) -> bool {
    character.is_ascii_digit()
}

fn is_ascii_alpha(character: u8) -> bool {
    character.is_ascii_alphabetic()
}

/// AK's is_ascii_space, which unlike u8::is_ascii_whitespace includes the vertical tab.
fn is_ascii_space(character: u8) -> bool {
    matches!(character, b' ' | b'\t' | b'\n' | 0x0B | 0x0C | b'\r')
}

fn pow10(pow: usize) -> u64 {
    assert!(pow <= 9, "VERIFY_NOT_REACHED");
    10_u64.pow(pow as u32)
}

pub struct DateParser {
    /// The input, all ASCII, as a Utf16GenericLexer over it would see it.
    input: Vec<u8>,
    index: usize,

    numbers: Vec<u64>,

    year: Option<i64>,
    month: Option<u8>,
    day: Option<u8>,
    hours: Option<u8>,
    minutes: Option<u8>,
    seconds: Option<u8>,
    milliseconds: Option<u16>,

    // True if GMT/UTC/Z specified and there is no timezone offset; false if timezone offset.
    // No value if there is no timezone information: we have to guess whether the date was given in GMT or local time.
    timezone_utc: Option<bool>,
    timezone_sign: Option<i8>, // +1 or -1 if there is a timezone offset.
    timezone_hours: Option<u8>,
    timezone_minutes: Option<u8>,
    timezone_seconds: Option<u8>,
    timezone_nanoseconds: Option<u64>,

    date_is_iso_like: bool,
    previous_separator_was_space: bool,
}

impl DateParser {
    pub fn parse(string: Utf16View<'_>) -> f64 {
        if !string.code_units().all(|code_unit| code_unit < 0x80) {
            return f64::NAN;
        }
        let input = string.code_units().map(|code_unit| code_unit as u8).collect();
        DateParser::new(input).parse_date().unwrap_or(f64::NAN)
    }

    fn new(input: Vec<u8>) -> Self {
        Self {
            input,
            index: 0,
            numbers: Vec::new(),
            year: None,
            month: None,
            day: None,
            hours: None,
            minutes: None,
            seconds: None,
            milliseconds: None,
            timezone_utc: None,
            timezone_sign: None,
            timezone_hours: None,
            timezone_minutes: None,
            timezone_seconds: None,
            timezone_nanoseconds: None,
            date_is_iso_like: false,
            previous_separator_was_space: false,
        }
    }

    fn is_eof(&self) -> bool {
        self.index >= self.input.len()
    }

    /// The character `offset` after the current one, or NUL past the end of the input, like GenericLexer::peek().
    fn peek(&self, offset: usize) -> u8 {
        self.input.get(self.index + offset).copied().unwrap_or(0)
    }

    fn next_is(&self, predicate: impl Fn(u8) -> bool) -> bool {
        predicate(self.peek(0))
    }

    fn consume(&mut self) -> u8 {
        assert!(!self.is_eof(), "!is_eof()");
        let character = self.input[self.index];
        self.index += 1;
        character
    }

    fn ignore(&mut self) {
        if !self.is_eof() {
            self.index += 1;
        }
    }

    fn consume_specific(&mut self, expected: u8) -> bool {
        if self.peek(0) != expected {
            return false;
        }
        self.ignore();
        true
    }

    fn consume_specific_string(&mut self, expected: &str) -> bool {
        if !self.input[self.index.min(self.input.len())..].starts_with(expected.as_bytes()) {
            return false;
        }
        self.index += expected.len();
        true
    }

    /// Consumes the characters that satisfy `predicate`, and returns whether there were any.
    fn consume_while(&mut self, predicate: impl Fn(u8) -> bool) -> bool {
        let start = self.index;
        while !self.is_eof() && predicate(self.peek(0)) {
            self.index += 1;
        }
        self.index != start
    }

    fn ignore_while(&mut self, predicate: impl Fn(u8) -> bool) {
        self.consume_while(predicate);
    }

    fn ignore_until(&mut self, stop: u8) {
        while !self.is_eof() && self.peek(0) != stop {
            self.index += 1;
        }
    }

    // Reads a contiguous sequence of digits. Ignores digits read past MaxLength.
    // Returns the numeric value of the sequence and the number of digits not ignored.
    fn read_number(&mut self, max_length: usize) -> ParsedNumber {
        let mut result = ParsedNumber { number: 0, digits: 0 };

        while result.digits < max_length {
            if self.is_eof() || !self.next_is(is_ascii_digit) {
                return result;
            }
            result.number *= 10;
            result.number += u64::from(self.consume() - b'0');
            result.digits += 1;
        }

        self.ignore_while(is_ascii_digit);
        result
    }

    fn guess_date_from_numbers(&mut self) -> bool // Guess an ambiguous date, like 1/1/1.
    {
        if self.year.is_some() && self.month.is_some() && self.day.is_some() {
            return self.numbers.is_empty();
        }

        match self.numbers.len() {
            0 => true, // The year and possibly month may have already been calculated. Verify later
            1 => self.guess_date_from_1_number(),
            2 => self.guess_date_from_2_numbers(),
            3 => self.guess_date_from_3_numbers(),
            _ => false, // Too many numbers
        }
    }

    fn guess_date_from_3_numbers(&mut self) -> bool // Only "month-day-year" (default) and "year-month-day" are supported (same as firefox and chrome).
    {
        assert!(self.numbers.len() == 3);

        let number0 = self.numbers[0];
        let number1 = self.numbers[1];
        let number2 = self.numbers[2];

        if self.year.is_some() || self.month.is_some() {
            return false; // Too many numbers
        }

        if number0 > 31 || number0 == 0 {
            // YMD
            if number1 > 12 {
                return false;
            }
            if number2 > 31 {
                return false;
            }

            if number1 == 0 || number2 == 0 {
                // 0 for day or month
                return false;
            }

            self.month = Some(number1 as u8);
            self.day = Some(number2 as u8);
            self.year = Some(guess_year(number0) as i64);

            return true;
        }

        // MDY
        if number0 > 12 {
            return false; // Both Firefox and Chrome fail for the first number >12 and <=31. Weird. We do the same
        }
        if number1 > 31 {
            return false;
        }

        if number0 == 0 || number1 == 0 {
            // 0 for day or month
            return false;
        }

        self.month = Some(number0 as u8);
        self.day = Some(number1 as u8);
        self.year = Some(guess_year(number2) as i64);

        true
    }

    fn guess_date_from_2_numbers(&mut self) -> bool // Guess default order "day-year" or adapt.
    {
        assert!(self.numbers.len() == 2);

        let number0 = self.numbers[0];
        let number1 = self.numbers[1];

        if self.year.is_some() && self.month.is_some() {
            return false; // Too many numbers
        }

        if self.year.is_some() {
            if number0 <= 12 {
                // ... is a month
                if number1 > 31 {
                    return false;
                }
                if number0 == 0 || number1 == 0 {
                    // 0 for day or month
                    return false;
                }

                self.month = Some(number0 as u8);
                self.day = Some(number1 as u8);
                return true;
            }

            if number0 <= 31 {
                // ... is a day
                if number1 > 12 {
                    return false;
                }

                if number0 == 0 || number1 == 0 {
                    // 0 for day or month
                    return false;
                }

                self.month = Some(number1 as u8);
                self.day = Some(number0 as u8);
                return true;
            }
        }

        // At this point, one of the numbers is the year
        if self.month.is_none() {
            return false; // Firefox fails on guessing 2 numbers. We do the same
        }

        // At this point, the month has been read from a month name
        if number0 > 31 && number1 > 31 {
            return false; // Neither of the numbers can be a day
        }
        if number0 > 31 || number0 == 0 {
            // ... is a year
            if number1 == 0 {
                // 0 for day
                return false;
            }

            self.day = Some(number1 as u8);
            self.year = Some(guess_year(number0) as i64);
            return true;
        }

        // Default order is day -> year
        if number0 == 0 {
            // 0 for day
            return false;
        }

        self.day = Some(number0 as u8);
        self.year = Some(guess_year(number1) as i64);

        true
    }

    fn guess_date_from_1_number(&mut self) -> bool // Guess in this order: year, month, day.
    {
        assert!(self.numbers.len() == 1);

        let number0 = self.numbers[0];

        if self.year.is_none() && self.month.is_none() {
            return self.one_number(number0);
        }

        if self.year.is_none() {
            // the number must be a year
            self.year = Some(guess_year(number0) as i64);
            return true;
        }

        if self.month.is_none() {
            // Firefox fails on two numbers. So do we. Chrome is weird.
            return false;
        }

        // At this point, the year and month must have been specified some other way (e.g. "Feb +002002").
        if number0 > 31 || number0 == 0 {
            // Invalid day number.
            return false;
        }

        self.day = Some(number0 as u8);

        true
    }

    fn one_number(&mut self, number: u64) -> bool // The whole input string is just one stand-alone number.
    {
        match number {
            0 => {
                self.year = Some(2000);
                true
            }
            1..=12 => {
                self.year = Some(2001);
                self.month = Some(number as u8); // Firefox and Chrome interpret standalone numbers below 12 as months in 2001. Weird! We do the same.
                true
            }
            13..=31 => false, // Firefox and Chrome fail on standalone numbers between 12 and 31. Weird! We do the same.
            32..=49 => {
                self.year = Some(2000 + number as i64);
                true
            }
            50..=99 => {
                self.year = Some(1900 + number as i64);
                true
            }
            _ => {
                self.year = Some(number as i64);
                true
            }
        }
    }

    // Permissive, greedy shallow date parser for date components.
    // Returns:
    // - false: Hard fail. Some invalid input condition has been found. Caller should return NAN.
    // - true: Parsing can continue.
    fn loop_(&mut self) -> bool {
        match self.peek(0) {
            b'0'..=b'9' => self.maybe_number(), // Also captures time.
            b'+' | b'-' => self.maybe_sign(),   // Also captures signed 6-digit year and timezone offset.
            b'A'..=b'Z' => self.maybe_word(), // Captures all date string "keywords". Accepts any kind of "junk" before date and time..
            b' ' => {
                self.previous_separator_was_space = true;
                self.ignore();
                true
            }
            b'.' | b',' | b'/' => {
                // Firefox seems to accept (ignore) this punctuation. So do we.
                // Firefox also accepts a bare '+' sometimes. We do not.
                // Chrome is a lot more permissive.
                self.previous_separator_was_space = false;
                self.ignore(); // Ignore punctuation.
                true
            }
            b'(' => {
                self.ignore_until(b')'); // Consume time zone name (Anything in brackets).
                self.ignore();
                self.ignore_while(is_ascii_space);
                true
            }
            _ => false,
        }
    }

    fn maybe_ampm(&mut self) -> bool // Side effect: consumes space at the end of a time component, even if there is no AM/PM.
    {
        let hours = self.hours.expect("m_hours.has_value()");

        self.consume_while(is_ascii_space);

        if self.consume_specific_string("AM") {
            if !self.separator() {
                return false; // 12:34 AMsomething
            }

            if hours > 12 {
                return false; // 14:45AM
            }
            if hours == 12 {
                self.hours = Some(0); // 12:05AM -> 00:05
            }
            return true;
        }

        if self.consume_specific_string("PM") {
            if !self.separator() {
                return false; // 12:34 PMsomething
            }
            if hours > 12 {
                return false; // 14:45PM
            }
            if hours < 12 {
                self.hours = Some(hours + 12);
            }
            return true;
        }

        true
    }

    // H[H]:MM[:SS[.mss[...]]][ ][AM|PM] At this point, H[H]: has already been read.
    fn maybe_time(&mut self, hours: ParsedNumber, allow_single_digit_minutes_and_seconds: bool) -> bool {
        if self.hours.is_some() {
            return false; // Time has already been read.
        }

        if hours.digits > 2 {
            return false; // 123:
        }
        if hours.number > 24 {
            return false;
        }
        self.hours = Some(hours.number as u8); // No precision loss during the conversion u32 -> u8 since the hours has at most 2 digits.

        let minutes = self.read_number(3);
        if minutes.digits != 2 && !(allow_single_digit_minutes_and_seconds && minutes.digits == 1) {
            return false; // 12:345 or 12:3
        }
        if minutes.number > 59 {
            return false;
        }
        self.minutes = Some(minutes.number as u8);

        if self.consume_specific(b'.') {
            return false; // 12:34.
        }
        if !self.consume_specific(b':') {
            return true;
        }

        let seconds = self.read_number(3);
        if seconds.digits != 2 && !(allow_single_digit_minutes_and_seconds && seconds.digits == 1) {
            return false; // 12:34:567 or 12:34:5
        }
        if seconds.number > 59 {
            return false;
        }
        self.seconds = Some(seconds.number as u8);

        if !self.consume_specific(b'.') {
            return true;
        }

        let milliseconds = self.read_number(3);
        let milliseconds = milliseconds.number as u16
            * match milliseconds.digits {
                0 => return false, // 12:34:56.
                1 => 100,
                2 => 10,
                3 => 1,
                _ => unreachable!("VERIFY_NOT_REACHED"),
            };
        self.milliseconds = Some(milliseconds);

        true
    }

    fn maybe_number(&mut self) -> bool {
        let previous_separator_was_space = self.previous_separator_was_space;
        self.previous_separator_was_space = false;

        let number = self.read_number(7);
        assert!(number.digits > 0);

        if number.digits > 6 {
            return false; // 1234567
        }

        if self.consume_specific(b':') {
            if !self.maybe_time(number, self.date_is_iso_like && previous_separator_was_space) {
                return false;
            }
            if !self.maybe_ampm() {
                return false;
            }
            return true;
        }

        self.numbers.push(number.number);

        self.separator() // Must be followed by a separator.
    }

    fn maybe_sign(&mut self) -> bool {
        let sign: i8 = if self.consume() == b'-' { -1 } else { 1 };
        let number = self.read_number(7);

        match number.digits {
            0 => {
                // Not a sign after all; '+' is forbidden if it is not a sign. '-' is ignored as punctuation.
                return sign == -1;
            }
            1..=5 => {
                // Too small to be a signed year;
                if self.hours.is_none() {
                    // Otherwise a candidate for timezone offset.
                    self.numbers.push(number.number); // Ignore the sign and treat it as a number.
                    return true;
                }
            }
            6 => {
                if self.hours.is_none() {
                    // Otherwise a candidate for timezone offset.
                    if self.year.is_some() {
                        return false; // To many digits to be anything else than a signed year.
                    }

                    self.year = Some(i64::from(sign) * number.number as i64); // Candidate for signed year
                    return true;
                }
            }
            _ => return false,
        }

        self.timezone_sign = Some(sign);
        self.tz_offset_with_number(number, false)
    }

    fn tz_offset(&mut self) -> bool // Read a full timezone offset, including the sign.
    {
        self.timezone_sign = Some(if self.consume() == b'-' { -1 } else { 1 });
        let number = self.read_number(7);
        self.tz_offset_with_number(number, false)
    }

    // Continue reading a timezone offset, after the sign and the first number have beeen read.
    fn tz_offset_with_number(&mut self, number: ParsedNumber, iso_8601_format: bool) -> bool {
        if self.timezone_hours.is_some() {
            // Cannot have more than one timezone offset or a timezone name followed by a timezone offset.
            return false;
        }

        self.timezone_utc = Some(false);
        match number.digits {
            0 => return false,
            1 | 2 => {
                if number.digits == 1 && iso_8601_format {
                    return false; // +1
                }
                // Candidate for timezone offset with colon.
            }
            3 | 4 => {
                if number.digits == 3 && iso_8601_format {
                    return false; // +123
                }
                // "Military" timezone offset
                let tz_hhmm = number.number as u16;
                self.timezone_hours = Some((tz_hhmm / 100) as u8);
                self.timezone_minutes = Some((tz_hhmm % 100) as u8);
                return true;
            }
            5 | 6 => {
                if number.digits == 5 && iso_8601_format {
                    return false; // +12345
                }
                // "Military" timezone offset
                let tz_hhmmss = number.number as u32;
                self.timezone_hours = Some((tz_hhmmss / 10000) as u8);
                self.timezone_minutes = Some((tz_hhmmss % 10000 / 100) as u8);
                self.timezone_seconds = Some((tz_hhmmss % 100) as u8);
                return true;
            }
            _ => return false,
        }

        let timezone_offset_requires_colon = iso_8601_format && number.digits == 2;
        self.timezone_hours = Some(number.number as u8); // Guaranteed to be a 1- or 2-digit number.

        if !self.consume_specific(b':') {
            return !timezone_offset_requires_colon; // +H[H] "military" time offset
        }

        // Timezone with colon
        let minutes = self.read_number(3);
        if minutes.digits != 2 {
            return false;
        }
        self.timezone_minutes = Some(minutes.number as u8);

        if !self.consume_specific(b':') {
            return true;
        }

        let seconds = self.read_number(3);
        if seconds.digits != 2 {
            return false;
        }
        self.timezone_seconds = Some(seconds.number as u8);

        if !self.consume_specific(b'.') {
            return true;
        }

        let nanoseconds = self.read_number(3);
        if nanoseconds.digits == 0 {
            return false;
        }
        if nanoseconds.digits > 9 {
            return false;
        }

        self.timezone_nanoseconds = Some(nanoseconds.number * pow10(9 - nanoseconds.digits));

        true
    }

    fn separator(&mut self) -> bool // Ignore space and Firefox punctuation.
    {
        if self.is_eof() {
            return true;
        }
        match self.peek(0) {
            b' ' => {
                self.previous_separator_was_space = true;
                self.ignore();
                true
            }
            b',' | b'.' | b'/' | b'-' => {
                self.previous_separator_was_space = false;
                self.ignore();
                true
            }
            _ => false,
        }
    }

    fn gmt(&mut self, string: &str) -> bool // Z or GMT or UTC can be used interchangeably.
    {
        if !self.consume_specific_string(string) {
            return false;
        }

        self.timezone_utc = Some(true);

        let space = self.consume_while(is_ascii_space);
        if matches!(self.peek(0), b'+' | b'-') {
            return self.tz_offset(); // GMT+1234
        }

        space || self.separator()
    }

    // Same as Chrome and Firefox, we only support abbreviations for timezones covering the US mainland.
    fn us_timezone(&mut self, string: &str, hours: u8) -> bool {
        const MINUTES: u8 = 0;
        const SIGN: i8 = -1;

        assert!(string.len() == 3); // only 3-letter timezone names
        if !self.consume_specific_string(string) {
            return false;
        }

        if self.hours.is_none() && self.numbers.is_empty() && self.year.is_none() {
            // Ignore timezone before date or time.
            return false;
        }

        self.timezone_sign = Some(SIGN);
        self.timezone_hours = Some(hours);
        self.timezone_minutes = Some(MINUTES);

        self.separator() // Must end with a separator.
    }

    fn month_name(&mut self, string: &str, month: u8) -> bool {
        assert!(string.len() == 3); // Only looking for 3-letter month prefixes.
        for (i, character) in string.bytes().enumerate() {
            if character != self.peek(i) {
                return false;
            }
        }

        self.ignore_while(is_ascii_alpha); // ... which can followed by anything. Just like Firefox and Chrome.
        self.month = Some(month);

        self.separator() // Must end with a separator.
    }

    fn word(&mut self) -> bool // Alphanumeric strings that are not date "keywords".
    {
        self.consume_while(is_ascii_alpha);
        // Just like Firefox and Chrome:
        // - Ignore junk (bare words) at the beginning (before time or a date fragment has been read).
        // - Fail if a word is read later in the date string (exception: final time zone name, in brackets).
        self.numbers.is_empty() && self.hours.is_none() && self.year.is_none()
    }

    fn maybe_word(&mut self) -> bool // The top of a trie catching date "keywords".
    {
        match self.peek(0) {
            // APR AUG
            b'A' => self.month_name("APR", 4) || self.month_name("AUG", 8) || self.word(),
            // CST CDT
            b'C' => self.us_timezone("CST", 6) || self.us_timezone("CDT", 5) || self.word(),
            // DEC
            b'D' => self.month_name("DEC", 12) || self.word(),
            // EST EDT
            b'E' => self.us_timezone("EST", 5) || self.us_timezone("EDT", 4) || self.word(),
            // FEB
            b'F' => self.month_name("FEB", 2) || self.word(),
            // GMT
            b'G' => self.gmt("GMT") || self.word(),
            // JAN JUN JUL
            b'J' => self.month_name("JAN", 1) || self.month_name("JUN", 6) || self.month_name("JUL", 7) || self.word(),
            // MAR MAY MST MDT
            b'M' => {
                self.month_name("MAR", 3)
                    || self.month_name("MAY", 5)
                    || self.us_timezone("MST", 7)
                    || self.us_timezone("MDT", 6)
                    || self.word()
            }
            // NOV
            b'N' => self.month_name("NOV", 11) || self.word(),
            // OCT
            b'O' => self.month_name("OCT", 10) || self.word(),
            // PST PDT
            b'P' => self.us_timezone("PST", 8) || self.us_timezone("PDT", 7) || self.word(),
            // SEP
            b'S' => self.month_name("SEP", 9) || self.word(),
            // UTC
            b'U' => self.gmt("UTC") || self.word(),
            // Z
            b'Z' => self.gmt("Z") || self.word(),
            _ => self.word(),
        }
    }

    // Capture simplified ISO8601 date format. https://tc39.es/ecma262/#sec-date-time-string-format
    // All of the iso_... functions return:
    // - true: if the input can be parsed as an ISO8601 date.
    // - false: cannot be parsed as an ISO8601 date. Will be defered to a non-standard date string.
    // - Error: hard fail; caller is supposed to return NAN.
    fn maybe_iso_8601(&mut self) -> Result<bool, ParseError> {
        for step in 0..4 {
            match step {
                0 => {
                    if !self.maybe_iso_year()? {
                        return Ok(false);
                    }
                }
                1 => {
                    if !self.maybe_iso_month_day()? {
                        return Ok(false);
                    }
                }
                2 => {
                    if !self.maybe_iso_time()? {
                        return Ok(false);
                    }
                }
                _ => self.maybe_iso_tz()?,
            }

            if self.is_eof() {
                return Ok(true);
            }
        }

        Err("Read ISO8601 format, but have some input left over.")
    }

    fn maybe_iso_year(&mut self) -> Result<bool, ParseError> {
        match self.peek(0) {
            b'0'..=b'9' => {
                if self.maybe_iso_year4()? {
                    return Ok(true);
                }
            }
            b'+' | b'-' => {
                if self.maybe_iso_signed_year6()? {
                    return Ok(true);
                }
            }
            _ => return Ok(false),
        }

        Ok(false)
    }

    fn maybe_iso_year4(&mut self) -> Result<bool, ParseError> // Like "2025".
    {
        let number = self.read_number(7);

        match number.digits {
            0 => Ok(false), // no digits
            1 | 2 if self.next_is(|character| character == b':') => {
                // This may not be a year after all but the start of a "time" component.
                self.ignore();
                if !self.maybe_time(number, false) {
                    return Err("Cannot parse time.");
                }
                if !self.maybe_ampm() {
                    return Err("Cannot parse am/pm.");
                }
                Ok(false)
            }
            1 | 2 | 3 | 5 | 6 => {
                // Six-digit year number; no sign means not ISO8601 date.
                // At this point, this is not an ISO8601 date.
                self.numbers.push(number.number);
                Ok(false)
            }
            4 => {
                // Four-digit year number.
                self.year = Some(number.number as i64);
                Ok(true) // This can be the start of an ISO8601 date.
            }
            _ => Err("String too long to be a year."),
        }
    }

    fn maybe_iso_signed_year6(&mut self) -> Result<bool, ParseError> // Like "+002025".
    {
        let mut sign: i64 = 1;
        if self.consume() == b'-' {
            sign = -1;
        }

        let ParsedNumber { number, digits } = self.read_number(7);

        match digits {
            0 => {
                if sign == 1 {
                    // Standalone '+' is invalid.
                    return Err("Invalid character in date string ('+').");
                }
                return Ok(false);
            }
            1..=5 => {
                self.numbers.push(number); // This is not an ISO8601 date.
                return Ok(false);
            }
            6 => {}
            _ => return Err("String too long to be a 6-digit signed year."),
        }

        self.year = Some(sign * number as i64);

        // "The representation of the year 0 as -000000 is invalid." https://tc39.es/ecma262/#sec-expanded-years
        // Firefox interprets "-000000" as "Jan 1, 2000".
        if sign == -1 && self.year == Some(0) {
            return Err("The representation of the year 0 as '-000000' is invalid.");
        }

        Ok(true)
    }

    fn maybe_iso_month_day(&mut self) -> Result<bool, ParseError> // [-MM[-DD]]
    {
        if !self.consume_specific(b'-') {
            return Ok(true);
        }

        self.date_is_iso_like = true;

        let month = self.read_number(3);
        if self.consume_specific(b':') {
            // Like "2000-12:34". Firefox and Chrome parse it correctly. We do the same.
            if self.maybe_time(month, false) {
                return Ok(false);
            }
            return Err("Found something that looks like time, but is not.");
        }

        match month.digits {
            0 => return Ok(false), // Not ISO8601 date format. Continue reading.
            1 => {
                self.month = Some(month.number as u8);
                if self.month == Some(0) {
                    return Err("Month number cannot be zero.");
                }
                return Ok(false);
            }
            2 => {}
            _ => return Err("Month number too long."), // month number too long
        }

        self.month = Some(month.number as u8);

        if self.month == Some(0) || self.month.is_some_and(|month| month > 12) {
            return Err("Invalid month number.");
        }

        if !self.consume_specific(b'-') {
            return Ok(true);
        }

        if self.is_eof() {
            return Err("Expecting month number. Got eof.");
        }

        let day = self.read_number(3);

        match day.digits {
            0 => return Ok(false), // Not ISO8601 date format. Continue reading.
            1 => {
                self.day = Some(day.number as u8);
                if self.day == Some(0) {
                    return Err("Day number cannot be zero.");
                }
                return Ok(false);
            }
            2 => {}
            _ => return Err("Day number too long."), // month number too long
        }

        self.day = Some(day.number as u8);
        if self.day == Some(0) || self.day.is_some_and(|day| day > 31) {
            return Err("Invalid day number.");
        }

        Ok(true)
    }

    fn maybe_iso_time(&mut self) -> Result<bool, ParseError> // THH:MM[:SS[.M[SS...]]]
    {
        // The ECMA date string format requires uppercase 'T' and 'Z' https://tc39.es/ecma262/#sec-date-time-string-format
        // - Chrome supports lower case occurrences.
        // - Firefox and us do not.
        if !self.consume_specific(b'T') {
            return Ok(false);
        }

        // After reading the 'T', any failures return NAN (failed parse)
        let hours = self.read_number(3);

        if !self.consume_specific(b':') {
            // 12
            return Err("Well specified time needs minutes.");
        }

        if hours.digits != 2 {
            return Err("Hours: invalid length.");
        }

        if !self.maybe_time(hours, false) {
            // The only difference is that ISO8601 requires 2-digit hours.
            return Err("Cannot parse time.");
        }

        Ok(true)
    }

    fn maybe_iso_tz(&mut self) -> Result<(), ParseError> // After the 'T' for iso_time has been read, reading an ISO8601 timezone either succeeds or the whole parse fails.
    {
        match self.consume() {
            b'Z' => {
                self.timezone_utc = Some(true);
                return Ok(());
            }
            b'-' => self.timezone_sign = Some(-1),
            b'+' => self.timezone_sign = Some(1),
            _ => return Err("Invalid timezone offset format."),
        }

        let number = self.read_number(7);
        if !self.tz_offset_with_number(number, true) {
            // A sign and a number have been read. Continue reading a timezone offset..
            return Err("Invalid timezone offset format.");
        }

        Ok(())
    }

    fn build_date(&self, is_iso8601_date: bool) -> f64 // Build a date (milliseconds since epoch) from parts collected.
    {
        assert!(self.is_eof(), "is_eof()");

        let Some(year) = self.year else {
            return f64::NAN; // Needs at least one year.
        };

        if self.hours == Some(24) && (self.minutes.unwrap_or(0) > 0 || self.seconds.unwrap_or(0) > 0) {
            return f64::NAN; // 24:01:02
        }

        // local time
        // AK::UnixDateTime::from_unix_time_parts(), whose year is an i32.
        let year = year as i32;
        let days = days_since_epoch(
            year,
            i32::from(self.month.unwrap_or(1)),
            i32::from(self.day.unwrap_or(1)),
        );
        let seconds_since_epoch = days * 86_400
            + i64::from(self.hours.unwrap_or(0)) * 3_600
            + i64::from(self.minutes.unwrap_or(0)) * 60
            + i64::from(self.seconds.unwrap_or(0));
        let milliseconds_since_epoch = seconds_since_epoch * 1_000 + i64::from(self.milliseconds.unwrap_or(0));

        let mut time_ms = milliseconds_since_epoch as f64; // Assume the date was given in UTC.

        if let Some(timezone_sign) = self.timezone_sign {
            // A timezone offset was specified.
            if self.timezone_hours.is_some_and(|hours| hours > 24) {
                return f64::NAN;
            }
            if self.timezone_minutes.is_some_and(|minutes| minutes > 59) {
                return f64::NAN;
            }
            if self.timezone_seconds.is_some_and(|seconds| seconds > 59) {
                return f64::NAN;
            }

            // Convert to a UTC timestamp: local timestamp minus timezone offset.
            let offset_milliseconds = u64::from(self.timezone_hours.unwrap_or(0)) * 3_600_000
                + u64::from(self.timezone_minutes.unwrap_or(0)) * 60_000
                + u64::from(self.timezone_seconds.unwrap_or(0)) * 1_000
                + self.timezone_nanoseconds.unwrap_or(0) / 1_000_000;
            time_ms -= 1.0 * f64::from(timezone_sign) * offset_milliseconds as f64;
        } else if self.timezone_utc.is_none() && (!is_iso8601_date || self.hours.is_some()) {
            // If a timezone offset or GMT/UTC/Z was not specified and:
            // - Either this is not an ISO8601 [simplified] date
            // - Or this is a date-time form [of an ISO8601 date].
            // https://tc39.es/ecma262/#sec-date.parse:
            // "When the UTC offset representation is absent, date-only forms are interpreted as a UTC time and date-time forms are interpreted as a local time."

            time_ms = utc_time(time_ms); // The date was given in local time; convert it to a UTC timestamp.
        } else {
            // An ISO8601 date-only form
            // nop; leave timestamp as UTC
        }

        time_clip(time_ms)
    }

    fn parse_date(mut self) -> Result<f64, ParseError> {
        if self.maybe_iso_8601()? {
            return Ok(self.build_date(true));
        }

        // Convert the input string to uppercase only ~after~ parsing ISO8601 failed.
        // The index stays exactly where it was before converting to uppercase.
        self.input.make_ascii_uppercase();

        while !self.is_eof() {
            if !self.loop_() {
                return Err("Cannot parse date components.");
            }
        }

        if !self.guess_date_from_numbers() {
            return Err("Cannot guess date.");
        }

        Ok(self.build_date(false))
    }
}

fn guess_year(number: u64) -> u64 // Guess one- or two-digit year.
{
    match number {
        0..=49 => 2000 + number,
        50..=99 => 1900 + number,
        _ => number,
    }
}
