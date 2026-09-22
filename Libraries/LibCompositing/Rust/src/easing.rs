/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Easing functions: the flat descriptor a host hands across FFI, an owned form that outlives the
//! host's storage, and the evaluation both share.

pub const STEP_POSITION_JUMP_START: u8 = 0;
pub const STEP_POSITION_JUMP_NONE: u8 = 2;
pub const STEP_POSITION_JUMP_BOTH: u8 = 3;
pub const STEP_POSITION_START: u8 = 4;
const MAXIMUM_STEP_POSITION: u8 = 5;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiEasingKind {
    Linear,
    CubicBezier,
    Steps,
}

impl FfiEasingKind {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Linear),
            1 => Some(Self::CubicBezier),
            2 => Some(Self::Steps),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiLinearEasingPoint {
    pub input: f64,
    pub output: f64,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiEasingDescriptor {
    pub kind: FfiEasingKind,
    pub linear_points: *const FfiLinearEasingPoint,
    pub linear_point_count: usize,
    pub x1: f64,
    pub y1: f64,
    pub x2: f64,
    pub y2: f64,
    pub interval_count: i32,
    pub step_position: u8,
}

/// An easing function that owns its control points, for values that outlive the descriptor they
/// were handed in.
#[derive(Clone, Debug, PartialEq)]
pub enum Easing {
    Linear(Vec<FfiLinearEasingPoint>),
    CubicBezier { x1: f64, y1: f64, x2: f64, y2: f64 },
    Steps { interval_count: i32, position: u8 },
}

impl Default for Easing {
    /// The `linear` keyword.
    fn default() -> Self {
        Self::Linear(vec![
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0,
            },
            FfiLinearEasingPoint {
                input: 1.0,
                output: 1.0,
            },
        ])
    }
}

/// # Safety
///
/// The descriptor's linear point range must be live when its kind is linear.
unsafe fn linear_points(descriptor: &FfiEasingDescriptor) -> &[FfiLinearEasingPoint] {
    if descriptor.linear_point_count == 0 {
        return &[];
    }
    // SAFETY: The caller guarantees the point range is live for a linear descriptor.
    unsafe { std::slice::from_raw_parts(descriptor.linear_points, descriptor.linear_point_count) }
}

impl Easing {
    /// # Safety
    ///
    /// The descriptor's linear point range must be live when its kind is linear.
    pub unsafe fn from_descriptor(descriptor: &FfiEasingDescriptor) -> Self {
        match descriptor.kind {
            // SAFETY: The caller guarantees the point range is live for a linear descriptor.
            FfiEasingKind::Linear => Self::Linear(unsafe { linear_points(descriptor) }.to_vec()),
            FfiEasingKind::CubicBezier => Self::CubicBezier {
                x1: descriptor.x1,
                y1: descriptor.y1,
                x2: descriptor.x2,
                y2: descriptor.y2,
            },
            FfiEasingKind::Steps => Self::Steps {
                interval_count: descriptor.interval_count,
                position: descriptor.step_position,
            },
        }
    }

    pub fn kind(&self) -> FfiEasingKind {
        match self {
            Self::Linear(_) => FfiEasingKind::Linear,
            Self::CubicBezier { .. } => FfiEasingKind::CubicBezier,
            Self::Steps { .. } => FfiEasingKind::Steps,
        }
    }

    pub fn evaluate_at(&self, input_progress: f64, before_flag: bool) -> f64 {
        match self {
            Self::Linear(points) => evaluate_linear_easing(points, input_progress, before_flag),
            Self::CubicBezier { x1, y1, x2, y2 } => evaluate_cubic_bezier_easing(*x1, *y1, *x2, *y2, input_progress),
            Self::Steps {
                interval_count,
                position,
            } => evaluate_steps_easing(*interval_count, *position, input_progress, before_flag),
        }
    }

    /// Whether the function can be evaluated: a linear function needs two or more finite points
    /// with non-decreasing inputs, a cubic bezier its x control points within [0, 1], and steps a
    /// positive interval count with a known position.
    pub fn is_valid(&self) -> bool {
        match self {
            Self::Linear(points) => {
                if points.len() < 2 {
                    return false;
                }
                let mut previous_input = f64::MIN;
                for point in points {
                    if !point.input.is_finite() || !point.output.is_finite() || point.input < previous_input {
                        return false;
                    }
                    previous_input = point.input;
                }
                true
            }
            Self::CubicBezier { x1, y1, x2, y2 } => {
                [x1, y1, x2, y2].iter().all(|value| value.is_finite())
                    && (0.0..=1.0).contains(x1)
                    && (0.0..=1.0).contains(x2)
            }
            Self::Steps {
                interval_count,
                position,
            } => *interval_count > 0 && *position <= MAXIMUM_STEP_POSITION,
        }
    }
}

