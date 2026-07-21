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
use crate::cascaded_properties::CascadedPropertyStore;
use crate::css_pixels::CssPixels;
use crate::property_metadata::longhands_for_shorthand;
use crate::property_metadata::property_is_inherited;
use crate::property_metadata::property_is_shorthand;
use crate::style_value::StyleValueData;

include!(concat!(env!("OUT_DIR"), "/length_units_generated.rs"));
include!(concat!(env!("OUT_DIR"), "/keywords_generated.rs"));
include!(concat!(env!("OUT_DIR"), "/css_enums_generated.rs"));

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

pub(crate) fn px_length_unit() -> u8 {
    static PX: OnceLock<u8> = OnceLock::new();
    *PX.get_or_init(|| LENGTH_UNIT_NAMES.iter().position(|&name| name == "px").unwrap() as u8)
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

/// Whether a length unit is font-relative or container-relative, the two
/// relativities that make a length depend on more than global information.
pub(crate) fn length_unit_is_font_or_container_relative(unit: u8) -> bool {
    matches!(
        length_unit_kinds().get(unit as usize),
        Some(LengthUnitKind::FontRelative { .. } | LengthUnitKind::ContainerRelative)
    )
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

/// Crate-visible length absolutization for the calc evaluation, which needs
/// the unrounded pixel result.
pub(crate) fn absolutize_length_for_calc(
    value: f64,
    unit: usize,
    context: &FfiLengthResolutionContext,
) -> FfiAbsolutizedLength {
    absolutize_length(value, unit, context)
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
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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

/// Result of computing line-height: like FfiComputedNumber, but a resolved
/// calc may produce either a pixel length or a unitless number multiplier.
#[repr(C)]
pub struct FfiComputedLineHeight {
    /// False when the value needs C++ handling (calc the core cannot resolve).
    pub handled: bool,
    /// True when the absolutized value is already the computed value.
    pub unchanged: bool,
    /// True when the value is a unitless multiplier rather than pixels.
    pub is_number: bool,
    pub value: f64,
}

const LINE_HEIGHT_UNHANDLED: FfiComputedLineHeight = FfiComputedLineHeight {
    handled: false,
    unchanged: false,
    is_number: false,
    value: 0.0,
};

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
        // Calc values resolve in the calc core with no external context; anything the
        // core cannot resolve keeps the C++ caller's behavior.
        StyleValueData::Calculated { .. } => match crate::calc::resolve_calculated_number_without_context(value) {
            Some(resolved) => computed(resolved),
            None => NUMBER_UNHANDLED,
        },
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
        // Calc percentages resolve in the calc core with no external context; anything
        // the core cannot resolve keeps the C++ caller's behavior.
        StyleValueData::Calculated { .. } => match crate::calc::resolve_calculated_percentage_without_context(value) {
            Some(resolved) => computed(resolved),
            None => NUMBER_UNHANDLED,
        },
        _ => NUMBER_UNHANDLED,
    }
}

/// Computes the font-width property from its absolutized value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_font_width(absolutized_value: *const c_void) -> FfiComputedNumber {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_font_width(value)
    })
}

/// An <absolute-size> keyword refers to an entry in a table of font sizes computed and kept by
/// the user agent. See 2.5.1 Absolute Size Keyword Mapping Table.
fn absolute_size_font_mapping(keyword: u16, default_font_size: CssPixels) -> Option<CssPixels> {
    let entry = |numerator: i64, denominator: i64| {
        Some(
            (default_font_size * CssPixels::from_integer(numerator))
                .div_as_fraction(CssPixels::from_integer(denominator)),
        )
    };
    match keyword {
        keyword::XX_SMALL => entry(3, 5),
        keyword::X_SMALL => entry(3, 4),
        keyword::SMALL => entry(8, 9),
        keyword::MEDIUM => Some(default_font_size),
        keyword::LARGE => entry(6, 5),
        keyword::X_LARGE => entry(3, 2),
        keyword::XX_LARGE => Some(default_font_size * CssPixels::from_integer(2)),
        keyword::XXX_LARGE => Some(default_font_size * CssPixels::from_integer(3)),
        _ => None,
    }
}

/// A <relative-size> keyword is interpreted relative to the computed font-size of the parent
/// element. User agents may use a simple ratio, which should be around 1.2-1.5.
fn relative_size_font_mapping(keyword: u16, inherited_font_size: CssPixels) -> Option<CssPixels> {
    let entry = |numerator: i64, denominator: i64| {
        Some(
            (inherited_font_size * CssPixels::from_integer(numerator))
                .div_as_fraction(CssPixels::from_integer(denominator)),
        )
    };
    match keyword {
        keyword::SMALLER => entry(4, 5),
        keyword::LARGER => entry(5, 4),
        _ => None,
    }
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
        StyleValueData::Keyword { keyword } => {
            if let Some(px) = absolute_size_font_mapping(*keyword, default_font_size) {
                return computed(px.to_double());
            }
            if let Some(px) = relative_size_font_mapping(*keyword, inherited_font_size) {
                return computed(px.to_double());
            }
            match *keyword {
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
                        let e = f64::from(b) - f64::from(a);
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
            }
        }
        // Calc lengths and percentages resolve in the calc core against the inherited
        // font size; anything the core cannot resolve keeps the C++ caller's behavior.
        StyleValueData::Calculated { .. } => {
            match crate::calc::resolve_calculated_length_without_context(value, inherited_font_size.to_double()) {
                Some(px) => computed(px),
                None => NUMBER_UNHANDLED,
            }
        }
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
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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

/// What one step of the monospace font-size time travel decided.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FontSizeRecascadeAction {
    /// Keep the current size and viewport-dependency flag.
    Unchanged,
    /// Replace the current size and viewport-dependency flag.
    Set,
    /// The value is a calc, which the traversal does not support yet; skip it.
    CalcSkipped,
    /// The value is a length the core cannot resolve, either because no
    /// resolution context was supplied or because the unit is unsupported;
    /// the caller resolves it.
    NeedsLengthResolution,
}

#[repr(C)]
pub struct FfiFontSizeRecascadeStep {
    pub action: FontSizeRecascadeAction,
    pub new_size_raw: i32,
    pub depends_on_viewport_metrics: bool,
}

/// One ancestor step of the time-traveling font-size inheritance applied when
/// the cascade ends up with `font-family: monospace`: interprets the
/// ancestor's raw cascaded font-size against the running size, with keyword
/// sizes mapped against the default monospace font size.
///
/// `length_resolution_context` may be null; a length value then reports
/// `NeedsLengthResolution` so the caller can build the context lazily and call
/// again, since building it involves font work the other value types never
/// need.
///
/// # Safety
/// `value` must point at a valid StyleValueData, and
/// `length_resolution_context` must be null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_recascade_font_size_step(
    value: *const c_void,
    current_size_raw: i32,
    current_depends_on_viewport_metrics: bool,
    default_size_raw: i32,
    length_resolution_context: *const FfiLengthResolutionContext,
) -> FfiFontSizeRecascadeStep {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(value as *const StyleValueData) };
        let current_size = CssPixels::from_raw(current_size_raw);
        let default_size = CssPixels::from_raw(default_size_raw);
        let unchanged = FfiFontSizeRecascadeStep {
            action: FontSizeRecascadeAction::Unchanged,
            new_size_raw: current_size_raw,
            depends_on_viewport_metrics: current_depends_on_viewport_metrics,
        };
        let set = |size: CssPixels, depends_on_viewport_metrics: bool| FfiFontSizeRecascadeStep {
            action: FontSizeRecascadeAction::Set,
            new_size_raw: size.raw_value(),
            depends_on_viewport_metrics,
        };
        let needs_length_resolution = FfiFontSizeRecascadeStep {
            action: FontSizeRecascadeAction::NeedsLengthResolution,
            new_size_raw: current_size_raw,
            depends_on_viewport_metrics: current_depends_on_viewport_metrics,
        };

        match value {
            StyleValueData::Keyword { keyword } => {
                if *keyword == keyword::INITIAL || *keyword == keyword::UNSET {
                    return set(default_size, false);
                }
                if *keyword == keyword::INHERIT {
                    // Do nothing.
                    return unchanged;
                }
                if let Some(px) = absolute_size_font_mapping(*keyword, default_size) {
                    return set(px, false);
                }
                if let Some(px) = relative_size_font_mapping(*keyword, current_size) {
                    return set(px, current_depends_on_viewport_metrics);
                }
                // FIXME: Resolve `font-size: math`
                if *keyword == keyword::MATH {
                    return unchanged;
                }
                // Unknown keywords fall through to the caller, which rejects them.
                needs_length_resolution
            }
            StyleValueData::Percentage { value } => set(
                CssPixels::nearest_value_for(value / 100.0 * current_size.to_double()),
                current_depends_on_viewport_metrics,
            ),
            StyleValueData::Length { value, unit } => {
                if length_resolution_context.is_null() {
                    return needs_length_resolution;
                }
                let result = absolutize_length(*value, *unit as usize, unsafe { &*length_resolution_context });
                if !result.handled {
                    return needs_length_resolution;
                }
                set(
                    CssPixels::nearest_value_for(result.px),
                    result.resolved_viewport_relative_length,
                )
            }
            StyleValueData::Calculated { .. } => FfiFontSizeRecascadeStep {
                action: FontSizeRecascadeAction::CalcSkipped,
                new_size_raw: current_size_raw,
                depends_on_viewport_metrics: current_depends_on_viewport_metrics,
            },
            _ => needs_length_resolution,
        }
    })
}

/// Some pseudo-elements are generated regardless of CSS rules, so their
/// styles must be computed even when no rules matched.
#[unsafe(no_mangle)]
pub extern "C" fn rust_pseudo_element_has_implicit_style(pseudo_element: u8) -> bool {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    use crate::selector_engine::PseudoElementType;
    abort_on_panic(|| {
        matches!(
            crate::selector_engine::pseudo_element_type_from_code(pseudo_element),
            PseudoElementType::DetailsContent
                | PseudoElementType::FileSelectorButton
                | PseudoElementType::Marker
                | PseudoElementType::Placeholder
        )
    })
}

