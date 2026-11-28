/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the ink of the glyphs of a run is, from the extents and outlines HarfBuzz gives for them.

use crate::font::FontHandle;
use crate::harfbuzz::{self, HbDrawFuncs, HbDrawState, HbGlyphExtents};
use crate::text_layout::PlacedGlyph;
use std::ffi::{c_int, c_void};
use std::sync::OnceLock;

// NB: These pick as AK's min() and max() do, which matters for NaN.
fn min<T: PartialOrd>(a: T, b: T) -> T {
    if b < a { b } else { a }
}

fn max<T: PartialOrd>(a: T, b: T) -> T {
    if a < b { b } else { a }
}

/// The horizontal and vertical scale of the font's HarfBuzz font, or None if either is not positive.
fn harfbuzz_scale(font: &FontHandle) -> Option<(c_int, c_int)> {
    let mut x_scale: c_int = 0;
    let mut y_scale: c_int = 0;
    // SAFETY: The handle keeps the font, and with it its immutable HarfBuzz font, live.
    unsafe { harfbuzz::hb_font_get_scale(font.harfbuzz_font(), &raw mut x_scale, &raw mut y_scale) };
    (x_scale > 0 && y_scale > 0).then_some((x_scale, y_scale))
}

fn glyph_extents(font: &FontHandle, glyph_id: u32) -> Option<HbGlyphExtents> {
    let mut extents = HbGlyphExtents::default();
    // SAFETY: The handle keeps the font live. HarfBuzz reads the font's tables, which it loads once in a way that is
    //         safe for any number of threads, and writes only to the local extents.
    let found = unsafe { harfbuzz::hb_font_get_glyph_extents(font.harfbuzz_font(), glyph_id, &raw mut extents) };
    (found != 0).then_some(extents)
}

/// A rectangle as `Gfx::FloatRect` keeps it, uniting as it does.
#[derive(Clone, Copy, Default)]
struct Rect {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
}

impl Rect {
    fn is_empty(&self) -> bool {
        self.width <= 0.0 || self.height <= 0.0
    }

    fn unite(&mut self, other: Rect) {
        if self.is_empty() {
            *self = other;
            return;
        }
        if other.is_empty() {
            return;
        }
        let new_left = min(self.x, other.x);
        let new_right = max(self.x + self.width, other.x + other.width);
        self.x = new_left;
        self.width = new_right - self.x;
        let new_top = min(self.y, other.y);
        let new_bottom = max(self.y + self.height, other.y + other.height);
        self.y = new_top;
        self.height = new_bottom - self.y;
    }
}

/// Tight bounds of the ink of the painted glyphs, from their own extents, in device pixels relative to the run's baseline
/// origin (glyph positions scaled by `scale`, with the font ascent added to y), as x, y, width and height. Empty if no
/// painted glyph has ink.
pub(crate) fn ink_bounding_box(font: &FontHandle, glyphs: impl Iterator<Item = PlacedGlyph>, scale: f32) -> [f32; 4] {
    let facts = font.facts();
    let Some((x_scale, y_scale)) = harfbuzz_scale(font) else {
        return [0.0; 4];
    };
    let units_to_pixels_x = facts.pixel_size * scale / x_scale as f32;
    let units_to_pixels_y = facts.pixel_size * scale / y_scale as f32;

    let mut bounds = Rect::default();
    for glyph in glyphs {
        if !glyph.glyph.should_paint() {
            continue;
        }
        let Some(extents) = glyph_extents(font, u32::from(glyph.glyph.glyph_id)) else {
            continue;
        };
        let origin_x = glyph.x * scale;
        let origin_y = (glyph.y + facts.ascent) * scale;
        // HarfBuzz extents have y pointing up, so `height` is negative for ink above the baseline.
        bounds.unite(Rect {
            x: origin_x + extents.x_bearing as f32 * units_to_pixels_x,
            y: origin_y - extents.y_bearing as f32 * units_to_pixels_y,
            width: extents.width as f32 * units_to_pixels_x,
            height: extents.height.wrapping_neg() as f32 * units_to_pixels_y,
        });
    }
    [bounds.x, bounds.y, bounds.width, bounds.height]
}

