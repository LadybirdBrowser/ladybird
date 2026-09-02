/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::parser::{Input, Parser as Lexer};
use libgfx_rust::path::{OwnedPath, PathBuilder};
use std::ffi::c_void;
use std::fmt::Write;

struct SerializedNumber(f32);

impl std::fmt::Display for SerializedNumber {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.0 == 0.0 {
            return formatter.write_str("0");
        }
        if self.0.is_nan() {
            return formatter.write_str("nan");
        }
        if self.0.is_infinite() {
            let finite_value = if self.0.is_sign_positive() { f32::MAX } else { f32::MIN };
            return SerializedNumber(finite_value).fmt(formatter);
        }

        let mut serialized = format!("{:?}", self.0);
        if let Some(exponent_offset) = serialized.find('e') {
            if !serialized[exponent_offset + 1..].starts_with(['+', '-']) {
                serialized.insert(exponent_offset + 1, '+');
            }
        } else if serialized.ends_with(".0") {
            serialized.truncate(serialized.len() - 2);
        }
        formatter.write_str(&serialized)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum PathInstruction {
    MoveTo {
        absolute: bool,
        point: [f32; 2],
    },
    ClosePath,
    LineTo {
        absolute: bool,
        point: [f32; 2],
    },
    HorizontalLineTo {
        absolute: bool,
        x: f32,
    },
    VerticalLineTo {
        absolute: bool,
        y: f32,
    },
    CurveTo {
        absolute: bool,
        control_point_1: [f32; 2],
        control_point_2: [f32; 2],
        point: [f32; 2],
    },
    SmoothCurveTo {
        absolute: bool,
        control_point_2: [f32; 2],
        point: [f32; 2],
    },
    QuadraticBezierCurveTo {
        absolute: bool,
        control_point: [f32; 2],
        point: [f32; 2],
    },
    SmoothQuadraticBezierCurveTo {
        absolute: bool,
        point: [f32; 2],
    },
    EllipticalArc {
        absolute: bool,
        radius: [f32; 2],
        x_axis_rotation: f32,
        large_arc: bool,
        sweep: bool,
        point: [f32; 2],
    },
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct ParsedPath {
    instructions: Vec<PathInstruction>,
}

impl ParsedPath {
    pub(crate) fn is_empty(&self) -> bool {
        self.instructions.is_empty()
    }

    pub(crate) fn to_gfx_path(&self) -> OwnedPath {
        let mut path = PathBuilder::new();
        let mut current_point = [0.0, 0.0];
        let mut subpath_start = [0.0, 0.0];
        let mut previous_control_point = None;
        let mut last_instruction = None;

        for instruction in &self.instructions {
            let relative_to_current = |point: [f32; 2]| [point[0] + current_point[0], point[1] + current_point[1]];
            let point = match *instruction {
                PathInstruction::MoveTo { absolute, point } => {
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.move_to(point[0], point[1]);
                    subpath_start = point;
                    point
                }
                PathInstruction::ClosePath => {
                    path.close();
                    subpath_start
                }
                PathInstruction::LineTo { absolute, point } => {
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.line_to(point[0], point[1]);
                    point
                }
                PathInstruction::HorizontalLineTo { absolute, x } => {
                    let point = [if absolute { x } else { current_point[0] + x }, current_point[1]];
                    path.line_to(point[0], point[1]);
                    point
                }
                PathInstruction::VerticalLineTo { absolute, y } => {
                    let point = [current_point[0], if absolute { y } else { current_point[1] + y }];
                    path.line_to(point[0], point[1]);
                    point
                }
                PathInstruction::CurveTo {
                    absolute,
                    control_point_1,
                    control_point_2,
                    point,
                } => {
                    let control_point_1 = if absolute {
                        control_point_1
                    } else {
                        relative_to_current(control_point_1)
                    };
                    let control_point_2 = if absolute {
                        control_point_2
                    } else {
                        relative_to_current(control_point_2)
                    };
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.cubic_bezier_curve_to(
                        control_point_1[0],
                        control_point_1[1],
                        control_point_2[0],
                        control_point_2[1],
                        point[0],
                        point[1],
                    );
                    previous_control_point = Some(control_point_2);
                    point
                }
                PathInstruction::SmoothCurveTo {
                    absolute,
                    control_point_2,
                    point,
                } => {
                    let reflected_from = if matches!(
                        last_instruction,
                        Some(PathInstruction::CurveTo { .. } | PathInstruction::SmoothCurveTo { .. })
                    ) {
                        previous_control_point.unwrap()
                    } else {
                        current_point
                    };
                    let control_point_1 = [
                        2.0 * current_point[0] - reflected_from[0],
                        2.0 * current_point[1] - reflected_from[1],
                    ];
                    let control_point_2 = if absolute {
                        control_point_2
                    } else {
                        relative_to_current(control_point_2)
                    };
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.cubic_bezier_curve_to(
                        control_point_1[0],
                        control_point_1[1],
                        control_point_2[0],
                        control_point_2[1],
                        point[0],
                        point[1],
                    );
                    previous_control_point = Some(control_point_2);
                    point
                }
                PathInstruction::QuadraticBezierCurveTo {
                    absolute,
                    control_point,
                    point,
                } => {
                    let control_point = if absolute {
                        control_point
                    } else {
                        relative_to_current(control_point)
                    };
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.quadratic_bezier_curve_to(control_point[0], control_point[1], point[0], point[1]);
                    previous_control_point = Some(control_point);
                    point
                }
                PathInstruction::SmoothQuadraticBezierCurveTo { absolute, point } => {
                    let reflected_from = if matches!(
                        last_instruction,
                        Some(
                            PathInstruction::QuadraticBezierCurveTo { .. }
                                | PathInstruction::SmoothQuadraticBezierCurveTo { .. }
                        )
                    ) {
                        previous_control_point.unwrap()
                    } else {
                        current_point
                    };
                    let control_point = [
                        2.0 * current_point[0] - reflected_from[0],
                        2.0 * current_point[1] - reflected_from[1],
                    ];
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.quadratic_bezier_curve_to(control_point[0], control_point[1], point[0], point[1]);
                    previous_control_point = Some(control_point);
                    point
                }
                PathInstruction::EllipticalArc {
                    absolute,
                    radius,
                    x_axis_rotation,
                    large_arc,
                    sweep,
                    point,
                } => {
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.elliptical_arc_to(
                        point[0],
                        point[1],
                        radius[0],
                        radius[1],
                        (f64::from(x_axis_rotation).to_radians()) as f32,
                        large_arc,
                        sweep,
                    );
                    point
                }
            };

            if !matches!(
                instruction,
                PathInstruction::CurveTo { .. }
                    | PathInstruction::SmoothCurveTo { .. }
                    | PathInstruction::QuadraticBezierCurveTo { .. }
                    | PathInstruction::SmoothQuadraticBezierCurveTo { .. }
            ) {
                previous_control_point = None;
            }
            current_point = point;
            last_instruction = Some(*instruction);
        }

        path.build()
    }

    pub(crate) fn serialize(&self) -> String {
        let mut output = String::new();
        let number = |value| SerializedNumber(value);
        for (index, instruction) in self.instructions.iter().enumerate() {
            if index > 0 {
                output.push(' ');
            }
            match instruction {
                PathInstruction::MoveTo { absolute, point } => {
                    write!(
                        output,
                        "{} {} {}",
                        if *absolute { 'M' } else { 'm' },
                        number(point[0]),
                        number(point[1])
                    )
                    .unwrap();
                }
                PathInstruction::ClosePath => output.push('Z'),
                PathInstruction::LineTo { absolute, point } => {
                    write!(
                        output,
                        "{} {} {}",
                        if *absolute { 'L' } else { 'l' },
                        number(point[0]),
                        number(point[1])
                    )
                    .unwrap();
                }
                PathInstruction::HorizontalLineTo { absolute, x } => {
                    write!(output, "{} {}", if *absolute { 'H' } else { 'h' }, number(*x)).unwrap();
                }
                PathInstruction::VerticalLineTo { absolute, y } => {
                    write!(output, "{} {}", if *absolute { 'V' } else { 'v' }, number(*y)).unwrap();
                }
                PathInstruction::CurveTo {
                    absolute,
                    control_point_1,
                    control_point_2,
                    point,
                } => {
                    write!(
                        output,
                        "{} {} {} {} {} {} {}",
                        if *absolute { 'C' } else { 'c' },
                        number(control_point_1[0]),
                        number(control_point_1[1]),
                        number(control_point_2[0]),
                        number(control_point_2[1]),
                        number(point[0]),
                        number(point[1])
                    )
                    .unwrap();
                }
                PathInstruction::SmoothCurveTo {
                    absolute,
                    control_point_2,
                    point,
                } => {
                    write!(
                        output,
                        "{} {} {} {} {}",
                        if *absolute { 'S' } else { 's' },
                        number(control_point_2[0]),
                        number(control_point_2[1]),
                        number(point[0]),
                        number(point[1])
                    )
                    .unwrap();
                }
                PathInstruction::QuadraticBezierCurveTo {
                    absolute,
                    control_point,
                    point,
                } => {
                    write!(
                        output,
                        "{} {} {} {} {}",
                        if *absolute { 'Q' } else { 'q' },
                        number(control_point[0]),
                        number(control_point[1]),
                        number(point[0]),
                        number(point[1])
                    )
                    .unwrap();
                }
                PathInstruction::SmoothQuadraticBezierCurveTo { absolute, point } => {
                    write!(
                        output,
                        "{} {} {}",
                        if *absolute { 'T' } else { 't' },
                        number(point[0]),
                        number(point[1])
                    )
                    .unwrap();
                }
                PathInstruction::EllipticalArc {
                    absolute,
                    radius,
                    x_axis_rotation,
                    large_arc,
                    sweep,
                    point,
                } => {
                    write!(
                        output,
                        "{} {} {} {} {} {} {} {}",
                        if *absolute { 'A' } else { 'a' },
                        number(radius[0]),
                        number(radius[1]),
                        number(*x_axis_rotation),
                        u8::from(*large_arc),
                        u8::from(*sweep),
                        number(point[0]),
                        number(point[1])
                    )
                    .unwrap();
                }
            }
        }
        output
    }
}

pub(crate) fn parse_ascii_path(input: &[u8], allow_error_recovery: bool) -> Option<ParsedPath> {
    Parser::new(Input::Ascii(input))
        .parse(allow_error_recovery)
        .map(|instructions| ParsedPath { instructions })
}

pub(crate) fn parse_utf16_path(input: &[u16], allow_error_recovery: bool) -> Option<ParsedPath> {
    Parser::new(Input::Utf16(input))
        .parse(allow_error_recovery)
        .map(|instructions| ParsedPath { instructions })
}

struct Parser<'a> {
    lexer: Lexer<'a>,
    instructions: Vec<PathInstruction>,
}

impl<'a> Parser<'a> {
    fn new(input: Input<'a>) -> Self {
        Self {
            lexer: Lexer::new(input),
            instructions: Vec::new(),
        }
    }

