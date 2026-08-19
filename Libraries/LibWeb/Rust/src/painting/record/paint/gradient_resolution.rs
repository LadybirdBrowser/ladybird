/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::color_resolution::{ColorResolutionInput, Rgba, to_color};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
use crate::css::style_value::{RetainedColorStop, RetainedStyleValueData, StyleValueData};
use crate::painting::display_list::recorder::{ColorStops, ConicGradientData, LinearGradientData, RadialGradientData};
use crate::painting::record::PaintRecorder;
use crate::painting::style_queries;
use crate::painting::visual_context::basic_shapes::{position_resolved, resolve_circle_size, resolve_ellipse_size};
use libgfx_rust::{
    Color, FloatRect, GradientInterpolationMethod, GradientInterpolationType, HueInterpolationMethod, IntRect,
    PolarColorSpace, RectangularColorSpace,
};

#[derive(Clone, Copy, Debug, PartialEq)]
struct GradientStop {
    color: Color,
    position: f32,
    transition_hint: Option<f32>,
}

struct ResolvedColorStopData {
    list: Vec<GradientStop>,
    repeat_length: Option<f32>,
    repeating: bool,
}

pub(crate) enum ResolvedGradientPaint {
    Linear(LinearGradientData),
    Conic {
        data: ConicGradientData,
        position: CssPixelPoint,
    },
    Radial {
        data: RadialGradientData,
        center: CssPixelPoint,
        size: CssPixelSize,
    },
}

#[allow(clippy::excessive_precision, clippy::approx_constant)]
fn ak_log2_f32(x: f32) -> f32 {
    if x == 0.0 {
        return f32::NEG_INFINITY;
    }
    if x <= 0.0 || x.is_nan() {
        return f32::NAN;
    }

    let bits = x.to_bits();
    let exponent = (((bits >> 23) & 0xff) as i32 - 127) as f32;
    let mantissa = bits & 0x007f_ffff;
    if mantissa == 0 {
        return exponent;
    }

    let mut m = f32::from_bits((bits & 0x8000_0000) | (127 << 23) | mantissa);
    let sqrt_2 = 1.414213562373095048801688724209698079_f64 as f32;
    let mut inverted = false;
    if m > sqrt_2 {
        inverted = true;
        m = 2.0 / m;
    }
    let s = (m - 1.0) / (m + 1.0);
    let s2 = s * s;
    let high_approx = s2
        * (0.6666666666666735130_f64 as f32
            + s2 * (0.3999999999940941908_f64 as f32
                + s2 * (0.2857142874366239149_f64 as f32
                    + s2 * (0.2222219843214978396_f64 as f32
                        + s2 * (0.1818357216161805012_f64 as f32
                            + s2 * (0.1531383769920937332_f64 as f32 + s2 * 0.1479819860511658591_f64 as f32))))));
    let log2_e = 1.442695040888963407359924681001892137_f64 as f32;
    let mut log2_mantissa = log2_e * (2.0 * s + s * high_approx);
    if inverted {
        log2_mantissa = 1.0 - log2_mantissa;
    }
    exponent + log2_mantissa
}

fn ak_pow_f32(x: f32, y: f32) -> f32 {
    if y.is_nan() {
        return y;
    }
    if y == 0.0 {
        return 1.0;
    }
    if x == 0.0 {
        return 0.0;
    }
    if y == 1.0 {
        return x;
    }
    let y_as_int = y as i32;
    if y == y_as_int as f32 {
        let mut result = x;
        let mut i = 0;
        while (i as f32) < y.abs() - 1.0 {
            result *= x;
            i += 1;
        }
        if y < 0.0 {
            result = (1.0f64 / result as f64) as f32;
        }
        return result;
    }
    ((y * ak_log2_f32(x)) as f64).exp2() as f32
}

fn ak_log_f32(x: f32) -> f32 {
    (x as f64).ln() as f32
}

fn color_stop_step(previous_stop: &GradientStop, next_stop: &GradientStop, position: f32) -> f32 {
    if position < previous_stop.position {
        return 0.0;
    }
    if position > next_stop.position {
        return 1.0;
    }
    let stop_length = next_stop.position - previous_stop.position;
    if stop_length <= 0.0 {
        return 1.0;
    }
    let p = (position - previous_stop.position) / stop_length;
    let Some(transition_hint) = next_stop.transition_hint else {
        return p;
    };
    if transition_hint >= 1.0 {
        return 0.0;
    }
    if transition_hint <= 0.0 {
        return 1.0;
    }
    ak_pow_f32(p, ak_log_f32(0.5) / ak_log_f32(transition_hint))
}

