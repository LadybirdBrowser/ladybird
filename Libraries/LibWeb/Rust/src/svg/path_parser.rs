/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::parser::{Input, Parser as Lexer};
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::ffi_support::{FfiUtf16View, OwnedBox};
use libgfx_rust::path::Path;
use libgfx_rust::{FloatPoint, FloatSize};
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

        let mut buffer = StackBuffer::<32>::default();
        write!(buffer, "{:?}", self.0)?;
        let serialized = buffer.as_str();
        if let Some(exponent_offset) = serialized.find('e') {
            let (mantissa, exponent) = serialized.split_at(exponent_offset + 1);
            formatter.write_str(mantissa)?;
            if !exponent.starts_with(['+', '-']) {
                formatter.write_char('+')?;
            }
            formatter.write_str(exponent)
        } else if let Some(integer) = serialized.strip_suffix(".0") {
            formatter.write_str(integer)
        } else {
            formatter.write_str(serialized)
        }
    }
}

struct StackBuffer<const CAPACITY: usize> {
    bytes: [u8; CAPACITY],
    length: usize,
}

impl<const CAPACITY: usize> Default for StackBuffer<CAPACITY> {
    fn default() -> Self {
        Self {
            bytes: [0; CAPACITY],
            length: 0,
        }
    }
}

impl<const CAPACITY: usize> StackBuffer<CAPACITY> {
    fn as_str(&self) -> &str {
        std::str::from_utf8(&self.bytes[..self.length]).expect("StackBuffer holds UTF-8")
    }
}