/// Whether style computation for a pseudo-element bails because no
/// pseudo-element box would be generated for the winning cascaded content
/// value: content: none generates nothing, and content: normal (also the
/// initial value, so an absent value counts) generates nothing for ::before
/// and ::after.
///
/// # Safety
/// `content_value` must be null or point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_pseudo_element_content_bails(content_value: *const c_void, pseudo_element: u8) -> bool {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    use crate::selector_engine::PseudoElementType;
    abort_on_panic(|| {
        let content_is_normal = if content_value.is_null() {
            // NOTE: `normal` is the initial value, so the absence of a value is treated as `normal`.
            true
        } else {
            match unsafe { &*(content_value as *const StyleValueData) } {
                StyleValueData::Keyword { keyword } => {
                    if *keyword == keyword::NONE {
                        return true;
                    }
                    *keyword == keyword::NORMAL
                }
                _ => false,
            }
        };
        content_is_normal
            && matches!(
                crate::selector_engine::pseudo_element_type_from_code(pseudo_element),
                PseudoElementType::Before | PseudoElementType::After
            )
    })
}

/// Whether a value is computationally independent, when the decision is
/// available in the core; `handled` is false for value types whose rule still
/// lives with their C++ shells.
#[repr(C)]
pub struct FfiIndependenceDecision {
    pub handled: bool,
    pub independent: bool,
}

/// https://drafts.css-houdini.org/css-properties-values-api/#computationally-independent
/// A property value is computationally independent if it can be converted into a computed value
/// using only the value of the property on the element, and "global" information that cannot be
/// changed by CSS.
///
/// Returns None for value types whose rule still lives with their C++ shells; a container is
/// also undecided when any of its nested values is, so the whole tree falls back.
fn value_is_computationally_independent(
    value: &StyleValueData,
    data_of: unsafe extern "C" fn(*const c_void) -> *const c_void,
    decide_fallback: unsafe extern "C" fn(*const c_void) -> bool,
) -> Option<bool> {
    use crate::style_value::RetainedStyleValue;
    // An absent nested value never makes its container dependent. A nested value the core
    // cannot decide is decided by the C++ fallback in place, so containers never go
    // unhandled; only root values fall back.
    let child = |retained: &RetainedStyleValue| -> Option<bool> {
        let shell = retained.shell_pointer();
        if shell.is_null() {
            return Some(true);
        }
        let data = unsafe { data_of(shell) };
        match value_is_computationally_independent(
            unsafe { &*(data as *const StyleValueData) },
            data_of,
            decide_fallback,
        ) {
            Some(independent) => Some(independent),
            None => Some(unsafe { decide_fallback(shell) }),
        }
    };
    let all_of = |children: &[&RetainedStyleValue]| -> Option<bool> {
        let mut independent = true;
        for retained in children {
            independent = independent && child(retained)?;
        }
        Some(independent)
    };
    let all_in_list = |list: &crate::style_value::RetainedStyleValueList| -> Option<bool> {
        let mut independent = true;
        for retained in list.as_slice() {
            independent = independent && child(retained)?;
        }
        Some(independent)
    };
    match value {
        StyleValueData::Keyword { keyword } => {
            if value_is_css_wide_keyword(value) {
                return Some(false);
            }
            // FIXME: Are there any other color keywords which aren't computationally independent?
            if *keyword == keyword::ACCENTCOLOR || *keyword == keyword::ACCENTCOLORTEXT {
                return Some(false);
            }
            // FIXME: Are there any other keywords which aren't computationally independent?
            Some(true)
        }
        StyleValueData::Length { unit, .. } => Some(!length_unit_is_font_or_container_relative(*unit)),
        StyleValueData::Number { .. }
        | StyleValueData::Integer { .. }
        | StyleValueData::Percentage { .. }
        | StyleValueData::Angle { .. }
        | StyleValueData::Flex { .. }
        | StyleValueData::Frequency { .. }
        | StyleValueData::Resolution { .. }
        | StyleValueData::Time { .. }
        | StyleValueData::String { .. } => Some(true),
        // NB: anchor() and anchor-size() count as independent even though they carry a
        //     fallback value, matching their C++ rules.
        StyleValueData::Anchor { .. }
        | StyleValueData::AnchorSize { .. }
        | StyleValueData::ColorScheme { .. }
        | StyleValueData::CounterStyle { .. }
        | StyleValueData::CustomIdent { .. }
        | StyleValueData::Display { .. }
        | StyleValueData::EmptyOptional
        | StyleValueData::GridAutoFlow { .. }
        | StyleValueData::GridTemplateArea { .. }
        | StyleValueData::Image { .. }
        | StyleValueData::RepeatStyle { .. }
        | StyleValueData::TextUnderlinePosition { .. }
        | StyleValueData::Url { .. }
        | StyleValueData::FontSource { .. }
        | StyleValueData::ScrollbarGutter { .. } => Some(true),
        StyleValueData::LightDark { .. } | StyleValueData::TreeCountingFunction { .. } => Some(false),
        // The calculation tree decides natively; its style value leaves resolve through the
        // same decision, falling back for the types the core cannot decide.
        StyleValueData::Calculated { rust_calculation, .. } => {
            Some(rust_calculation.node().is_computationally_independent(
                &|unit| !length_unit_is_font_or_container_relative(unit),
                &|retained| {
                    let shell = retained.shell_pointer();
                    if shell.is_null() {
                        return true;
                    }
                    let data = unsafe { data_of(shell) };
                    match value_is_computationally_independent(
                        unsafe { &*(data as *const StyleValueData) },
                        data_of,
                        decide_fallback,
                    ) {
                        Some(independent) => independent,
                        None => unsafe { decide_fallback(shell) },
                    }
                },
            ))
        }
        StyleValueData::Ratio {
            numerator, denominator, ..
        } => all_of(&[numerator, denominator]),
        StyleValueData::Edge { offset, .. } => child(offset),
        StyleValueData::Function { value, .. } => child(value),
        StyleValueData::OpacityValue { value } => child(value),
        // Auto placements carry no value; spans and lines recurse into theirs.
        StyleValueData::GridTrackPlacement { value, .. } => child(value),
        // FIXME: Consider sub-values once we support <custom-color-space> values
        StyleValueData::ColorInterpolationMethod { .. } => Some(true),
        StyleValueData::ColorFunction {
            channel_0,
            channel_1,
            channel_2,
            alpha,
            origin_color,
            ..
        } => all_of(&[channel_0, channel_1, channel_2, alpha, origin_color]),
        StyleValueData::BorderImageSlice {
            top,
            right,
            bottom,
            left,
            ..
        } => all_of(&[top, right, bottom, left]),
        StyleValueData::Content { content, alt_text } => all_of(&[content, alt_text]),
        // Extent components carry no value; explicit sizes recurse into theirs.
        StyleValueData::RadialSize { value_0, value_1, .. } => all_of(&[value_0, value_1]),
        // Every shape kind's rule is a conjunction over the values it uses; the
        // unused generic fields and point list of the other kinds are absent, so
        // one null-tolerant conjunction covers inset, xywh, rect, circle,
        // ellipse, polygon and path exactly.
        StyleValueData::BasicShape {
            v0,
            v1,
            v2,
            v3,
            v4,
            points,
            ..
        } => {
            let mut independent = all_of(&[v0, v1, v2, v3, v4])?;
            for point in points.as_slice() {
                independent = independent && all_of(&point.values())?;
            }
            Some(independent)
        }
        // Every filter kind's rule recurses into its single value.
        StyleValueData::Filter { value, .. } => child(value),
        StyleValueData::Counter { counter_style, .. } => child(counter_style),
        StyleValueData::OpenTypeTagged { value, .. } => child(value),
        StyleValueData::RandomValueSharing { fixed_value, .. } => child(fixed_value),
        StyleValueData::Cursor { image, x, y } => all_of(&[image, x, y]),
        // The unused fields of the non-matching easing kinds are absent, so one
        // null-tolerant conjunction covers every kind's rule.
        StyleValueData::Easing {
            linear_stops,
            x1,
            y1,
            x2,
            y2,
            number_of_intervals,
            ..
        } => {
            let mut independent = all_of(&[x1, y1, x2, y2, number_of_intervals])?;
            for stop in linear_stops.as_slice() {
                independent = independent && all_of(&stop.values())?;
            }
            Some(independent)
        }
        StyleValueData::ImageSet { options } => {
            let mut independent = true;
            for option in options.as_slice() {
                independent = independent && all_of(&option.values())?;
            }
            Some(independent)
        }
        StyleValueData::CounterDefinitions { counter_definitions } => {
            let mut independent = true;
            for definition in counter_definitions.as_slice() {
                independent = independent && child(definition.value())?;
            }
            Some(independent)
        }
        StyleValueData::LinearGradient {
            direction_value,
            color_stop_list,
            color_interpolation_method,
            ..
        } => {
            let mut independent = child(direction_value)? && child(color_interpolation_method)?;
            for stop in color_stop_list.as_slice() {
                independent = independent && all_of(&stop.values())?;
            }
            Some(independent)
        }
        StyleValueData::ConicGradient {
            from_angle,
            position,
            color_stop_list,
            color_interpolation_method,
            ..
        } => {
            let mut independent = child(from_angle)? && child(position)? && child(color_interpolation_method)?;
            for stop in color_stop_list.as_slice() {
                independent = independent && all_of(&stop.values())?;
            }
            Some(independent)
        }
        StyleValueData::RadialGradient {
            size,
            position,
            color_stop_list,
            color_interpolation_method,
            ..
        } => {
            let mut independent = child(size)? && child(position)? && child(color_interpolation_method)?;
            for stop in color_stop_list.as_slice() {
                independent = independent && all_of(&stop.values())?;
            }
            Some(independent)
        }
        StyleValueData::ContrastColor { color, .. } => child(color),
        StyleValueData::Superellipse { parameter } => child(parameter),
        StyleValueData::ScrollbarColor {
            thumb_color,
            track_color,
            ..
        } => all_of(&[thumb_color, track_color]),
        StyleValueData::Rect {
            top,
            right,
            bottom,
            left,
            ..
        } => all_of(&[top, right, bottom, left]),
        StyleValueData::FontStyle { angle_value, .. } => child(angle_value),
        StyleValueData::TextIndent { length_percentage, .. } => child(length_percentage),
        StyleValueData::OverflowClipMargin { offset, .. } => child(offset),
        StyleValueData::BackgroundSize { size_x, size_y, .. } => all_of(&[size_x, size_y]),
        StyleValueData::Position { edge_x, edge_y, .. } => all_of(&[edge_x, edge_y]),
        StyleValueData::Shadow {
            color,
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            ..
        } => all_of(&[color, offset_x, offset_y, blur_radius, spread_distance]),
        StyleValueData::ColorMix {
            color_interpolation_method,
            first_color,
            first_percentage,
            second_color,
            second_percentage,
            ..
        } => all_of(&[
            color_interpolation_method,
            first_color,
            first_percentage,
            second_color,
            second_percentage,
        ]),
        StyleValueData::ValueList { values, .. }
        | StyleValueData::Tuple { values }
        | StyleValueData::Transformation { values, .. }
        | StyleValueData::Shorthand { values, .. } => all_in_list(values),
        StyleValueData::BorderRadiusRect {
            top_left,
            top_right,
            bottom_right,
            bottom_left,
            ..
        } => all_of(&[top_left, top_right, bottom_right, bottom_left]),
        StyleValueData::BorderRadius {
            horizontal_radius,
            vertical_radius,
            ..
        } => all_of(&[horizontal_radius, vertical_radius]),
        _ => None,
    }
}