fn replace_transition_hints_with_normal_color_stops(color_stop_list: &[GradientStop]) -> Vec<GradientStop> {
    let mut stops_with_replaced_transition_hints = Vec::new();

    let first_color_stop = &color_stop_list[0];
    // First color stop in the list should never have transition hint value
    assert!(first_color_stop.transition_hint.is_none());
    stops_with_replaced_transition_hints.push(GradientStop {
        color: first_color_stop.color,
        position: first_color_stop.position,
        transition_hint: None,
    });

    // This loop replaces transition hints with five regular points, calculated using the
    // formula defined in the spec. After rendering using linear interpolation, this will
    // produce a result close enough to that obtained if the color of each point were calculated
    // using the non-linear formula from the spec.
    for i in 1..color_stop_list.len() {
        let color_stop = &color_stop_list[i];
        let Some(transition_hint) = color_stop.transition_hint else {
            stops_with_replaced_transition_hints.push(GradientStop {
                color: color_stop.color,
                position: color_stop.position,
                transition_hint: None,
            });
            continue;
        };

        let previous_color_stop = &color_stop_list[i - 1];
        let next_color_stop = &color_stop_list[i];

        let distance_between_stops = next_color_stop.position - previous_color_stop.position;

        let transition_hint_relative_sampling_positions = [
            transition_hint * 0.33,
            transition_hint * 0.66,
            transition_hint,
            transition_hint + (1.0 - transition_hint) * 0.33,
            transition_hint + (1.0 - transition_hint) * 0.66,
        ];

        for transition_hint_relative_sampling_position in transition_hint_relative_sampling_positions {
            let position =
                previous_color_stop.position + transition_hint_relative_sampling_position * distance_between_stops;
            let value = color_stop_step(previous_color_stop, next_color_stop, position);
            let color = previous_color_stop.color.mixed_with(next_color_stop.color, value);
            stops_with_replaced_transition_hints.push(GradientStop {
                color,
                position,
                transition_hint: None,
            });
        }

        stops_with_replaced_transition_hints.push(GradientStop {
            color: color_stop.color,
            position: color_stop.position,
            transition_hint: None,
        });
    }

    stops_with_replaced_transition_hints
}

fn expand_repeat_length(color_stop_list: &[GradientStop], repeat_length: f32) -> Vec<GradientStop> {
    assert!(repeat_length.is_finite());
    assert!(repeat_length > 0.0);

    // https://drafts.csswg.org/css-images/#repeating-gradients
    // When rendered, however, the color-stops are repeated infinitely in both directions, with their
    // positions shifted by multiples of the difference between the last specified color-stop's position
    // and the first specified color-stop's position. For example, repeating-linear-gradient(red 10px, blue 50px)
    // is equivalent to linear-gradient(..., red -30px, blue 10px, red 10px, blue 50px, red 50px, blue 90px, ...).

    let first_stop_position = color_stop_list[0].position;
    let negative_repeat_count = (first_stop_position / repeat_length).ceil() as i32;
    let positive_repeat_count = ((1.0 - first_stop_position) / repeat_length).ceil() as i32;

    let mut color_stop_list_with_expanded_repeat: Vec<GradientStop> = color_stop_list.to_vec();

    let get_color_between_stops = |position: f32, current_stop: &GradientStop, previous_stop: &GradientStop| {
        let distance_between_stops = current_stop.position - previous_stop.position;
        let percentage = (position - previous_stop.position) / distance_between_stops;
        previous_stop.color.mixed_with(current_stop.color, percentage)
    };

    for repeat_count in 1..=negative_repeat_count {
        for stop in color_stop_list.iter().rev() {
            let mut stop = *stop;
            stop.position -= repeat_length * repeat_count as f32;
            if stop.position < 0.0 {
                stop.color = get_color_between_stops(0.0, &stop, &color_stop_list_with_expanded_repeat[0]);
                stop.position = 0.0;
                color_stop_list_with_expanded_repeat.insert(0, stop);
                break;
            }
            color_stop_list_with_expanded_repeat.insert(0, stop);
        }
    }

    for repeat_count in 1..positive_repeat_count {
        for stop in color_stop_list {
            let mut stop = *stop;
            stop.position += repeat_length * repeat_count as f32;
            if stop.position > 1.0 {
                stop.color = get_color_between_stops(
                    1.0,
                    &stop,
                    color_stop_list_with_expanded_repeat
                        .last()
                        .expect("the expanded list starts from a non-empty stop list"),
                );
                stop.position = 1.0;
                color_stop_list_with_expanded_repeat.push(stop);
                break;
            }
            color_stop_list_with_expanded_repeat.push(stop);
        }
    }

    color_stop_list_with_expanded_repeat
}