    fn parse(mut self, allow_error_recovery: bool) -> Option<Vec<PathInstruction>> {
        self.parse_whitespace();
        while !self.done() {
            if self.parse_draw_to().is_err() {
                if !allow_error_recovery {
                    return None;
                }
                break;
            }
        }

        if !self.instructions.is_empty() && !matches!(self.instructions[0], PathInstruction::MoveTo { .. }) {
            return None;
        }
        Some(self.instructions)
    }

    fn parse_draw_to(&mut self) -> Result<(), ()> {
        match self.current_ascii() {
            Some(b'M' | b'm') => self.parse_move_to(),
            Some(b'Z' | b'z') => {
                self.consume();
                self.parse_whitespace();
                self.instructions.push(PathInstruction::ClosePath);
                Ok(())
            }
            Some(b'L' | b'l') => self.parse_line_to(),
            Some(b'H' | b'h') => self.parse_horizontal_line_to(),
            Some(b'V' | b'v') => self.parse_vertical_line_to(),
            Some(b'C' | b'c') => self.parse_curve_to(),
            Some(b'S' | b's') => self.parse_smooth_curve_to(),
            Some(b'Q' | b'q') => self.parse_quadratic_bezier_curve_to(),
            Some(b'T' | b't') => self.parse_smooth_quadratic_bezier_curve_to(),
            Some(b'A' | b'a') => self.parse_elliptical_arc(),
            _ => Err(()),
        }
    }

