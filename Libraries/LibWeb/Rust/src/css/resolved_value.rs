/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The resolved values of the properties whose resolved value is not their computed value.
//! https://drafts.csswg.org/cssom/#resolved-values

use crate::css::color_conversion::{LAB, LCH, OKLAB, OKLCH};
use crate::css::color_resolution::{
    ColorResolutionInput, FfiColorResolutionStyle, Rgba, SerializationBehavior, descriptor_for_color_type,
    resolve_color_mix, resolve_relative_form, to_color,
};
use crate::css::computed_value_types::ComputedShadow;
use crate::css::css_enums::keyword;
use crate::css::css_pixels::CssPixels;
use crate::css::serialize::color_function_has_missing_component;
use crate::css::style_value::{
    ColorBase, ColorFunctionPayload, CssString, FILTER_KIND_DROP_SHADOW, RetainedStyleValueData,
    RetainedStyleValueDataList, ShadowPayload, StyleValueBox, StyleValueData, shared_style_value,
    value_depends_on_current_color,
};
use crate::css::style_value_arc::StyleValueArc;
use crate::css::value_codes::{COLOR_SYNTAX_MODERN, COLOR_TYPE_RGB};

const VALUE_LIST_SEPARATOR_SPACE: u8 = 0;
const VALUE_LIST_SEPARATOR_COMMA: u8 = 1;

fn number(value: f64) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(shared_style_value(StyleValueData::Number { value }))
}

fn length_px(value: f64) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(shared_style_value(StyleValueData::Length {
        value,
        unit: crate::css::calc::canonical_unit_code(&crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS),
    }))
}

fn color_value(color: Rgba) -> StyleValueArc {
    StyleValueArc::new(StyleValueData::ColorFunction(StyleValueBox::new(
        ColorFunctionPayload {
            color_base: ColorBase {
                has_color_type: true,
                color_type: COLOR_TYPE_RGB,
                color_syntax: COLOR_SYNTAX_MODERN,
            },
            channel_0: number(f64::from(color.r)),
            channel_1: number(f64::from(color.g)),
            channel_2: number(f64::from(color.b)),
            alpha: number(f64::from(color.a) / 255.0),
            has_name: false,
            name: CssString::none(),
            origin_color: RetainedStyleValueData::none(),
        },
    )))
}

fn color_base(value: &StyleValueData) -> Option<&ColorBase> {
    match value {
        StyleValueData::ColorFunction(payload) => Some(&payload.color_base),
        StyleValueData::ColorMix(payload) => Some(&payload.color_base),
        StyleValueData::LightDark(payload) => Some(&payload.color_base),
        StyleValueData::ContrastColor { color_base, .. } => Some(color_base),
        _ => None,
    }
}

/// The resolved value of a color property, whose computed value is `value` and whose used color is `computed_color`.
/// A `style` of `None` keeps relative colors and mixes as they are.
fn resolve_color_style_value(
    value: &StyleValueArc,
    computed_color: Rgba,
    style: Option<&ColorResolutionInput>,
) -> StyleValueArc {
    let color_type = color_base(value)
        .filter(|base| base.has_color_type)
        .map(|base| base.color_type);

    if let Some(style) = style {
        if let StyleValueData::ColorFunction(color_function) = &**value
            && color_function.origin_color.optional_data().is_some()
            && color_type.is_some()
        {
            return match resolve_relative_form(value, style) {
                Some(resolved) => StyleValueArc::new(crate::css::color_resolution::relative_color_computed_value_form(
                    resolved,
                )),
                None => value.clone(),
            };
        }

        // NB: A computed color-mix() is still a mix only where it reads currentcolor, which the used value resolves.
        if matches!(&**value, StyleValueData::ColorMix(_))
            && let Some(resolved) = resolve_color_mix(value, style)
        {
            return StyleValueArc::new(resolved);
        }
    }

    if let StyleValueData::ColorFunction(_) = &**value {
        let serializes_as_color_function = color_type.is_some_and(|color_type| {
            matches!(
                descriptor_for_color_type(color_type).behavior,
                SerializationBehavior::ColorFunction
            )
        });
        if serializes_as_color_function {
            return value.clone();
        }
        // https://drafts.csswg.org/css-color-4/#css-serialization-of-srgb
        // NB: An sRGB value with missing components serializes them as none, which the used color cannot represent.
        if color_function_has_missing_component(value) {
            return value.clone();
        }
    }
    if color_type.is_some_and(|color_type| matches!(color_type, LAB | OKLAB | LCH | OKLCH)) {
        return value.clone();
    }

    color_value(computed_color)
}