pub fn evaluate_easing_descriptor(descriptor: &FfiEasingDescriptor, input_progress: f64, before_flag: bool) -> f64 {
    match descriptor.kind {
        FfiEasingKind::Linear => {
            // SAFETY: A linear descriptor handed to the style host keeps its points live for the call.
            let points = unsafe { linear_points(descriptor) };
            assert!(!points.is_empty());
            evaluate_linear_easing(points, input_progress, before_flag)
        }
        FfiEasingKind::CubicBezier => evaluate_cubic_bezier_easing(
            descriptor.x1,
            descriptor.y1,
            descriptor.x2,
            descriptor.y2,
            input_progress,
        ),
        FfiEasingKind::Steps => evaluate_steps_easing(
            descriptor.interval_count,
            descriptor.step_position,
            input_progress,
            before_flag,
        ),
    }
}

pub fn evaluate_linear_easing(points: &[FfiLinearEasingPoint], input_progress: f64, before_flag: bool) -> f64 {
    // https://drafts.csswg.org/css-easing/#linear-easing-function-output
    // To calculate linear easing output progress for a given linear easing function func,
    // an input progress value inputProgress, and an optional before flag (defaulting to false),
    // perform the following:

    // 1. Let points be func’s control points.

    // 2. If points holds only a single item, return the output progress value of that item.
    if points.len() == 1 {
        return points[0].output;
    }

    // 3. If inputProgress matches the input progress value of the first point in points,
    // and the before flag is true, return the first point’s output progress value.
    if input_progress == points[0].input && before_flag {
        return points[0].output;
    }

    // 4. If inputProgress matches the input progress value of at least one point in points,
    // return the output progress value of the last such point.
    if let Some(point) = points.iter().rfind(|point| input_progress == point.input) {
        return point.output;
    }

    // 5. Otherwise, find two control points in points, A and B, which will be used for interpolation:
    let (a, b) = if input_progress < points[0].input {
        // 1. If inputProgress is smaller than any input progress value in points,
        // let A and B be the first two items in points.
        // If A and B have the same input progress value, return A’s output progress value.
        let (a, b) = (&points[0], &points[1]);
        if a.input == b.input {
            return a.output;
        }
        (a, b)
    } else if input_progress > points[points.len() - 1].input {
        // 2. If inputProgress is larger than any input progress value in points,
        // let A and B be the last two items in points.
        // If A and B have the same input progress value, return B’s output progress value.
        let (a, b) = (&points[points.len() - 2], &points[points.len() - 1]);
        if a.input == b.input {
            return b.output;
        }
        (a, b)
    } else {
        // 3. Otherwise, let A be the last control point whose input progress value is smaller than inputProgress,
        // and let B be the first control point whose input progress value is larger than inputProgress.
        let a = points
            .iter()
            .rfind(|point| point.input < input_progress)
            .expect("canonical linear easing has a preceding point");
        let b = points
            .iter()
            .find(|point| point.input > input_progress)
            .expect("canonical linear easing has a following point");
        (a, b)
    };

    // 6. Linearly interpolate (or extrapolate) inputProgress along the line defined by A and B, and return the result.
    let factor = (input_progress - a.input) / (b.input - a.input);
    a.output + factor * (b.output - a.output)
}

fn cubic_bezier_at(first: f64, second: f64, parameter: f64) -> f64 {
    let a = 1.0 - 3.0 * second + 3.0 * first;
    let b = 3.0 * second - 6.0 * first;
    let c = 3.0 * first;
    (a * parameter * parameter * parameter) + (b * parameter * parameter) + (c * parameter)
}