impl<const CAPACITY: usize> Write for StackBuffer<CAPACITY> {
    fn write_str(&mut self, text: &str) -> std::fmt::Result {
        let end = self.length + text.len();
        if end > CAPACITY {
            return Err(std::fmt::Error);
        }
        self.bytes[self.length..end].copy_from_slice(text.as_bytes());
        self.length = end;
        Ok(())
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

impl PathInstruction {
    fn is_relative(&self) -> bool {
        match *self {
            PathInstruction::ClosePath => false,
            PathInstruction::MoveTo { absolute, .. }
            | PathInstruction::LineTo { absolute, .. }
            | PathInstruction::HorizontalLineTo { absolute, .. }
            | PathInstruction::VerticalLineTo { absolute, .. }
            | PathInstruction::CurveTo { absolute, .. }
            | PathInstruction::SmoothCurveTo { absolute, .. }
            | PathInstruction::QuadraticBezierCurveTo { absolute, .. }
            | PathInstruction::SmoothQuadraticBezierCurveTo { absolute, .. }
            | PathInstruction::EllipticalArc { absolute, .. } => !absolute,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ParsedPath {
    instructions: Vec<PathInstruction>,
}

fn float_point([x, y]: [f32; 2]) -> FloatPoint {
    FloatPoint { x, y }
}

impl ParsedPath {
    pub(crate) fn normalize_for_serialization(&mut self) {
        let normalize = |values: &mut [f32]| {
            for value in values {
                *value = if *value == 0.0 {
                    0.0
                } else {
                    value.clamp(f32::MIN, f32::MAX)
                };
            }
        };
        for instruction in &mut self.instructions {
            match instruction {
                PathInstruction::MoveTo { point, .. }
                | PathInstruction::LineTo { point, .. }
                | PathInstruction::SmoothQuadraticBezierCurveTo { point, .. } => normalize(point),
                PathInstruction::ClosePath => {}
                PathInstruction::HorizontalLineTo { x, .. } | PathInstruction::VerticalLineTo { y: x, .. } => {
                    normalize(std::slice::from_mut(x));
                }
                PathInstruction::CurveTo {
                    control_point_1,
                    control_point_2,
                    point,
                    ..
                } => {
                    normalize(control_point_1);
                    normalize(control_point_2);
                    normalize(point);
                }
                PathInstruction::SmoothCurveTo {
                    control_point_2: control_point,
                    point,
                    ..
                }
                | PathInstruction::QuadraticBezierCurveTo {
                    control_point, point, ..
                } => {
                    normalize(control_point);
                    normalize(point);
                }
                PathInstruction::EllipticalArc {
                    radius,
                    x_axis_rotation,
                    point,
                    ..
                } => {
                    normalize(radius);
                    normalize(std::slice::from_mut(x_axis_rotation));
                    normalize(point);
                }
            }
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.instructions.is_empty()
    }

    /// Hash the instructions consistently with `PartialEq`: zero of either sign hashes alike.
    pub(crate) fn write_content_hash(&self, hasher: &mut impl std::hash::Hasher) {
        fn write_values(hasher: &mut impl std::hash::Hasher, values: &[f32]) {
            for value in values {
                hasher.write_u32(if *value == 0.0 { 0 } else { value.to_bits() });
            }
        }
        hasher.write_usize(self.instructions.len());
        for instruction in &self.instructions {
            match *instruction {
                PathInstruction::MoveTo { absolute, point } => {
                    hasher.write_u8(1 | (absolute as u8) << 4);
                    write_values(hasher, &point);
                }
                PathInstruction::ClosePath => hasher.write_u8(2),
                PathInstruction::LineTo { absolute, point } => {
                    hasher.write_u8(3 | (absolute as u8) << 4);
                    write_values(hasher, &point);
                }
                PathInstruction::HorizontalLineTo { absolute, x } => {
                    hasher.write_u8(4 | (absolute as u8) << 4);
                    write_values(hasher, &[x]);
                }
                PathInstruction::VerticalLineTo { absolute, y } => {
                    hasher.write_u8(5 | (absolute as u8) << 4);
                    write_values(hasher, &[y]);
                }
                PathInstruction::CurveTo {
                    absolute,
                    control_point_1,
                    control_point_2,
                    point,
                } => {
                    hasher.write_u8(6 | (absolute as u8) << 4);
                    write_values(hasher, &control_point_1);
                    write_values(hasher, &control_point_2);
                    write_values(hasher, &point);
                }
                PathInstruction::SmoothCurveTo {
                    absolute,
                    control_point_2,
                    point,
                } => {
                    hasher.write_u8(7 | (absolute as u8) << 4);
                    write_values(hasher, &control_point_2);
                    write_values(hasher, &point);
                }
                PathInstruction::QuadraticBezierCurveTo {
                    absolute,
                    control_point,
                    point,
                } => {
                    hasher.write_u8(8 | (absolute as u8) << 4);
                    write_values(hasher, &control_point);
                    write_values(hasher, &point);
                }
                PathInstruction::SmoothQuadraticBezierCurveTo { absolute, point } => {
                    hasher.write_u8(9 | (absolute as u8) << 4);
                    write_values(hasher, &point);
                }
                PathInstruction::EllipticalArc {
                    absolute,
                    radius,
                    x_axis_rotation,
                    large_arc,
                    sweep,
                    point,
                } => {
                    hasher.write_u8(10 | (absolute as u8) << 4 | (large_arc as u8) << 5 | (sweep as u8) << 6);
                    write_values(hasher, &radius);
                    write_values(hasher, &[x_axis_rotation]);
                    write_values(hasher, &point);
                }
            }
        }
    }

    pub(crate) fn to_gfx_path(&self) -> Path {
        let mut path = Path::new();
        let mut current_point = [0.0, 0.0];
        let mut subpath_start = [0.0, 0.0];
        let mut previous_control_point = None;
        let mut last_instruction = None;

        for instruction in &self.instructions {
            let relative_to_current = |point: [f32; 2]| [point[0] + current_point[0], point[1] + current_point[1]];
            let point = match *instruction {
                PathInstruction::MoveTo { absolute, point } => {
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.move_to(float_point(point));
                    subpath_start = point;
                    point
                }
                PathInstruction::ClosePath => {
                    path.close();
                    subpath_start
                }
                PathInstruction::LineTo { absolute, point } => {
                    let point = if absolute { point } else { relative_to_current(point) };
                    path.line_to(float_point(point));
                    point
                }
                PathInstruction::HorizontalLineTo { absolute, x } => {
                    let point = [if absolute { x } else { current_point[0] + x }, current_point[1]];
                    path.line_to(float_point(point));
                    point
                }
                PathInstruction::VerticalLineTo { absolute, y } => {
                    let point = [current_point[0], if absolute { y } else { current_point[1] + y }];
                    path.line_to(float_point(point));
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
                        float_point(control_point_1),
                        float_point(control_point_2),
                        float_point(point),
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
                        float_point(control_point_1),
                        float_point(control_point_2),
                        float_point(point),
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
                    path.quadratic_bezier_curve_to(float_point(control_point), float_point(point));
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
                    path.quadratic_bezier_curve_to(float_point(control_point), float_point(point));
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
                        float_point(point),
                        FloatSize {
                            width: radius[0],
                            height: radius[1],
                        },
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

        path
    }

    /// The same path with each relative command replaced by its absolute equivalent, or `None` when the path has
    /// no relative commands.
    pub(crate) fn to_absolute(&self) -> Option<ParsedPath> {
        if !self.instructions.iter().any(PathInstruction::is_relative) {
            return None;
        }
        let mut current_point = [0.0f32, 0.0];
        let mut subpath_start = [0.0f32, 0.0];
        let mut instructions = Vec::with_capacity(self.instructions.len());
        for instruction in &self.instructions {
            let resolve = |absolute: bool, point: [f32; 2]| {
                if absolute {
                    point
                } else {
                    [point[0] + current_point[0], point[1] + current_point[1]]
                }
            };
            let (converted, end_point) = match *instruction {
                PathInstruction::MoveTo { absolute, point } => {
                    let point = resolve(absolute, point);
                    subpath_start = point;
                    (PathInstruction::MoveTo { absolute: true, point }, point)
                }
                PathInstruction::ClosePath => (PathInstruction::ClosePath, subpath_start),
                PathInstruction::LineTo { absolute, point } => {
                    let point = resolve(absolute, point);
                    (PathInstruction::LineTo { absolute: true, point }, point)
                }
                PathInstruction::HorizontalLineTo { absolute, x } => {
                    let x = if absolute { x } else { current_point[0] + x };
                    (
                        PathInstruction::HorizontalLineTo { absolute: true, x },
                        [x, current_point[1]],
                    )
                }
                PathInstruction::VerticalLineTo { absolute, y } => {
                    let y = if absolute { y } else { current_point[1] + y };
                    (
                        PathInstruction::VerticalLineTo { absolute: true, y },
                        [current_point[0], y],
                    )
                }
                PathInstruction::CurveTo {
                    absolute,
                    control_point_1,
                    control_point_2,
                    point,
                } => {
                    let point = resolve(absolute, point);
                    (
                        PathInstruction::CurveTo {
                            absolute: true,
                            control_point_1: resolve(absolute, control_point_1),
                            control_point_2: resolve(absolute, control_point_2),
                            point,
                        },
                        point,
                    )
                }
                PathInstruction::SmoothCurveTo {
                    absolute,
                    control_point_2,
                    point,
                } => {
                    let point = resolve(absolute, point);
                    (
                        PathInstruction::SmoothCurveTo {
                            absolute: true,
                            control_point_2: resolve(absolute, control_point_2),
                            point,
                        },
                        point,
                    )
                }
                PathInstruction::QuadraticBezierCurveTo {
                    absolute,
                    control_point,
                    point,
                } => {
                    let point = resolve(absolute, point);
                    (
                        PathInstruction::QuadraticBezierCurveTo {
                            absolute: true,
                            control_point: resolve(absolute, control_point),
                            point,
                        },
                        point,
                    )
                }
                PathInstruction::SmoothQuadraticBezierCurveTo { absolute, point } => {
                    let point = resolve(absolute, point);
                    (
                        PathInstruction::SmoothQuadraticBezierCurveTo { absolute: true, point },
                        point,
                    )
                }
                PathInstruction::EllipticalArc {
                    absolute,
                    radius,
                    x_axis_rotation,
                    large_arc,
                    sweep,
                    point,
                } => {
                    let point = resolve(absolute, point);
                    (
                        PathInstruction::EllipticalArc {
                            absolute: true,
                            radius,
                            x_axis_rotation,
                            large_arc,
                            sweep,
                            point,
                        },
                        point,
                    )
                }
            };
            instructions.push(converted);
            current_point = end_point;
        }
        let mut path = ParsedPath { instructions };
        path.normalize_for_serialization();
        Some(path)
    }

    /// https://svgwg.org/svg2-draft/paths.html#PathElement
    /// https://drafts.csswg.org/css-shapes-1/#interpolating-shape
    /// Interpolates two paths with the same commands, in order, component by component. Returns `None` when the
    /// commands do not match.
    pub(crate) fn interpolate(&self, to: &ParsedPath, delta: f32) -> Option<ParsedPath> {
        if self.instructions.len() != to.instructions.len() {
            return None;
        }
        let delta = f64::from(delta);
        let number = |from: f32, to: f32| (f64::from(from) + (f64::from(to) - f64::from(from)) * delta) as f32;
        let point = |from: [f32; 2], to: [f32; 2]| [number(from[0], to[0]), number(from[1], to[1])];
        // NB: Like the arc command of shape(), an arc whose flags differ between the starting and ending list uses the
        //     set flag for any progress value between 0 and 1.
        let flag = |from: bool, to: bool| {
            if from == to || delta <= 0.0 {
                from
            } else if delta >= 1.0 {
                to
            } else {
                true
            }
        };
        let mut instructions = Vec::with_capacity(self.instructions.len());
        for (from, to) in self.instructions.iter().zip(&to.instructions) {
            instructions.push(match (*from, *to) {
                (
                    PathInstruction::MoveTo {
                        absolute,
                        point: from_point,
                    },
                    PathInstruction::MoveTo {
                        absolute: to_absolute,
                        point: to_point,
                    },
                ) if absolute == to_absolute => PathInstruction::MoveTo {
                    absolute,
                    point: point(from_point, to_point),
                },
                (PathInstruction::ClosePath, PathInstruction::ClosePath) => PathInstruction::ClosePath,
                (
                    PathInstruction::LineTo {
                        absolute,
                        point: from_point,
                    },
                    PathInstruction::LineTo {
                        absolute: to_absolute,
                        point: to_point,
                    },
                ) if absolute == to_absolute => PathInstruction::LineTo {
                    absolute,
                    point: point(from_point, to_point),
                },
                (
                    PathInstruction::HorizontalLineTo { absolute, x: from_x },
                    PathInstruction::HorizontalLineTo {
                        absolute: to_absolute,
                        x: to_x,
                    },
                ) if absolute == to_absolute => PathInstruction::HorizontalLineTo {
                    absolute,
                    x: number(from_x, to_x),
                },
                (
                    PathInstruction::VerticalLineTo { absolute, y: from_y },
                    PathInstruction::VerticalLineTo {
                        absolute: to_absolute,
                        y: to_y,
                    },
                ) if absolute == to_absolute => PathInstruction::VerticalLineTo {
                    absolute,
                    y: number(from_y, to_y),
                },
                (
                    PathInstruction::CurveTo {
                        absolute,
                        control_point_1: from_1,
                        control_point_2: from_2,
                        point: from_point,
                    },
                    PathInstruction::CurveTo {
                        absolute: to_absolute,
                        control_point_1: to_1,
                        control_point_2: to_2,
                        point: to_point,
                    },
                ) if absolute == to_absolute => PathInstruction::CurveTo {
                    absolute,
                    control_point_1: point(from_1, to_1),
                    control_point_2: point(from_2, to_2),
                    point: point(from_point, to_point),
                },
                (
                    PathInstruction::SmoothCurveTo {
                        absolute,
                        control_point_2: from_2,
                        point: from_point,
                    },
                    PathInstruction::SmoothCurveTo {
                        absolute: to_absolute,
                        control_point_2: to_2,
                        point: to_point,
                    },
                ) if absolute == to_absolute => PathInstruction::SmoothCurveTo {
                    absolute,
                    control_point_2: point(from_2, to_2),
                    point: point(from_point, to_point),
                },
                (
                    PathInstruction::QuadraticBezierCurveTo {
                        absolute,
                        control_point: from_control,
                        point: from_point,
                    },
                    PathInstruction::QuadraticBezierCurveTo {
                        absolute: to_absolute,
                        control_point: to_control,
                        point: to_point,
                    },
                ) if absolute == to_absolute => PathInstruction::QuadraticBezierCurveTo {
                    absolute,
                    control_point: point(from_control, to_control),
                    point: point(from_point, to_point),
                },
                (
                    PathInstruction::SmoothQuadraticBezierCurveTo {
                        absolute,
                        point: from_point,
                    },
                    PathInstruction::SmoothQuadraticBezierCurveTo {
                        absolute: to_absolute,
                        point: to_point,
                    },
                ) if absolute == to_absolute => PathInstruction::SmoothQuadraticBezierCurveTo {
                    absolute,
                    point: point(from_point, to_point),
                },
                (
                    PathInstruction::EllipticalArc {
                        absolute,
                        radius: from_radius,
                        x_axis_rotation: from_rotation,
                        large_arc: from_large_arc,
                        sweep: from_sweep,
                        point: from_point,
                    },
                    PathInstruction::EllipticalArc {
                        absolute: to_absolute,
                        radius: to_radius,
                        x_axis_rotation: to_rotation,
                        large_arc: to_large_arc,
                        sweep: to_sweep,
                        point: to_point,
                    },
                ) if absolute == to_absolute => PathInstruction::EllipticalArc {
                    absolute,
                    radius: point(from_radius, to_radius),
                    x_axis_rotation: number(from_rotation, to_rotation),
                    large_arc: flag(from_large_arc, to_large_arc),
                    sweep: flag(from_sweep, to_sweep),
                    point: point(from_point, to_point),
                },
                _ => return None,
            });
        }
        let mut path = ParsedPath { instructions };
        path.normalize_for_serialization();
        Some(path)
    }

    /// https://drafts.csswg.org/css-shapes-1/#interpolating-shape
    /// The commands and points of the shape() equivalent to this path, for interpolating it with a shape(). The first
    /// command of the path becomes the initial <coordinate-pair> of the shape(). Absolute coordinates become positions
    /// and relative ones coordinate pairs, the way shape() holds them after parsing.
    pub(crate) fn to_shape_commands(
        &self,
    ) -> Option<(
        Vec<crate::css::style_value::ShapeCommand>,
        Vec<crate::css::style_value::RetainedShapePoint>,
    )> {
        use crate::css::style_value::{
            RetainedShapePoint, RetainedStyleValueData, ShapeCommand, ShapeCommandKind, ShapeControlPointAnchor,
            StyleValueData,
        };
        let length = |value: f32| StyleValueData::Length {
            value: f64::from(value),
            unit: crate::css::style_compute::px_length_unit(),
        };
        let value = |value: StyleValueData| RetainedStyleValueData::from_owned(value);
        let edge = |offset: f32| {
            value(StyleValueData::Edge {
                has_edge: false,
                edge: 0,
                offset: value(length(offset)),
            })
        };
        let point = |absolute: bool, point: [f32; 2]| {
            if absolute {
                RetainedShapePoint::from_retained_values(edge(point[0]), edge(point[1]))
            } else {
                RetainedShapePoint::from_retained_values(value(length(point[0])), value(length(point[1])))
            }
        };
        let zero = || value(StyleValueData::Percentage { value: 0.0 });
        let mut commands = Vec::with_capacity(self.instructions.len());
        let mut points = Vec::with_capacity(self.instructions.len());
        for (index, instruction) in self.instructions.iter().enumerate() {
            let (kind, absolute) = match *instruction {
                PathInstruction::MoveTo {
                    absolute,
                    point: end_point,
                } => {
                    if index == 0 {
                        // NB: The current point starts at the origin, so a relative first move is absolute too.
                        points.push(point(true, end_point));
                        commands.push(ShapeCommand::new(ShapeCommandKind::From, false));
                        continue;
                    }
                    points.push(point(absolute, end_point));
                    (ShapeCommandKind::Move, absolute)
                }
                _ if index == 0 => return None,
                PathInstruction::ClosePath => (ShapeCommandKind::Close, true),
                PathInstruction::LineTo {
                    absolute,
                    point: end_point,
                } => {
                    points.push(point(absolute, end_point));
                    (ShapeCommandKind::Line, absolute)
                }
                PathInstruction::HorizontalLineTo { absolute, x } => {
                    points.push(RetainedShapePoint::from_retained_values(value(length(x)), zero()));
                    (ShapeCommandKind::HorizontalLine, absolute)
                }
                PathInstruction::VerticalLineTo { absolute, y } => {
                    points.push(RetainedShapePoint::from_retained_values(zero(), value(length(y))));
                    (ShapeCommandKind::VerticalLine, absolute)
                }
                PathInstruction::CurveTo { .. }
                | PathInstruction::SmoothCurveTo { .. }
                | PathInstruction::QuadraticBezierCurveTo { .. }
                | PathInstruction::SmoothQuadraticBezierCurveTo { .. } => {
                    let (kind, absolute, end_point, control_points) = match *instruction {
                        PathInstruction::CurveTo {
                            absolute,
                            control_point_1,
                            control_point_2,
                            point,
                        } => (
                            ShapeCommandKind::Curve,
                            absolute,
                            point,
                            vec![control_point_1, control_point_2],
                        ),
                        PathInstruction::QuadraticBezierCurveTo {
                            absolute,
                            control_point,
                            point,
                        } => (ShapeCommandKind::Curve, absolute, point, vec![control_point]),
                        PathInstruction::SmoothCurveTo {
                            absolute,
                            control_point_2,
                            point,
                        } => (ShapeCommandKind::Smooth, absolute, point, vec![control_point_2]),
                        PathInstruction::SmoothQuadraticBezierCurveTo { absolute, point } => {
                            (ShapeCommandKind::Smooth, absolute, point, vec![])
                        }
                        _ => unreachable!(),
                    };
                    let mut command = ShapeCommand::new(kind, !absolute);
                    points.push(point(absolute, end_point));
                    for (control_index, control_point) in control_points.into_iter().enumerate() {
                        // Absolute control points are positions in the reference box, and relative ones are relative to
                        // the start of the segment.
                        command.control_points[control_index] = if absolute {
                            ShapeControlPointAnchor::Origin
                        } else {
                            ShapeControlPointAnchor::Start
                        };
                        command.control_point_count += 1;
                        points.push(point(absolute, control_point));
                    }
                    commands.push(command);
                    continue;
                }
                PathInstruction::EllipticalArc {
                    absolute,
                    radius,
                    x_axis_rotation,
                    large_arc,
                    sweep,
                    point: end_point,
                } => {
                    let mut command = ShapeCommand::new(ShapeCommandKind::Arc, !absolute);
                    command.arc_clockwise = sweep;
                    command.arc_large = large_arc;
                    command.arc_has_rotation = x_axis_rotation != 0.0;
                    points.push(point(absolute, end_point));
                    points.push(RetainedShapePoint::from_retained_values(
                        value(length(radius[0])),
                        value(length(radius[1])),
                    ));
                    let rotation = || {
                        value(StyleValueData::Angle {
                            value: f64::from(x_axis_rotation),
                            unit: 0,
                        })
                    };
                    points.push(RetainedShapePoint::from_retained_values(rotation(), rotation()));
                    commands.push(command);
                    continue;
                }
            };
            commands.push(ShapeCommand::new(kind, !absolute));
        }
        Some((commands, points))
    }

    /// Whether the path ends with a closepath command, which makes it a closed loop.
    pub(crate) fn ends_with_close_path(&self) -> bool {
        matches!(self.instructions.last(), Some(PathInstruction::ClosePath))
    }

    pub(crate) fn serialize(&self) -> String {
        let mut output = String::new();
        self.serialize_into(&mut output);
        output
    }

    pub(crate) fn serialize_utf16(&self) -> Vec<u16> {
        struct Utf16Output(Vec<u16>);
        impl Write for Utf16Output {
            fn write_str(&mut self, text: &str) -> std::fmt::Result {
                self.0.extend(text.encode_utf16());
                Ok(())
            }
        }
        let mut output = Utf16Output(Vec::with_capacity(self.instructions.len() * 16 + 16));
        self.serialize_into(&mut output);
        output.0
    }

    fn serialize_into(&self, output: &mut impl Write) {
        let number = |value| SerializedNumber(value);
        for (index, instruction) in self.instructions.iter().enumerate() {
            if index > 0 {
                output.write_char(' ').unwrap();
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
                PathInstruction::ClosePath => output.write_char('Z').unwrap(),
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
    }
}

#[cfg(test)]
fn parse_ascii_path(input: &[u8], allow_error_recovery: bool) -> Option<ParsedPath> {
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
        self.lexer.parse_whitespace();
        while !self.lexer.done() {
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
        match self.lexer.current_ascii() {
            Some(b'M' | b'm') => self.parse_move_to(),
            Some(b'Z' | b'z') => {
                self.lexer.consume();
                self.lexer.parse_whitespace();
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
        let absolute = self.lexer.consume() == u16::from(b'M');
        self.lexer.parse_whitespace();

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
        let absolute = self.lexer.consume() == u16::from(b'L');
        self.lexer.parse_whitespace();
        self.parse_coordinate_pair_sequence(|parser, point| {
            parser.instructions.push(PathInstruction::LineTo { absolute, point });
        })
    }

    fn parse_horizontal_line_to(&mut self) -> Result<(), ()> {
        let absolute = self.lexer.consume() == u16::from(b'H');
        self.lexer.parse_whitespace();
        self.parse_coordinate_sequence(|parser, x| {
            parser
                .instructions
                .push(PathInstruction::HorizontalLineTo { absolute, x });
        })
    }

    fn parse_vertical_line_to(&mut self) -> Result<(), ()> {
        let absolute = self.lexer.consume() == u16::from(b'V');
        self.lexer.parse_whitespace();
        self.parse_coordinate_sequence(|parser, y| {
            parser
                .instructions
                .push(PathInstruction::VerticalLineTo { absolute, y });
        })
    }

    fn parse_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.lexer.consume() == u16::from(b'C');
        self.lexer.parse_whitespace();
        loop {
            let control_point_1 = self.lexer.parse_coordinate_pair()?;
            self.lexer.parse_optional_comma_whitespace();
            let control_point_2 = self.lexer.parse_coordinate_pair()?;
            self.lexer.parse_optional_comma_whitespace();
            let point = self.lexer.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::CurveTo {
                absolute,
                control_point_1,
                control_point_2,
                point,
            });
            self.lexer.parse_optional_comma_whitespace();
            if !self.lexer.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_smooth_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.lexer.consume() == u16::from(b'S');
        self.lexer.parse_whitespace();
        loop {
            let control_point_2 = self.lexer.parse_coordinate_pair()?;
            self.lexer.parse_optional_comma_whitespace();
            let point = self.lexer.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::SmoothCurveTo {
                absolute,
                control_point_2,
                point,
            });
            self.lexer.parse_optional_comma_whitespace();
            if !self.lexer.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_quadratic_bezier_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.lexer.consume() == u16::from(b'Q');
        self.lexer.parse_whitespace();
        loop {
            let control_point = self.lexer.parse_coordinate_pair()?;
            self.lexer.parse_optional_comma_whitespace();
            let point = self.lexer.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::QuadraticBezierCurveTo {
                absolute,
                control_point,
                point,
            });
            self.lexer.parse_optional_comma_whitespace();
            if !self.lexer.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_smooth_quadratic_bezier_curve_to(&mut self) -> Result<(), ()> {
        let absolute = self.lexer.consume() == u16::from(b'T');
        self.lexer.parse_whitespace();
        loop {
            let point = self.lexer.parse_coordinate_pair()?;
            self.instructions
                .push(PathInstruction::SmoothQuadraticBezierCurveTo { absolute, point });
            self.lexer.parse_optional_comma_whitespace();
            if !self.lexer.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_elliptical_arc(&mut self) -> Result<(), ()> {
        let absolute = self.lexer.consume() == u16::from(b'A');
        self.lexer.parse_whitespace();
        loop {
            let radius_x = self.lexer.parse_number()?;
            self.lexer.parse_optional_comma_whitespace();
            let radius_y = self.lexer.parse_number()?;
            self.lexer.parse_optional_comma_whitespace();
            let x_axis_rotation = self.lexer.parse_number()?;
            self.lexer.parse_optional_comma_whitespace();
            let large_arc = self.parse_flag()?;
            self.lexer.parse_optional_comma_whitespace();
            let sweep = self.parse_flag()?;
            self.lexer.parse_optional_comma_whitespace();
            let point = self.lexer.parse_coordinate_pair()?;
            self.instructions.push(PathInstruction::EllipticalArc {
                absolute,
                radius: [radius_x, radius_y],
                x_axis_rotation,
                large_arc,
                sweep,
                point,
            });
            self.lexer.parse_optional_comma_whitespace();
            if !self.lexer.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_coordinate_sequence(&mut self, mut visit: impl FnMut(&mut Self, f32)) -> Result<(), ()> {
        let mut first = true;
        loop {
            let coordinate = match self.lexer.parse_number() {
                Ok(coordinate) => coordinate,
                Err(()) if first => return Err(()),
                Err(()) => break,
            };
            first = false;
            visit(self, coordinate);
            self.lexer.parse_optional_comma_whitespace();
            if !self.lexer.matches_comma_whitespace() && !self.lexer.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_coordinate_pair_sequence(&mut self, mut visit: impl FnMut(&mut Self, [f32; 2])) -> Result<(), ()> {
        let mut first = true;
        loop {
            let pair = match self.lexer.parse_coordinate_pair() {
                Ok(pair) => pair,
                Err(()) if first => return Err(()),
                Err(()) => break,
            };
            first = false;
            visit(self, pair);
            self.lexer.parse_optional_comma_whitespace();
            if !self.lexer.matches_comma_whitespace() && !self.lexer.matches_coordinate() {
                break;
            }
        }
        Ok(())
    }

    fn parse_flag(&mut self) -> Result<bool, ()> {
        match self.lexer.current_ascii() {
            Some(b'0') => {
                self.lexer.consume();
                Ok(false)
            }
            Some(b'1') => {
                self.lexer.consume();
                Ok(true)
            }
            _ => Err(()),
        }
    }
}

/// The units of `view`, or none for a view that names both or neither kind of storage.
pub(super) fn svg_input(view: FfiUtf16View<'_>) -> Option<Input<'_>> {
    Some(match view.units()? {
        TokenizerInput::Ascii(units) => Input::Ascii(units),
        TokenizerInput::Utf16(units) => Input::Utf16(units),
    })
}

/// The path that the path data describes, as SVG parses it, with its error recovery: what parsing a string of path
/// data gives Path2D's constructor, which is an empty path where the data describes none.
pub fn gfx_path_from_path_data(input: ak::Utf16StringUnits<'_>) -> Path {
    let input = match input {
        ak::Utf16StringUnits::Ascii(units) => Input::Ascii(units),
        ak::Utf16StringUnits::Utf16(units) => Input::Utf16(units),
    };
    let instructions = Parser::new(input).parse(true).unwrap_or_default();
    ParsedPath { instructions }.to_gfx_path()
}

/// The path `input` describes, as a new path C++ owns, or null where it describes none.
#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_svg_path_data(input: FfiUtf16View<'_>) -> *mut ParsedPath {
    let Some(input) = svg_input(input) else {
        return std::ptr::null_mut();
    };
    let Some(instructions) = Parser::new(input).parse(true) else {
        return std::ptr::null_mut();
    };
    Box::into_raw(Box::new(ParsedPath { instructions }))
}

/// A copy of `path`, as a new path C++ owns.
#[unsafe(no_mangle)]
pub extern "C" fn rust_svg_path_clone(path: &ParsedPath) -> *mut ParsedPath {
    Box::into_raw(Box::new(path.clone()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_svg_path_destroy(path: OwnedBox<ParsedPath>) {
    drop(path);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_svg_path_equals(a: &ParsedPath, b: &ParsedPath) -> bool {
    a == b
}

/// # Safety
/// `context` must be valid for `append` for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_svg_path_serialize(
    path: &ParsedPath,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let serialized = path.serialize();
    unsafe { append(context, serialized.as_ptr(), serialized.len()) };
}

/// Calls `append` once with the byte form of the path that `path` draws, which `Gfx::Path` reads.
///
/// # Safety
/// `context` must be valid for `append` for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_svg_path_encode_gfx_path(
    path: &ParsedPath,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let bytes = path.to_gfx_path().serialize_to_bytes();
    unsafe { append(context, bytes.as_ptr(), bytes.len()) };
}

#[cfg(test)]
mod tests {
    use super::{Input, ParsedPath, Parser, PathInstruction};

    fn path_hash(input: &str) -> u64 {
        use std::hash::Hasher;
        let mut path = ParsedPath {
            instructions: parse(input, false).unwrap(),
        };
        path.normalize_for_serialization();
        let mut hasher = crate::css::style::fast_hash::fast_hasher();
        path.write_content_hash(&mut hasher);
        hasher.finish()
    }

    #[test]
    fn equal_paths_hash_alike_and_different_ones_apart() {
        assert_eq!(path_hash("M 0 0 L 30 40 Z"), path_hash("M0,0L30,40z"));
        assert_eq!(path_hash("M -0 0 H 5"), path_hash("M 0 0 H 5"));
        assert_ne!(path_hash("M 0 0 L 30 40"), path_hash("M 0 0 l 30 40"));
        assert_ne!(path_hash("M 0 0 H 5"), path_hash("M 0 0 V 5"));
        assert_ne!(path_hash("M 0 0 A 1 1 0 0 1 5 5"), path_hash("M 0 0 A 1 1 0 1 0 5 5"));
    }

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