// https://drafts.csswg.org/css-color-4/#resolving-color-values
// The resolved value of a <color> is its used value.
fn resolve_svg_color_style_value(value: &StyleValueArc, style: &ColorResolutionInput) -> StyleValueArc {
    if !value_depends_on_current_color(value) {
        return value.clone();
    }

    if let StyleValueData::ValueList {
        values,
        separator,
        collapsible,
    } = &**value
    {
        let values = values
            .as_slice()
            .iter()
            .map(|item| {
                RetainedStyleValueData::from_arc(resolve_svg_color_style_value(&item.clone().into_arc(), style))
            })
            .collect();
        return StyleValueArc::new(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(values),
            separator: *separator,
            collapsible: *collapsible,
        });
    }

    match to_color(value, style) {
        Some(color) => resolve_color_style_value(value, color, Some(style)),
        None => value.clone(),
    }
}

// The stored computed filter value keeps currentcolor inside drop-shadow(); the resolved value
// serializes it against the element's used color, like the color-valued properties.
fn resolve_filter_style_value(value: &StyleValueArc, current_color: Rgba) -> StyleValueArc {
    let StyleValueData::ValueList { values, .. } = &**value else {
        return value.clone();
    };
    let mut resolved_any = false;
    let filters = values
        .as_slice()
        .iter()
        .map(|filter_value| {
            if let Some(StyleValueData::Filter {
                kind: FILTER_KIND_DROP_SHADOW,
                color_operation,
                value: shadow,
            }) = filter_value.optional_data()
                && let Some(StyleValueData::Shadow(shadow)) = shadow.optional_data()
                && shadow.color.optional_data().is_none_or(
                    |color| matches!(color, StyleValueData::Keyword { keyword } if *keyword == keyword::CURRENTCOLOR),
                )
            {
                resolved_any = true;
                let shadow = StyleValueData::Shadow(StyleValueBox::new(ShadowPayload {
                    color: RetainedStyleValueData::from_arc(color_value(current_color)),
                    ..(**shadow).clone()
                }));
                return RetainedStyleValueData::from_arc(StyleValueArc::new(StyleValueData::Filter {
                    kind: FILTER_KIND_DROP_SHADOW,
                    color_operation: *color_operation,
                    value: RetainedStyleValueData::from_arc(StyleValueArc::new(shadow)),
                }));
            }
            filter_value.clone()
        })
        .collect();
    if !resolved_any {
        return value.clone();
    }
    StyleValueArc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(filters),
        separator: VALUE_LIST_SEPARATOR_SPACE,
        collapsible: false,
    })
}

// Allowing exactly panning and pinch-zoom serializes as `manipulation`, whatever form specified it. The parser
// orders a list as horizontal, vertical, pinch-zoom, with at most one of each.
fn resolve_touch_action_style_value(value: &StyleValueArc) -> StyleValueArc {
    if let StyleValueData::ValueList { values, .. } = &**value {
        let is_keyword = |item: &RetainedStyleValueData, expected: u16| matches!(item.optional_data(), Some(StyleValueData::Keyword { keyword }) if *keyword == expected);
        if let [horizontal, vertical, _] = values.as_slice()
            && is_keyword(horizontal, keyword::PAN_X)
            && is_keyword(vertical, keyword::PAN_Y)
        {
            return shared_style_value(StyleValueData::Keyword {
                keyword: keyword::MANIPULATION,
            });
        }
    }
    value.clone()
}

