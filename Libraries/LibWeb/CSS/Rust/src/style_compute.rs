/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Rust style computation core.
//!
//! This module is growing into StyleComputer's cascade and compute logic. The
//! first piece is length absolutization: resolving font-relative, viewport-
//! relative and absolute lengths to pixels during style computation, exactly
//! as the C++ implementation does (the resolved value is the unrounded double;
//! CSSPixels fixed-point rounding happens later at the consumers).
//!
//! Container-relative units are not handled yet and report handled = false so
//! the C++ caller falls back to its own resolution.

use std::ffi::c_void;
use std::sync::OnceLock;

use crate::abort_on_panic;
use crate::css_pixels::CssPixels;
use crate::property_metadata::property_is_inherited;
use crate::style_value::StyleValueData;

include!(concat!(env!("OUT_DIR"), "/length_units_generated.rs"));
include!(concat!(env!("OUT_DIR"), "/keywords_generated.rs"));

/// FFI accessor for the keyword-code parity test on the C++ side.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_compute_keyword_code_bold() -> u16 {
    keyword::BOLD
}

/// The font metrics needed for font-relative length resolution, as unrounded
/// CSS pixel values.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiFontMetrics {
    pub font_size: f64,
    pub x_height: f64,
    pub cap_height: f64,
    pub zero_advance: f64,
    pub line_height: f64,
}

/// Mirror of the length resolution parts of Length::ResolutionContext.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiLengthResolutionContext {
    pub viewport_width: f64,
    pub viewport_height: f64,
    pub font_metrics: FfiFontMetrics,
    pub root_font_metrics: FfiFontMetrics,
    pub font_metrics_depend_on_viewport_metrics: bool,
    pub root_font_metrics_depend_on_viewport_metrics: bool,
}

/// Result of absolutizing a length.
#[repr(C)]
pub struct FfiAbsolutizedLength {
    /// False when the unit is not handled in Rust yet (container-relative);
    /// the caller must fall back to the C++ resolution.
    pub handled: bool,
    /// False when the length was already absolute pixels and is unchanged.
    pub changed: bool,
    /// True when resolving consumed viewport metrics, either directly or
    /// through font metrics that depend on them.
    pub resolved_viewport_relative_length: bool,
    pub px: f64,
}

#[derive(Clone, Copy)]
enum LengthUnitKind {
    Px,
    Absolute { px_per_unit: f64 },
    FontRelative { metric: FontMetricSelector, root: bool },
    ViewportRelative { axis: ViewportAxis },
    ContainerRelative,
}

#[derive(Clone, Copy)]
enum FontMetricSelector {
    FontSize,
    XHeight,
    CapHeight,
    ZeroAdvance,
    LineHeight,
}

#[derive(Clone, Copy)]
enum ViewportAxis {
    Width,
    Height,
    Min,
    Max,
}

fn length_unit_kinds() -> &'static [LengthUnitKind] {
    static KINDS: OnceLock<Vec<LengthUnitKind>> = OnceLock::new();
    KINDS.get_or_init(|| {
        LENGTH_UNIT_NAMES
            .iter()
            .zip(LENGTH_UNIT_CANONICAL_PX_RATIOS)
            .map(|(&name, ratio)| match name {
                "px" => LengthUnitKind::Px,
                // NB: ic and ric use the font size until the CJK water ideograph
                //     advance is available, matching the C++ FIXME.
                "em" | "ic" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::FontSize,
                    root: false,
                },
                "rem" | "ric" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::FontSize,
                    root: true,
                },
                "ex" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::XHeight,
                    root: false,
                },
                "rex" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::XHeight,
                    root: true,
                },
                "cap" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::CapHeight,
                    root: false,
                },
                "rcap" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::CapHeight,
                    root: true,
                },
                "ch" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::ZeroAdvance,
                    root: false,
                },
                "rch" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::ZeroAdvance,
                    root: true,
                },
                "lh" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::LineHeight,
                    root: false,
                },
                "rlh" => LengthUnitKind::FontRelative {
                    metric: FontMetricSelector::LineHeight,
                    root: true,
                },
                // NB: vi maps to the width and vb to the height until the C++
                //     side selects by inline axis, matching the FIXME there.
                "vw" | "svw" | "lvw" | "dvw" | "vi" | "svi" | "lvi" | "dvi" => LengthUnitKind::ViewportRelative {
                    axis: ViewportAxis::Width,
                },
                "vh" | "svh" | "lvh" | "dvh" | "vb" | "svb" | "lvb" | "dvb" => LengthUnitKind::ViewportRelative {
                    axis: ViewportAxis::Height,
                },
                "vmin" | "svmin" | "lvmin" | "dvmin" => LengthUnitKind::ViewportRelative {
                    axis: ViewportAxis::Min,
                },
                "vmax" | "svmax" | "lvmax" | "dvmax" => LengthUnitKind::ViewportRelative {
                    axis: ViewportAxis::Max,
                },
                _ if ratio.is_finite() => LengthUnitKind::Absolute { px_per_unit: ratio },
                _ => LengthUnitKind::ContainerRelative,
            })
            .collect()
    })
}

fn select_font_metric(metrics: &FfiFontMetrics, metric: FontMetricSelector) -> f64 {
    match metric {
        FontMetricSelector::FontSize => metrics.font_size,
        FontMetricSelector::XHeight => metrics.x_height,
        FontMetricSelector::CapHeight => metrics.cap_height,
        FontMetricSelector::ZeroAdvance => metrics.zero_advance,
        FontMetricSelector::LineHeight => metrics.line_height,
    }
}