/// # Safety
/// `data` must point at a valid StyleValueData and `data_of` must be a valid callback mapping a
/// nested value's shell pointer to its Rust-owned data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_is_computationally_independent(
    data: *const c_void,
    data_of: unsafe extern "C" fn(shell: *const c_void) -> *const c_void,
    decide_fallback: unsafe extern "C" fn(shell: *const c_void) -> bool,
) -> FfiIndependenceDecision {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::StyleValueQueryEntry);
    abort_on_panic(|| {
        match value_is_computationally_independent(
            unsafe { &*(data as *const StyleValueData) },
            data_of,
            decide_fallback,
        ) {
            Some(independent) => FfiIndependenceDecision {
                handled: true,
                independent,
            },
            None => FfiIndependenceDecision {
                handled: false,
                independent: false,
            },
        }
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
        // Calc values resolve to an integer in the calc core with no external context;
        // anything the core cannot resolve keeps the C++ caller's behavior.
        StyleValueData::Calculated { .. } => match crate::calc::resolve_calculated_integer_without_context(value) {
            Some(int) => computed(int),
            None => NUMBER_UNHANDLED,
        },
        // AD-HOC: the add(<integer>) function form is resolved by the C++ caller.
        StyleValueData::Function { .. } => NUMBER_UNHANDLED,
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
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_math_depth(value, inherited_math_depth, inherited_math_style_is_compact)
    })
}

// https://drafts.csswg.org/css-inline-3/#line-height-property
fn compute_line_height(value: &StyleValueData, computed_font_size: CssPixels) -> FfiComputedLineHeight {
    match value {
        // normal
        // <length [0,inf]>
        // <number [0,inf]>
        StyleValueData::Keyword { keyword } if *keyword == keyword::NORMAL => FfiComputedLineHeight {
            handled: true,
            unchanged: true,
            is_number: false,
            value: 0.0,
        },
        StyleValueData::Length { .. } | StyleValueData::Number { .. } => FfiComputedLineHeight {
            handled: true,
            unchanged: true,
            is_number: false,
            value: 0.0,
        },
        // <percentage [0,inf]>
        StyleValueData::Percentage { value } => FfiComputedLineHeight {
            handled: true,
            unchanged: false,
            is_number: false,
            value: computed_font_size.to_double() * (value / 100.0),
        },
        // Calc lengths, percentages, and numbers resolve in the calc core against the
        // computed font size; anything the core cannot resolve keeps the C++ caller's
        // behavior. Any other value would be unreachable there.
        StyleValueData::Calculated { .. } => {
            match crate::calc::resolve_calculated_line_height_without_context(value, computed_font_size.to_double()) {
                Some(crate::calc::ResolvedLineHeightCalc::Px(px)) => FfiComputedLineHeight {
                    handled: true,
                    unchanged: false,
                    is_number: false,
                    value: px,
                },
                Some(crate::calc::ResolvedLineHeightCalc::Number(number)) => FfiComputedLineHeight {
                    handled: true,
                    unchanged: false,
                    is_number: true,
                    value: number,
                },
                None => LINE_HEIGHT_UNHANDLED,
            }
        }
        _ => LINE_HEIGHT_UNHANDLED,
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
) -> FfiComputedLineHeight {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_corner_shape_parameter(value)
    })
}

/// Whether a font-family value is a single monospace keyword, which triggers
/// the monospace font-size recascade. The list entry's keyword is read through
/// the nested value's shell pointer.
///
/// # Safety
/// `data` must point at a valid StyleValueData and `data_of` map a nested
/// value's shell pointer to its Rust-owned data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_family_is_monospace(
    data: *const c_void,
    data_of: unsafe extern "C" fn(shell: *const c_void) -> *const c_void,
) -> bool {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let StyleValueData::ValueList { values, .. } = (unsafe { &*(data as *const StyleValueData) }) else {
            return false;
        };
        let values = values.as_slice();
        if values.len() != 1 {
            return false;
        }
        let entry_data = unsafe { data_of(values[0].shell_pointer()) };
        matches!(
            unsafe { &*(entry_data as *const StyleValueData) },
            StyleValueData::Keyword { keyword } if *keyword == keyword::MONOSPACE
        )
    })
}

/// Computes the ordering of a font-feature-settings or font-variation-settings
/// value list: deduplicate by tag with the later occurrence taking precedence,
/// then sort the survivors ascending by tag. The tag comparisons run through
/// C++ callbacks over the entry indices, since the tags are interned fly
/// strings the core does not read directly. Writes the surviving original
/// indices in computed order to `out_indices` and returns their count.
/// https://drafts.csswg.org/css-fonts/#font-feature-settings-prop
///
/// # Safety
/// The callbacks must be valid and `out_indices` must have room for `count`
/// entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_feature_settings_computed_order(
    count: usize,
    context: *mut c_void,
    tags_equal: unsafe extern "C" fn(*mut c_void, usize, usize) -> bool,
    tag_less: unsafe extern "C" fn(*mut c_void, usize, usize) -> bool,
    out_indices: *mut u32,
) -> usize {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        // Keep the last occurrence of each tag; later declarations take precedence.
        let mut survivors: Vec<usize> = (0..count)
            .filter(|&i| !((i + 1)..count).any(|j| unsafe { tags_equal(context, i, j) }))
            .collect();
        // The survivors have distinct tags, so tag_less is a total order over them.
        survivors.sort_by(|&a, &b| {
            if unsafe { tag_less(context, a, b) } {
                std::cmp::Ordering::Less
            } else {
                std::cmp::Ordering::Greater
            }
        });
        for (slot, &index) in survivors.iter().enumerate() {
            unsafe { *out_indices.add(slot) = index as u32 };
        }
        survivors.len()
    })
}

/// Whether a value contains a percentage, either directly or inside a
/// calculation tree; the shared core of the two font predicates below.
fn value_contains_percentage(value: &StyleValueData) -> bool {
    match value {
        StyleValueData::Percentage { .. } => true,
        StyleValueData::Calculated { rust_calculation, .. } => rust_calculation.node().contains_percentage(),
        _ => false,
    }
}

/// Whether a value's computed value depends on inherited font metrics because
/// of the property it belongs to: a font-weight of bolder or lighter (relative
/// to the inherited weight), a font-size that is a percentage, a percentage-
/// bearing calc(), or one of larger/smaller/math (relative to the inherited
/// size), or a line-height that is a percentage or percentage-bearing calc()
/// (relative to the computed font size). This is the property-specific part of
/// the flow's inheritance-dependency decision; the property-agnostic parts
/// (depends-on-current-color and computational independence) stay with the
/// value's own operations.
///
/// # Safety
/// `value` must point at a valid StyleValueData.
pub(crate) fn value_depends_on_inherited_info_for_property(value: &StyleValueData, property_id: u16) -> bool {
    use crate::property_metadata::property_id as prop;
    match property_id {
        prop::FONT_WEIGHT => {
            matches!(value, StyleValueData::Keyword { keyword } if matches!(*keyword, keyword::BOLDER | keyword::LIGHTER))
        }
        prop::FONT_SIZE => {
            value_contains_percentage(value)
                || matches!(value, StyleValueData::Keyword { keyword }
                    if matches!(*keyword, keyword::LARGER | keyword::SMALLER | keyword::MATH))
        }
        prop::LINE_HEIGHT => value_contains_percentage(value),
        _ => false,
    }
}

/// # Safety
/// `value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_value_depends_on_inherited_info_for_property(
    value: *const c_void,
    property_id: u16,
) -> bool {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::StyleValueQueryEntry);
    abort_on_panic(|| {
        value_depends_on_inherited_info_for_property(unsafe { &*(value as *const StyleValueData) }, property_id)
    })
}

/// The outcome of the font-style computation: whether a keyword was mapped to
/// a font-style keyword the caller should construct, and that keyword's code.
#[repr(C)]
pub struct FfiFontStyleComputation {
    pub is_keyword: bool,
    pub font_style_keyword: u8,
}

/// Computes font-style: a bare keyword maps to a font-style keyword (the caller
/// constructs the FontStyleStyleValue from it); any other value is already the
/// computed value. Font-style normally parses straight to a FontStyleStyleValue;
/// this keyword arm is reached when StylePropertyMap sets a keyword directly.
/// https://drafts.csswg.org/css-fonts-4/#font-style-prop
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_font_style(absolutized_value: *const c_void) -> FfiFontStyleComputation {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        if let StyleValueData::Keyword { keyword } = (unsafe { &*(absolutized_value as *const StyleValueData) })
            && let Some(font_style_keyword) = keyword_to_font_style_keyword(*keyword)
        {
            return FfiFontStyleComputation {
                is_keyword: true,
                font_style_keyword,
            };
        }
        FfiFontStyleComputation {
            is_keyword: false,
            font_style_keyword: 0,
        }
    })
}

/// Computes letter-spacing or word-spacing: the normal keyword computes to a
/// zero length, and any other value is already the computed value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_letter_or_word_spacing(absolutized_value: *const c_void) -> FfiComputedNumber {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| compute_letter_or_word_spacing_value(unsafe { &*(absolutized_value as *const StyleValueData) }))
}

