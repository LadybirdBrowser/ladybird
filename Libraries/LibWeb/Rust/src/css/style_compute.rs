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
//! Element-bound inputs, such as container sizes and tree positions, are
//! snapshotted by C++ before entering the Rust computation drive.

use std::ffi::c_void;
use std::sync::{Arc, OnceLock};

use crate::abort_on_panic;
use crate::css::cascaded_properties::CascadedPropertyStore;
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::css_pixels::CssPixels;
use crate::css::display::FfiDisplay;
use crate::css::property_metadata::longhands_for_shorthand;
use crate::css::property_metadata::property_is_inherited;
use crate::css::property_metadata::property_is_shorthand;
use crate::css::style_value::{GridTrackEntryKind, RetainedStyleValueData, RetainedStyleValueDataList, StyleValueData};

pub use crate::css::css_enums::*;

include!(concat!(env!("OUT_DIR"), "/length_units_generated.rs"));

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
    pub has_container_width_basis: bool,
    pub has_container_height_basis: bool,
    pub container_width_basis: f64,
    pub container_height_basis: f64,
    pub container_width_basis_depends_on_viewport_metrics: bool,
    pub container_height_basis_depends_on_viewport_metrics: bool,
    pub subject_inline_axis_is_horizontal: bool,
    /// Optional flag owned by Length::ResolutionContext, set to true whenever a
    /// resolution here consumed viewport metrics. Callers that report the
    /// dependency through their return value may leave this null.
    pub resolved_viewport_relative_length: *mut bool,
}

/// Records a viewport metric dependency on the context's tracking flag, if one is set.
fn record_viewport_relative_length_resolution(context: &FfiLengthResolutionContext) {
    if context.resolved_viewport_relative_length.is_null() {
        return;
    }
    // SAFETY: The flag belongs to a Length::ResolutionContext that outlives this call.
    unsafe { *context.resolved_viewport_relative_length = true };
}

/// Result of absolutizing a length.
#[repr(C)]
pub struct FfiAbsolutizedLength {
    /// False when the unit cannot be handled with the supplied context; the
    /// caller must fall back to the C++ resolution.
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
    ContainerRelative { axis: ContainerAxis },
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

#[derive(Clone, Copy)]
enum ContainerAxis {
    Width,
    Height,
    Inline,
    Block,
    Min,
    Max,
}

pub(crate) fn px_length_unit() -> u8 {
    static PX: OnceLock<u8> = OnceLock::new();
    *PX.get_or_init(|| LENGTH_UNIT_NAMES.iter().position(|&name| name == "px").unwrap() as u8)
}

pub(crate) fn none_keyword() -> u16 {
    keyword::NONE
}

pub(crate) fn current_color_keyword() -> u16 {
    keyword::CURRENTCOLOR
}

pub(crate) fn absolute_length_to_px(value: f64, unit: u8) -> Option<f64> {
    match length_unit_kinds().get(unit as usize)? {
        LengthUnitKind::Px => Some(value),
        LengthUnitKind::Absolute { px_per_unit } => Some(value * px_per_unit),
        _ => None,
    }
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
                "cqw" => LengthUnitKind::ContainerRelative {
                    axis: ContainerAxis::Width,
                },
                "cqh" => LengthUnitKind::ContainerRelative {
                    axis: ContainerAxis::Height,
                },
                "cqi" => LengthUnitKind::ContainerRelative {
                    axis: ContainerAxis::Inline,
                },
                "cqb" => LengthUnitKind::ContainerRelative {
                    axis: ContainerAxis::Block,
                },
                "cqmin" => LengthUnitKind::ContainerRelative {
                    axis: ContainerAxis::Min,
                },
                "cqmax" => LengthUnitKind::ContainerRelative {
                    axis: ContainerAxis::Max,
                },
                _ if ratio.is_finite() => LengthUnitKind::Absolute { px_per_unit: ratio },
                _ => unreachable!("unknown length unit without an absolute conversion ratio"),
            })
            .collect()
    })
}