fn absolutize_length(value: f64, unit: usize, context: &FfiLengthResolutionContext) -> FfiAbsolutizedLength {
    let kinds = length_unit_kinds();
    let unhandled = FfiAbsolutizedLength {
        handled: false,
        changed: false,
        resolved_viewport_relative_length: false,
        px: 0.0,
    };
    let Some(kind) = kinds.get(unit) else {
        return unhandled;
    };
    match *kind {
        LengthUnitKind::Px => FfiAbsolutizedLength {
            handled: true,
            changed: false,
            resolved_viewport_relative_length: false,
            px: value,
        },
        LengthUnitKind::Absolute { px_per_unit } => FfiAbsolutizedLength {
            handled: true,
            changed: true,
            resolved_viewport_relative_length: false,
            px: px_per_unit * value,
        },
        LengthUnitKind::FontRelative { metric, root } => {
            let metrics = if root {
                &context.root_font_metrics
            } else {
                &context.font_metrics
            };
            let depends_on_viewport = if root {
                context.root_font_metrics_depend_on_viewport_metrics
            } else {
                context.font_metrics_depend_on_viewport_metrics
            };
            FfiAbsolutizedLength {
                handled: true,
                changed: true,
                resolved_viewport_relative_length: depends_on_viewport,
                px: value * select_font_metric(metrics, metric),
            }
        }
        LengthUnitKind::ViewportRelative { axis } => {
            let basis = match axis {
                ViewportAxis::Width => context.viewport_width,
                ViewportAxis::Height => context.viewport_height,
                ViewportAxis::Min => context.viewport_width.min(context.viewport_height),
                ViewportAxis::Max => context.viewport_width.max(context.viewport_height),
            };
            FfiAbsolutizedLength {
                handled: true,
                changed: true,
                resolved_viewport_relative_length: true,
                px: basis * value / 100.0,
            }
        }
        LengthUnitKind::ContainerRelative => unhandled,
    }
}

/// Absolutizes a length during style computation.
///
/// # Safety
/// `context` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_absolutize_length(
    value: f64,
    unit: u8,
    context: *const FfiLengthResolutionContext,
) -> FfiAbsolutizedLength {
    abort_on_panic(|| absolutize_length(value, unit as usize, unsafe { &*context }))
}

/// Result of computing a property that resolves to a number.
#[repr(C)]
pub struct FfiComputedNumber {
    /// False when the value needs C++ handling (calc).
    pub handled: bool,
    /// True when the absolutized value is already the computed value.
    pub unchanged: bool,
    pub value: f64,
}

const NUMBER_UNHANDLED: FfiComputedNumber = FfiComputedNumber {
    handled: false,
    unchanged: false,
    value: 0.0,
};

// https://drafts.csswg.org/css-fonts-4/#font-weight-prop
// a number, see below
fn compute_font_weight(value: &StyleValueData, inherited_font_weight: f64) -> FfiComputedNumber {
    let computed = |value| FfiComputedNumber {
        handled: true,
        unchanged: false,
        value,
    };
    match value {
        // <number [1,1000]>
        StyleValueData::Number { .. } => FfiComputedNumber {
            handled: true,
            unchanged: true,
            value: 0.0,
        },
        StyleValueData::Keyword { keyword } => match *keyword {
            // normal
            // Same as 400.
            keyword::NORMAL => computed(400.0),
            // bold
            // Same as 700.
            keyword::BOLD => computed(700.0),
            // Specified values of bolder and lighter indicate weights relative to the weight of the
            // parent element. The computed weight is calculated based on the inherited font-weight
            // value using the chart below.
            //
            // Inherited value (w)  bolder     lighter
            // w < 100              400        No change
            // 100 <= w < 350       400        100
            // 350 <= w < 550       700        100
            // 550 <= w < 750       900        400
            // 750 <= w < 900       900        700
            // 900 <= w             No change  700
            //
            // bolder
            // Specifies a bolder weight than the inherited value. See 2.2.1 Relative Weights.
            keyword::BOLDER => {
                if inherited_font_weight < 350.0 {
                    computed(400.0)
                } else if inherited_font_weight < 550.0 {
                    computed(700.0)
                } else if inherited_font_weight < 900.0 {
                    computed(900.0)
                } else {
                    computed(inherited_font_weight)
                }
            }
            // lighter
            // Specifies a lighter weight than the inherited value. See 2.2.1 Relative Weights.
            keyword::LIGHTER => {
                if inherited_font_weight < 100.0 {
                    computed(inherited_font_weight)
                } else if inherited_font_weight < 550.0 {
                    computed(100.0)
                } else if inherited_font_weight < 750.0 {
                    computed(400.0)
                } else {
                    computed(700.0)
                }
            }
            _ => NUMBER_UNHANDLED,
        },
        // AD-HOC: calc values are resolved by the C++ caller.
        _ => NUMBER_UNHANDLED,
    }
}