fn has_degenerate_repeat_length(resolved_color_stops: &ResolvedColorStopData) -> bool {
    resolved_color_stops
        .repeat_length
        .is_some_and(|repeat_length| !repeat_length.is_finite() || repeat_length <= 0.0)
}

fn average_color_for_degenerate_repeating_gradient(color_stop_list: &[GradientStop]) -> Color {
    assert!(!color_stop_list.is_empty());

    if color_stop_list.len() == 1 {
        return color_stop_list[0].color;
    }

    // https://drafts.csswg.org/css-images-3/#repeating-gradients
    // For zero-length repeating gradients, average a gradient with the same
    // colors and equally-spaced stops over an arbitrary non-zero distance.
    let mut premultiplied_red = 0.0f32;
    let mut premultiplied_green = 0.0f32;
    let mut premultiplied_blue = 0.0f32;
    let mut alpha = 0.0f32;
    let weight = 0.5f32 / (color_stop_list.len() - 1) as f32;

    let mut add_weighted_color = |color: Color| {
        let color_alpha = color.alpha() as f32 / 255.0;
        alpha += color_alpha * weight;
        premultiplied_red += color.red() as f32 / 255.0 * color_alpha * weight;
        premultiplied_green += color.green() as f32 / 255.0 * color_alpha * weight;
        premultiplied_blue += color.blue() as f32 / 255.0 * color_alpha * weight;
    };

    for i in 1..color_stop_list.len() {
        add_weighted_color(color_stop_list[i - 1].color);
        add_weighted_color(color_stop_list[i].color);
    }

    if alpha == 0.0 {
        return Color::TRANSPARENT;
    }

    let normalized_to_u8 = |value: f32| -> u8 { ((value * 255.0).round() as i64).clamp(0, 255) as u8 };

    Color::from_rgba(
        normalized_to_u8(premultiplied_red / alpha),
        normalized_to_u8(premultiplied_green / alpha),
        normalized_to_u8(premultiplied_blue / alpha),
        normalized_to_u8(alpha),
    )
}

fn normalize_degenerate_repeating_gradient(resolved_color_stops: &mut ResolvedColorStopData) {
    let average_color = average_color_for_degenerate_repeating_gradient(&resolved_color_stops.list);
    resolved_color_stops.list = vec![
        GradientStop {
            color: average_color,
            position: 0.0,
            transition_hint: None,
        },
        GradientStop {
            color: average_color,
            position: 1.0,
            transition_hint: None,
        },
    ];
    resolved_color_stops.repeat_length = None;
    resolved_color_stops.repeating = false;
}

fn expand_color_stops_for_painting(
    color_stop_list: Vec<GradientStop>,
    repeat_length: Option<f32>,
) -> Vec<GradientStop> {
    let expanded = match repeat_length {
        Some(repeat_length) => expand_repeat_length(&color_stop_list, repeat_length),
        None => color_stop_list,
    };
    replace_transition_hints_with_normal_color_stops(&expanded)
}

fn finalized_color_stops(mut resolved_color_stops: ResolvedColorStopData) -> ColorStops {
    if has_degenerate_repeat_length(&resolved_color_stops) {
        normalize_degenerate_repeating_gradient(&mut resolved_color_stops);
    }
    // Expand color stops for painting (replace transition hints and expand repeat length)
    resolved_color_stops.list =
        expand_color_stops_for_painting(resolved_color_stops.list, resolved_color_stops.repeat_length);
    to_color_stops(&resolved_color_stops.list, resolved_color_stops.repeating, |position| {
        position
    })
}

fn to_color_stops(
    color_stop_list: &[GradientStop],
    repeating: bool,
    mut to_position: impl FnMut(f32) -> f32,
) -> ColorStops {
    let mut colors = Vec::with_capacity(color_stop_list.len());
    let mut positions = Vec::with_capacity(color_stop_list.len());
    for color_stop in color_stop_list {
        let position = to_position(color_stop.position);
        if colors.last() == Some(&color_stop.color) && positions.last() == Some(&position) {
            continue;
        }
        colors.push(color_stop.color);
        positions.push(position);
    }
    ColorStops {
        colors,
        positions,
        repeating,
    }
}