fn compute_letter_or_word_spacing_value(absolutized_value: &StyleValueData) -> FfiComputedNumber {
    match absolutized_value {
        StyleValueData::Keyword { keyword } if *keyword == keyword::NORMAL => FfiComputedNumber {
            handled: true,
            unchanged: false,
            value: 0.0,
        },
        _ => FfiComputedNumber {
            handled: true,
            unchanged: true,
            value: 0.0,
        },
    }
}

// https://drafts.csswg.org/css-anchor-position/#position-area-computed
// The computed value of a <position-area> value is the two keywords indicating the selected
// tracks in each axis, with the long (block-start) and short (start) logical keywords treated
// as equivalent. It serializes with the logical keywords in their short forms.
#[unsafe(no_mangle)]
pub extern "C" fn rust_position_area_short_keyword(keyword: u16) -> u16 {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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

/// The outcome of the position-area span-all remapping: whether a single
/// keyword replaces the two-keyword value, and that keyword.
#[repr(C)]
pub struct FfiPositionAreaRemap {
    pub remapped: bool,
    pub keyword: u16,
}

/// When one axis of a position-area value is span-all, the value computes to a
/// single logical keyword drawn from the other axis. Returns that keyword, or
/// reports that no span-all remapping applies (both axes are then serialized
/// in short form by the caller).
/// https://drafts.csswg.org/css-anchor-position/#position-area-computed
#[unsafe(no_mangle)]
pub extern "C" fn rust_position_area_span_all_remap(block_keyword: u16, inline_keyword: u16) -> FfiPositionAreaRemap {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    let remapped = |keyword| FfiPositionAreaRemap {
        remapped: true,
        keyword,
    };
    let not_remapped = FfiPositionAreaRemap {
        remapped: false,
        keyword: 0,
    };
    if block_keyword == keyword::SPAN_ALL {
        return match inline_keyword {
            keyword::START => remapped(keyword::INLINE_START),
            keyword::END => remapped(keyword::INLINE_END),
            keyword::SELF_START => remapped(keyword::SELF_INLINE_START),
            keyword::SELF_END => remapped(keyword::SELF_INLINE_END),
            keyword::SPAN_START => remapped(keyword::SPAN_INLINE_START),
            keyword::SPAN_END => remapped(keyword::SPAN_INLINE_END),
            keyword::SPAN_SELF_START => remapped(keyword::SPAN_SELF_INLINE_START),
            keyword::SPAN_SELF_END => remapped(keyword::SPAN_SELF_INLINE_END),
            _ => not_remapped,
        };
    }
    if inline_keyword == keyword::SPAN_ALL {
        return match block_keyword {
            keyword::START => remapped(keyword::BLOCK_START),
            keyword::END => remapped(keyword::BLOCK_END),
            keyword::SELF_START => remapped(keyword::SELF_BLOCK_START),
            keyword::SELF_END => remapped(keyword::SELF_BLOCK_END),
            keyword::SPAN_START => remapped(keyword::SPAN_BLOCK_START),
            keyword::SPAN_END => remapped(keyword::SPAN_BLOCK_END),
            keyword::SPAN_SELF_START => remapped(keyword::SPAN_SELF_BLOCK_START),
            keyword::SPAN_SELF_END => remapped(keyword::SPAN_SELF_BLOCK_END),
            _ => not_remapped,
        };
    }
    not_remapped
}

/// A style value crossing the FFI as its C++ shell pointer paired with its
/// Rust-owned data pointer.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiShellAndData {
    pub shell: *const c_void,
    pub data: *const c_void,
}

impl FfiShellAndData {
    pub const fn null() -> Self {
        Self {
            shell: std::ptr::null(),
            data: std::ptr::null(),
        }
    }
}

/// The per-longhand initial values. The C++ side pins every entry for the
/// process lifetime before installing the table, so lookups never cross the
/// FFI and the pointers never dangle.
struct InitialValueTable(Vec<FfiShellAndData>);

// SAFETY: The entries reference immortal, immutable style values.
unsafe impl Send for InitialValueTable {}
unsafe impl Sync for InitialValueTable {}

static INITIAL_VALUE_TABLE: std::sync::OnceLock<InitialValueTable> = std::sync::OnceLock::new();

/// Installs the initial value table, one entry per longhand in property id
/// order.
///
/// # Safety
/// `entries` must point at `length` valid entries whose shells and data stay
/// alive for the process lifetime.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_metadata_set_initial_value_table(entries: *const FfiShellAndData, length: usize) {
    abort_on_panic(|| {
        let entries = unsafe { std::slice::from_raw_parts(entries, length) }.to_vec();
        assert_eq!(
            length,
            crate::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES,
            "initial value table has one entry per longhand"
        );
        assert!(
            INITIAL_VALUE_TABLE.set(InitialValueTable(entries)).is_ok(),
            "initial value table installed twice"
        );
    });
}

/// Returns the initial value of a longhand property.
pub(crate) fn initial_value(property_id: u16) -> FfiShellAndData {
    use crate::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let table = INITIAL_VALUE_TABLE.get().expect("initial value table not installed");
    table.0[(property_id - FIRST_LONGHAND_PROPERTY_ID) as usize]
}

/// FFI accessor for the parity test on the C++ side.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_metadata_initial_value(property_id: u16) -> FfiShellAndData {
    abort_on_panic(|| initial_value(property_id))
}

/// One bit per keyword marking the color keywords, installed once from the
/// C++ side's KeywordStyleValue::is_color classification.
static COLOR_KEYWORD_BITMAP: std::sync::OnceLock<Vec<u64>> = std::sync::OnceLock::new();

/// Installs the color keyword bitmap.
///
/// # Safety
/// `words` must point at `length` valid words.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_metadata_set_color_keyword_bitmap(words: *const u64, length: usize) {
    abort_on_panic(|| {
        let words = unsafe { std::slice::from_raw_parts(words, length) }.to_vec();
        assert!(
            COLOR_KEYWORD_BITMAP.set(words).is_ok(),
            "color keyword bitmap installed twice"
        );
    });
}