// https://drafts.csswg.org/css-fonts-4/#font-width-prop
// a percentage, see below
fn compute_font_width(value: &StyleValueData) -> FfiComputedNumber {
    let computed = |value| FfiComputedNumber {
        handled: true,
        unchanged: false,
        value,
    };
    match value {
        // <percentage [0,inf]>
        StyleValueData::Percentage { .. } => FfiComputedNumber {
            handled: true,
            unchanged: true,
            value: 0.0,
        },
        StyleValueData::Keyword { keyword } => match *keyword {
            // ultra-condensed 50%
            keyword::ULTRA_CONDENSED => computed(50.0),
            // extra-condensed 62.5%
            keyword::EXTRA_CONDENSED => computed(62.5),
            // condensed 75%
            keyword::CONDENSED => computed(75.0),
            // semi-condensed 87.5%
            keyword::SEMI_CONDENSED => computed(87.5),
            // normal 100%
            keyword::NORMAL => computed(100.0),
            // semi-expanded 112.5%
            keyword::SEMI_EXPANDED => computed(112.5),
            // expanded 125%
            keyword::EXPANDED => computed(125.0),
            // extra-expanded 150%
            keyword::EXTRA_EXPANDED => computed(150.0),
            // ultra-expanded 200%
            keyword::ULTRA_EXPANDED => computed(200.0),
            _ => NUMBER_UNHANDLED,
        },
        // AD-HOC: calc percentages are resolved by the C++ caller.
        _ => NUMBER_UNHANDLED,
    }
}

/// Computes the font-width property from its absolutized value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_font_width(absolutized_value: *const c_void) -> FfiComputedNumber {
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_font_width(value)
    })
}

// https://drafts.csswg.org/css-fonts/#font-size-prop
// an absolute length
//
// The keyword mappings use CSSPixels fixed-point arithmetic, exactly as the
// C++ implementation does.
fn compute_font_size(
    value: &StyleValueData,
    computed_math_depth: i32,
    inherited_font_size: CssPixels,
    inherited_math_depth: i32,
    default_font_size: CssPixels,
) -> FfiComputedNumber {
    let computed = |px: f64| FfiComputedNumber {
        handled: true,
        unchanged: false,
        value: px,
    };
    // An <absolute-size> keyword refers to an entry in a table of font sizes computed and kept by
    // the user agent. See 2.5.1 Absolute Size Keyword Mapping Table.
    let absolute = |numerator: i64, denominator: i64| {
        computed(
            (default_font_size * CssPixels::from_integer(numerator))
                .div_as_fraction(CssPixels::from_integer(denominator))
                .to_double(),
        )
    };
    // A <relative-size> keyword is interpreted relative to the computed font-size of the parent
    // element. User agents may use a simple ratio, which should be around 1.2-1.5.
    let relative = |numerator: i64, denominator: i64| {
        computed(
            (inherited_font_size * CssPixels::from_integer(numerator))
                .div_as_fraction(CssPixels::from_integer(denominator))
                .to_double(),
        )
    };
    match value {
        // <length-percentage [0,inf]>
        // A length value specifies an absolute font size (independent of the user agent's font
        // table). Negative lengths are invalid.
        StyleValueData::Length { .. } => FfiComputedNumber {
            handled: true,
            unchanged: true,
            value: 0.0,
        },
        // A percentage value specifies an absolute font size relative to the parent element's
        // computed font-size. Negative percentages are invalid.
        StyleValueData::Percentage { value } => computed(inherited_font_size.to_double() * (value / 100.0)),
        StyleValueData::Keyword { keyword } => match *keyword {
            keyword::XX_SMALL => absolute(3, 5),
            keyword::X_SMALL => absolute(3, 4),
            keyword::SMALL => absolute(8, 9),
            keyword::MEDIUM => computed(default_font_size.to_double()),
            keyword::LARGE => absolute(6, 5),
            keyword::X_LARGE => absolute(3, 2),
            keyword::XX_LARGE => computed((default_font_size * CssPixels::from_integer(2)).to_double()),
            keyword::XXX_LARGE => computed((default_font_size * CssPixels::from_integer(3)).to_double()),
            keyword::SMALLER => relative(4, 5),
            keyword::LARGER => relative(5, 4),
            // math
            // Special mathematical scaling rules must be applied when determining the computed
            // value of the font-size property.
            keyword::MATH => {
                // https://w3c.github.io/mathml-core/#the-math-script-level-property
                // If the specified value font-size is math then the computed value of font-size is
                // obtained by multiplying the inherited value of font-size by a nonzero scale
                // factor calculated by the following procedure:
                // 1. Let A be the inherited math-depth value, B the computed math-depth value,
                //    C be 0.71 and S be 1.0
                let mut a = inherited_math_depth;
                let mut b = computed_math_depth;
                let size_ratio = 0.71f64;
                let mut scale = 1.0f64;
                let math_scaling_factor = if a == b {
                    // 2. If A = B then return S.
                    scale
                } else {
                    // If B < A, swap A and B and set InvertScaleFactor to true.
                    // Otherwise B > A and set InvertScaleFactor to false.
                    let invert_scale_factor = if b < a {
                        std::mem::swap(&mut a, &mut b);
                        true
                    } else {
                        false
                    };
                    // 3. Let E be B - A > 0.
                    let e = f64::from(b - a > 0);
                    // FIXME: 4. If the inherited first available font has an OpenType MATH table:
                    //    - If A <= 0 and B >= 2 then multiply S by scriptScriptPercentScaleDown
                    //      and decrement E by 2.
                    //    - Otherwise if A = 1 then multiply S by scriptScriptPercentScaleDown /
                    //      scriptPercentScaleDown and decrement E by 1.
                    //    - Otherwise if B = 1 then multiply S by scriptPercentScaleDown and
                    //      decrement E by 1.
                    // 5. Multiply S by C^E.
                    scale *= size_ratio.powf(e);
                    // 6. Return S if InvertScaleFactor is false and 1/S otherwise.
                    if !invert_scale_factor { scale } else { 1.0 / scale }
                };
                computed(inherited_font_size.scaled(math_scaling_factor).to_double())
            }
            _ => NUMBER_UNHANDLED,
        },
        // AD-HOC: calc values are resolved by the C++ caller.
        _ => NUMBER_UNHANDLED,
    }
}