#[derive(Clone, Copy)]
struct OutlinePoint {
    x: f64,
    y: f64,
}

/// Roots of a*t^2 + b*t + c = 0 with t in [0, 1]. Duplicate roots may be reported twice.
fn quadratic_roots_in_unit_interval(a: f64, b: f64, c: f64, roots: &mut [f64; 2]) -> usize {
    let mut count = 0;
    let mut add_root = |t: f64| {
        // NOTE: A NaN root (e.g. from a degenerate division) is rejected too.
        if (0.0..=1.0).contains(&t) {
            roots[count] = t;
            count += 1;
        }
    };

    if a == 0.0 {
        if b != 0.0 {
            add_root(-c / b);
        }
        return count;
    }

    let discriminant = b * b - 4.0 * a * c;
    if discriminant < 0.0 {
        return 0;
    }

    // The numerically stable form: computing the smaller root as c / q instead of (-b - sqrt(d)) / 2a avoids
    // catastrophic cancellation when |a| is tiny compared to |b| (a nearly-linear curve).
    let q = -0.5 * (b + discriminant.sqrt().copysign(b));
    add_root(q / a);
    if q != 0.0 {
        add_root(c / q);
    }
    count
}

/// Roots of a*t^3 + b*t^2 + c*t + d = 0 with t in [0, 1]. The polynomial is split at the extrema of its derivative,
/// so each piece is monotonic and has at most one root, which is then found by bisection. Unlike a closed-form
/// solution, this can neither miss nor invent roots inside the interval.
fn cubic_roots_in_unit_interval(a: f64, b: f64, c: f64, d: f64, roots: &mut [f64; 3]) -> usize {
    let evaluate = |t: f64| ((a * t + b) * t + c) * t + d;

    let mut breakpoints = [0.0f64; 4];
    let mut breakpoint_count = 0;
    breakpoints[breakpoint_count] = 0.0;
    breakpoint_count += 1;
    let mut extrema = [0.0f64; 2];
    let extrema_count = quadratic_roots_in_unit_interval(3.0 * a, 2.0 * b, c, &mut extrema);
    if extrema_count == 2 && extrema[0] > extrema[1] {
        extrema.swap(0, 1);
    }
    for &extremum in &extrema[..extrema_count] {
        if extremum > 0.0 && extremum < 1.0 && extremum != breakpoints[breakpoint_count - 1] {
            breakpoints[breakpoint_count] = extremum;
            breakpoint_count += 1;
        }
    }
    breakpoints[breakpoint_count] = 1.0;
    breakpoint_count += 1;

    const VALUE_TOLERANCE: f64 = 1e-6;
    const PARAMETER_TOLERANCE: f64 = 1e-7;

    let mut count = 0;
    for i in 0..breakpoint_count - 1 {
        let mut start = breakpoints[i];
        let mut end = breakpoints[i + 1];
        let mut value_at_start = evaluate(start);
        let value_at_end = evaluate(end);
        if value_at_start.abs() <= VALUE_TOLERANCE {
            roots[count] = start;
            count += 1;
            continue;
        }
        if value_at_end.abs() <= VALUE_TOLERANCE {
            roots[count] = end;
            count += 1;
            continue;
        }
        if (value_at_start < 0.0) == (value_at_end < 0.0) {
            continue;
        }
        while end - start > PARAMETER_TOLERANCE {
            let middle = 0.5 * (start + end);
            let value_at_middle = evaluate(middle);
            if value_at_middle == 0.0 {
                start = middle;
                end = middle;
                break;
            }
            if (value_at_middle < 0.0) == (value_at_start < 0.0) {
                start = middle;
                value_at_start = value_at_middle;
            } else {
                end = middle;
            }
        }
        roots[count] = 0.5 * (start + end);
        count += 1;
    }
    count
}