pub(crate) fn keyword_is_color(keyword: u16) -> bool {
    let Some(bitmap) = COLOR_KEYWORD_BITMAP.get() else {
        return false;
    };
    let index = keyword as usize;
    bitmap
        .get(index / 64)
        .is_some_and(|word| word & (1 << (index % 64)) != 0)
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

/// One deferred store operation for a longhand whose selected value needs no
/// computation: the value shell and the flags driving the C++ side effects
/// (animated-inheritance copy and inheritance-dependent bookkeeping).
#[repr(C)]
pub struct FfiComputedStoreEntry {
    pub property_id: u16,
    pub inherited_property_id: u16,
    /// The selected specified value; also the stored value unless a computed
    /// pixel length replaces it.
    pub shell: *const c_void,
    pub inheritance_dependent: bool,
    pub inherited: bool,
    /// How the natively computed value crosses: with COMPUTED_KIND_SHELL the
    /// stored value is `shell` itself; the other kinds carry a replacement in
    /// `value` while `shell` remains the specified value for the
    /// inheritance-dependence bookkeeping.
    pub computed_kind: u8,
    pub value: f64,
}

pub const COMPUTED_KIND_SHELL: u8 = 0;
/// A pixel length of `value`.
pub const COMPUTED_KIND_PX_LENGTH: u8 = 1;
/// An integer of `value`.
pub const COMPUTED_KIND_INTEGER: u8 = 2;
/// A superellipse with parameter `value`.
pub const COMPUTED_KIND_SUPERELLIPSE: u8 = 3;

/// The leaf callbacks the C++ side provides to the property computation
/// driver. The driver selects each longhand's cascaded, inherited or initial
/// value natively and calls back only to compute and store the result.
#[repr(C)]
pub struct FfiLonghandCallbacks {
    pub context: *mut c_void,
    /// Computes the selected value and stores the result; only called for
    /// properties that require computation. `value_shell` stays alive for
    /// the duration of the call: cascaded values are retained by the store,
    /// initial values are immortal, and parent values are pinned by the
    /// snapshot or the fetch below.
    pub compute_and_store: unsafe extern "C" fn(
        context: *mut c_void,
        property_id: u16,
        inherited_property_id: u16,
        value_shell: *const c_void,
        inheritance_dependent: bool,
        inherited: bool,
    ),
    /// Stores a batch of selected values that need no computation, applying
    /// each entry's side effects in property order. The driver flushes the
    /// batch before any callback that may read the stored values, so the
    /// C++ side always observes the same store sequence as one call per
    /// property would produce.
    pub store_computed_batch:
        unsafe extern "C" fn(context: *mut c_void, entries: *const FfiComputedStoreEntry, count: usize),
    /// Rare: fetches the parent's computed value for an explicit `inherit` of
    /// a non-inherited property, which the parent snapshot does not carry.
    /// The C++ side pins the returned shell until the end of the drive, so
    /// deferred store batches may hold it.
    pub fetch_non_inherited_parent_value:
        unsafe extern "C" fn(context: *mut c_void, inherited_property_id: u16) -> FfiShellAndData,
    /// Maps a nested value's shell pointer to its Rust-owned data while the
    /// driver decides inheritance dependence.
    pub data_of: unsafe extern "C" fn(shell: *const c_void) -> *const c_void,
    /// Decides computational independence for value kinds whose rule still
    /// lives with their C++ shells.
    pub computational_independence_fallback: unsafe extern "C" fn(shell: *const c_void) -> bool,
    /// Returns the element's computed writing mode and direction, packed as
    /// writing_mode | direction << 8.
    pub writing_mode_and_direction: unsafe extern "C" fn(context: *mut c_void) -> u16,
    /// Fetches the length resolution context the property's computation would
    /// use; the driver caches one per context kind and flushes pending stores
    /// first, since building a context reads stored values.
    pub length_resolution_context:
        unsafe extern "C" fn(context: *mut c_void, property_id: u16, out: *mut FfiLengthResolutionContext),
}

/// The computation-context kind a property's lengths resolve against,
/// mirroring StyleComputer::get_computation_context_for_property until the
/// context construction moves into the core.
#[derive(Clone, Copy, PartialEq)]
enum ComputationContextKind {
    Font,
    LineHeight,
    Generic,
}

fn computation_context_kind(property_id: u16) -> ComputationContextKind {
    use crate::property_metadata::property_id as prop;
    match property_id {
        prop::COLOR_SCHEME
        | prop::FONT_FAMILY
        | prop::FONT_FEATURE_SETTINGS
        | prop::FONT_KERNING
        | prop::FONT_OPTICAL_SIZING
        | prop::FONT_SIZE
        | prop::FONT_STYLE
        | prop::FONT_VARIANT_ALTERNATES
        | prop::FONT_VARIANT_CAPS
        | prop::FONT_VARIANT_EAST_ASIAN
        | prop::FONT_VARIANT_EMOJI
        | prop::FONT_VARIANT_LIGATURES
        | prop::FONT_VARIANT_NUMERIC
        | prop::FONT_VARIANT_POSITION
        | prop::FONT_VARIATION_SETTINGS
        | prop::FONT_WEIGHT
        | prop::FONT_WIDTH
        | prop::MATH_DEPTH
        | prop::TEXT_RENDERING => ComputationContextKind::Font,
        prop::LINE_HEIGHT => ComputationContextKind::LineHeight,
        _ => ComputationContextKind::Generic,
    }
}

/// Whether a value's absolutization is the identity, so the specified value
/// is already the computed value. Mirrors the value types that fall through
/// to the default arm of StyleValue::absolutized, plus keywords that resolve
/// to themselves: the currentcolor keyword computes to itself, and only
/// color keywords resolve to something else at computed-value time.
fn absolutization_is_identity(value: &StyleValueData) -> bool {
    match value {
        StyleValueData::Keyword { keyword } => *keyword == keyword::CURRENTCOLOR || !keyword_is_color(*keyword),
        StyleValueData::Number { .. }
        | StyleValueData::Integer { .. }
        | StyleValueData::String { .. }
        | StyleValueData::CustomIdent { .. }
        | StyleValueData::Percentage { .. }
        | StyleValueData::Flex { .. }
        | StyleValueData::UnicodeRange { .. }
        | StyleValueData::Url { .. } => true,
        _ => false,
    }
}

/// Properties with a dedicated computed-value rule in the C++ dispatcher
/// (StyleComputer::compute_value_of_property); everything else computes as
/// plain absolutization. Mirrors the C++ switch until the dispatch moves
/// into the core.
fn property_has_dedicated_compute_rule(property_id: u16) -> bool {
    use crate::property_metadata::property_id as prop;
    matches!(
        property_id,
        prop::ANIMATION_NAME
            | prop::BACKGROUND_ATTACHMENT
            | prop::BACKGROUND_CLIP
            | prop::BACKGROUND_ORIGIN
            | prop::BACKGROUND_POSITION_X
            | prop::BACKGROUND_POSITION_Y
            | prop::BACKGROUND_REPEAT
            | prop::BACKGROUND_SIZE
            | prop::BORDER_BOTTOM_WIDTH
            | prop::BORDER_LEFT_WIDTH
            | prop::BORDER_RIGHT_WIDTH
            | prop::BORDER_TOP_WIDTH
            | prop::OUTLINE_WIDTH
            | prop::CORNER_BOTTOM_LEFT_SHAPE
            | prop::CORNER_BOTTOM_RIGHT_SHAPE
            | prop::CORNER_TOP_LEFT_SHAPE
            | prop::CORNER_TOP_RIGHT_SHAPE
            | prop::FONT_SIZE
            | prop::FONT_STYLE
            | prop::FONT_WEIGHT
            | prop::FONT_WIDTH
            | prop::FONT_FEATURE_SETTINGS
            | prop::FONT_VARIATION_SETTINGS
            | prop::LETTER_SPACING
            | prop::WORD_SPACING
            | prop::LINE_HEIGHT
            | prop::MATH_DEPTH
            | prop::POSITION_AREA
    )
}

/// The parent's inheritable computed values, prepared once per element: one
/// (shell, data) entry per inherited-by-default longhand in property id
/// order. Null entries mark values the parent could not provide. The C++ side
/// pins every entry for the duration of the drive.
#[repr(C)]
pub struct FfiParentSnapshot {
    pub entries: *const FfiShellAndData,
    pub entry_count: usize,
    pub font_metrics_depend_on_viewport_metrics: bool,
}

/// Bulk results of one longhand drive, applied by C++ after the loop instead
/// of once per longhand. The bitmap storage is provided by the caller, one
/// bit per longhand in property id order.
#[repr(C)]
pub struct FfiLonghandDriverResults {
    pub important_words: *mut u64,
    pub inherited_words: *mut u64,
    pub word_count: usize,
    /// The raw winning cascaded font-size value, or null; borrowed from the
    /// cascaded property store.
    pub raw_cascaded_font_size_shell: *const c_void,
    pub depends_on_viewport_metrics: bool,
    pub font_metrics_depend_on_viewport_metrics: bool,
    pub explicitly_inherited_non_inherited_property: bool,
}

fn table_row_maps(table: &std::sync::OnceLock<Vec<u16>>, property_id: u16) -> bool {
    use crate::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let Some(table) = table.get() else {
        return false;
    };
    let start = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize * WRITING_MODE_COUNT * DIRECTION_COUNT;
    table[start..start + WRITING_MODE_COUNT * DIRECTION_COUNT]
        .iter()
        .any(|&entry| entry != 0)
}

fn value_is_initial_or_unset(value: *const c_void) -> bool {
    if value.is_null() {
        return false;
    }
    match unsafe { &*(value as *const StyleValueData) } {
        StyleValueData::Keyword { keyword } => *keyword == keyword::INITIAL || *keyword == keyword::UNSET,
        _ => false,
    }
}

fn set_longhand_bit(words: &mut [u64], property_id: u16) {
    use crate::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    words[index / 64] |= 1 << (index % 64);
}

/// Drives the property computation loop: iterates every longhand in
/// computation order, resolves logical pairing, reads the winning cascaded
/// declarations straight from the store, selects between the cascaded,
/// inherited and initial values, decides inheritance dependence, and calls
/// back into C++ once per longhand to compute and store the result. The
/// importance and inheritance flags and the other per-element side effects
/// accumulate in `results` for bulk application after the loop.
///
/// # Safety
/// `callbacks` must point at a valid callback table, `store` at a valid
/// cascaded property store, `parent_snapshot` at a valid snapshot or null,
/// and `results` at a results block whose bitmap storage covers every
/// longhand; the callbacks must not mutate the store for the duration of the
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_drive_property_computation(
    callbacks: *const FfiLonghandCallbacks,
    store: *const CascadedPropertyStore,
    parent_snapshot: *const FfiParentSnapshot,
    has_new_font_size: bool,
    device_pixels_per_css_pixel: f64,
    results: *mut FfiLonghandDriverResults,
) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::LonghandDriverEntry);
    abort_on_panic(|| {
        use crate::property_metadata::{
            FIRST_INHERITED_PROPERTY_ID, NUMBER_OF_LONGHAND_PROPERTIES, REQUIRES_COMPUTATION_ALWAYS,
            REQUIRES_COMPUTATION_CASCADED, REQUIRES_COMPUTATION_NON_INHERITED, property_is_inherited,
            property_requires_computation_level,
        };

        let callbacks = unsafe { &*callbacks };
        let context = callbacks.context;
        let store = unsafe { &*store };
        let snapshot = if parent_snapshot.is_null() {
            None
        } else {
            Some(unsafe { &*parent_snapshot })
        };
        let has_inheritance_parent = snapshot.is_some();
        let results = unsafe { &mut *results };
        assert!(results.word_count * 64 >= NUMBER_OF_LONGHAND_PROPERTIES);
        let important_words = unsafe { std::slice::from_raw_parts_mut(results.important_words, results.word_count) };
        let inherited_words = unsafe { std::slice::from_raw_parts_mut(results.inherited_words, results.word_count) };
        let mut cached_writing_mode_and_direction: Option<(u8, u8)> = None;

        // Store operations queued for properties that need no computation, flushed in one
        // crossing before any callback that may read the stored values.
        let mut pending_stores: Vec<FfiComputedStoreEntry> = Vec::new();
        // Length resolution contexts fetched from C++ on first use, one per kind, like
        // the C++ side's per-element computation context caches.
        let mut cached_length_resolution_contexts: [Option<FfiLengthResolutionContext>; 3] = [None; 3];
        fn flush_pending_stores(
            callbacks: &FfiLonghandCallbacks,
            context: *mut c_void,
            pending_stores: &mut Vec<FfiComputedStoreEntry>,
        ) {
            if pending_stores.is_empty() {
                return;
            }
            crate::ffi_stats::bump(crate::ffi_stats::FfiOp::LonghandStoreBatchCallback);
            // SAFETY: The entries and their shells stay alive for the call; the callback
            // table outlives the drive.
            unsafe { (callbacks.store_computed_batch)(context, pending_stores.as_ptr(), pending_stores.len()) };
            pending_stores.clear();
        }

        for &property_id in crate::property_metadata::property_computation_order() {
            let mut cascaded_property_id = property_id;
            let mut inherited_property_id = property_id;

            // https://drafts.csswg.org/css-logical/#box
            // Within each logical property group, corresponding flow-relative and physical
            // properties are paired using the element's own computed writing mode; the computed
            // value of both properties in the pair is derived from the specified value of the
            // property declared with higher priority in the CSS cascade. A longhand is in a
            // logical property group exactly when either mapping table maps it.
            let is_logical_alias = table_row_maps(&LOGICAL_ALIAS_TABLE, property_id);
            if is_logical_alias || table_row_maps(&PHYSICAL_TO_LOGICAL_TABLE, property_id) {
                if cached_writing_mode_and_direction.is_none() {
                    flush_pending_stores(callbacks, context, &mut pending_stores);
                }
                let (writing_mode, direction) = *cached_writing_mode_and_direction.get_or_insert_with(|| {
                    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::LonghandWritingModeCallback);
                    let packed = unsafe { (callbacks.writing_mode_and_direction)(context) };
                    ((packed & 0xff) as u8, (packed >> 8) as u8)
                });
                let counterpart_property_id = if is_logical_alias {
                    let physical = map_logical_alias_to_physical(property_id, writing_mode, direction);
                    // AD-HOC: While the spec says that inheritance of logical aliases should be
                    // direct, other browsers instead inherit from the physical counterpart - the
                    // CSSWG has resolved to update the spec to reflect this -
                    // see https://github.com/w3c/csswg-drafts/issues/3029
                    inherited_property_id = physical;
                    physical
                } else {
                    map_physical_to_logical_alias(property_id, writing_mode, direction)
                };
                cascaded_property_id = store.property_with_higher_priority(property_id, counterpart_property_id);
            }

            let mut value = FfiShellAndData::null();
            if let Some((value_shell, value_data, important)) = store.winning_declaration(cascaded_property_id) {
                value = FfiShellAndData {
                    shell: value_shell,
                    data: value_data,
                };
                if important {
                    set_longhand_bit(important_words, property_id);
                }
                // Keep the raw winning cascaded font-size for the monospace font-size
                // recascade (see recascade_font_size_if_needed on the C++ side).
                if property_id == crate::property_metadata::property_id::FONT_SIZE {
                    results.raw_cascaded_font_size_shell = value_shell;
                }
            } else if property_id == crate::property_metadata::property_id::FONT_SIZE && has_new_font_size {
                // NOTE: The recascaded font-size has already been stored before the loop.
                continue;
            }

            let decision = longhand_decision(
                if value.data.is_null() {
                    None
                } else {
                    Some(unsafe { &*(value.data as *const StyleValueData) })
                },
                property_id,
            );

            // The computation-need level to compare against depends on which source wins;
            // cascaded is the baseline and is overridden by the inherit and initial paths.
            let mut required_level = REQUIRES_COMPUTATION_CASCADED;

            let inherit_fetch_attempted = decision.should_inherit && has_inheritance_parent;
            if inherit_fetch_attempted {
                let snapshot = snapshot.unwrap();
                set_longhand_bit(inherited_words, property_id);
                if decision.explicitly_inherits_non_inherited_property {
                    results.explicitly_inherited_non_inherited_property = true;
                }
                value = if property_is_inherited(inherited_property_id) {
                    let index = (inherited_property_id - FIRST_INHERITED_PROPERTY_ID) as usize;
                    assert!(index < snapshot.entry_count);
                    unsafe { *snapshot.entries.add(index) }
                } else {
                    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::LonghandParentValueFetchCallback);
                    unsafe { (callbacks.fetch_non_inherited_parent_value)(context, inherited_property_id) }
                };
                if property_affects_font_metrics(inherited_property_id)
                    && snapshot.font_metrics_depend_on_viewport_metrics
                {
                    results.font_metrics_depend_on_viewport_metrics = true;
                }
                required_level = REQUIRES_COMPUTATION_ALWAYS;
            }

            let use_initial = if inherit_fetch_attempted {
                value.data.is_null() || value_is_initial_or_unset(value.data)
            } else {
                decision.use_initial_without_inherit
            };
            if use_initial {
                value = initial_value(property_id);
                required_level = REQUIRES_COMPUTATION_NON_INHERITED;
            }

            let requires_computation = property_requires_computation_level(property_id) >= required_level;

            // Whether the computed value depends on inherited information, so the specified
            // value must be kept for re-resolution when an ancestor changes.
            let value_data = unsafe { &*(value.data as *const StyleValueData) };
            let inheritance_dependent =
                crate::style_value::value_depends_on_current_color(value_data, callbacks.data_of)
                    || !value_is_computationally_independent(
                        value_data,
                        callbacks.data_of,
                        callbacks.computational_independence_fallback,
                    )
                    .unwrap_or_else(|| {
                        crate::ffi_stats::bump(crate::ffi_stats::FfiOp::LonghandIndependenceFallbackCallback);
                        unsafe { (callbacks.computational_independence_fallback)(value.shell) }
                    })
                    || value_depends_on_inherited_info_for_property(value_data, property_id);

            if requires_computation {
                // Plain length values of properties without a dedicated computed-value rule
                // absolutize natively; everything else still computes through C++.
                // The specified value absolutized natively when the core can:
                // Some(None) leaves the value unchanged, Some(Some(px)) resolves it to
                // a pixel length, and None means C++ must handle it.
                let absolutized: Option<Option<f64>> = if absolutization_is_identity(value_data) {
                    Some(None)
                } else if let StyleValueData::Length {
                    value: length_value,
                    unit,
                } = value_data
                {
                    let kind = computation_context_kind(inherited_property_id) as usize;
                    let resolution_context = cached_length_resolution_contexts[kind].get_or_insert_with(|| {
                        // Building a context on the C++ side reads stored values.
                        flush_pending_stores(callbacks, context, &mut pending_stores);
                        crate::ffi_stats::bump(crate::ffi_stats::FfiOp::LonghandContextFetchCallback);
                        let mut fetched = std::mem::MaybeUninit::<FfiLengthResolutionContext>::uninit();
                        // SAFETY: The callback fills the context before returning.
                        unsafe {
                            (callbacks.length_resolution_context)(context, inherited_property_id, fetched.as_mut_ptr());
                            fetched.assume_init()
                        }
                    });
                    let result = absolutize_length(*length_value, *unit as usize, resolution_context);
                    if result.handled {
                        if result.resolved_viewport_relative_length {
                            results.depends_on_viewport_metrics = true;
                            if property_affects_font_metrics(inherited_property_id) {
                                results.font_metrics_depend_on_viewport_metrics = true;
                            }
                        }
                        Some(result.changed.then_some(result.px))
                    } else {
                        None
                    }
                } else {
                    None
                };

                // The computed value: for properties without a dedicated rule the
                // absolutized value is the computed value; the dedicated rules that
                // have moved into the core run over the absolutized value here.
                enum NativeValue {
                    Unsupported,
                    Unchanged,
                    Px(f64),
                    Integer(i32),
                    Superellipse(f64),
                }
                use crate::property_metadata::property_id as prop;
                let synthesized_px_length = |absolutized: Option<f64>| {
                    absolutized.map(|px| StyleValueData::Length {
                        value: px,
                        unit: px_length_unit(),
                    })
                };
                let native = match (absolutized, inherited_property_id) {
                    (
                        Some(absolutized),
                        prop::BORDER_BOTTOM_WIDTH
                        | prop::BORDER_LEFT_WIDTH
                        | prop::BORDER_RIGHT_WIDTH
                        | prop::BORDER_TOP_WIDTH
                        | prop::OUTLINE_WIDTH,
                    ) => {
                        let synthesized = synthesized_px_length(absolutized);
                        let result = compute_border_or_outline_width(
                            synthesized.as_ref().unwrap_or(value_data),
                            device_pixels_per_css_pixel,
                        );
                        if result.handled {
                            NativeValue::Px(result.value)
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (
                        Some(_),
                        prop::CORNER_BOTTOM_LEFT_SHAPE
                        | prop::CORNER_BOTTOM_RIGHT_SHAPE
                        | prop::CORNER_TOP_LEFT_SHAPE
                        | prop::CORNER_TOP_RIGHT_SHAPE,
                    ) => {
                        // Corner shape values are keywords or superellipses; only keywords
                        // reach here since superellipse absolutization stays with C++.
                        let result = compute_corner_shape_parameter(value_data);
                        if result.handled && !result.unchanged {
                            NativeValue::Superellipse(result.value)
                        } else if result.handled {
                            NativeValue::Unchanged
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (Some(_), prop::MATH_DEPTH) => {
                        // The inherited math-depth and math-style come from the parent
                        // snapshot; without an inheritance parent the initial values apply
                        // (math-depth 0, math-style normal).
                        let (inherited_math_depth, inherited_math_style_is_compact) = match snapshot {
                            Some(snapshot) => {
                                let entry_data = |property_id: u16| {
                                    let index = (property_id - FIRST_INHERITED_PROPERTY_ID) as usize;
                                    assert!(index < snapshot.entry_count);
                                    // SAFETY: Snapshot entries are valid for the drive.
                                    unsafe { (*snapshot.entries.add(index)).data as *const StyleValueData }
                                };
                                let math_depth = match unsafe { entry_data(prop::MATH_DEPTH).as_ref() } {
                                    Some(StyleValueData::Integer { value }) => *value,
                                    _ => 0,
                                };
                                let compact = matches!(
                                    unsafe { entry_data(prop::MATH_STYLE).as_ref() },
                                    Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::COMPACT
                                );
                                (math_depth, compact)
                            }
                            None => (0, false),
                        };
                        let result =
                            compute_math_depth(value_data, inherited_math_depth, inherited_math_style_is_compact);
                        if result.handled {
                            NativeValue::Integer(result.value as i32)
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (Some(absolutized), prop::LETTER_SPACING | prop::WORD_SPACING) => {
                        let synthesized = synthesized_px_length(absolutized);
                        let result = compute_letter_or_word_spacing_value(synthesized.as_ref().unwrap_or(value_data));
                        if result.handled {
                            if result.unchanged {
                                match absolutized {
                                    Some(px) => NativeValue::Px(px),
                                    None => NativeValue::Unchanged,
                                }
                            } else {
                                NativeValue::Px(result.value)
                            }
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (Some(absolutized), _) if !property_has_dedicated_compute_rule(inherited_property_id) => {
                        match absolutized {
                            Some(px) => NativeValue::Px(px),
                            None => NativeValue::Unchanged,
                        }
                    }
                    _ => NativeValue::Unsupported,
                };

                if !matches!(native, NativeValue::Unsupported) {
                    let (computed_kind, computed_value) = match native {
                        NativeValue::Px(px) => (COMPUTED_KIND_PX_LENGTH, px),
                        NativeValue::Integer(integer) => (COMPUTED_KIND_INTEGER, integer as f64),
                        NativeValue::Superellipse(parameter) => (COMPUTED_KIND_SUPERELLIPSE, parameter),
                        _ => (COMPUTED_KIND_SHELL, 0.0),
                    };
                    pending_stores.push(FfiComputedStoreEntry {
                        property_id,
                        inherited_property_id,
                        shell: value.shell,
                        inheritance_dependent,
                        inherited: inherit_fetch_attempted,
                        computed_kind,
                        value: computed_value,
                    });
                } else {
                    flush_pending_stores(callbacks, context, &mut pending_stores);
                    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::LonghandComputeAndStoreCallback);
                    unsafe {
                        (callbacks.compute_and_store)(
                            context,
                            property_id,
                            inherited_property_id,
                            value.shell,
                            inheritance_dependent,
                            inherit_fetch_attempted,
                        );
                    }
                }
            } else {
                pending_stores.push(FfiComputedStoreEntry {
                    property_id,
                    inherited_property_id,
                    shell: value.shell,
                    inheritance_dependent,
                    inherited: inherit_fetch_attempted,
                    computed_kind: COMPUTED_KIND_SHELL,
                    value: 0.0,
                });
            }
        }

        flush_pending_stores(callbacks, context, &mut pending_stores);
    });
}

fn property_affects_font_metrics(property_id: u16) -> bool {
    property_id == crate::property_metadata::property_id::FONT_SIZE
        || property_id == crate::property_metadata::property_id::LINE_HEIGHT
}

/// Stage callbacks for the cascade origin sequence.
#[repr(C)]
pub struct FfiCascadeStageCallbacks {
    pub context: *mut c_void,
    pub cascade_user_agent_rules: unsafe extern "C" fn(context: *mut c_void, important: bool),
    pub cascade_user_rules: unsafe extern "C" fn(context: *mut c_void, important: bool),
    pub cascade_presentational_hints: unsafe extern "C" fn(context: *mut c_void),
    pub cascade_author_rules: unsafe extern "C" fn(context: *mut c_void, important: bool),
}

/// Runs the cascade origin stages in css-cascade priority order.
///
/// https://drafts.csswg.org/css-cascade-5/#cascade-origin
/// Declarations are applied lowest priority first, so that later stages
/// overwrite earlier ones: normal user agent, normal user, author
/// presentational hints (treated as an independent origin for cascading per
/// css-cascade-5, but as part of the author origin for revert), normal author,
/// important author, important user, and important user agent declarations.
///
/// # Safety
/// `callbacks` must point at a valid callback table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_drive_cascade_origins(callbacks: *const FfiCascadeStageCallbacks) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadeOriginDriverEntry);
    abort_on_panic(|| {
        let callbacks = unsafe { &*callbacks };
        let context = callbacks.context;
        crate::ffi_stats::bump_by(crate::ffi_stats::FfiOp::CascadeStageCallback, 7);
        unsafe {
            (callbacks.cascade_user_agent_rules)(context, false);
            (callbacks.cascade_user_rules)(context, false);
            (callbacks.cascade_presentational_hints)(context);
            (callbacks.cascade_author_rules)(context, false);
            (callbacks.cascade_author_rules)(context, true);
            (callbacks.cascade_user_rules)(context, true);
            (callbacks.cascade_user_agent_rules)(context, true);
        }
    });
}

/// Shell-level callbacks for the shorthand expansion recursion. Values cross
/// as opaque C++ style value shells; the C++ side pins every value it creates
/// until the expansion returns.
#[repr(C)]
pub struct FfiShorthandExpansionCallbacks {
    pub context: *mut c_void,
    /// Returns the Rust-owned data of a C++ style value shell.
    pub data_of: unsafe extern "C" fn(context: *mut c_void, shell: *const c_void) -> *const c_void,
    /// Creates and pins a pending-substitution value wrapping the given value;
    /// returns its shell.
    pub create_pending_substitution: unsafe extern "C" fn(context: *mut c_void, shell: *const c_void) -> *const c_void,
    pub set_longhand_property: unsafe extern "C" fn(context: *mut c_void, property_id: u16, shell: *const c_void),
}

pub(crate) fn value_is_css_wide_keyword(value: &StyleValueData) -> bool {
    match value {
        StyleValueData::Keyword { keyword } => matches!(
            *keyword,
            keyword::INHERIT | keyword::INITIAL | keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER
        ),
        _ => false,
    }
}

/// The expansion recursion over `(shell, data)` value pairs. `data_of` returns the Rust-owned
/// data of a shell, `create_pending_substitution` wraps a shell in a pinned
/// pending-substitution value, and `sink` receives each `(longhand id, shell, data)` result.
pub(crate) fn expand_shorthands_with<DataOf, CreatePendingSubstitution, Sink>(
    data_of: &DataOf,
    create_pending_substitution: &CreatePendingSubstitution,
    property_id: u16,
    shell: *const c_void,
    data: *const c_void,
    sink: &mut Sink,
) where
    DataOf: Fn(*const c_void) -> *const c_void,
    CreatePendingSubstitution: Fn(*const c_void) -> *const c_void,
    Sink: FnMut(u16, *const c_void, *const c_void),
{
    let value = unsafe { &*(data as *const StyleValueData) };
    let is_shorthand = property_is_shorthand(property_id);

    if is_shorthand
        && matches!(
            value,
            StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. }
        )
    {
        // If a shorthand property contains an arbitrary substitution function in its value, the
        // longhand properties it's associated with must instead be filled in with a special,
        // unobservable-to-authors pending-substitution value that indicates the shorthand
        // contains an arbitrary substitution function, and thus the longhand's value can't be
        // determined until after substituted.
        // https://drafts.csswg.org/css-values-5/#pending-substitution-value
        // Ensure we keep the longhand around until it can be resolved.
        sink(property_id, shell, data);
        let pending = create_pending_substitution(shell);
        let pending_data = data_of(pending);
        for &longhand in longhands_for_shorthand(property_id) {
            expand_shorthands_with(
                data_of,
                create_pending_substitution,
                longhand,
                pending,
                pending_data,
                sink,
            );
        }
        return;
    }

    if let StyleValueData::Shorthand {
        sub_properties, values, ..
    } = value
    {
        for (&sub_property, sub_value) in sub_properties.as_slice().iter().zip(values.as_slice()) {
            let sub_shell = sub_value.shell_pointer();
            let sub_data = data_of(sub_shell);
            expand_shorthands_with(
                data_of,
                create_pending_substitution,
                sub_property,
                sub_shell,
                sub_data,
                sink,
            );
        }
        return;
    }

    if is_shorthand {
        // ShorthandStyleValue was handled already, as were unresolved shorthands. That means the
        // only values we should see are the CSS-wide keywords, or the guaranteed-invalid value.
        // Both should be applied to our longhand properties. We do not directly set the longhand
        // because the longhands might have longhands of their own.
        assert!(value_is_css_wide_keyword(value) || matches!(value, StyleValueData::GuaranteedInvalid));
        for &longhand in longhands_for_shorthand(property_id) {
            expand_shorthands_with(data_of, create_pending_substitution, longhand, shell, data, sink);
        }
        return;
    }

    sink(property_id, shell, data);
}

fn expand_shorthands(
    callbacks: &FfiShorthandExpansionCallbacks,
    property_id: u16,
    shell: *const c_void,
    data: *const c_void,
) {
    let context = callbacks.context;
    expand_shorthands_with(
        &|shell| {
            crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadeDataOfCallback);
            unsafe { (callbacks.data_of)(context, shell) }
        },
        &|shell| {
            crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadePendingSubstitutionCallback);
            unsafe { (callbacks.create_pending_substitution)(context, shell) }
        },
        property_id,
        shell,
        data,
        &mut |longhand_id, longhand_shell, _longhand_data| {
            crate::ffi_stats::bump(crate::ffi_stats::FfiOp::ShorthandSetLonghandCallback);
            unsafe { (callbacks.set_longhand_property)(context, longhand_id, longhand_shell) };
        },
    );
}