/// Whether a length unit is font-relative or container-relative, the two
/// relativities that make a length depend on more than global information.
pub(crate) fn length_unit_is_font_or_container_relative(unit: u8) -> bool {
    matches!(
        length_unit_kinds().get(unit as usize),
        Some(LengthUnitKind::FontRelative { .. } | LengthUnitKind::ContainerRelative { .. })
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

pub(crate) fn absolutize_length(value: f64, unit: usize, context: &FfiLengthResolutionContext) -> FfiAbsolutizedLength {
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
            if depends_on_viewport {
                record_viewport_relative_length_resolution(context);
            }
            FfiAbsolutizedLength {
                handled: true,
                changed: true,
                resolved_viewport_relative_length: depends_on_viewport,
                px: value * select_font_metric(metrics, metric),
            }
        }
        LengthUnitKind::ViewportRelative { axis } => {
            record_viewport_relative_length_resolution(context);
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
        LengthUnitKind::ContainerRelative { axis } => {
            let physical_axis_basis = |width: bool| {
                if width {
                    context.has_container_width_basis.then_some((
                        context.container_width_basis,
                        context.container_width_basis_depends_on_viewport_metrics,
                    ))
                } else {
                    context.has_container_height_basis.then_some((
                        context.container_height_basis,
                        context.container_height_basis_depends_on_viewport_metrics,
                    ))
                }
            };
            let inline_is_width = context.subject_inline_axis_is_horizontal;
            let basis = match axis {
                ContainerAxis::Width => physical_axis_basis(true),
                ContainerAxis::Height => physical_axis_basis(false),
                ContainerAxis::Inline => physical_axis_basis(inline_is_width),
                ContainerAxis::Block => physical_axis_basis(!inline_is_width),
                ContainerAxis::Min | ContainerAxis::Max => {
                    match (physical_axis_basis(true), physical_axis_basis(false)) {
                        (Some((width, width_depends_on_viewport)), Some((height, height_depends_on_viewport))) => {
                            Some((
                                if matches!(axis, ContainerAxis::Min) {
                                    width.min(height)
                                } else {
                                    width.max(height)
                                },
                                width_depends_on_viewport || height_depends_on_viewport,
                            ))
                        }
                        _ => None,
                    }
                }
            };
            let Some((basis, depends_on_viewport)) = basis else {
                return unhandled;
            };
            if depends_on_viewport {
                record_viewport_relative_length_resolution(context);
            }
            FfiAbsolutizedLength {
                handled: true,
                changed: true,
                resolved_viewport_relative_length: depends_on_viewport,
                px: basis * value / 100.0,
            }
        }
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
        // core cannot resolve is reported as unhandled.
        StyleValueData::Calculated { .. } => match crate::css::calc::resolve_calculated_number_without_context(value) {
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
        // the core cannot resolve is reported as unhandled.
        StyleValueData::Calculated { .. } => {
            match crate::css::calc::resolve_calculated_percentage_without_context(value) {
                Some(resolved) => computed(resolved),
                None => NUMBER_UNHANDLED,
            }
        }
        _ => NUMBER_UNHANDLED,
    }
}

/// Computes the font-width property from its absolutized value.
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_font_width(absolutized_value: *const c_void) -> FfiComputedNumber {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
        // font size; anything the core cannot resolve is reported as unhandled.
        StyleValueData::Calculated { .. } => {
            match crate::css::calc::resolve_calculated_length_without_context(value, inherited_font_size.to_double()) {
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    use crate::css::selector::PseudoElementType;
    abort_on_panic(|| {
        matches!(
            crate::css::selector::pseudo_element_type_from_code(pseudo_element),
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    use crate::css::selector::PseudoElementType;
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
                crate::css::selector::pseudo_element_type_from_code(pseudo_element),
                PseudoElementType::Before | PseudoElementType::After
            )
    })
}

/// https://drafts.css-houdini.org/css-properties-values-api/#computationally-independent
/// A property value is computationally independent if it can be converted into a computed value
/// using only the value of the property on the element, and "global" information that cannot be
/// changed by CSS.
///
/// Returns None for value types that must not reach computational-independence checks. A
/// container is likewise undecided when any nested value is unsupported.
pub(crate) fn value_is_computationally_independent(value: &StyleValueData) -> Option<bool> {
    fn grid_entries_are_computationally_independent(
        entries: &[crate::css::style_value::RetainedGridTrackEntry],
    ) -> Option<bool> {
        for entry in entries {
            let independent = match entry.kind {
                // A line-name entry carries no style value.
                GridTrackEntryKind::LineNames => true,
                // A single track size.
                GridTrackEntryKind::Size => value_is_computationally_independent(entry.size_value.data())?,
                // A minmax() track size.
                GridTrackEntryKind::MinMax => {
                    value_is_computationally_independent(entry.min_value.data())?
                        && value_is_computationally_independent(entry.max_value.data())?
                }
                // A repeat() track and its optional fixed repeat count.
                GridTrackEntryKind::Repeat => {
                    grid_entries_are_computationally_independent(entry.repeat_entries())?
                        && match entry.repeat_count.optional_data() {
                            Some(count) => value_is_computationally_independent(count)?,
                            None => true,
                        }
                }
            };
            if !independent {
                return Some(false);
            }
        }
        Some(true)
    }

    let all_data_in_list = |list: &crate::css::style_value::RetainedStyleValueDataList| -> Option<bool> {
        let mut independent = true;
        for retained in list.as_slice() {
            if let Some(data) = retained.optional_data() {
                independent = independent && value_is_computationally_independent(data)?;
            }
        }
        Some(independent)
    };
    let all_data = |children: &[&crate::css::style_value::RetainedStyleValueData]| -> Option<bool> {
        let mut independent = true;
        for retained in children {
            independent = independent && value_is_computationally_independent(retained.data())?;
        }
        Some(independent)
    };
    match value {
        StyleValueData::Keyword { keyword } => {
            if value_is_css_wide_keyword(value) {
                return Some(false);
            }
            if *keyword != keyword::CURRENTCOLOR && keyword_is_color(*keyword) {
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
                &|retained| value_is_computationally_independent(retained.data()).unwrap_or_default(),
            ))
        }
        StyleValueData::Ratio {
            numerator, denominator, ..
        } => all_data(&[numerator, denominator]),
        StyleValueData::Edge { offset, .. } => match offset.optional_data() {
            Some(offset) => value_is_computationally_independent(offset),
            None => Some(true),
        },
        StyleValueData::Function { value, .. } => all_data(&[value]),
        StyleValueData::OpacityValue { value } => all_data(&[value]),
        // Auto placements carry no value; spans and lines recurse into theirs.
        StyleValueData::GridTrackPlacement { value, .. } => match value.optional_data() {
            Some(value) => value_is_computationally_independent(value),
            None => Some(true),
        },
        StyleValueData::GridTrackSizeList { entries, .. } => {
            grid_entries_are_computationally_independent(entries.as_slice())
        }
        // FIXME: Consider sub-values once we support <custom-color-space> values
        StyleValueData::ColorInterpolationMethod { .. } => Some(true),
        StyleValueData::ColorFunction {
            channel_0,
            channel_1,
            channel_2,
            alpha,
            origin_color,
            ..
        } => {
            let mut independent = true;
            for value in [channel_0, channel_1, channel_2, alpha, origin_color] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            Some(independent)
        }
        StyleValueData::BorderImageSlice {
            top,
            right,
            bottom,
            left,
            ..
        } => all_data(&[top, right, bottom, left]),
        StyleValueData::Content { content, alt_text } => {
            let mut independent = value_is_computationally_independent(content.data())?;
            if let Some(alt_text) = alt_text.optional_data() {
                independent = independent && value_is_computationally_independent(alt_text)?;
            }
            Some(independent)
        }
        // Extent components carry no value; explicit sizes recurse into theirs.
        StyleValueData::RadialSize {
            component_count,
            is_extent_0,
            value_0,
            is_extent_1,
            value_1,
            ..
        } => {
            let mut independent = true;
            if !is_extent_0 {
                independent = value_is_computationally_independent(value_0.data())?;
            }
            if *component_count == 2 && !is_extent_1 {
                independent = independent && value_is_computationally_independent(value_1.data())?;
            }
            Some(independent)
        }
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
            let mut independent = true;
            for value in [v0, v1, v2, v3, v4] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            for point in points.as_slice() {
                for value in point.values() {
                    independent = independent && value_is_computationally_independent(value.data())?;
                }
            }
            Some(independent)
        }
        // Every filter kind's rule recurses into its single value.
        StyleValueData::Filter { value, .. } => all_data(&[value]),
        StyleValueData::Counter { counter_style, .. } => all_data(&[counter_style]),
        StyleValueData::OpenTypeTagged { value, .. } => all_data(&[value]),
        StyleValueData::RandomValueSharing { fixed_value, .. } => match fixed_value.optional_data() {
            Some(fixed_value) => value_is_computationally_independent(fixed_value),
            None => Some(true),
        },
        StyleValueData::Cursor { image, x, y } => {
            let mut independent = value_is_computationally_independent(image.data())?;
            for coordinate in [x, y] {
                if let Some(coordinate) = coordinate.optional_data() {
                    independent = independent && value_is_computationally_independent(coordinate)?;
                }
            }
            Some(independent)
        }
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
            let mut independent = true;
            for value in [x1, y1, x2, y2, number_of_intervals] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            for stop in linear_stops.as_slice() {
                for value in stop.values() {
                    if let Some(value) = value.optional_data() {
                        independent = independent && value_is_computationally_independent(value)?;
                    }
                }
            }
            Some(independent)
        }
        StyleValueData::ImageSet { options } => {
            let mut independent = true;
            for option in options.as_slice() {
                independent = independent && all_data(&option.values())?;
            }
            Some(independent)
        }
        StyleValueData::CounterDefinitions { counter_definitions } => {
            let mut independent = true;
            for definition in counter_definitions.as_slice() {
                if let Some(value) = definition.value().optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            Some(independent)
        }
        StyleValueData::LinearGradient {
            direction_value,
            color_stop_list,
            color_interpolation_method,
            ..
        } => {
            let mut independent = true;
            for value in [direction_value, color_interpolation_method] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            for stop in color_stop_list.as_slice() {
                for value in stop.values() {
                    if let Some(value) = value.optional_data() {
                        independent = independent && value_is_computationally_independent(value)?;
                    }
                }
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
            let mut independent = true;
            for value in [from_angle, position, color_interpolation_method] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            for stop in color_stop_list.as_slice() {
                for value in stop.values() {
                    if let Some(value) = value.optional_data() {
                        independent = independent && value_is_computationally_independent(value)?;
                    }
                }
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
            let mut independent = true;
            for value in [size, position, color_interpolation_method] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            for stop in color_stop_list.as_slice() {
                for value in stop.values() {
                    if let Some(value) = value.optional_data() {
                        independent = independent && value_is_computationally_independent(value)?;
                    }
                }
            }
            Some(independent)
        }
        StyleValueData::ContrastColor { color, .. } => all_data(&[color]),
        StyleValueData::Superellipse { parameter } => all_data(&[parameter]),
        StyleValueData::ScrollbarColor {
            thumb_color,
            track_color,
            ..
        } => all_data(&[thumb_color, track_color]),
        StyleValueData::Rect {
            top,
            right,
            bottom,
            left,
            ..
        } => all_data(&[top, right, bottom, left]),
        StyleValueData::FontStyle { angle_value, .. } => match angle_value.optional_data() {
            Some(angle_value) => value_is_computationally_independent(angle_value),
            None => Some(true),
        },
        StyleValueData::TextIndent { length_percentage, .. } => all_data(&[length_percentage]),
        StyleValueData::OverflowClipMargin { offset, .. } => all_data(&[offset]),
        StyleValueData::BackgroundSize { size_x, size_y, .. } => all_data(&[size_x, size_y]),
        StyleValueData::Position { edge_x, edge_y, .. } => all_data(&[edge_x, edge_y]),
        StyleValueData::Shadow {
            color,
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            ..
        } => {
            let mut independent = value_is_computationally_independent(offset_x.data())?
                && value_is_computationally_independent(offset_y.data())?;
            for value in [color, blur_radius, spread_distance] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            Some(independent)
        }
        StyleValueData::ColorMix {
            color_interpolation_method,
            first_color,
            first_percentage,
            second_color,
            second_percentage,
            ..
        } => {
            let mut independent = true;
            for value in [
                color_interpolation_method,
                first_color,
                first_percentage,
                second_color,
                second_percentage,
            ] {
                if let Some(value) = value.optional_data() {
                    independent = independent && value_is_computationally_independent(value)?;
                }
            }
            Some(independent)
        }
        StyleValueData::Shorthand { values, .. }
        | StyleValueData::ValueList { values, .. }
        | StyleValueData::Tuple { values }
        | StyleValueData::Transformation { values, .. } => all_data_in_list(values),
        StyleValueData::BorderRadiusRect {
            top_left,
            top_right,
            bottom_right,
            bottom_left,
            ..
        } => all_data(&[top_left, top_right, bottom_right, bottom_left]),
        StyleValueData::BorderRadius {
            horizontal_radius,
            vertical_radius,
            ..
        } => all_data(&[horizontal_radius, vertical_radius]),
        _ => None,
    }
}

#[derive(Clone, Copy, Default)]
pub(crate) struct ExternalValueDependencies {
    pub uses_tree_counting_function: bool,
    pub container_relative_length_unit_mask: u8,
    pub has_unfixed_random_sharing: bool,
    pub needs_document_base_url: bool,
    pub inheritance_dependent: bool,
}

fn container_relative_length_unit_bit(unit: u8) -> u8 {
    match LENGTH_UNIT_NAMES[unit as usize] {
        "cqw" => 1 << 0,
        "cqh" => 1 << 1,
        "cqi" => 1 << 2,
        "cqb" => 1 << 3,
        "cqmin" => 1 << 4,
        "cqmax" => 1 << 5,
        _ => 0,
    }
}

pub(crate) fn external_value_dependencies(value: &StyleValueData) -> ExternalValueDependencies {
    fn collect_optional(value: &RetainedStyleValueData, dependencies: &mut ExternalValueDependencies) {
        if let Some(value) = value.optional_data() {
            collect(value, dependencies);
        }
    }

    fn collect_values(values: &[&RetainedStyleValueData], dependencies: &mut ExternalValueDependencies) {
        for value in values {
            collect_optional(value, dependencies);
        }
    }

    fn collect_grid_entries(
        entries: &[crate::css::style_value::RetainedGridTrackEntry],
        dependencies: &mut ExternalValueDependencies,
    ) {
        for entry in entries {
            match entry.kind {
                GridTrackEntryKind::LineNames => {}
                GridTrackEntryKind::Size => collect(entry.size_value.data(), dependencies),
                GridTrackEntryKind::MinMax => {
                    collect(entry.min_value.data(), dependencies);
                    collect(entry.max_value.data(), dependencies);
                }
                GridTrackEntryKind::Repeat => {
                    collect_grid_entries(entry.repeat_entries(), dependencies);
                    collect_optional(&entry.repeat_count, dependencies);
                }
            }
        }
    }

    fn collect_calculation(node: &crate::css::calc::CalcNode, dependencies: &mut ExternalValueDependencies) {
        match node {
            crate::css::calc::CalcNode::Numeric(crate::css::calc::CalcNumericValue::Length { unit, .. }) => {
                dependencies.container_relative_length_unit_mask |= container_relative_length_unit_bit(*unit);
            }
            crate::css::calc::CalcNode::Random { sharing, .. }
            | crate::css::calc::CalcNode::NonMathFunction { value: sharing, .. } => {
                collect(sharing.data(), dependencies);
            }
            _ => {}
        }
        node.for_each_child(&mut |child| collect_calculation(child, dependencies));
    }

    fn collect(value: &StyleValueData, dependencies: &mut ExternalValueDependencies) {
        match value {
            StyleValueData::TreeCountingFunction { .. } => dependencies.uses_tree_counting_function = true,
            StyleValueData::Length { unit, .. } => {
                dependencies.container_relative_length_unit_mask |= container_relative_length_unit_bit(*unit);
            }
            StyleValueData::Image {
                url, resource_context, ..
            } => {
                dependencies.needs_document_base_url |= !url.as_bytes().is_empty() && !resource_context.has_base_url;
            }
            StyleValueData::Calculated { rust_calculation, .. } => {
                collect_calculation(rust_calculation.node(), dependencies);
            }
            StyleValueData::Ratio {
                numerator, denominator, ..
            } => {
                collect_values(&[numerator, denominator], dependencies);
            }
            StyleValueData::Edge { offset, .. } => collect_optional(offset, dependencies),
            StyleValueData::Function { value, .. }
            | StyleValueData::OpacityValue { value }
            | StyleValueData::Filter { value, .. }
            | StyleValueData::OpenTypeTagged { value, .. }
            | StyleValueData::GridTrackPlacement { value, .. } => collect_optional(value, dependencies),
            StyleValueData::GridTrackSizeList { entries, .. } => {
                collect_grid_entries(entries.as_slice(), dependencies);
            }
            StyleValueData::ColorFunction {
                channel_0,
                channel_1,
                channel_2,
                alpha,
                origin_color,
                ..
            } => collect_values(&[channel_0, channel_1, channel_2, alpha, origin_color], dependencies),
            StyleValueData::BorderImageSlice {
                top,
                right,
                bottom,
                left,
                ..
            }
            | StyleValueData::Rect {
                top,
                right,
                bottom,
                left,
            } => {
                collect_values(&[top, right, bottom, left], dependencies);
            }
            StyleValueData::Content { content, alt_text } => {
                collect(content.data(), dependencies);
                collect_optional(alt_text, dependencies);
            }
            StyleValueData::RadialSize { value_0, value_1, .. } => {
                collect_values(&[value_0, value_1], dependencies);
            }
            StyleValueData::BasicShape {
                v0,
                v1,
                v2,
                v3,
                v4,
                points,
                ..
            } => {
                collect_values(&[v0, v1, v2, v3, v4], dependencies);
                for point in points.as_slice() {
                    for value in point.values() {
                        collect_optional(value, dependencies);
                    }
                }
            }
            StyleValueData::Counter { counter_style, .. } => collect(counter_style.data(), dependencies),
            StyleValueData::RandomValueSharing { fixed_value, .. } => {
                if fixed_value.optional_data().is_some() {
                    collect_optional(fixed_value, dependencies);
                } else {
                    dependencies.has_unfixed_random_sharing = true;
                }
            }
            StyleValueData::Cursor { image, x, y } => {
                collect(image.data(), dependencies);
                collect_values(&[x, y], dependencies);
            }
            StyleValueData::Easing {
                linear_stops,
                x1,
                y1,
                x2,
                y2,
                number_of_intervals,
                ..
            } => {
                collect_values(&[x1, y1, x2, y2, number_of_intervals], dependencies);
                for stop in linear_stops.as_slice() {
                    for value in stop.values() {
                        collect_optional(value, dependencies);
                    }
                }
            }
            StyleValueData::ImageSet { options } => {
                for option in options.as_slice() {
                    for value in option.values() {
                        collect_optional(value, dependencies);
                    }
                }
            }
            StyleValueData::CounterDefinitions { counter_definitions } => {
                for definition in counter_definitions.as_slice() {
                    collect_optional(definition.value(), dependencies);
                }
            }
            StyleValueData::LinearGradient {
                direction_value,
                color_stop_list,
                color_interpolation_method,
                ..
            } => {
                collect_values(&[direction_value, color_interpolation_method], dependencies);
                for stop in color_stop_list.as_slice() {
                    for value in stop.values() {
                        collect_optional(value, dependencies);
                    }
                }
            }
            StyleValueData::ConicGradient {
                from_angle,
                position,
                color_stop_list,
                color_interpolation_method,
                ..
            } => {
                collect_values(&[from_angle, position, color_interpolation_method], dependencies);
                for stop in color_stop_list.as_slice() {
                    for value in stop.values() {
                        collect_optional(value, dependencies);
                    }
                }
            }
            StyleValueData::RadialGradient {
                size,
                position,
                color_stop_list,
                color_interpolation_method,
                ..
            } => {
                collect_values(&[size, position, color_interpolation_method], dependencies);
                for stop in color_stop_list.as_slice() {
                    for value in stop.values() {
                        collect_optional(value, dependencies);
                    }
                }
            }
            StyleValueData::ColorMix {
                color_interpolation_method,
                first_color,
                first_percentage,
                second_color,
                second_percentage,
                ..
            } => collect_values(
                &[
                    color_interpolation_method,
                    first_color,
                    first_percentage,
                    second_color,
                    second_percentage,
                ],
                dependencies,
            ),
            StyleValueData::ContrastColor { color, .. } => collect(color.data(), dependencies),
            StyleValueData::LightDark { light, dark, .. } => collect_values(&[light, dark], dependencies),
            StyleValueData::Superellipse { parameter } => collect_optional(parameter, dependencies),
            StyleValueData::ScrollbarColor {
                thumb_color,
                track_color,
            } => {
                collect_values(&[thumb_color, track_color], dependencies);
            }
            StyleValueData::FontStyle { angle_value, .. } => collect_optional(angle_value, dependencies),
            StyleValueData::TextIndent { length_percentage, .. } => {
                collect_optional(length_percentage, dependencies);
            }
            StyleValueData::OverflowClipMargin { offset, .. } => collect_optional(offset, dependencies),
            StyleValueData::BackgroundSize { size_x, size_y } => collect_values(&[size_x, size_y], dependencies),
            StyleValueData::Position { edge_x, edge_y } => collect_values(&[edge_x, edge_y], dependencies),
            StyleValueData::Shadow {
                color,
                offset_x,
                offset_y,
                blur_radius,
                spread_distance,
                ..
            } => {
                collect_values(&[color, offset_x, offset_y, blur_radius, spread_distance], dependencies);
            }
            StyleValueData::Shorthand { values, .. }
            | StyleValueData::ValueList { values, .. }
            | StyleValueData::Tuple { values }
            | StyleValueData::Transformation { values, .. } => {
                for value in values.as_slice() {
                    collect_optional(value, dependencies);
                }
            }
            StyleValueData::BorderRadiusRect {
                top_left,
                top_right,
                bottom_right,
                bottom_left,
            } => {
                collect_values(&[top_left, top_right, bottom_right, bottom_left], dependencies);
            }
            StyleValueData::BorderRadius {
                horizontal_radius,
                vertical_radius,
                ..
            } => {
                collect_values(&[horizontal_radius, vertical_radius], dependencies);
            }
            _ => {}
        }
    }

    let mut dependencies = ExternalValueDependencies::default();
    collect(value, &mut dependencies);
    dependencies.inheritance_dependent = crate::css::style_value::value_depends_on_current_color(value)
        || !value_is_computationally_independent(value)
            .expect("computational independence requested for an unsupported value");
    dependencies
}

/// Collects the element- or document-cached random sharing nodes reachable through the
/// recursively absolutized style-value graph. Keep this aligned with new structural variants
/// and with `absolutize::absolutize`.
pub(crate) fn collect_unfixed_random_sharings_in_value(
    value: &StyleValueData,
    sharings: &mut Vec<*const StyleValueData>,
) {
    fn collect_optional(value: &RetainedStyleValueData, sharings: &mut Vec<*const StyleValueData>) {
        if let Some(value) = value.optional_data() {
            collect_unfixed_random_sharings_in_value(value, sharings);
        }
    }

    fn collect_list(values: &RetainedStyleValueDataList, sharings: &mut Vec<*const StyleValueData>) {
        for value in values.as_slice() {
            collect_optional(value, sharings);
        }
    }

    let collect_values = |values: &[&RetainedStyleValueData], sharings: &mut Vec<*const StyleValueData>| {
        for value in values {
            collect_optional(value, sharings);
        }
    };
    match value {
        StyleValueData::Calculated { rust_calculation, .. } => {
            crate::css::calc::collect_unfixed_random_sharings(rust_calculation.node(), sharings);
        }
        StyleValueData::Ratio { numerator, denominator } => collect_values(&[numerator, denominator], sharings),
        StyleValueData::Edge { offset, .. } => collect_values(&[offset], sharings),
        StyleValueData::Function { value, .. }
        | StyleValueData::OpacityValue { value }
        | StyleValueData::Filter { value, .. }
        | StyleValueData::OpenTypeTagged { value, .. }
        | StyleValueData::GridTrackPlacement { value, .. } => collect_values(&[value], sharings),
        StyleValueData::ColorFunction {
            channel_0,
            channel_1,
            channel_2,
            alpha,
            origin_color,
            ..
        } => collect_values(&[channel_0, channel_1, channel_2, alpha, origin_color], sharings),
        StyleValueData::BorderImageSlice {
            top,
            right,
            bottom,
            left,
            ..
        }
        | StyleValueData::Rect {
            top,
            right,
            bottom,
            left,
        } => collect_values(&[top, right, bottom, left], sharings),
        StyleValueData::Content { content, alt_text } => collect_values(&[content, alt_text], sharings),
        StyleValueData::RadialSize { value_0, value_1, .. } => collect_values(&[value_0, value_1], sharings),
        StyleValueData::BasicShape {
            v0,
            v1,
            v2,
            v3,
            v4,
            points,
            ..
        } => {
            collect_values(&[v0, v1, v2, v3, v4], sharings);
            for point in points.as_slice() {
                collect_values(&point.values(), sharings);
            }
        }
        StyleValueData::Counter { counter_style, .. } => collect_values(&[counter_style], sharings),
        StyleValueData::Cursor { image, x, y } => collect_values(&[image, x, y], sharings),
        StyleValueData::Easing {
            linear_stops,
            x1,
            y1,
            x2,
            y2,
            number_of_intervals,
            ..
        } => {
            collect_values(&[x1, y1, x2, y2, number_of_intervals], sharings);
            for stop in linear_stops.as_slice() {
                collect_values(&stop.values(), sharings);
            }
        }
        StyleValueData::ImageSet { options } => {
            for option in options.as_slice() {
                collect_values(&option.values(), sharings);
            }
        }
        StyleValueData::CounterDefinitions { counter_definitions } => {
            for definition in counter_definitions.as_slice() {
                collect_optional(definition.value(), sharings);
            }
        }
        StyleValueData::LinearGradient {
            direction_value,
            color_stop_list,
            color_interpolation_method,
            ..
        } => {
            collect_values(&[direction_value, color_interpolation_method], sharings);
            for stop in color_stop_list.as_slice() {
                collect_values(&stop.values(), sharings);
            }
        }
        StyleValueData::ConicGradient {
            from_angle,
            position,
            color_stop_list,
            color_interpolation_method,
            ..
        } => {
            collect_values(&[from_angle, position, color_interpolation_method], sharings);
            for stop in color_stop_list.as_slice() {
                collect_values(&stop.values(), sharings);
            }
        }
        StyleValueData::RadialGradient {
            size,
            position,
            color_stop_list,
            color_interpolation_method,
            ..
        } => {
            collect_values(&[size, position, color_interpolation_method], sharings);
            for stop in color_stop_list.as_slice() {
                collect_values(&stop.values(), sharings);
            }
        }
        StyleValueData::ColorMix {
            color_interpolation_method,
            first_color,
            first_percentage,
            second_color,
            second_percentage,
            ..
        } => collect_values(
            &[
                color_interpolation_method,
                first_color,
                first_percentage,
                second_color,
                second_percentage,
            ],
            sharings,
        ),
        StyleValueData::ContrastColor { color, .. } => collect_values(&[color], sharings),
        StyleValueData::LightDark { light, dark, .. } => collect_values(&[light, dark], sharings),
        StyleValueData::Superellipse { parameter } => collect_values(&[parameter], sharings),
        StyleValueData::ScrollbarColor {
            thumb_color,
            track_color,
        } => collect_values(&[thumb_color, track_color], sharings),
        StyleValueData::FontStyle { angle_value, .. } => collect_values(&[angle_value], sharings),
        StyleValueData::TextIndent { length_percentage, .. } => collect_values(&[length_percentage], sharings),
        StyleValueData::OverflowClipMargin { offset, .. } => collect_values(&[offset], sharings),
        StyleValueData::BackgroundSize { size_x, size_y } => collect_values(&[size_x, size_y], sharings),
        StyleValueData::Position { edge_x, edge_y } => collect_values(&[edge_x, edge_y], sharings),
        StyleValueData::Shadow {
            color,
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            ..
        } => collect_values(&[color, offset_x, offset_y, blur_radius, spread_distance], sharings),
        StyleValueData::Shorthand { values, .. }
        | StyleValueData::ValueList { values, .. }
        | StyleValueData::Tuple { values }
        | StyleValueData::Transformation { values, .. } => collect_list(values, sharings),
        StyleValueData::BorderRadiusRect {
            top_left,
            top_right,
            bottom_right,
            bottom_left,
        } => collect_values(&[top_left, top_right, bottom_right, bottom_left], sharings),
        StyleValueData::BorderRadius {
            horizontal_radius,
            vertical_radius,
            ..
        } => collect_values(&[horizontal_radius, vertical_radius], sharings),
        _ => {}
    }
}

/// # Safety
/// `data` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_is_computationally_independent(data: *const c_void) -> bool {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueQueryEntry);
    abort_on_panic(|| {
        value_is_computationally_independent(unsafe { &*(data as *const StyleValueData) })
            .expect("computational independence requested for an unsupported value")
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
        // anything the core cannot resolve is reported as unhandled.
        StyleValueData::Calculated { .. } => {
            match crate::css::calc::resolve_calculated_integer_without_context(value) {
                Some(int) => computed(int),
                None => NUMBER_UNHANDLED,
            }
        }
        // - If the specified value of math-depth is of the form add(<integer>) then the computed
        //   value of math-depth of the element is its inherited value plus the specified integer.
        StyleValueData::Function { value, .. } => {
            let Some(value) = value.optional_data() else {
                return NUMBER_UNHANDLED;
            };
            let added = match value {
                StyleValueData::Integer { value } => Some(*value),
                StyleValueData::Calculated { .. } => {
                    crate::css::calc::resolve_calculated_integer_without_context(value)
                }
                _ => None,
            };
            match added {
                Some(added) => computed(inherited_math_depth.saturating_add(added)),
                None => NUMBER_UNHANDLED,
            }
        }
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
            match crate::css::calc::resolve_calculated_line_height_without_context(
                value,
                computed_font_size.to_double(),
            ) {
                Some(crate::css::calc::ResolvedLineHeightCalc::Px(px)) => FfiComputedLineHeight {
                    handled: true,
                    unchanged: false,
                    is_number: false,
                    value: px,
                },
                Some(crate::css::calc::ResolvedLineHeightCalc::Number(number)) => FfiComputedLineHeight {
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_line_height(value, CssPixels::from_raw(computed_font_size_raw))
    })
}

// https://drafts.csswg.org/css-backgrounds/#border-width
// absolute length, snapped as a border width
fn compute_border_or_outline_width(
    value: &StyleValueData,
    device_pixels_per_css_pixel: f64,
    length_resolution_context: Option<&FfiLengthResolutionContext>,
) -> FfiComputedNumber {
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
        StyleValueData::Calculated { .. } => {
            let Some(length_resolution_context) = length_resolution_context else {
                return NUMBER_UNHANDLED;
            };
            let Some(px) = crate::css::calc::resolve_calculated_length_with_context(value, length_resolution_context)
            else {
                return NUMBER_UNHANDLED;
            };
            CssPixels::nearest_value_for(px)
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_border_or_outline_width(value, device_pixels_per_css_pixel, None)
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_corner_shape_parameter(value)
    })
}

/// Whether a font-family value is a single monospace keyword, which triggers
/// the monospace font-size recascade. The list entry's keyword is read through
/// the nested value's shared Rust data handle.
///
/// # Safety
/// `data` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_family_is_monospace(data: *const c_void) -> bool {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let StyleValueData::ValueList { values, .. } = (unsafe { &*(data as *const StyleValueData) }) else {
            return false;
        };
        let values = values.as_slice();
        if values.len() != 1 {
            return false;
        }
        matches!(
            values[0].data(),
            StyleValueData::Keyword { keyword } if *keyword == keyword::MONOSPACE
        )
    })
}

