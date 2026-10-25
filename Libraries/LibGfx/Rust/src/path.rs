/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::sync::OnceLock;

use crate::geometry::{FloatPoint, FloatSize};
use crate::matrix::AffineTransform;
use crate::paint_enums::WindingRule;
use geometry::PathGeometry;
pub use measure::PathVertex;

mod arcs;
mod curves;
mod encoding;
pub mod ffi;
mod geometry;
mod hit_test;
mod measure;
mod stroke;
mod svg_text;

unsafe extern "C" {
    fn ladybird_gfx_process_next_path_identity() -> u64;
    fn ladybird_gfx_font_glyph_outlines(
        font: *const c_void,
        glyphs: *const crate::text_layout::DrawGlyph,
        glyph_count: usize,
        visit: unsafe extern "C" fn(*mut c_void, usize, *const ffi::PathGeometryData),
        context: *mut c_void,
    );
}

/// A shaped run of glyphs in one font, as the path operations below take one.
pub struct GlyphRun {
    pub font: crate::font::FontHandle,
    pub glyphs: Vec<crate::text_layout::DrawGlyph>,
}

struct GlyphOutlineSink<'a> {
    geometry: &'a mut PathGeometry,
    transforms: &'a [AffineTransform],
}

unsafe extern "C" fn append_glyph_outline(
    context: *mut c_void,
    glyph_index: usize,
    outline: *const ffi::PathGeometryData,
) {
    // SAFETY: `context` is the sink that append_glyph_outlines() passes, and the outline is a live path for this
    // synchronous call.
    unsafe {
        let sink = &mut *context.cast::<GlyphOutlineSink<'_>>();
        let transform = sink.transforms[glyph_index];
        (*outline).append_mapped_to(sink.geometry, |point| transform.map_point(point));
    }
}

/// Adds the outline of each glyph, in pixels with its origin on the baseline, mapped by the transform at the same
/// index.
fn append_glyph_outlines(
    geometry: &mut PathGeometry,
    font: &crate::font::FontHandle,
    glyphs: &[crate::text_layout::DrawGlyph],
    transforms: &[AffineTransform],
) {
    debug_assert_eq!(glyphs.len(), transforms.len());
    if glyphs.is_empty() {
        return;
    }
    let mut sink = GlyphOutlineSink { geometry, transforms };
    // SAFETY: The handle keeps the font live, and the glyphs and the sink stay valid for the synchronous call.
    unsafe {
        ladybird_gfx_font_glyph_outlines(
            font.as_raw(),
            glyphs.as_ptr(),
            glyphs.len(),
            append_glyph_outline,
            (&raw mut sink).cast(),
        );
    }
}

/// How a path is stroked, as `Path::stroke_to_fill()` takes it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct StrokeStyle {
    pub thickness: f32,
    pub cap_style: crate::CapStyle,
    pub join_style: crate::JoinStyle,
    pub miter_limit: f32,
    pub dash_array: Vec<f32>,
    pub dash_offset: f32,
}

/// A path, with the rules of `Gfx::Path` for each operation.
pub struct Path {
    geometry: PathGeometry,
    // A process-unique, never-reused identity, so that consumers that hold a copied snapshot can
    // recognize an unchanged path without comparing contents. It is taken when first asked for, and a
    // change to the path drops it.
    identity: OnceLock<u64>,
}

impl Default for Path {
    fn default() -> Self {
        Self::new()
    }
}

impl std::fmt::Debug for Path {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Path")
            .field("identity", &self.identity.get())
            .finish()
    }
}

impl Path {
    pub fn new() -> Self {
        Self::from_geometry(PathGeometry::new())
    }

    fn from_geometry(geometry: PathGeometry) -> Self {
        Self {
            geometry,
            identity: OnceLock::new(),
        }
    }

    fn geometry_mut(&mut self) -> &mut PathGeometry {
        self.identity.take();
        &mut self.geometry
    }

    #[inline]
    pub fn identity(&self) -> u64 {
        // The counter is LibGfx's: this crate is compiled into more than one library, and a counter
        // here would let each of them hand out the same identities. See `LibGfx/RustProcessState.cpp`.
        // SAFETY: The counter is an atomic on the other side of the boundary.
        *self
            .identity
            .get_or_init(|| unsafe { ladybird_gfx_process_next_path_identity() })
    }

    pub fn move_to(&mut self, point: FloatPoint) {
        self.geometry_mut().move_to(point);
    }

    pub fn line_to(&mut self, point: FloatPoint) {
        self.geometry_mut().line_to(point);
    }

    pub fn quadratic_bezier_curve_to(&mut self, through: FloatPoint, point: FloatPoint) {
        self.geometry_mut().quadratic_bezier_curve_to(through, point);
    }

    pub fn cubic_bezier_curve_to(&mut self, c1: FloatPoint, c2: FloatPoint, point: FloatPoint) {
        self.geometry_mut().cubic_bezier_curve_to(c1, c2, point);
    }

    /// An arc of an ellipse, with the parameters of the SVG path command. The rotation is in radians.
    pub fn elliptical_arc_to(
        &mut self,
        point: FloatPoint,
        radii: FloatSize,
        x_axis_rotation: f32,
        large_arc: bool,
        sweep: bool,
    ) {
        self.geometry_mut()
            .elliptical_arc_to(point, (radii.width, radii.height), x_axis_rotation, large_arc, sweep);
    }

    pub fn arc_to(&mut self, point: FloatPoint, radius: f32, large_arc: bool, sweep: bool) {
        self.geometry_mut().arc_to(point, radius, large_arc, sweep);
    }

