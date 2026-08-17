/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Native absolutization of style values.
//!
//! Mirrors the C++ `StyleValue::absolutized(ComputationContext const&)` dispatch: structural
//! values recurse into their children and rebuild only when a child changed, dimensions convert
//! to their canonical units, and lengths resolve through the shared length-resolution context.
//! Types whose absolutization has not been ported (or that need C++-only state not captured in the
//! computation environment) decline, and the caller routes the whole value through the C++ fallback. Preserving
//! identity for unchanged values is a real contract: the store batch reuses wrappers and shared
//! allocations based on pointer equality.

// Style value data allocations are thread-confined, like style_value.rs.
#![allow(clippy::arc_with_non_send_sync)]

use std::cell::Cell;
use std::sync::Arc;

use crate::css::color_resolution::{
    ColorResolutionInput, EMPTY_INPUT, PREFERRED_COLOR_SCHEME_DARK, RECTANGULAR_COLOR_SPACE_OKLAB,
    descriptor_for_color_type, normalize_percentage_pair, percentage_from_style_value, resolve_alpha,
    resolve_color_for_interpolation, resolve_hue, resolve_relative_form, resolve_with_reference_value, to_color,
};
use crate::css::css_enums::keyword;
use crate::css::style_compute::{FfiLengthResolutionContext, absolutize_length, keyword_is_color};
use crate::css::style_value::{
    ColorBase, RetainedStyleValueData, RetainedStyleValueDataList, RetainedUtf16FlyString, StyleValueData,
};

pub(crate) struct AbsolutizationContext<'a> {
    pub(crate) length: &'a FfiLengthResolutionContext,
    /// The used color scheme, present exactly when the C++ computation context carries one:
    /// only the generic context kind sets it. PreferredColorScheme codes; Dark is 1.
    pub(crate) scheme: Option<u8>,
    /// Set when a viewport-relative length resolved, mirroring the C++ tracking flag.
    pub(crate) resolved_viewport_relative_length: Cell<bool>,
    /// Immutable sibling facts supplied only when a winning cascaded value uses a
    /// tree-counting function.
    pub(crate) tree_counting: Option<(u64, u64)>,
    /// Random base values captured before entering the longhand drive.
    pub(crate) random_base_values: &'a [crate::css::style_compute::FfiRandomBaseValue],
    /// The document base URL captured before entering the longhand drive.
    pub(crate) document_base_url: &'a [u8],
    /// The stylesheet resource context for the selected cascade source, if any.
    pub(crate) style_sheet_resource_context: Option<StyleSheetResourceContext<'a>>,
}

#[derive(Clone, Copy)]
pub(crate) struct StyleSheetResourceContext<'a> {
    pub(crate) base_url: &'a [u8],
    pub(crate) origin_clean: bool,
}

/// The outcome for one value: unchanged (preserve identity) or a new allocation.
pub(crate) enum Absolutized {
    Unchanged,
    Changed(RetainedStyleValueData),
}

fn retain_new(value: StyleValueData) -> RetainedStyleValueData {
    let pointer = Arc::into_raw(Arc::new(value));
    // SAFETY: The freshly leaked Arc reference is exactly one strong reference.
    unsafe { RetainedStyleValueData::from_retained_pointer(pointer) }
}

/// Absolutizes one retained child slot. Returns the child for the rebuilt parent, whether any
/// change happened, or None when the child's absolutization declined. Null children stay null.
fn absolutize_child(
    child: &RetainedStyleValueData,
    context: &AbsolutizationContext,
    any_changed: &mut bool,
) -> Option<RetainedStyleValueData> {
    let Some(data) = child.optional_data() else {
        return Some(child.clone_retained());
    };
    match absolutize(data, context)? {
        Absolutized::Unchanged => Some(child.clone_retained()),
        Absolutized::Changed(new_child) => {
            *any_changed = true;
            Some(new_child)
        }
    }
}

fn absolutize_list(
    values: &RetainedStyleValueDataList,
    context: &AbsolutizationContext,
    any_changed: &mut bool,
) -> Option<Vec<RetainedStyleValueData>> {
    values
        .as_slice()
        .iter()
        .map(|child| absolutize_child(child, context, any_changed))
        .collect()
}

fn zero_px_length() -> StyleValueData {
    StyleValueData::Length {
        value: 0.0,
        unit: crate::css::style_compute::px_length_unit(),
    }
}

fn currentcolor_keyword() -> StyleValueData {
    StyleValueData::Keyword {
        keyword: keyword::CURRENTCOLOR,
    }
}

// ColorStyleValue.h: ColorSyntax::Legacy = 0, Modern = 1.
const COLOR_SYNTAX_LEGACY: u8 = 0;
const COLOR_SYNTAX_MODERN: u8 = 1;

fn retained_null() -> RetainedStyleValueData {
    // SAFETY: null encodes an absent optional value.
    unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) }
}

fn absolutize_image(value: &StyleValueData, context: &AbsolutizationContext) -> Option<Absolutized> {
    let StyleValueData::Image {
        url,
        url_type,
        url_modifiers,
        resource_context,
    } = value
    else {
        return None;
    };
    if url.as_bytes().is_empty() {
        return Some(Absolutized::Unchanged);
    }

    let base_url = context
        .style_sheet_resource_context
        .map(|resource_context| resource_context.base_url)
        .filter(|base_url| !base_url.is_empty())
        .or_else(|| {
            resource_context
                .has_base_url
                .then(|| resource_context.base_url.as_bytes())
        })
        .unwrap_or(context.document_base_url);
    if base_url.is_empty() {
        return Some(Absolutized::Unchanged);
    }

    let url_string = std::str::from_utf8(url.as_bytes()).ok()?;
    let should_absolutize_url_for_computed_value =
        context.style_sheet_resource_context.is_some() || resource_context.should_absolutize_url_for_computed_value;
    let absolutized_url = if should_absolutize_url_for_computed_value {
        if liburl_rust::basic_parse(url_string, liburl_rust::BasicParseOptions::new()).is_some() {
            crate::css::style_value::RetainedString::from_utf8(url_string.to_owned())
        } else {
            let base_url = std::str::from_utf8(base_url).ok()?;
            let base_url = liburl_rust::basic_parse(base_url, liburl_rust::BasicParseOptions::new())?;
            let resolved_url =
                liburl_rust::basic_parse(url_string, liburl_rust::BasicParseOptions::new().base_url(&base_url));
            let Some(resolved_url) = resolved_url else {
                return Some(Absolutized::Unchanged);
            };
            crate::css::style_value::RetainedString::from_utf8(resolved_url.serialization())
        }
    } else {
        crate::css::style_value::RetainedString::from_utf8(url_string.to_owned())
    };
    let base_url = std::str::from_utf8(base_url).ok()?;
    let base_url = crate::css::style_value::RetainedString::from_utf8(base_url.to_owned());

    let (has_parent_style_sheet_origin_clean, parent_style_sheet_origin_clean) = context
        .style_sheet_resource_context
        .map(|context| (true, context.origin_clean))
        .unwrap_or((
            resource_context.has_parent_style_sheet_origin_clean,
            resource_context.parent_style_sheet_origin_clean,
        ));

    Some(Absolutized::Changed(retain_new(StyleValueData::Image {
        url: absolutized_url,
        url_type: *url_type,
        url_modifiers: url_modifiers.clone(),
        resource_context: crate::css::style_value::ImageResourceContext {
            base_url,
            has_base_url: true,
            has_parent_style_sheet_origin_clean,
            parent_style_sheet_origin_clean,
            should_absolutize_url_for_computed_value,
        },
    })))
}