fn resolve_color_stop_positions(
    color_stop_list: &[RetainedColorStop],
    color_input: &ColorResolutionInput<'_>,
    mut resolve_position_to_float: impl FnMut(&StyleValueData) -> f32,
    repeating: bool,
) -> ResolvedColorStopData {
    assert!(!color_stop_list.is_empty());

    let color_stop_length = |stop: &RetainedColorStop| if stop.second_position_value().is_some() { 2 } else { 1 };

    let mut resolved_color_stops: Vec<GradientStop> = Vec::new();
    for stop in color_stop_list {
        let rgba = to_color(stop.color_value(), color_input).expect("a computed gradient stop color resolves in Rust");
        let resolved_stop = GradientStop {
            color: Color::from_rgba(rgba.r, rgba.g, rgba.b, rgba.a),
            position: f32::NAN,
            transition_hint: None,
        };
        for _ in 0..color_stop_length(stop) {
            resolved_color_stops.push(resolved_stop);
        }
    }

    // https://drafts.csswg.org/css-images-3/#color-stop-fixup
    // 1. If the first color stop does not have a position, set its position to 0%.
    if color_stop_list[0].position_value().is_none() {
        resolved_color_stops[0].position = 0.0;
    }
    //    If the last color stop does not have a position, set its position to 100%
    let last_stop = &color_stop_list[color_stop_list.len() - 1];
    if last_stop.second_position_value().is_none() && last_stop.position_value().is_none() {
        resolved_color_stops
            .last_mut()
            .expect("a non-empty stop list resolves to a non-empty list")
            .position = 1.0;
    }

    // 2. If a color stop or transition hint has a position that is less than the
    //    specified position of any color stop or transition hint before it in the list,
    //    set its position to be equal to the largest specified position of any color stop
    //    or transition hint before it.
    let mut max_previous_color_stop_or_hint = f32::NEG_INFINITY;
    let mut resolve_stop_position = |position: &StyleValueData| -> f32 {
        let resolved = resolve_position_to_float(position);
        let value = if resolved < max_previous_color_stop_or_hint {
            max_previous_color_stop_or_hint
        } else {
            resolved
        };
        max_previous_color_stop_or_hint = value;
        value
    };
    let mut resolved_index = 0;
    for stop in color_stop_list {
        if let Some(transition_hint) = stop.transition_hint_value() {
            resolved_color_stops[resolved_index].transition_hint = Some(resolve_stop_position(transition_hint));
        }
        if let Some(position) = stop.position_value() {
            resolved_color_stops[resolved_index].position = resolve_stop_position(position);
        }
        if let Some(second_position) = stop.second_position_value() {
            resolved_index += 1;
            resolved_color_stops[resolved_index].position = resolve_stop_position(second_position);
        }
        resolved_index += 1;
    }

    // 3. If any color stop still does not have a position, then, for each run of adjacent color stops
    //    without positions, set their positions so that they are evenly spaced between the preceding
    //    and following color stops with positions.
    // Note: Though not mentioned anywhere in the specification transition hints are counted as "color stops with positions".
    let color_stop_has_position =
        |color_stop: &GradientStop| color_stop.transition_hint.is_some() || color_stop.position.is_finite();
    let mut i = 1;
    while i < resolved_color_stops.len() - 1 {
        if !resolved_color_stops[i].position.is_finite() {
            let run_start = i - 1;
            let start_position = resolved_color_stops[i]
                .transition_hint
                .unwrap_or(resolved_color_stops[run_start].position);
            i += 1;
            while i < color_stop_list.len() - 1 && !color_stop_has_position(&resolved_color_stops[i]) {
                i += 1;
            }
            let run_end = i;
            let end_position = resolved_color_stops[run_end]
                .transition_hint
                .unwrap_or(resolved_color_stops[run_end].position);
            let spacing = (end_position - start_position) / (run_end - run_start) as f32;
            #[allow(clippy::needless_range_loop)]
            for j in (run_start + 1)..run_end {
                resolved_color_stops[j].position = start_position + (j - run_start) as f32 * spacing;
            }
        }
        i += 1;
    }

    // Determine the location of the transition hint as a percentage of the distance between the two color stops,
    // denoted as a number between 0 and 1, where 0 indicates the hint is placed right on the first color stop,
    // and 1 indicates the hint is placed right on the second color stop.
    for i in 1..resolved_color_stops.len() {
        let previous_position = resolved_color_stops[i - 1].position;
        let color_stop = &mut resolved_color_stops[i];
        if let Some(transition_hint) = color_stop.transition_hint {
            let stop_length = color_stop.position - previous_position;
            color_stop.transition_hint = Some(if stop_length > 0.0 {
                (transition_hint - previous_position) / stop_length
            } else {
                0.0
            });
        }
    }

    let repeat_length = repeating
        .then(|| resolved_color_stops[resolved_color_stops.len() - 1].position - resolved_color_stops[0].position);

    ResolvedColorStopData {
        list: resolved_color_stops,
        repeat_length,
        repeating,
    }
}