pub fn evaluate_cubic_bezier_easing(x1: f64, y1: f64, x2: f64, y2: f64, input_progress: f64) -> f64 {
    // https://drafts.csswg.org/css-easing-1/#cubic-bezier-algo
    // For input progress values outside the range [0, 1], the curve is extended infinitely using tangent of the curve
    // at the closest endpoint as follows:

    // - For input progress values less than zero,
    if input_progress < 0.0 {
        // 1. If the x value of P1 is greater than zero, use a straight line that passes through P1 and P0 as the
        //    tangent.
        if x1 > 0.0 {
            return y1 / x1 * input_progress;
        }

        // 2. Otherwise, if the x value of P2 is greater than zero, use a straight line that passes through P2 and P0 as
        //    the tangent.
        if x2 > 0.0 {
            return y2 / x2 * input_progress;
        }

        // 3. Otherwise, let the output progress value be zero for all input progress values in the range [-∞, 0).
        return 0.0;
    }

    // - For input progress values greater than one,
    if input_progress > 1.0 {
        // 1. If the x value of P2 is less than one, use a straight line that passes through P2 and P3 as the tangent.
        if x2 < 1.0 {
            return (1.0 - y2) / (1.0 - x2) * (input_progress - 1.0) + 1.0;
        }

        // 2. Otherwise, if the x value of P1 is less than one, use a straight line that passes through P1 and P3 as the
        //    tangent.
        if x1 < 1.0 {
            return (1.0 - y1) / (1.0 - x1) * (input_progress - 1.0) + 1.0;
        }

        // 3. Otherwise, let the output progress value be one for all input progress values in the range (1, ∞].
        return 1.0;
    }

    // The evaluation of this curve is covered in many sources such as [FUND-COMP-GRAPHICS].
    // NB: Use Newton-Raphson iteration to solve x(t) = inputProgress, then fall back to bisection.
    let derivative = |parameter: f64| {
        let a = 1.0 - 3.0 * x2 + 3.0 * x1;
        let b = 3.0 * x2 - 6.0 * x1;
        let c = 3.0 * x1;
        3.0 * a * parameter * parameter + 2.0 * b * parameter + c
    };
    let epsilon = 1e-7;
    let mut parameter = input_progress;
    for _ in 0..8 {
        let difference = cubic_bezier_at(x1, x2, parameter) - input_progress;
        if difference.abs() < epsilon {
            return cubic_bezier_at(y1, y2, parameter);
        }
        let derivative = derivative(parameter);
        if derivative.abs() < 1e-12 {
            break;
        }
        parameter -= difference / derivative;
    }

    let mut low = 0.0;
    let mut high = 1.0;
    parameter = input_progress;
    for _ in 0..64 {
        let value = cubic_bezier_at(x1, x2, parameter);
        if (value - input_progress).abs() < epsilon {
            return cubic_bezier_at(y1, y2, parameter);
        }
        if input_progress > value {
            low = parameter;
        } else {
            high = parameter;
        }
        parameter = (low + high) / 2.0;
    }
    cubic_bezier_at(y1, y2, parameter)
}