/// Tracks the horizontal extent of a glyph outline's ink inside a horizontal band, in glyph-local device pixels with
/// y pointing down. This mirrors how Skia computes text blob intercepts: every point where an outline segment
/// crosses the top or bottom edge of the band widens the extent, and so does every segment point (including
/// control points) that lies strictly inside the band.
struct InkExtentInBand {
    band_top: f64,
    band_bottom: f64,
    units_to_pixels_x: f64,
    units_to_pixels_y: f64,
    left: f64,
    right: f64,
}

impl InkExtentInBand {
    fn to_device_pixels(&self, x: f32, y: f32) -> OutlinePoint {
        // HarfBuzz outlines have y pointing up.
        OutlinePoint {
            x: f64::from(x) * self.units_to_pixels_x,
            y: f64::from(-y) * self.units_to_pixels_y,
        }
    }

    fn expand(&mut self, x: f64) {
        self.left = min(self.left, x);
        self.right = max(self.right, x);
    }

    fn overlaps_band(&self, segment_top: f64, segment_bottom: f64) -> bool {
        self.band_top <= segment_bottom && segment_top <= self.band_bottom
    }

    fn expand_by_points_strictly_inside_band(&mut self, points: &[OutlinePoint]) {
        for point in points {
            if self.band_top < point.y && point.y < self.band_bottom {
                self.expand(point.x);
            }
        }
    }

    fn add_line(&mut self, p0: OutlinePoint, p1: OutlinePoint) {
        if !self.overlaps_band(min(p0.y, p1.y), max(p0.y, p1.y)) {
            return;
        }
        for offset in [self.band_top, self.band_bottom] {
            // NOTE: A horizontal line makes this a division by zero; the resulting inf/NaN fails the range check.
            let t = (offset - p0.y) / (p1.y - p0.y);
            if (0.0..1.0).contains(&t) {
                self.expand(p0.x + t * (p1.x - p0.x));
            }
        }
        self.expand_by_points_strictly_inside_band(&[p0, p1]);
    }

    fn add_quadratic(&mut self, p0: OutlinePoint, p1: OutlinePoint, p2: OutlinePoint) {
        if !self.overlaps_band(min(p0.y, min(p1.y, p2.y)), max(p0.y, max(p1.y, p2.y))) {
            return;
        }
        // y(t) = (y0 - 2*y1 + y2) * t^2 + 2 * (y1 - y0) * t + y0
        let a = p0.y - 2.0 * p1.y + p2.y;
        let b = 2.0 * (p1.y - p0.y);
        for offset in [self.band_top, self.band_bottom] {
            let mut roots = [0.0f64; 2];
            let count = quadratic_roots_in_unit_interval(a, b, p0.y - offset, &mut roots);
            for &t in &roots[..count] {
                let u = 1.0 - t;
                self.expand(u * u * p0.x + 2.0 * u * t * p1.x + t * t * p2.x);
            }
        }
        self.expand_by_points_strictly_inside_band(&[p0, p1, p2]);
    }

    fn add_cubic(&mut self, p0: OutlinePoint, p1: OutlinePoint, p2: OutlinePoint, p3: OutlinePoint) {
        if !self.overlaps_band(
            min(min(p0.y, p1.y), min(p2.y, p3.y)),
            max(max(p0.y, p1.y), max(p2.y, p3.y)),
        ) {
            return;
        }
        // y(t) = (-y0 + 3*y1 - 3*y2 + y3) * t^3 + (3*y0 - 6*y1 + 3*y2) * t^2 + (-3*y0 + 3*y1) * t + y0
        let a = -p0.y + 3.0 * p1.y - 3.0 * p2.y + p3.y;
        let b = 3.0 * p0.y - 6.0 * p1.y + 3.0 * p2.y;
        let c = -3.0 * p0.y + 3.0 * p1.y;
        for offset in [self.band_top, self.band_bottom] {
            let mut roots = [0.0f64; 3];
            let count = cubic_roots_in_unit_interval(a, b, c, p0.y - offset, &mut roots);
            for &t in &roots[..count] {
                let u = 1.0 - t;
                self.expand(u * u * u * p0.x + 3.0 * u * u * t * p1.x + 3.0 * u * t * t * p2.x + t * t * t * p3.x);
            }
        }
        self.expand_by_points_strictly_inside_band(&[p0, p1, p2, p3]);
    }
}