fn stop_color_resolution_input(style: ComputedValuesView<'_>) -> ColorResolutionInput<'_> {
    let inherited_text = style.inherited_text();
    let current_color = inherited_text.color;
    ColorResolutionInput {
        scheme: Some(style.inherited_ui().color_scheme),
        current_color: Some(Rgba {
            r: (current_color >> 16) as u8,
            g: (current_color >> 8) as u8,
            b: current_color as u8,
            a: (current_color >> 24) as u8,
        }),
        current_color_value: style_queries::handle_value(&inherited_text.color_style_value),
        length: None,
        channels: None,
    }
}

fn stop_length_px(position: &StyleValueData, percentage_basis_px: f64) -> f64 {
    match position {
        StyleValueData::Length { value, unit } => {
            let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[*unit as usize];
            assert!(ratio.is_finite(), "a computed gradient stop length is absolute");
            ratio * value
        }
        StyleValueData::Percentage { value } => value * 0.01 * percentage_basis_px,
        StyleValueData::Calculated { .. } => {
            crate::css::calc::resolve_calculated_length_without_context(position, percentage_basis_px)
                .expect("a computed gradient stop length resolves without context")
        }
        _ => unreachable!("a gradient stop position is a length, a percentage or a calculation"),
    }
}

fn angle_to_degrees(value: &StyleValueData) -> f64 {
    match value {
        StyleValueData::Angle { value, unit } => crate::css::calc::ANGLE_UNIT_CANONICAL_RATIOS[*unit as usize] * value,
        StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_angle_without_context(value)
            .expect("a computed gradient angle resolves without context"),
        _ => unreachable!("a computed gradient angle is an angle or a calculation"),
    }
}

const ONE_TURN_DEGREES: f64 = 360.0;

fn stop_angle_fraction_of_turn(position: &StyleValueData) -> f32 {
    let degrees = match position {
        StyleValueData::Angle { value, unit } => crate::css::calc::ANGLE_UNIT_CANONICAL_RATIOS[*unit as usize] * value,
        StyleValueData::Percentage { value } => value * 0.01 * ONE_TURN_DEGREES,
        StyleValueData::Calculated { .. } => {
            crate::css::calc::resolve_calculated_angle_with_percentage_basis_without_context(position, ONE_TURN_DEGREES)
                .expect("a computed conic gradient stop angle resolves without context")
        }
        _ => unreachable!("a conic gradient stop position is an angle, a percentage or a calculation"),
    };
    (degrees / ONE_TURN_DEGREES) as f32
}

fn to_gfx_rectangular_color_space(color_space: u8) -> RectangularColorSpace {
    use crate::css::css_enums::rectangular_color_space;
    match color_space {
        rectangular_color_space::SRGB => RectangularColorSpace::Srgb,
        rectangular_color_space::SRGB_LINEAR => RectangularColorSpace::SrgbLinear,
        rectangular_color_space::DISPLAY_P3 => RectangularColorSpace::DisplayP3,
        rectangular_color_space::DISPLAY_P3_LINEAR => RectangularColorSpace::DisplayP3Linear,
        rectangular_color_space::A98_RGB => RectangularColorSpace::A98Rgb,
        rectangular_color_space::PROPHOTO_RGB => RectangularColorSpace::ProphotoRgb,
        rectangular_color_space::REC2020 => RectangularColorSpace::Rec2020,
        rectangular_color_space::LAB => RectangularColorSpace::Lab,
        rectangular_color_space::OKLAB => RectangularColorSpace::Oklab,
        rectangular_color_space::XYZ => RectangularColorSpace::Xyz,
        rectangular_color_space::XYZ_D50 => RectangularColorSpace::XyzD50,
        rectangular_color_space::XYZ_D65 => RectangularColorSpace::XyzD65,
        _ => unreachable!("a computed rectangular color space holds an unknown keyword"),
    }
}

fn to_gfx_polar_color_space(color_space: u8) -> PolarColorSpace {
    use crate::css::css_enums::polar_color_space;
    match color_space {
        polar_color_space::HSL => PolarColorSpace::Hsl,
        polar_color_space::HWB => PolarColorSpace::Hwb,
        polar_color_space::LCH => PolarColorSpace::Lch,
        polar_color_space::OKLCH => PolarColorSpace::Oklch,
        _ => unreachable!("a computed polar color space holds an unknown keyword"),
    }
}

fn to_gfx_hue_interpolation_method(hue_interpolation_method: u8) -> HueInterpolationMethod {
    use crate::css::css_enums::hue_interpolation_method;
    match hue_interpolation_method {
        hue_interpolation_method::SHORTER => HueInterpolationMethod::Shorter,
        hue_interpolation_method::LONGER => HueInterpolationMethod::Longer,
        hue_interpolation_method::INCREASING => HueInterpolationMethod::Increasing,
        hue_interpolation_method::DECREASING => HueInterpolationMethod::Decreasing,
        _ => unreachable!("a computed hue interpolation method holds an unknown keyword"),
    }
}

fn gradient_interpolation_method(
    color_interpolation_method: &RetainedStyleValueData,
    color_syntax: u8,
) -> GradientInterpolationMethod {
    match color_interpolation_method.optional_data() {
        Some(StyleValueData::ColorInterpolationMethod {
            is_polar: true,
            color_space,
            hue_interpolation_method,
        }) => GradientInterpolationMethod {
            interpolation_type: GradientInterpolationType::Polar,
            polar_color_space: to_gfx_polar_color_space(*color_space),
            hue_interpolation_method: to_gfx_hue_interpolation_method(*hue_interpolation_method),
            ..GradientInterpolationMethod::default()
        },
        Some(StyleValueData::ColorInterpolationMethod {
            is_polar: false,
            color_space,
            ..
        }) => GradientInterpolationMethod {
            interpolation_type: GradientInterpolationType::Rectangular,
            rectangular_color_space: to_gfx_rectangular_color_space(*color_space),
            ..GradientInterpolationMethod::default()
        },
        Some(_) => unreachable!("a gradient's interpolation method is a color interpolation method value"),
        None => GradientInterpolationMethod {
            interpolation_type: GradientInterpolationType::Rectangular,
            rectangular_color_space: if color_syntax == 0 {
                RectangularColorSpace::Srgb
            } else {
                RectangularColorSpace::Oklab
            },
            ..GradientInterpolationMethod::default()
        },
    }
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum SideOrCorner {
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
}

impl SideOrCorner {
    fn from_computed_value(value: u8) -> Self {
        match value {
            value if value == Self::Top as u8 => Self::Top,
            value if value == Self::Bottom as u8 => Self::Bottom,
            value if value == Self::Left as u8 => Self::Left,
            value if value == Self::Right as u8 => Self::Right,
            value if value == Self::TopLeft as u8 => Self::TopLeft,
            value if value == Self::TopRight as u8 => Self::TopRight,
            value if value == Self::BottomLeft as u8 => Self::BottomLeft,
            value if value == Self::BottomRight as u8 => Self::BottomRight,
            _ => unreachable!("a computed side-or-corner holds an unknown value"),
        }
    }
}

fn linear_gradient_angle_degrees(
    direction_value: Option<&StyleValueData>,
    side_or_corner: u8,
    gradient_type: u8,
    gradient_size: CssPixelSize,
) -> f32 {
    const GRADIENT_TYPE_WEBKIT: u8 = 1;
    if let Some(direction) = direction_value {
        let angle = angle_to_degrees(direction);
        // Note: With -webkit-linear-gradient, 0deg points to the right instead of top,
        // and the direction is reversed (counter-clockwise instead of clockwise)
        if gradient_type == GRADIENT_TYPE_WEBKIT {
            (90.0 - angle) as f32
        } else {
            angle as f32
        }
    } else {
        let corner_angle_degrees =
            || gradient_size.height.to_double().atan2(gradient_size.width.to_double()) * 180.0 / std::f64::consts::PI;
        let angle = match SideOrCorner::from_computed_value(side_or_corner) {
            SideOrCorner::Top => 0.0,
            SideOrCorner::Bottom => 180.0,
            SideOrCorner::Left => 270.0,
            SideOrCorner::Right => 90.0,
            SideOrCorner::TopLeft => -corner_angle_degrees(),
            SideOrCorner::TopRight => corner_angle_degrees(),
            SideOrCorner::BottomLeft => corner_angle_degrees() + 180.0,
            SideOrCorner::BottomRight => -(corner_angle_degrees() + 180.0),
        };
        // Note: For unknowable reasons the angles are opposite on the -webkit- version
        if gradient_type == GRADIENT_TYPE_WEBKIT {
            (angle + 180.0) as f32
        } else {
            angle as f32
        }
    }
}

fn calculate_gradient_length(gradient_size: CssPixelSize, gradient_angle: f32) -> f32 {
    let real_angle = 90.0 - gradient_angle;
    let radians = real_angle * std::f32::consts::PI / 180.0;
    let sin_angle = (radians as f64).sin() as f32;
    let cos_angle = (radians as f64).cos() as f32;
    (gradient_size.height.to_float() * sin_angle).abs() + (gradient_size.width.to_float() * cos_angle).abs()
}

#[allow(clippy::too_many_arguments)]
fn resolve_linear_gradient_paint(
    color_stop_list: &[RetainedColorStop],
    direction_value: Option<&StyleValueData>,
    side_or_corner: u8,
    gradient_type: u8,
    repeating: bool,
    color_interpolation_method: &RetainedStyleValueData,
    color_syntax: u8,
    gradient_size: CssPixelSize,
    color_input: &ColorResolutionInput<'_>,
) -> LinearGradientData {
    let gradient_angle = linear_gradient_angle_degrees(direction_value, side_or_corner, gradient_type, gradient_size);
    let gradient_length_px = calculate_gradient_length(gradient_size, gradient_angle);

    let mut resolved_color_stops = resolve_color_stop_positions(
        color_stop_list,
        color_input,
        |position| (stop_length_px(position, gradient_length_px as f64) / gradient_length_px as f64) as f32,
        repeating,
    );

    if has_degenerate_repeat_length(&resolved_color_stops) {
        normalize_degenerate_repeating_gradient(&mut resolved_color_stops);
    }

    // Replace transition hints for painting; keep repeat_length for Skia's native tiling
    resolved_color_stops.list = replace_transition_hints_with_normal_color_stops(&resolved_color_stops.list);

    let repeat_length = resolved_color_stops.repeat_length.unwrap_or(1.0);
    let first_stop_position = if resolved_color_stops.repeat_length.is_some() {
        resolved_color_stops.list[0].position
    } else {
        0.0
    };
    let color_stops = to_color_stops(&resolved_color_stops.list, resolved_color_stops.repeating, |position| {
        (position - first_stop_position) / repeat_length
    });
    LinearGradientData {
        gradient_angle,
        color_stops,
        first_stop_position,
        repeat_length,
        interpolation_method: gradient_interpolation_method(color_interpolation_method, color_syntax),
    }
}

#[allow(clippy::too_many_arguments)]
fn resolve_conic_gradient_paint(
    color_stop_list: &[RetainedColorStop],
    from_angle: &RetainedStyleValueData,
    position: &RetainedStyleValueData,
    repeating: bool,
    color_interpolation_method: &RetainedStyleValueData,
    color_syntax: u8,
    gradient_box: CssPixelRect,
    color_input: &ColorResolutionInput<'_>,
) -> (ConicGradientData, CssPixelPoint) {
    let resolved_color_stops =
        resolve_color_stop_positions(color_stop_list, color_input, stop_angle_fraction_of_turn, repeating);
    let color_stops = finalized_color_stops(resolved_color_stops);
    let start_angle = match from_angle.optional_data() {
        Some(from_angle) => angle_to_degrees(from_angle) as f32,
        None => 0.0,
    };
    let data = ConicGradientData {
        start_angle,
        color_stops,
        interpolation_method: gradient_interpolation_method(color_interpolation_method, color_syntax),
    };
    (data, position_resolved(position.optional_data(), gradient_box))
}

#[allow(clippy::too_many_arguments)]
fn resolve_radial_gradient_paint(
    color_stop_list: &[RetainedColorStop],
    ending_shape: u8,
    size: &RetainedStyleValueData,
    position: &RetainedStyleValueData,
    repeating: bool,
    color_interpolation_method: &RetainedStyleValueData,
    color_syntax: u8,
    gradient_box: CssPixelRect,
    color_input: &ColorResolutionInput<'_>,
) -> (RadialGradientData, CssPixelPoint, CssPixelSize) {
    const ENDING_SHAPE_CIRCLE: u8 = 0;
    let center = position_resolved(position.optional_data(), gradient_box);
    let gradient_size = if ending_shape == ENDING_SHAPE_CIRCLE {
        let radius = resolve_circle_size(size.data(), center, gradient_box);
        CssPixelSize::new(radius, radius)
    } else {
        resolve_ellipse_size(size.data(), center, gradient_box)
    };

    // Start center, goes right to ending point, where the gradient line intersects the ending shape
    let resolved_color_stops = resolve_color_stop_positions(
        color_stop_list,
        color_input,
        |position| {
            (stop_length_px(position, gradient_size.width.to_double()) / gradient_size.width.to_float() as f64) as f32
        },
        repeating,
    );
    let color_stops = finalized_color_stops(resolved_color_stops);
    let data = RadialGradientData {
        color_stops,
        interpolation_method: gradient_interpolation_method(color_interpolation_method, color_syntax),
    };
    (data, center, gradient_size)
}

pub(crate) fn gradient_paint_value<'a>(
    image: &crate::painting::record::paint::background_resolution::LayerImageSource<'a>,
) -> Option<&'a StyleValueData> {
    let value = match image.value {
        StyleValueData::ImageSet { .. } => image.selected_image_value?,
        direct => direct,
    };
    matches!(
        value,
        StyleValueData::LinearGradient { .. }
            | StyleValueData::ConicGradient { .. }
            | StyleValueData::RadialGradient { .. }
    )
    .then_some(value)
}