pub fn evaluate_steps_easing(interval_count: i32, position: u8, input_progress: f64, before_flag: bool) -> f64 {
    // https://drafts.csswg.org/css-easing-1/#step-easing-algo
    let mut current_step = (input_progress * f64::from(interval_count)).floor();

    // 2. If the step position property is one of:
    //    - jump-start,
    //    - jump-both,
    //    increment current step by one.
    if matches!(
        position,
        STEP_POSITION_JUMP_START | STEP_POSITION_START | STEP_POSITION_JUMP_BOTH
    ) {
        current_step += 1.0;
    }

    // 3. If both of the following conditions are true:
    //    - the before flag is set, and
    //    - input progress value × steps mod 1 equals zero (that is, if input progress value × steps is integral), then
    //    decrement current step by one.
    let step_progress = input_progress * f64::from(interval_count);
    if before_flag && step_progress.trunc() == step_progress {
        current_step -= 1.0;
    }

    // 4. If input progress value ≥ 0 and current step < 0, let current step be zero.
    if input_progress >= 0.0 && current_step < 0.0 {
        current_step = 0.0;
    }

    // 5. Calculate jumps based on the step position as follows:

    //    jump-start or jump-end -> steps
    //    jump-none -> steps - 1
    //    jump-both -> steps + 1
    let jumps = match position {
        STEP_POSITION_JUMP_NONE => interval_count - 1,
        STEP_POSITION_JUMP_BOTH => interval_count + 1,
        _ => interval_count,
    };

    // 6. If input progress value ≤ 1 and current step > jumps, let current step be jumps.
    if input_progress <= 1.0 && current_step > f64::from(jumps) {
        current_step = f64::from(jumps);
    }

    // 7. The output progress value is current step / jumps.
    current_step / f64::from(jumps)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn evaluates_linear_easing() {
        let points = [
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0,
            },
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.5,
            },
            FfiLinearEasingPoint {
                input: 1.0,
                output: 1.0,
            },
        ];
        assert_eq!(evaluate_linear_easing(&points, 0.0, true), 0.0);
        assert_eq!(evaluate_linear_easing(&points, 0.0, false), 0.5);
        assert_eq!(evaluate_linear_easing(&points, 0.5, false), 0.75);
    }

    #[test]
    fn evaluates_cubic_bezier_easing() {
        assert!((evaluate_cubic_bezier_easing(0.42, 0.0, 0.58, 1.0, 0.5) - 0.5).abs() < 1e-7);
        assert_eq!(evaluate_cubic_bezier_easing(0.5, 1.0, 1.0, 1.0, -0.5), -1.0);
    }

    #[test]
    fn evaluates_steps_easing() {
        assert_eq!(evaluate_steps_easing(4, 1, 0.5, false), 0.5);
        assert_eq!(evaluate_steps_easing(4, 1, 0.5, true), 0.25);
        assert_eq!(evaluate_steps_easing(4, STEP_POSITION_JUMP_START, 0.0, false), 0.25);
    }

    #[test]
    fn an_owned_easing_evaluates_like_its_descriptor() {
        let points = [
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0,
            },
            FfiLinearEasingPoint {
                input: 1.0,
                output: 2.0,
            },
        ];
        let descriptors = [
            FfiEasingDescriptor {
                kind: FfiEasingKind::Linear,
                linear_points: points.as_ptr(),
                linear_point_count: points.len(),
                x1: 0.0,
                y1: 0.0,
                x2: 0.0,
                y2: 0.0,
                interval_count: 0,
                step_position: 0,
            },
            FfiEasingDescriptor {
                kind: FfiEasingKind::CubicBezier,
                linear_points: std::ptr::null(),
                linear_point_count: 0,
                x1: 0.42,
                y1: 0.0,
                x2: 0.58,
                y2: 1.0,
                interval_count: 0,
                step_position: 0,
            },
            FfiEasingDescriptor {
                kind: FfiEasingKind::Steps,
                linear_points: std::ptr::null(),
                linear_point_count: 0,
                x1: 0.0,
                y1: 0.0,
                x2: 0.0,
                y2: 0.0,
                interval_count: 4,
                step_position: 1,
            },
        ];
        for descriptor in &descriptors {
            let easing = unsafe { Easing::from_descriptor(descriptor) };
            assert!(easing.is_valid());
            assert_eq!(easing.kind(), descriptor.kind);
            for (progress, before_flag) in [(0.0, true), (0.25, false), (0.5, false), (1.0, false)] {
                assert_eq!(
                    easing.evaluate_at(progress, before_flag),
                    evaluate_easing_descriptor(descriptor, progress, before_flag)
                );
            }
        }
    }

    #[test]
    fn validity_covers_each_kind() {
        assert!(Easing::default().is_valid());
        assert!(!Easing::Linear(Vec::new()).is_valid());
        assert!(
            !Easing::Linear(vec![FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0
            }])
            .is_valid()
        );
        assert!(
            !Easing::Linear(vec![
                FfiLinearEasingPoint {
                    input: 1.0,
                    output: 0.0
                },
                FfiLinearEasingPoint {
                    input: 0.0,
                    output: 1.0
                },
            ])
            .is_valid()
        );
        assert!(
            !Easing::Linear(vec![
                FfiLinearEasingPoint {
                    input: 0.0,
                    output: 0.0
                },
                FfiLinearEasingPoint {
                    input: 1.0,
                    output: f64::NAN
                },
            ])
            .is_valid()
        );
        assert!(
            Easing::CubicBezier {
                x1: 0.5,
                y1: -1.0,
                x2: 0.5,
                y2: 2.0
            }
            .is_valid()
        );
        assert!(
            !Easing::CubicBezier {
                x1: 1.5,
                y1: 0.0,
                x2: 0.5,
                y2: 1.0
            }
            .is_valid()
        );
        assert!(
            !Easing::CubicBezier {
                x1: 0.5,
                y1: f64::INFINITY,
                x2: 0.5,
                y2: 1.0
            }
            .is_valid()
        );
        assert!(
            Easing::Steps {
                interval_count: 4,
                position: MAXIMUM_STEP_POSITION
            }
            .is_valid()
        );
        assert!(
            !Easing::Steps {
                interval_count: 0,
                position: 0
            }
            .is_valid()
        );
        assert!(
            !Easing::Steps {
                interval_count: 4,
                position: MAXIMUM_STEP_POSITION + 1
            }
            .is_valid()
        );
    }
}