/// Computes a font-feature-settings or font-variation-settings value list:
/// deduplicate by tag with the later occurrence taking precedence, then sort
/// the survivors ascending by tag.
/// https://drafts.csswg.org/css-fonts/#font-feature-settings-prop
#[allow(clippy::arc_with_non_send_sync)]
fn compute_font_feature_tag_value_list(value: &StyleValueData) -> Arc<StyleValueData> {
    let StyleValueData::ValueList {
        values,
        separator,
        collapsible,
    } = value
    else {
        unreachable!("font feature settings must be a value list")
    };
    let values = values.as_slice();
    let packed_tag = |index: usize| match values[index].data() {
        StyleValueData::OpenTypeTagged { packed_tag, .. } => *packed_tag,
        _ => unreachable!("font feature settings must contain OpenType tagged values"),
    };

    // Keep the last occurrence of each tag; later declarations take precedence.
    let mut survivors: Vec<usize> = (0..values.len())
        .filter(|&i| !((i + 1)..values.len()).any(|j| packed_tag(i) == packed_tag(j)))
        .collect();
    survivors.sort_unstable_by_key(|&index| packed_tag(index));
    let values = survivors
        .into_iter()
        .map(|index| values[index].clone_retained())
        .collect();
    Arc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(values),
        separator: *separator,
        collapsible: *collapsible,
    })
}

/// Computes a font-feature-settings or font-variation-settings value list:
/// deduplicate by tag with the later occurrence taking precedence, then sort
/// the survivors ascending by tag.
/// https://drafts.csswg.org/css-fonts/#font-feature-settings-prop
///
/// # Safety
/// `data` must point at a value list of OpenType tagged values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_font_feature_settings(data: *const c_void) -> *const c_void {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        Arc::into_raw(compute_font_feature_tag_value_list(unsafe {
            &*data.cast::<StyleValueData>()
        }))
        .cast()
    })
}

/// Computes a transform-origin value list over an already absolutized value.
/// A null return means the absolutized value is already the computed value.
/// https://drafts.csswg.org/css-transforms/#transform-origin-property
///
/// # Safety
/// `absolutized_value` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compute_transform_origin(absolutized_value: *const c_void) -> *const c_void {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(
        || match compute_transform_origin(unsafe { &*absolutized_value.cast::<StyleValueData>() }) {
            Some(value) => Arc::into_raw(value).cast(),
            None => std::ptr::null(),
        },
    )
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
    use crate::css::property_metadata::property_id as prop;
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueQueryEntry);
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
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

fn position_area_short_keyword(keyword: u16) -> u16 {
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

fn position_area_span_all_remap(block_keyword: u16, inline_keyword: u16) -> Option<u16> {
    if block_keyword == keyword::SPAN_ALL {
        return match inline_keyword {
            keyword::START => Some(keyword::INLINE_START),
            keyword::END => Some(keyword::INLINE_END),
            keyword::SELF_START => Some(keyword::SELF_INLINE_START),
            keyword::SELF_END => Some(keyword::SELF_INLINE_END),
            keyword::SPAN_START => Some(keyword::SPAN_INLINE_START),
            keyword::SPAN_END => Some(keyword::SPAN_INLINE_END),
            keyword::SPAN_SELF_START => Some(keyword::SPAN_SELF_INLINE_START),
            keyword::SPAN_SELF_END => Some(keyword::SPAN_SELF_INLINE_END),
            _ => None,
        };
    }
    if inline_keyword == keyword::SPAN_ALL {
        return match block_keyword {
            keyword::START => Some(keyword::BLOCK_START),
            keyword::END => Some(keyword::BLOCK_END),
            keyword::SELF_START => Some(keyword::SELF_BLOCK_START),
            keyword::SELF_END => Some(keyword::SELF_BLOCK_END),
            keyword::SPAN_START => Some(keyword::SPAN_BLOCK_START),
            keyword::SPAN_END => Some(keyword::SPAN_BLOCK_END),
            keyword::SPAN_SELF_START => Some(keyword::SPAN_SELF_BLOCK_START),
            keyword::SPAN_SELF_END => Some(keyword::SPAN_SELF_BLOCK_END),
            _ => None,
        };
    }
    None
}

// https://drafts.csswg.org/css-anchor-position/#position-area-computed
#[allow(clippy::arc_with_non_send_sync)]
fn compute_position_area(value: &StyleValueData) -> Option<Arc<StyleValueData>> {
    // The computed value of a <position-area> value is the two keywords indicating the selected tracks in each axis,
    // with the long (block-start) and short (start) logical keywords treated as equivalent. It serializes in the order
    // given in the grammar (above), with the logical keywords serialized in their short forms (e.g. start start
    // instead of block-start inline-start).
    let StyleValueData::ValueList {
        values,
        separator,
        collapsible,
    } = value
    else {
        return None;
    };
    let [block_value, inline_value] = values.as_slice() else {
        unreachable!("position-area must contain two keywords")
    };
    let StyleValueData::Keyword { keyword: block_keyword } = block_value.data() else {
        unreachable!("position-area must contain keywords")
    };
    let StyleValueData::Keyword {
        keyword: inline_keyword,
    } = inline_value.data()
    else {
        unreachable!("position-area must contain keywords")
    };

    if let Some(keyword) = position_area_span_all_remap(*block_keyword, *inline_keyword) {
        return Some(Arc::new(StyleValueData::Keyword { keyword }));
    }
    let short_block_keyword = position_area_short_keyword(*block_keyword);
    let short_inline_keyword = position_area_short_keyword(*inline_keyword);
    if short_block_keyword == *block_keyword && short_inline_keyword == *inline_keyword {
        return None;
    }

    let retained_keyword = |keyword| unsafe {
        RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(StyleValueData::Keyword { keyword })))
    };
    Some(Arc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(vec![
            retained_keyword(short_block_keyword),
            retained_keyword(short_inline_keyword),
        ]),
        separator: *separator,
        collapsible: *collapsible,
    }))
}

/// The computed dash list, with each number converted to the length it measures in user units.
/// None when the list holds no numbers, so an unchanged list keeps its identity.
#[allow(clippy::arc_with_non_send_sync)]
fn stroke_dasharray_numbers_as_lengths(value: &StyleValueData) -> Option<Arc<StyleValueData>> {
    let StyleValueData::ValueList {
        values,
        separator,
        collapsible,
    } = value
    else {
        return None;
    };
    let dashes = values.as_slice();
    if !dashes
        .iter()
        .any(|dash| matches!(dash.data(), StyleValueData::Number { .. }))
    {
        return None;
    }
    let computed_dashes = dashes
        .iter()
        .map(|dash| match dash.data() {
            StyleValueData::Number { value } => unsafe {
                RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(StyleValueData::Length {
                    value: *value,
                    unit: px_length_unit(),
                })))
            },
            _ => dash.clone_retained(),
        })
        .collect();
    Some(Arc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(computed_dashes),
        separator: *separator,
        collapsible: *collapsible,
    }))
}

/// The computed two-value list for a `border-spacing` member used for both axes.
#[allow(clippy::arc_with_non_send_sync)]
fn border_spacing_pair(single: StyleValueData) -> Arc<StyleValueData> {
    let single = unsafe { RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(single))) };
    Arc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(vec![single.clone_retained(), single]),
        separator: 0,
        collapsible: true,
    })
}

// https://drafts.csswg.org/css-contain-2/#contain-property
#[allow(clippy::arc_with_non_send_sync)]
fn collapse_containment_list(value: &StyleValueData) -> Option<Arc<StyleValueData>> {
    let StyleValueData::ValueList { values, .. } = value else {
        return None;
    };

    let mut contains_size = false;
    let mut contains_layout = false;
    let mut contains_style = false;
    let mut contains_paint = false;

    for containment in values.as_slice() {
        match containment.data() {
            StyleValueData::Keyword { keyword } if *keyword == keyword::SIZE => contains_size = true,
            StyleValueData::Keyword { keyword } if *keyword == keyword::LAYOUT => contains_layout = true,
            StyleValueData::Keyword { keyword } if *keyword == keyword::STYLE => contains_style = true,
            StyleValueData::Keyword { keyword } if *keyword == keyword::PAINT => contains_paint = true,
            _ => return None,
        }
    }

    if !contains_layout || !contains_style || !contains_paint {
        return None;
    }

    let collapsed_keyword = if contains_size {
        keyword::STRICT
    } else {
        keyword::CONTENT
    };
    Some(Arc::new(StyleValueData::Keyword {
        keyword: collapsed_keyword,
    }))
}

/// The per-longhand initial values as shared Rust value identities.
struct InitialValueTable {
    values: Vec<crate::css::style_value::RetainedStyleValueData>,
    dependencies: Vec<ExternalValueDependencies>,
}

// SAFETY: The entries reference immortal, immutable style values.
unsafe impl Send for InitialValueTable {}
unsafe impl Sync for InitialValueTable {}

static INITIAL_VALUE_TABLE: std::sync::OnceLock<InitialValueTable> = std::sync::OnceLock::new();

/// Installs the initial value table, one entry per longhand in property id
/// order.
///
/// # Safety
/// `entries` must point at `length` transferred strong references.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_metadata_set_initial_value_table(entries: *const *const c_void, length: usize) {
    abort_on_panic(|| {
        let values: Vec<_> = unsafe { std::slice::from_raw_parts(entries, length) }
            .iter()
            .map(|entry| unsafe {
                crate::css::style_value::RetainedStyleValueData::from_retained_pointer(
                    (*entry).cast::<crate::css::style_value::StyleValueData>(),
                )
            })
            .collect();
        assert_eq!(
            length,
            crate::css::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES,
            "initial value table has one entry per longhand"
        );
        assert!(
            INITIAL_VALUE_TABLE
                .set(InitialValueTable {
                    dependencies: values
                        .iter()
                        .map(|value| external_value_dependencies(value.data()))
                        .collect(),
                    values,
                })
                .is_ok(),
            "initial value table installed twice"
        );
    });
}

/// Returns the initial value data of a longhand property.
pub(crate) fn initial_value_data(property_id: u16) -> *const crate::css::style_value::StyleValueData {
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let table = INITIAL_VALUE_TABLE.get().expect("initial value table not installed");
    table.values[(property_id - FIRST_LONGHAND_PROPERTY_ID) as usize].pointer()
}

fn initial_value_dependencies(property_id: u16) -> ExternalValueDependencies {
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let table = INITIAL_VALUE_TABLE.get().expect("initial value table not installed");
    table.dependencies[(property_id - FIRST_LONGHAND_PROPERTY_ID) as usize]
}

/// FFI accessor for the parity test on the C++ side.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_metadata_initial_value(property_id: u16) -> *const c_void {
    abort_on_panic(|| initial_value_data(property_id).cast())
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
        property_id == crate::css::property_metadata::property_id::COLOR && keyword == Some(keyword::CURRENTCOLOR);

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
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
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
        // Both tables are in by now, so what they map between them can be summarized once.
        let _ = TABLE_MAPS_SUMMARY.set(summarize_table_rows());
    });
}

/// Maps a physical longhand to its logical alias for the given writing mode
/// and direction, or returns the property itself when it has no logical alias.
pub fn map_physical_to_logical_alias(property_id: u16, writing_mode: u8, direction: u8) -> u16 {
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
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

/// One deferred side-effect operation for a longhand result. Rust stores
/// native results before returning the batch; the entry also carries the selected
/// value and flags needed for animated inheritance, inheritance-dependence
/// bookkeeping, and C++ side effects.
#[repr(C)]
pub struct FfiComputedStoreEntry {
    pub property_id: u16,
    pub inherited_property_id: u16,
    /// The selected specified value; also the stored value when
    /// `computed_kind` is `COMPUTED_KIND_UNCHANGED`.
    pub data: *const c_void,
    /// The C++ declaration-source slot for a cascaded value, or -1.
    pub source_slot: i64,
    /// Whether the selected cascaded facade carried stylesheet context.
    pub has_style_sheet_context: bool,
    pub inheritance_dependent: bool,
    pub inherited: bool,
    /// An owned replacement style value when `computed_kind` is
    /// `COMPUTED_KIND_STYLE_VALUE`. The Rust table adopts this reference before
    /// returning the batch.
    pub computed_data: *const c_void,
    /// How Rust constructs the table value: with COMPUTED_KIND_UNCHANGED the
    /// stored value is `data` itself; the other kinds carry a replacement in
    /// `value` while `data` remains the specified value for the
    /// inheritance-dependence bookkeeping.
    pub computed_kind: u8,
    pub value: f64,
}

pub const COMPUTED_KIND_UNCHANGED: u8 = 0;
/// A pixel length of `value`.
pub const COMPUTED_KIND_PX_LENGTH: u8 = 1;
/// An integer of `value`.
pub const COMPUTED_KIND_INTEGER: u8 = 2;
/// A superellipse with parameter `value`.
pub const COMPUTED_KIND_SUPERELLIPSE: u8 = 3;
/// A number of `value`.
pub const COMPUTED_KIND_NUMBER: u8 = 4;
/// A percentage of `value`.
pub const COMPUTED_KIND_PERCENTAGE: u8 = 5;
/// A font-style value of the font-style keyword code in `value`.
pub const COMPUTED_KIND_FONT_STYLE: u8 = 6;
/// A keyword whose numeric code is carried in `value`.
pub const COMPUTED_KIND_KEYWORD: u8 = 8;
/// A display value encoded as tag | first << 8 | second << 16 | third << 24.
pub const COMPUTED_KIND_DISPLAY: u8 = 9;
/// A complete Rust-owned style value transferred through `computed_data`.
pub const COMPUTED_KIND_STYLE_VALUE: u8 = 10;

#[repr(C)]
pub struct FfiLonghandStoreBatch {
    pub entries: *const FfiComputedStoreEntry,
    pub count: usize,
    pub effective_color_scheme: i16,
    pub storage: *mut c_void,
}

/// Document-level inputs to used color-scheme resolution. Scheme values use
/// the C++ PreferredColorScheme discriminants: auto, dark, and light.
#[repr(C)]
pub struct FfiEffectiveColorSchemeInput {
    pub preferred_color_scheme: u8,
    pub has_document_supported_schemes: bool,
    pub document_supported_scheme_codes: *const u8,
    pub document_supported_scheme_count: usize,
}

impl FfiEffectiveColorSchemeInput {
    /// # Safety
    /// The document-supported scheme storage must be valid for the returned
    /// slice's lifetime.
    unsafe fn document_supported_schemes(&self) -> Option<&[u8]> {
        self.has_document_supported_schemes.then(|| {
            if self.document_supported_scheme_count == 0 {
                &[][..]
            } else {
                unsafe {
                    std::slice::from_raw_parts(
                        self.document_supported_scheme_codes,
                        self.document_supported_scheme_count,
                    )
                }
            }
        })
    }
}

/// Stable element and document facts supplied once for a longhand drive.
#[repr(C)]
pub struct FfiStyleComputationEnvironment {
    pub box_type_input: FfiBoxTypeTransformationInput,
    pub color_scheme_input: FfiEffectiveColorSchemeInput,
    pub is_th_element: bool,
    pub has_new_font_size: bool,
    pub has_animated_inheritance_parent: bool,
    pub has_tree_counting_context: bool,
    pub sibling_count: u64,
    pub sibling_index: u64,
    pub random_base_values: *const FfiRandomBaseValue,
    pub random_base_value_count: usize,
    pub document_base_url: *const u8,
    pub document_base_url_length: usize,
    pub style_sheet_resource_contexts: *const FfiStyleSheetResourceContext,
    pub style_sheet_resource_context_count: usize,
    pub device_pixels_per_css_pixel: f64,
    pub initial_font_size_raw: i32,
    pub default_font_size_raw: i32,
}

#[repr(C)]
pub struct FfiRandomBaseValue {
    pub source: *const c_void,
    pub value: f64,
}

#[repr(C)]
pub struct FfiStyleSheetResourceContext {
    pub base_url: *const u8,
    pub base_url_length: usize,
    pub has_value: bool,
    pub origin_clean: bool,
}

/// Whether a value's absolutization is the identity, so the specified value
/// is already the computed value. Mirrors the value types that fall through
/// to the default arm of StyleValue::absolutized, plus keywords that resolve
/// to themselves: the currentcolor keyword computes to itself, and only
/// color keywords resolve to something else at computed-value time.
pub(crate) fn value_absolutization_is_identity(value: &StyleValueData) -> bool {
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

/// Properties with a dedicated computed-value rule in the native driver;
/// everything else computes as plain absolutization.
fn property_has_dedicated_compute_rule(property_id: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
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
            | prop::CONTAIN
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
            | prop::TRANSFORM_ORIGIN
    )
}

/// Repeats a coordinated list to the background layer count. The outer option
/// rejects non-list or empty values, while the inner option preserves the
/// original allocation when its length already matches.
#[allow(clippy::arc_with_non_send_sync)]
fn repeat_style_value_list_to_n_elements(value: &StyleValueData, count: usize) -> Option<Option<Arc<StyleValueData>>> {
    let StyleValueData::ValueList { values, separator, .. } = value else {
        return None;
    };
    let values = values.as_slice();
    if values.len() == count {
        return Some(None);
    }
    if values.is_empty() {
        return None;
    }
    let repeated_values = (0..count)
        .map(|index| values[index % values.len()].clone_retained())
        .collect();
    Some(Some(Arc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(repeated_values),
        separator: *separator,
        collapsible: true,
    })))
}

