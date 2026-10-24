/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::parser::{Input, Parser};
use crate::css::ffi_support::FfiUtf16View;
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq)]
struct NumberPercentage {
    value: f32,
    is_percentage: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Transform {
    kind: u8,
    values: [f32; 6],
}

pub const TRANSFORM_TRANSLATE: u8 = 0;
pub const TRANSFORM_SCALE: u8 = 1;
pub const TRANSFORM_ROTATE: u8 = 2;
pub const TRANSFORM_SKEW_X: u8 = 3;
pub const TRANSFORM_SKEW_Y: u8 = 4;
pub const TRANSFORM_MATRIX: u8 = 5;

fn parse_integer(input: Input<'_>) -> Option<i32> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let integer = parser.parse_integer().ok()?;
    parser.parse_whitespace();
    parser.done().then_some(integer)
}

fn parse_number_percentage(input: Input<'_>) -> Option<NumberPercentage> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let value = parser.parse_number().ok()?;
    let is_percentage = parser.current_ascii() == Some(b'%');
    if is_percentage {
        parser.consume();
    }
    parser.parse_whitespace();
    parser.done().then_some(NumberPercentage { value, is_percentage })
}

// https://svgwg.org/svg2-draft/types.html#syntax
fn parse_number(input: Input<'_>) -> Option<f32> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let value = parser.parse_css_number().ok()?;
    parser.parse_whitespace();
    parser.done().then_some(value)
}

// https://drafts.fxtf.org/filter-effects/#typedef-number-optional-number
// <number-optional-number> = <number> <number>?
fn parse_number_optional_number(input: Input<'_>) -> Option<(f32, Option<f32>)> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let first = parser.parse_css_number().ok()?;
    parser.parse_whitespace();
    if parser.done() {
        return Some((first, None));
    }
    parser.parse_optional_comma_whitespace();
    let second = parser.parse_css_number().ok()?;
    parser.parse_whitespace();
    parser.done().then_some((first, Some(second)))
}

fn parse_points(input: Input<'_>) -> Vec<[f32; 2]> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let mut points = Vec::new();
    loop {
        let point = match parser.parse_coordinate_pair() {
            Ok(point) => point,
            Err(()) if points.is_empty() => return Vec::new(),
            Err(()) => break,
        };
        points.push(point);
        parser.parse_optional_comma_whitespace();
        if !parser.matches_comma_whitespace() && !parser.matches_coordinate() {
            break;
        }
    }
    points
}

fn token(parser: &mut Parser<'_>) -> Vec<u16> {
    let mut token = Vec::new();
    while !parser.done() && !parser.current().is_some_and(is_whitespace) {
        token.push(parser.consume());
    }
    token
}

fn ascii_token_equals(token: &[u16], expected: &[u8]) -> bool {
    token.len() == expected.len()
        && token
            .iter()
            .zip(expected)
            .all(|(unit, expected)| *unit == u16::from(*expected))
}

// https://svgwg.org/svg2-draft/coords.html#PreserveAspectRatioAttribute
fn parse_preserve_aspect_ratio(input: Input<'_>) -> Option<(u8, u8)> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let align = token(&mut parser);
    if align.is_empty() {
        return None;
    }
    parser.parse_whitespace();
    let meet_or_slice = token(&mut parser);

    let align = [
        b"none".as_slice(),
        b"xMinYMin",
        b"xMidYMin",
        b"xMaxYMin",
        b"xMinYMid",
        b"xMidYMid",
        b"xMaxYMid",
        b"xMinYMax",
        b"xMidYMax",
        b"xMaxYMax",
    ]
    .iter()
    .position(|expected| ascii_token_equals(&align, expected))? as u8;
    let meet_or_slice = if meet_or_slice.is_empty() || ascii_token_equals(&meet_or_slice, b"meet") {
        0
    } else if ascii_token_equals(&meet_or_slice, b"slice") {
        1
    } else {
        return None;
    };
    Some((align, meet_or_slice))
}

fn parse_units(input: Input<'_>) -> Option<u8> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let units = token(&mut parser);
    if ascii_token_equals(&units, b"objectBoundingBox") {
        Some(0)
    } else if ascii_token_equals(&units, b"userSpaceOnUse") {
        Some(1)
    } else {
        None
    }
}

fn parse_spread_method(input: Input<'_>) -> Option<u8> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let method = token(&mut parser);
    if ascii_token_equals(&method, b"pad") {
        Some(0)
    } else if ascii_token_equals(&method, b"repeat") {
        Some(1)
    } else if ascii_token_equals(&method, b"reflect") {
        Some(2)
    } else {
        None
    }
}

// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-tablevalues
fn parse_table_values(input: Input<'_>) -> Vec<f32> {
    let mut parser = Parser::new(input);
    let mut values = Vec::new();
    while !parser.done() {
        parser.parse_whitespace();
        let Ok(value) = parser.parse_number() else {
            return Vec::new();
        };
        values.push(value);
        parser.parse_whitespace();
        if parser.current_ascii() == Some(b',') {
            parser.consume();
        }
    }
    values
}

fn parse_transform(input: Input<'_>) -> Option<Vec<Transform>> {
    let mut parser = Parser::new(input);
    let mut transforms = Vec::new();
    parser.parse_whitespace();
    while !parser.done() {
        let (kind, defaults, parameter_count) = if parser.consume_specific(b"translate") {
            (TRANSFORM_TRANSLATE, [0.0; 6], 2)
        } else if parser.consume_specific(b"scale") {
            (TRANSFORM_SCALE, [0.0; 6], 2)
        } else if parser.consume_specific(b"rotate") {
            (TRANSFORM_ROTATE, [0.0; 6], 3)
        } else if parser.consume_specific(b"skewX") {
            (TRANSFORM_SKEW_X, [0.0; 6], 1)
        } else if parser.consume_specific(b"skewY") {
            (TRANSFORM_SKEW_Y, [0.0; 6], 1)
        } else if parser.consume_specific(b"matrix") {
            (TRANSFORM_MATRIX, [0.0; 6], 6)
        } else {
            return None;
        };

        parser.parse_whitespace();
        if parser.current_ascii() != Some(b'(') {
            return None;
        }
        parser.consume();
        parser.parse_whitespace();

        let mut values = defaults;
        values[0] = parser.parse_number().ok()?;
        match kind {
            TRANSFORM_TRANSLATE => {
                parser.parse_optional_comma_whitespace();
                values[1] = parser.parse_number().unwrap_or(0.0);
            }
            TRANSFORM_SCALE => {
                parser.parse_optional_comma_whitespace();
                values[1] = parser.parse_number().unwrap_or(values[0]);
            }
            TRANSFORM_ROTATE => {
                parser.parse_optional_comma_whitespace();
                values[1] = parser.parse_number().unwrap_or(0.0);
                parser.parse_optional_comma_whitespace();
                values[2] = parser.parse_number().unwrap_or(0.0);
            }
            TRANSFORM_SKEW_X | TRANSFORM_SKEW_Y => {}
            TRANSFORM_MATRIX => {
                for value in values.iter_mut().take(parameter_count).skip(1) {
                    parser.parse_optional_comma_whitespace();
                    *value = parser.parse_number().ok()?;
                }
            }
            _ => unreachable!(),
        }

        parser.parse_whitespace();
        if parser.current_ascii() != Some(b')') {
            return None;
        }
        parser.consume();
        transforms.push(Transform { kind, values });
        parser.parse_whitespace();
        if parser.current_ascii() == Some(b',') {
            parser.consume();
        }
        parser.parse_whitespace();
    }
    Some(transforms)
}

fn parse_view_box(input: Input<'_>) -> Option<[f64; 4]> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let mut values = [0.0; 4];
    values[0] = f64::from(parser.parse_number().ok()?);
    for value in values.iter_mut().skip(1) {
        if !parser.matches_comma_whitespace() {
            return None;
        }
        parser.parse_optional_comma_whitespace();
        *value = f64::from(parser.parse_number().ok()?);
    }
    parser.parse_whitespace();
    parser.done().then_some(values)
}

fn is_digits(value: &str) -> bool {
    !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit())
}