/// Expands a declared property into longhand assignments, recursing through
/// shorthand and pending-substitution values.
///
/// # Safety
/// `callbacks` must be a valid callback table and `shell`/`data` a valid
/// C++ style value and its Rust-owned data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_for_each_property_expanding_shorthands(
    callbacks: *const FfiShorthandExpansionCallbacks,
    property_id: u16,
    shell: *const c_void,
    data: *const c_void,
) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::ShorthandExpansionEntry);
    abort_on_panic(|| expand_shorthands(unsafe { &*callbacks }, property_id, shell, data));
}

/// Mirror of the CSS Display value; the C++ tagged union crosses as explicit
/// fields, with the unused fields zeroed so equality is field-wise. `tag` uses
/// the same discriminants as Display::Type.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct FfiDisplay {
    /// 0 = outside-and-inside, 1 = internal, 2 = box.
    pub tag: u8,
    pub outside: u8,
    pub inside: u8,
    pub list_item: bool,
    pub internal: u8,
    pub box_value: u8,
}

const DISPLAY_TAG_OUTSIDE_AND_INSIDE: u8 = 0;
const DISPLAY_TAG_INTERNAL: u8 = 1;
const DISPLAY_TAG_BOX: u8 = 2;

impl FfiDisplay {
    fn outside_and_inside(outside: u8, inside: u8, list_item: bool) -> Self {
        Self {
            tag: DISPLAY_TAG_OUTSIDE_AND_INSIDE,
            outside,
            inside,
            list_item,
            internal: 0,
            box_value: 0,
        }
    }