fn style_value_for_shadow(shadow_type: u8, shadows: &[ComputedShadow], current_color: Rgba) -> StyleValueArc {
    let pixels = |raw: i32| length_px(CssPixels::from_raw(raw).to_double());
    let make_shadow_style_value = |shadow: &ComputedShadow| {
        let color = if shadow.color.is_current_color {
            current_color
        } else {
            Rgba::from_packed(shadow.color.rgba)
        };
        StyleValueArc::new(StyleValueData::Shadow(StyleValueBox::new(ShadowPayload {
            shadow_type,
            color: RetainedStyleValueData::from_arc(color_value(color)),
            offset_x: pixels(shadow.offset_x),
            offset_y: pixels(shadow.offset_y),
            blur_radius: pixels(shadow.blur_radius),
            spread_distance: pixels(shadow.spread_distance),
            placement: shadow.placement as u8,
        })))
    };

    match shadows {
        [] => shared_style_value(StyleValueData::Keyword { keyword: keyword::NONE }),
        [shadow] => make_shadow_style_value(shadow),
        shadows => StyleValueArc::new(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(
                shadows
                    .iter()
                    .map(|shadow| RetainedStyleValueData::from_arc(make_shadow_style_value(shadow)))
                    .collect(),
            ),
            separator: VALUE_LIST_SEPARATOR_COMMA,
            collapsible: true,
        }),
    }
}

/// A new reference to `value`, which the caller lends.
fn retained(value: &StyleValueData) -> StyleValueArc {
    // SAFETY: Every value the host lends is a counted one.
    unsafe {
        StyleValueArc::increment_strong_count(value);
        StyleValueArc::from_raw(value)
    }
}

/// The resolved value of a color property, as one strong reference. `style` may be null.
///
/// # Safety
/// `style` must be null or point at a style whose group payloads outlive the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolved_color_value(
    value: &StyleValueData,
    computed_color: u32,
    style: *const FfiColorResolutionStyle,
) -> *const StyleValueData {
    // SAFETY: Guaranteed by the caller.
    let input = (!style.is_null()).then(|| unsafe { ColorResolutionInput::for_ffi_style(style) });
    StyleValueArc::into_raw(resolve_color_style_value(
        &retained(value),
        Rgba::from_packed(computed_color),
        input.as_ref(),
    ))
}

/// The resolved value of a paint or color SVG property, as one strong reference.
///
/// # Safety
/// `style` must point at a style whose group payloads outlive the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolved_svg_color_value(
    value: &StyleValueData,
    style: *const FfiColorResolutionStyle,
) -> *const StyleValueData {
    // SAFETY: Guaranteed by the caller.
    let input = unsafe { ColorResolutionInput::for_ffi_style(style) };
    StyleValueArc::into_raw(resolve_svg_color_style_value(&retained(value), &input))
}

/// The resolved value of a filter property, as one strong reference.
#[unsafe(no_mangle)]
pub extern "C" fn rust_resolved_filter_value(value: &StyleValueData, current_color: u32) -> *const StyleValueData {
    StyleValueArc::into_raw(resolve_filter_style_value(
        &retained(value),
        Rgba::from_packed(current_color),
    ))
}

/// The resolved value of touch-action, as one strong reference.
#[unsafe(no_mangle)]
pub extern "C" fn rust_resolved_touch_action_value(value: &StyleValueData) -> *const StyleValueData {
    StyleValueArc::into_raw(resolve_touch_action_style_value(&retained(value)))
}

/// The resolved value of a shadow property, as one strong reference.
///
/// # Safety
/// `shadows` must point at `shadow_count` `ComputedShadow`s, or be null with a count of 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolved_shadow_value(
    shadow_type: u8,
    shadows: *const core::ffi::c_void,
    shadow_count: usize,
    current_color: u32,
) -> *const StyleValueData {
    // SAFETY: Guaranteed by the caller.
    let shadows = unsafe { ak::slice_from_raw_parts(shadows.cast::<ComputedShadow>(), shadow_count) };
    StyleValueArc::into_raw(style_value_for_shadow(
        shadow_type,
        shadows,
        Rgba::from_packed(current_color),
    ))
}