/// # Safety
///
/// `draw_data` must point to the `InkExtentInBand` that `hb_font_draw_glyph()` was given, and `state` to HarfBuzz's
/// drawing state.
unsafe fn ink_extent_and_current_point<'a>(
    draw_data: *mut c_void,
    state: *const HbDrawState,
) -> (&'a mut InkExtentInBand, OutlinePoint) {
    // SAFETY: Guaranteed by the caller: HarfBuzz hands back the data it was given, for the duration of the callback.
    let ink_extent = unsafe { &mut *draw_data.cast::<InkExtentInBand>() };
    // SAFETY: Guaranteed by the caller.
    let state = unsafe { &*state };
    let current_point = ink_extent.to_device_pixels(state.current_x, state.current_y);
    (ink_extent, current_point)
}

unsafe extern "C" fn line_to(
    _: *mut HbDrawFuncs,
    draw_data: *mut c_void,
    state: *mut HbDrawState,
    to_x: f32,
    to_y: f32,
    _: *mut c_void,
) {
    // SAFETY: HarfBuzz calls this with the data glyph_intercepts() gave it, and its own state.
    let (ink_extent, current_point) = unsafe { ink_extent_and_current_point(draw_data, state) };
    let to = ink_extent.to_device_pixels(to_x, to_y);
    ink_extent.add_line(current_point, to);
}

unsafe extern "C" fn quadratic_to(
    _: *mut HbDrawFuncs,
    draw_data: *mut c_void,
    state: *mut HbDrawState,
    control_x: f32,
    control_y: f32,
    to_x: f32,
    to_y: f32,
    _: *mut c_void,
) {
    // SAFETY: As for line_to().
    let (ink_extent, current_point) = unsafe { ink_extent_and_current_point(draw_data, state) };
    let control = ink_extent.to_device_pixels(control_x, control_y);
    let to = ink_extent.to_device_pixels(to_x, to_y);
    ink_extent.add_quadratic(current_point, control, to);
}

#[allow(
    clippy::too_many_arguments,
    reason = "HarfBuzz gives the points one coordinate at a time"
)]
unsafe extern "C" fn cubic_to(
    _: *mut HbDrawFuncs,
    draw_data: *mut c_void,
    state: *mut HbDrawState,
    control1_x: f32,
    control1_y: f32,
    control2_x: f32,
    control2_y: f32,
    to_x: f32,
    to_y: f32,
    _: *mut c_void,
) {
    // SAFETY: As for line_to().
    let (ink_extent, current_point) = unsafe { ink_extent_and_current_point(draw_data, state) };
    let control1 = ink_extent.to_device_pixels(control1_x, control1_y);
    let control2 = ink_extent.to_device_pixels(control2_x, control2_y);
    let to = ink_extent.to_device_pixels(to_x, to_y);
    ink_extent.add_cubic(current_point, control1, control2, to);
}

/// HarfBuzz's draw functions that feed an `InkExtentInBand`.
struct InkExtentDrawFuncs(*mut HbDrawFuncs);

// SAFETY: The draw functions are made immutable before anything else sees them, and HarfBuzz lets any number of threads
//         draw with an immutable hb_draw_funcs_t at once: drawing reads the callbacks and changes nothing in it.
unsafe impl Send for InkExtentDrawFuncs {}
// SAFETY: See the Send implementation above.
unsafe impl Sync for InkExtentDrawFuncs {}

