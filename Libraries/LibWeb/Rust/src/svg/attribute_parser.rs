/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::parser::{Input, Parser};
use super::path_parser::FfiSvgInput;
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

fn parse_coordinate_pair(parser: &mut Parser<'_>) -> Result<[f32; 2], ()> {
    let x = parser.parse_number()?;
    parser.parse_optional_comma_whitespace();
    let y = parser.parse_number()?;
    Ok([x, y])
}

fn parse_points(input: Input<'_>) -> Vec<[f32; 2]> {
    let mut parser = Parser::new(input);
    parser.parse_whitespace();
    let mut points = Vec::new();
    loop {
        let point = match parse_coordinate_pair(&mut parser) {
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

fn with_input<T>(input: FfiSvgInput, parse: impl FnOnce(Input<'_>) -> T) -> Option<T> {
    // SAFETY: FFI callers satisfy FfiSvgInput's pointer contract for the duration of the call.
    Some(parse(unsafe { input.input()? }))
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and `value` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_integer(input: FfiSvgInput, value: *mut i32) -> bool {
    crate::abort_on_panic(|| {
        let Some(parsed_value) = with_input(input, parse_integer).flatten() else {
            return false;
        };
        unsafe { value.write(parsed_value) };
        true
    })
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and the output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_number_percentage(
    input: FfiSvgInput,
    value: *mut f32,
    is_percentage: *mut bool,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(parsed_value) = with_input(input, parse_number_percentage).flatten() else {
            return false;
        };
        unsafe {
            value.write(parsed_value.value);
            is_percentage.write(parsed_value.is_percentage);
        }
        true
    })
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and `context` must be valid for
/// `set_points`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_points(
    input: FfiSvgInput,
    context: *mut c_void,
    set_points: unsafe extern "C" fn(*mut c_void, *const FfiSvgPoint, usize),
) {
    crate::abort_on_panic(|| {
        let points = with_input(input, parse_points).unwrap_or_default();
        let points: Vec<_> = points
            .into_iter()
            .map(|point| FfiSvgPoint {
                x: point[0],
                y: point[1],
            })
            .collect();
        unsafe { set_points(context, points.as_ptr(), points.len()) };
    });
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and `context` must be valid for
/// `set_transforms`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_transform(
    input: FfiSvgInput,
    context: *mut c_void,
    set_transforms: unsafe extern "C" fn(*mut c_void, *const FfiSvgTransform, usize),
) -> bool {
    crate::abort_on_panic(|| {
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
    })
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and the output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_preserve_aspect_ratio(
    input: FfiSvgInput,
    align: *mut u8,
    meet_or_slice: *mut u8,
) -> bool {
    crate::abort_on_panic(|| {
        let Some((parsed_align, parsed_meet_or_slice)) = with_input(input, parse_preserve_aspect_ratio).flatten()
        else {
            return false;
        };
        unsafe {
            align.write(parsed_align);
            meet_or_slice.write(parsed_meet_or_slice);
        }
        true
    })
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and `value` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_units(input: FfiSvgInput, value: *mut u8) -> bool {
    crate::abort_on_panic(|| {
        let Some(parsed_value) = with_input(input, parse_units).flatten() else {
            return false;
        };
        unsafe { value.write(parsed_value) };
        true
    })
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and `value` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_spread_method(input: FfiSvgInput, value: *mut u8) -> bool {
    crate::abort_on_panic(|| {
        let Some(parsed_value) = with_input(input, parse_spread_method).flatten() else {
            return false;
        };
        unsafe { value.write(parsed_value) };
        true
    })
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and `context` must be valid for
/// `set_values`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_table_values(
    input: FfiSvgInput,
    context: *mut c_void,
    set_values: unsafe extern "C" fn(*mut c_void, *const f32, usize),
) {
    crate::abort_on_panic(|| {
        let values = with_input(input, parse_table_values).unwrap_or_default();
        unsafe { set_values(context, values.as_ptr(), values.len()) };
    });
}

/// # Safety
/// `input` must satisfy [`FfiSvgInput::input`]'s requirements and the output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_view_box(
    input: FfiSvgInput,
    min_x: *mut f64,
    min_y: *mut f64,
    width: *mut f64,
    height: *mut f64,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(values) = with_input(input, parse_view_box).flatten() else {
            return false;
        };
        unsafe {
            min_x.write(values[0]);
            min_y.write(values[1]);
            width.write(values[2]);
            height.write(values[3]);
        }
        true
    })
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