/// Computes the font-size property from its absolutized value. The font size
/// inputs are raw CSSPixels values.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_font_size(
    absolutized_value: *const c_void,
    computed_math_depth: i32,
    inherited_font_size_raw: i32,
    inherited_math_depth: i32,
    default_font_size_raw: i32,
) -> FfiComputedNumber {
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_font_size(
            value,
            computed_math_depth,
            CssPixels::from_raw(inherited_font_size_raw),
            inherited_math_depth,
            CssPixels::from_raw(default_font_size_raw),
        )
    })
}

// The computed value of the math-depth value is determined as follows:
fn compute_math_depth(
    value: &StyleValueData,
    inherited_math_depth: i32,
    inherited_math_style_is_compact: bool,
) -> FfiComputedNumber {
    let computed = |depth: i32| FfiComputedNumber {
        handled: true,
        unchanged: false,
        value: depth as f64,
    };
    match value {
        // - If the specified value of math-depth is auto-add and the inherited value of math-style
        //   is compact then the computed value of math-depth of the element is its inherited value
        //   plus one.
        StyleValueData::Keyword { keyword } if *keyword == keyword::AUTO_ADD && inherited_math_style_is_compact => {
            computed(inherited_math_depth.saturating_add(1))
        }
        // - If the specified value of math-depth is of the form <integer> then the computed value
        //   of math-depth of the element is the specified integer.
        StyleValueData::Integer { value } => computed(*value),
        // AD-HOC: add(<integer>) functions and calc values are resolved by the C++ caller.
        StyleValueData::Function { .. } | StyleValueData::Calculated { .. } => NUMBER_UNHANDLED,
        // - Otherwise, the computed value of math-depth of the element is the inherited one.
        _ => computed(inherited_math_depth),
    }
}

/// Computes the math-depth property from its absolutized value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_math_depth(
    absolutized_value: *const c_void,
    inherited_math_depth: i32,
    inherited_math_style_is_compact: bool,
) -> FfiComputedNumber {
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_math_depth(value, inherited_math_depth, inherited_math_style_is_compact)
    })
}

// https://drafts.csswg.org/css-inline-3/#line-height-property
fn compute_line_height(value: &StyleValueData, computed_font_size: CssPixels) -> FfiComputedNumber {
    match value {
        // normal
        // <length [0,inf]>
        // <number [0,inf]>
        StyleValueData::Keyword { keyword } if *keyword == keyword::NORMAL => FfiComputedNumber {
            handled: true,
            unchanged: true,
            value: 0.0,
        },
        StyleValueData::Length { .. } | StyleValueData::Number { .. } => FfiComputedNumber {
            handled: true,
            unchanged: true,
            value: 0.0,
        },
        // <percentage [0,inf]>
        StyleValueData::Percentage { value } => FfiComputedNumber {
            handled: true,
            unchanged: false,
            value: computed_font_size.to_double() * (value / 100.0),
        },
        // NB: calc lengths and numbers are resolved by the C++ caller, and any other value would
        //     be unreachable there.
        _ => NUMBER_UNHANDLED,
    }
}

/// Computes the line-height property from its absolutized value. The computed
/// font size is a raw CSSPixels value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_line_height(
    absolutized_value: *const c_void,
    computed_font_size_raw: i32,
) -> FfiComputedNumber {
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_line_height(value, CssPixels::from_raw(computed_font_size_raw))
    })
}

// https://drafts.csswg.org/css-backgrounds/#border-width
// absolute length, snapped as a border width
fn compute_border_or_outline_width(value: &StyleValueData, device_pixels_per_css_pixel: f64) -> FfiComputedNumber {
    let absolute_length = match value {
        // The thin, medium, and thick keywords are equivalent to 1px, 3px, and 5px, respectively.
        // https://drafts.csswg.org/css-backgrounds/#typedef-line-width
        StyleValueData::Keyword { keyword } => match *keyword {
            keyword::THIN => CssPixels::from_integer(1),
            keyword::MEDIUM => CssPixels::from_integer(3),
            keyword::THICK => CssPixels::from_integer(5),
            _ => return NUMBER_UNHANDLED,
        },
        StyleValueData::Length { value, unit } => {
            let kinds = length_unit_kinds();
            match kinds.get(*unit as usize) {
                Some(LengthUnitKind::Px) => CssPixels::nearest_value_for(*value),
                Some(LengthUnitKind::Absolute { px_per_unit }) => CssPixels::nearest_value_for(px_per_unit * value),
                _ => return NUMBER_UNHANDLED,
            }
        }
        _ => return NUMBER_UNHANDLED,
    };
    FfiComputedNumber {
        handled: true,
        unchanged: false,
        value: snap_a_length_as_a_border_width(device_pixels_per_css_pixel, absolute_length).to_double(),
    }
}