pub(crate) fn resolve_gradient_paint(
    style: ComputedValuesView<'_>,
    gradient_value: &StyleValueData,
    tile_size: CssPixelSize,
) -> ResolvedGradientPaint {
    resolve_gradient_paint_with_input(gradient_value, tile_size, &stop_color_resolution_input(style))
}

pub(crate) fn resolve_gradient_paint_with_input(
    gradient_value: &StyleValueData,
    tile_size: CssPixelSize,
    color_input: &ColorResolutionInput<'_>,
) -> ResolvedGradientPaint {
    let gradient_box = CssPixelRect::from_location_and_size(CssPixelPoint::default(), tile_size);
    match gradient_value {
        StyleValueData::LinearGradient {
            has_direction_value,
            direction_value,
            side_or_corner,
            color_stop_list,
            gradient_type,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => ResolvedGradientPaint::Linear(resolve_linear_gradient_paint(
            color_stop_list.as_slice(),
            has_direction_value.then(|| direction_value.data()),
            *side_or_corner,
            *gradient_type,
            *repeating,
            color_interpolation_method,
            *color_syntax,
            tile_size,
            color_input,
        )),
        StyleValueData::ConicGradient {
            from_angle,
            position,
            color_stop_list,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => {
            let (data, resolved_position) = resolve_conic_gradient_paint(
                color_stop_list.as_slice(),
                from_angle,
                position,
                *repeating,
                color_interpolation_method,
                *color_syntax,
                gradient_box,
                color_input,
            );
            ResolvedGradientPaint::Conic {
                data,
                position: resolved_position,
            }
        }
        StyleValueData::RadialGradient {
            ending_shape,
            size,
            position,
            color_stop_list,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => {
            let (data, center, gradient_size) = resolve_radial_gradient_paint(
                color_stop_list.as_slice(),
                *ending_shape,
                size,
                position,
                *repeating,
                color_interpolation_method,
                *color_syntax,
                gradient_box,
                color_input,
            );
            ResolvedGradientPaint::Radial {
                data,
                center,
                size: gradient_size,
            }
        }
        _ => unreachable!("resolve_gradient_paint takes a gradient style value"),
    }
}

pub(crate) fn record_gradient_fill(
    recorder: &mut PaintRecorder<'_>,
    paint: &ResolvedGradientPaint,
    dest_rect: FloatRect,
) {
    let converter = recorder.converter;
    let dest_int_rect = IntRect::new(
        dest_rect.x as i32,
        dest_rect.y as i32,
        dest_rect.width as i32,
        dest_rect.height as i32,
    );
    match paint {
        ResolvedGradientPaint::Linear(data) => {
            recorder.recorder.fill_rect_with_linear_gradient(dest_int_rect, data);
        }
        ResolvedGradientPaint::Radial { data, center, size } => {
            let center = converter.rounded_device_point(*center);
            let size = converter.rounded_device_size(*size);
            recorder
                .recorder
                .fill_rect_with_radial_gradient(dest_int_rect, data, center, size);
        }
        ResolvedGradientPaint::Conic { data, position } => {
            let position = converter.rounded_device_point(*position);
            recorder
                .recorder
                .fill_rect_with_conic_gradient(dest_int_rect, data, position);
        }
    }
}