// https://svgwg.org/specs/animations/#ClockValueSyntax
// Timecount ::= DIGIT+
// Fraction ::= DIGIT+
// NB: This parses a Timecount ("." Fraction)?, which also matches a Seconds ("." Fraction)?.
fn parse_digits_with_optional_fraction(value: &str) -> Option<f64> {
    let (integer, fraction) = value.split_once('.').unwrap_or((value, "0"));
    if !is_digits(integer) || !is_digits(fraction) {
        return None;
    }
    value.parse().ok()
}

// https://svgwg.org/specs/animations/#ClockValueSyntax
// Minutes ::= 2DIGIT; range from 00 to 59
// Seconds ::= 2DIGIT; range from 00 to 59
fn parse_minutes_or_seconds(value: &str) -> Option<f64> {
    if value.len() != 2 || !is_digits(value) {
        return None;
    }
    let value = value.parse().ok()?;
    (value < 60.0).then_some(value)
}

// https://svgwg.org/specs/animations/#ClockValueSyntax
// Seconds ("." Fraction)?
fn parse_seconds_with_optional_fraction(value: &str) -> Option<f64> {
    let (seconds, _) = value.split_once('.').unwrap_or((value, ""));
    parse_minutes_or_seconds(seconds)?;
    parse_digits_with_optional_fraction(value)
}