// https://drafts.csswg.org/css-backgrounds/#compute-a-border-width
fn snap_a_length_as_a_border_width(device_pixels_per_css_pixel: f64, length: CssPixels) -> CssPixels {
    // 1. Assert: len is non-negative.
    // NB: The caller guarantees this; negative widths are invalid at parse time.

    // 2. If len is an integer number of device pixels, do nothing.
    let device_pixels = length.to_double() * device_pixels_per_css_pixel;
    if device_pixels == device_pixels.trunc() {
        return length;
    }

    // 3. If len is greater than zero, but less than 1 device pixel, round len up to 1 device pixel.
    if device_pixels > 0.0 && device_pixels < 1.0 {
        return CssPixels::nearest_value_for(1.0 / device_pixels_per_css_pixel);
    }

    // 4. If len is greater than 1 device pixel, round it down to the nearest integer number of
    //    device pixels.
    if device_pixels > 1.0 {
        return CssPixels::nearest_value_for(device_pixels.floor() / device_pixels_per_css_pixel);
    }

    length
}

/// Computes a border or outline width property from its absolutized value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_border_or_outline_width(
    absolutized_value: *const c_void,
    device_pixels_per_css_pixel: f64,
) -> FfiComputedNumber {
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_border_or_outline_width(value, device_pixels_per_css_pixel)
    })
}

// https://drafts.csswg.org/css-borders-4/#propdef-corner-top-left-shape
// the corresponding superellipse() value
fn compute_corner_shape_parameter(value: &StyleValueData) -> FfiComputedNumber {
    let computed = |parameter: f64| FfiComputedNumber {
        handled: true,
        unchanged: false,
        value: parameter,
    };
    match value {
        StyleValueData::Keyword { keyword } => match *keyword {
            // The corner shape is a quarter of a convex ellipse. Equivalent to superellipse(1).
            keyword::ROUND => computed(1.0),
            // The corner shape is a quarter of a "squircle", a convex curve between round and
            // square. Equivalent to superellipse(2).
            keyword::SQUIRCLE => computed(2.0),
            // The corner shape is a convex 90deg angle. Equivalent to superellipse(infinity).
            keyword::SQUARE => computed(f64::INFINITY),
            // The corner shape is a straight diagonal line, neither convex nor concave.
            // Equivalent to superellipse(0).
            keyword::BEVEL => computed(0.0),
            // The corner shape is a concave quarter-ellipse. Equivalent to superellipse(-1).
            keyword::SCOOP => computed(-1.0),
            // The corner shape is a concave 90deg angle. Equivalent to superellipse(-infinity).
            keyword::NOTCH => computed(f64::NEG_INFINITY),
            _ => NUMBER_UNHANDLED,
        },
        // Superellipse values are already computed.
        StyleValueData::Superellipse { .. } => FfiComputedNumber {
            handled: true,
            unchanged: true,
            value: 0.0,
        },
        _ => NUMBER_UNHANDLED,
    }
}

/// Computes the superellipse parameter for a corner shape property.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_corner_shape_parameter(absolutized_value: *const c_void) -> FfiComputedNumber {
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_corner_shape_parameter(value)
    })
}

// https://drafts.csswg.org/css-anchor-position/#position-area-computed
// The computed value of a <position-area> value is the two keywords indicating the selected
// tracks in each axis, with the long (block-start) and short (start) logical keywords treated
// as equivalent. It serializes with the logical keywords in their short forms.
#[unsafe(no_mangle)]
pub extern "C" fn rust_position_area_short_keyword(keyword: u16) -> u16 {
    match keyword {
        keyword::BLOCK_START | keyword::INLINE_START => keyword::START,
        keyword::BLOCK_END | keyword::INLINE_END => keyword::END,
        keyword::SELF_BLOCK_START | keyword::SELF_INLINE_START => keyword::SELF_START,
        keyword::SELF_BLOCK_END | keyword::SELF_INLINE_END => keyword::SELF_END,
        keyword::SPAN_BLOCK_START | keyword::SPAN_INLINE_START => keyword::SPAN_START,
        keyword::SPAN_BLOCK_END | keyword::SPAN_INLINE_END => keyword::SPAN_END,
        keyword::SPAN_SELF_BLOCK_START | keyword::SPAN_SELF_INLINE_START => keyword::SPAN_SELF_START,
        keyword::SPAN_SELF_BLOCK_END | keyword::SPAN_SELF_INLINE_END => keyword::SPAN_SELF_END,
        _ => keyword,
    }
}

/// The inherit-or-initial decision for one longhand in the property
/// computation loop.
#[repr(C)]
pub struct FfiLonghandDecision {
    pub should_inherit: bool,
    pub explicitly_inherits_non_inherited_property: bool,
    /// True when, absent a successful inheritance fetch, the value falls back
    /// to the property's initial value.
    pub use_initial_without_inherit: bool,
}

