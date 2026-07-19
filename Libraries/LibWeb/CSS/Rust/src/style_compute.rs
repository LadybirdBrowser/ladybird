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

// The exported computed-values FFI shares one header; keep an anchor so the
// context types stay in the generated bindings even without other references.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_compute_context_anchor(_context: *const c_void) {}

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
    fn container_relative_units_are_unhandled() {
        assert!(!absolutize_length(1.0, unit_code("cqw"), &test_context()).handled);
    }
}