// https://svgwg.org/specs/animations/#ClockValueSyntax
fn parse_clock_value_in_milliseconds(input: Input<'_>) -> Option<f64> {
    // No embedded white space is allowed in clock values, although leading and trailing white space characters will
    // be ignored.
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let clock_value = String::from_utf16(&token(&mut parser)).ok()?;
    parser.parse_whitespace();
    if !parser.done() {
        return None;
    }

    let fields = clock_value.split(':').collect::<Vec<_>>();
    let seconds = match fields.as_slice() {
        // Full-clock-val ::= Hours ":" Minutes ":" Seconds ("." Fraction)?
        // Hours ::= DIGIT+; any positive number
        [hours, minutes, seconds] => {
            if !is_digits(hours) {
                return None;
            }
            hours.parse::<f64>().ok()? * 3600.0
                + parse_minutes_or_seconds(minutes)? * 60.0
                + parse_seconds_with_optional_fraction(seconds)?
        }
        // Partial-clock-val ::= Minutes ":" Seconds ("." Fraction)?
        [minutes, seconds] => {
            parse_minutes_or_seconds(minutes)? * 60.0 + parse_seconds_with_optional_fraction(seconds)?
        }
        // Timecount-val ::= Timecount ("." Fraction)? (Metric)?
        // Metric ::= "h" | "min" | "s" | "ms"
        // For Timecount values, the default metric suffix is "s" (for seconds).
        [timecount] => {
            let (timecount, milliseconds_per_unit) =
                [("h", 3_600_000.0), ("min", 60_000.0), ("ms", 1.0), ("s", 1000.0)]
                    .into_iter()
                    .find_map(|(metric, milliseconds_per_unit)| {
                        timecount
                            .strip_suffix(metric)
                            .map(|timecount| (timecount, milliseconds_per_unit))
                    })
                    .unwrap_or((timecount, 1000.0));
            return Some(parse_digits_with_optional_fraction(timecount)? * milliseconds_per_unit);
        }
        _ => return None,
    };
    Some(seconds * 1000.0)
}