/// The color-resolution input the C++ absolutizers build from a computation context: the
/// context's color scheme and calculation inputs, no currentcolor and no channels.
fn color_resolution_input<'a>(context: &'a AbsolutizationContext) -> ColorResolutionInput<'a> {
    ColorResolutionInput {
        scheme: context.scheme,
        current_color: None,
        current_color_value: None,
        length: Some(context.length),
        channels: None,
    }
}

/// The rgb() function form ColorFunctionStyleValue::create builds for an already-resolved
/// color: number channels, a number alpha, no name and no origin.
fn rgb_color_function(r: f64, g: f64, b: f64, alpha: f64, color_syntax: u8) -> StyleValueData {
    StyleValueData::ColorFunction {
        color_base: ColorBase {
            has_color_type: true,
            color_type: crate::css::color_conversion::RGB,
            color_syntax,
        },
        channel_0: retain_new(StyleValueData::Number { value: r }),
        channel_1: retain_new(StyleValueData::Number { value: g }),
        channel_2: retain_new(StyleValueData::Number { value: b }),
        alpha: retain_new(StyleValueData::Number { value: alpha }),
        has_name: false,
        name: RetainedUtf16FlyString::none(),
        origin_color: retained_null(),
    }
}

/// The canonical computed form of a fully-resolved legacy sRGB color.
fn canonical_legacy_rgb(value: &StyleValueData) -> Option<StyleValueData> {
    let rgba = crate::css::color_resolution::to_color(value, &EMPTY_INPUT)?;
    Some(rgb_color_function(
        f64::from(rgba.r),
        f64::from(rgba.g),
        f64::from(rgba.b),
        f64::from(rgba.a) / 255.0,
        COLOR_SYNTAX_LEGACY,
    ))
}

/// Port of hsl_to_absolutized_rgb() in ColorFunctionStyleValue.cpp.
// https://drafts.csswg.org/css-color-4/#hsl-to-rgb
fn hsl_to_absolutized_rgb(
    hue_degrees: f64,
    saturation_0_100: f64,
    lightness_0_100: f64,
    alpha_0_1: f64,
) -> StyleValueData {
    let mut hue = hue_degrees % 360.0;
    if hue < 0.0 {
        hue += 360.0;
    }
    let saturation = (saturation_0_100 / 100.0).clamp(0.0, 1.0);
    let lightness = (lightness_0_100 / 100.0).clamp(0.0, 1.0);

    let to_rgb = |offset: f64| {
        let k = (offset + hue / 30.0) % 12.0;
        let a = saturation * lightness.min(1.0 - lightness);
        lightness - a * (-1.0f64).max((k - 3.0).min(9.0 - k).min(1.0))
    };

    let r = to_rgb(0.0);
    let g = to_rgb(8.0);
    let b = to_rgb(4.0);

    rgb_color_function(
        (r * 255.0).clamp(0.0, 255.0),
        (g * 255.0).clamp(0.0, 255.0),
        (b * 255.0).clamp(0.0, 255.0),
        alpha_0_1.clamp(0.0, 1.0),
        COLOR_SYNTAX_LEGACY,
    )
}

/// Port of hwb_to_absolutized_rgb() in ColorFunctionStyleValue.cpp, including its
/// single-precision arithmetic.
// https://drafts.csswg.org/css-color-4/#hwb-to-rgb
fn hwb_to_absolutized_rgb(
    hue_degrees: f64,
    whiteness_0_100: f64,
    blackness_0_100: f64,
    alpha_0_1: f64,
) -> StyleValueData {
    let whiteness = (whiteness_0_100 / 100.0).clamp(0.0, 1.0) as f32;
    let blackness = (blackness_0_100 / 100.0).clamp(0.0, 1.0) as f32;

    if whiteness + blackness >= 1.0 {
        // The gray channel value is shared across the three slots, like the C++ code.
        let gray = retain_new(StyleValueData::Number {
            value: f64::from((whiteness / (whiteness + blackness) * 255.0).clamp(0.0, 255.0)),
        });
        return StyleValueData::ColorFunction {
            color_base: ColorBase {
                has_color_type: true,
                color_type: crate::css::color_conversion::RGB,
                color_syntax: COLOR_SYNTAX_LEGACY,
            },
            channel_0: gray.clone_retained(),
            channel_1: gray.clone_retained(),
            channel_2: gray,
            alpha: retain_new(StyleValueData::Number {
                value: alpha_0_1.clamp(0.0, 1.0),
            }),
            has_name: false,
            name: RetainedUtf16FlyString::none(),
            origin_color: retained_null(),
        };
    }

    let mut hue = (hue_degrees as f32) % 360.0;
    if hue < 0.0 {
        hue += 360.0;
    }

    let hue_to_rgb = |offset: f32| {
        let k = (offset + hue / 30.0) % 12.0;
        0.5 - 0.5 * (-1.0f32).max((k - 3.0).min(9.0 - k).min(1.0))
    };

    let scale = 1.0 - whiteness - blackness;
    let r = hue_to_rgb(0.0) * scale + whiteness;
    let g = hue_to_rgb(8.0) * scale + whiteness;
    let b = hue_to_rgb(4.0) * scale + whiteness;

    rgb_color_function(
        f64::from((r * 255.0).clamp(0.0, 255.0)),
        f64::from((g * 255.0).clamp(0.0, 255.0)),
        f64::from((b * 255.0).clamp(0.0, 255.0)),
        alpha_0_1.clamp(0.0, 1.0),
        COLOR_SYNTAX_LEGACY,
    )
}

