/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compositor-driven animations of opacity, background color, filter and transform: the
//! description the main thread publishes with its visual context tree, and the sampling of that
//! description at a point in time, which both the compositor and the main thread's intersection
//! observations run.

use std::rc::Rc;

use crate::css::easing::Easing;
use crate::painting::filter_bytes::filter_functions_graph;
use crate::painting::filter_bytes::{FfiFilterFunction, FfiFilterFunctionKind};
use crate::painting::host::{
    FfiVisualAnimationFillMode, FfiVisualAnimationPlaybackDirection, FfiVisualAnimationTargetKind,
    FfiVisualAnimationTransformOperationKind,
};
use libgfx_rust::filter::Filter;
use libgfx_rust::{Color, ColorFilterType, FloatMatrix4x4, rotation_matrix, scale_matrix, translation_matrix};

pub const MAXIMUM_TRANSFORM_OPERATION_VALUE_COUNT: usize = 3;

impl FfiVisualAnimationTargetKind {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Opacity),
            1 => Some(Self::BackgroundColor),
            2 => Some(Self::Filter),
            3 => Some(Self::Transform),
            _ => None,
        }
    }
}

impl FfiVisualAnimationPlaybackDirection {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Normal),
            1 => Some(Self::Reverse),
            2 => Some(Self::Alternate),
            3 => Some(Self::AlternateReverse),
            _ => None,
        }
    }
}

impl FfiVisualAnimationFillMode {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::None),
            1 => Some(Self::Backwards),
            _ => None,
        }
    }
}

impl FfiVisualAnimationTransformOperationKind {
    pub fn from_u8(value: u8) -> Option<Self> {
        Some(match value {
            0 => Self::Translate,
            1 => Self::Translate3d,
            2 => Self::TranslateX,
            3 => Self::TranslateY,
            4 => Self::TranslateZ,
            5 => Self::Scale,
            6 => Self::Scale3d,
            7 => Self::ScaleX,
            8 => Self::ScaleY,
            9 => Self::ScaleZ,
            10 => Self::Rotate,
            11 => Self::RotateX,
            12 => Self::RotateY,
            13 => Self::RotateZ,
            14 => Self::Skew,
            15 => Self::SkewX,
            16 => Self::SkewY,
            _ => return None,
        })
    }
}

impl FfiFilterFunctionKind {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Blur),
            1 => Some(Self::DropShadow),
            2 => Some(Self::Color),
            3 => Some(Self::HueRotate),
            _ => None,
        }
    }
}

/// One operation of a transform list, holding as many values as any operation takes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct VisualAnimationTransformOperation {
    pub kind: FfiVisualAnimationTransformOperationKind,
    values: [f32; MAXIMUM_TRANSFORM_OPERATION_VALUE_COUNT],
    value_count: u8,
}

/// Whether `values` is a value list `kind` takes: the 2D translate, scale and skew take one or two
/// values, the 3D ones three, and every other operation one, all finite.
pub fn transform_operation_values_are_valid(kind: FfiVisualAnimationTransformOperationKind, values: &[f32]) -> bool {
    use FfiVisualAnimationTransformOperationKind as Kind;
    let count_is_valid = match kind {
        Kind::Translate | Kind::Scale | Kind::Skew => values.len() == 1 || values.len() == 2,
        Kind::Translate3d | Kind::Scale3d => values.len() == 3,
        _ => values.len() == 1,
    };
    count_is_valid && values.iter().all(|value| value.is_finite())
}

impl VisualAnimationTransformOperation {
    /// None for more values than any operation takes.
    pub fn new(kind: FfiVisualAnimationTransformOperationKind, values: &[f32]) -> Option<Self> {
        if values.len() > MAXIMUM_TRANSFORM_OPERATION_VALUE_COUNT {
            return None;
        }
        let mut operation = Self {
            kind,
            values: [0.0; MAXIMUM_TRANSFORM_OPERATION_VALUE_COUNT],
            value_count: values.len() as u8,
        };
        operation.values[..values.len()].copy_from_slice(values);
        Some(operation)
    }

    pub fn values(&self) -> &[f32] {
        &self.values[..self.value_count as usize]
    }

    pub fn is_valid(&self) -> bool {
        transform_operation_values_are_valid(self.kind, self.values())
    }

    fn value(&self, index: usize) -> Option<f32> {
        self.values().get(index).copied()
    }