fn is_whitespace(unit: u16) -> bool {
    matches!(unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20)
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSvgPoint {
    pub x: f32,
    pub y: f32,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSvgTransform {
    pub kind: u8,
    pub values: [f32; 6],
}

fn with_input<T>(input: FfiUtf16View<'_>, parse: impl FnOnce(Input<'_>) -> T) -> Option<T> {
    Some(parse(super::path_parser::svg_input(input)?))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_integer(input: FfiUtf16View<'_>, value: &mut i32) -> bool {
    let Some(parsed_value) = with_input(input, parse_integer).flatten() else {
        return false;
    };
    *value = parsed_value;
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_number(input: FfiUtf16View<'_>, value: &mut f32) -> bool {
    let Some(parsed_value) = with_input(input, parse_number).flatten() else {
        return false;
    };
    *value = parsed_value;
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_number_optional_number(
    input: FfiUtf16View<'_>,
    first: &mut f32,
    second: &mut f32,
    has_second: &mut bool,
) -> bool {
    let Some((parsed_first, parsed_second)) = with_input(input, parse_number_optional_number).flatten() else {
        return false;
    };
    *first = parsed_first;
    *second = parsed_second.unwrap_or(0.0);
    *has_second = parsed_second.is_some();
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_number_percentage(
    input: FfiUtf16View<'_>,
    value: &mut f32,
    is_percentage: &mut bool,
) -> bool {
    let Some(parsed_value) = with_input(input, parse_number_percentage).flatten() else {
        return false;
    };
    *value = parsed_value.value;
    *is_percentage = parsed_value.is_percentage;
    true
}

/// # Safety
/// `context` must be valid for `set_points`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_points(
    input: FfiUtf16View<'_>,
    context: *mut c_void,
    set_points: unsafe extern "C" fn(*mut c_void, *const FfiSvgPoint, usize),
) {
    let points = with_input(input, parse_points).unwrap_or_default();
    let points: Vec<_> = points
        .into_iter()
        .map(|point| FfiSvgPoint {
            x: point[0],
            y: point[1],
        })
        .collect();
    unsafe { set_points(context, points.as_ptr(), points.len()) };
}

/// # Safety
/// `context` must be valid for `set_transforms`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_transform(
    input: FfiUtf16View<'_>,
    context: *mut c_void,
    set_transforms: unsafe extern "C" fn(*mut c_void, *const FfiSvgTransform, usize),
) -> bool {
    let Some(transforms) = with_input(input, parse_transform).flatten() else {
        return false;
    };
    let transforms: Vec<_> = transforms
        .into_iter()
        .map(|transform| FfiSvgTransform {
            kind: transform.kind,
            values: transform.values,
        })
        .collect();
    unsafe { set_transforms(context, transforms.as_ptr(), transforms.len()) };
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_preserve_aspect_ratio(
    input: FfiUtf16View<'_>,
    align: &mut u8,
    meet_or_slice: &mut u8,
) -> bool {
    let Some((parsed_align, parsed_meet_or_slice)) = with_input(input, parse_preserve_aspect_ratio).flatten() else {
        return false;
    };
    *align = parsed_align;
    *meet_or_slice = parsed_meet_or_slice;
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_units(input: FfiUtf16View<'_>, value: &mut u8) -> bool {
    let Some(parsed_value) = with_input(input, parse_units).flatten() else {
        return false;
    };
    *value = parsed_value;
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_spread_method(input: FfiUtf16View<'_>, value: &mut u8) -> bool {
    let Some(parsed_value) = with_input(input, parse_spread_method).flatten() else {
        return false;
    };
    *value = parsed_value;
    true
}

/// # Safety
/// `context` must be valid for `set_values`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_table_values(
    input: FfiUtf16View<'_>,
    context: *mut c_void,
    set_values: unsafe extern "C" fn(*mut c_void, *const f32, usize),
) {
    let values = with_input(input, parse_table_values).unwrap_or_default();
    unsafe { set_values(context, values.as_ptr(), values.len()) };
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_view_box(
    input: FfiUtf16View<'_>,
    min_x: &mut f64,
    min_y: &mut f64,
    width: &mut f64,
    height: &mut f64,
) -> bool {
    let Some(values) = with_input(input, parse_view_box).flatten() else {
        return false;
    };
    *min_x = values[0];
    *min_y = values[1];
    *width = values[2];
    *height = values[3];
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_clock_value_in_milliseconds(input: FfiUtf16View<'_>, value: &mut f64) -> bool {
    let Some(parsed_value) = with_input(input, parse_clock_value_in_milliseconds).flatten() else {
        return false;
    };
    *value = parsed_value;
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(source: &str) -> Input<'_> {
        Input::Ascii(source.as_bytes())
    }

    #[test]
    fn parses_scalars() {
        assert_eq!(parse_integer(input(" -2147483648 ")), Some(i32::MIN));
        assert_eq!(parse_integer(input("2147483648")), None);
        assert_eq!(
            parse_number_percentage(input(" 12.5% ")),
            Some(NumberPercentage {
                value: 12.5,
                is_percentage: true
            })
        );
        assert_eq!(parse_number_percentage(input("12px")), None);
        assert_eq!(parse_number(input(" +5 ")), Some(5.0));
        assert_eq!(parse_number(input(".5")), Some(0.5));
        assert_eq!(parse_number(input("5.")), None);
        assert_eq!(parse_number(input("--3")), None);
        assert_eq!(parse_number(input("3 4")), None);
        assert_eq!(parse_number(input("")), None);
        assert_eq!(parse_number_optional_number(input(" 3 ")), Some((3.0, None)));
        assert_eq!(parse_number_optional_number(input("3, 4")), Some((3.0, Some(4.0))));
        assert_eq!(parse_number_optional_number(input("3 abc")), None);
        assert_eq!(parse_number_optional_number(input("3,")), None);
    }

    #[test]
    fn parses_points_and_table_values() {
        assert_eq!(parse_points(input(" 1,2 3 4")), vec![[1.0, 2.0], [3.0, 4.0]]);
        assert_eq!(parse_points(input("1")), Vec::<[f32; 2]>::new());

        assert_eq!(parse_table_values(input("")), Vec::<f32>::new());
        assert_eq!(parse_table_values(input(" \t")), Vec::<f32>::new());
        assert_eq!(parse_table_values(input("1, 2 3")), vec![1.0, 2.0, 3.0]);
        assert_eq!(parse_table_values(input(" 1 ")), vec![1.0]);
        assert_eq!(parse_table_values(input("1,")), vec![1.0]);
        assert_eq!(parse_table_values(input("1, ")), Vec::<f32>::new());
        assert_eq!(parse_table_values(input(".5, 1e2")), vec![0.5, 100.0]);
        assert_eq!(parse_table_values(input("1,,2")), Vec::<f32>::new());
        assert_eq!(parse_table_values(input("-1, +2")), vec![-1.0, 2.0]);
    }

    #[test]
    fn parses_keyword_attributes() {
        assert_eq!(parse_preserve_aspect_ratio(input(" xMidYMid slice")), Some((5, 1)));
        assert_eq!(parse_preserve_aspect_ratio(input("none")), Some((0, 0)));
        assert_eq!(parse_units(input(" userSpaceOnUse ignored")), Some(1));
        assert_eq!(parse_spread_method(input("reflect")), Some(2));
    }

    #[test]
    fn parses_transforms() {
        let transforms = parse_transform(input(
            "translate(1) scale(2, 3) rotate(4 5 6) skewX(7) skewY(8) matrix(1 2 3 4 5 6)",
        ))
        .unwrap();
        assert_eq!(transforms.len(), 6);
        assert_eq!(transforms[0].values[..2], [1.0, 0.0]);
        assert_eq!(transforms[1].values[..2], [2.0, 3.0]);
        assert_eq!(transforms[2].values[..3], [4.0, 5.0, 6.0]);
        assert_eq!(transforms[5].values, [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
        assert_eq!(parse_transform(input("translate()")), None);
    }

    #[test]
    fn parses_clock_values() {
        let milliseconds = |source| parse_clock_value_in_milliseconds(input(source));
        assert_eq!(milliseconds("02:30:03"), Some(9_003_000.0));
        assert_eq!(milliseconds("50:00:10.25"), Some(180_010_250.0));
        assert_eq!(milliseconds("02:33"), Some(153_000.0));
        assert_eq!(milliseconds("00:10.5"), Some(10_500.0));
        assert_eq!(milliseconds("3.2h"), Some(11_520_000.0));
        assert_eq!(milliseconds("45min"), Some(2_700_000.0));
        assert_eq!(milliseconds("30s"), Some(30_000.0));
        assert_eq!(milliseconds("5ms"), Some(5.0));
        assert_eq!(milliseconds("12.467"), Some(12_467.0));
        assert_eq!(milliseconds("00.5s"), Some(500.0));
        assert_eq!(milliseconds("00:00.005"), Some(5.0));
        assert_eq!(milliseconds(" 2s\t"), Some(2000.0));

        assert_eq!(milliseconds(""), None);
        assert_eq!(milliseconds("2 s"), None);
        assert_eq!(milliseconds(".5s"), None);
        assert_eq!(milliseconds("1."), None);
        assert_eq!(milliseconds("-1s"), None);
        assert_eq!(milliseconds("1sec"), None);
        assert_eq!(milliseconds("indefinite"), None);
        assert_eq!(milliseconds("2:30"), None);
        assert_eq!(milliseconds("00:60"), None);
        assert_eq!(milliseconds("1:60:00"), None);
        assert_eq!(milliseconds("00:00:005"), None);
    }

    #[test]
    fn parses_view_box() {
        assert_eq!(parse_view_box(input(" 1, 2 3 4 ")), Some([1.0, 2.0, 3.0, 4.0]));
        assert_eq!(parse_view_box(input("1 2 3")), None);
    }

    #[test]
    fn parses_utf16_without_converting_the_input() {
        let source = " 1,2 3,4".encode_utf16().collect::<Vec<_>>();
        assert_eq!(parse_points(Input::Utf16(&source)), vec![[1.0, 2.0], [3.0, 4.0]]);
    }
}