/// Port of ColorFunctionStyleValue::absolutized: relative forms recurse their channels, alpha
/// and origin; HSL and HWB resolve and convert to a legacy rgb() form; every other color
/// function recurses its channels and alpha.
fn absolutize_color_function(value: &StyleValueData, context: &AbsolutizationContext) -> Option<Absolutized> {
    let StyleValueData::ColorFunction {
        color_base,
        channel_0,
        channel_1,
        channel_2,
        alpha,
        has_name,
        name,
        origin_color,
    } = value
    else {
        return None;
    };
    // The C++ absolutization reads the descriptor through the unconditional color type.
    if !color_base.has_color_type {
        return None;
    }
    let mut changed = false;
    let absolutized_c1 = absolutize_child(channel_0, context, &mut changed)?;
    let absolutized_c2 = absolutize_child(channel_1, context, &mut changed)?;
    let absolutized_c3 = absolutize_child(channel_2, context, &mut changed)?;
    let absolutized_alpha = absolutize_child(alpha, context, &mut changed)?;

    // https://drafts.csswg.org/css-color-5/#relative-color
    if origin_color.optional_data().is_some() {
        let absolutized_origin = absolutize_child(origin_color, context, &mut changed)?;
        if !changed {
            return Some(Absolutized::Unchanged);
        }
        return Some(Absolutized::Changed(retain_new(StyleValueData::ColorFunction {
            color_base: *color_base,
            channel_0: absolutized_c1,
            channel_1: absolutized_c2,
            channel_2: absolutized_c3,
            alpha: absolutized_alpha,
            has_name: *has_name,
            name: name.clone(),
            origin_color: absolutized_origin,
        })));
    }

    let descriptor = descriptor_for_color_type(color_base.color_type);
    if descriptor.absolutizes_to_rgb {
        // https://drafts.csswg.org/css-color-4/#resolving-sRGB-values
        let c1 = if descriptor.channels[0].is_hue {
            resolve_hue(absolutized_c1.data(), &EMPTY_INPUT)
        } else {
            resolve_with_reference_value(
                absolutized_c1.data(),
                f64::from(descriptor.channels[0].percent_reference),
                &EMPTY_INPUT,
            )
        }?;
        let c2 = resolve_with_reference_value(
            absolutized_c2.data(),
            f64::from(descriptor.channels[1].percent_reference),
            &EMPTY_INPUT,
        )?;
        let c3 = resolve_with_reference_value(
            absolutized_c3.data(),
            f64::from(descriptor.channels[2].percent_reference),
            &EMPTY_INPUT,
        )?;
        let alpha = match absolutized_alpha.optional_data() {
            Some(alpha) => resolve_alpha(alpha, &EMPTY_INPUT)?,
            None => 1.0,
        };
        let converted = if color_base.color_type == crate::css::color_conversion::HSL {
            hsl_to_absolutized_rgb(c1, c2, c3, alpha)
        } else {
            hwb_to_absolutized_rgb(c1, c2, c3, alpha)
        };
        let canonical = canonical_legacy_rgb(&converted).unwrap_or(converted);
        return Some(Absolutized::Changed(retain_new(canonical)));
    }

    let rebuilt = StyleValueData::ColorFunction {
        color_base: *color_base,
        channel_0: absolutized_c1,
        channel_1: absolutized_c2,
        channel_2: absolutized_c3,
        alpha: absolutized_alpha,
        has_name: *has_name,
        name: name.clone(),
        origin_color: retained_null(),
    };

    if color_base.color_type == crate::css::color_conversion::RGB
        && let Some(canonical) = canonical_legacy_rgb(&rebuilt)
    {
        if canonical == *value {
            return Some(Absolutized::Unchanged);
        }
        return Some(Absolutized::Changed(retain_new(canonical)));
    }

    if !changed {
        return Some(Absolutized::Unchanged);
    }
    Some(Absolutized::Changed(retain_new(rebuilt)))
}

/// Port of ColorMixStyleValue::absolutized: normalizes the mix percentages, resolves relative
/// color forms, and interpolates to a concrete color; when interpolation cannot complete the
/// color-mix rebuilds around its absolutized parts instead.
fn absolutize_color_mix(value: &StyleValueData, context: &AbsolutizationContext) -> Option<Absolutized> {
    let StyleValueData::ColorMix {
        color_interpolation_method,
        first_color,
        first_percentage,
        second_color,
        second_percentage,
        ..
    } = value
    else {
        return None;
    };

    // Port of normalize_percentages(): each present percentage absolutizes, then the pair
    // normalizes per https://drafts.csswg.org/css-color-5/#color-mix-percent-norm.
    let absolutized_percentage = |slot: &RetainedStyleValueData| -> Option<Option<f64>> {
        if slot.optional_data().is_none() {
            return Some(None);
        }
        let mut changed = false;
        let absolutized = absolutize_child(slot, context, &mut changed)?;
        Some(Some(percentage_from_style_value(absolutized.data())?))
    };
    let p1 = absolutized_percentage(first_percentage)?;
    let p2 = absolutized_percentage(second_percentage)?;
    let normalized = normalize_percentage_pair(p1, p2);

    let input = color_resolution_input(context);

    let mut method_changed = false;
    let absolutized_method = absolutize_child(color_interpolation_method, context, &mut method_changed)?;

    let delta = normalized.second_percentage / 100.0;

    // An absent method interpolates in the rectangular Oklab space.
    let default_method = StyleValueData::ColorInterpolationMethod {
        is_polar: false,
        color_space: RECTANGULAR_COLOR_SPACE_OKLAB,
        hue_interpolation_method: 0,
    };
    let method = absolutized_method.optional_data().unwrap_or(&default_method);

    // Resolve relative-color components before interpolation so channel-keyword references
    // and `none` missing-channel markers are made visible during interpolation.
    let resolve_if_relative = |color: &StyleValueData| -> Option<StyleValueData> {
        if let StyleValueData::ColorFunction { origin_color, .. } = color
            && origin_color.optional_data().is_some()
        {
            return resolve_relative_form(color, &input);
        }
        None
    };
    let resolved_first_storage = resolve_if_relative(first_color.data());
    let resolved_second_storage = resolve_if_relative(second_color.data());
    let resolved_first = resolved_first_storage.as_ref().unwrap_or(first_color.data());
    let resolved_second = resolved_second_storage.as_ref().unwrap_or(second_color.data());

    let interpolated = (|| -> Option<RetainedStyleValueData> {
        let resolved_from = resolve_color_for_interpolation(resolved_first, &input)?;
        let resolved_to = resolve_color_for_interpolation(resolved_second, &input)?;
        // SAFETY: All pointers stay live for the duration of the call; the non-null result
        // owns exactly one strong reference.
        let result = unsafe {
            crate::css::color_interpolation::rust_interpolate_color(
                &raw const resolved_from,
                &raw const resolved_to,
                std::ptr::from_ref(method),
                delta as f32,
                normalized.alpha_multiplier as f32,
            )
        };
        if result.is_null() {
            return None;
        }
        // SAFETY: The returned pointer owns exactly one strong reference.
        Some(unsafe { RetainedStyleValueData::from_retained_pointer(result) })
    })();
    if let Some(result) = interpolated {
        return Some(Absolutized::Changed(result));
    }

    // Fall back to a color-mix() over absolutized values when interpolation cannot complete;
    // currently that means a component relies on currentcolor.
    let mut colors_changed = false;
    let absolutized_first = absolutize_child(first_color, context, &mut colors_changed)?;
    let absolutized_second = absolutize_child(second_color, context, &mut colors_changed)?;
    let percentage_matches = |slot: &RetainedStyleValueData, normalized_value: f64| matches!(slot.optional_data(), Some(StyleValueData::Percentage { value }) if *value == normalized_value);
    if !colors_changed
        && percentage_matches(first_percentage, normalized.first_percentage)
        && percentage_matches(second_percentage, normalized.second_percentage)
    {
        return Some(Absolutized::Unchanged);
    }
    Some(Absolutized::Changed(retain_new(StyleValueData::ColorMix {
        color_base: ColorBase {
            has_color_type: false,
            color_type: 0,
            color_syntax: COLOR_SYNTAX_MODERN,
        },
        color_interpolation_method: absolutized_method,
        first_color: absolutized_first,
        first_percentage: retain_new(StyleValueData::Percentage {
            value: normalized.first_percentage,
        }),
        second_color: absolutized_second,
        second_percentage: retain_new(StyleValueData::Percentage {
            value: normalized.second_percentage,
        }),
    })))
}