fn longhand_decision(value: Option<&StyleValueData>, property_id: u16) -> FfiLonghandDecision {
    let keyword = match value {
        Some(StyleValueData::Keyword { keyword }) => Some(*keyword),
        _ => None,
    };
    let is_inherit = keyword == Some(keyword::INHERIT);
    let is_unset = keyword == Some(keyword::UNSET);
    let is_initial = keyword == Some(keyword::INITIAL);
    let inherited_property = property_is_inherited(property_id);

    let explicitly_inherits_non_inherited_property = is_inherit && !inherited_property;
    let mut should_inherit = value.is_none() && inherited_property;

    // https://www.w3.org/TR/css-cascade-4/#inherit
    // If the cascaded value of a property is the inherit keyword, the property's specified and
    // computed values are the inherited value.
    should_inherit |= is_inherit;

    // https://www.w3.org/TR/css-cascade-4/#inherit-initial
    // If the cascaded value of a property is the unset keyword, then if it is an inherited
    // property, this is treated as inherit, and if it is not, this is treated as initial.
    should_inherit |= is_unset && inherited_property;

    // https://www.w3.org/TR/css-color-4/#resolving-other-colors
    // In the color property, the used value of currentcolor is the resolved inherited value.
    should_inherit |=
        property_id == crate::property_metadata::property_id::COLOR && keyword == Some(keyword::CURRENTCOLOR);

    FfiLonghandDecision {
        should_inherit,
        explicitly_inherits_non_inherited_property,
        use_initial_without_inherit: value.is_none() || is_initial || is_unset || should_inherit,
    }
}

/// Decides how the cascaded value of a longhand resolves against inheritance
/// and the initial value.
///
/// # Safety
/// `cascaded_value` must be null or point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_longhand_decision(
    cascaded_value: *const c_void,
    property_id: u16,
) -> FfiLonghandDecision {
    abort_on_panic(|| {
        let value = if cascaded_value.is_null() {
            None
        } else {
            Some(unsafe { &*(cascaded_value as *const StyleValueData) })
        };
        longhand_decision(value, property_id)
    })
}

/// The logical-alias-to-physical-property mapping, marshalled once from the
/// C++ generated mapping function so the two sides cannot drift: for each
/// longhand, one physical property id per (writing-mode, direction) pair, with
/// zero marking properties that are not logical aliases.
pub const WRITING_MODE_COUNT: usize = 5;
pub const DIRECTION_COUNT: usize = 2;

static LOGICAL_ALIAS_TABLE: std::sync::OnceLock<Vec<u16>> = std::sync::OnceLock::new();

/// Installs the logical alias mapping table. `table` holds one entry per
/// (longhand, writing-mode, direction) triple in row-major order.
///
/// # Safety
/// `table` must point at `longhand_count * WRITING_MODE_COUNT * DIRECTION_COUNT` entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_metadata_set_logical_alias_table(table: *const u16, length: usize) {
    abort_on_panic(|| {
        let entries = unsafe { std::slice::from_raw_parts(table, length) }.to_vec();
        assert!(
            LOGICAL_ALIAS_TABLE.set(entries).is_ok(),
            "logical alias table installed twice"
        );
    });
}

/// Maps a logical alias longhand to its physical property for the given
/// writing mode and direction, or returns the property itself when it is not
/// a logical alias.
pub fn map_logical_alias_to_physical(property_id: u16, writing_mode: u8, direction: u8) -> u16 {
    use crate::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let Some(table) = LOGICAL_ALIAS_TABLE.get() else {
        return property_id;
    };
    let longhand_index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    let index = (longhand_index * WRITING_MODE_COUNT + writing_mode as usize) * DIRECTION_COUNT + direction as usize;
    match table.get(index) {
        Some(&physical) if physical != 0 => physical,
        _ => property_id,
    }
}

/// FFI accessor for the parity test on the C++ side.
#[unsafe(no_mangle)]
pub extern "C" fn rust_map_logical_alias_to_physical(property_id: u16, writing_mode: u8, direction: u8) -> u16 {
    map_logical_alias_to_physical(property_id, writing_mode, direction)
}

static PHYSICAL_TO_LOGICAL_TABLE: std::sync::OnceLock<Vec<u16>> = std::sync::OnceLock::new();

/// Installs the physical-to-logical-alias mapping table, in the same layout as
/// the logical alias table.
///
/// # Safety
/// `table` must point at `longhand_count * WRITING_MODE_COUNT * DIRECTION_COUNT` entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_metadata_set_physical_to_logical_table(table: *const u16, length: usize) {
    abort_on_panic(|| {
        let entries = unsafe { std::slice::from_raw_parts(table, length) }.to_vec();
        assert!(
            PHYSICAL_TO_LOGICAL_TABLE.set(entries).is_ok(),
            "physical to logical table installed twice"
        );
    });
}

/// Maps a physical longhand to its logical alias for the given writing mode
/// and direction, or returns the property itself when it has no logical alias.
pub fn map_physical_to_logical_alias(property_id: u16, writing_mode: u8, direction: u8) -> u16 {
    use crate::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let Some(table) = PHYSICAL_TO_LOGICAL_TABLE.get() else {
        return property_id;
    };
    let longhand_index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    let index = (longhand_index * WRITING_MODE_COUNT + writing_mode as usize) * DIRECTION_COUNT + direction as usize;
    match table.get(index) {
        Some(&logical) if logical != 0 => logical,
        _ => property_id,
    }
}

/// FFI accessor for the parity test on the C++ side.
#[unsafe(no_mangle)]
pub extern "C" fn rust_map_physical_to_logical_alias(property_id: u16, writing_mode: u8, direction: u8) -> u16 {
    map_physical_to_logical_alias(property_id, writing_mode, direction)
}

/// Drives the property computation loop: iterates every longhand in
/// computation order and calls back into C++ for the per-property work that
/// has not moved into the core yet.
///
/// # Safety
/// `process_longhand` must be callable with `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_drive_property_computation(
    context: *mut c_void,
    process_longhand: unsafe extern "C" fn(context: *mut c_void, property_id: u16),
) {
    abort_on_panic(|| {
        for &property_id in crate::property_metadata::property_computation_order() {
            unsafe { process_longhand(context, property_id) };
        }
    });
}