    // https://www.w3.org/TR/SVG2/paths.html#PathDataMovetoCommands
    fn parse_move_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'M');
        self.parse_whitespace();

        let mut first = true;
        self.parse_coordinate_pair_sequence(|parser, point| {
            if first {
                parser.instructions.push(PathInstruction::MoveTo { absolute, point });
            } else {
                // NOTE: "M 1 2 3 4" is equivalent to "M 1 2 L 3 4".
                parser.instructions.push(PathInstruction::LineTo { absolute, point });
            }
            first = false;
        })
    }

    fn parse_line_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'L');
        self.parse_whitespace();
        self.parse_coordinate_pair_sequence(|parser, point| {
            parser.instructions.push(PathInstruction::LineTo { absolute, point });
        })
    }

    fn parse_horizontal_line_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'H');
        self.parse_whitespace();
        self.parse_coordinate_sequence(|parser, x| {
            parser
                .instructions
                .push(PathInstruction::HorizontalLineTo { absolute, x });
        })
    }

    fn parse_vertical_line_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'V');
        self.parse_whitespace();
        self.parse_coordinate_sequence(|parser, y| {
            parser
                .instructions
                .push(PathInstruction::VerticalLineTo { absolute, y });
        })
    }

    fn parse_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'C');
        self.parse_whitespace();
        loop {
            let control_point_1 = self.parse_coordinate_pair()?;
            self.parse_optional_comma_whitespace();
            let control_point_2 = self.parse_coordinate_pair()?;
            self.parse_optional_comma_whitespace();
            let point = self.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::CurveTo {
                absolute,
                control_point_1,
                control_point_2,
                point,
            });
            self.parse_optional_comma_whitespace();
            if !self.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_smooth_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'S');
        self.parse_whitespace();
        loop {
            let control_point_2 = self.parse_coordinate_pair()?;
            self.parse_optional_comma_whitespace();
            let point = self.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::SmoothCurveTo {
                absolute,
                control_point_2,
                point,
            });
            self.parse_optional_comma_whitespace();
            if !self.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_quadratic_bezier_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'Q');
        self.parse_whitespace();
        loop {
            let control_point = self.parse_coordinate_pair()?;
            self.parse_optional_comma_whitespace();
            let point = self.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::QuadraticBezierCurveTo {
                absolute,
                control_point,
                point,
            });
            self.parse_optional_comma_whitespace();
            if !self.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_smooth_quadratic_bezier_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'T');
        self.parse_whitespace();
        loop {
            let point = self.parse_coordinate_pair()?;
            self.instructions
                .push(PathInstruction::SmoothQuadraticBezierCurveTo { absolute, point });
            self.parse_optional_comma_whitespace();
            if !self.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_elliptical_arc(&mut self) -> Result<(), ()> {
        let absolute = self.consume() == u16::from(b'A');
        self.parse_whitespace();
        loop {
            let radius_x = self.parse_number()?;
            self.parse_optional_comma_whitespace();
            let radius_y = self.parse_number()?;
            self.parse_optional_comma_whitespace();
            let x_axis_rotation = self.parse_number()?;
            self.parse_optional_comma_whitespace();
            let large_arc = self.parse_flag()?;
            self.parse_optional_comma_whitespace();
            let sweep = self.parse_flag()?;
            self.parse_optional_comma_whitespace();
            let point = self.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::EllipticalArc {
                absolute,
                radius: [radius_x, radius_y],
                x_axis_rotation,
                large_arc,
                sweep,
                point,
            });
            self.parse_optional_comma_whitespace();
            if !self.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_coordinate_pair(&mut self) -> Result<[f32; 2], ()> {
        let x = self.parse_number()?;
        self.parse_optional_comma_whitespace();
        let y = self.parse_number()?;
        Ok([x, y])
    }

    fn parse_coordinate_sequence(&mut self, mut visit: impl FnMut(&mut Self, f32)) -> Result<(), ()> {
        let mut first = true;
        loop {
            let coordinate = match self.parse_number() {
                Ok(coordinate) => coordinate,
                Err(()) if first => return Err(()),
                Err(()) => break,
            };
            first = false;
            visit(self, coordinate);
            self.parse_optional_comma_whitespace();
            if !self.matches_comma_whitespace() && !self.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_coordinate_pair_sequence(&mut self, mut visit: impl FnMut(&mut Self, [f32; 2])) -> Result<(), ()> {
        let mut first = true;
        loop {
            let pair = match self.parse_coordinate_pair() {
                Ok(pair) => pair,
                Err(()) if first => return Err(()),
                Err(()) => break,
            };
            first = false;
            visit(self, pair);
            self.parse_optional_comma_whitespace();
            if !self.matches_comma_whitespace() && !self.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_number(&mut self) -> Result<f32, ()> {
        self.lexer.parse_number()
    }

    fn parse_flag(&mut self) -> Result<bool, ()> {
        match self.current_ascii() {
            Some(b'0') => {
                self.consume();
                Ok(false)
            }
            Some(b'1') => {
                self.consume();
                Ok(true)
            }
            _ => Err(()),
        }
    }

    fn parse_optional_comma_whitespace(&mut self) {
        self.lexer.parse_optional_comma_whitespace();
    }

    fn parse_whitespace(&mut self) {
        self.lexer.parse_whitespace();
    }

    fn matches_coordinate(&self) -> bool {
        self.lexer.matches_coordinate()
    }

    fn matches_comma_whitespace(&self) -> bool {
        self.lexer.matches_comma_whitespace()
    }

    fn current_ascii(&self) -> Option<u8> {
        self.lexer.current_ascii()
    }

    fn consume(&mut self) -> u16 {
        self.lexer.consume()
    }

    fn done(&self) -> bool {
        self.lexer.done()
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSvgInput {
    pub ascii: *const u8,
    pub utf16: *const u16,
    pub length: usize,
}

impl FfiSvgInput {
    /// # Safety
    /// For non-empty input, exactly one pointer must identify `length` readable units.
    pub(super) unsafe fn input<'a>(self) -> Option<Input<'a>> {
        if self.length == 0 {
            return Some(Input::Ascii(&[]));
        }
        match (self.ascii.is_null(), self.utf16.is_null()) {
            (false, true) => Some(Input::Ascii(unsafe {
                std::slice::from_raw_parts(self.ascii, self.length)
            })),
            (true, false) => Some(Input::Utf16(unsafe {
                std::slice::from_raw_parts(self.utf16, self.length)
            })),
            _ => None,
        }
    }
}

/// # Safety
/// - `input` must satisfy [`FfiSvgInput::input`]'s requirements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_svg_path_data(input: FfiSvgInput) -> *mut c_void {
    let Some(input) = (unsafe { input.input() }) else {
        return std::ptr::null_mut();
    };
    let Some(instructions) = Parser::new(input).parse(true) else {
        return std::ptr::null_mut();
    };
    Box::into_raw(Box::new(ParsedPath { instructions })).cast()
}

/// # Safety
/// `path` must point to a live `ParsedPath`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_svg_path_clone(path: *const c_void) -> *mut c_void {
    let path = unsafe { &*path.cast::<ParsedPath>() };
    Box::into_raw(Box::new(path.clone())).cast()
}

/// # Safety
/// `path` must be a `ParsedPath` allocation returned by this module that has not yet been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_svg_path_destroy(path: *mut c_void) {
    drop(unsafe { Box::from_raw(path.cast::<ParsedPath>()) });
}

/// # Safety
/// `a` and `b` must point to live `ParsedPath` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_svg_path_equals(a: *const c_void, b: *const c_void) -> bool {
    unsafe { *a.cast::<ParsedPath>() == *b.cast::<ParsedPath>() }
}

/// # Safety
/// - `path` must point to a live `ParsedPath`.
/// - `context` must be valid for `append` for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_svg_path_serialize(
    path: *const c_void,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let serialized = unsafe { &*path.cast::<ParsedPath>() }.serialize();
    unsafe { append(context, serialized.as_ptr(), serialized.len()) };
}

/// # Safety
/// `path` must point to a live `ParsedPath`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_svg_path_to_gfx_path(path: *const c_void) -> *mut c_void {
    unsafe { &*path.cast::<ParsedPath>() }.to_gfx_path().into_raw()
}

#[cfg(test)]
mod tests {
    use super::{Input, Parser, PathInstruction};

    fn parse(input: &str, allow_error_recovery: bool) -> Option<Vec<PathInstruction>> {
        Parser::new(Input::Ascii(input.as_bytes())).parse(allow_error_recovery)
    }

    #[test]
    fn parses_every_command_and_repetition_form() {
        let instructions = parse(
            "M1 2 3 4z l5,6 h7 8 V9 10 C11 12 13 14 15 16 17 18 19 20 21 22 s23 24 25 26 Q27 28 29 30 t31 32 A33 34 35 0 1 36 37 a38 39 40 10 41 42",
            false,
        )
        .unwrap();
        assert_eq!(instructions.len(), 15);
        assert_eq!(
            instructions[0],
            PathInstruction::MoveTo {
                absolute: true,
                point: [1.0, 2.0]
            }
        );
        assert_eq!(
            instructions[1],
            PathInstruction::LineTo {
                absolute: true,
                point: [3.0, 4.0]
            }
        );
        assert_eq!(instructions[2], PathInstruction::ClosePath);
        assert!(matches!(
            instructions[8],
            PathInstruction::CurveTo { absolute: true, .. }
        ));
        assert!(matches!(
            instructions[9],
            PathInstruction::CurveTo { absolute: true, .. }
        ));
        assert!(matches!(
            instructions[14],
            PathInstruction::EllipticalArc {
                absolute: false,
                large_arc: true,
                sweep: false,
                ..
            }
        ));
    }

    #[test]
    fn parses_utf16_without_converting_the_input() {
        let input: Vec<u16> = "M.5-.25L1e2 2E-1".encode_utf16().collect();
        let instructions = Parser::new(Input::Utf16(&input)).parse(false).unwrap();
        assert_eq!(
            instructions[0],
            PathInstruction::MoveTo {
                absolute: true,
                point: [0.5, -0.25]
            }
        );
        assert_eq!(
            instructions[1],
            PathInstruction::LineTo {
                absolute: true,
                point: [100.0, 0.2]
            }
        );
    }

    #[test]
    fn implements_error_recovery() {
        assert_eq!(
            parse("M1 2 L3 4 nope", true).unwrap(),
            vec![
                PathInstruction::MoveTo {
                    absolute: true,
                    point: [1.0, 2.0]
                },
                PathInstruction::LineTo {
                    absolute: true,
                    point: [3.0, 4.0]
                },
            ]
        );
        assert_eq!(parse("M1 2 L3 4 nope", false), None);
        assert_eq!(parse("L1 2", true), None);
        assert_eq!(parse("", false), Some(vec![]));
    }

    #[test]
    fn preserves_partial_sequence_handling() {
        assert_eq!(
            parse("M1 2 3", false),
            Some(vec![PathInstruction::MoveTo {
                absolute: true,
                point: [1.0, 2.0]
            }])
        );
        assert_eq!(parse("M1 2,", false).unwrap().len(), 1);
        assert_eq!(parse("M1 2 C3 4 5", true).unwrap().len(), 1);
        assert_eq!(parse("M1 2 C3 4 5", false), None);
    }

    #[test]
    fn serializes_commands_canonically() {
        let path = super::parse_ascii_path(b"m1,2 h3 v4 z", false).unwrap();
        assert_eq!(path.serialize(), "m 1 2 h 3 v 4 Z");
    }

    #[test]
    fn serializes_numbers_compactly() {
        let path = super::parse_ascii_path(
            b"M 1e20 -1e20 L 1e-20 -1e-20 H 1.2345678e20 V -0 C 1.5 2 3 4 5 6",
            false,
        )
        .unwrap();
        assert_eq!(
            path.serialize(),
            "M 1e+20 -1e+20 L 1e-20 -1e-20 H 1.2345678e+20 V 0 C 1.5 2 3 4 5 6"
        );

        let path = super::parse_ascii_path(b"M 1e40 -1e40", false).unwrap();
        assert_eq!(path.serialize(), "M 3.4028235e+38 -3.4028235e+38");
    }
}
