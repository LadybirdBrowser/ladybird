/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The RFC 9557 / ISO 8601 grammar of Temporal: a backtracking parser whose parse results are views into the parsed
//! string.

use crate::runtime::temporal::date_equations::{epoch_time_for_year, mathematical_in_leap_year};
use crate::runtime::value_conversions::string_to_number;
use crate::utf16::Utf16View;

#[derive(Clone, Copy, Debug)]
pub struct Annotation<'a> {
    pub critical: bool,
    pub key: Utf16View<'a>,
    pub value: Utf16View<'a>,
}

#[derive(Clone, Copy, Debug)]
pub struct TimeZoneOffset<'a> {
    pub sign: Option<u8>,
    pub hours: Option<Utf16View<'a>>,
    pub minutes: Option<Utf16View<'a>>,
    pub seconds: Option<Utf16View<'a>>,
    pub fraction: Option<Utf16View<'a>>,
    pub source_text: Utf16View<'a>,
}

impl Default for TimeZoneOffset<'_> {
    fn default() -> Self {
        Self {
            sign: None,
            hours: None,
            minutes: None,
            seconds: None,
            fraction: None,
            source_text: Utf16View::EMPTY,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct ParseResult<'a> {
    pub sign: Option<u8>,

    pub date_year: Option<Utf16View<'a>>,
    pub date_month: Option<Utf16View<'a>>,
    pub date_day: Option<Utf16View<'a>>,
    pub time_hour: Option<Utf16View<'a>>,
    pub time_minute: Option<Utf16View<'a>>,
    pub time_second: Option<Utf16View<'a>>,
    pub time_fraction: Option<Utf16View<'a>>,
    pub date_time_offset: Option<TimeZoneOffset<'a>>,

    pub utc_designator: Option<Utf16View<'a>>,
    pub time_zone_identifier: Option<Utf16View<'a>>,
    pub time_zone_iana_name: Option<Utf16View<'a>>,
    pub time_zone_offset: Option<TimeZoneOffset<'a>>,

    pub duration_years: Option<Utf16View<'a>>,
    pub duration_months: Option<Utf16View<'a>>,
    pub duration_weeks: Option<Utf16View<'a>>,
    pub duration_days: Option<Utf16View<'a>>,
    pub duration_hours: Option<Utf16View<'a>>,
    pub duration_hours_fraction: Option<Utf16View<'a>>,
    pub duration_minutes: Option<Utf16View<'a>>,
    pub duration_minutes_fraction: Option<Utf16View<'a>>,
    pub duration_seconds: Option<Utf16View<'a>>,
    pub duration_seconds_fraction: Option<Utf16View<'a>>,

    pub annotations: Vec<Annotation<'a>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Production {
    AnnotationValue,
    DateMonth,
    TemporalDateTimeString,
    TemporalDurationString,
    TemporalInstantString,
    TemporalMonthDayString,
    TemporalTimeString,
    TemporalYearMonthString,
    TemporalZonedDateTimeString,
    TimeZoneIdentifier,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SubMinutePrecision {
    No,
    Yes,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Extended {
    No,
    Yes,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TimeRequired {
    No,
    Yes,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ZDesignator {
    No,
    Yes,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Zoned {
    No,
    Yes,
}

fn is_one_of(view: Option<Utf16View<'_>>, candidates: &[&str]) -> bool {
    view.is_some_and(|view| candidates.iter().any(|candidate| view == *candidate))
}

// 13.31.1 Static Semantics: IsValidMonthDay, https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar-static-semantics-isvalidmonthday
fn is_valid_month_day(result: &ParseResult<'_>) -> bool {
    // 1. If DateDay is "31" and DateMonth is "02", "04", "06", "09", "11", return false.
    if is_one_of(result.date_day, &["31"]) && is_one_of(result.date_month, &["02", "04", "06", "09", "11"]) {
        return false;
    }

    // 2. If DateMonth is "02" and DateDay is "30", return false.
    if is_one_of(result.date_month, &["02"]) && is_one_of(result.date_day, &["30"]) {
        return false;
    }

    // 3. Return true.
    true
}

// 13.31.2 Static Semantics: IsValidDate, https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar-static-semantics-isvaliddate
fn is_valid_date(result: &ParseResult<'_>) -> bool {
    // 1. If IsValidMonthDay of DateSpec is false, return false.
    if !is_valid_month_day(result) {
        return false;
    }

    // 2. Let year be ℝ(StringToNumber(CodePointsToString(DateYear))).
    let year_code_units: Vec<u16> = result.date_year.expect("a date has a year").code_units().collect();
    let year = string_to_number(&year_code_units);

    // 3. If DateMonth is "02" and DateDay is "29" and MathematicalInLeapYear(EpochTimeForYear(year)) = 0, return false.
    if is_one_of(result.date_month, &["02"])
        && is_one_of(result.date_day, &["29"])
        && mathematical_in_leap_year(epoch_time_for_year(year)) == 0
    {
        return false;
    }

    // 4. Return true.
    true
}

#[derive(Clone)]
struct State<'a> {
    position: usize,
    parse_result: ParseResult<'a>,
}

// 13.31 RFC 9557 / ISO 8601 grammar, https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar
struct ISO8601Parser<'a> {
    input: Utf16View<'a>,
    state: State<'a>,
}

impl<'a> ISO8601Parser<'a> {
    fn new(input: Utf16View<'a>) -> Self {
        Self {
            input,
            state: State {
                position: 0,
                parse_result: ParseResult::default(),
            },
        }
    }

    fn is_eof(&self) -> bool {
        self.state.position >= self.input.length_in_code_units()
    }

    fn next_code_unit(&self) -> Option<u16> {
        (!self.is_eof()).then(|| self.input.code_unit_at(self.state.position))
    }

    fn next_is(&self, predicate: impl FnOnce(u16) -> bool) -> bool {
        self.next_code_unit().is_some_and(predicate)
    }

    fn consume(&mut self) {
        self.state.position += 1;
    }

    fn consume_specific(&mut self, code_unit: u8) -> bool {
        if self.next_code_unit() != Some(u16::from(code_unit)) {
            return false;
        }
        self.consume();
        true
    }

    fn consume_specific_string(&mut self, string: &str) -> bool {
        let remaining = self.input.length_in_code_units() - self.state.position;
        if string.len() > remaining {
            return false;
        }
        if self.input.substring_view(self.state.position, string.len()) != string {
            return false;
        }
        self.state.position += string.len();
        true
    }

    fn parsed_string_view(&self, start_index: usize) -> Utf16View<'a> {
        self.input
            .substring_view(start_index, self.state.position - start_index)
    }

    /// Runs `body` with the position it starts at, and restores the state from before it if it fails. `body` returns
    /// whether it commits.
    fn transaction(&mut self, body: impl FnOnce(&mut Self, usize) -> bool) -> bool {
        let saved_state = self.state.clone();
        let start_index = self.state.position;
        if body(self, start_index) {
            return true;
        }
        self.state = saved_state;
        false
    }

    /// The source text `parser` matches, or None with the state restored if it fails.
    fn scoped_parse(&mut self, parser: impl FnOnce(&mut Self) -> bool) -> Option<Utf16View<'a>> {
        let mut parsed = None;
        self.transaction(|parser_state, start_index| {
            if !parser(parser_state) {
                return false;
            }
            parsed = Some(parser_state.parsed_string_view(start_index));
            true
        });
        parsed
    }

    fn scoped_parse_sign(&mut self, parser: impl FnOnce(&mut Self) -> bool) -> Option<u8> {
        self.scoped_parse(parser).map(|parsed| parsed.code_unit_at(0) as u8)
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalDateTimeString
    fn parse_temporal_date_time_string(&mut self) -> bool {
        // TemporalDateTimeString[Zoned] :::
        //     AnnotatedDateTime[?Zoned, ~TimeRequired]
        self.parse_annotated_date_time(Zoned::No, TimeRequired::No)
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalDateTimeString
    fn parse_temporal_zoned_date_time_string(&mut self) -> bool {
        // TemporalDateTimeString[Zoned] :::
        //     AnnotatedDateTime[?Zoned, ~TimeRequired]
        self.parse_annotated_date_time(Zoned::Yes, TimeRequired::No)
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalDurationString
    fn parse_temporal_duration_string(&mut self) -> bool {
        // TemporalDurationString :::
        //     Duration
        self.parse_duration()
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalInstantString
    fn parse_temporal_instant_string(&mut self) -> bool {
        // TemporalInstantString :::
        //     Date DateTimeSeparator Time DateTimeUTCOffset[+Z] TimeZoneAnnotation[opt] Annotations[opt]
        if !self.parse_date() {
            return false;
        }
        if !self.parse_date_time_separator() {
            return false;
        }
        if !self.parse_time() {
            return false;
        }
        if !self.parse_date_time_utc_offset(ZDesignator::Yes) {
            return false;
        }

        self.parse_time_zone_annotation();
        self.parse_annotations();

        true
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalMonthDayString
    fn parse_temporal_month_day_string(&mut self) -> bool {
        // TemporalMonthDayString :::
        //     AnnotatedMonthDay
        //     AnnotatedDateTime[~Zoned, ~TimeRequired]
        //  NOTE: Reverse order here because `AnnotatedMonthDay` can be a subset of `AnnotatedDateTime`.
        self.parse_annotated_date_time(Zoned::No, TimeRequired::No) || self.parse_annotated_month_day()
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalTimeString
    fn parse_temporal_time_string(&mut self) -> bool {
        // TemporalTimeString :::
        //     AnnotatedTime
        //     AnnotatedDateTime[~Zoned, +TimeRequired]
        // NOTE: Reverse order here because `AnnotatedTime` can be a subset of `AnnotatedDateTime`.
        self.parse_annotated_date_time(Zoned::No, TimeRequired::Yes) || self.parse_annotated_time()
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalYearMonthString
    fn parse_temporal_year_month_string(&mut self) -> bool {
        // TemporalYearMonthString :::
        //     AnnotatedYearMonth
        //     AnnotatedDateTime[~Zoned, ~TimeRequired]
        //  NOTE: Reverse order here because `AnnotatedYearMonth` can be a subset of `AnnotatedDateTime`.
        self.parse_annotated_date_time(Zoned::No, TimeRequired::No) || self.parse_annotated_year_month()
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedDateTime
    fn parse_annotated_date_time(&mut self, zoned: Zoned, time_required: TimeRequired) -> bool {
        // AnnotatedDateTime[Zoned, TimeRequired] :::
        //     [~Zoned] DateTime[~Z, ?TimeRequired] TimeZoneAnnotation[opt] Annotations[opt]
        //     [+Zoned] DateTime[+Z, ?TimeRequired] TimeZoneAnnotation Annotations[opt]
        let z_designator = if zoned == Zoned::Yes {
            ZDesignator::Yes
        } else {
            ZDesignator::No
        };
        if !self.parse_date_time(z_designator, time_required) {
            return false;
        }

        if !self.parse_time_zone_annotation() && zoned == Zoned::Yes {
            return false;
        }

        self.parse_annotations();

        true
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedMonthDay
    fn parse_annotated_month_day(&mut self) -> bool {
        // AnnotatedMonthDay :::
        //     DateSpecMonthDay TimeZoneAnnotation[opt] Annotations[opt]
        if !self.parse_date_spec_month_day() {
            return false;
        }

        self.parse_time_zone_annotation();
        self.parse_annotations();

        true
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedTime
    fn parse_annotated_time(&mut self) -> bool {
        // AnnotatedTime :::
        //     TimeDesignator Time DateTimeUTCOffset[~Z][opt] TimeZoneAnnotation[opt] Annotations[opt]
        //     Time DateTimeUTCOffset[~Z][opt] TimeZoneAnnotation[opt] Annotations[opt]
        let has_time_designator = self.parse_time_designator();

        if !has_time_designator {
            // NB: This looks ahead without committing, so the state is restored either way.
            let saved_state = self.state.clone();

            // It is a Syntax Error if ParseText(Time DateTimeUTCOffset[~Z], DateSpecMonthDay) is a Parse Node.
            // It is a Syntax Error if ParseText(Time DateTimeUTCOffset[~Z], DateSpecYearMonth) is a Parse Node.
            let is_month_day_or_year_month = self.parse_date_spec_month_day() || self.parse_date_spec_year_month();
            self.state = saved_state;
            if is_month_day_or_year_month {
                return false;
            }
        }

        if !self.parse_time() {
            return false;
        }

        self.parse_date_time_utc_offset(ZDesignator::No);
        self.parse_time_zone_annotation();
        self.parse_annotations();

        true
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedYearMonth
    fn parse_annotated_year_month(&mut self) -> bool {
        // AnnotatedYearMonth :::
        //     DateSpecYearMonth TimeZoneAnnotation[opt] Annotations[opt]
        if !self.parse_date_spec_year_month() {
            return false;
        }

        self.parse_time_zone_annotation();
        self.parse_annotations();

        true
    }

    // https://tc39.es/proposal-temporal/#prod-DateTime
    fn parse_date_time(&mut self, z_designator: ZDesignator, time_required: TimeRequired) -> bool {
        self.transaction(|parser, _| {
            // DateTime[Z, TimeRequired] :::
            //     [~TimeRequired] Date
            //     Date DateTimeSeparator Time DateTimeUTCOffset[?Z][opt]
            if !parser.parse_date() {
                return false;
            }

            if parser.parse_date_time_separator() {
                if !parser.parse_time() {
                    return false;
                }

                parser.parse_date_time_utc_offset(z_designator);
            } else if time_required == TimeRequired::Yes {
                return false;
            }

            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-Date
    fn parse_date(&mut self) -> bool {
        // Date :::
        //     DateSpec[+Extended]
        //     DateSpec[~Extended]
        self.parse_date_spec(Extended::Yes) || self.parse_date_spec(Extended::No)
    }

    // https://tc39.es/proposal-temporal/#prod-DateSpec
    fn parse_date_spec(&mut self, extended: Extended) -> bool {
        self.transaction(|parser, _| {
            // DateSpec[Extended] :::
            //     DateYear DateSeparator[?Extended] DateMonth DateSeparator[?Extended] DateDay
            if !parser.parse_date_year() {
                return false;
            }
            if !parser.parse_date_separator(extended) {
                return false;
            }
            if !parser.parse_date_month() {
                return false;
            }
            if !parser.parse_date_separator(extended) {
                return false;
            }
            if !parser.parse_date_day() {
                return false;
            }

            // Note the prohibition on invalid combinations of month and day in 13.31.3.
            // https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar-static-semantics-early-errors
            // It is a Syntax Error if IsValidDate of DateSpec is false.
            is_valid_date(&parser.state.parse_result)
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DateSpecMonthDay
    fn parse_date_spec_month_day(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DateSpecMonthDay :::
            //     --[opt] DateMonth DateSeparator[+Extended] DateDay
            //     --[opt] DateMonth DateSeparator[~Extended] DateDay
            parser.consume_specific_string("--");

            if !parser.parse_date_month() {
                return false;
            }
            if !parser.parse_date_separator(Extended::Yes) && !parser.parse_date_separator(Extended::No) {
                return false;
            }
            if !parser.parse_date_day() {
                return false;
            }

            // Note the prohibition on invalid combinations of month and day in 13.31.3.
            // https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar-static-semantics-early-errors
            // It is a Syntax Error if IsValidMonthDay of DateSpecMonthDay is false.
            is_valid_month_day(&parser.state.parse_result)
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DateSpecYearMonth
    fn parse_date_spec_year_month(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DateSpecYearMonth :::
            //     DateYear DateSeparator[+Extended] DateMonth
            //     DateYear DateSeparator[~Extended] DateMonth
            if !parser.parse_date_year() {
                return false;
            }
            if !parser.parse_date_separator(Extended::Yes) && !parser.parse_date_separator(Extended::No) {
                return false;
            }
            parser.parse_date_month()
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DateYear
    fn parse_date_year(&mut self) -> bool {
        self.transaction(|parser, start_index| {
            // DateYear :::
            //     DecimalDigit DecimalDigit DecimalDigit DecimalDigit
            //     ASCIISign DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
            let digit_count = if parser.parse_ascii_sign() { 6 } else { 4 };

            for _ in 0..digit_count {
                if !parser.parse_decimal_digit() {
                    return false;
                }
            }

            // Note the prohibition on negative zero in 13.31.3.
            // https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar-static-semantics-early-errors
            // It is a Syntax Error if DateYear is "-000000".
            let parsed = parser.parsed_string_view(start_index);
            if parsed == "-000000" {
                return false;
            }

            parser.state.parse_result.date_year = Some(parsed);
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DateMonth
    fn parse_date_month(&mut self) -> bool {
        self.transaction(|parser, start_index| {
            // DateMonth :::
            //     0 NonZeroDigit
            //     10
            //     11
            //     12
            if parser.consume_specific(b'0') {
                if !parser.parse_non_zero_digit() {
                    return false;
                }
            } else {
                let success = parser.consume_specific_string("10")
                    || parser.consume_specific_string("11")
                    || parser.consume_specific_string("12");
                if !success {
                    return false;
                }
            }

            parser.state.parse_result.date_month = Some(parser.parsed_string_view(start_index));
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DateDay
    fn parse_date_day(&mut self) -> bool {
        self.transaction(|parser, start_index| {
            // DateDay :::
            //     0 NonZeroDigit
            //     1 DecimalDigit
            //     2 DecimalDigit
            //     30
            //     31
            if parser.consume_specific(b'0') {
                if !parser.parse_non_zero_digit() {
                    return false;
                }
            } else if parser.consume_specific(b'1') || parser.consume_specific(b'2') {
                if !parser.parse_decimal_digit() {
                    return false;
                }
            } else {
                let success = parser.consume_specific_string("30") || parser.consume_specific_string("31");
                if !success {
                    return false;
                }
            }

            parser.state.parse_result.date_day = Some(parser.parsed_string_view(start_index));
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DateTimeUTCOffset
    fn parse_date_time_utc_offset(&mut self, z_designator: ZDesignator) -> bool {
        // DateTimeUTCOffset[Z] :::
        //     [+Z] UTCDesignator
        //     UTCOffset[+SubMinutePrecision]
        if z_designator == ZDesignator::Yes && self.parse_utc_designator() {
            return true;
        }

        match self.parse_utc_offset(SubMinutePrecision::Yes) {
            Some(offset) => {
                self.state.parse_result.date_time_offset = Some(offset);
                true
            }
            None => false,
        }
    }

    // https://tc39.es/proposal-temporal/#prod-Time
    fn parse_time(&mut self) -> bool {
        // Time :::
        //     TimeSpec[+Extended]
        //     TimeSpec[~Extended]
        self.parse_time_spec()
    }

    // https://tc39.es/proposal-temporal/#prod-TimeSpec
    fn parse_time_spec(&mut self) -> bool {
        fn parse_time_hour(parser: &mut ISO8601Parser<'_>) -> bool {
            match parser.scoped_parse(ISO8601Parser::parse_hour) {
                Some(hour) => {
                    parser.state.parse_result.time_hour = Some(hour);
                    true
                }
                None => false,
            }
        }
        fn parse_time_minute(parser: &mut ISO8601Parser<'_>) -> bool {
            match parser.scoped_parse(ISO8601Parser::parse_minute_second) {
                Some(minute) => {
                    parser.state.parse_result.time_minute = Some(minute);
                    true
                }
                None => false,
            }
        }
        fn parse_time_fraction(parser: &mut ISO8601Parser<'_>) -> bool {
            match parser.scoped_parse(ISO8601Parser::parse_temporal_decimal_fraction) {
                Some(fraction) => {
                    parser.state.parse_result.time_fraction = Some(fraction);
                    true
                }
                None => false,
            }
        }

        self.transaction(|parser, _| {
            // TimeSpec[Extended] :::
            //     Hour
            //     Hour TimeSeparator[?Extended] MinuteSecond
            //     Hour TimeSeparator[?Extended] MinuteSecond TimeSeparator[?Extended] TimeSecond TemporalDecimalFraction[opt]
            if !parse_time_hour(parser) {
                return false;
            }

            if parser.parse_time_separator(Extended::Yes) {
                if !parse_time_minute(parser) {
                    return false;
                }

                if parser.parse_time_separator(Extended::Yes) {
                    if !parser.parse_time_second() {
                        return false;
                    }

                    parse_time_fraction(parser);
                }
            } else if parse_time_minute(parser) && parser.parse_time_second() {
                parse_time_fraction(parser);
            }

            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-TimeSecond
    fn parse_time_second(&mut self) -> bool {
        self.transaction(|parser, start_index| {
            // TimeSecond :::
            //     MinuteSecond
            //     60
            let success = parser.parse_minute_second() || parser.consume_specific_string("60");
            if !success {
                return false;
            }

            parser.state.parse_result.time_second = Some(parser.parsed_string_view(start_index));
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneAnnotation
    fn parse_time_zone_annotation(&mut self) -> bool {
        self.transaction(|parser, _| {
            // TimeZoneAnnotation :::
            //    [ AnnotationCriticalFlag[opt] TimeZoneIdentifier ]
            if !parser.consume_specific(b'[') {
                return false;
            }

            parser.parse_annotation_critical_flag();
            if !parser.parse_time_zone_identifier() {
                return false;
            }

            parser.consume_specific(b']')
        })
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneIdentifier
    fn parse_time_zone_identifier(&mut self) -> bool {
        self.transaction(|parser, start_index| {
            // TimeZoneIdentifier :::
            //    UTCOffset[~SubMinutePrecision]
            //    TimeZoneIANAName
            let success = match parser.parse_utc_offset(SubMinutePrecision::No) {
                Some(offset) => {
                    parser.state.parse_result.time_zone_offset = Some(offset);
                    true
                }
                None => parser.parse_time_zone_iana_name(),
            };
            if !success {
                return false;
            }

            parser.state.parse_result.time_zone_identifier = Some(parser.parsed_string_view(start_index));
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneIANAName
    fn parse_time_zone_iana_name(&mut self) -> bool {
        self.transaction(|parser, start_index| {
            // TimeZoneIANAName :::
            //     TimeZoneIANANameComponent
            //     TimeZoneIANAName / TimeZoneIANANameComponent
            if !parser.parse_time_zone_iana_name_component() {
                return false;
            }

            while parser.consume_specific(b'/') {
                if !parser.parse_time_zone_iana_name_component() {
                    return false;
                }
            }

            parser.state.parse_result.time_zone_iana_name = Some(parser.parsed_string_view(start_index));
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneIANANameComponent
    fn parse_time_zone_iana_name_component(&mut self) -> bool {
        // TimeZoneIANANameComponent :::
        //     TZLeadingChar
        //     TimeZoneIANANameComponent TZChar
        if !self.parse_tz_leading_char() {
            return false;
        }
        while self.parse_tz_leading_char() {}
        while self.parse_tz_char() {}

        true
    }

    // https://tc39.es/proposal-temporal/#prod-TZLeadingChar
    fn parse_tz_leading_char(&mut self) -> bool {
        // TZLeadingChar :::
        //     Alpha
        //     .
        //     _
        self.parse_alpha() || self.consume_specific(b'.') || self.consume_specific(b'_')
    }

    // https://tc39.es/proposal-temporal/#prod-TZChar
    fn parse_tz_char(&mut self) -> bool {
        // TZChar :::
        //     TZLeadingChar
        //     DecimalDigit
        //     -
        //     +
        self.parse_tz_leading_char()
            || self.parse_decimal_digit()
            || self.consume_specific(b'-')
            || self.consume_specific(b'+')
    }

    // https://tc39.es/proposal-temporal/#prod-Annotations
    fn parse_annotations(&mut self) -> bool {
        // Annotations :::
        //     Annotation Annotationsopt
        if !self.parse_annotation() {
            return false;
        }
        while self.parse_annotation() {}
        true
    }

    // https://tc39.es/proposal-temporal/#prod-Annotation
    fn parse_annotation(&mut self) -> bool {
        self.transaction(|parser, _| {
            // Annotation :::
            //     [ AnnotationCriticalFlag[opt] AnnotationKey = AnnotationValue ]
            if !parser.consume_specific(b'[') {
                return false;
            }

            let critical = parser.parse_annotation_critical_flag();

            let Some(key) = parser.scoped_parse(ISO8601Parser::parse_annotation_key) else {
                return false;
            };
            if !parser.consume_specific(b'=') {
                return false;
            }
            let Some(value) = parser.scoped_parse(ISO8601Parser::parse_annotation_value) else {
                return false;
            };

            if !parser.consume_specific(b']') {
                return false;
            }

            parser
                .state
                .parse_result
                .annotations
                .push(Annotation { critical, key, value });
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationKey
    fn parse_annotation_key(&mut self) -> bool {
        // AnnotationKey :::
        //     AKeyLeadingChar
        //     AnnotationKey AKeyChar
        if !self.parse_annotation_key_leading_char() {
            return false;
        }
        while self.parse_annotation_key_leading_char() {}
        while self.parse_annotation_key_char() {}

        true
    }

    // https://tc39.es/proposal-temporal/#prod-AKeyLeadingChar
    fn parse_annotation_key_leading_char(&mut self) -> bool {
        // AKeyLeadingChar :::
        //     LowercaseAlpha
        //     _
        self.parse_lowercase_alpha() || self.consume_specific(b'_')
    }

    // https://tc39.es/proposal-temporal/#prod-AKeyChar
    fn parse_annotation_key_char(&mut self) -> bool {
        // AKeyChar :::
        //     AKeyLeadingChar
        //     DecimalDigit
        //     -
        self.parse_annotation_key_leading_char() || self.parse_decimal_digit() || self.consume_specific(b'-')
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationValue
    fn parse_annotation_value(&mut self) -> bool {
        // AnnotationValue :::
        //     AnnotationValueComponent
        //     AnnotationValueComponent - AnnotationValue
        if !self.parse_annotation_value_component() {
            return false;
        }

        while self.consume_specific(b'-') {
            if !self.parse_annotation_value_component() {
                return false;
            }
        }

        true
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationValueComponent
    fn parse_annotation_value_component(&mut self) -> bool {
        // AnnotationValueComponent :::
        //     Alpha AnnotationValueComponent[opt]
        //     DecimalDigit AnnotationValueComponent[opt]
        let parse_component = |parser: &mut Self| parser.parse_alpha() || parser.parse_decimal_digit();

        if !parse_component(self) {
            return false;
        }
        while parse_component(self) {}

        true
    }

    // https://tc39.es/proposal-temporal/#prod-UTCOffset
    fn parse_utc_offset(&mut self, sub_minute_precision: SubMinutePrecision) -> Option<TimeZoneOffset<'a>> {
        let mut result = None;
        self.transaction(|parser, start_index| {
            let mut time_zone_offset = TimeZoneOffset::default();

            // UTCOffset[SubMinutePrecision] :::
            //     ASCIISign Hour
            //     ASCIISign Hour TimeSeparator[+Extended] MinuteSecond
            //     ASCIISign Hour TimeSeparator[~Extended] MinuteSecond
            //     [+SubMinutePrecision] ASCIISign Hour TimeSeparator[+Extended] MinuteSecond TimeSeparator[+Extended] MinuteSecond TemporalDecimalFraction[opt]
            //     [+SubMinutePrecision] ASCIISign Hour TimeSeparator[~Extended] MinuteSecond TimeSeparator[~Extended] MinuteSecond TemporalDecimalFraction[opt]
            let Some(sign) = parser.scoped_parse_sign(ISO8601Parser::parse_ascii_sign) else {
                return false;
            };
            time_zone_offset.sign = Some(sign);

            let Some(hours) = parser.scoped_parse(ISO8601Parser::parse_hour) else {
                return false;
            };
            time_zone_offset.hours = Some(hours);

            if parser.parse_time_separator(Extended::Yes) {
                let Some(minutes) = parser.scoped_parse(ISO8601Parser::parse_minute_second) else {
                    return false;
                };
                time_zone_offset.minutes = Some(minutes);

                if sub_minute_precision == SubMinutePrecision::Yes && parser.parse_time_separator(Extended::Yes) {
                    let Some(seconds) = parser.scoped_parse(ISO8601Parser::parse_minute_second) else {
                        return false;
                    };
                    time_zone_offset.seconds = Some(seconds);

                    time_zone_offset.fraction = parser.scoped_parse(ISO8601Parser::parse_temporal_decimal_fraction);
                }
            } else if let Some(minutes) = parser.scoped_parse(ISO8601Parser::parse_minute_second) {
                time_zone_offset.minutes = Some(minutes);

                if sub_minute_precision == SubMinutePrecision::Yes
                    && let Some(seconds) = parser.scoped_parse(ISO8601Parser::parse_minute_second)
                {
                    time_zone_offset.seconds = Some(seconds);
                    time_zone_offset.fraction = parser.scoped_parse(ISO8601Parser::parse_temporal_decimal_fraction);
                }
            }

            time_zone_offset.source_text = parser.parsed_string_view(start_index);
            result = Some(time_zone_offset);
            true
        });
        result
    }

    // https://tc39.es/ecma262/#prod-Hour
    fn parse_hour(&mut self) -> bool {
        // Hour :::
        //     0 DecimalDigit
        //     1 DecimalDigit
        //     20
        //     21
        //     22
        //     23
        if self.consume_specific(b'0') || self.consume_specific(b'1') {
            if !self.parse_decimal_digit() {
                return false;
            }
        } else {
            let success = self.consume_specific_string("20")
                || self.consume_specific_string("21")
                || self.consume_specific_string("22")
                || self.consume_specific_string("23");
            if !success {
                return false;
            }
        }

        true
    }

    // https://tc39.es/ecma262/#prod-MinuteSecond
    fn parse_minute_second(&mut self) -> bool {
        // MinuteSecond :::
        //     0 DecimalDigit
        //     1 DecimalDigit
        //     2 DecimalDigit
        //     3 DecimalDigit
        //     4 DecimalDigit
        //     5 DecimalDigit
        let success = self.consume_specific(b'0')
            || self.consume_specific(b'1')
            || self.consume_specific(b'2')
            || self.consume_specific(b'3')
            || self.consume_specific(b'4')
            || self.consume_specific(b'5');
        if !success {
            return false;
        }
        if !self.parse_decimal_digit() {
            return false;
        }

        true
    }

    // https://tc39.es/proposal-temporal/#prod-DurationDate
    fn parse_duration_date(&mut self) -> bool {
        // DurationDate :::
        //     DurationYearsPart DurationTime[opt]
        //     DurationMonthsPart DurationTime[opt]
        //     DurationWeeksPart DurationTime[opt]
        //     DurationDaysPart DurationTime[opt]
        let success = self.parse_duration_years_part()
            || self.parse_duration_months_part()
            || self.parse_duration_weeks_part()
            || self.parse_duration_days_part();
        if !success {
            return false;
        }

        self.parse_duration_time();
        true
    }

    // https://tc39.es/proposal-temporal/#prod-Duration
    fn parse_duration(&mut self) -> bool {
        self.transaction(|parser, _| {
            // Duration :::
            //    ASCIISign[opt] DurationDesignator DurationDate
            //    ASCIISign[opt] DurationDesignator DurationTime
            if let Some(sign) = parser.scoped_parse_sign(ISO8601Parser::parse_ascii_sign) {
                parser.state.parse_result.sign = Some(sign);
            }

            if !parser.parse_duration_designator() {
                return false;
            }

            parser.parse_duration_date() || parser.parse_duration_time()
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationYearsPart
    fn parse_duration_years_part(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationYearsPart :::
            //     DecimalDigits[~Sep] YearsDesignator DurationMonthsPart
            //     DecimalDigits[~Sep] YearsDesignator DurationWeeksPart
            //     DecimalDigits[~Sep] YearsDesignator DurationDaysPart[opt]
            let Some(years) = parser.parse_decimal_digits() else {
                return false;
            };
            parser.state.parse_result.duration_years = Some(years);

            if !parser.parse_years_designator() {
                return false;
            }

            let _ = parser.parse_duration_months_part()
                || parser.parse_duration_weeks_part()
                || parser.parse_duration_days_part();

            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationMonthsPart
    fn parse_duration_months_part(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationMonthsPart :::
            //     DecimalDigits[~Sep] MonthsDesignator DurationWeeksPart
            //     DecimalDigits[~Sep] MonthsDesignator DurationDaysPart[opt]
            let Some(months) = parser.parse_decimal_digits() else {
                return false;
            };
            parser.state.parse_result.duration_months = Some(months);

            if !parser.parse_months_designator() {
                return false;
            }

            let _ = parser.parse_duration_weeks_part() || parser.parse_duration_days_part();

            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationWeeksPart
    fn parse_duration_weeks_part(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationWeeksPart :::
            //     DecimalDigits[~Sep] WeeksDesignator DurationDaysPart[opt]
            let Some(weeks) = parser.parse_decimal_digits() else {
                return false;
            };
            parser.state.parse_result.duration_weeks = Some(weeks);

            if !parser.parse_weeks_designator() {
                return false;
            }

            parser.parse_duration_days_part();

            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationDaysPart
    fn parse_duration_days_part(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationDaysPart :::
            //     DecimalDigits[~Sep] DaysDesignator
            let Some(days) = parser.parse_decimal_digits() else {
                return false;
            };
            parser.state.parse_result.duration_days = Some(days);

            parser.parse_days_designator()
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationTime
    fn parse_duration_time(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationTime :::
            //     TimeDesignator DurationHoursPart
            //     TimeDesignator DurationMinutesPart
            //     TimeDesignator DurationSecondsPart
            if !parser.parse_time_designator() {
                return false;
            }

            parser.parse_duration_hours_part()
                || parser.parse_duration_minutes_part()
                || parser.parse_duration_seconds_part()
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationHoursPart
    fn parse_duration_hours_part(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationHoursPart :::
            //     DecimalDigits[~Sep] TemporalDecimalFraction HoursDesignator
            //     DecimalDigits[~Sep] HoursDesignator DurationMinutesPart
            //     DecimalDigits[~Sep] HoursDesignator DurationSecondsPart[opt]
            let Some(hours) = parser.parse_decimal_digits() else {
                return false;
            };
            parser.state.parse_result.duration_hours = Some(hours);

            let fraction = parser.scoped_parse(ISO8601Parser::parse_temporal_decimal_fraction);
            let is_fractional = fraction.is_some();
            if let Some(fraction) = fraction {
                parser.state.parse_result.duration_hours_fraction = Some(fraction);
            }

            if !parser.parse_hours_designator() {
                return false;
            }
            if !is_fractional {
                let _ = parser.parse_duration_minutes_part() || parser.parse_duration_seconds_part();
            }

            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationMinutesPart
    fn parse_duration_minutes_part(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationMinutesPart :::
            //     DecimalDigits[~Sep] TemporalDecimalFraction MinutesDesignator
            //     DecimalDigits[~Sep] MinutesDesignator DurationSecondsPart[opt]
            let Some(minutes) = parser.parse_decimal_digits() else {
                return false;
            };
            parser.state.parse_result.duration_minutes = Some(minutes);

            let fraction = parser.scoped_parse(ISO8601Parser::parse_temporal_decimal_fraction);
            let is_fractional = fraction.is_some();
            if let Some(fraction) = fraction {
                parser.state.parse_result.duration_minutes_fraction = Some(fraction);
            }

            if !parser.parse_minutes_designator() {
                return false;
            }
            if !is_fractional {
                parser.parse_duration_seconds_part();
            }

            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-DurationSecondsPart
    fn parse_duration_seconds_part(&mut self) -> bool {
        self.transaction(|parser, _| {
            // DurationSecondsPart :::
            //     DecimalDigits[~Sep] TemporalDecimalFraction[opt] SecondsDesignator
            let Some(seconds) = parser.parse_decimal_digits() else {
                return false;
            };
            parser.state.parse_result.duration_seconds = Some(seconds);

            if let Some(fraction) = parser.scoped_parse(ISO8601Parser::parse_temporal_decimal_fraction) {
                parser.state.parse_result.duration_seconds_fraction = Some(fraction);
            }

            parser.parse_seconds_designator()
        })
    }

    // https://tc39.es/ecma262/#prod-TemporalDecimalFraction
    fn parse_temporal_decimal_fraction(&mut self) -> bool {
        // TemporalDecimalFraction :::
        //     TemporalDecimalSeparator DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        if !self.parse_temporal_decimal_separator() {
            return false;
        }
        if !self.parse_decimal_digit() {
            return false;
        }

        for _ in 0..8 {
            if !self.parse_decimal_digit() {
                break;
            }
        }

        true
    }

    // https://tc39.es/proposal-temporal/#prod-Alpha
    fn parse_alpha(&mut self) -> bool {
        // Alpha ::: one of
        //     A B C D E F G H I J K L M N O P Q R S T U V W X Y Z a b c d e f g h i j k l m n o p q r s t u v w x y z
        if self.next_is(|code_unit| u8::try_from(code_unit).is_ok_and(|code_unit| code_unit.is_ascii_alphabetic())) {
            self.consume();
            return true;
        }
        false
    }

    // https://tc39.es/proposal-temporal/#prod-LowercaseAlpha
    fn parse_lowercase_alpha(&mut self) -> bool {
        // LowercaseAlpha ::: one of
        //     a b c d e f g h i j k l m n o p q r s t u v w x y z
        if self.next_is(|code_unit| u8::try_from(code_unit).is_ok_and(|code_unit| code_unit.is_ascii_lowercase())) {
            self.consume();
            return true;
        }
        false
    }

    // https://tc39.es/ecma262/#prod-DecimalDigit
    fn parse_decimal_digit(&mut self) -> bool {
        // DecimalDigit : one of
        //     0 1 2 3 4 5 6 7 8 9
        if self.next_is(|code_unit| (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit)) {
            self.consume();
            return true;
        }
        false
    }

    // https://tc39.es/ecma262/#prod-DecimalDigits
    /// DecimalDigits[~Sep]: the digits, or None with the state restored. [+Sep] is not needed.
    fn parse_decimal_digits(&mut self) -> Option<Utf16View<'a>> {
        // DecimalDigits[Sep] ::
        //     DecimalDigit
        //     DecimalDigits[?Sep] DecimalDigit
        //     [+Sep] DecimalDigits[+Sep] NumericLiteralSeparator DecimalDigit
        self.scoped_parse(|parser| {
            if !parser.parse_decimal_digit() {
                return false;
            }
            while parser.parse_decimal_digit() {}
            true
        })
    }

    // https://tc39.es/ecma262/#prod-NonZeroDigit
    fn parse_non_zero_digit(&mut self) -> bool {
        // NonZeroDigit : one of
        //     1 2 3 4 5 6 7 8 9
        if self.next_is(|code_unit| (u16::from(b'1')..=u16::from(b'9')).contains(&code_unit)) {
            self.consume();
            return true;
        }
        false
    }

    // https://tc39.es/ecma262/#prod-ASCIISign
    fn parse_ascii_sign(&mut self) -> bool {
        // ASCIISign : one of
        //     + -
        self.consume_specific(b'+') || self.consume_specific(b'-')
    }

    // https://tc39.es/proposal-temporal/#prod-DateSeparator
    fn parse_date_separator(&mut self, extended: Extended) -> bool {
        // DateSeparator[Extended] :::
        //     [+Extended] -
        //     [~Extended] [empty]
        if extended == Extended::Yes {
            return self.consume_specific(b'-');
        }
        true
    }

    // https://tc39.es/ecma262/#prod-TimeSeparator
    fn parse_time_separator(&mut self, extended: Extended) -> bool {
        // TimeSeparator[Extended] :::
        //     [+Extended] :
        //     [~Extended] [empty]
        if extended == Extended::Yes {
            return self.consume_specific(b':');
        }
        true
    }

    // https://tc39.es/proposal-temporal/#prod-TimeDesignator
    fn parse_time_designator(&mut self) -> bool {
        // TimeDesignator : one of
        //     T t
        self.consume_specific(b'T') || self.consume_specific(b't')
    }

    // https://tc39.es/proposal-temporal/#prod-DateTimeSeparator
    fn parse_date_time_separator(&mut self) -> bool {
        // DateTimeSeparator :::
        //     <SP>
        //     T
        //     t
        self.consume_specific(b' ') || self.consume_specific(b'T') || self.consume_specific(b't')
    }

    // https://tc39.es/ecma262/#prod-TemporalDecimalSeparator
    fn parse_temporal_decimal_separator(&mut self) -> bool {
        // TemporalDecimalSeparator ::: one of
        //    . ,
        self.consume_specific(b'.') || self.consume_specific(b',')
    }

    // https://tc39.es/proposal-temporal/#prod-DurationDesignator
    fn parse_duration_designator(&mut self) -> bool {
        // DurationDesignator : one of
        //     P p
        self.consume_specific(b'P') || self.consume_specific(b'p')
    }

    // https://tc39.es/proposal-temporal/#prod-YearsDesignator
    fn parse_years_designator(&mut self) -> bool {
        // YearsDesignator : one of
        //     Y y
        self.consume_specific(b'Y') || self.consume_specific(b'y')
    }

    // https://tc39.es/proposal-temporal/#prod-MonthsDesignator
    fn parse_months_designator(&mut self) -> bool {
        // MonthsDesignator : one of
        //     M m
        self.consume_specific(b'M') || self.consume_specific(b'm')
    }

    // https://tc39.es/proposal-temporal/#prod-WeeksDesignator
    fn parse_weeks_designator(&mut self) -> bool {
        // WeeksDesignator : one of
        //     W w
        self.consume_specific(b'W') || self.consume_specific(b'w')
    }

    // https://tc39.es/proposal-temporal/#prod-DaysDesignator
    fn parse_days_designator(&mut self) -> bool {
        // DaysDesignator : one of
        //     D d
        self.consume_specific(b'D') || self.consume_specific(b'd')
    }

    // https://tc39.es/proposal-temporal/#prod-HoursDesignator
    fn parse_hours_designator(&mut self) -> bool {
        // HoursDesignator : one of
        //     H h
        self.consume_specific(b'H') || self.consume_specific(b'h')
    }

    // https://tc39.es/proposal-temporal/#prod-MinutesDesignator
    fn parse_minutes_designator(&mut self) -> bool {
        // MinutesDesignator : one of
        //     M m
        self.consume_specific(b'M') || self.consume_specific(b'm')
    }

    // https://tc39.es/proposal-temporal/#prod-SecondsDesignator
    fn parse_seconds_designator(&mut self) -> bool {
        // SecondsDesignator : one of
        //     S s
        self.consume_specific(b'S') || self.consume_specific(b's')
    }

    // https://tc39.es/proposal-temporal/#prod-UTCDesignator
    fn parse_utc_designator(&mut self) -> bool {
        self.transaction(|parser, start_index| {
            // UTCDesignator : one of
            //     Z z
            let success = parser.consume_specific(b'Z') || parser.consume_specific(b'z');
            if !success {
                return false;
            }

            parser.state.parse_result.utc_designator = Some(parser.parsed_string_view(start_index));
            true
        })
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationCriticalFlag
    fn parse_annotation_critical_flag(&mut self) -> bool {
        // AnnotationCriticalFlag :::
        //     !
        self.consume_specific(b'!')
    }
}

pub fn parse_iso8601(production: Production, input: Utf16View<'_>) -> Option<ParseResult<'_>> {
    let mut parser = ISO8601Parser::new(input);

    let parsed = match production {
        Production::AnnotationValue => parser.parse_annotation_value(),
        Production::DateMonth => parser.parse_date_month(),
        Production::TemporalDateTimeString => parser.parse_temporal_date_time_string(),
        Production::TemporalDurationString => parser.parse_temporal_duration_string(),
        Production::TemporalInstantString => parser.parse_temporal_instant_string(),
        Production::TemporalMonthDayString => parser.parse_temporal_month_day_string(),
        Production::TemporalTimeString => parser.parse_temporal_time_string(),
        Production::TemporalYearMonthString => parser.parse_temporal_year_month_string(),
        Production::TemporalZonedDateTimeString => parser.parse_temporal_zoned_date_time_string(),
        Production::TimeZoneIdentifier => parser.parse_time_zone_identifier(),
    };
    if !parsed {
        return None;
    }

    // If we parsed successfully but didn't reach the end, the string doesn't match the given production.
    if !parser.is_eof() {
        return None;
    }

    Some(parser.state.parse_result)
}

pub fn parse_utc_offset(input: Utf16View<'_>, sub_minute_precision: SubMinutePrecision) -> Option<TimeZoneOffset<'_>> {
    let mut parser = ISO8601Parser::new(input);

    let utc_offset = parser.parse_utc_offset(sub_minute_precision)?;

    // If we parsed successfully but didn't reach the end, the string doesn't match the given production.
    if !parser.is_eof() {
        return None;
    }

    Some(utc_offset)
}