    fn internal(internal: u8) -> Self {
        Self {
            tag: DISPLAY_TAG_INTERNAL,
            outside: 0,
            inside: 0,
            list_item: false,
            internal,
            box_value: 0,
        }
    }

    fn block() -> Self {
        Self::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, false)
    }

    fn is_outside_and_inside(&self) -> bool {
        self.tag == DISPLAY_TAG_OUTSIDE_AND_INSIDE
    }

    fn is_internal(&self) -> bool {
        self.tag == DISPLAY_TAG_INTERNAL
    }

    fn is_none(&self) -> bool {
        self.tag == DISPLAY_TAG_BOX && self.box_value == display_box::NONE
    }

    fn is_contents(&self) -> bool {
        self.tag == DISPLAY_TAG_BOX && self.box_value == display_box::CONTENTS
    }

    fn is_block_outside(&self) -> bool {
        self.is_outside_and_inside() && self.outside == display_outside::BLOCK
    }

    fn is_inline_outside(&self) -> bool {
        self.is_outside_and_inside() && self.outside == display_outside::INLINE
    }

    fn is_math_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::MATH
    }

    fn is_inline_block(&self) -> bool {
        self.is_inline_outside() && self.inside == display_inside::FLOW_ROOT
    }

    fn is_grid_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::GRID
    }

    fn is_flex_inside(&self) -> bool {
        self.is_outside_and_inside() && self.inside == display_inside::FLEX
    }
}