    /// The matrix of an operation whose values are valid for its kind.
    pub fn to_matrix(&self) -> FloatMatrix4x4 {
        use FfiVisualAnimationTransformOperationKind as Kind;
        let first = self.value(0).unwrap_or(0.0);
        let skew = |x: f32, y: f32| FloatMatrix4x4 {
            elements: [
                [1.0, x.tan(), 0.0, 0.0],
                [y.tan(), 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        };
        match self.kind {
            Kind::Translate => translation_matrix(first, self.value(1).unwrap_or(0.0), 0.0),
            Kind::Translate3d => translation_matrix(first, self.value(1).unwrap_or(0.0), self.value(2).unwrap_or(0.0)),
            Kind::TranslateX => translation_matrix(first, 0.0, 0.0),
            Kind::TranslateY => translation_matrix(0.0, first, 0.0),
            Kind::TranslateZ => translation_matrix(0.0, 0.0, first),
            Kind::Scale => scale_matrix(first, self.value(1).unwrap_or(first), 1.0),
            Kind::Scale3d => scale_matrix(first, self.value(1).unwrap_or(1.0), self.value(2).unwrap_or(1.0)),
            Kind::ScaleX => scale_matrix(first, 1.0, 1.0),
            Kind::ScaleY => scale_matrix(1.0, first, 1.0),
            Kind::ScaleZ => scale_matrix(1.0, 1.0, first),
            Kind::Rotate | Kind::RotateZ => rotation_matrix([0.0, 0.0, 1.0], first),
            Kind::RotateX => rotation_matrix([1.0, 0.0, 0.0], first),
            Kind::RotateY => rotation_matrix([0.0, 1.0, 0.0], first),
            Kind::Skew => skew(first, self.value(1).unwrap_or(0.0)),
            Kind::SkewX => skew(first, 0.0),
            Kind::SkewY => skew(0.0, first),
        }
    }

    /// The operation between this one and `other`, which has the same kind and value count.
    fn interpolated_with(&self, other: &Self, progress: f64) -> Self {
        let mut interpolated = *self;
        for (index, value) in interpolated.values.iter_mut().enumerate() {
            let to = other.values[index];
            *value = interpolate_f32(*value, to, progress);
        }
        interpolated
    }
}

fn interpolate_f32(from: f32, to: f32, progress: f64) -> f32 {
    (f64::from(from) + (f64::from(to) - f64::from(from)) * progress) as f32
}

fn round_to_u8(value: f64) -> u8 {
    value.round_ties_even() as u8
}

/// Legacy sRGB colors interpolate premultiplied in gamma-encoded sRGB.
fn interpolate_legacy_srgb_color(from: Color, to: Color, progress: f64) -> Color {
    let from_alpha = f64::from(from.alpha()) / 255.0;
    let to_alpha = f64::from(to.alpha()) / 255.0;
    let alpha = (from_alpha + (to_alpha - from_alpha) * progress).clamp(0.0, 1.0);
    if alpha == 0.0 {
        return Color::TRANSPARENT;
    }
    let interpolate_channel = |from_channel: u8, to_channel: u8| {
        let from_premultiplied = f64::from(from_channel) * from_alpha;
        let to_premultiplied = f64::from(to_channel) * to_alpha;
        let channel = (from_premultiplied + (to_premultiplied - from_premultiplied) * progress) / alpha;
        round_to_u8(channel.clamp(0.0, 255.0))
    };
    Color::from_rgba(
        interpolate_channel(from.red(), to.red()),
        interpolate_channel(from.green(), to.green()),
        interpolate_channel(from.blue(), to.blue()),
        round_to_u8(alpha * 255.0),
    )
}

fn color_operation_amount_is_a_fraction(operation: ColorFilterType) -> bool {
    matches!(
        operation,
        ColorFilterType::Grayscale | ColorFilterType::Invert | ColorFilterType::Opacity | ColorFilterType::Sepia
    )
}

/// Whether two functions interpolate: the same kind, and the same operation for color functions.
fn filter_functions_match(a: &FfiFilterFunction, b: &FfiFilterFunction) -> bool {
    a.kind == b.kind && (a.kind != FfiFilterFunctionKind::Color || a.color_operation == b.color_operation)
}

/// The function of the same kind that changes nothing, which a shorter list is padded with.
fn filter_function_initial_value(function: &FfiFilterFunction) -> FfiFilterFunction {
    let mut initial = *function;
    initial.amount = 0.0;
    initial.offset_x = 0.0;
    initial.offset_y = 0.0;
    initial.color = Color::TRANSPARENT;
    if function.kind == FfiFilterFunctionKind::Color
        && !matches!(
            function.color_operation,
            ColorFilterType::Grayscale | ColorFilterType::Invert | ColorFilterType::Sepia
        )
    {
        initial.amount = 1.0;
    }
    initial
}

fn interpolate_filter_function(from: &FfiFilterFunction, to: &FfiFilterFunction, progress: f64) -> FfiFilterFunction {
    let mut result = *from;
    result.amount = interpolate_f32(from.amount, to.amount, progress);
    result.offset_x = interpolate_f32(from.offset_x, to.offset_x, progress);
    result.offset_y = interpolate_f32(from.offset_y, to.offset_y, progress);
    if from.kind == FfiFilterFunctionKind::DropShadow {
        result.color = interpolate_legacy_srgb_color(from.color, to.color, progress);
    }
    if matches!(
        from.kind,
        FfiFilterFunctionKind::Blur | FfiFilterFunctionKind::DropShadow
    ) {
        result.amount = result.amount.max(0.0);
    }
    if from.kind == FfiFilterFunctionKind::Color {
        result.amount = result.amount.max(0.0);
        if color_operation_amount_is_a_fraction(from.color_operation) {
            result.amount = result.amount.min(1.0);
        }
    }
    result
}

pub fn filter_function_is_valid(function: &FfiFilterFunction) -> bool {
    if !function.amount.is_finite() || !function.offset_x.is_finite() || !function.offset_y.is_finite() {
        return false;
    }
    if matches!(
        function.kind,
        FfiFilterFunctionKind::Blur | FfiFilterFunctionKind::DropShadow
    ) && function.amount < 0.0
    {
        return false;
    }
    if function.kind != FfiFilterFunctionKind::Color {
        return true;
    }
    if function.amount < 0.0 {
        return false;
    }
    !color_operation_amount_is_a_fraction(function.color_operation) || function.amount <= 1.0
}

#[derive(Clone, Debug, PartialEq)]
pub enum VisualAnimationValue {
    Opacity(f32),
    BackgroundColor(Color),
    Filter(Vec<FfiFilterFunction>),
    Transform(Vec<VisualAnimationTransformOperation>),
}

impl VisualAnimationValue {
    fn interpolated_with(&self, to: &Self, progress: f64) -> Option<Self> {
        match (self, to) {
            (Self::Opacity(from), Self::Opacity(to)) => Some(Self::Opacity(interpolate_f32(*from, *to, progress))),
            (Self::BackgroundColor(from), Self::BackgroundColor(to)) => Some(Self::BackgroundColor(
                interpolate_legacy_srgb_color(*from, *to, progress),
            )),
            (Self::Filter(from_functions), Self::Filter(to_functions)) => {
                let mut padded_from = from_functions.clone();
                let mut padded_to = to_functions.clone();
                while padded_from.len() < padded_to.len() {
                    padded_from.push(filter_function_initial_value(&padded_to[padded_from.len()]));
                }
                while padded_to.len() < padded_from.len() {
                    padded_to.push(filter_function_initial_value(&padded_from[padded_to.len()]));
                }
                if !padded_from
                    .iter()
                    .zip(&padded_to)
                    .all(|(from, to)| filter_functions_match(from, to))
                {
                    // Lists that do not interpolate switch over halfway, keeping their own lengths.
                    return Some(Self::Filter(if progress < 0.5 {
                        from_functions.clone()
                    } else {
                        to_functions.clone()
                    }));
                }
                Some(Self::Filter(
                    padded_from
                        .iter()
                        .zip(&padded_to)
                        .map(|(from, to)| interpolate_filter_function(from, to, progress))
                        .collect(),
                ))
            }
            (Self::Transform(from_operations), Self::Transform(to_operations)) => {
                if from_operations.len() != to_operations.len() {
                    return None;
                }
                Some(Self::Transform(
                    from_operations
                        .iter()
                        .zip(to_operations)
                        .map(|(from, to)| from.interpolated_with(to, progress))
                        .collect(),
                ))
            }
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct VisualAnimationKeyframe {
    pub offset: f64,
    pub easing: Easing,
    pub value: VisualAnimationValue,
}

/// The value an animation takes at a point in time, ready to be written into the tree: a filter
/// sample carries the serialized graph, or none for an empty filter list.
#[derive(Clone, Debug, PartialEq)]
pub enum VisualAnimationSample {
    Opacity(f32),
    BackgroundColor(Color),
    Filter(Option<Rc<Vec<u8>>>),
    Transform(FloatMatrix4x4),
}

/// A compositor animation: the timing of the effect at the moment the main thread anchored it to
/// the shared monotonic clock, and the keyframes it interpolates between.
#[derive(Clone, Debug, PartialEq)]
pub struct VisualAnimation {
    pub target_kind: FfiVisualAnimationTargetKind,
    /// The effect nodes of an opacity, background color or filter animation, or the spatial nodes
    /// of a transform animation.
    pub node_indices: Vec<u32>,
    pub monotonic_time_at_anchor_ns: i64,
    pub local_time_at_anchor_ms: f64,
    pub playback_rate: f64,
    pub start_delay_ms: f64,
    pub iteration_duration_ms: f64,
    pub iteration_count: f64,
    pub iteration_start: f64,
    pub playback_direction: FfiVisualAnimationPlaybackDirection,
    pub fill_mode: FfiVisualAnimationFillMode,
    pub easing: Easing,
    pub keyframes: Vec<VisualAnimationKeyframe>,
}

impl Default for VisualAnimation {
    fn default() -> Self {
        Self {
            target_kind: FfiVisualAnimationTargetKind::Opacity,
            node_indices: Vec::new(),
            monotonic_time_at_anchor_ns: 0,
            local_time_at_anchor_ms: 0.0,
            playback_rate: 1.0,
            start_delay_ms: 0.0,
            iteration_duration_ms: 0.0,
            iteration_count: f64::INFINITY,
            iteration_start: 0.0,
            playback_direction: FfiVisualAnimationPlaybackDirection::Normal,
            fill_mode: FfiVisualAnimationFillMode::None,
            easing: Easing::default(),
            keyframes: Vec::new(),
        }
    }
}

/// The seconds a nanosecond count spans, split the way a duration stores them.
fn duration_seconds(nanoseconds: i64) -> f64 {
    (nanoseconds / 1_000_000_000) as f64 + (nanoseconds % 1_000_000_000) as f64 / 1_000_000_000.0
}

impl VisualAnimation {
    /// The time since the anchor, which a clock that has not reached the anchor reads as zero.
    pub fn elapsed_since_anchor_ns(&self, sample_time_ns: i64) -> i64 {
        sample_time_ns.saturating_sub(self.monotonic_time_at_anchor_ns).max(0)
    }

    fn local_time_ms_after(&self, elapsed_ns: i64) -> f64 {
        self.local_time_at_anchor_ms + duration_seconds(elapsed_ns) * 1000.0 * self.playback_rate
    }

    pub fn local_time_ms_at(&self, sample_time_ns: i64) -> f64 {
        self.local_time_ms_after(self.elapsed_since_anchor_ns(sample_time_ns))
    }

    /// The local time the active phase of a finite animation ends at.
    pub fn active_end_ms(&self) -> f64 {
        self.start_delay_ms + self.iteration_duration_ms * self.iteration_count
    }

    /// Whether the animation is in its active phase, or its timing cannot be told.
    pub fn is_active_at(&self, sample_time_ns: i64) -> bool {
        let local_time = self.local_time_ms_at(sample_time_ns);
        if !local_time.is_finite() {
            return true;
        }
        if local_time < self.start_delay_ms {
            return false;
        }
        if !self.iteration_count.is_finite() {
            return true;
        }
        local_time < self.active_end_ms()
    }

    /// Whether a finite animation has reached the end of its active phase.
    pub fn has_finished_at(&self, sample_time_ns: i64) -> bool {
        self.iteration_count.is_finite() && self.local_time_ms_at(sample_time_ns) >= self.active_end_ms()
    }

    pub fn is_valid(&self) -> bool {
        !self.node_indices.is_empty() && self.has_valid_parameters()
    }

    /// Whether the two animate the same values the same way, whatever nodes they drive and
    /// whenever they were anchored: an animation published again with these parameters keeps
    /// the anchor of the one it replaces.
    pub fn has_same_animation_parameters(&self, other: &Self) -> bool {
        self.target_kind == other.target_kind
            && self.playback_rate == other.playback_rate
            && self.start_delay_ms == other.start_delay_ms
            && self.iteration_duration_ms == other.iteration_duration_ms
            && self.iteration_count == other.iteration_count
            && self.iteration_start == other.iteration_start
            && self.playback_direction == other.playback_direction
            && self.fill_mode == other.fill_mode
            && self.easing == other.easing
            && self.keyframes == other.keyframes
    }

    /// Whether the timing and keyframes describe an animation that can be sampled: positive
    /// durations and rates, keyframes from offset 0 to offset 1 in increasing order, and values of
    /// the target kind that interpolate with each other.
    pub fn has_valid_parameters(&self) -> bool {
        if self.monotonic_time_at_anchor_ns < 0
            || !self.local_time_at_anchor_ms.is_finite()
            || !self.playback_rate.is_finite()
            || self.playback_rate <= 0.0
            || !self.start_delay_ms.is_finite()
            || !self.iteration_duration_ms.is_finite()
            || self.iteration_duration_ms <= 0.0
            || self.iteration_count.is_nan()
            || self.iteration_count <= 0.0
            || !self.iteration_start.is_finite()
            || self.iteration_start < 0.0
            || !self.easing.is_valid()
            || self.keyframes.len() < 2
        {
            return false;
        }
        if self.keyframes[0].offset != 0.0 || self.keyframes[self.keyframes.len() - 1].offset != 1.0 {
            return false;
        }

        let mut transform_operation_count = None;
        for (keyframe_index, keyframe) in self.keyframes.iter().enumerate() {
            if !keyframe.offset.is_finite()
                || keyframe.offset < 0.0
                || keyframe.offset > 1.0
                || !keyframe.easing.is_valid()
            {
                return false;
            }
            if keyframe_index > 0 && keyframe.offset <= self.keyframes[keyframe_index - 1].offset {
                return false;
            }
            match (self.target_kind, &keyframe.value) {
                (FfiVisualAnimationTargetKind::Opacity, VisualAnimationValue::Opacity(opacity)) => {
                    if !opacity.is_finite() || *opacity < 0.0 || *opacity > 1.0 {
                        return false;
                    }
                }
                (FfiVisualAnimationTargetKind::BackgroundColor, VisualAnimationValue::BackgroundColor(_)) => {}
                (FfiVisualAnimationTargetKind::Filter, VisualAnimationValue::Filter(functions)) => {
                    if !functions.iter().all(filter_function_is_valid) {
                        return false;
                    }
                }
                (FfiVisualAnimationTargetKind::Transform, VisualAnimationValue::Transform(operations)) => {
                    if operations.is_empty() {
                        return false;
                    }
                    let expected_count = *transform_operation_count.get_or_insert(operations.len());
                    if operations.len() != expected_count {
                        return false;
                    }
                    for (operation_index, operation) in operations.iter().enumerate() {
                        if !operation.is_valid() {
                            return false;
                        }
                        if keyframe_index == 0 {
                            continue;
                        }
                        let VisualAnimationValue::Transform(previous_operations) =
                            &self.keyframes[keyframe_index - 1].value
                        else {
                            return false;
                        };
                        let previous = &previous_operations[operation_index];
                        if operation.kind != previous.kind || operation.values().len() != previous.values().len() {
                            return false;
                        }
                    }
                }
                _ => return false,
            }
        }
        true
    }

    /// The value at `elapsed_ns` after the anchor, or none while the animation is dormant, once
    /// its timing cannot be resolved, or when the interpolated value is not representable.
    pub fn sample(&self, elapsed_ns: i64) -> Option<VisualAnimationSample> {
        if self.iteration_duration_ms <= 0.0 || self.playback_rate <= 0.0 || self.keyframes.len() < 2 {
            return None;
        }

        let mut local_time = self.local_time_ms_after(elapsed_ns);
        if !local_time.is_finite() {
            return None;
        }
        let is_in_before_phase = local_time < self.start_delay_ms;
        if is_in_before_phase {
            if self.fill_mode != FfiVisualAnimationFillMode::Backwards {
                return None;
            }
            local_time = self.start_delay_ms;
        }
        let overall_progress = (local_time - self.start_delay_ms) / self.iteration_duration_ms + self.iteration_start;
        if !overall_progress.is_finite() || overall_progress < 0.0 {
            return None;
        }

        let active_progress = overall_progress - self.iteration_start;
        let is_at_or_after_end = self.iteration_count.is_finite() && active_progress >= self.iteration_count;
        let mut current_iteration = overall_progress.floor();
        let mut simple_iteration_progress = overall_progress % 1.0;
        if is_at_or_after_end {
            let terminal_overall_progress = self.iteration_start + self.iteration_count;
            simple_iteration_progress = terminal_overall_progress % 1.0;
            if simple_iteration_progress == 0.0 {
                simple_iteration_progress = 1.0;
            }
            current_iteration =
                terminal_overall_progress.floor() - if simple_iteration_progress == 1.0 { 1.0 } else { 0.0 };
        }
        let going_forwards = match self.playback_direction {
            FfiVisualAnimationPlaybackDirection::Normal => true,
            FfiVisualAnimationPlaybackDirection::Reverse => false,
            FfiVisualAnimationPlaybackDirection::Alternate => current_iteration % 2.0 == 0.0,
            FfiVisualAnimationPlaybackDirection::AlternateReverse => (current_iteration + 1.0) % 2.0 == 0.0,
        };

        let directed_progress = if going_forwards {
            simple_iteration_progress
        } else {
            1.0 - simple_iteration_progress
        };
        let before_flag = (is_in_before_phase && going_forwards) || (is_at_or_after_end && !going_forwards);
        let transformed_progress = self.easing.evaluate_at(directed_progress, before_flag);
        if !transformed_progress.is_finite() {
            return None;
        }

        let last_keyframe_index = self.keyframes.len() - 1;
        let to_index = (1..=last_keyframe_index)
            .find(|&index| transformed_progress < self.keyframes[index].offset)
            .unwrap_or(last_keyframe_index);
        let from_keyframe = &self.keyframes[to_index - 1];
        let to_keyframe = &self.keyframes[to_index];
        let interval_width = to_keyframe.offset - from_keyframe.offset;
        if interval_width <= 0.0 {
            return None;
        }
        let interval_progress = (transformed_progress - from_keyframe.offset) / interval_width;
        let eased_interval_progress = from_keyframe.easing.evaluate_at(interval_progress, false);
        if !eased_interval_progress.is_finite() {
            return None;
        }
        let value = from_keyframe
            .value
            .interpolated_with(&to_keyframe.value, eased_interval_progress)?;

        match (self.target_kind, value) {
            (FfiVisualAnimationTargetKind::Opacity, VisualAnimationValue::Opacity(opacity)) => opacity
                .is_finite()
                .then(|| VisualAnimationSample::Opacity(opacity.clamp(0.0, 1.0))),
            (FfiVisualAnimationTargetKind::BackgroundColor, VisualAnimationValue::BackgroundColor(color)) => {
                Some(VisualAnimationSample::BackgroundColor(color))
            }
            (FfiVisualAnimationTargetKind::Filter, VisualAnimationValue::Filter(functions)) => {
                if !functions.iter().all(filter_function_is_valid) {
                    return None;
                }
                let graph = filter_functions_graph(functions.iter().copied().map(Filter::from));
                Some(VisualAnimationSample::Filter(
                    graph.map(|graph| Rc::new(graph.serialize())),
                ))
            }
            (FfiVisualAnimationTargetKind::Transform, VisualAnimationValue::Transform(operations)) => {
                let matrix = operations.iter().fold(FloatMatrix4x4::identity(), |matrix, operation| {
                    matrix.multiplied(operation.to_matrix())
                });
                matrix
                    .elements
                    .iter()
                    .flatten()
                    .all(|element| element.is_finite())
                    .then_some(VisualAnimationSample::Transform(matrix))
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::easing::FfiLinearEasingPoint;
    use FfiVisualAnimationTransformOperationKind as TransformKind;

    fn milliseconds(value: i64) -> i64 {
        value * 1_000_000
    }

    fn opacity_keyframe(offset: f64, easing: Easing, opacity: f32) -> VisualAnimationKeyframe {
        VisualAnimationKeyframe {
            offset,
            easing,
            value: VisualAnimationValue::Opacity(opacity),
        }
    }

    fn keyframe(offset: f64, value: VisualAnimationValue) -> VisualAnimationKeyframe {
        VisualAnimationKeyframe {
            offset,
            easing: Easing::default(),
            value,
        }
    }

    fn steps(interval_count: i32, position: u8) -> Easing {
        Easing::Steps {
            interval_count,
            position,
        }
    }

    fn transform(operations: &[(TransformKind, &[f32])]) -> VisualAnimationValue {
        VisualAnimationValue::Transform(
            operations
                .iter()
                .map(|(kind, values)| VisualAnimationTransformOperation::new(*kind, values).unwrap())
                .collect(),
        )
    }

    fn opacity_of(sample: Option<VisualAnimationSample>) -> f32 {
        match sample {
            Some(VisualAnimationSample::Opacity(opacity)) => opacity,
            other => panic!("not an opacity sample: {other:?}"),
        }
    }

    fn color_of(sample: Option<VisualAnimationSample>) -> Color {
        match sample {
            Some(VisualAnimationSample::BackgroundColor(color)) => color,
            other => panic!("not a background color sample: {other:?}"),
        }
    }

    fn assert_approximately(actual: f32, expected: f32) {
        assert!((actual - expected).abs() < 1e-5, "{actual} is not {expected}");
    }

    #[test]
    fn samples_opacity_across_iterations() {
        let animation = VisualAnimation {
            node_indices: vec![0],
            local_time_at_anchor_ms: 250.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                opacity_keyframe(0.0, Easing::default(), 0.0),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };

        assert_approximately(opacity_of(animation.sample(0)), 0.25);
        assert_approximately(opacity_of(animation.sample(milliseconds(500))), 0.75);
        assert_approximately(opacity_of(animation.sample(milliseconds(1000))), 0.25);
    }

    #[test]
    fn clamps_overshooting_opacity_easing() {
        let overshooting_easing = Easing::CubicBezier {
            x1: 0.5,
            y1: -1.0,
            x2: 0.5,
            y2: 2.0,
        };
        let mut animation = VisualAnimation {
            node_indices: vec![0],
            local_time_at_anchor_ms: 100.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                opacity_keyframe(0.0, overshooting_easing, 0.0),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };

        assert_eq!(opacity_of(animation.sample(0)), 0.0);
        animation.local_time_at_anchor_ms = 900.0;
        assert_eq!(opacity_of(animation.sample(0)), 1.0);
    }

    #[test]
    fn applies_playback_direction_and_keyframe_easing() {
        let animation = VisualAnimation {
            node_indices: vec![0],
            local_time_at_anchor_ms: 250.0,
            iteration_duration_ms: 1000.0,
            playback_direction: FfiVisualAnimationPlaybackDirection::AlternateReverse,
            keyframes: vec![
                opacity_keyframe(0.0, steps(4, 1), 0.0),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };

        assert_approximately(opacity_of(animation.sample(0)), 0.75);
        assert_approximately(opacity_of(animation.sample(milliseconds(1000))), 0.25);
    }

    #[test]
    fn clamps_finite_animation_to_end_progress() {
        let mut animation = VisualAnimation {
            node_indices: vec![0],
            local_time_at_anchor_ms: 900.0,
            iteration_duration_ms: 1000.0,
            iteration_count: 1.0,
            keyframes: vec![
                opacity_keyframe(0.0, Easing::default(), 0.0),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };

        assert_approximately(opacity_of(animation.sample(milliseconds(200))), 1.0);
        animation.playback_direction = FfiVisualAnimationPlaybackDirection::Reverse;
        assert_approximately(opacity_of(animation.sample(milliseconds(200))), 0.0);
        animation.iteration_start = 0.5;
        assert_approximately(opacity_of(animation.sample(milliseconds(200))), 0.5);
    }

    #[test]
    fn uses_following_interval_at_keyframe_boundary() {
        let animation = VisualAnimation {
            node_indices: vec![0],
            local_time_at_anchor_ms: 500.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                opacity_keyframe(0.0, Easing::default(), 0.0),
                opacity_keyframe(0.5, steps(4, 0), 0.5),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };

        assert_approximately(opacity_of(animation.sample(0)), 0.625);
    }

    #[test]
    fn interpolates_transform_operations_before_composing() {
        let animation = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Transform,
            node_indices: vec![0],
            local_time_at_anchor_ms: 500.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                keyframe(
                    0.0,
                    transform(&[(TransformKind::TranslateX, &[0.0]), (TransformKind::Rotate, &[0.0])]),
                ),
                keyframe(
                    1.0,
                    transform(&[
                        (TransformKind::TranslateX, &[100.0]),
                        (TransformKind::Rotate, &[std::f32::consts::PI]),
                    ]),
                ),
            ],
            ..VisualAnimation::default()
        };

        let expected = translation_matrix(50.0, 0.0, 0.0)
            .multiplied(rotation_matrix([0.0, 0.0, 1.0], std::f32::consts::FRAC_PI_2));
        let Some(VisualAnimationSample::Transform(sampled)) = animation.sample(0) else {
            panic!("not a transform sample");
        };
        for (row, expected_row) in sampled.elements.iter().zip(&expected.elements) {
            for (value, expected_value) in row.iter().zip(expected_row) {
                assert_approximately(*value, *expected_value);
            }
        }
    }

    #[test]
    fn interpolates_background_color_in_premultiplied_srgb() {
        let animation = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::BackgroundColor,
            node_indices: vec![0],
            local_time_at_anchor_ms: 500.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                keyframe(
                    0.0,
                    VisualAnimationValue::BackgroundColor(Color::from_rgba(255, 0, 0, 0)),
                ),
                keyframe(
                    1.0,
                    VisualAnimationValue::BackgroundColor(Color::from_rgba(0, 0, 255, 255)),
                ),
            ],
            ..VisualAnimation::default()
        };

        let sampled = color_of(animation.sample(0));
        assert_eq!(sampled.red(), 0);
        assert_eq!(sampled.green(), 0);
        assert_eq!(sampled.blue(), 255);
        assert_eq!(sampled.alpha(), 128);
    }

    #[test]
    fn extrapolates_overshooting_background_color_easing() {
        let animation = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::BackgroundColor,
            node_indices: vec![0],
            local_time_at_anchor_ms: 750.0,
            iteration_duration_ms: 1000.0,
            easing: Easing::Linear(vec![
                FfiLinearEasingPoint {
                    input: 0.0,
                    output: 0.0,
                },
                FfiLinearEasingPoint {
                    input: 1.0,
                    output: 2.0,
                },
            ]),
            keyframes: vec![
                keyframe(0.0, VisualAnimationValue::BackgroundColor(Color::from_rgb(100, 20, 30))),
                keyframe(1.0, VisualAnimationValue::BackgroundColor(Color::from_rgb(140, 40, 50))),
            ],
            ..VisualAnimation::default()
        };

        let sampled = color_of(animation.sample(0));
        assert_eq!(sampled.red(), 160);
        assert_eq!(sampled.green(), 50);
        assert_eq!(sampled.blue(), 60);
        assert_eq!(sampled.alpha(), 255);
    }

    #[test]
    fn interpolates_filter_function_initial_values() {
        let drop_shadow = FfiFilterFunction {
            kind: FfiFilterFunctionKind::DropShadow,
            amount: 10.0,
            offset_x: 20.0,
            offset_y: -10.0,
            color: Color::from_rgb(0, 0, 255),
            color_operation: ColorFilterType::Brightness,
        };
        let interpolated_drop_shadow =
            interpolate_filter_function(&filter_function_initial_value(&drop_shadow), &drop_shadow, 0.5);
        assert_eq!(interpolated_drop_shadow.amount, 5.0);
        assert_eq!(interpolated_drop_shadow.offset_x, 10.0);
        assert_eq!(interpolated_drop_shadow.offset_y, -5.0);
        assert_eq!(interpolated_drop_shadow.color.blue(), 255);
        assert_eq!(interpolated_drop_shadow.color.alpha(), 128);

        let grayscale = FfiFilterFunction {
            kind: FfiFilterFunctionKind::Color,
            amount: 0.75,
            offset_x: 0.0,
            offset_y: 0.0,
            color: Color::TRANSPARENT,
            color_operation: ColorFilterType::Grayscale,
        };
        assert_eq!(filter_function_initial_value(&grayscale).amount, 0.0);
        assert_eq!(
            interpolate_filter_function(&filter_function_initial_value(&grayscale), &grayscale, 2.0).amount,
            1.0
        );
    }

    #[test]
    fn rejects_non_finite_interpolated_filter_values() {
        let function = |kind, amount, offset_x, offset_y| FfiFilterFunction {
            kind,
            amount,
            offset_x,
            offset_y,
            color: Color::TRANSPARENT,
            color_operation: ColorFilterType::Brightness,
        };
        let functions = [
            function(FfiFilterFunctionKind::Blur, f32::MAX, 0.0, 0.0),
            function(FfiFilterFunctionKind::DropShadow, f32::MAX, f32::MAX, f32::MAX),
            function(FfiFilterFunctionKind::Color, f32::MAX, 0.0, 0.0),
            function(FfiFilterFunctionKind::HueRotate, f32::MAX, 0.0, 0.0),
        ];
        for function in functions {
            let mut initial = function;
            initial.amount = 0.0;
            initial.offset_x = 0.0;
            initial.offset_y = 0.0;
            let animation = VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Filter,
                node_indices: vec![0],
                local_time_at_anchor_ms: 750.0,
                iteration_duration_ms: 1000.0,
                keyframes: vec![
                    VisualAnimationKeyframe {
                        offset: 0.0,
                        easing: Easing::Linear(vec![
                            FfiLinearEasingPoint {
                                input: 0.0,
                                output: 0.0,
                            },
                            FfiLinearEasingPoint {
                                input: 1.0,
                                output: 2.0,
                            },
                        ]),
                        value: VisualAnimationValue::Filter(vec![initial]),
                    },
                    keyframe(1.0, VisualAnimationValue::Filter(vec![function])),
                ],
                ..VisualAnimation::default()
            };

            assert!(animation.is_valid());
            assert!(animation.sample(0).is_none());
        }
    }

    #[test]
    fn samples_a_filter_list_into_its_serialized_graph() {
        let opacity = |amount| FfiFilterFunction {
            kind: FfiFilterFunctionKind::Color,
            amount,
            offset_x: 0.0,
            offset_y: 0.0,
            color: Color::TRANSPARENT,
            color_operation: ColorFilterType::Opacity,
        };
        let animation = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Filter,
            node_indices: vec![0],
            local_time_at_anchor_ms: 500.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                keyframe(0.0, VisualAnimationValue::Filter(vec![opacity(1.0)])),
                keyframe(1.0, VisualAnimationValue::Filter(vec![opacity(0.0)])),
            ],
            ..VisualAnimation::default()
        };
        let Some(VisualAnimationSample::Filter(Some(bytes))) = animation.sample(0) else {
            panic!("not a filter sample with a graph");
        };
        assert_eq!(
            Filter::deserialize(&bytes),
            Some(Filter::color(ColorFilterType::Opacity, 0.5, None))
        );

        let empty_list = VisualAnimation {
            keyframes: vec![
                keyframe(0.0, VisualAnimationValue::Filter(Vec::new())),
                keyframe(1.0, VisualAnimationValue::Filter(Vec::new())),
            ],
            ..animation
        };
        assert_eq!(empty_list.sample(0), Some(VisualAnimationSample::Filter(None)));
    }

    #[test]
    fn rejects_invalid_timing() {
        let animation = VisualAnimation::default();
        assert!(animation.sample(0).is_none());
        assert!(!animation.is_valid());
    }

    #[test]
    fn applies_backwards_fill_during_start_delay() {
        let mut animation = VisualAnimation {
            node_indices: vec![0],
            local_time_at_anchor_ms: 50.0,
            start_delay_ms: 100.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                opacity_keyframe(0.0, Easing::default(), 0.25),
                opacity_keyframe(1.0, Easing::default(), 0.75),
            ],
            ..VisualAnimation::default()
        };

        assert!(animation.sample(0).is_none());
        animation.fill_mode = FfiVisualAnimationFillMode::Backwards;
        assert_approximately(opacity_of(animation.sample(0)), 0.25);
        assert_approximately(opacity_of(animation.sample(milliseconds(550))), 0.5);
    }

    #[test]
    fn applies_before_flag_only_to_effect_easing_during_start_delay() {
        let jump_start = steps(4, 0);
        let mut animation = VisualAnimation {
            node_indices: vec![0],
            local_time_at_anchor_ms: 50.0,
            start_delay_ms: 100.0,
            iteration_duration_ms: 1000.0,
            fill_mode: FfiVisualAnimationFillMode::Backwards,
            easing: jump_start.clone(),
            keyframes: vec![
                opacity_keyframe(0.0, Easing::default(), 0.0),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };

        assert_eq!(opacity_of(animation.sample(0)), 0.0);

        animation.easing = Easing::default();
        animation.keyframes[0].easing = jump_start;
        assert_eq!(opacity_of(animation.sample(0)), 0.25);
    }

    #[test]
    fn activity_follows_the_delay_and_the_active_end() {
        let animation = VisualAnimation {
            node_indices: vec![0],
            monotonic_time_at_anchor_ns: milliseconds(1000),
            start_delay_ms: 100.0,
            iteration_duration_ms: 1000.0,
            iteration_count: 2.0,
            keyframes: vec![
                opacity_keyframe(0.0, Easing::default(), 0.0),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };

        assert_eq!(animation.elapsed_since_anchor_ns(milliseconds(500)), 0);
        assert!(!animation.is_active_at(0));
        assert!(!animation.is_active_at(milliseconds(1050)));
        assert!(animation.is_active_at(milliseconds(1100)));
        assert!(animation.is_active_at(milliseconds(3099)));
        assert!(!animation.is_active_at(milliseconds(3100)));
        assert!(!animation.has_finished_at(milliseconds(3099)));
        assert!(animation.has_finished_at(milliseconds(3100)));

        let endless = VisualAnimation {
            iteration_count: f64::INFINITY,
            ..animation
        };
        assert!(endless.is_active_at(milliseconds(1_000_000)));
        assert!(!endless.has_finished_at(milliseconds(1_000_000)));
    }

    #[test]
    fn parameter_validation_covers_timing_and_keyframes() {
        let valid = VisualAnimation {
            node_indices: vec![0],
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                opacity_keyframe(0.0, Easing::default(), 0.0),
                opacity_keyframe(0.5, Easing::default(), 0.5),
                opacity_keyframe(1.0, Easing::default(), 1.0),
            ],
            ..VisualAnimation::default()
        };
        assert!(valid.is_valid());

        let invalid_variants: Vec<VisualAnimation> = vec![
            VisualAnimation {
                node_indices: Vec::new(),
                ..valid.clone()
            },
            VisualAnimation {
                monotonic_time_at_anchor_ns: -1,
                ..valid.clone()
            },
            VisualAnimation {
                playback_rate: 0.0,
                ..valid.clone()
            },
            VisualAnimation {
                iteration_count: f64::NAN,
                ..valid.clone()
            },
            VisualAnimation {
                iteration_start: -1.0,
                ..valid.clone()
            },
            VisualAnimation {
                easing: Easing::Linear(Vec::new()),
                ..valid.clone()
            },
            VisualAnimation {
                keyframes: valid.keyframes[..1].to_vec(),
                ..valid.clone()
            },
            VisualAnimation {
                keyframes: vec![valid.keyframes[1].clone(), valid.keyframes[2].clone()],
                ..valid.clone()
            },
            VisualAnimation {
                keyframes: vec![
                    valid.keyframes[0].clone(),
                    opacity_keyframe(0.5, Easing::default(), 0.5),
                    opacity_keyframe(0.5, Easing::default(), 0.5),
                    valid.keyframes[2].clone(),
                ],
                ..valid.clone()
            },
            VisualAnimation {
                keyframes: vec![
                    valid.keyframes[0].clone(),
                    opacity_keyframe(0.5, Easing::default(), 1.5),
                    valid.keyframes[2].clone(),
                ],
                ..valid.clone()
            },
            VisualAnimation {
                keyframes: vec![
                    valid.keyframes[0].clone(),
                    keyframe(1.0, VisualAnimationValue::BackgroundColor(Color::TRANSPARENT)),
                ],
                ..valid.clone()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Transform,
                keyframes: vec![
                    keyframe(0.0, transform(&[(TransformKind::TranslateX, &[0.0])])),
                    keyframe(1.0, transform(&[(TransformKind::TranslateY, &[0.0])])),
                ],
                ..valid.clone()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Transform,
                keyframes: vec![
                    keyframe(0.0, transform(&[(TransformKind::Translate, &[0.0])])),
                    keyframe(1.0, transform(&[(TransformKind::Translate, &[0.0, 1.0])])),
                ],
                ..valid.clone()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Transform,
                keyframes: vec![
                    keyframe(0.0, transform(&[(TransformKind::Translate3d, &[0.0, 0.0])])),
                    keyframe(1.0, transform(&[(TransformKind::Translate3d, &[0.0, 0.0])])),
                ],
                ..valid.clone()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Transform,
                keyframes: vec![keyframe(0.0, transform(&[])), keyframe(1.0, transform(&[]))],
                ..valid.clone()
            },
        ];
        for invalid in &invalid_variants {
            assert!(!invalid.is_valid(), "{invalid:?}");
        }

        let transform_animation = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Transform,
            keyframes: vec![
                keyframe(
                    0.0,
                    transform(&[(TransformKind::Scale, &[1.0]), (TransformKind::Rotate, &[0.0])]),
                ),
                keyframe(
                    1.0,
                    transform(&[(TransformKind::Scale, &[2.0]), (TransformKind::Rotate, &[1.0])]),
                ),
            ],
            ..valid.clone()
        };
        assert!(transform_animation.is_valid());
    }

    #[test]
    fn transform_operations_take_the_value_counts_of_their_kinds() {
        assert!(transform_operation_values_are_valid(TransformKind::Translate, &[1.0]));
        assert!(transform_operation_values_are_valid(
            TransformKind::Translate,
            &[1.0, 2.0]
        ));
        assert!(!transform_operation_values_are_valid(
            TransformKind::Translate,
            &[1.0, 2.0, 3.0]
        ));
        assert!(transform_operation_values_are_valid(
            TransformKind::Scale3d,
            &[1.0, 2.0, 3.0]
        ));
        assert!(!transform_operation_values_are_valid(TransformKind::RotateX, &[]));
        assert!(!transform_operation_values_are_valid(
            TransformKind::RotateX,
            &[f32::NAN]
        ));
        assert!(VisualAnimationTransformOperation::new(TransformKind::Skew, &[1.0, 2.0, 3.0, 4.0]).is_none());
    }
}