/// https://drafts.csswg.org/css-animations-1/#animation-name
/// list, each item either a case-sensitive css identifier or the keyword none
#[allow(clippy::arc_with_non_send_sync)]
fn compute_animation_name(value: &StyleValueData) -> Option<Arc<StyleValueData>> {
    let StyleValueData::ValueList { values, .. } = value else {
        return None;
    };
    let computed_entries = values
        .as_slice()
        .iter()
        .map(|entry| match entry.data() {
            // none | <custom-ident>
            StyleValueData::Keyword { keyword } if *keyword == keyword::NONE => Some(entry.clone_retained()),
            StyleValueData::CustomIdent { .. } => Some(entry.clone_retained()),
            // <string>
            StyleValueData::String {
                string,
                is_valid_animation_name_custom_ident,
            } => Some(if *is_valid_animation_name_custom_ident {
                let pointer = Arc::into_raw(Arc::new(StyleValueData::CustomIdent {
                    custom_ident: string.clone(),
                }));
                unsafe { RetainedStyleValueData::from_retained_pointer(pointer) }
            } else {
                entry.clone_retained()
            }),
            _ => None,
        })
        .collect::<Option<Vec<_>>>()?;
    Some(Arc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(computed_entries),
        separator: 1,
        collapsible: true,
    }))
}

/// https://drafts.csswg.org/css-transforms/#transform-origin-property
/// top and left compute to 0%, center computes to 50%, and bottom and right
/// compute to 100%. A None return means the value is already computed.
#[allow(clippy::arc_with_non_send_sync)]
fn compute_transform_origin(value: &StyleValueData) -> Option<Arc<StyleValueData>> {
    let StyleValueData::ValueList {
        values,
        separator,
        collapsible,
    } = value
    else {
        return None;
    };
    let offset_percentage = |offset: &RetainedStyleValueData| match offset.data() {
        StyleValueData::Keyword { keyword: code } => match *code {
            keyword::LEFT | keyword::TOP => Some(0.0),
            keyword::CENTER => Some(50.0),
            keyword::RIGHT | keyword::BOTTOM => Some(100.0),
            _ => None,
        },
        _ => None,
    };
    let offsets = values.as_slice();
    if !offsets.iter().any(|offset| offset_percentage(offset).is_some()) {
        return None;
    }
    let computed_offsets = offsets
        .iter()
        .map(|offset| match offset_percentage(offset) {
            Some(percentage) => {
                let pointer = Arc::into_raw(Arc::new(StyleValueData::Percentage { value: percentage }));
                unsafe { RetainedStyleValueData::from_retained_pointer(pointer) }
            }
            None => offset.clone_retained(),
        })
        .collect();
    Some(Arc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(computed_offsets),
        separator: *separator,
        collapsible: *collapsible,
    }))
}

/// The parent's computed values, handed to the drive as the parent style's own
/// longhand table span: one shared Rust data identity per longhand in property
/// id order, null where the parent stored none. Everything the span points at
/// is owned by the parent style, which outlives the drive, so nothing is pinned.
///
/// The sparse override list carries the parent's recorded specified values that
/// depend on currentColor, which take precedence over the stored computed value
/// exactly as `computed_style_value_for_inheritance` prefers them on the C++
/// side; the owning maps also outlive the drive.
#[repr(C)]
pub struct FfiParentSnapshot {
    pub table_values: *const *const c_void,
    pub table_value_count: usize,
    pub override_properties: *const u16,
    pub override_values: *const *const c_void,
    pub override_count: usize,
    pub font_metrics_depend_on_viewport_metrics: bool,
}

/// Results of one longhand drive that remain outside the Rust longhand table.
#[repr(C)]
pub struct FfiLonghandDriverResults {
    /// Longhands whose specified-to-computed evaluation ran in this drive.
    pub longhand_evaluations: u32,
    /// The raw winning cascaded font-size value data, or null; borrowed from the
    /// cascaded property store.
    pub raw_cascaded_font_size_data: *const c_void,
    pub depends_on_viewport_metrics: bool,
    pub font_metrics_depend_on_viewport_metrics: bool,
    pub explicitly_inherited_non_inherited_property: bool,
    pub uses_tree_counting_function: bool,
    /// The used color scheme produced by the color-scheme stage, or -1 until
    /// that stage has run.
    pub effective_color_scheme: i16,
    /// Inputs for the post-compute continuation. C++ captures the facade state
    /// that can only be observed between the longhand drive and adjustments,
    /// then resumes Rust with the requested line-height metrics.
    pub post_compute_adjustment: FfiPostComputeAdjustment,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiPostComputeAdjustment {
    pub display_before: FfiDisplay,
    pub float_before: u16,
    pub overflow_x_before: u16,
    pub overflow_y_before: u16,
    pub text_align_before: u16,
    pub position_before: u16,
    pub box_type_transformation: FfiBoxTypeTransformation,
    pub element_style_adjustment: FfiElementStyleAdjustment,
}

pub const POST_ADJUSTED_FLOAT: u8 = 1 << 0;
pub const POST_ADJUSTED_DISPLAY: u8 = 1 << 1;
pub const POST_ADJUSTED_LINE_HEIGHT: u8 = 1 << 2;
pub const POST_ADJUSTED_POSITION: u8 = 1 << 3;
pub const POST_ADJUSTED_TEXT_ALIGN: u8 = 1 << 4;

pub const LONGHAND_DRIVE_PHASE_FONT: u8 = 0;
pub const LONGHAND_DRIVE_PHASE_LINE_HEIGHT: u8 = 1;
pub const LONGHAND_DRIVE_PHASE_COLOR_SCHEME: u8 = 2;
pub const LONGHAND_DRIVE_PHASE_REMAINING: u8 = 3;

fn property_computation_order_for_phase(phase: u8) -> &'static [u16] {
    use crate::css::property_metadata::{property_computation_order, property_id as prop};

    static PHASE_BOUNDARIES: OnceLock<(usize, usize)> = OnceLock::new();
    let order = property_computation_order();
    let &(line_height, color_scheme) = PHASE_BOUNDARIES.get_or_init(|| {
        let line_height = order
            .iter()
            .position(|&property_id| property_id == prop::LINE_HEIGHT)
            .expect("line-height must be in the property computation order");
        let color_scheme = order
            .iter()
            .position(|&property_id| property_id == prop::COLOR_SCHEME)
            .expect("color-scheme must be in the property computation order");
        assert_eq!(color_scheme, line_height + 1);
        (line_height, color_scheme)
    });
    match phase {
        LONGHAND_DRIVE_PHASE_FONT => &order[..line_height],
        LONGHAND_DRIVE_PHASE_LINE_HEIGHT => &order[line_height..color_scheme],
        LONGHAND_DRIVE_PHASE_COLOR_SCHEME => &order[color_scheme..color_scheme + 1],
        LONGHAND_DRIVE_PHASE_REMAINING => &order[color_scheme + 1..],
        _ => unreachable!("unknown longhand drive phase"),
    }
}

/// Which longhands either table maps at all, as one bit each.
///
/// Whether a longhand is in a logical property group is a fact about the stylesheet language, and
/// the driver asked it of both tables for every longhand of every element - ten entries scanned
/// twice, seven hundred times an element, to answer something that never changes.
static TABLE_MAPS_SUMMARY: std::sync::OnceLock<Vec<u8>> = std::sync::OnceLock::new();

const LOGICAL_ALIAS_BIT: u8 = 1;
const PHYSICAL_TO_LOGICAL_BIT: u8 = 2;

fn summarize_table_rows() -> Vec<u8> {
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let row_maps = |table: &std::sync::OnceLock<Vec<u16>>, property_id: u16| -> bool {
        let Some(table) = table.get() else {
            return false;
        };
        let start = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize * WRITING_MODE_COUNT * DIRECTION_COUNT;
        table[start..start + WRITING_MODE_COUNT * DIRECTION_COUNT]
            .iter()
            .any(|&entry| entry != 0)
    };
    let longhand_count = LOGICAL_ALIAS_TABLE
        .get()
        .map_or(0, |table| table.len() / (WRITING_MODE_COUNT * DIRECTION_COUNT));
    (0..longhand_count)
        .map(|index| {
            let property_id = FIRST_LONGHAND_PROPERTY_ID + index as u16;
            let mut bits = 0;
            if row_maps(&LOGICAL_ALIAS_TABLE, property_id) {
                bits |= LOGICAL_ALIAS_BIT;
            }
            if row_maps(&PHYSICAL_TO_LOGICAL_TABLE, property_id) {
                bits |= PHYSICAL_TO_LOGICAL_BIT;
            }
            bits
        })
        .collect()
}

fn table_row_bits(property_id: u16) -> u8 {
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let Some(summary) = TABLE_MAPS_SUMMARY.get() else {
        return 0;
    };
    let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    summary.get(index).copied().unwrap_or(0)
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
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    words[index / 64] |= 1 << (index % 64);
}

fn clear_longhand_bit(words: &mut [u64], property_id: u16) {
    use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
    let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    words[index / 64] &= !(1 << (index % 64));
}

#[allow(clippy::arc_with_non_send_sync)]
fn retained_new(value: StyleValueData) -> RetainedStyleValueData {
    unsafe { RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(value))) }
}

fn store_computed_value(longhand_table: &mut ComputedLonghandTable, entry: &FfiComputedStoreEntry) {
    if entry.inheritance_dependent {
        let specified_value = unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                entry.data.cast(),
            ))
        };
        longhand_table.append_drive_inheritance_dependent_value(entry.property_id, specified_value);
    }
    let retained = match entry.computed_kind {
        COMPUTED_KIND_UNCHANGED => unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                entry.data.cast(),
            ))
        },
        COMPUTED_KIND_PX_LENGTH => retained_new(StyleValueData::Length {
            value: entry.value,
            unit: px_length_unit(),
        }),
        COMPUTED_KIND_INTEGER => retained_new(StyleValueData::Integer {
            value: entry.value as i32,
        }),
        COMPUTED_KIND_SUPERELLIPSE => retained_new(StyleValueData::Superellipse {
            parameter: retained_new(StyleValueData::Number { value: entry.value }),
        }),
        COMPUTED_KIND_NUMBER => retained_new(StyleValueData::Number { value: entry.value }),
        COMPUTED_KIND_PERCENTAGE => retained_new(StyleValueData::Percentage { value: entry.value }),
        COMPUTED_KIND_FONT_STYLE => retained_new(StyleValueData::FontStyle {
            font_style: entry.value as u8,
            angle_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) },
        }),
        COMPUTED_KIND_KEYWORD => retained_new(StyleValueData::Keyword {
            keyword: entry.value as u16,
        }),
        COMPUTED_KIND_DISPLAY => retained_new(StyleValueData::Display {
            raw: entry.value as u32,
        }),
        COMPUTED_KIND_STYLE_VALUE => unsafe {
            RetainedStyleValueData::from_retained_pointer(entry.computed_data.cast())
        },
        _ => unreachable!("unknown computed longhand store kind"),
    };
    let source_slot = if entry.has_style_sheet_context {
        entry.source_slot
    } else {
        -1
    };
    longhand_table.set(entry.property_id, retained, source_slot);
}

fn requires_cpp_store_side_effect(entry: &FfiComputedStoreEntry, has_animated_inheritance_parent: bool) -> bool {
    use crate::css::property_metadata::property_id as prop;
    entry.has_style_sheet_context
        || entry.inherited && has_animated_inheritance_parent
        || matches!(
            entry.property_id,
            prop::COLOR_SCHEME
                | prop::FONT_FAMILY
                | prop::FONT_SIZE
                | prop::FONT_STYLE
                | prop::FONT_VARIATION_SETTINGS
                | prop::FONT_WEIGHT
                | prop::FONT_WIDTH
        )
}

fn publish_longhand_store_batch(
    entries: Vec<FfiComputedStoreEntry>,
    effective_color_scheme: i16,
) -> FfiLonghandStoreBatch {
    let entries = Box::new(entries);
    let pointer = entries.as_ptr();
    let count = entries.len();
    FfiLonghandStoreBatch {
        entries: pointer,
        count,
        effective_color_scheme,
        storage: Box::into_raw(entries).cast(),
    }
}