// NB: HarfBuzz emits the implicit line that closes each contour as a regular line_to before close_path, so only
//     the segment callbacks are needed. The quadratic callback must be set, otherwise HarfBuzz converts quadratic
//     segments to cubics.
fn ink_extent_draw_funcs() -> *mut HbDrawFuncs {
    // NB: Made once and never let go of, as each copy of the crate makes its own, which only costs one more.
    static INK_EXTENT_DRAW_FUNCS: OnceLock<InkExtentDrawFuncs> = OnceLock::new();
    INK_EXTENT_DRAW_FUNCS
        .get_or_init(|| {
            // SAFETY: The draw functions are this thread's own until they are made immutable and published.
            unsafe {
                let funcs = harfbuzz::hb_draw_funcs_create();
                harfbuzz::hb_draw_funcs_set_line_to_func(funcs, line_to, std::ptr::null_mut(), None);
                harfbuzz::hb_draw_funcs_set_quadratic_to_func(funcs, quadratic_to, std::ptr::null_mut(), None);
                harfbuzz::hb_draw_funcs_set_cubic_to_func(funcs, cubic_to, std::ptr::null_mut(), None);
                harfbuzz::hb_draw_funcs_make_immutable(funcs);
                InkExtentDrawFuncs(funcs)
            }
        })
        .0
}

/// Pairs of [start, end] device-pixel x values, relative to the run's baseline origin, where glyph ink intersects the
/// horizontal band [y_top, y_bottom]. One pair per painted glyph that has ink inside the band, in run order.
pub(crate) fn glyph_intercepts(
    font: &FontHandle,
    glyphs: impl Iterator<Item = PlacedGlyph>,
    scale: f32,
    origin_y: f32,
    y_top: f32,
    y_bottom: f32,
) -> Vec<f32> {
    // NB: A NaN scale is not usable either.
    let scale_is_usable = scale > 0.0 && scale.is_finite();
    if !scale_is_usable {
        return Vec::new();
    }

    let Some((x_scale, y_scale)) = harfbuzz_scale(font) else {
        return Vec::new();
    };
    let facts = font.facts();
    let units_to_pixels_x = f64::from(facts.pixel_size) * f64::from(scale) / f64::from(x_scale);
    let units_to_pixels_y = f64::from(facts.pixel_size) * f64::from(scale) / f64::from(y_scale);
    let font_ascent = facts.ascent;

    let mut intervals = Vec::new();
    for glyph in glyphs {
        if !glyph.glyph.should_paint() {
            continue;
        }
        let glyph_id = u32::from(glyph.glyph.glyph_id);
        let origin_x = f64::from(glyph.x) * f64::from(scale);
        // The rasterizer draws each glyph of horizontal text from a baseline it puts on a whole device pixel, so the
        // glyph's ink is where that baseline puts it. origin_y is where the run's origin falls in device pixels.
        let baseline_y = f64::from(origin_y) + (f64::from(glyph.y) + f64::from(font_ascent)) * f64::from(scale);
        let glyph_origin_y = (baseline_y + 0.5).floor() - f64::from(origin_y);

        // Move the band into glyph-local coordinates, with the origin at the glyph's baseline origin.
        let band_top = f64::from(y_top) - glyph_origin_y;
        let band_bottom = f64::from(y_bottom) - glyph_origin_y;

        // Skip glyphs whose bounding box doesn't reach the band before decoding their outline.
        if let Some(extents) = glyph_extents(font, glyph_id) {
            let glyph_top = f64::from(extents.y_bearing.wrapping_neg()) * units_to_pixels_y;
            let glyph_bottom =
                f64::from(extents.y_bearing.wrapping_add(extents.height).wrapping_neg()) * units_to_pixels_y;
            if glyph_bottom < band_top || band_bottom < glyph_top {
                continue;
            }
        }

        let mut ink_extent = InkExtentInBand {
            band_top,
            band_bottom,
            units_to_pixels_x,
            units_to_pixels_y,
            left: f64::MAX,
            right: f64::MIN,
        };
        // SAFETY: The handle keeps the font live, and drawing reads it as getting extents does. The draw functions are
        //         immutable, and the callbacks write only to the local ink extent, which outlives this call.
        unsafe {
            harfbuzz::hb_font_draw_glyph(
                font.harfbuzz_font(),
                glyph_id,
                ink_extent_draw_funcs(),
                (&raw mut ink_extent).cast(),
            )
        };

        // Glyphs without an outline (e.g. spaces) never widen the extent and produce no interval.
        if ink_extent.left < ink_extent.right {
            intervals.push((ink_extent.left + origin_x) as f32);
            intervals.push((ink_extent.right + origin_x) as f32);
        }
    }
    intervals
}
