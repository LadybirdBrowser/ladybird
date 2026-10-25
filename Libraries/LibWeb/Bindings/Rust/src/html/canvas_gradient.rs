/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasgradient

use core::cell::RefCell;

use ak::Utf16String;
use libcompositing_rust::display_list::canvas_2d::{ColorStops, Paint};
use libgfx_rust::{Color, FloatPoint};
use libjs_rust::api::{Trace, Vm};

use super::color_stops::ColorStopList;
use super::predefined_color_space::PredefinedColorSpace;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    /// Parses the color as the CSS color value of a canvas without an element to resolve it against, which LibWeb's
    /// C++ does, and returns whether it is one. The color is given in each predefined color space, in the order of
    /// PredefinedColorSpace. The string is a borrowed AK::Utf16String.
    fn web_canvas_gradient_parse_color(color: usize, out_colors: *mut [Color; 4]) -> bool;
}

/// Where the colors of a gradient go, which the method of the context that created it decides. The points and the
/// radii are in the coordinates that a draw is transformed from.
#[derive(Clone, Copy)]
enum GradientGeometry {
    Linear {
        start_point: FloatPoint,
        end_point: FloatPoint,
    },
    Radial {
        start_center: FloatPoint,
        start_radius: f32,
        end_center: FloatPoint,
        end_radius: f32,
    },
    Conic {
        center: FloatPoint,
        start_angle: f32,
    },
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CanvasGradient {
    #[gc(untraced)]
    geometry: GradientGeometry,
    #[gc(untraced)]
    color_stops: RefCell<ColorStopList>,
}

impl Wrappable for CanvasGradient {}

#[allow(
    clippy::cast_possible_truncation,
    reason = "the gradient keeps its geometry in floats, as the C++ did"
)]
fn point(x: f64, y: f64) -> FloatPoint {
    FloatPoint {
        x: x as f32,
        y: y as f32,
    }
}

impl CanvasGradient {
    fn create(vm: &Vm, geometry: GradientGeometry) -> Ref<Self> {
        allocate(vm, |_| Self {
            geometry,
            color_stops: RefCell::new(ColorStopList::default()),
        })
    }

    /// The gradient of createLinearGradient(), https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createlineargradient
    pub(crate) fn create_linear(vm: &Vm, x0: f64, y0: f64, x1: f64, y1: f64) -> Ref<Self> {
        Self::create(
            vm,
            GradientGeometry::Linear {
                start_point: point(x0, y0),
                end_point: point(x1, y1),
            },
        )
    }

    /// The gradient of createRadialGradient(), whose radii the context has checked,
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createradialgradient
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the gradient keeps its geometry in floats, as the C++ did"
    )]
    pub(crate) fn create_radial(vm: &Vm, x0: f64, y0: f64, r0: f64, x1: f64, y1: f64, r1: f64) -> Ref<Self> {
        Self::create(
            vm,
            GradientGeometry::Radial {
                start_center: point(x0, y0),
                start_radius: r0 as f32,
                end_center: point(x1, y1),
                end_radius: r1 as f32,
            },
        )
    }

    /// The gradient of createConicGradient(), https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createconicgradient
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the gradient keeps its geometry in floats, as the C++ did"
    )]
    pub(crate) fn create_conic(vm: &Vm, start_angle: f64, x: f64, y: f64) -> Ref<Self> {
        Self::create(
            vm,
            GradientGeometry::Conic {
                center: point(x, y),
                start_angle: start_angle as f32,
            },
        )
    }

    /// Whether a draw with the gradient can change a pixel: whether one of its stops is not transparent.
    pub(crate) fn is_visible(&self) -> bool {
        self.color_stops
            .borrow()
            .colors(PredefinedColorSpace::Srgb)
            .iter()
            .any(|color| color.alpha() > 0)
    }

    /// The paint of a draw with the gradient in a context of the color space, which borrows the stops until the
    /// gradient changes.
    ///
    /// # Safety
    ///
    /// Nothing may add a stop to the gradient while the paint is in use.
    pub(crate) unsafe fn paint_unguarded(&self, color_space: PredefinedColorSpace) -> Paint<'_> {
        // SAFETY: The caller keeps the stops from changing while the paint borrows them.
        let color_stops = unsafe { self.color_stops.try_borrow_unguarded() }.expect("nothing adds a stop meanwhile");
        self.paint_with_color_stops(color_stops, color_space)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#interpolation
    /// The color of the gradient at each stop is the color specified for that stop, converted to the context's color
    /// space.
    fn paint_with_color_stops<'a>(
        &self,
        color_stops: &'a ColorStopList,
        color_space: PredefinedColorSpace,
    ) -> Paint<'a> {
        let color_stops = ColorStops {
            colors: color_stops.colors(color_space),
            positions: color_stops.positions(),
        };
        match self.geometry {
            GradientGeometry::Linear { start_point, end_point } => Paint::LinearGradient {
                start_point,
                end_point,
                color_stops,
            },
            GradientGeometry::Radial {
                start_center,
                start_radius,
                end_center,
                end_radius,
            } => Paint::RadialGradient {
                start_center,
                start_radius,
                end_center,
                end_radius,
                color_stops,
            },
            GradientGeometry::Conic { center, start_angle } => Paint::ConicGradient {
                center,
                start_angle,
                color_stops,
            },
        }
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvasgradient-addcolorstop
    pub(crate) fn add_color_stop(&self, _: &mut CallContext, offset: f64, color: Utf16String) -> WebIdlResult<()> {
        // 1. If the offset is less than 0 or greater than 1, then throw an "IndexSizeError" DOMException.
        if !(0.0..=1.0).contains(&offset) {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"CanvasGradient color stop offset out of bounds",
            )));
        }

        // 2. Let parsed color be the result of parsing color.
        // https://drafts.csswg.org/css-color/#parse-a-css-color-value
        let mut parsed_color = [Color(0); 4];
        // SAFETY: The string is live for the call, and the colors are a local.
        let is_color = unsafe { web_canvas_gradient_parse_color(color.raw_identity(), &raw mut parsed_color) };

        // 3. If parsed color is failure, throw a "SyntaxError" DOMException.
        if !is_color {
            return Err(Exception::syntax_error(Utf16String::from_ascii(
                b"Could not parse color for CanvasGradient",
            )));
        }

        // 4. Place a new stop on the gradient, at offset offset relative to the whole gradient, and with the color
        //    parsed color.
        //    If multiple stops are added at the same offset on a gradient, then they must be placed in the order
        //    added, with the first one closest to the start of the gradient, and each subsequent one infinitesimally
        //    further along towards the end point (in effect causing all but the first and last stop added at each
        //    point to be ignored).
        #[allow(
            clippy::cast_possible_truncation,
            reason = "the gradient keeps its stops in floats, as the C++ did"
        )]
        self.color_stops.borrow_mut().add(offset as f32, parsed_color);

        Ok(())
    }
}
