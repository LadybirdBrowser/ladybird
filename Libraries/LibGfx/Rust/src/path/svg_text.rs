/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The path as the text of SVG path data, with absolute commands and no separator before a command.

use super::curves::Point;
use super::geometry::{PathElement, PathGeometry, Segment};
use crate::geometry::FloatPoint;
use std::fmt::Write;

/// The furthest that the quadratic curves that stand for a conic may be from it.
const CONIC_TO_QUADS_TOLERANCE: f64 = 1.0 / 1024.0;

/// A conic becomes at most 2^5 quadratic curves.
const MAXIMUM_CONIC_SPLIT_DEPTH: u32 = 5;

pub fn to_svg_string(geometry: &PathGeometry) -> String {
    let mut output = String::new();
    if !geometry.has_geometry() {
        return output;
    }
    for element in geometry.elements() {
        match element {
            PathElement::MoveTo(point) => write_command(&mut output, 'M', &[point]),
            PathElement::Segment(Segment::Line([_, end])) => write_command(&mut output, 'L', &[end]),
            PathElement::Segment(Segment::Quad([_, control, end])) => write_command(&mut output, 'Q', &[control, end]),
            PathElement::Segment(Segment::Cubic([_, control1, control2, end])) => {
                write_command(&mut output, 'C', &[control1, control2, end]);
            }
            PathElement::Segment(Segment::Conic(points, weight)) => {
                for (control, end) in quads_for_conic(points, weight) {
                    write_command(&mut output, 'Q', &[control, end]);
                }
            }
            PathElement::Close([current, start]) => {
                if current != start {
                    write_command(&mut output, 'L', &[start]);
                }
                output.push('Z');
            }
        }
    }
    output
}

fn write_command(output: &mut String, command: char, points: &[FloatPoint]) {
    output.push(command);
    for (index, point) in points.iter().enumerate() {
        if index > 0 {
            output.push(' ');
        }
        write_number(output, point.x);
        output.push(' ');
        write_number(output, point.y);
    }
}

/// Writes the number with eight significant digits and without trailing zeros, in the notation that
/// printf() gives for "%.8g": a fixed point when the decimal exponent is at least -4 and less than 8,
/// and an exponent of at least two digits otherwise.
fn write_number(output: &mut String, value: f32) {
    const SIGNIFICANT_DIGITS: i32 = 8;
    let value = f64::from(value);
    let scientific = format!("{:.*e}", (SIGNIFICANT_DIGITS - 1) as usize, value);
    let (mantissa, exponent) = scientific
        .split_once('e')
        .expect("the scientific notation has an exponent");
    let exponent: i32 = exponent.parse().expect("the exponent is a number");
    if (-4..SIGNIFICANT_DIGITS).contains(&exponent) {
        let fixed = format!("{:.*}", (SIGNIFICANT_DIGITS - 1 - exponent) as usize, value);
        output.push_str(without_trailing_zeros(&fixed));
    } else {
        output.push_str(without_trailing_zeros(mantissa));
        let sign = if exponent < 0 { '-' } else { '+' };
        write!(output, "e{sign}{:02}", exponent.abs()).expect("writing to a String succeeds");
    }
}

fn without_trailing_zeros(number: &str) -> &str {
    if !number.contains('.') {
        return number;
    }
    number.trim_end_matches('0').trim_end_matches('.')
}

/// The quadratic curves that stand for a conic: the conic halved n times, with n the smallest number for
/// which the pieces are within the tolerance of it. The quadratic curve with the control points of a conic
/// of weight w is furthest from it halfway, by (1 - w) / (4 (1 + w)) times the length of p0 - 2 p1 + p2,
/// and each halving cuts that distance to about a quarter.
fn quads_for_conic(points: [FloatPoint; 3], weight: f32) -> Vec<(FloatPoint, FloatPoint)> {
    let conic = (points.map(Point::from_float), f64::from(weight));
    let ([p0, p1, p2], weight) = conic;
    let mut distance = (1.0 - weight).abs() / (4.0 * (1.0 + weight)) * p0.minus(p1.scaled(2.0)).plus(p2).length();
    let mut depth = 0;
    while depth < MAXIMUM_CONIC_SPLIT_DEPTH && distance > CONIC_TO_QUADS_TOLERANCE {
        distance /= 4.0;
        depth += 1;
    }

    let mut pieces = vec![conic];
    for _ in 0..depth {
        pieces = pieces
            .iter()
            .flat_map(|piece| {
                let (first, second) = halve_conic(*piece);
                [first, second]
            })
            .collect();
    }
    let piece_count = pieces.len();
    pieces
        .iter()
        .enumerate()
        .map(|(index, ([_, control, end], _))| {
            // The end of the whole conic stays exactly where it is.
            let end = if index + 1 == piece_count {
                points[2]
            } else {
                end.to_float()
            };
            (control.to_float(), end)
        })
        .collect()
}

/// The two halves of a conic, each with the weight that keeps the weights of its ends at 1. In
/// homogeneous coordinates, a conic is a quadratic curve, which halves as one.
fn halve_conic(([p0, p1, p2], weight): ([Point; 3], f64)) -> (([Point; 3], f64), ([Point; 3], f64)) {
    let lift = |point: Point, w: f64| (point.x * w, point.y * w, w);
    let middle_of = |a: (f64, f64, f64), b: (f64, f64, f64)| {
        (
            a.0 + (b.0 - a.0) * 0.5,
            a.1 + (b.1 - a.1) * 0.5,
            a.2 + (b.2 - a.2) * 0.5,
        )
    };
    let project = |h: (f64, f64, f64)| Point::new(h.0 / h.2, h.1 / h.2);
    let (h0, h1, h2) = (lift(p0, 1.0), lift(p1, weight), lift(p2, 1.0));
    let first_control = middle_of(h0, h1);
    let second_control = middle_of(h1, h2);
    let middle = middle_of(first_control, second_control);
    let middle_point = project(middle);
    (
        (
            [p0, project(first_control), middle_point],
            first_control.2 / (h0.2 * middle.2).sqrt(),
        ),
        (
            [middle_point, project(second_control), p2],
            second_control.2 / (middle.2 * h2.2).sqrt(),
        ),
    )
}