/// The gradient absolutizers recurse the same children the C++ ports do, but the retained
/// color-stop type has no Rust-side constructor yet, so a changed gradient still declines to
/// the C++ fallback; only the identity outcome resolves natively. The C++ linear gradient
/// absolutization leaves the direction untouched, so this does too.
fn absolutize_gradient(value: &StyleValueData, context: &AbsolutizationContext) -> Option<Absolutized> {
    let mut changed = false;
    let absolutize_stops = |color_stop_list: &crate::css::style_value::RetainedColorStopList,
                            changed: &mut bool|
     -> Option<crate::css::style_value::RetainedColorStopList> {
        let mut stops = Vec::with_capacity(color_stop_list.as_slice().len());
        for stop in color_stop_list.as_slice() {
            let [transition_hint, color, position, second_position] = stop.values();
            let transition_hint = absolutize_child(transition_hint, context, changed)?;
            let color = absolutize_child(color, context, changed)?;
            let position = absolutize_child(position, context, changed)?;
            let second_position = absolutize_child(second_position, context, changed)?;
            stops.push(crate::css::style_value::RetainedColorStop::from_retained_values(
                transition_hint,
                color,
                position,
                second_position,
            ));
        }
        Some(crate::css::style_value::RetainedColorStopList::from_retained_elements(
            stops,
        ))
    };
    match value {
        StyleValueData::LinearGradient {
            has_direction_value,
            direction_value,
            side_or_corner,
            color_stop_list,
            gradient_type,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => {
            // The C++ recursion deliberately leaves the direction untouched.
            let color_stop_list = absolutize_stops(color_stop_list, &mut changed)?;
            let color_interpolation_method = absolutize_child(color_interpolation_method, context, &mut changed)?;
            if changed {
                Some(Absolutized::Changed(retain_new(StyleValueData::LinearGradient {
                    has_direction_value: *has_direction_value,
                    direction_value: direction_value.clone_retained(),
                    side_or_corner: *side_or_corner,
                    color_stop_list,
                    gradient_type: *gradient_type,
                    repeating: *repeating,
                    color_interpolation_method,
                    color_syntax: *color_syntax,
                })))
            } else {
                Some(Absolutized::Unchanged)
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
            let size = absolutize_child(size, context, &mut changed)?;
            let position = absolutize_child(position, context, &mut changed)?;
            let color_stop_list = absolutize_stops(color_stop_list, &mut changed)?;
            let color_interpolation_method = absolutize_child(color_interpolation_method, context, &mut changed)?;
            if changed {
                Some(Absolutized::Changed(retain_new(StyleValueData::RadialGradient {
                    ending_shape: *ending_shape,
                    size,
                    position,
                    color_stop_list,
                    repeating: *repeating,
                    color_interpolation_method,
                    color_syntax: *color_syntax,
                })))
            } else {
                Some(Absolutized::Unchanged)
            }
        }
        StyleValueData::ConicGradient {
            from_angle,
            position,
            color_stop_list,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => {
            let from_angle = absolutize_child(from_angle, context, &mut changed)?;
            let position = absolutize_child(position, context, &mut changed)?;
            let color_stop_list = absolutize_stops(color_stop_list, &mut changed)?;
            let color_interpolation_method = absolutize_child(color_interpolation_method, context, &mut changed)?;
            if changed {
                Some(Absolutized::Changed(retain_new(StyleValueData::ConicGradient {
                    from_angle,
                    position,
                    color_stop_list,
                    repeating: *repeating,
                    color_interpolation_method,
                    color_syntax: *color_syntax,
                })))
            } else {
                Some(Absolutized::Unchanged)
            }
        }
        _ => None,
    }
}

/// Absolutizes a child slot whose C++ accessor substitutes a default when absent; the default
/// materializes into the rebuilt value, which always counts as a change.
fn absolutize_defaulted_child(
    child: &RetainedStyleValueData,
    default: fn() -> StyleValueData,
    context: &AbsolutizationContext,
    any_changed: &mut bool,
) -> Option<RetainedStyleValueData> {
    if child.optional_data().is_none() {
        *any_changed = true;
        let materialized = retain_new(default());
        let data = materialized.data();
        return match absolutize(data, context)? {
            Absolutized::Unchanged => Some(materialized.clone_retained()),
            Absolutized::Changed(absolutized) => Some(absolutized),
        };
    }
    absolutize_child(child, context, any_changed)
}

/// Port of EdgeStyleValue::absolutized via with_resolved_keywords: center becomes 50%,
/// right/bottom flip to 100% minus the offset, and remaining keywords drop, leaving an
/// offset-only edge whose offset then absolutizes.
fn absolutize_edge(value: &StyleValueData, context: &AbsolutizationContext) -> Option<Absolutized> {
    use crate::css::css_enums::position_edge;
    let StyleValueData::Edge { has_edge, edge, offset } = value else {
        return None;
    };
    let resolved_offset: Option<StyleValueData> = if *has_edge && *edge == position_edge::CENTER {
        Some(StyleValueData::Percentage { value: 50.0 })
    } else if *has_edge && (*edge == position_edge::RIGHT || *edge == position_edge::BOTTOM) {
        match offset.optional_data() {
            None => Some(StyleValueData::Percentage { value: 100.0 }),
            Some(offset) => Some(crate::css::calc::flipped_edge_offset(offset)?),
        }
    } else if offset.optional_data().is_none() {
        Some(StyleValueData::Percentage { value: 0.0 })
    } else {
        None
    };
    let mut changed = *has_edge || resolved_offset.is_some();
    let offset = match resolved_offset {
        Some(resolved) => {
            let retained = retain_new(resolved);
            let mut resolved_changed = false;
            absolutize_child(&retained, context, &mut resolved_changed)?
        }
        None => absolutize_child(offset, context, &mut changed)?,
    };
    if !changed {
        return Some(Absolutized::Unchanged);
    }
    Some(Absolutized::Changed(retain_new(StyleValueData::Edge {
        has_edge: false,
        edge: 0,
        offset,
    })))
}

/// Port of number_from_style_value with a percentage basis of one and an empty calc context.
fn number_from_value(value: &StyleValueData, percentage_basis: f64) -> Option<f64> {
    match value {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Percentage { value } => Some(value * 0.01 * percentage_basis),
        StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_number_without_context(value)
            .or_else(|| {
                crate::css::calc::resolve_calculated_percentage_without_context(value)
                    .map(|percentage| percentage * 0.01 * percentage_basis)
            }),
        _ => None,
    }
}

/// Port of BasicShapeStyleValue::absolutized: structural recursion, with xywh() and rect()
/// lowering to the equivalent inset() through 100%-minus calculations, and rect()'s auto
/// edges resolving to 0% or 100%.
fn absolutize_basic_shape(value: &StyleValueData, context: &AbsolutizationContext) -> Option<Absolutized> {
    let StyleValueData::BasicShape {
        kind,
        v0,
        v1,
        v2,
        v3,
        v4,
        fill_rule,
        points,
        path_string,
    } = value
    else {
        return None;
    };
    // Absolutizes a freshly built 100%-minus calculation for the inset lowering.
    let flipped = |values: &[&StyleValueData], changed: &mut bool| -> Option<RetainedStyleValueData> {
        *changed = true;
        let built = retain_new(crate::css::calc::one_hundred_percent_minus_value(values)?);
        let mut built_changed = false;
        absolutize_child(&built, context, &mut built_changed)
    };
    // rect()'s auto edges coincide with the reference box edge: 0% for top/left,
    // 100% for right/bottom.
    let resolve_auto =
        |side: &RetainedStyleValueData, value_of_auto: f64, changed: &mut bool| -> Option<RetainedStyleValueData> {
            match side.optional_data()? {
                StyleValueData::Keyword { .. } => {
                    *changed = true;
                    Some(retain_new(StyleValueData::Percentage { value: value_of_auto }))
                }
                _ => Some(side.clone_retained()),
            }
        };
    match kind {
        0 => {
            let mut changed = false;
            let top = absolutize_child(v0, context, &mut changed)?;
            let right = absolutize_child(v1, context, &mut changed)?;
            let bottom = absolutize_child(v2, context, &mut changed)?;
            let left = absolutize_child(v3, context, &mut changed)?;
            let border_radius = absolutize_child(v4, context, &mut changed)?;
            if !changed {
                return Some(Absolutized::Unchanged);
            }
            Some(Absolutized::Changed(retain_new(StyleValueData::BasicShape {
                kind: 0,
                v0: top,
                v1: right,
                v2: bottom,
                v3: left,
                v4: border_radius,
                fill_rule: *fill_rule,
                points: points.clone(),
                path_string: path_string.clone(),
            })))
        }
        1 | 2 => {
            // xywh(x y w h) is inset(y calc(100% - x - w) calc(100% - y - h) x);
            // rect(t r b l) is inset(t calc(100% - r) calc(100% - b) l).
            let mut changed = true;
            let (top, right, bottom, left) = if *kind == 1 {
                let x = v0.optional_data()?;
                let y = v1.optional_data()?;
                let width = v2.optional_data()?;
                let height = v3.optional_data()?;
                (
                    absolutize_child(v1, context, &mut changed)?,
                    flipped(&[x, width], &mut changed)?,
                    flipped(&[y, height], &mut changed)?,
                    absolutize_child(v0, context, &mut changed)?,
                )
            } else {
                let mut auto_changed = false;
                let top = resolve_auto(v0, 0.0, &mut auto_changed)?;
                let right = resolve_auto(v1, 100.0, &mut auto_changed)?;
                let bottom = resolve_auto(v2, 100.0, &mut auto_changed)?;
                let left = resolve_auto(v3, 0.0, &mut auto_changed)?;
                let mut child_changed = false;
                (
                    absolutize_child(&top, context, &mut child_changed)?,
                    flipped(&[right.optional_data()?], &mut child_changed)?,
                    flipped(&[bottom.optional_data()?], &mut child_changed)?,
                    absolutize_child(&left, context, &mut child_changed)?,
                )
            };
            let mut radius_changed = false;
            let border_radius = absolutize_child(v4, context, &mut radius_changed)?;
            let _ = changed;
            Some(Absolutized::Changed(retain_new(StyleValueData::BasicShape {
                kind: 0,
                v0: top,
                v1: right,
                v2: bottom,
                v3: left,
                v4: border_radius,
                fill_rule: *fill_rule,
                points: points.clone(),
                path_string: path_string.clone(),
            })))
        }
        3 | 4 => {
            let mut changed = false;
            let radius = absolutize_child(v0, context, &mut changed)?;
            let position = absolutize_child(v1, context, &mut changed)?;
            if !changed {
                return Some(Absolutized::Unchanged);
            }
            Some(Absolutized::Changed(retain_new(StyleValueData::BasicShape {
                kind: *kind,
                v0: radius,
                v1: position,
                v2: v2.clone_retained(),
                v3: v3.clone_retained(),
                v4: v4.clone_retained(),
                fill_rule: *fill_rule,
                points: points.clone(),
                path_string: path_string.clone(),
            })))
        }
        5 => {
            let mut changed = false;
            let mut absolutized_points = Vec::with_capacity(points.as_slice().len());
            for point in points.as_slice() {
                let [x, y] = point.values();
                let x = absolutize_child(x, context, &mut changed)?;
                let y = absolutize_child(y, context, &mut changed)?;
                absolutized_points.push(crate::css::style_value::RetainedShapePoint::from_retained_values(x, y));
            }
            if !changed {
                return Some(Absolutized::Unchanged);
            }
            Some(Absolutized::Changed(retain_new(StyleValueData::BasicShape {
                kind: 5,
                v0: v0.clone_retained(),
                v1: v1.clone_retained(),
                v2: v2.clone_retained(),
                v3: v3.clone_retained(),
                v4: v4.clone_retained(),
                fill_rule: *fill_rule,
                points: crate::css::style_value::RetainedShapePointList::from_retained_points(absolutized_points),
                path_string: path_string.clone(),
            })))
        }
        _ => Some(Absolutized::Unchanged),
    }
}

/// Absolutizes a grid track entry list, recursing through repeat() entries.
fn absolutize_grid_track_entries(
    entries: &[crate::css::style_value::RetainedGridTrackEntry],
    context: &AbsolutizationContext,
    any_changed: &mut bool,
) -> Option<Vec<crate::css::style_value::RetainedGridTrackEntry>> {
    use crate::css::style_value::RetainedGridTrackEntry;
    let mut absolutized = Vec::with_capacity(entries.len());
    for entry in entries {
        let size_value = absolutize_child(&entry.size_value, context, any_changed)?;
        let min_value = absolutize_child(&entry.min_value, context, any_changed)?;
        let max_value = absolutize_child(&entry.max_value, context, any_changed)?;
        let repeat_count = absolutize_child(&entry.repeat_count, context, any_changed)?;
        let nested = absolutize_grid_track_entries(entry.repeat_entries(), context, any_changed)?;
        let nested = nested.into_boxed_slice();
        let repeat_entries_length = nested.len();
        let repeat_entries_pointer = if repeat_entries_length == 0 {
            std::ptr::null_mut()
        } else {
            Box::into_raw(nested) as *mut RetainedGridTrackEntry
        };
        absolutized.push(RetainedGridTrackEntry {
            kind: entry.kind,
            names: entry.names.clone(),
            size_value,
            min_value,
            max_value,
            repeat_type: entry.repeat_type,
            repeat_count,
            repeat_is_subgrid: entry.repeat_is_subgrid,
            repeat_preserve_line_name_sets: entry.repeat_preserve_line_name_sets,
            repeat_entries_pointer,
            repeat_entries_length,
        });
    }
    Some(absolutized)
}

fn canonicalized_dimension(value: f64, unit: u8, ratios: &[f64]) -> Option<(f64, u8)> {
    let canonical = ratios.iter().position(|&ratio| ratio == 1.0)? as u8;
    if unit == canonical {
        return None;
    }
    Some((value * ratios[unit as usize], canonical))
}

/// Absolutizes a style value. None means the C++ fallback must handle the whole value.
pub(crate) fn absolutize(value: &StyleValueData, context: &AbsolutizationContext) -> Option<Absolutized> {
    use crate::css::calc::{
        ANGLE_UNIT_CANONICAL_RATIOS, FREQUENCY_UNIT_CANONICAL_RATIOS, RESOLUTION_UNIT_CANONICAL_RATIOS,
        TIME_UNIT_CANONICAL_RATIOS,
    };

    // Structural rebuild helper: absolutize each child; identity when nothing changed.
    macro_rules! rebuild {
        ($any_changed:ident, $build:expr) => {{
            if $any_changed {
                Some(Absolutized::Changed(retain_new($build)))
            } else {
                Some(Absolutized::Unchanged)
            }
        }};
    }

    match value {
        // The dimension conversions: identity when already canonical.
        StyleValueData::Angle { value, unit } => {
            match canonicalized_dimension(*value, *unit, &ANGLE_UNIT_CANONICAL_RATIOS) {
                None => Some(Absolutized::Unchanged),
                Some((value, unit)) => Some(Absolutized::Changed(retain_new(StyleValueData::Angle { value, unit }))),
            }
        }
        StyleValueData::Time { value, unit } => {
            match canonicalized_dimension(*value, *unit, &TIME_UNIT_CANONICAL_RATIOS) {
                None => Some(Absolutized::Unchanged),
                Some((value, unit)) => Some(Absolutized::Changed(retain_new(StyleValueData::Time { value, unit }))),
            }
        }
        StyleValueData::Frequency { value, unit } => {
            match canonicalized_dimension(*value, *unit, &FREQUENCY_UNIT_CANONICAL_RATIOS) {
                None => Some(Absolutized::Unchanged),
                Some((value, unit)) => Some(Absolutized::Changed(retain_new(StyleValueData::Frequency {
                    value,
                    unit,
                }))),
            }
        }
        StyleValueData::Resolution { value, unit } => {
            match canonicalized_dimension(*value, *unit, &RESOLUTION_UNIT_CANONICAL_RATIOS) {
                None => Some(Absolutized::Unchanged),
                Some((value, unit)) => Some(Absolutized::Changed(retain_new(StyleValueData::Resolution {
                    value,
                    unit,
                }))),
            }
        }
        StyleValueData::Length { value, unit } => {
            let result = absolutize_length(*value, *unit as usize, context.length);
            if !result.handled {
                return None;
            }
            if result.resolved_viewport_relative_length {
                context.resolved_viewport_relative_length.set(true);
            }
            if !result.changed {
                return Some(Absolutized::Unchanged);
            }
            Some(Absolutized::Changed(retain_new(StyleValueData::Length {
                value: result.px,
                unit: crate::css::style_compute::px_length_unit(),
            })))
        }

        // Keywords: currentcolor computes to itself; other color keywords resolve through the
        // system-color tables into a legacy rgb() form, mirroring KeywordStyleValue::absolutized
        // (an unresolvable color keyword computes to itself there too); everything else is
        // identity.
        StyleValueData::Keyword { keyword: code } => {
            if *code != keyword::CURRENTCOLOR && keyword_is_color(*code) {
                let input = color_resolution_input(context);
                return match to_color(value, &input) {
                    None => Some(Absolutized::Unchanged),
                    Some(color) => Some(Absolutized::Changed(retain_new(rgb_color_function(
                        f64::from(color.r),
                        f64::from(color.g),
                        f64::from(color.b),
                        f64::from(f32::from(color.a) / 255.0),
                        COLOR_SYNTAX_LEGACY,
                    )))),
                };
            }
            Some(Absolutized::Unchanged)
        }

        // Structural values: recurse and rebuild only on change.
        StyleValueData::Ratio { numerator, denominator } => {
            let mut changed = false;
            let numerator = absolutize_child(numerator, context, &mut changed)?;
            let denominator = absolutize_child(denominator, context, &mut changed)?;
            rebuild!(changed, StyleValueData::Ratio { numerator, denominator })
        }
        StyleValueData::Superellipse { parameter } => {
            let mut changed = false;
            let parameter = absolutize_child(parameter, context, &mut changed)?;
            rebuild!(changed, StyleValueData::Superellipse { parameter })
        }
        StyleValueData::OpenTypeTagged {
            mode,
            tag,
            packed_tag,
            value,
        } => {
            let mut changed = false;
            let value = absolutize_child(value, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::OpenTypeTagged {
                    mode: *mode,
                    tag: tag.clone(),
                    packed_tag: *packed_tag,
                    value,
                }
            )
        }
        StyleValueData::Function { name, value } => {
            let mut changed = false;
            let value = absolutize_child(value, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::Function {
                    name: name.clone(),
                    value,
                }
            )
        }
        StyleValueData::Tuple { values } => {
            let mut changed = false;
            let values = absolutize_list(values, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::Tuple {
                    values: RetainedStyleValueDataList::from_retained_values(values),
                }
            )
        }
        StyleValueData::ValueList {
            values,
            separator,
            collapsible,
        } => {
            let mut changed = false;
            let values = absolutize_list(values, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::ValueList {
                    values: RetainedStyleValueDataList::from_retained_values(values),
                    separator: *separator,
                    collapsible: *collapsible,
                }
            )
        }
        StyleValueData::Rect {
            top,
            right,
            bottom,
            left,
        } => {
            let mut changed = false;
            let top = absolutize_child(top, context, &mut changed)?;
            let right = absolutize_child(right, context, &mut changed)?;
            let bottom = absolutize_child(bottom, context, &mut changed)?;
            let left = absolutize_child(left, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::Rect {
                    top,
                    right,
                    bottom,
                    left
                }
            )
        }
        StyleValueData::BorderRadius {
            is_elliptical,
            horizontal_radius,
            vertical_radius,
        } => {
            let mut changed = false;
            let horizontal_radius = absolutize_child(horizontal_radius, context, &mut changed)?;
            let vertical_radius = absolutize_child(vertical_radius, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::BorderRadius {
                    is_elliptical: *is_elliptical,
                    horizontal_radius,
                    vertical_radius,
                }
            )
        }
        StyleValueData::BorderRadiusRect {
            top_left,
            top_right,
            bottom_right,
            bottom_left,
        } => {
            let mut changed = false;
            let top_left = absolutize_child(top_left, context, &mut changed)?;
            let top_right = absolutize_child(top_right, context, &mut changed)?;
            let bottom_right = absolutize_child(bottom_right, context, &mut changed)?;
            let bottom_left = absolutize_child(bottom_left, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::BorderRadiusRect {
                    top_left,
                    top_right,
                    bottom_right,
                    bottom_left,
                }
            )
        }
        StyleValueData::BackgroundSize { size_x, size_y } => {
            let mut changed = false;
            let size_x = absolutize_child(size_x, context, &mut changed)?;
            let size_y = absolutize_child(size_y, context, &mut changed)?;
            rebuild!(changed, StyleValueData::BackgroundSize { size_x, size_y })
        }
        StyleValueData::ScrollbarColor {
            thumb_color,
            track_color,
        } => {
            let mut changed = false;
            let thumb_color = absolutize_child(thumb_color, context, &mut changed)?;
            let track_color = absolutize_child(track_color, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::ScrollbarColor {
                    thumb_color,
                    track_color
                }
            )
        }
        StyleValueData::Shadow {
            shadow_type,
            color,
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            placement,
        } => {
            // The C++ absolutization reads through the defaulting accessors, so absent color,
            // blur and spread slots materialize as currentcolor and zero lengths.
            let mut changed = false;
            let color = absolutize_defaulted_child(color, currentcolor_keyword, context, &mut changed)?;
            let offset_x = absolutize_child(offset_x, context, &mut changed)?;
            let offset_y = absolutize_child(offset_y, context, &mut changed)?;
            let blur_radius = absolutize_defaulted_child(blur_radius, zero_px_length, context, &mut changed)?;
            let spread_distance = absolutize_defaulted_child(spread_distance, zero_px_length, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::Shadow {
                    shadow_type: *shadow_type,
                    color,
                    offset_x,
                    offset_y,
                    blur_radius,
                    spread_distance,
                    placement: *placement,
                }
            )
        }
        StyleValueData::FontStyle {
            font_style,
            angle_value,
        } => {
            let mut changed = false;
            let angle_value = absolutize_child(angle_value, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::FontStyle {
                    font_style: *font_style,
                    angle_value,
                }
            )
        }
        StyleValueData::TextIndent {
            length_percentage,
            hanging,
            each_line,
        } => {
            let mut changed = false;
            let length_percentage = absolutize_child(length_percentage, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::TextIndent {
                    length_percentage,
                    hanging: *hanging,
                    each_line: *each_line,
                }
            )
        }
        StyleValueData::OverflowClipMargin {
            has_visual_box,
            visual_box,
            offset,
        } => {
            let mut changed = false;
            let offset = absolutize_child(offset, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::OverflowClipMargin {
                    has_visual_box: *has_visual_box,
                    visual_box: *visual_box,
                    offset,
                }
            )
        }
        StyleValueData::RadialSize {
            component_count,
            is_extent_0,
            extent_0,
            value_0,
            is_extent_1,
            extent_1,
            value_1,
        } => {
            let mut changed = false;
            let value_0 = absolutize_child(value_0, context, &mut changed)?;
            let value_1 = absolutize_child(value_1, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::RadialSize {
                    component_count: *component_count,
                    is_extent_0: *is_extent_0,
                    extent_0: *extent_0,
                    value_0,
                    is_extent_1: *is_extent_1,
                    extent_1: *extent_1,
                    value_1,
                }
            )
        }
        StyleValueData::Transformation {
            property,
            transform_function,
            values,
        } => {
            let mut changed = false;
            let values = absolutize_list(values, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::Transformation {
                    property: *property,
                    transform_function: *transform_function,
                    values: RetainedStyleValueDataList::from_retained_values(values),
                }
            )
        }
        StyleValueData::Filter {
            kind,
            color_operation,
            value,
        } => {
            // Color filters resolve their amount to a number, clamped to [0, 1] for
            // grayscale, invert, opacity and sepia, exactly like the C++ absolutization.
            if *kind == 3 {
                let inner = value.optional_data()?;
                let mut changed = false;
                let absolutized = absolutize_child(value, context, &mut changed)?;
                let mut amount = number_from_value(absolutized.optional_data()?, 1.0)?;
                if matches!(color_operation, 2 | 3 | 4 | 6) {
                    amount = amount.clamp(0.0, 1.0);
                }
                if !changed && matches!(inner, StyleValueData::Number { value } if *value == amount) {
                    return Some(Absolutized::Unchanged);
                }
                return Some(Absolutized::Changed(retain_new(StyleValueData::Filter {
                    kind: *kind,
                    color_operation: *color_operation,
                    value: retain_new(StyleValueData::Number { value: amount }),
                })));
            }
            let mut changed = false;
            // drop-shadow hand-rolls its shadow absolutization in C++: color and blur
            // materialize their defaults, but spread stays absent.
            let value = if *kind == 1 {
                let Some(StyleValueData::Shadow {
                    shadow_type,
                    color,
                    offset_x,
                    offset_y,
                    blur_radius,
                    spread_distance,
                    placement,
                }) = value.optional_data()
                else {
                    return None;
                };
                let color = absolutize_defaulted_child(color, currentcolor_keyword, context, &mut changed)?;
                let offset_x = absolutize_child(offset_x, context, &mut changed)?;
                let offset_y = absolutize_child(offset_y, context, &mut changed)?;
                let blur_radius = absolutize_defaulted_child(blur_radius, zero_px_length, context, &mut changed)?;
                if changed {
                    retain_new(StyleValueData::Shadow {
                        shadow_type: *shadow_type,
                        color,
                        offset_x,
                        offset_y,
                        blur_radius,
                        spread_distance: spread_distance.clone_retained(),
                        placement: *placement,
                    })
                } else {
                    value.clone_retained()
                }
            } else {
                absolutize_child(value, context, &mut changed)?
            };
            rebuild!(
                changed,
                StyleValueData::Filter {
                    kind: *kind,
                    color_operation: *color_operation,
                    value,
                }
            )
        }

        StyleValueData::Calculated { .. } => {
            // Thread a local tracking flag so viewport-relative lengths resolved inside the
            // calculation report the dependency exactly like the C++ path.
            let mut tracked = false;
            let mut calc_length_context = *context.length;
            calc_length_context.resolved_viewport_relative_length = &raw mut tracked;
            let outcome = crate::css::calc::absolutize_calculation_value(
                value,
                (&raw const calc_length_context).cast(),
                context.tree_counting,
                context.random_base_values,
            );
            if tracked {
                context.resolved_viewport_relative_length.set(true);
            }
            match outcome? {
                crate::css::calc::AbsolutizedCalculation::Unchanged => Some(Absolutized::Unchanged),
                crate::css::calc::AbsolutizedCalculation::Percentage(percentage) => {
                    Some(Absolutized::Changed(retain_new(StyleValueData::Percentage {
                        value: percentage,
                    })))
                }
                crate::css::calc::AbsolutizedCalculation::Value(new_value) => {
                    Some(Absolutized::Changed(retain_new(new_value)))
                }
            }
        }
        StyleValueData::Edge { .. } => absolutize_edge(value, context),
        StyleValueData::Position { edge_x, edge_y } => {
            let mut changed = false;
            let edge_x = absolutize_child(edge_x, context, &mut changed)?;
            let edge_y = absolutize_child(edge_y, context, &mut changed)?;
            rebuild!(changed, StyleValueData::Position { edge_x, edge_y })
        }

        StyleValueData::BasicShape { .. } => absolutize_basic_shape(value, context),

        StyleValueData::Cursor { image, x, y } => {
            let mut changed = false;
            let image = absolutize_child(image, context, &mut changed)?;
            let x = absolutize_child(x, context, &mut changed)?;
            let y = absolutize_child(y, context, &mut changed)?;
            rebuild!(changed, StyleValueData::Cursor { image, x, y })
        }
        StyleValueData::Easing {
            kind,
            linear_stops,
            x1,
            y1,
            x2,
            y2,
            number_of_intervals,
            step_position,
        } => {
            let mut changed = false;
            let mut stops = Vec::with_capacity(linear_stops.as_slice().len());
            for stop in linear_stops.as_slice() {
                let [output, input] = stop.values();
                let output = absolutize_child(output, context, &mut changed)?;
                let input = absolutize_child(input, context, &mut changed)?;
                stops.push(crate::css::style_value::RetainedLinearEasingStop::from_retained_values(
                    output, input,
                ));
            }
            let x1 = absolutize_child(x1, context, &mut changed)?;
            let y1 = absolutize_child(y1, context, &mut changed)?;
            let x2 = absolutize_child(x2, context, &mut changed)?;
            let y2 = absolutize_child(y2, context, &mut changed)?;
            let number_of_intervals = absolutize_child(number_of_intervals, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::Easing {
                    kind: *kind,
                    linear_stops: crate::css::style_value::RetainedLinearEasingStopList::from_retained_elements(stops),
                    x1,
                    y1,
                    x2,
                    y2,
                    number_of_intervals,
                    step_position: *step_position,
                }
            )
        }
        StyleValueData::ImageSet { options } => {
            let mut changed = false;
            let mut absolutized_options = Vec::with_capacity(options.as_slice().len());
            for option in options.as_slice() {
                let [image, resolution] = option.values();
                let image = absolutize_child(image, context, &mut changed)?;
                let resolution = absolutize_child(resolution, context, &mut changed)?;
                absolutized_options.push(option.with_values(image, resolution));
            }
            rebuild!(
                changed,
                StyleValueData::ImageSet {
                    options: crate::css::style_value::RetainedImageSetOptionList::from_retained_elements(
                        absolutized_options
                    ),
                }
            )
        }
        StyleValueData::CounterDefinitions { counter_definitions } => {
            let mut changed = false;
            let mut definitions = Vec::with_capacity(counter_definitions.as_slice().len());
            for definition in counter_definitions.as_slice() {
                let value = absolutize_child(definition.value(), context, &mut changed)?;
                definitions.push(definition.with_value(value));
            }
            rebuild!(
                changed,
                StyleValueData::CounterDefinitions {
                    counter_definitions: crate::css::style_value::RetainedCounterDefinitionList::from_retained_elements(
                        definitions
                    ),
                }
            )
        }
        StyleValueData::CounterStyleSystem {
            kind,
            system,
            first_symbol,
            name,
        } => {
            let mut changed = false;
            let first_symbol = absolutize_child(first_symbol, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::CounterStyleSystem {
                    kind: *kind,
                    system: *system,
                    first_symbol,
                    name: name.clone(),
                }
            )
        }
        StyleValueData::GridTrackPlacement {
            kind,
            value,
            has_name,
            name,
            implicit_start_name,
            implicit_end_name,
        } => {
            let mut changed = false;
            let value = absolutize_child(value, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::GridTrackPlacement {
                    kind: *kind,
                    value,
                    has_name: *has_name,
                    name: name.clone(),
                    implicit_start_name: implicit_start_name.clone(),
                    implicit_end_name: implicit_end_name.clone(),
                }
            )
        }
        StyleValueData::GridTrackSizeList {
            is_subgrid,
            preserve_line_name_sets,
            entries,
        } => {
            let mut changed = false;
            let entries = absolutize_grid_track_entries(entries.as_slice(), context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::GridTrackSizeList {
                    is_subgrid: *is_subgrid,
                    preserve_line_name_sets: *preserve_line_name_sets,
                    entries: crate::css::style_value::RetainedGridTrackEntryList::from_retained_elements(entries),
                }
            )
        }

        // Tree-counting functions resolve from immutable sibling facts captured before the
        // longhand drive. Random sharing and image base URLs still need live external state.
        StyleValueData::TreeCountingFunction {
            function,
            computed_type,
        } => {
            let (sibling_count, sibling_index) = context.tree_counting?;
            let value = if *function == 0 { sibling_count } else { sibling_index };
            Some(Absolutized::Changed(retain_new(if *computed_type == 0 {
                StyleValueData::Number { value: value as f64 }
            } else {
                StyleValueData::Integer {
                    value: i32::try_from(value).expect("sibling count exceeds CSS integer range"),
                }
            })))
        }
        StyleValueData::Image { .. } => absolutize_image(value, context),
        StyleValueData::RandomValueSharing { .. } => None,

        StyleValueData::ColorFunction { .. } => absolutize_color_function(value, context),
        StyleValueData::ColorMix { .. } => absolutize_color_mix(value, context),
        StyleValueData::LinearGradient { .. }
        | StyleValueData::RadialGradient { .. }
        | StyleValueData::ConicGradient { .. } => absolutize_gradient(value, context),

        // Port of ContrastColorStyleValue::absolutized: a resolvable contrast-color computes
        // to its picked foreground color; otherwise the inner color absolutizes in place.
        StyleValueData::ContrastColor { color, .. } => {
            let input = color_resolution_input(context);
            if let Some(resolved) = to_color(value, &input) {
                return Some(Absolutized::Changed(retain_new(rgb_color_function(
                    f64::from(resolved.r),
                    f64::from(resolved.g),
                    f64::from(resolved.b),
                    f64::from(resolved.a) / 255.0,
                    COLOR_SYNTAX_MODERN,
                ))));
            }
            let mut changed = false;
            let color = absolutize_child(color, context, &mut changed)?;
            rebuild!(
                changed,
                StyleValueData::ContrastColor {
                    color_base: ColorBase {
                        has_color_type: false,
                        color_type: 0,
                        color_syntax: COLOR_SYNTAX_MODERN,
                    },
                    color,
                }
            )
        }

        // Port of LightDarkStyleValue::absolutized: with no scheme the value computes to
        // itself; otherwise it collapses to the matching branch's absolutized value.
        StyleValueData::LightDark { light, dark, .. } => {
            let Some(scheme) = context.scheme else {
                return Some(Absolutized::Unchanged);
            };
            let branch = if scheme == PREFERRED_COLOR_SCHEME_DARK {
                dark
            } else {
                light
            };
            let mut changed = false;
            let absolutized = absolutize_child(branch, context, &mut changed)?;
            Some(Absolutized::Changed(absolutized))
        }

        StyleValueData::OpacityValue { value } => {
            let inner = value.optional_data()?;
            // Plain numbers strictly inside (0, 1) stay untouched, like the C++ fast path.
            if matches!(inner, StyleValueData::Number { value } if *value > 0.0 && *value < 1.0) {
                return Some(Absolutized::Unchanged);
            }
            let mut changed = false;
            let absolutized = absolutize_child(value, context, &mut changed)?;
            let number = number_from_value(absolutized.optional_data()?, 1.0)?;
            Some(Absolutized::Changed(retain_new(StyleValueData::OpacityValue {
                value: retain_new(StyleValueData::Number {
                    value: number.clamp(0.0, 1.0),
                }),
            })))
        }

        // Everything else is identity under the C++ dispatcher's default case.
        _ => Some(Absolutized::Unchanged),
    }
}

/// The absolutization outcome kinds for the FFI entry.
pub const ABSOLUTIZED_DECLINED: u8 = 0;
pub const ABSOLUTIZED_UNCHANGED: u8 = 1;
pub const ABSOLUTIZED_CHANGED: u8 = 2;

/// The FFI result: `data` carries one strong reference when kind is changed.
#[repr(C)]
pub struct FfiAbsolutizedValue {
    pub kind: u8,
    pub data: *const core::ffi::c_void,
}

/// Absolutizes a style value for the C++ dispatch outside the style drive: container and
/// feature queries, custom property registration and counter-style definitions. Declines
/// exactly where the in-drive recursion declines.
///
/// # Safety
/// `value` must point at live style value data; `length` may be null or point at a valid
/// length resolution context outliving the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_absolutize(
    value: *const core::ffi::c_void,
    length: *const core::ffi::c_void,
    has_scheme: bool,
    scheme: u8,
) -> FfiAbsolutizedValue {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueQueryEntry);
    crate::abort_on_panic(|| {
        let value = unsafe { &*value.cast::<StyleValueData>() };
        let length = unsafe { length.cast::<FfiLengthResolutionContext>().as_ref() };
        // Without a length context the recursion still handles everything that does not
        // resolve lengths; a zeroed context would silently mis-resolve, so decline instead.
        let Some(length) = length else {
            return match value {
                value if crate::css::style_compute::value_absolutization_is_identity(value) => FfiAbsolutizedValue {
                    kind: ABSOLUTIZED_UNCHANGED,
                    data: core::ptr::null(),
                },
                _ => FfiAbsolutizedValue {
                    kind: ABSOLUTIZED_DECLINED,
                    data: core::ptr::null(),
                },
            };
        };
        let context = AbsolutizationContext {
            length,
            scheme: has_scheme.then_some(scheme),
            resolved_viewport_relative_length: Cell::new(false),
            tree_counting: None,
            random_base_values: &[],
            document_base_url: &[],
            style_sheet_resource_context: None,
        };
        match absolutize(value, &context) {
            Some(Absolutized::Unchanged) => FfiAbsolutizedValue {
                kind: ABSOLUTIZED_UNCHANGED,
                data: core::ptr::null(),
            },
            Some(Absolutized::Changed(new_value)) => {
                let pointer = new_value.pointer();
                core::mem::forget(new_value);
                FfiAbsolutizedValue {
                    kind: ABSOLUTIZED_CHANGED,
                    data: pointer.cast(),
                }
            }
            None => FfiAbsolutizedValue {
                kind: ABSOLUTIZED_DECLINED,
                data: core::ptr::null(),
            },
        }
    })
}