/// Computes the font-weight property from its absolutized value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_font_weight(
    absolutized_value: *const c_void,
    inherited_font_weight: f64,
) -> FfiComputedNumber {
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_font_weight(value, inherited_font_weight)
    })
}

// The exported computed-values FFI shares one header; keep an anchor so the
// context types stay in the generated bindings even without other references.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_compute_context_anchor(_context: *const c_void) {}

// The standalone cargo test binary has no C++ side, so the release callbacks
// that StyleValueData's retained members call on drop are stubbed out here.
#[cfg(test)]
mod ffi_test_stubs {
    use std::ffi::c_void;

    #[unsafe(no_mangle)]
    extern "C" fn ladybird_style_value_unref(_style_value: *const c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_utf16_fly_string_unref(_raw: usize) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_string_unref(_raw: usize) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_calculation_node_unref(_node: *const c_void) {}
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_context() -> FfiLengthResolutionContext {
        FfiLengthResolutionContext {
            viewport_width: 800.0,
            viewport_height: 600.0,
            font_metrics: FfiFontMetrics {
                font_size: 16.0,
                x_height: 8.0,
                cap_height: 11.0,
                zero_advance: 7.5,
                line_height: 19.0,
            },
            root_font_metrics: FfiFontMetrics {
                font_size: 20.0,
                x_height: 10.0,
                cap_height: 14.0,
                zero_advance: 9.0,
                line_height: 24.0,
            },
            font_metrics_depend_on_viewport_metrics: false,
            root_font_metrics_depend_on_viewport_metrics: true,
        }
    }

    fn unit_code(name: &str) -> usize {
        LENGTH_UNIT_NAMES.iter().position(|&n| n == name).unwrap()
    }

    #[test]
    fn px_is_unchanged() {
        let result = absolutize_length(4.0, unit_code("px"), &test_context());
        assert!(result.handled);
        assert!(!result.changed);
    }

    #[test]
    fn font_relative_units_resolve() {
        let context = test_context();
        let em = absolutize_length(2.0, unit_code("em"), &context);
        assert!(em.changed);
        assert_eq!(em.px, 32.0);
        assert!(!em.resolved_viewport_relative_length);

        let rem = absolutize_length(2.0, unit_code("rem"), &context);
        assert_eq!(rem.px, 40.0);
        assert!(rem.resolved_viewport_relative_length);

        assert_eq!(absolutize_length(1.0, unit_code("lh"), &context).px, 19.0);
        assert_eq!(absolutize_length(1.0, unit_code("rch"), &context).px, 9.0);
    }

    #[test]
    fn viewport_relative_units_resolve() {
        let context = test_context();
        let vw = absolutize_length(50.0, unit_code("vw"), &context);
        assert_eq!(vw.px, 400.0);
        assert!(vw.resolved_viewport_relative_length);
        assert_eq!(absolutize_length(50.0, unit_code("vmin"), &context).px, 300.0);
        assert_eq!(absolutize_length(50.0, unit_code("dvmax"), &context).px, 400.0);
    }

    #[test]
    fn absolute_units_resolve() {
        let context = test_context();
        let inch = absolutize_length(1.0, unit_code("in"), &context);
        assert!(inch.changed);
        assert_eq!(inch.px, 96.0);
    }

    #[test]
    fn font_weight_keywords_compute() {
        let bolder = StyleValueData::Keyword {
            keyword: keyword::BOLDER,
        };
        assert_eq!(compute_font_weight(&bolder, 300.0).value, 400.0);
        assert_eq!(compute_font_weight(&bolder, 400.0).value, 700.0);
        assert_eq!(compute_font_weight(&bolder, 700.0).value, 900.0);
        assert_eq!(compute_font_weight(&bolder, 900.0).value, 900.0);

        let lighter = StyleValueData::Keyword {
            keyword: keyword::LIGHTER,
        };
        assert_eq!(compute_font_weight(&lighter, 50.0).value, 50.0);
        assert_eq!(compute_font_weight(&lighter, 400.0).value, 100.0);
        assert_eq!(compute_font_weight(&lighter, 700.0).value, 400.0);
        assert_eq!(compute_font_weight(&lighter, 900.0).value, 700.0);

        assert_eq!(
            compute_font_weight(
                &StyleValueData::Keyword {
                    keyword: keyword::NORMAL
                },
                700.0
            )
            .value,
            400.0
        );
        assert_eq!(
            compute_font_weight(&StyleValueData::Keyword { keyword: keyword::BOLD }, 100.0).value,
            700.0
        );
        assert!(compute_font_weight(&StyleValueData::Number { value: 512.0 }, 100.0).unchanged);
    }

    #[test]
    fn font_width_keywords_compute() {
        assert_eq!(
            compute_font_width(&StyleValueData::Keyword {
                keyword: keyword::ULTRA_CONDENSED
            })
            .value,
            50.0
        );
        assert_eq!(
            compute_font_width(&StyleValueData::Keyword {
                keyword: keyword::SEMI_EXPANDED
            })
            .value,
            112.5
        );
        assert_eq!(
            compute_font_width(&StyleValueData::Keyword {
                keyword: keyword::NORMAL
            })
            .value,
            100.0
        );
        assert!(compute_font_width(&StyleValueData::Percentage { value: 80.0 }).unchanged);
    }

    #[test]
    fn font_size_keywords_compute() {
        let sixteen = CssPixels::from_integer(16);
        let medium = compute_font_size(
            &StyleValueData::Keyword {
                keyword: keyword::MEDIUM,
            },
            0,
            sixteen,
            0,
            sixteen,
        );
        assert_eq!(medium.value, 16.0);
        let large = compute_font_size(
            &StyleValueData::Keyword {
                keyword: keyword::LARGE,
            },
            0,
            sixteen,
            0,
            sixteen,
        );
        assert_eq!(large.value, 19.1875); // 16 * 6 / 5 in 6-bit fixed point
        let percent = compute_font_size(&StyleValueData::Percentage { value: 150.0 }, 0, sixteen, 0, sixteen);
        assert_eq!(percent.value, 24.0);
        let math = compute_font_size(
            &StyleValueData::Keyword { keyword: keyword::MATH },
            1,
            sixteen,
            0,
            sixteen,
        );
        assert_eq!(math.value, CssPixels::from_integer(16).scaled(0.71).to_double());
    }

    #[test]
    fn math_depth_computes() {
        let auto_add = StyleValueData::Keyword {
            keyword: keyword::AUTO_ADD,
        };
        assert_eq!(compute_math_depth(&auto_add, 2, true).value, 3.0);
        assert_eq!(compute_math_depth(&auto_add, 2, false).value, 2.0);
        assert_eq!(
            compute_math_depth(&StyleValueData::Integer { value: 5 }, 2, true).value,
            5.0
        );
        assert_eq!(
            compute_math_depth(&StyleValueData::Keyword { keyword: keyword::AUTO }, 4, true).value,
            4.0
        );
    }

    #[test]
    fn line_height_computes() {
        let sixteen = CssPixels::from_integer(16);
        assert!(
            compute_line_height(
                &StyleValueData::Keyword {
                    keyword: keyword::NORMAL
                },
                sixteen
            )
            .unchanged
        );
        assert!(compute_line_height(&StyleValueData::Number { value: 1.5 }, sixteen).unchanged);
        assert_eq!(
            compute_line_height(&StyleValueData::Percentage { value: 150.0 }, sixteen).value,
            24.0
        );
    }

    #[test]
    fn border_width_snaps() {
        // 2.5px at 2 dppx is an integer number of device pixels: unchanged.
        let length = StyleValueData::Length {
            value: 2.5,
            unit: unit_code("px") as u8,
        };
        assert_eq!(compute_border_or_outline_width(&length, 2.0).value, 2.5);
        // 0.4px at 1 dppx rounds up to 1 device pixel.
        let thin = StyleValueData::Length {
            value: 0.4,
            unit: unit_code("px") as u8,
        };
        assert_eq!(compute_border_or_outline_width(&thin, 1.0).value, 1.0);
        // medium is 3px.
        assert_eq!(
            compute_border_or_outline_width(
                &StyleValueData::Keyword {
                    keyword: keyword::MEDIUM
                },
                1.0
            )
            .value,
            3.0
        );
    }

    #[test]
    fn corner_shapes_map_to_superellipse_parameters() {
        assert_eq!(
            compute_corner_shape_parameter(&StyleValueData::Keyword {
                keyword: keyword::ROUND
            })
            .value,
            1.0
        );
        assert_eq!(
            compute_corner_shape_parameter(&StyleValueData::Keyword {
                keyword: keyword::NOTCH
            })
            .value,
            f64::NEG_INFINITY
        );
    }

    #[test]
    fn longhand_decisions() {
        use crate::property_metadata::{FIRST_INHERITED_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
        let inherited_id = FIRST_INHERITED_PROPERTY_ID;
        let reset_id = LAST_LONGHAND_PROPERTY_ID;

        // Missing value: inherited properties inherit, reset properties go initial.
        let missing_inherited = longhand_decision(None, inherited_id);
        assert!(missing_inherited.should_inherit);
        let missing_reset = longhand_decision(None, reset_id);
        assert!(!missing_reset.should_inherit);
        assert!(missing_reset.use_initial_without_inherit);

        // Explicit inherit on a reset property is flagged.
        let inherit = StyleValueData::Keyword {
            keyword: keyword::INHERIT,
        };
        let explicit = longhand_decision(Some(&inherit), reset_id);
        assert!(explicit.should_inherit);
        assert!(explicit.explicitly_inherits_non_inherited_property);

        // unset: inherit for inherited properties, initial for reset ones.
        let unset = StyleValueData::Keyword {
            keyword: keyword::UNSET,
        };
        assert!(longhand_decision(Some(&unset), inherited_id).should_inherit);
        let unset_reset = longhand_decision(Some(&unset), reset_id);
        assert!(!unset_reset.should_inherit);
        assert!(unset_reset.use_initial_without_inherit);

        // currentcolor in the color property inherits.
        let currentcolor = StyleValueData::Keyword {
            keyword: keyword::CURRENTCOLOR,
        };
        assert!(longhand_decision(Some(&currentcolor), crate::property_metadata::property_id::COLOR).should_inherit);
        assert!(!longhand_decision(Some(&currentcolor), reset_id).should_inherit);
    }

    #[test]
    fn container_relative_units_are_unhandled() {
        assert!(!absolutize_length(1.0, unit_code("cqw"), &test_context()).handled);
    }
}