    pub fn close(&mut self) {
        self.geometry_mut().close();
    }

    /// Adds all the subpaths of the other path to this one.
    pub fn append_path(&mut self, other: &Path) {
        self.geometry_mut().append_path(&other.geometry);
    }

    pub fn set_fill_rule(&mut self, fill_rule: WindingRule) {
        self.geometry_mut().set_fill_rule(fill_rule);
    }

    /// Whether the path has no current point, which Gfx::Path::is_empty() calls empty.
    pub fn is_empty(&self) -> bool {
        self.geometry.is_empty()
    }

    pub fn last_point(&self) -> FloatPoint {
        self.geometry.last_point()
    }

    pub fn bounding_box(&self) -> [f32; 4] {
        self.geometry.bounding_box()
    }

    pub fn length(&self) -> f32 {
        measure::length(&self.geometry)
    }

    /// The point at the given distance along the path, followed by the path's tangent there.
    pub fn point_at_length(&self, distance: f32) -> Option<[f32; 4]> {
        measure::point_at_length(&self.geometry, distance)
            .map(|(point, tangent)| [point.x, point.y, tangent.x, tangent.y])
    }

    /// The vertices of polylines through the path, with their distance along it (see [`measure::vertices`]).
    pub fn vertices(&self) -> Vec<PathVertex> {
        measure::vertices(&self.geometry)
    }

    pub fn contains(&self, x: f32, y: f32, winding_rule: WindingRule) -> bool {
        hit_test::contains(&self.geometry, x, y, winding_rule)
    }

    /// The outline of the area that stroking this path with the given style covers, as a path to fill with the
    /// nonzero winding rule.
    pub fn stroke_to_fill(&self, style: &StrokeStyle) -> Path {
        Self::from_geometry(stroke::stroke_to_fill(
            &self.geometry,
            &stroke::StrokeStyle {
                thickness: style.thickness,
                cap_style: style.cap_style,
                join_style: style.join_style,
                miter_limit: style.miter_limit,
                dash_array: &style.dash_array,
                dash_offset: style.dash_offset,
            },
        ))
    }

    pub fn copy_transformed(&self, affine_values: [f32; 6]) -> Path {
        Self::from_geometry(
            self.geometry
                .copy_transformed(AffineTransform { values: affine_values }),
        )
    }

    pub fn to_svg_string(&self) -> String {
        svg_text::to_svg_string(&self.geometry)
    }

    pub fn serialize_to_bytes(&self) -> Vec<u8> {
        encoding::encode(&self.geometry)
    }

    /// The path that the bytes describe. Bytes that describe no path give an empty path.
    pub fn from_serialized_bytes(bytes: &[u8]) -> Path {
        Self::from_geometry(encoding::decode(bytes).unwrap_or_default())
    }

    /// The outlines of the given glyphs, as one path.
    pub fn from_glyph_runs(runs: &[GlyphRun]) -> Path {
        let mut geometry = PathGeometry::new();
        for run in runs {
            if run.font.is_invisible() {
                continue;
            }
            let ascent = run.font.facts().ascent;
            let transforms: Vec<_> = run
                .glyphs
                .iter()
                .map(|glyph| AffineTransform {
                    values: [1.0, 0.0, 0.0, 1.0, glyph.x, glyph.y + ascent],
                })
                .collect();
            append_glyph_outlines(&mut geometry, &run.font, &run.glyphs, &transforms);
        }
        geometry.update_state_from_contours();
        Self::from_geometry(geometry)
    }

    /// The outlines of the given glyphs, laid along the first subpath of this path that has length. Each glyph has
    /// its origin at the distance of its x position plus the offset, and its baseline along the direction of the
    /// path there. The glyphs stop at the first one whose middle would go past the end.
    pub fn place_glyph_runs_along(&self, runs: &[GlyphRun], offset: f32) -> Path {
        let mut geometry = PathGeometry::new();
        let Some(contour) = measure::ContourMeasure::contours_of(&self.geometry).next() else {
            return Self::from_geometry(geometry);
        };
        let path_length = contour.length();
        for run in runs {
            if run.font.is_invisible() {
                continue;
            }
            let mut transforms = Vec::with_capacity(run.glyphs.len());
            for glyph in &run.glyphs {
                let glyph_distance = offset + glyph.x;
                if glyph_distance + (glyph.glyph_width / 2.0) > path_length {
                    break;
                }
                let (point, tangent) = contour.position_and_tangent(glyph_distance);
                transforms.push(AffineTransform {
                    values: [tangent.x, tangent.y, -tangent.y, tangent.x, point.x, point.y],
                });
            }
            append_glyph_outlines(&mut geometry, &run.font, &run.glyphs[..transforms.len()], &transforms);
            if transforms.len() < run.glyphs.len() {
                break;
            }
        }
        geometry.update_state_from_contours();
        Self::from_geometry(geometry)
    }

    /// The contours of the path, as C++ reads them, valid until the path changes or is dropped.
    pub fn geometry_data(&self) -> ffi::PathGeometryData {
        ffi::PathGeometryData::of(&self.geometry)
    }
}

/// A copy of the path, which has an identity of its own.
impl Clone for Path {
    fn clone(&self) -> Self {
        Self::from_geometry(self.geometry.clone())
    }
}

impl PartialEq for Path {
    fn eq(&self, other: &Self) -> bool {
        std::ptr::eq(self, other) || self.geometry.has_same_contours(&other.geometry)
    }
}

// The contours are compared bit for bit, which is reflexive even for NaN.
impl Eq for Path {}