/// The element facts the box type transformation needs, marshalled by the C++
/// side. The parent display is the first non-`display: contents` ancestor's.
#[repr(C)]
pub struct FfiBoxTypeTransformationInput {
    pub display: FfiDisplay,
    /// Computed position and float keywords.
    pub position: u16,
    pub float_value: u16,
    pub is_br_element: bool,
    pub is_document_element: bool,
    pub is_mathml_element: bool,
    pub is_mathml_mtable: bool,
    pub is_mathml_mtr: bool,
    pub is_mathml_mtd: bool,
    pub has_parent_display: bool,
    pub parent_display: FfiDisplay,
}

/// Result of the box type transformation: whether float must be reset to none,
/// and the possibly replaced display.
#[repr(C)]
pub struct FfiBoxTypeTransformation {
    pub set_float_none: bool,
    pub changed_display: bool,
    pub display: FfiDisplay,
}

// NB: css-display-3 also defines inlinification, but nothing triggers it yet (the only
//     candidate is the ruby containment FIXME below), so only blockification is modelled.
enum BoxTypeTransformation {
    None,
    Blockify,
}

fn required_box_type_transformation(input: &FfiBoxTypeTransformationInput) -> BoxTypeTransformation {
    // NOTE: We never blockify <br> elements. They are always inline.
    //       There is currently no way to express in CSS how a <br> element really behaves.
    //       Spec issue: https://github.com/whatwg/html/issues/2291
    if input.is_br_element {
        return BoxTypeTransformation::None;
    }

    // Absolute positioning or floating an element blockifies the box's display type. [CSS2]
    if input.position == keyword::ABSOLUTE || input.position == keyword::FIXED || input.float_value != keyword::NONE {
        return BoxTypeTransformation::Blockify;
    }

    // FIXME: Containment in a ruby container inlinifies the box's display type, as described in [CSS-RUBY-1].

    // A parent with a grid or flex display value blockifies the box's display type. [CSS-GRID-1] [CSS-FLEXBOX-1]
    // NB: The C++ side supplies the first ancestor that is not `display: contents`; for a
    //     pseudo-element that climb starts at the originating element itself.
    if input.has_parent_display && (input.parent_display.is_grid_inside() || input.parent_display.is_flex_inside()) {
        return BoxTypeTransformation::Blockify;
    }

    BoxTypeTransformation::None
}

/// https://drafts.csswg.org/css-display/#transformations
/// 2.7. Automatic Box Type Transformations
///
/// # Safety
/// `input` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_transform_box_type(
    input: *const FfiBoxTypeTransformationInput,
) -> FfiBoxTypeTransformation {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let input = unsafe { &*input };
        let display = input.display;
        let unchanged = |set_float_none: bool| FfiBoxTypeTransformation {
            set_float_none,
            changed_display: false,
            display,
        };

        // Some layout effects require blockification or inlinification of the box type,
        // which sets the box's computed outer display type to block or inline (respectively).
        // (This has no effect on display types that generate no box at all, such as none or contents.)
        if display.is_none() || (display.is_contents() && !input.is_document_element) {
            return unchanged(false);
        }

        // https://drafts.csswg.org/css-display/#root
        // The root element's display type is always blockified, and its principal box always establishes an independent formatting context.
        if input.is_document_element && !display.is_block_outside() {
            return FfiBoxTypeTransformation {
                set_float_none: false,
                changed_display: true,
                display: FfiDisplay::block(),
            };
        }

        let mut new_display = display;

        if display.is_math_inside() {
            // https://w3c.github.io/mathml-core/#new-display-math-value
            // For elements that are not MathML elements, if the specified value of display is inline math or block math
            // then the computed value is block flow and inline flow respectively.
            if !input.is_mathml_element {
                new_display = FfiDisplay::outside_and_inside(display.outside, display_inside::FLOW, display.list_item);
            }
            // For the mtable element the computed value is block table and inline table respectively.
            else if input.is_mathml_mtable {
                new_display = FfiDisplay::outside_and_inside(display.outside, display_inside::TABLE, display.list_item);
            }
            // For the mtr element, the computed value is table-row.
            else if input.is_mathml_mtr {
                new_display = FfiDisplay::internal(display_internal::TABLE_ROW);
            }
            // For the mtd element, the computed value is table-cell.
            else if input.is_mathml_mtd {
                new_display = FfiDisplay::internal(display_internal::TABLE_CELL);
            }
        }

        // https://www.w3.org/TR/CSS2/visuren.html#dis-pos-flo
        // If 'position' has the value 'absolute' or 'fixed', [...] 'float' is set to 'none'
        let set_float_none = input.position == keyword::ABSOLUTE || input.position == keyword::FIXED;

        match required_box_type_transformation(input) {
            BoxTypeTransformation::None => {}
            BoxTypeTransformation::Blockify => {
                if display.is_block_outside() {
                    return unchanged(set_float_none);
                }
                // If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a block container.
                if display.is_internal() {
                    new_display = FfiDisplay::block();
                } else {
                    assert!(display.is_outside_and_inside());

                    // For legacy reasons, if an inline block box (inline flow-root) is blockified, it becomes a block box (losing its flow-root nature).
                    // For consistency, a run-in flow-root box also blockifies to a block box.
                    if display.is_inline_block() {
                        new_display = FfiDisplay::outside_and_inside(
                            display_outside::BLOCK,
                            display_inside::FLOW,
                            display.list_item,
                        );
                    } else {
                        new_display =
                            FfiDisplay::outside_and_inside(display_outside::BLOCK, display.inside, display.list_item);
                    }
                }
            }
        }

        FfiBoxTypeTransformation {
            set_float_none,
            changed_display: new_display != display,
            display: new_display,
        }
    })
}

/// Result of resolving the effective overflow pair: each axis' possibly
/// replaced computed keyword.
#[repr(C)]
pub struct FfiEffectiveOverflow {
    pub changed_x: bool,
    pub x_keyword: u16,
    pub changed_y: bool,
    pub y_keyword: u16,
}

/// https://www.w3.org/TR/css-overflow-3/#overflow-control
/// The visible/clip values of overflow compute to auto/hidden (respectively) if one of overflow-x or
/// overflow-y is neither visible nor clip.
#[unsafe(no_mangle)]
pub extern "C" fn rust_resolve_effective_overflow_keywords(overflow_x: u16, overflow_y: u16) -> FfiEffectiveOverflow {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let is_visible_or_clip = |keyword: u16| keyword == keyword::VISIBLE || keyword == keyword::CLIP;
        let mut result = FfiEffectiveOverflow {
            changed_x: false,
            x_keyword: overflow_x,
            changed_y: false,
            y_keyword: overflow_y,
        };
        if !is_visible_or_clip(overflow_x) || !is_visible_or_clip(overflow_y) {
            if overflow_x == keyword::VISIBLE {
                result.changed_x = true;
                result.x_keyword = keyword::AUTO;
            }
            if overflow_x == keyword::CLIP {
                result.changed_x = true;
                result.x_keyword = keyword::HIDDEN;
            }
            if overflow_y == keyword::VISIBLE {
                result.changed_y = true;
                result.y_keyword = keyword::AUTO;
            }
            if overflow_y == keyword::CLIP {
                result.changed_y = true;
                result.y_keyword = keyword::HIDDEN;
            }
        }
        result
    })
}

/// Result of the text-align adjustment: the replacement keyword and whether it
/// counts as inherited.
#[repr(C)]
pub struct FfiTextAlignAdjustment {
    pub changed: bool,
    pub keyword: u16,
    pub inherited: bool,
}

/// Decides the text-align adjustments applied after computation. The parent
/// arguments are only read when `has_parent_with_computed_values` is set.
#[unsafe(no_mangle)]
pub extern "C" fn rust_compute_text_align(
    text_align: u16,
    is_th_element: bool,
    has_parent_with_computed_values: bool,
    parent_text_align: u16,
    parent_direction_is_ltr: bool,
) -> FfiTextAlignAdjustment {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let unchanged = FfiTextAlignAdjustment {
            changed: false,
            keyword: text_align,
            inherited: false,
        };
        let replace = |keyword: u16| FfiTextAlignAdjustment {
            changed: true,
            keyword,
            inherited: false,
        };

        // https://drafts.csswg.org/css-text-4/#valdef-text-align-match-parent
        // This value behaves the same as inherit (computes to its parent's computed value) except that an inherited
        // value of start or end is interpreted against the parent's direction value and results in a computed value of
        // either left or right. Computes to start when specified on the root element.
        if text_align == keyword::MATCH_PARENT {
            if !has_parent_with_computed_values {
                return replace(keyword::START);
            }
            return match parent_text_align {
                keyword::START if parent_direction_is_ltr => replace(keyword::LEFT),
                keyword::START => replace(keyword::RIGHT),
                keyword::END if parent_direction_is_ltr => replace(keyword::RIGHT),
                keyword::END => replace(keyword::LEFT),
                _ => replace(parent_text_align),
            };
        }

        // AD-HOC: The -libweb-inherit-or-center style defaults to centering, unless the parent element has a
        //         non-initial computed text-align value. This is used to support the ad-hoc default <th>
        //         text-align behavior.
        if text_align == keyword::_LIBWEB_INHERIT_OR_CENTER && is_th_element {
            if has_parent_with_computed_values && parent_text_align != keyword::START {
                return FfiTextAlignAdjustment {
                    changed: true,
                    keyword: parent_text_align,
                    inherited: true,
                };
            }
            return replace(keyword::CENTER);
        }

        unchanged
    })
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
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
    extern "C" fn ladybird_style_value_ref(_style_value: *const c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_utf16_fly_string_unref(_raw: usize) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_utf16_fly_string_ref(_raw: usize) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_string_unref(_raw: usize) {}
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
        let math_two_levels = compute_font_size(
            &StyleValueData::Keyword { keyword: keyword::MATH },
            2,
            sixteen,
            0,
            sixteen,
        );
        assert_eq!(
            math_two_levels.value,
            CssPixels::from_integer(16).scaled(0.71f64.powi(2)).to_double()
        );
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