/// Drives the property computation loop: iterates every longhand in
/// computation order, resolves logical pairing, reads the winning cascaded
/// declarations straight from the store, selects between the cascaded,
/// inherited and initial values, decides inheritance dependence, and stores
/// native results and their metadata directly in the computed longhand table.
/// Other per-element side effects accumulate in `results` for bulk application
/// after the loop.
///
/// Returns one owned batch of ordered C++ side effects after storing the native
/// results. Every entry's borrowed value stays alive until the returned batch is
/// destroyed: cascaded data is retained by the store, initial values are
/// immortal, and parent values are owned by the parent style behind the
/// snapshot, which outlives the drive.
///
/// # Safety
/// `longhand_table` must point at the drive's live mutable computed longhand
/// table, `store` at a valid cascaded property store, `parent_snapshot` at a
/// valid snapshot or null,
/// `environment` at valid element and document facts,
/// `length_resolution_context` at the context for this stage or null for the
/// color-scheme stage, and `results` at a valid results block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_drive_property_computation(
    longhand_table: *mut ComputedLonghandTable,
    store: *const CascadedPropertyStore,
    parent_snapshot: *const FfiParentSnapshot,
    environment: *const FfiStyleComputationEnvironment,
    computed_group_mask: u32,
    computed_property_words: *const u64,
    phase: u8,
    length_resolution_context: *const FfiLengthResolutionContext,
    results: *mut FfiLonghandDriverResults,
) -> FfiLonghandStoreBatch {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::LonghandDriverEntry);
    abort_on_panic(|| {
        use crate::css::property_metadata::{
            NUMBER_OF_LONGHAND_PROPERTIES, REQUIRES_COMPUTATION_ALWAYS, REQUIRES_COMPUTATION_CASCADED,
            REQUIRES_COMPUTATION_NON_INHERITED, property_id as prop, property_requires_computation_level,
        };

        let store = unsafe { &*store };
        let snapshot = if parent_snapshot.is_null() {
            None
        } else {
            Some(unsafe { &*parent_snapshot })
        };
        let has_inheritance_parent = snapshot.is_some();
        let environment = unsafe { &*environment };
        let box_type_input = &environment.box_type_input;
        let color_scheme_input = &environment.color_scheme_input;
        let is_th_element = environment.is_th_element;
        let has_new_font_size = environment.has_new_font_size;
        let device_pixels_per_css_pixel = environment.device_pixels_per_css_pixel;
        let initial_font_size_raw = environment.initial_font_size_raw;
        let default_font_size_raw = environment.default_font_size_raw;
        let tree_counting_context = environment
            .has_tree_counting_context
            .then_some((environment.sibling_count, environment.sibling_index));
        let random_base_values = if environment.random_base_value_count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(environment.random_base_values, environment.random_base_value_count) }
        };
        let document_base_url = if environment.document_base_url_length == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(environment.document_base_url, environment.document_base_url_length) }
        };
        let style_sheet_resource_contexts = if environment.style_sheet_resource_context_count == 0 {
            &[][..]
        } else {
            unsafe {
                std::slice::from_raw_parts(
                    environment.style_sheet_resource_contexts,
                    environment.style_sheet_resource_context_count,
                )
            }
        };
        let length_resolution_context = unsafe { length_resolution_context.as_ref() };
        let results = unsafe { &mut *results };
        const LONGHAND_WORD_COUNT: usize = NUMBER_OF_LONGHAND_PROPERTIES.div_ceil(64);
        let mut important_words = [0; LONGHAND_WORD_COUNT];
        let mut inherited_words = [0; LONGHAND_WORD_COUNT];
        let mut evaluated_words = [0; LONGHAND_WORD_COUNT];
        let mut cached_writing_mode_and_direction: Option<(u8, u8)> = None;

        let computation_order = property_computation_order_for_phase(phase);
        let mut cpp_store_side_effects = Vec::new();
        let mut pending_overflow_x_store: Option<FfiComputedStoreEntry> = None;
        let mut pending_effective_color_scheme: i16 = -1;
        // The element's used color scheme, produced by the preceding color-scheme
        // stage for generic absolutizations.
        let effective_color_scheme = u8::try_from(results.effective_color_scheme).ok();
        /// The parent's computed value for a longhand: a recorded specified value
        /// that depends on currentColor wins over the stored computed value, in
        /// exactly the order computed_style_value_for_inheritance applies.
        fn snapshot_entry_data(snapshot: &FfiParentSnapshot, property_id: u16) -> Option<&StyleValueData> {
            use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
            for index in 0..snapshot.override_count {
                // SAFETY: The override lists are valid for the drive.
                if unsafe { *snapshot.override_properties.add(index) } != property_id {
                    continue;
                }
                // SAFETY: Override values are owned by the parent style, which outlives the drive.
                return unsafe { ((*snapshot.override_values.add(index)) as *const StyleValueData).as_ref() };
            }
            let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
            assert!(index < snapshot.table_value_count);
            // SAFETY: The span borrows the parent style's longhand table, which outlives the drive.
            unsafe { ((*snapshot.table_values.add(index)) as *const StyleValueData).as_ref() }
        }

        // The computed math-depth, remembered for the font-size rule. A partial font restyle
        // may reuse the value already stored in the working table instead.
        let mut computed_math_depth: Option<i32> = None;
        // The background-image list length, for the coordinated background properties.
        let mut background_image_list_length: Option<usize> = None;
        // The computed writing-mode and direction, tracked for logical alias pairing;
        // both properties only take keywords, whose computed value is the specified one.
        let mut computed_writing_mode: Option<u8> = None;
        let mut computed_direction: Option<u8> = None;
        let mut computed_overflow_x: Option<u16> = None;
        let mut computed_overflow_y: Option<u16> = None;
        let mut computed_text_align_before_adjustment: Option<u16> = None;
        let mut computed_text_align: Option<u16> = None;
        let mut computed_display: Option<FfiDisplay> = None;
        let mut computed_float: Option<u16> = None;
        let mut computed_position: Option<u16> = None;
        let computed_property_words = if computed_property_words.is_null() {
            None
        } else {
            Some(unsafe { std::slice::from_raw_parts(computed_property_words, LONGHAND_WORD_COUNT) })
        };

        for &property_id in computation_order {
            use crate::css::computed_values::computed_group_output_mask;
            let required_driver_input = matches!(
                property_id,
                prop::COLOR_SCHEME
                    | prop::DIRECTION
                    | prop::DISPLAY
                    | prop::FLOAT
                    | prop::MATH_DEPTH
                    | prop::OVERFLOW_X
                    | prop::OVERFLOW_Y
                    | prop::POSITION
                    | prop::TEXT_ALIGN
                    | prop::WRITING_MODE
            );
            let output_is_selected = computed_group_output_mask(property_id)
                .is_none_or(|output_mask| output_mask & computed_group_mask != 0);
            let property_index = usize::from(property_id - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
            let property_is_selected = computed_property_words
                .is_some_and(|words| words[property_index / 64] & (1 << (property_index % 64)) != 0);
            if !required_driver_input
                && !property_is_selected
                && (computed_property_words.is_some() || computed_group_mask != u32::MAX && !output_is_selected)
            {
                continue;
            }
            set_longhand_bit(&mut evaluated_words, property_id);
            results.longhand_evaluations = results
                .longhand_evaluations
                .checked_add(1)
                .expect("longhand evaluation count overflow");
            let mut cascaded_property_id = property_id;
            let mut inherited_property_id = property_id;

            // https://drafts.csswg.org/css-logical/#box
            // Within each logical property group, corresponding flow-relative and physical
            // properties are paired using the element's own computed writing mode; the computed
            // value of both properties in the pair is derived from the specified value of the
            // property declared with higher priority in the CSS cascade. A longhand is in a
            // logical property group exactly when either mapping table maps it.
            let table_bits = table_row_bits(property_id);
            let is_logical_alias = table_bits & LOGICAL_ALIAS_BIT != 0;
            if table_bits != 0 {
                let (writing_mode, direction) = *cached_writing_mode_and_direction.get_or_insert_with(|| {
                    // Direction and writing-mode precede every logical property in the
                    // generated computation order and only accept keywords, so their
                    // selected values are their computed mapping inputs.
                    (
                        computed_writing_mode.expect("writing-mode must precede logical properties"),
                        computed_direction.expect("direction must precede logical properties"),
                    )
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

            let mut value = std::ptr::null();
            let mut source_slot = -1;
            let mut has_style_sheet_context = false;
            let mut external_dependencies = None;
            if let Some((
                value_data,
                important,
                cascaded_source_slot,
                cascaded_has_style_sheet_context,
                cascaded_external_dependencies,
            )) = store.winning_declaration(cascaded_property_id)
            {
                value = value_data;
                source_slot = i64::from(cascaded_source_slot);
                has_style_sheet_context = cascaded_has_style_sheet_context;
                external_dependencies = Some(cascaded_external_dependencies);
                if important {
                    set_longhand_bit(&mut important_words, property_id);
                }
                // Keep the raw winning cascaded font-size for the monospace font-size
                // recascade (see recascade_font_size_if_needed on the C++ side).
                if property_id == crate::css::property_metadata::property_id::FONT_SIZE {
                    results.raw_cascaded_font_size_data = value_data;
                }
            } else if property_id == crate::css::property_metadata::property_id::FONT_SIZE && has_new_font_size {
                // NOTE: The recascaded font-size has already been stored before the loop.
                continue;
            }

            let decision = longhand_decision(
                if value.is_null() {
                    None
                } else {
                    Some(unsafe { &*(value as *const StyleValueData) })
                },
                property_id,
            );

            // The computation-need level to compare against depends on which source wins;
            // cascaded is the baseline and is overridden by the inherit and initial paths.
            let mut required_level = REQUIRES_COMPUTATION_CASCADED;

            let inherit_fetch_attempted = decision.should_inherit && has_inheritance_parent;
            if inherit_fetch_attempted {
                source_slot = -1;
                has_style_sheet_context = false;
                external_dependencies = None;
                let snapshot = snapshot.unwrap();
                set_longhand_bit(&mut inherited_words, property_id);
                if decision.explicitly_inherits_non_inherited_property {
                    results.explicitly_inherited_non_inherited_property = true;
                }
                // Both the inherited-by-default read and an explicit `inherit` of a
                // non-inherited property take the parent's stored computed value for
                // `inherited_property_id` straight from the snapshot's table span.
                value = snapshot_entry_data(snapshot, inherited_property_id)
                    .map_or(std::ptr::null(), |data| (data as *const StyleValueData).cast());
                if property_affects_font_metrics(inherited_property_id)
                    && snapshot.font_metrics_depend_on_viewport_metrics
                {
                    results.font_metrics_depend_on_viewport_metrics = true;
                }
                required_level = REQUIRES_COMPUTATION_ALWAYS;
            }

            let use_initial = if inherit_fetch_attempted {
                value.is_null() || value_is_initial_or_unset(value)
            } else {
                decision.use_initial_without_inherit
            };
            if use_initial {
                source_slot = -1;
                has_style_sheet_context = false;
                external_dependencies = Some(initial_value_dependencies(property_id));
                value = initial_value_data(property_id).cast();
                required_level = REQUIRES_COMPUTATION_NON_INHERITED;
            }

            let requires_computation = property_requires_computation_level(property_id) >= required_level;

            // Whether the computed value depends on inherited information, so the specified
            // value must be kept for re-resolution when an ancestor changes.
            let value_data = unsafe { &*(value as *const StyleValueData) };
            let external_dependencies =
                external_dependencies.unwrap_or_else(|| external_value_dependencies(value_data));

            if tree_counting_context.is_some() && external_dependencies.uses_tree_counting_function {
                results.uses_tree_counting_function = true;
            }

            if inherited_property_id == crate::css::property_metadata::property_id::MATH_DEPTH
                && let StyleValueData::Integer { value } = value_data
            {
                // An inherited or initial math-depth is already computed and skips the
                // cascaded-value computation rule, but font-size still consumes it.
                computed_math_depth = Some(*value);
            }
            if inherited_property_id == crate::css::property_metadata::property_id::BACKGROUND_IMAGE
                && let StyleValueData::ValueList { values, .. } = value_data
            {
                background_image_list_length = Some(values.as_slice().len());
            }
            if let StyleValueData::Keyword { keyword } = value_data {
                if property_id == crate::css::property_metadata::property_id::WRITING_MODE {
                    computed_writing_mode = keyword_to_writing_mode(*keyword);
                } else if property_id == crate::css::property_metadata::property_id::DIRECTION {
                    computed_direction = keyword_to_direction(*keyword);
                }
            }
            let inheritance_dependent = external_dependencies.inheritance_dependent
                || value_depends_on_inherited_info_for_property(value_data, property_id);

            let style_sheet_resource_context = if has_style_sheet_context && source_slot >= 0 {
                style_sheet_resource_contexts
                    .get(source_slot as usize)
                    .filter(|context| context.has_value)
                    .map(|context| {
                        let base_url = if context.base_url_length == 0 {
                            &[][..]
                        } else {
                            unsafe { std::slice::from_raw_parts(context.base_url, context.base_url_length) }
                        };
                        crate::css::absolutize::StyleSheetResourceContext {
                            base_url,
                            origin_clean: context.origin_clean,
                        }
                    })
            } else {
                None
            };

            let mut entry = if requires_computation {
                // First classify values handled by simple absolutization. Recursive and
                // dedicated property rules below handle the remaining shapes.
                // The specified value absolutized natively when the core can:
                // Some(None) leaves the value unchanged, Some(Some(px)) resolves it to
                // a pixel length, and None means no simple result is available.
                let mut absolutized: Option<Option<f64>> = if value_absolutization_is_identity(value_data) {
                    Some(None)
                } else if let StyleValueData::Length {
                    value: length_value,
                    unit,
                } = value_data
                {
                    let resolution_context =
                        length_resolution_context.expect("a length-valued property must run with a resolution context");
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

                // Resolve recursively absolutized inputs once against the immutable facts
                // captured before entering the drive. Dedicated property rules then consume
                // the resolved structure just like any other specified value.
                let externally_absolutized = if external_dependencies.uses_tree_counting_function
                    || external_dependencies.container_relative_length_unit_mask != 0
                    || external_dependencies.has_unfixed_random_sharing
                    || matches!(value_data, StyleValueData::Calculated { .. })
                    || inherited_property_id == crate::css::property_metadata::property_id::MATH_DEPTH
                        && matches!(
                            value_data,
                            StyleValueData::Calculated { .. } | StyleValueData::Function { .. }
                        ) {
                    let resolution_context =
                        length_resolution_context.expect("recursive inputs require a length resolution context");
                    let scheme = if phase == LONGHAND_DRIVE_PHASE_REMAINING {
                        effective_color_scheme
                    } else {
                        None
                    };
                    let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                        length: resolution_context,
                        scheme,
                        resolved_viewport_relative_length: std::cell::Cell::new(false),
                        tree_counting: tree_counting_context,
                        random_base_values,
                        document_base_url,
                        style_sheet_resource_context,
                    };
                    let outcome = crate::css::absolutize::absolutize(value_data, &absolutization_context);
                    if absolutization_context.resolved_viewport_relative_length.get() {
                        results.depends_on_viewport_metrics = true;
                        if property_affects_font_metrics(inherited_property_id) {
                            results.font_metrics_depend_on_viewport_metrics = true;
                        }
                    }
                    match outcome {
                        Some(crate::css::absolutize::Absolutized::Changed(value)) => Some(value.into_arc()),
                        Some(crate::css::absolutize::Absolutized::Unchanged) | None => None,
                    }
                } else {
                    None
                };
                let value_data = externally_absolutized.as_deref().unwrap_or(value_data);
                if absolutized.is_none()
                    && externally_absolutized.is_some()
                    && let StyleValueData::Length {
                        value: length_value,
                        unit,
                    } = value_data
                {
                    let resolution_context =
                        length_resolution_context.expect("a length-valued property must run with a resolution context");
                    let result = absolutize_length(*length_value, *unit as usize, resolution_context);
                    if result.handled {
                        absolutized = Some(result.changed.then_some(result.px));
                    }
                }

                // The computed value: for properties without a dedicated rule the
                // absolutized value is the computed value; the dedicated rules that
                // have moved into the core run over the absolutized value here.
                enum NativeValue {
                    Unsupported,
                    Unchanged,
                    Px(f64),
                    Integer(i32),
                    Superellipse(f64),
                    Number(f64),
                    Percentage(f64),
                    FontStyle(u8),
                    StyleValue(Arc<StyleValueData>),
                }
                use crate::css::property_metadata::property_id as prop;
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
                            None,
                        );
                        if result.handled {
                            NativeValue::Px(result.value)
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (
                        None,
                        prop::BORDER_BOTTOM_WIDTH
                        | prop::BORDER_LEFT_WIDTH
                        | prop::BORDER_RIGHT_WIDTH
                        | prop::BORDER_TOP_WIDTH
                        | prop::OUTLINE_WIDTH,
                    ) if matches!(value_data, StyleValueData::Calculated { .. }) => {
                        let resolution_context = length_resolution_context
                            .expect("calculated border widths require a length resolution context");
                        let mut resolved_viewport_relative_length = false;
                        let mut calc_resolution_context = *resolution_context;
                        calc_resolution_context.resolved_viewport_relative_length =
                            &raw mut resolved_viewport_relative_length;
                        let result = compute_border_or_outline_width(
                            value_data,
                            device_pixels_per_css_pixel,
                            Some(&calc_resolution_context),
                        );
                        if resolved_viewport_relative_length {
                            results.depends_on_viewport_metrics = true;
                        }
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
                        // Corner shape keywords reach here because their absolutization is the identity.
                        let result = compute_corner_shape_parameter(value_data);
                        if result.handled && !result.unchanged {
                            NativeValue::Superellipse(result.value)
                        } else if result.handled {
                            NativeValue::Unchanged
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (
                        None,
                        prop::CORNER_BOTTOM_LEFT_SHAPE
                        | prop::CORNER_BOTTOM_RIGHT_SHAPE
                        | prop::CORNER_TOP_LEFT_SHAPE
                        | prop::CORNER_TOP_RIGHT_SHAPE,
                    ) => {
                        let resolution_context =
                            length_resolution_context.expect("corner shapes require a length resolution context");
                        let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                            length: resolution_context,
                            scheme: effective_color_scheme,
                            resolved_viewport_relative_length: std::cell::Cell::new(false),
                            tree_counting: tree_counting_context,
                            random_base_values,
                            document_base_url,
                            style_sheet_resource_context,
                        };
                        let absolutized = crate::css::absolutize::absolutize(value_data, &absolutization_context);
                        if absolutization_context.resolved_viewport_relative_length.get() {
                            results.depends_on_viewport_metrics = true;
                        }
                        match absolutized {
                            Some(crate::css::absolutize::Absolutized::Unchanged) => {
                                let result = compute_corner_shape_parameter(value_data);
                                if result.handled && !result.unchanged {
                                    NativeValue::Superellipse(result.value)
                                } else if result.handled {
                                    NativeValue::Unchanged
                                } else {
                                    NativeValue::Unsupported
                                }
                            }
                            Some(crate::css::absolutize::Absolutized::Changed(value)) => {
                                let result = compute_corner_shape_parameter(value.data());
                                if result.handled && !result.unchanged {
                                    NativeValue::Superellipse(result.value)
                                } else if result.handled {
                                    NativeValue::StyleValue(value.into_arc())
                                } else {
                                    NativeValue::Unsupported
                                }
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    (native_absolutized, prop::MATH_DEPTH)
                        if native_absolutized.is_some()
                            || externally_absolutized.is_some()
                            || matches!(
                                value_data,
                                StyleValueData::Calculated { .. } | StyleValueData::Function { .. }
                            ) =>
                    {
                        // The inherited math-depth and math-style come from the parent
                        // snapshot; without an inheritance parent the initial values apply
                        // (math-depth 0, math-style normal).
                        let (inherited_math_depth, inherited_math_style_is_compact) = match snapshot {
                            Some(snapshot) => {
                                let math_depth = match snapshot_entry_data(snapshot, prop::MATH_DEPTH) {
                                    Some(StyleValueData::Integer { value }) => *value,
                                    _ => 0,
                                };
                                let compact = matches!(
                                    snapshot_entry_data(snapshot, prop::MATH_STYLE),
                                    Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::COMPACT
                                );
                                (math_depth, compact)
                            }
                            None => (0, false),
                        };
                        let result =
                            compute_math_depth(value_data, inherited_math_depth, inherited_math_style_is_compact);
                        if result.handled {
                            computed_math_depth = Some(result.value as i32);
                            NativeValue::Integer(result.value as i32)
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (native_absolutized, prop::FONT_SIZE)
                        if native_absolutized.is_some()
                            || externally_absolutized.is_some()
                            || matches!(value_data, StyleValueData::Calculated { .. }) =>
                    {
                        let absolutized = native_absolutized.flatten();
                        let computed_math_depth = computed_math_depth.or_else(|| {
                            unsafe { &*longhand_table }
                                .get(prop::MATH_DEPTH)
                                .and_then(|value| match value.data() {
                                    StyleValueData::Integer { value } => Some(*value),
                                    _ => None,
                                })
                        });
                        if let Some(computed_math_depth) = computed_math_depth {
                            // A font-size relative to the inherited size also inherits the
                            // parent's viewport dependence of its font metrics.
                            if value_depends_on_inherited_info_for_property(value_data, prop::FONT_SIZE)
                                && snapshot.is_some_and(|snapshot| snapshot.font_metrics_depend_on_viewport_metrics)
                            {
                                results.depends_on_viewport_metrics = true;
                                results.font_metrics_depend_on_viewport_metrics = true;
                            }
                            let inherited = match snapshot {
                                Some(snapshot) => match snapshot_entry_data(snapshot, prop::FONT_SIZE) {
                                    Some(StyleValueData::Length { value, unit }) if *unit == px_length_unit() => {
                                        let math_depth = match snapshot_entry_data(snapshot, prop::MATH_DEPTH) {
                                            Some(StyleValueData::Integer { value }) => *value,
                                            _ => 0,
                                        };
                                        Some((CssPixels::nearest_value_for(*value), math_depth))
                                    }
                                    _ => None,
                                },
                                None => Some((CssPixels::from_raw(initial_font_size_raw), 0)),
                            };
                            match inherited {
                                Some((inherited_font_size, inherited_math_depth)) => {
                                    let synthesized = synthesized_px_length(absolutized);
                                    let result = compute_font_size(
                                        synthesized.as_ref().unwrap_or(value_data),
                                        computed_math_depth,
                                        inherited_font_size,
                                        inherited_math_depth,
                                        CssPixels::from_raw(default_font_size_raw),
                                    );
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
                                None => NativeValue::Unsupported,
                            }
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (native_absolutized, prop::FONT_WEIGHT)
                        if native_absolutized.is_some()
                            || externally_absolutized.is_some()
                            || matches!(value_data, StyleValueData::Calculated { .. }) =>
                    {
                        let inherited_font_weight = match snapshot {
                            Some(snapshot) => match snapshot_entry_data(snapshot, prop::FONT_WEIGHT) {
                                Some(StyleValueData::Number { value }) => Some(*value),
                                _ => None,
                            },
                            None => Some(400.0),
                        };
                        match inherited_font_weight {
                            Some(inherited_font_weight) => {
                                let result = compute_font_weight(value_data, inherited_font_weight);
                                if result.handled {
                                    if result.unchanged {
                                        NativeValue::Unchanged
                                    } else {
                                        NativeValue::Number(result.value)
                                    }
                                } else {
                                    NativeValue::Unsupported
                                }
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    (Some(_), prop::FONT_STYLE) => match value_data {
                        StyleValueData::Keyword { keyword } => match keyword_to_font_style_keyword(*keyword) {
                            Some(font_style_keyword) => NativeValue::FontStyle(font_style_keyword),
                            None => NativeValue::Unchanged,
                        },
                        _ => NativeValue::Unchanged,
                    },
                    (None, prop::FONT_STYLE) if matches!(value_data, StyleValueData::FontStyle { .. }) => {
                        let resolution_context =
                            length_resolution_context.expect("font-style requires a length resolution context");
                        let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                            length: resolution_context,
                            scheme: None,
                            resolved_viewport_relative_length: std::cell::Cell::new(false),
                            tree_counting: tree_counting_context,
                            random_base_values,
                            document_base_url,
                            style_sheet_resource_context,
                        };
                        match crate::css::absolutize::absolutize(value_data, &absolutization_context) {
                            Some(crate::css::absolutize::Absolutized::Unchanged) => NativeValue::Unchanged,
                            Some(crate::css::absolutize::Absolutized::Changed(value)) => {
                                NativeValue::StyleValue(value.into_arc())
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    (native_absolutized, prop::FONT_WIDTH)
                        if native_absolutized.is_some()
                            || externally_absolutized.is_some()
                            || matches!(value_data, StyleValueData::Calculated { .. }) =>
                    {
                        let result = compute_font_width(value_data);
                        if result.handled {
                            if result.unchanged {
                                NativeValue::Unchanged
                            } else {
                                NativeValue::Percentage(result.value)
                            }
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (Some(_), prop::FONT_FEATURE_SETTINGS | prop::FONT_VARIATION_SETTINGS)
                        if matches!(value_data, StyleValueData::Keyword { .. }) =>
                    {
                        NativeValue::Unchanged
                    }
                    (None, prop::FONT_FEATURE_SETTINGS | prop::FONT_VARIATION_SETTINGS) => {
                        let resolution_context = length_resolution_context
                            .expect("font feature settings require a length resolution context");
                        let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                            length: resolution_context,
                            scheme: None,
                            resolved_viewport_relative_length: std::cell::Cell::new(false),
                            tree_counting: tree_counting_context,
                            random_base_values,
                            document_base_url,
                            style_sheet_resource_context,
                        };
                        let absolutized = crate::css::absolutize::absolutize(value_data, &absolutization_context);
                        if absolutization_context.resolved_viewport_relative_length.get() {
                            results.depends_on_viewport_metrics = true;
                        }
                        match absolutized {
                            Some(crate::css::absolutize::Absolutized::Unchanged) => {
                                NativeValue::StyleValue(compute_font_feature_tag_value_list(value_data))
                            }
                            Some(crate::css::absolutize::Absolutized::Changed(value)) => {
                                NativeValue::StyleValue(compute_font_feature_tag_value_list(value.data()))
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    (_, prop::LINE_HEIGHT) if matches!(value_data, StyleValueData::Calculated { .. }) => {
                        let resolution_context = length_resolution_context
                            .expect("calculated line-height requires a length resolution context");
                        let result = compute_line_height(
                            value_data,
                            CssPixels::nearest_value_for(resolution_context.font_metrics.font_size),
                        );
                        if result.handled && result.is_number {
                            NativeValue::Number(result.value)
                        } else if result.handled && !result.unchanged {
                            NativeValue::Px(result.value)
                        } else if result.handled {
                            NativeValue::Unchanged
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (_, prop::LINE_HEIGHT) => {
                        let absolutized = absolutized.flatten();
                        let result = if matches!(value_data, StyleValueData::Percentage { .. }) {
                            let resolution_context =
                                length_resolution_context.expect("line-height must run with a resolution context");
                            compute_line_height(
                                value_data,
                                CssPixels::nearest_value_for(resolution_context.font_metrics.font_size),
                            )
                        } else {
                            let synthesized = synthesized_px_length(absolutized);
                            compute_line_height(synthesized.as_ref().unwrap_or(value_data), CssPixels::from_raw(0))
                        };
                        if result.handled {
                            if result.unchanged {
                                match absolutized {
                                    Some(px) => NativeValue::Px(px),
                                    None => NativeValue::Unchanged,
                                }
                            } else if result.is_number {
                                NativeValue::Number(result.value)
                            } else {
                                NativeValue::Px(result.value)
                            }
                        } else {
                            NativeValue::Unsupported
                        }
                    }
                    (None, prop::FONT_FAMILY) if matches!(value_data, StyleValueData::ValueList { .. }) => {
                        // A font-family list only ever holds keywords, strings and custom
                        // identifiers, whose absolutization is the identity.
                        NativeValue::Unchanged
                    }
                    (
                        None,
                        prop::BACKGROUND_ATTACHMENT
                        | prop::BACKGROUND_CLIP
                        | prop::BACKGROUND_ORIGIN
                        | prop::BACKGROUND_POSITION_X
                        | prop::BACKGROUND_POSITION_Y
                        | prop::BACKGROUND_REPEAT
                        | prop::BACKGROUND_SIZE,
                    ) => {
                        // NB: The background properties are coordinated at compute time rather
                        //     than use time, unlike other coordinating list property groups.
                        let layer_count = background_image_list_length
                            .or_else(|| {
                                unsafe { &*longhand_table }
                                    .get(prop::BACKGROUND_IMAGE)
                                    .and_then(|value| match value.data() {
                                        StyleValueData::ValueList { values, .. } => Some(values.as_slice().len()),
                                        _ => None,
                                    })
                            })
                            .expect("background-image must be a computed value list");
                        let resolution_context =
                            length_resolution_context.expect("background lists require a length resolution context");
                        let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                            length: resolution_context,
                            scheme: effective_color_scheme,
                            resolved_viewport_relative_length: std::cell::Cell::new(false),
                            tree_counting: tree_counting_context,
                            random_base_values,
                            document_base_url,
                            style_sheet_resource_context,
                        };
                        let absolutized = crate::css::absolutize::absolutize(value_data, &absolutization_context);
                        if absolutization_context.resolved_viewport_relative_length.get() {
                            results.depends_on_viewport_metrics = true;
                        }
                        match absolutized {
                            Some(crate::css::absolutize::Absolutized::Unchanged) => {
                                match repeat_style_value_list_to_n_elements(value_data, layer_count) {
                                    Some(None) => NativeValue::Unchanged,
                                    Some(Some(value)) => NativeValue::StyleValue(value),
                                    None => NativeValue::Unsupported,
                                }
                            }
                            Some(crate::css::absolutize::Absolutized::Changed(value)) => {
                                match repeat_style_value_list_to_n_elements(value.data(), layer_count) {
                                    Some(None) => NativeValue::StyleValue(value.into_arc()),
                                    Some(Some(value)) => NativeValue::StyleValue(value),
                                    None => NativeValue::Unsupported,
                                }
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    (Some(_), prop::ANIMATION_NAME)
                        if matches!(
                            value_data,
                            StyleValueData::Keyword { .. } | StyleValueData::CustomIdent { .. }
                        ) =>
                    {
                        NativeValue::Unchanged
                    }
                    (None, prop::ANIMATION_NAME) => match compute_animation_name(value_data) {
                        Some(value) => NativeValue::StyleValue(value),
                        None => NativeValue::Unsupported,
                    },
                    (_, prop::LETTER_SPACING | prop::WORD_SPACING)
                        if matches!(value_data, StyleValueData::Calculated { .. }) =>
                    {
                        NativeValue::Unchanged
                    }
                    (_, prop::LETTER_SPACING | prop::WORD_SPACING) => {
                        let absolutized = absolutized.flatten();
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
                    (_, prop::POSITION_AREA) => match compute_position_area(value_data) {
                        Some(value) => NativeValue::StyleValue(value),
                        None => NativeValue::Unchanged,
                    },
                    (_, prop::STROKE_DASHOFFSET | prop::STROKE_WIDTH)
                        if matches!(value_data, StyleValueData::Number { .. }) =>
                    {
                        let StyleValueData::Number { value } = value_data else {
                            unreachable!("the guard accepted only numbers");
                        };
                        NativeValue::Px(*value)
                    }
                    (None, prop::STROKE_DASHARRAY) if matches!(value_data, StyleValueData::ValueList { .. }) => {
                        let resolution_context =
                            length_resolution_context.expect("a dash list must run with a resolution context");
                        let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                            length: resolution_context,
                            scheme: effective_color_scheme,
                            resolved_viewport_relative_length: std::cell::Cell::new(false),
                            tree_counting: tree_counting_context,
                            random_base_values,
                            document_base_url,
                            style_sheet_resource_context,
                        };
                        let outcome = crate::css::absolutize::absolutize(value_data, &absolutization_context);
                        if absolutization_context.resolved_viewport_relative_length.get() {
                            results.depends_on_viewport_metrics = true;
                        }
                        match outcome {
                            Some(crate::css::absolutize::Absolutized::Unchanged) => {
                                match stroke_dasharray_numbers_as_lengths(value_data) {
                                    Some(value) => NativeValue::StyleValue(value),
                                    None => NativeValue::Unchanged,
                                }
                            }
                            Some(crate::css::absolutize::Absolutized::Changed(value)) => {
                                match stroke_dasharray_numbers_as_lengths(value.data()) {
                                    Some(computed) => NativeValue::StyleValue(computed),
                                    None => NativeValue::StyleValue(value.into_arc()),
                                }
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    (None, prop::TRANSFORM_ORIGIN) => {
                        let resolution_context =
                            length_resolution_context.expect("transform-origin requires a length resolution context");
                        let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                            length: resolution_context,
                            scheme: effective_color_scheme,
                            resolved_viewport_relative_length: std::cell::Cell::new(false),
                            tree_counting: tree_counting_context,
                            random_base_values,
                            document_base_url,
                            style_sheet_resource_context,
                        };
                        let absolutized = crate::css::absolutize::absolutize(value_data, &absolutization_context);
                        if absolutization_context.resolved_viewport_relative_length.get() {
                            results.depends_on_viewport_metrics = true;
                        }
                        match absolutized {
                            Some(crate::css::absolutize::Absolutized::Unchanged) => {
                                match compute_transform_origin(value_data) {
                                    Some(value) => NativeValue::StyleValue(value),
                                    None => NativeValue::Unchanged,
                                }
                            }
                            Some(crate::css::absolutize::Absolutized::Changed(value)) => {
                                let computed = compute_transform_origin(value.data());
                                NativeValue::StyleValue(computed.unwrap_or_else(|| value.into_arc()))
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    // https://drafts.csswg.org/css-tables-3/#border-spacing-property
                    // two absolute lengths
                    // A single specified length computes to the pair with both members equal, so
                    // every computed border-spacing has the same two-value list shape; a specified
                    // pair takes the generic arms below.
                    (_, prop::BORDER_SPACING) if !matches!(value_data, StyleValueData::ValueList { .. }) => {
                        let single = match absolutized {
                            Some(Some(px)) => StyleValueData::Length {
                                value: px,
                                unit: px_length_unit(),
                            },
                            _ => value_data.clone(),
                        };
                        NativeValue::StyleValue(border_spacing_pair(single))
                    }
                    (_, prop::CONTAIN) => match collapse_containment_list(value_data) {
                        Some(value) => NativeValue::StyleValue(value),
                        None => NativeValue::Unchanged,
                    },
                    (Some(absolutized), _) if !property_has_dedicated_compute_rule(inherited_property_id) => {
                        match absolutized {
                            Some(px) => NativeValue::Px(px),
                            None => NativeValue::Unchanged,
                        }
                    }
                    (None, _) if !property_has_dedicated_compute_rule(inherited_property_id) => {
                        // The recursive native absolutization: structural values and their
                        // length leaves resolve here; anything it declines computes in C++.
                        let resolution_context = length_resolution_context
                            .expect("recursive absolutization must run with a resolution context");
                        // Only the generic computation context carries a color scheme in C++;
                        // the font and line-height contexts absolutize without one.
                        let scheme = if phase == LONGHAND_DRIVE_PHASE_REMAINING {
                            effective_color_scheme
                        } else {
                            None
                        };
                        let absolutization_context = crate::css::absolutize::AbsolutizationContext {
                            length: resolution_context,
                            scheme,
                            resolved_viewport_relative_length: std::cell::Cell::new(false),
                            tree_counting: tree_counting_context,
                            random_base_values,
                            document_base_url,
                            style_sheet_resource_context,
                        };
                        let outcome = crate::css::absolutize::absolutize(value_data, &absolutization_context);
                        if absolutization_context.resolved_viewport_relative_length.get() {
                            results.depends_on_viewport_metrics = true;
                            if property_affects_font_metrics(inherited_property_id) {
                                results.font_metrics_depend_on_viewport_metrics = true;
                            }
                        }
                        match outcome {
                            Some(crate::css::absolutize::Absolutized::Unchanged) => NativeValue::Unchanged,
                            Some(crate::css::absolutize::Absolutized::Changed(new_value)) => {
                                NativeValue::StyleValue(new_value.into_arc())
                            }
                            None => NativeValue::Unsupported,
                        }
                    }
                    _ => NativeValue::Unsupported,
                };

                // An unchanged dedicated-rule result refers to the value presented to that
                // rule. Preserve an externally resolved replacement instead of the original declaration.
                let native = match (native, externally_absolutized) {
                    (NativeValue::Unchanged, Some(value)) => NativeValue::StyleValue(value),
                    (native, _) => native,
                };
                let (computed_kind, computed_value, computed_data) = match native {
                    NativeValue::Px(px) => (COMPUTED_KIND_PX_LENGTH, px, std::ptr::null()),
                    NativeValue::Integer(integer) => (COMPUTED_KIND_INTEGER, integer as f64, std::ptr::null()),
                    NativeValue::Superellipse(parameter) => (COMPUTED_KIND_SUPERELLIPSE, parameter, std::ptr::null()),
                    NativeValue::Number(number) => (COMPUTED_KIND_NUMBER, number, std::ptr::null()),
                    NativeValue::Percentage(percentage) => (COMPUTED_KIND_PERCENTAGE, percentage, std::ptr::null()),
                    NativeValue::FontStyle(font_style_keyword) => {
                        (COMPUTED_KIND_FONT_STYLE, font_style_keyword as f64, std::ptr::null())
                    }
                    NativeValue::StyleValue(value) => (COMPUTED_KIND_STYLE_VALUE, 0.0, Arc::into_raw(value).cast()),
                    NativeValue::Unchanged => (COMPUTED_KIND_UNCHANGED, 0.0, std::ptr::null()),
                    NativeValue::Unsupported => {
                        unreachable!("unsupported native computation for longhand property {inherited_property_id}")
                    }
                };
                FfiComputedStoreEntry {
                    property_id,
                    inherited_property_id,
                    data: value,
                    source_slot,
                    has_style_sheet_context,
                    inheritance_dependent,
                    inherited: inherit_fetch_attempted,
                    computed_data,
                    computed_kind,
                    value: computed_value,
                }
            } else {
                FfiComputedStoreEntry {
                    property_id,
                    inherited_property_id,
                    data: value,
                    source_slot,
                    has_style_sheet_context,
                    inheritance_dependent,
                    inherited: inherit_fetch_attempted,
                    computed_data: std::ptr::null(),
                    computed_kind: COMPUTED_KIND_UNCHANGED,
                    value: 0.0,
                }
            };

            if property_id == prop::OVERFLOW_X {
                if let StyleValueData::Keyword { keyword } = value_data {
                    computed_overflow_x = Some(*keyword);
                }
            } else if property_id == prop::OVERFLOW_Y
                && let (Some(overflow_x), StyleValueData::Keyword { keyword: overflow_y }) =
                    (computed_overflow_x, value_data)
            {
                computed_overflow_y = Some(*overflow_y);
                let effective_overflow = resolve_effective_overflow_keywords(overflow_x, *overflow_y);
                let overflow_x_entry = pending_overflow_x_store
                    .as_mut()
                    .expect("overflow-x must precede overflow-y in computation order");
                if effective_overflow.changed_x {
                    debug_assert_eq!(overflow_x_entry.computed_kind, COMPUTED_KIND_UNCHANGED);
                    overflow_x_entry.computed_kind = COMPUTED_KIND_KEYWORD;
                    overflow_x_entry.value = effective_overflow.x_keyword as f64;
                    clear_longhand_bit(&mut important_words, prop::OVERFLOW_X);
                    clear_longhand_bit(&mut inherited_words, prop::OVERFLOW_X);
                }
                if effective_overflow.changed_y {
                    debug_assert_eq!(entry.computed_kind, COMPUTED_KIND_UNCHANGED);
                    entry.computed_kind = COMPUTED_KIND_KEYWORD;
                    entry.value = effective_overflow.y_keyword as f64;
                    clear_longhand_bit(&mut important_words, prop::OVERFLOW_Y);
                    clear_longhand_bit(&mut inherited_words, prop::OVERFLOW_Y);
                }
            } else if property_id == prop::TEXT_ALIGN
                && let StyleValueData::Keyword { keyword: text_align } = value_data
            {
                computed_text_align_before_adjustment = Some(*text_align);
                let (has_parent_with_computed_values, parent_text_align, parent_direction_is_ltr) =
                    if let Some(snapshot) = snapshot {
                        let parent_text_align = match snapshot_entry_data(snapshot, prop::TEXT_ALIGN) {
                            Some(StyleValueData::Keyword { keyword }) => *keyword,
                            _ => unreachable!("parent text-align must be a keyword"),
                        };
                        let parent_direction_is_ltr = match snapshot_entry_data(snapshot, prop::DIRECTION) {
                            Some(StyleValueData::Keyword {
                                keyword: parent_direction,
                            }) => *parent_direction == keyword::LTR,
                            _ => unreachable!("parent direction must be a keyword"),
                        };
                        (true, parent_text_align, parent_direction_is_ltr)
                    } else {
                        (false, 0, true)
                    };
                let adjustment = compute_text_align_adjustment(
                    *text_align,
                    is_th_element,
                    has_parent_with_computed_values,
                    parent_text_align,
                    parent_direction_is_ltr,
                );
                if adjustment.changed {
                    debug_assert_eq!(entry.computed_kind, COMPUTED_KIND_UNCHANGED);
                    entry.computed_kind = COMPUTED_KIND_KEYWORD;
                    entry.value = adjustment.keyword as f64;
                    clear_longhand_bit(&mut important_words, property_id);
                    clear_longhand_bit(&mut inherited_words, property_id);
                    if adjustment.inherited {
                        set_longhand_bit(&mut inherited_words, property_id);
                    }
                }
                computed_text_align = Some(if adjustment.changed {
                    adjustment.keyword
                } else {
                    *text_align
                });
            } else if property_id == prop::COLOR_SCHEME
                && let StyleValueData::ColorScheme { scheme_codes, .. } = value_data
            {
                let color_scheme = resolve_effective_color_scheme(
                    scheme_codes.as_slice(),
                    color_scheme_input.preferred_color_scheme,
                    unsafe { color_scheme_input.document_supported_schemes() },
                );
                pending_effective_color_scheme = i16::from(color_scheme);
                results.effective_color_scheme = i16::from(color_scheme);
            }

            match (property_id, value_data) {
                (prop::DISPLAY, StyleValueData::Display { raw }) => {
                    computed_display = Some(FfiDisplay::from_raw(*raw));
                }
                (prop::FLOAT, StyleValueData::Keyword { keyword }) => {
                    computed_float = Some(*keyword);
                }
                (prop::POSITION, StyleValueData::Keyword { keyword }) => {
                    computed_position = Some(*keyword);
                }
                _ => {}
            }

            let longhand_table = unsafe { &mut *longhand_table };
            let mut store_and_collect_side_effect = |entry: FfiComputedStoreEntry| {
                store_computed_value(longhand_table, &entry);
                if requires_cpp_store_side_effect(&entry, environment.has_animated_inheritance_parent) {
                    cpp_store_side_effects.push(entry);
                }
            };
            if property_id == prop::OVERFLOW_X {
                pending_overflow_x_store = Some(entry);
            } else {
                if property_id == prop::OVERFLOW_Y {
                    store_and_collect_side_effect(
                        pending_overflow_x_store
                            .take()
                            .expect("overflow-x must precede overflow-y in computation order"),
                    );
                }
                store_and_collect_side_effect(entry);
            }
        }

        assert!(
            pending_overflow_x_store.is_none(),
            "overflow-y must follow overflow-x in computation order"
        );
        let longhand_table = unsafe { &mut *longhand_table };
        longhand_table.finish_drive_inheritance_dependent_values();
        longhand_table.merge_driver_flags(&important_words, &inherited_words, &evaluated_words);
        if phase != LONGHAND_DRIVE_PHASE_REMAINING {
            return publish_longhand_store_batch(cpp_store_side_effects, pending_effective_color_scheme);
        }
        let display_before = computed_display.expect("display must be computed by the longhand driver");
        let mut box_type_input = *box_type_input;
        box_type_input.display = display_before;
        let float_before = computed_float.expect("float must be computed by the longhand driver");
        box_type_input.float_value = float_before;
        box_type_input.position = computed_position.expect("position must be computed by the longhand driver");
        let text_align = computed_text_align.expect("text-align must be computed by the longhand driver");
        let adjustments = compute_element_style_adjustments(&box_type_input, text_align);
        let transformation = adjustments.box_type;
        let element_adjustment = adjustments.element_style;
        results.post_compute_adjustment = FfiPostComputeAdjustment {
            display_before,
            float_before,
            overflow_x_before: computed_overflow_x.expect("overflow-x must be computed by the longhand driver"),
            overflow_y_before: computed_overflow_y.expect("overflow-y must be computed by the longhand driver"),
            text_align_before: computed_text_align_before_adjustment
                .expect("text-align must be computed by the longhand driver"),
            position_before: box_type_input.position,
            box_type_transformation: transformation,
            element_style_adjustment: element_adjustment,
        };
        publish_longhand_store_batch(cpp_store_side_effects, pending_effective_color_scheme)
    })
}

/// # Safety
/// `storage` must be a live pointer returned by `rust_drive_property_computation`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_longhand_store_batch_destroy(storage: *mut c_void) {
    abort_on_panic(|| drop(unsafe { Box::from_raw(storage.cast::<Vec<FfiComputedStoreEntry>>()) }));
}

/// Applies the post-compute box and element adjustments after C++ has captured
/// the facade state that precedes them.
///
/// # Safety
/// `longhand_table`, `adjustment`, and `input_line_height_metrics` must point at
/// the live objects for the longhand drive that immediately preceded this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_apply_post_compute_adjustments(
    longhand_table: *mut ComputedLonghandTable,
    adjustment: *const FfiPostComputeAdjustment,
    input_line_height_metrics: *const FfiInputLineHeightMetrics,
) -> u8 {
    abort_on_panic(|| {
        use crate::css::property_metadata::property_id as prop;

        let adjustment = unsafe { &*adjustment };
        let transformation = adjustment.box_type_transformation;
        let element_adjustment = adjustment.element_style_adjustment;
        let input_line_height_metrics = unsafe { &*input_line_height_metrics };
        let clamp_input_line_height = should_clamp_input_line_height(&element_adjustment, input_line_height_metrics);
        let display_after_box_type_transformation = if transformation.changed_display {
            transformation.display
        } else {
            adjustment.display_before
        };
        let adjusted_display = if element_adjustment.changed_display {
            element_adjustment.display
        } else {
            display_after_box_type_transformation
        };
        let adjusted_entry = |property_id, computed_kind, value| FfiComputedStoreEntry {
            property_id,
            inherited_property_id: property_id,
            data: initial_value_data(property_id).cast(),
            source_slot: -1,
            has_style_sheet_context: false,
            inheritance_dependent: false,
            inherited: false,
            computed_data: std::ptr::null(),
            computed_kind,
            value,
        };
        let mut post_adjusted_longhands = 0;
        let mut adjustments = Vec::new();
        if transformation.set_float_none {
            post_adjusted_longhands |= POST_ADJUSTED_FLOAT;
            adjustments.push(adjusted_entry(prop::FLOAT, COMPUTED_KIND_KEYWORD, keyword::NONE as f64));
        }
        if adjusted_display != adjustment.display_before {
            post_adjusted_longhands |= POST_ADJUSTED_DISPLAY;
            adjustments.push(adjusted_entry(
                prop::DISPLAY,
                COMPUTED_KIND_DISPLAY,
                adjusted_display.encoded() as f64,
            ));
        }
        let line_height_changed = element_adjustment.set_line_height_normal || clamp_input_line_height;
        if line_height_changed {
            post_adjusted_longhands |= POST_ADJUSTED_LINE_HEIGHT;
            adjustments.push(adjusted_entry(
                prop::LINE_HEIGHT,
                COMPUTED_KIND_KEYWORD,
                keyword::NORMAL as f64,
            ));
        }
        if element_adjustment.set_position_static {
            post_adjusted_longhands |= POST_ADJUSTED_POSITION;
            adjustments.push(adjusted_entry(
                prop::POSITION,
                COMPUTED_KIND_KEYWORD,
                keyword::STATIC as f64,
            ));
        }
        if element_adjustment.changed_text_align {
            post_adjusted_longhands |= POST_ADJUSTED_TEXT_ALIGN;
            adjustments.push(adjusted_entry(
                prop::TEXT_ALIGN,
                COMPUTED_KIND_KEYWORD,
                element_adjustment.text_align as f64,
            ));
        }
        if !adjustments.is_empty() {
            let longhand_table = unsafe { &mut *longhand_table };
            for entry in &adjustments {
                store_computed_value(longhand_table, entry);
            }
            longhand_table.finish_drive_inheritance_dependent_values();
        }
        if matches!(
            adjustment.text_align_before,
            keyword::MATCH_PARENT | keyword::_LIBWEB_INHERIT_OR_CENTER
        ) {
            unsafe { &mut *longhand_table }.add_inheritance_dependent_value(
                prop::TEXT_ALIGN,
                retained_new(StyleValueData::Keyword {
                    keyword: adjustment.text_align_before,
                }),
            );
        }

        let longhand_table = unsafe { &mut *longhand_table };
        let mut clear_adjusted_flags = |changed, property_id| {
            if changed {
                longhand_table.set_important(property_id, false);
                longhand_table.set_inherited(property_id, false);
            }
        };
        clear_adjusted_flags(transformation.set_float_none, prop::FLOAT);
        clear_adjusted_flags(
            transformation.changed_display || element_adjustment.changed_display,
            prop::DISPLAY,
        );
        clear_adjusted_flags(line_height_changed, prop::LINE_HEIGHT);
        clear_adjusted_flags(element_adjustment.set_position_static, prop::POSITION);
        clear_adjusted_flags(element_adjustment.changed_text_align, prop::TEXT_ALIGN);
        post_adjusted_longhands
    })
}

fn property_affects_font_metrics(property_id: u16) -> bool {
    property_id == crate::css::property_metadata::property_id::FONT_SIZE
        || property_id == crate::css::property_metadata::property_id::LINE_HEIGHT
}

#[repr(C)]
pub struct FfiExpandedProperty {
    pub property_id: u16,
    pub data: *const c_void,
}

#[repr(C)]
pub struct FfiShorthandExpansion {
    pub properties: *const FfiExpandedProperty,
    pub count: usize,
    pub storage: *mut c_void,
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

/// The expansion recursion over shared Rust value data. `has_style_sheet_context`
/// follows boundary values through CSS-wide propagation, while nested shorthand
/// values have no facade-local resource context.
pub(crate) fn expand_shorthands_with<Sink>(
    property_id: u16,
    data: *const c_void,
    has_style_sheet_context: bool,
    sink: &mut Sink,
) where
    Sink: FnMut(u16, *const c_void, bool),
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
        sink(property_id, data, has_style_sheet_context);
        let retained_data = unsafe { crate::css::style_value::retain_style_value(data.cast::<StyleValueData>()) };
        let pending_data =
            unsafe { crate::css::style_value::rust_style_value_create_pending_substitution(retained_data) };
        let pending = unsafe { RetainedStyleValueData::from_retained_pointer(pending_data) };
        for &longhand in longhands_for_shorthand(property_id) {
            expand_shorthands_with(longhand, pending.pointer().cast(), has_style_sheet_context, sink);
        }
        return;
    }

    if let StyleValueData::Shorthand {
        sub_properties, values, ..
    } = value
    {
        for (&sub_property, sub_value) in sub_properties.as_slice().iter().zip(values.as_slice()) {
            let sub_data = sub_value.pointer().cast();
            expand_shorthands_with(sub_property, sub_data, false, sink);
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
            expand_shorthands_with(longhand, data, has_style_sheet_context, sink);
        }
        return;
    }

    sink(property_id, data, has_style_sheet_context);
}

struct ShorthandExpansion {
    properties: Vec<FfiExpandedProperty>,
    _values: Vec<RetainedStyleValueData>,
}

fn expand_shorthands(property_id: u16, data: *const c_void) -> ShorthandExpansion {
    let mut properties = Vec::new();
    let mut values = Vec::new();
    expand_shorthands_with(property_id, data, false, &mut |longhand_id, longhand_data, _| {
        let value = unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                longhand_data.cast(),
            ))
        };
        properties.push(FfiExpandedProperty {
            property_id: longhand_id,
            data: value.pointer().cast(),
        });
        values.push(value);
    });
    ShorthandExpansion {
        properties,
        _values: values,
    }
}

/// Expands a declared property into one owned batch of longhand assignments, recursing through
/// shorthand and pending-substitution values.
///
/// # Safety
/// `data` must point to live Rust-owned style value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_expand_property_shorthands(
    property_id: u16,
    data: *const c_void,
) -> FfiShorthandExpansion {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::ShorthandExpansionEntry);
    abort_on_panic(|| {
        let expansion = Box::new(expand_shorthands(property_id, data));
        let properties = expansion.properties.as_ptr();
        let count = expansion.properties.len();
        FfiShorthandExpansion {
            properties,
            count,
            storage: Box::into_raw(expansion).cast(),
        }
    })
}

/// # Safety
/// `storage` must be a live pointer returned by `rust_expand_property_shorthands`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_shorthand_expansion_destroy(storage: *mut c_void) {
    abort_on_panic(|| drop(unsafe { Box::from_raw(storage.cast::<ShorthandExpansion>()) }));
}

pub(crate) fn display_is_none(raw: u32) -> bool {
    FfiDisplay::from_raw(raw).is_none()
}

/// The element facts the box type transformation needs, marshalled by the C++
/// side. The parent display is the first non-`display: contents` ancestor's.
#[repr(C)]
#[derive(Clone, Copy)]
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
    pub is_wbr_element: bool,
    pub disallow_display_contents: bool,
    pub rewrite_inline_flow: bool,
    pub is_button_element: bool,
    pub force_line_height_normal: bool,
    pub check_input_line_height: bool,
    pub hide_audio_without_controls: bool,
    pub is_table_element: bool,
    pub force_position_static: bool,
    pub force_symbol_display_inline: bool,
}

/// Result of the box type transformation: whether float must be reset to none,
/// and the possibly replaced display.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiBoxTypeTransformation {
    pub set_float_none: bool,
    pub changed_display: bool,
    pub display: FfiDisplay,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiElementStyleAdjustment {
    pub changed_display: bool,
    pub display: FfiDisplay,
    pub set_line_height_normal: bool,
    pub check_input_line_height: bool,
    pub set_position_static: bool,
    pub changed_text_align: bool,
    pub text_align: u16,
}

/// The automatic box type transformation and element-specific adjustment,
/// sequenced as they are in the normal property computation path.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiElementStyleAdjustments {
    pub box_type: FfiBoxTypeTransformation,
    pub element_style: FfiElementStyleAdjustment,
}

#[repr(C)]
pub struct FfiInputLineHeightMetrics {
    pub current_line_height: f64,
    pub minimum_line_height: f64,
}

fn should_clamp_input_line_height(adjustment: &FfiElementStyleAdjustment, metrics: &FfiInputLineHeightMetrics) -> bool {
    adjustment.check_input_line_height && metrics.current_line_height < metrics.minimum_line_height
}

fn adjust_element_style(
    input: &FfiBoxTypeTransformationInput,
    display: FfiDisplay,
    text_align: u16,
) -> FfiElementStyleAdjustment {
    let mut new_display = display;
    if input.is_wbr_element && display.is_contents() {
        new_display = FfiDisplay::none();
    }
    if input.disallow_display_contents && new_display.is_contents() {
        new_display = FfiDisplay::none();
    }
    if input.rewrite_inline_flow && new_display.is_inline_outside() && new_display.inside == display_inside::FLOW {
        new_display = FfiDisplay::inline_block();
    }
    if input.is_button_element
        && !(new_display.is_flex_inside()
            || new_display.is_grid_inside()
            || new_display.is_none()
            || new_display.is_contents())
    {
        new_display = if new_display.is_inline_outside() {
            FfiDisplay::inline_block()
        } else {
            FfiDisplay::flow_root()
        };
    }
    if input.is_br_element {
        new_display = if new_display.is_contents() {
            FfiDisplay::none()
        } else if new_display.is_none() {
            new_display
        } else {
            FfiDisplay::inline()
        };
    }
    if input.hide_audio_without_controls {
        new_display = FfiDisplay::none();
    }
    if input.force_symbol_display_inline {
        new_display = FfiDisplay::inline();
    }

    let new_text_align = if input.is_table_element
        && matches!(
            text_align,
            keyword::_LIBWEB_LEFT | keyword::_LIBWEB_CENTER | keyword::_LIBWEB_RIGHT
        ) {
        keyword::START
    } else {
        text_align
    };

    FfiElementStyleAdjustment {
        changed_display: new_display != display,
        display: new_display,
        set_line_height_normal: input.force_line_height_normal,
        check_input_line_height: input.check_input_line_height,
        set_position_static: input.force_position_static,
        changed_text_align: new_text_align != text_align,
        text_align: new_text_align,
    }
}

fn compute_element_style_adjustments(
    input: &FfiBoxTypeTransformationInput,
    text_align: u16,
) -> FfiElementStyleAdjustments {
    let box_type = transform_box_type(input);
    let display = if box_type.changed_display {
        box_type.display
    } else {
        input.display
    };
    let element_style = adjust_element_style(input, display, text_align);
    FfiElementStyleAdjustments {
        box_type,
        element_style,
    }
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
fn transform_box_type(input: &FfiBoxTypeTransformationInput) -> FfiBoxTypeTransformation {
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
                    new_display =
                        FfiDisplay::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, display.list_item);
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
}

/// # Safety
/// `input` must be a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_adjust_element_style(
    input: *const FfiBoxTypeTransformationInput,
    text_align: u16,
) -> FfiElementStyleAdjustments {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| compute_element_style_adjustments(unsafe { &*input }, text_align))
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
fn resolve_effective_overflow_keywords(overflow_x: u16, overflow_y: u16) -> FfiEffectiveOverflow {
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
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_resolve_effective_overflow_keywords(overflow_x: u16, overflow_y: u16) -> FfiEffectiveOverflow {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| resolve_effective_overflow_keywords(overflow_x, overflow_y))
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
fn resolve_effective_color_scheme(
    schemes: &[u8],
    preferred_scheme: u8,
    document_supported_schemes: Option<&[u8]>,
) -> u8 {
    const AUTO: u8 = 0;
    const LIGHT: u8 = 2;

    // To determine the used color scheme of an element:

    // 1. If the user’s preferred color scheme, as indicated by the prefers-color-scheme media feature,
    //    is present among the listed color schemes, and is supported by the user agent,
    //    that’s the element’s used color scheme.
    if preferred_scheme != AUTO && schemes.contains(&preferred_scheme) {
        return preferred_scheme;
    }

    // 2. Otherwise, if the user has indicated an overriding preference for their chosen color scheme,
    //    and the only keyword is not present in color-scheme for the element,
    //    the user agent must override the color scheme with the user’s preferred color scheme.
    //    See § 2.3 Overriding the Color Scheme.
    // FIXME: We don't currently support setting an "overriding preference" for color schemes.

    // 3. Otherwise, if the user agent supports at least one of the listed color schemes,
    //    the used color scheme is the first supported color scheme in the list.
    if let Some(first_supported) = schemes.iter().find(|scheme| **scheme != AUTO) {
        return *first_supported;
    }

    // 4. Otherwise, the used color scheme is the browser default. (Same as normal.)
    // `normal` indicates that the element supports the page’s supported color schemes, if they are set
    if let Some(document_supported_schemes) = document_supported_schemes {
        if preferred_scheme != AUTO && document_supported_schemes.contains(&preferred_scheme) {
            return preferred_scheme;
        }
        if let Some(first_supported) = document_supported_schemes.iter().find(|scheme| **scheme != AUTO) {
            return *first_supported;
        }
    }

    LIGHT
}

/// Resolves the used color scheme for a value that required C++ computation.
///
/// # Safety
/// `value` must point at valid ColorScheme StyleValueData and `input` at valid
/// document inputs for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolve_effective_color_scheme(
    value: *const c_void,
    input: *const FfiEffectiveColorSchemeInput,
) -> u8 {
    abort_on_panic(|| {
        let StyleValueData::ColorScheme { scheme_codes, .. } = (unsafe { &*(value as *const StyleValueData) }) else {
            unreachable!("computed color-scheme must have color-scheme data");
        };
        let input = unsafe { &*input };
        resolve_effective_color_scheme(scheme_codes.as_slice(), input.preferred_color_scheme, unsafe {
            input.document_supported_schemes()
        })
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
fn compute_text_align_adjustment(
    text_align: u16,
    is_th_element: bool,
    has_parent_with_computed_values: bool,
    parent_text_align: u16,
    parent_direction_is_ltr: bool,
) -> FfiTextAlignAdjustment {
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
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_compute_text_align(
    text_align: u16,
    is_th_element: bool,
    has_parent_with_computed_values: bool,
    parent_text_align: u16,
    parent_direction_is_ltr: bool,
) -> FfiTextAlignAdjustment {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        compute_text_align_adjustment(
            text_align,
            is_th_element,
            has_parent_with_computed_values,
            parent_text_align,
            parent_direction_is_ltr,
        )
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::NestedPropertyComputeEntry);
    abort_on_panic(|| {
        let value = unsafe { &*(absolutized_value as *const StyleValueData) };
        compute_font_weight(value, inherited_font_weight)
    })
}

// The exported computed-values FFI shares one header; keep an anchor so the
// context types stay in the generated bindings even without other references.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_compute_context_anchor(_context: *const c_void) {}

// The standalone cargo test binary has no C++ side, so release callbacks are
// stubbed out here.
#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_utf16_fly_string_unref(_raw: usize) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_utf16_fly_string_view(
        _raw: usize,
        _short_buffer: *mut u8,
    ) -> crate::css::serialize::FfiFlyStringView {
        crate::css::serialize::FfiFlyStringView::empty()
    }
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_string_unref(_raw: usize) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_animated_properties_unref(_values: *const std::ffi::c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_gfx_font_cascade_list_unref(_raw: *const std::ffi::c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_gfx_font_unref(_raw: *const std::ffi::c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_gfx_filter_destroy(_filter: *mut std::ffi::c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_gfx_glyph_run_unref(_retained: *mut std::ffi::c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_gfx_path_destroy(_path: *mut std::ffi::c_void) {}
    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_segmenter_destroy(_handle: *mut std::ffi::c_void) {}
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn effective_overflow_keywords_compute_together() {
        let result = resolve_effective_overflow_keywords(keyword::VISIBLE, keyword::AUTO);
        assert!(result.changed_x);
        assert_eq!(result.x_keyword, keyword::AUTO);
        assert!(!result.changed_y);
        assert_eq!(result.y_keyword, keyword::AUTO);

        let result = resolve_effective_overflow_keywords(keyword::CLIP, keyword::SCROLL);
        assert!(result.changed_x);
        assert_eq!(result.x_keyword, keyword::HIDDEN);
        assert!(!result.changed_y);
        assert_eq!(result.y_keyword, keyword::SCROLL);

        let result = resolve_effective_overflow_keywords(keyword::VISIBLE, keyword::CLIP);
        assert!(!result.changed_x);
        assert!(!result.changed_y);
    }

    #[test]
    fn effective_color_scheme_follows_element_and_document_preferences() {
        const AUTO: u8 = 0;
        const DARK: u8 = 1;
        const LIGHT: u8 = 2;

        assert_eq!(resolve_effective_color_scheme(&[LIGHT, DARK], DARK, None), DARK);
        assert_eq!(resolve_effective_color_scheme(&[DARK, LIGHT], LIGHT, None), LIGHT);
        assert_eq!(resolve_effective_color_scheme(&[DARK, LIGHT], AUTO, None), DARK);
        assert_eq!(
            resolve_effective_color_scheme(&[AUTO], DARK, Some(&[LIGHT, DARK])),
            DARK
        );
        assert_eq!(resolve_effective_color_scheme(&[], AUTO, Some(&[DARK])), DARK);
        assert_eq!(resolve_effective_color_scheme(&[], AUTO, None), LIGHT);
    }

    #[test]
    fn text_align_adjusts_match_parent_and_table_header_defaults() {
        let result = compute_text_align_adjustment(keyword::MATCH_PARENT, false, true, keyword::START, false);
        assert!(result.changed);
        assert_eq!(result.keyword, keyword::RIGHT);
        assert!(!result.inherited);

        let result =
            compute_text_align_adjustment(keyword::_LIBWEB_INHERIT_OR_CENTER, true, true, keyword::JUSTIFY, true);
        assert!(result.changed);
        assert_eq!(result.keyword, keyword::JUSTIFY);
        assert!(result.inherited);

        let result = compute_text_align_adjustment(keyword::_LIBWEB_INHERIT_OR_CENTER, true, false, 0, true);
        assert!(result.changed);
        assert_eq!(result.keyword, keyword::CENTER);
        assert!(!result.inherited);
    }

    fn element_adjustment_input() -> FfiBoxTypeTransformationInput {
        FfiBoxTypeTransformationInput {
            display: FfiDisplay::inline(),
            position: keyword::STATIC,
            float_value: keyword::NONE,
            is_br_element: false,
            is_document_element: false,
            is_mathml_element: false,
            is_mathml_mtable: false,
            is_mathml_mtr: false,
            is_mathml_mtd: false,
            has_parent_display: false,
            parent_display: FfiDisplay::block(),
            is_wbr_element: false,
            disallow_display_contents: false,
            rewrite_inline_flow: false,
            is_button_element: false,
            force_line_height_normal: false,
            check_input_line_height: false,
            hide_audio_without_controls: false,
            is_table_element: false,
            force_position_static: false,
            force_symbol_display_inline: false,
        }
    }

    #[test]
    fn element_styles_adjust_from_marshaled_facts() {
        let mut input = element_adjustment_input();
        input.disallow_display_contents = true;
        let adjustment = adjust_element_style(&input, FfiDisplay::contents(), keyword::LEFT);
        assert!(adjustment.changed_display);
        assert!(adjustment.display.is_none());

        input = element_adjustment_input();
        input.is_button_element = true;
        let adjustment = adjust_element_style(&input, FfiDisplay::inline(), keyword::LEFT);
        assert!(adjustment.changed_display);
        assert!(adjustment.display.is_inline_block());

        input = element_adjustment_input();
        input.is_table_element = true;
        let adjustment = adjust_element_style(&input, FfiDisplay::block(), keyword::_LIBWEB_CENTER);
        assert!(adjustment.changed_text_align);
        assert_eq!(adjustment.text_align, keyword::START);

        input = element_adjustment_input();
        input.check_input_line_height = true;
        let adjustment = adjust_element_style(&input, FfiDisplay::inline(), keyword::LEFT);
        assert!(should_clamp_input_line_height(
            &adjustment,
            &FfiInputLineHeightMetrics {
                current_line_height: 12.0,
                minimum_line_height: 16.0,
            }
        ));
        assert!(!should_clamp_input_line_height(
            &adjustment,
            &FfiInputLineHeightMetrics {
                current_line_height: 18.0,
                minimum_line_height: 16.0,
            }
        ));
    }

    #[test]
    fn element_style_adjustments_follow_box_type_transformation() {
        let mut input = element_adjustment_input();
        input.is_button_element = true;
        input.position = keyword::ABSOLUTE;

        let adjustments = compute_element_style_adjustments(&input, keyword::LEFT);
        assert!(adjustments.box_type.changed_display);
        assert!(adjustments.box_type.display.is_block_outside());
        assert!(adjustments.element_style.changed_display);
        assert!(adjustments.element_style.display.is_flow_root_inside());
    }

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
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: true,
            resolved_viewport_relative_length: std::ptr::null_mut(),
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
        assert_eq!(compute_border_or_outline_width(&length, 2.0, None).value, 2.5);
        // 0.4px at 1 dppx rounds up to 1 device pixel.
        let thin = StyleValueData::Length {
            value: 0.4,
            unit: unit_code("px") as u8,
        };
        assert_eq!(compute_border_or_outline_width(&thin, 1.0, None).value, 1.0);
        // medium is 3px.
        assert_eq!(
            compute_border_or_outline_width(
                &StyleValueData::Keyword {
                    keyword: keyword::MEDIUM
                },
                1.0,
                None
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
        use crate::css::property_metadata::{FIRST_INHERITED_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
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
        assert!(
            longhand_decision(Some(&currentcolor), crate::css::property_metadata::property_id::COLOR).should_inherit
        );
        assert!(!longhand_decision(Some(&currentcolor), reset_id).should_inherit);
    }

    #[test]
    fn container_relative_units_without_snapshotted_bases_are_unhandled() {
        assert!(!absolutize_length(1.0, unit_code("cqw"), &test_context()).handled);
    }

    #[test]
    fn container_relative_units_resolve_from_snapshotted_bases() {
        let mut context = test_context();
        context.has_container_width_basis = true;
        context.has_container_height_basis = true;
        context.container_width_basis = 400.0;
        context.container_height_basis = 200.0;

        assert_eq!(absolutize_length(10.0, unit_code("cqw"), &context).px, 40.0);
        assert_eq!(absolutize_length(10.0, unit_code("cqh"), &context).px, 20.0);
        assert_eq!(absolutize_length(10.0, unit_code("cqi"), &context).px, 40.0);
        assert_eq!(absolutize_length(10.0, unit_code("cqb"), &context).px, 20.0);
        assert_eq!(absolutize_length(10.0, unit_code("cqmin"), &context).px, 20.0);
        assert_eq!(absolutize_length(10.0, unit_code("cqmax"), &context).px, 40.0);

        context.subject_inline_axis_is_horizontal = false;
        assert_eq!(absolutize_length(10.0, unit_code("cqi"), &context).px, 20.0);
        assert_eq!(absolutize_length(10.0, unit_code("cqb"), &context).px, 40.0);
    }

    #[test]
    fn container_relative_viewport_fallback_records_its_dependency() {
        let mut dependency_was_recorded = false;
        let mut context = test_context();
        context.has_container_width_basis = true;
        context.container_width_basis = 800.0;
        context.container_width_basis_depends_on_viewport_metrics = true;
        context.resolved_viewport_relative_length = &mut dependency_was_recorded;

        let result = absolutize_length(10.0, unit_code("cqw"), &context);
        assert_eq!(result.px, 80.0);
        assert!(result.resolved_viewport_relative_length);
        assert!(dependency_was_recorded);
    }
}
