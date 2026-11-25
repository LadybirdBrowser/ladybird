/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::used_transform;
use libgfx_rust::{FloatPoint, FloatRect, FloatSize};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiFloatPoint {
    pub x: f32,
    pub y: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiFloatRect {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

/// The glyph cell one code unit of an SVG text content element's character data occupies, in the
/// element's coordinate system. The SVG DOM text methods read these back from the committed box.
/// https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
/// The full glyph cell must have width equal to the horizontal advance and height equal to the EM
/// box for horizontal text.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgTextCharacterCell {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    /// Where the alignment point of the typographic character is along the block axis: The current text position the
    /// character was placed at, which the dominant baseline of its font is aligned to.
    pub alignment_point_y: f32,
    /// Whether this code unit is the first, in document order, of the typographic character whose
    /// cell this is. A later code unit of the same character carries the same cell, unmarked.
    pub starts_typographic_character: bool,
}

/// Appends the cell of each character the edits map the given cells back to: Where an edit turned characters into
/// others, the characters it took share the cell of those it made.
fn append_svg_text_cells_through_edits<'a>(
    cells: &[FfiSvgTextCharacterCell],
    edits: impl IntoIterator<Item = &'a super::rendered_text::RenderedTextEdit>,
    out: &mut Vec<FfiSvgTextCharacterCell>,
) {
    let mut cursor = 0;
    for edit in edits {
        let start = edit.rendered_start_offset;
        let end = start + edit.rendered_length_in_code_units;
        out.extend_from_slice(&cells[cursor..start]);
        cursor = end;
        if edit.dom_length_in_code_units == 0 {
            continue;
        }
        let mut cell = svg_text_cell_for_source_span(cells, start, end);
        out.push(cell);
        cell.starts_typographic_character = false;
        out.extend(std::iter::repeat_n(cell, edit.dom_length_in_code_units - 1));
    }
    out.extend_from_slice(&cells[cursor..]);
}

fn svg_text_cell_for_source_span(
    cells: &[FfiSvgTextCharacterCell],
    start: usize,
    end: usize,
) -> FfiSvgTextCharacterCell {
    if let Some((&first, others)) = cells[start..end].split_first() {
        let mut bounds = FloatRect::new(first.x, first.y, first.width, first.height);
        for other in others {
            bounds = united_even_if_empty(
                Some(bounds),
                FloatRect::new(other.x, other.y, other.width, other.height),
            );
        }
        return FfiSvgTextCharacterCell {
            x: bounds.x,
            y: bounds.y,
            width: bounds.width,
            height: bounds.height,
            ..first
        };
    }
    // A source character removed by casing has an empty cell at its position between the surrounding glyphs.
    let mut cell = cells.get(start).copied().unwrap_or_else(|| {
        let mut last = cells.last().copied().unwrap_or_default();
        last.x += last.width;
        last
    });
    cell.width = 0.0;
    cell.starts_typographic_character = false;
    cell
}

/// What shaping one SVG text run yields: its glyphs, its total advance, the union of its glyph
/// cells, and the glyph cell of every one of its code units.
struct ShapedSvgText {
    runs: Vec<libgfx_rust::path::GlyphRun>,
    advance: f32,
    glyph_cells: SvgCssPixelRect,
    character_cells: Vec<FfiSvgTextCharacterCell>,
}

impl ShapedSvgText {
    fn shift_glyphs(&mut self, shifts: &[FfiFloatPoint]) {
        let mut run_offset = 0;
        for run in &mut self.runs {
            let mut run_length = 0;
            let offsets: Vec<_> = run
                .glyphs
                .placed()
                .map(|glyph| {
                    run_length += glyph.glyph.length_in_code_units();
                    shifts[run_offset + glyph.cluster_start as usize]
                })
                .collect();
            if offsets.iter().all(|offset| offset.x == 0.0 && offset.y == 0.0) {
                run_offset += run_length;
                continue;
            }
            let mut offsets = offsets.into_iter();
            run.glyphs.update_positions(|_, x, y| {
                let offset = offsets.next().expect("one offset per glyph");
                *x += offset.x;
                *y += offset.y;
            });
            run_offset += run_length;
        }
    }
}

/// A path with a closed subpath around each of the given rects.
fn path_of_rects(rects: &[FloatRect]) -> libgfx_rust::path::Path {
    let mut path = libgfx_rust::path::Path::new();
    for rect in rects {
        path.move_to(FloatPoint { x: rect.x, y: rect.y });
        path.line_to(FloatPoint {
            x: rect.right(),
            y: rect.y,
        });
        path.line_to(FloatPoint {
            x: rect.right(),
            y: rect.bottom(),
        });
        path.line_to(FloatPoint {
            x: rect.x,
            y: rect.bottom(),
        });
        path.close();
    }
    path
}

/// Appends the glyph cell of each code unit of one shaped run, in code unit order. A typographic
/// character's cell spans its advance, less the `spacing_after_each_character` the run was shaped
/// with; every code unit of it carries that cell, and the first one is marked as the one that starts
/// it.
/// https://svgwg.org/svg2-draft/text.html#TermTypographicCharacterUnit
/// A unit of a writing system - such as a Latin alphabetic letter (including its diacritics),
/// Hangul syllable, Chinese ideographic character, Myanmar syllable cluster - that is indivisible
/// with respect to a particular typographic operation [...]
/// FIXME: The glyphs are taken to be in code unit order, which holds for the left-to-right text
///        this layout shapes.
fn append_svg_text_character_cells(
    cells: &mut Vec<FfiSvgTextCharacterCell>,
    glyphs: &libgfx_rust::text_layout::GlyphBuffer,
    length_in_code_units: usize,
    run_end_x: f32,
    top: f32,
    height: f32,
    spacing_after_each_character: f32,
) {
    let placed: Vec<_> = glyphs.placed().collect();
    let mut covered = 0usize;
    let mut index = 0usize;
    while index < placed.len() && covered < length_in_code_units {
        let first = placed[index];
        // A glyph that maps to no code unit of its own (a combining mark's, e.g.) belongs to the
        // typographic character before it, so it extends that character's cell. One at the start
        // of the run has no character before it, so it joins the character after it instead.
        let mut next = index + 1;
        let mut code_units = first.glyph.length_in_code_units();
        while next < placed.len() && (placed[next].glyph.length_in_code_units() == 0 || code_units == 0) {
            code_units += placed[next].glyph.length_in_code_units();
            next += 1;
        }
        let end_x = placed.get(next).map_or(run_end_x, |glyph| glyph.x);
        let code_units = code_units.max(1).min(length_in_code_units - covered);
        let cell = FfiSvgTextCharacterCell {
            x: first.x,
            y: top,
            width: end_x - first.x - spacing_after_each_character,
            height,
            alignment_point_y: 0.0,
            starts_typographic_character: true,
        };
        cells.push(cell);
        cells.extend(std::iter::repeat_n(
            FfiSvgTextCharacterCell {
                starts_typographic_character: false,
                ..cell
            },
            code_units - 1,
        ));
        covered += code_units;
        index = next;
    }
    // A code unit no glyph accounts for gets an empty cell at the run's end, so that indices keep
    // lining up with the characters.
    cells.extend(std::iter::repeat_n(
        FfiSvgTextCharacterCell {
            x: run_end_x,
            y: top,
            width: 0.0,
            height,
            alignment_point_y: 0.0,
            starts_typographic_character: false,
        },
        length_in_code_units - covered,
    ));
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiAffineTransform {
    pub a: f32,
    pub b: f32,
    pub c: f32,
    pub d: f32,
    pub e: f32,
    pub f: f32,
}

impl Default for FfiAffineTransform {
    fn default() -> Self {
        Self {
            a: 1.0,
            b: 0.0,
            c: 0.0,
            d: 1.0,
            e: 0.0,
            f: 0.0,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgViewBox {
    pub min_x: f64,
    pub min_y: f64,
    pub width: f64,
    pub height: f64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgNumberPercentage {
    pub value: f32,
    pub is_percentage: bool,
}

/// A `<number> | <length> | <percentage>` attribute value, exactly as the element parsed it. A
/// relative length follows the element's font rather than its attributes, so the pass - not the
/// publication - is what turns one into pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgLengthValue {
    pub value: f64,
    pub kind: u8,
    /// The `CSS::LengthUnit` a length was written in; meaningless for the other kinds.
    pub unit: u8,
}

/// One addressable character's entries in the four positioning lists; an exhausted list has an absent value.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgTextPosition {
    pub x: FfiSvgLengthValue,
    pub y: FfiSvgLengthValue,
    pub dx: FfiSvgLengthValue,
    pub dy: FfiSvgLengthValue,
}

#[derive(Clone, Copy, Default)]
struct SvgTextPosition {
    x: Option<f32>,
    y: Option<f32>,
    dx: Option<f32>,
    dy: Option<f32>,
}

impl SvgTextPosition {
    fn starts_chunk(self) -> bool {
        self.x.is_some() || self.y.is_some()
    }

    fn is_specified(self) -> bool {
        self.starts_chunk() || self.dx.is_some() || self.dy.is_some()
    }
}

pub const SVG_LENGTH_KIND_NONE: u8 = 0;
pub const SVG_LENGTH_KIND_NUMBER: u8 = 1;
pub const SVG_LENGTH_KIND_LENGTH: u8 = 2;
pub const SVG_LENGTH_KIND_PERCENTAGE: u8 = 3;

/// The values an SVG box resolves from its own computed style and the viewport it sits in. They
/// are not element data - an ancestor's viewBox feeds a descendant's percentage basis - so the
/// pass computes them rather than reading them from a publication.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct SvgElementFacts {
    is_document_element: bool,
    document_is_decoded_svg: bool,
    additional_element_transform: FfiAffineTransform,
    visible_stroke_width: f32,
}

/// The SVG attributes one element parses, as the document last published them under
/// its style node. They are element data, not layout output: the document writes them when the
/// style tree names the element and whenever an attribute changes, so a running pass reads the
/// published copy instead of asking.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgAttributeFacts {
    /// The element class tests the pass cannot make from a row's NodeKind: `SVGGraphicsBox` also
    /// stands for `<g>`, `<symbol>` and `<pattern>`, and `<rect>` carries the same computed `x`/`y`
    /// properties that give `<use>` its additional transform.
    pub is_graphics_element: bool,
    pub is_use_element: bool,
    pub is_text_element: bool,
    pub is_fit_to_view_box: bool,
    pub has_active_view_box: bool,
    pub active_view_box: FfiSvgViewBox,
    pub preserve_aspect_ratio_align: u8,
    pub preserve_aspect_ratio_meet_or_slice: u8,
    pub content_units: u8,
    pub pattern_units: u8,
    pub pattern_width: FfiSvgNumberPercentage,
    pub pattern_height: FfiSvgNumberPercentage,
    pub mask_units: u8,
    pub mask_x: FfiSvgNumberPercentage,
    pub mask_y: FfiSvgNumberPercentage,
    pub mask_width: FfiSvgNumberPercentage,
    pub mask_height: FfiSvgNumberPercentage,
    /// Which shape a geometry element draws, and the endpoints a <line> parses. Every other
    /// shape's geometry is computed style, which the row already carries.
    pub geometry_kind: u8,
    pub line_x1: FfiSvgNumberPercentage,
    pub line_y1: FfiSvgNumberPercentage,
    pub line_x2: FfiSvgNumberPercentage,
    pub line_y2: FfiSvgNumberPercentage,
    /// The `pathLength` a geometry element parses, where it parses one: the length its stroke's dashes measure the
    /// path in.
    pub has_path_length: bool,
    pub path_length: f32,
    /// The `textLength` of a `<text>`, where it gives a valid one, and whether its `lengthAdjust` is
    /// `spacingAndGlyphs`.
    pub text_length: FfiSvgLengthValue,
    pub text_length_adjusts_glyphs: bool,
    /// The element this one's `href` names, as the style mirror's id index answers for it: the
    /// URL's decoded fragment, interned as the atom an element's id is indexed under. Zero when
    /// the element names nothing, or names a URL with no fragment.
    pub reference_fragment_atom: u32,
    /// The resources this element's style names, in the same form. These come from `mask`,
    /// `clip-path`, `fill` and `stroke`, so they are republished whenever the element's style
    /// record is replaced rather than when an attribute changes.
    pub mask_reference_atom: u32,
    pub clip_path_reference_atom: u32,
    pub fill_reference_atom: u32,
    pub stroke_reference_atom: u32,
    /// The `startOffset` of a `<textPath>`, against the length of the path it follows.
    pub text_path_start_offset: FfiSvgNumberPercentage,
    /// An `<svg>`'s `width` and `height` where they are a `<length>`, which its natural size is
    /// negotiated from.
    pub natural_width: FfiSvgLengthValue,
    pub natural_height: FfiSvgLengthValue,
    /// The natural aspect ratio an `<svg>`'s active SVG view or viewBox gives it, which the
    /// negotiation falls back to where its width and height do not both give it one.
    pub has_view_box_aspect_ratio: bool,
    pub view_box_aspect_ratio_numerator: CssPixels,
    pub view_box_aspect_ratio_denominator: CssPixels,
}

pub const SVG_GEOMETRY_KIND_NONE: u8 = 0;
pub const SVG_GEOMETRY_KIND_PATH: u8 = 1;
pub const SVG_GEOMETRY_KIND_RECT: u8 = 2;
pub const SVG_GEOMETRY_KIND_CIRCLE: u8 = 3;
pub const SVG_GEOMETRY_KIND_ELLIPSE: u8 = 4;
pub const SVG_GEOMETRY_KIND_LINE: u8 = 5;
pub const SVG_GEOMETRY_KIND_POLYLINE: u8 = 6;
pub const SVG_GEOMETRY_KIND_POLYGON: u8 = 7;

impl FfiSvgNumberPercentage {
    pub(crate) fn resolve_relative_to(self, length: f32) -> f32 {
        if self.is_percentage {
            self.value * length
        } else {
            self.value
        }
    }
}

pub(crate) const PRESERVE_ASPECT_RATIO_NONE: u8 = 0;
pub(crate) const PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN: u8 = 1;
pub(crate) const PRESERVE_ASPECT_RATIO_X_MID_Y_MID: u8 = 5;
pub(crate) const PRESERVE_ASPECT_RATIO_X_MAX_Y_MAX: u8 = 9;
pub(crate) const MEET_OR_SLICE_MEET: u8 = 0;
pub(crate) const MEET_OR_SLICE_SLICE: u8 = 1;
const SVG_UNITS_OBJECT_BOUNDING_BOX: u8 = 0;
const SVG_UNITS_USER_SPACE_ON_USE: u8 = 1;

pub(crate) fn kind_is_svg_graphics_box(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGGraphicsBox
            | NodeKind::SVGGeometryBox
            | NodeKind::SVGImageBox
            | NodeKind::SVGMaskBox
            | NodeKind::SVGTextBox
            | NodeKind::SVGTextPathBox
    )
}

fn kind_is_svg_container_element(kind: NodeKind) -> bool {
    // SVGGraphicsBox is the concrete kind used for <a>, <g>, <switch>,
    // <symbol>, and <use>.
    // FIXME: Include clipPath, defs, marker, and pattern once they are
    // treated as container elements by SVG layout.
    matches!(
        kind,
        NodeKind::SVGGraphicsBox | NodeKind::SVGMaskBox | NodeKind::SVGSVGBox
    )
}

/// The geometry layout found for an SVG element, in the user units of the SVG viewport the element is laid out in.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct SvgGeometry {
    /// The bounding box of the element's fill: its path or glyph cells, or the union of its children's object bounding
    /// boxes, each mapped through the child's local transform.
    pub object_bounding_box: FloatRect,
    /// The bounding box that covers the element's stroke too, which is the element's box.
    pub stroke_bounding_box: FloatRect,
    /// The element's transform about its transform origin, with a <use> element's translation.
    pub local_transform: FfiAffineTransform,
    /// The SVG viewport that the element's own lengths resolve against.
    pub viewport_user_rect: FloatRect,
    /// The SVG viewport that the element lays its contents out in, where it lays out contents of its own.
    pub establishes_viewport: bool,
    pub established_viewport_user_rect: FloatRect,
}

impl SvgGeometry {
    /// What the element's stroke percentages resolve against.
    pub(crate) fn percentage_basis(&self) -> CssPixels {
        crate::svg::stroke_style::percentage_basis(FfiCssPixelSize {
            width: CssPixels::nearest_value_for_f32(self.viewport_user_rect.width),
            height: CssPixels::nearest_value_for_f32(self.viewport_user_rect.height),
        })
    }

    /// The transform reference box that a used `transform-box` value names.
    pub(crate) fn transform_reference_box(&self, transform_box: u8) -> CssPixelRect {
        css_pixel_rect_of(match transform_box {
            transform_box::FILL_BOX => self.object_bounding_box,
            transform_box::VIEW_BOX => {
                FloatRect::new(0.0, 0.0, self.viewport_user_rect.width, self.viewport_user_rect.height)
            }
            _ => self.stroke_bounding_box,
        })
    }
}

pub(crate) fn css_pixel_rect_of(rect: FloatRect) -> CssPixelRect {
    CssPixelRect::new(
        CssPixels::nearest_value_for_f32(rect.x),
        CssPixels::nearest_value_for_f32(rect.y),
        CssPixels::nearest_value_for_f32(rect.width),
        CssPixels::nearest_value_for_f32(rect.height),
    )
}

/// The union of `rect` and `other`, which keeps the extent of an empty rect too.
fn united_even_if_empty(rect: Option<FloatRect>, other: FloatRect) -> FloatRect {
    let Some(rect) = rect else {
        return other;
    };
    let (left, top) = (rect.x.min(other.x), rect.y.min(other.y));
    let (right, bottom) = (rect.right().max(other.right()), rect.bottom().max(other.bottom()));
    FloatRect::new(left, top, right - left, bottom - top)
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct SvgCssPixelRect {
    x: CssPixels,
    y: CssPixels,
    width: CssPixels,
    height: CssPixels,
}

impl SvgCssPixelRect {
    fn inflate(&mut self, width: CssPixels, height: CssPixels) {
        self.x -= width / 2;
        self.width += width;
        self.y -= height / 2;
        self.height += height;
    }

    fn is_empty(&self) -> bool {
        self.width <= CssPixels::default() || self.height <= CssPixels::default()
    }

    fn to_float_rect(self) -> FloatRect {
        FloatRect::new(
            self.x.to_float(),
            self.y.to_float(),
            self.width.to_float(),
            self.height.to_float(),
        )
    }

    /// Grows this rect to cover `other` too. An empty rect covers nothing, so it doesn't take part.
    fn unite(&mut self, other: SvgCssPixelRect) {
        if other.is_empty() {
            return;
        }
        if self.is_empty() {
            *self = other;
            return;
        }
        let right = (self.x + self.width).max(other.x + other.width);
        let bottom = (self.y + self.height).max(other.y + other.height);
        self.x = self.x.min(other.x);
        self.y = self.y.min(other.y);
        self.width = right - self.x;
        self.height = bottom - self.y;
    }
}

impl From<FfiAffineTransform> for libgfx_rust::AffineTransform {
    fn from(transform: FfiAffineTransform) -> Self {
        let FfiAffineTransform { a, b, c, d, e, f } = transform;
        Self::new(a, b, c, d, e, f)
    }
}

impl From<libgfx_rust::AffineTransform> for FfiAffineTransform {
    fn from(transform: libgfx_rust::AffineTransform) -> Self {
        let [a, b, c, d, e, f] = transform.values;
        Self { a, b, c, d, e, f }
    }
}

impl FfiAffineTransform {
    fn is_identity(self) -> bool {
        libgfx_rust::AffineTransform::from(self).is_identity()
    }

    pub(crate) fn translated(self, x: f32, y: f32) -> Self {
        libgfx_rust::AffineTransform::from(self).translated(x, y).into()
    }

    pub(crate) fn scaled(self, x: f32, y: f32) -> Self {
        libgfx_rust::AffineTransform::from(self).scaled(x, y).into()
    }
}

fn float_rect_to_css_pixels(rect: FloatRect) -> SvgCssPixelRect {
    SvgCssPixelRect {
        x: CssPixels::nearest_value_for_f32(rect.x),
        y: CssPixels::nearest_value_for_f32(rect.y),
        width: CssPixels::nearest_value_for_f32(rect.width),
        height: CssPixels::nearest_value_for_f32(rect.height),
    }
}

// https://svgwg.org/svg2-draft/coords.html#PreserveAspectRatioAttribute
fn view_box_transform(
    align: u8,
    meet_or_slice: u8,
    view_box: FfiSvgViewBox,
    viewport_size: (CssPixels, CssPixels),
) -> FfiAffineTransform {
    // NB: The scales pass through f32, as the transform stores them.
    let scale_x = f64::from((viewport_size.0.to_double() / view_box.width) as f32);
    let scale_y = f64::from((viewport_size.1.to_double() / view_box.height) as f32);
    let (scale_x, scale_y, offset) = if align == PRESERVE_ASPECT_RATIO_NONE {
        // Do not force uniform scaling. Scale the graphic content of the given element non-uniformly
        // if necessary such that the element's bounding box exactly matches the SVG viewport rectangle.
        (scale_x, scale_y, (CssPixels::default(), CssPixels::default()))
    } else {
        let scale = match meet_or_slice {
            // meet (the default) - Scale the graphic such that:
            // - aspect ratio is preserved
            // - the entire ‘viewBox’ is visible within the SVG viewport
            // - the ‘viewBox’ is scaled up as much as possible, while still meeting the other criteria
            MEET_OR_SLICE_MEET => scale_x.min(scale_y),
            // slice - Scale the graphic such that:
            // aspect ratio is preserved
            // the entire SVG viewport is covered by the ‘viewBox’
            // the ‘viewBox’ is scaled down as much as possible, while still meeting the other criteria
            MEET_OR_SLICE_SLICE => scale_x.max(scale_y),
            _ => panic!("invalid preserveAspectRatio meet-or-slice value"),
        };
        assert!(
            (PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN..=PRESERVE_ASPECT_RATIO_X_MAX_Y_MAX).contains(&align),
            "invalid preserveAspectRatio alignment value"
        );
        // NB: The alignments count up from xMinYMin through min, mid and max along x, then through min, mid and max
        //     along y. Each aligns the min, the midpoint or the max of the ‘viewBox’ with that of the SVG viewport, so
        //     it offsets the content by none, half or all of the space the scaled ‘viewBox’ leaves.
        let alignment = align - PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN;
        let offset = |alignment_in_axis: u8, viewport_size: CssPixels, view_box_size: f64| {
            let free_space = viewport_size - CssPixels::nearest_value_for(view_box_size * scale);
            match alignment_in_axis {
                0 => CssPixels::default(),
                1 => free_space / 2,
                _ => free_space,
            }
        };
        (
            scale,
            scale,
            (
                offset(alignment % 3, viewport_size.0, view_box.width),
                offset(alignment / 3, viewport_size.1, view_box.height),
            ),
        )
    };
    FfiAffineTransform::default()
        .translated(offset.0.to_float(), offset.1.to_float())
        .scaled(scale_x as f32, scale_y as f32)
        .translated(-view_box.min_x as f32, -view_box.min_y as f32)
}

/// What laying out a <text>, a <tspan>, or an <a> inside one yields.
#[derive(Default)]
struct SvgTextBoxContents {
    /// The glyph outlines of the element's own text nodes.
    path: libgfx_rust::path::Path,
    /// Those outlines split by the child elements between them, where text follows a child element.
    segments: Option<Vec<fragment_tree::SvgTextSegment>>,
    /// The cells the glyphs of the element's own text nodes occupy.
    glyph_cells: SvgCssPixelRect,
    /// The glyph cell of each character in the element's subtree, in document order.
    character_cells: Vec<FfiSvgTextCharacterCell>,
    /// The glyph cell of each character of the element's own text nodes, in document order.
    own_character_cells: Vec<FfiSvgTextCharacterCell>,
    /// The union of the boxes of the element's text content child elements.
    children_bounding_box: SvgCssPixelRect,
}

/// A stretch of text that one text node of a text content element renders, which a text decoration line runs along.
#[derive(Clone, Copy, Debug)]
struct SvgTextRunSpan {
    x: f32,
    advance: f32,
    baseline: f32,
    /// The ascent of the first available font of the element the text is in, and that element's font size.
    ascent: f32,
    font_size: f32,
}

/// The text decoration lines a text content element draws along the text in its subtree.
#[derive(Clone, Copy, Debug, Default)]
struct SvgTextDecorationLines {
    underline: bool,
    overline: bool,
    line_through: bool,
    /// The thickness the element's 'text-decoration-thickness' gives the lines, where it gives one.
    thickness: Option<f32>,
    /// The element's 'text-decoration-style'.
    style: u8,
}

/// Which way a decoration line that's drawn wider than its thickness (a double or a wavy one) grows: away from the
/// text for an underline or an overline, and about its center for a line through the text.
#[derive(Clone, Copy, PartialEq)]
enum SvgDecorationLineGrowth {
    Down,
    Up,
    AboutCenter,
}

impl SvgTextDecorationLines {
    fn thickness_for(&self, span: &SvgTextRunSpan) -> f32 {
        self.thickness.unwrap_or(span.font_size / 10.0).max(1.0)
    }

    /// The underline and the overline along `span`, which paint before the text.
    fn add_lines_under_text(&self, span: &SvgTextRunSpan, path: &mut libgfx_rust::path::Path) {
        let thickness = self.thickness_for(span);
        if self.underline {
            // https://drafts.csswg.org/css-text-decor-4/#text-underline-position-property
            // The user agent may use any algorithm to determine the underline's position; however it must be placed
            // at or under the alphabetic baseline.
            let gap = (thickness / 2.0).ceil().max(1.0);
            self.add_line(
                span,
                span.baseline + gap,
                thickness,
                SvgDecorationLineGrowth::Down,
                path,
            );
        }
        if self.overline {
            let top = span.baseline - span.ascent - thickness;
            self.add_line(span, top, thickness, SvgDecorationLineGrowth::Up, path);
        }
    }

    /// The line through `span`, which paints after the text.
    fn add_lines_over_text(&self, span: &SvgTextRunSpan, path: &mut libgfx_rust::path::Path) {
        if self.line_through {
            let thickness = self.thickness_for(span);
            let top = span.baseline - span.ascent / 3.0 - thickness / 2.0;
            self.add_line(span, top, thickness, SvgDecorationLineGrowth::AboutCenter, path);
        }
    }

    /// Adds the outline of one decoration line along `span`, in the element's 'text-decoration-style', whose solid
    /// form would cover the rows from `top` down to `top + thickness`. The outline is filled and stroked as the
    /// text's glyphs are.
    /// https://drafts.csswg.org/css-text-decor-4/#text-decoration-style-property
    fn add_line(
        &self,
        span: &SvgTextRunSpan,
        top: f32,
        thickness: f32,
        growth: SvgDecorationLineGrowth,
        path: &mut libgfx_rust::path::Path,
    ) {
        let (x, length) = (span.x, span.advance);
        // NB: A dashed, dotted, or wavy line takes a few segments per thickness of its length, so a line too long for
        //     a pattern of reasonable size is drawn solid.
        const MAX_PATTERN_LENGTH_IN_THICKNESSES: f32 = 40_000.0;
        let style =
            if length / thickness > MAX_PATTERN_LENGTH_IN_THICKNESSES && self.style != text_decoration_style::DOUBLE {
                text_decoration_style::SOLID
            } else {
                self.style
            };
        match style {
            text_decoration_style::DOUBLE => {
                // NB: Two lines of the thickness, a thickness apart.
                let (first_top, second_top) = match growth {
                    SvgDecorationLineGrowth::Down => (top, top + 2.0 * thickness),
                    SvgDecorationLineGrowth::Up => (top - 2.0 * thickness, top),
                    SvgDecorationLineGrowth::AboutCenter => (top - thickness, top + thickness),
                };
                path.append_path(&path_of_rects(&[
                    FloatRect::new(x, first_top, length, thickness),
                    FloatRect::new(x, second_top, length, thickness),
                ]));
            }
            text_decoration_style::DASHED => {
                // NB: An odd number of dashes and gaps of equal length, a little over twice the thickness each, so
                //     that the line starts and ends with a dash. Each end reaches half the thickness past the text.
                let total = length + thickness;
                let count = (total / thickness / 4.0).floor() * 2.0 + 1.0;
                let dash = total / count;
                let start = x - thickness / 2.0;
                let rects: Vec<FloatRect> = (0..count as usize)
                    .step_by(2)
                    .map(|index| FloatRect::new(start + index as f32 * dash, top, dash, thickness))
                    .collect();
                path.append_path(&path_of_rects(&rects));
            }
            text_decoration_style::DOTTED => {
                // NB: Round dots as wide as the line is thick, about twice that apart, with one at each end.
                let radius = thickness / 2.0;
                let center_y = top + radius;
                let count = ((length - thickness) / (2.0 * thickness)).floor().max(0.0) as usize + 1;
                let step = if count > 1 {
                    (length - thickness) / (count - 1) as f32
                } else {
                    0.0
                };
                for index in 0..count {
                    let center_x = x + radius + index as f32 * step;
                    path.move_to(FloatPoint {
                        x: center_x - radius,
                        y: center_y,
                    });
                    path.arc_to(
                        FloatPoint {
                            x: center_x + radius,
                            y: center_y,
                        },
                        radius,
                        false,
                        true,
                    );
                    path.arc_to(
                        FloatPoint {
                            x: center_x - radius,
                            y: center_y,
                        },
                        radius,
                        false,
                        true,
                    );
                    path.close();
                }
            }
            text_decoration_style::WAVY => {
                // NB: A zigzag whose crests and troughs are one and a half times the thickness off its center line,
                //     at 45 degrees, stroked at the thickness. It grows away from the text as a double line does.
                let peak = 1.5 * thickness;
                let center_y = top
                    + thickness / 2.0
                    + match growth {
                        SvgDecorationLineGrowth::Down => peak,
                        SvgDecorationLineGrowth::Up => -peak,
                        SvgDecorationLineGrowth::AboutCenter => 0.0,
                    };
                // NB: The zigzag is built from the span's start and moved there afterward, so that each vertex
                //     advances by its full step even where the span starts at a coordinate too large to add it to.
                let mut wave = libgfx_rust::path::Path::new();
                let mut previous = FloatPoint { x: 0.0, y: center_y };
                wave.move_to(previous);
                let mut vertex = FloatPoint {
                    x: peak,
                    y: center_y - peak,
                };
                while vertex.x < length {
                    wave.line_to(vertex);
                    previous = vertex;
                    vertex = FloatPoint {
                        x: vertex.x + 2.0 * peak,
                        y: 2.0 * center_y - vertex.y,
                    };
                }
                let fraction = (length - previous.x) / (vertex.x - previous.x);
                wave.line_to(FloatPoint {
                    x: length,
                    y: previous.y + (vertex.y - previous.y) * fraction,
                });
                let wave = wave.copy_transformed([1.0, 0.0, 0.0, 1.0, x, 0.0]);
                path.append_path(&wave.stroke_to_fill(&libgfx_rust::path::StrokeStyle {
                    thickness,
                    cap_style: libgfx_rust::CapStyle::Butt,
                    join_style: libgfx_rust::JoinStyle::Miter,
                    miter_limit: 4.0,
                    dash_array: Vec::new(),
                    dash_offset: 0.0,
                }));
            }
            _ => path.append_path(&path_of_rects(&[FloatRect::new(x, top, length, thickness)])),
        }
    }
}

/// What a text content element paints in one go: glyph outlines, or the rects of text decoration lines.
enum SvgTextPaintPart {
    Glyphs(Vec<libgfx_rust::path::GlyphRun>),
    /// Glyphs of a color font, which have no outlines.
    ColorGlyphs(Vec<fragment_tree::SvgColorGlyphRun>),
    DecorationLines(libgfx_rust::path::Path),
}

/// The parts a text content element paints, each with the child element it paints after, in paint order.
#[derive(Default)]
struct SvgTextPaintParts {
    parts: Vec<(Node, SvgTextPaintPart)>,
}

impl SvgTextPaintParts {
    fn add_glyphs(&mut self, after_child: Node, runs: Vec<libgfx_rust::path::GlyphRun>) {
        if runs.is_empty() {
            return;
        }
        if let Some((last_after_child, SvgTextPaintPart::Glyphs(last_runs))) = self.parts.last_mut()
            && *last_after_child == after_child
        {
            last_runs.extend(runs);
            return;
        }
        self.parts.push((after_child, SvgTextPaintPart::Glyphs(runs)));
    }

    fn add_color_glyphs(&mut self, after_child: Node, run: fragment_tree::SvgColorGlyphRun) {
        if run.glyphs.is_empty() {
            return;
        }
        if let Some((last_after_child, SvgTextPaintPart::ColorGlyphs(last_runs))) = self.parts.last_mut()
            && *last_after_child == after_child
        {
            last_runs.push(run);
            return;
        }
        self.parts.push((after_child, SvgTextPaintPart::ColorGlyphs(vec![run])));
    }

    fn add_decoration_lines(&mut self, after_child: Node, lines: libgfx_rust::path::Path) {
        if lines.is_empty() {
            return;
        }
        if let Some((last_after_child, SvgTextPaintPart::DecorationLines(last_lines))) = self.parts.last_mut()
            && *last_after_child == after_child
        {
            last_lines.append_path(&lines);
            return;
        }
        self.parts.push((after_child, SvgTextPaintPart::DecorationLines(lines)));
    }

    /// Whether the parts are just the glyphs of text that comes before every child element, which the element's
    /// path paints as it is.
    fn are_one_path(&self) -> bool {
        match self.parts.as_slice() {
            [] => true,
            [(after_child, SvgTextPaintPart::Glyphs(_))] => after_child.is_invalid(),
            _ => false,
        }
    }
}

/// How a text content element adjusts its text to its 'textLength'.
/// https://svgwg.org/svg2-draft/text.html#TextElementLengthAdjustAttribute
#[derive(Clone, Copy, Debug, Default, PartialEq)]
enum SvgTextLengthAdjustment {
    #[default]
    None,
    /// spacing: Indicates that only the advance values are adjusted. The glyphs themselves are not stretched or
    /// compressed.
    /// NB: Holds the space to add after each typographic character.
    Spacing(f32),
    /// spacingAndGlyphs: Indicates that the advance values are adjusted and the glyphs themselves stretched or
    /// compressed in one axis (i.e., a direction parallel to the inline-base direction).
    /// NB: Holds the factor to scale by along that axis.
    SpacingAndGlyphs(f32),
}

impl SvgTextLengthAdjustment {
    fn spacing(self) -> f32 {
        match self {
            Self::Spacing(spacing) => spacing,
            _ => 0.0,
        }
    }

    fn glyph_scale(self) -> f32 {
        match self {
            Self::SpacingAndGlyphs(scale) => scale,
            _ => 1.0,
        }
    }
}

/// The extent excludes trailing 'textLength' spacing; the advance includes it for text that follows the subtree.
#[derive(Clone, Copy, Default)]
struct SvgTextLengthMeasurement {
    start: f32,
    end: f32,
    advance: f32,
    characters: usize,
}

#[derive(Clone, Copy)]
struct SvgResolvedTextLength {
    adjustment: SvgTextLengthAdjustment,
    measurement: SvgTextLengthMeasurement,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct SvgTextChunkMeasurement {
    /// The least and the greatest position along the x axis that the characters of the chunk cover, counted from where
    /// the chunk starts.
    start: f32,
    end: f32,
    anchor: u8,
}

impl Default for SvgTextChunkMeasurement {
    fn default() -> Self {
        Self {
            start: 0.0,
            end: 0.0,
            anchor: text_anchor::START,
        }
    }
}

/// Processes the white space in the character data of a text content block element: `sources` holds the text of each
/// of its text nodes, in document order, with the 'white-space-collapse' value of the element the text node is in.
/// Returns the characters each text node renders.
/// https://svgwg.org/svg2-draft/text.html#WhiteSpace
/// Rendering of white space in SVG 2 is controlled by the white-space property. [...] Values and
/// their meanings are defined in CSS Text Module Level 3.
/// NB: SVG text is laid out as one line, so both phases of CSS Text's white space processing
///     happen here, on the characters themselves, before they are shaped.
fn process_svg_text_white_space(sources: &[(&[u16], u8)]) -> Vec<Vec<u16>> {
    const SPACE: u16 = b' ' as u16;
    const TAB: u16 = b'\t' as u16;
    const LINE_FEED: u16 = b'\n' as u16;
    const CARRIAGE_RETURN: u16 = b'\r' as u16;
    // A segment break is a line feed; the parser has already normalized every other newline into
    // one. A carriage return that script wrote into the DOM never went through that
    // normalization, so it counts as a segment break here too.
    let is_segment_break = |unit: u16| unit == LINE_FEED || unit == CARRIAGE_RETURN;
    let is_white_space = |unit: u16| unit == SPACE || unit == TAB || is_segment_break(unit);

    let mut processed: Vec<Vec<u16>> = sources.iter().map(|(text, _)| Vec::with_capacity(text.len())).collect();
    // The text node that holds the first character of a run of collapsible white space the line hasn't ended in yet.
    let mut collapsible_space_owner: Option<usize> = None;
    let mut at_line_start = true;
    for (index, (text, white_space_collapse)) in sources.iter().enumerate() {
        for &unit in *text {
            if is_white_space(unit) {
                match *white_space_collapse {
                    // https://drafts.csswg.org/css-text-4/#white-space-phase-1
                    // If white-space-collapse is set to collapse or preserve-breaks, white space characters are
                    // considered collapsible and are processed by performing the following steps:
                    // 1. Any sequence of collapsible spaces and tabs immediately preceding or following a
                    //    segment break is removed.
                    // 2. Collapsible segment breaks are transformed for rendering according to the segment
                    //    break transformation rules.
                    // 3. Every collapsible tab is converted to a collapsible space (U+0020).
                    // 4. Any collapsible space immediately following another collapsible space—even one
                    //    outside the boundary of the inline containing that space, provided both spaces are
                    //    within the same inline formatting context—is collapsed to have zero advance width.
                    // https://drafts.csswg.org/css-text-4/#white-space-phase-2
                    // 1. A sequence of collapsible spaces at the beginning of a line is removed.
                    // 3. A sequence of collapsible spaces at the end of a line is removed [...]
                    // Together, those steps turn every run of collapsible white space into a single space, which
                    // the text node the run starts in renders, and drop the runs at either end of the one line SVG
                    // text renders as.
                    // https://drafts.csswg.org/css-text-4/#line-break-transform
                    // Then any remaining segment break is either transformed into a space (U+0020) or removed
                    // depending on the context before and after the break. The rules for this operation are
                    // UA-defined in this level.
                    // We transform every remaining segment break into a space.
                    // AD-HOC: preserve-breaks keeps segment breaks as forced line breaks, but SVG text has no
                    //         line breaking here, so its segment breaks collapse like collapse's do.
                    white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS => {
                        if !at_line_start && collapsible_space_owner.is_none() {
                            collapsible_space_owner = Some(index);
                        }
                        continue;
                    }
                    // https://drafts.csswg.org/css-text-4/#white-space-phase-1
                    // If white-space-collapse is set to preserve-spaces, each tab and segment break is
                    // converted to a space.
                    // https://drafts.csswg.org/css-text-4/#valdef-white-space-collapse-preserve
                    // preserve: This value prevents user agents from collapsing sequences of white space.
                    // Segment breaks such as line feeds are preserved as forced line breaks.
                    // AD-HOC: SVG text has no line breaking here, so preserve and break-spaces render each
                    //         segment break as a space too, rather than as a forced line break. Likewise a
                    //         preserved tab renders as a space rather than as a shift to the next tab stop.
                    _ => {}
                }
            }
            if let Some(owner) = collapsible_space_owner.take() {
                processed[owner].push(SPACE);
            }
            at_line_start = false;
            processed[index].push(if is_white_space(unit) { SPACE } else { unit });
        }
    }
    processed
}

fn code_point_length_at(text: &[u16], offset: usize) -> usize {
    let is_leading_surrogate = (0xd800..0xdc00).contains(&text[offset]);
    let has_trailing_surrogate = offset + 1 < text.len() && (0xdc00..0xe000).contains(&text[offset + 1]);
    if is_leading_surrogate && has_trailing_surrogate {
        2
    } else {
        1
    }
}

fn code_point_at(text: &[u16], offset: usize) -> u32 {
    if code_point_length_at(text, offset) == 2 {
        return 0x10000 + ((u32::from(text[offset]) - 0xd800) << 10) + (u32::from(text[offset + 1]) - 0xdc00);
    }
    u32::from(text[offset])
}

pub(super) struct SvgFormattingContext<'pass> {
    run: FormattingContextRun<'pass>,
    available_space: Option<AvailableSpace>,
    quirks_mode_percentage_basis_block_size: Option<CssPixels>,
    viewport_width: CssPixels,
    viewport_height: CssPixels,
    viewport_user_rect: FloatRect,
    /// Whether this context's box is an <svg> viewport whose view box and size are the ones it was
    /// last committed with, so that a shape below it resolves against the viewport it did then.
    viewport_is_committed_viewport: bool,
    /// Whether percentage block insets of the children of the run's root have a definite basis. The context of the root
    /// sets it once the root is definite. A nested context takes it from the context that creates it.
    percentage_block_insets_of_run_root_children_have_definite_basis: bool,
    current_text_position: FfiFloatPoint,
    /// How far 'text-anchor' moved the chunk in user units, independent of its glyph scale.
    text_anchor_shift: f32,
    /// Each explicitly sized text content element's adjustment and the extent and advance of its adjusted subtree.
    text_length_adjustments: RefCell<std::collections::HashMap<Node, SvgResolvedTextLength>>,
    text_positions: RefCell<std::collections::HashMap<Node, Vec<SvgTextPosition>>>,
    text_chunks: RefCell<std::collections::HashMap<(Node, usize), SvgTextChunkMeasurement>>,
    first_text_runs: RefCell<std::collections::HashMap<Node, Option<Node>>>,
    /// The characters each text node of the text content elements laid out so far renders.
    processed_svg_text: RefCell<std::collections::HashMap<Node, std::sync::Arc<[u16]>>>,
    /// Where text-transform changed the characters of a text node in `processed_svg_text`, how its characters map to
    /// those it had before the transform.
    svg_text_transform_edits: RefCell<std::collections::HashMap<Node, Vec<super::rendered_text::RenderedTextEdit>>>,
    /// The glyph outlines, laid along the path, and the character cells of each text content element inside the
    /// <textPath> laid out last, which that element paints in its own style.
    text_on_path_glyphs:
        RefCell<std::collections::HashMap<Node, (libgfx_rust::path::Path, Vec<FfiSvgTextCharacterCell>)>>,
    /// The glyph cells of the text content element laid out last: those of its whole subtree, in document order.
    last_text_subtree_cells: Option<std::sync::Arc<Vec<FfiSvgTextCharacterCell>>>,
    /// The stretches of text in the subtree of the <text>, <tspan>, or <a> laid out last, in document order.
    last_text_subtree_spans: Vec<SvgTextRunSpan>,
    /// What the run's root, where this is the context of the root, hands the root's fragment.
    root_payload: RefCell<fragment_tree::PendingPayload>,
}

impl<'pass> formatting_context::FormattingContextAlgorithm<'pass> for SvgFormattingContext<'pass> {
    fn lay_out(
        &mut self,
        run: &FormattingContextRun<'pass>,
        input: formatting_context::AlgorithmInput<'_>,
    ) -> formatting_context::AlgorithmOutput {
        let root = Cell::new(input.root_cells);
        self.lay_out_contents(run, input.body_input, &root);
        formatting_context::AlgorithmOutput::of_root(formatting_context::ChildLayoutResult::default(), root.get())
            .with_root_payload(self.root_payload.take())
    }
}

impl<'pass> SvgFormattingContext<'pass> {
    pub(super) fn new(run: &FormattingContextRun<'pass>) -> Self {
        Self::new_nested(run, run.box_, false)
    }

    fn new_nested(
        run: &FormattingContextRun<'pass>,
        box_: Node,
        percentage_block_insets_of_run_root_children_have_definite_basis: bool,
    ) -> Self {
        Self {
            run: FormattingContextRun { box_, ..run.clone() },
            available_space: None,
            quirks_mode_percentage_basis_block_size: None,
            viewport_width: CssPixels::default(),
            viewport_height: CssPixels::default(),
            viewport_user_rect: FloatRect::default(),
            viewport_is_committed_viewport: false,
            percentage_block_insets_of_run_root_children_have_definite_basis,
            current_text_position: FfiFloatPoint::default(),
            text_anchor_shift: 0.0,
            text_length_adjustments: RefCell::new(std::collections::HashMap::new()),
            text_positions: RefCell::new(std::collections::HashMap::new()),
            text_chunks: RefCell::new(std::collections::HashMap::new()),
            first_text_runs: RefCell::new(std::collections::HashMap::new()),
            processed_svg_text: RefCell::new(std::collections::HashMap::new()),
            svg_text_transform_edits: RefCell::new(std::collections::HashMap::new()),
            text_on_path_glyphs: RefCell::new(std::collections::HashMap::new()),
            last_text_subtree_cells: None,
            last_text_subtree_spans: Vec::new(),
            root_payload: RefCell::new(fragment_tree::PendingPayload::default()),
        }
    }

    /// Hands `node` what its layout produced besides its geometry, where the run builds fragments: the run's root through
    /// the output of this context, and any other box through the run's fragment builder.
    fn update_svg_payload_of(&self, node: Node, update: impl FnOnce(&mut fragment_tree::SvgPayload)) {
        if node != self.run.state.root() {
            self.run.update_svg_payload_of(node, update);
            return;
        }
        debug_assert_eq!(
            node, self.run.box_,
            "only the context of a run's root hands the root its payload"
        );
        if self.run.fragments.is_some() {
            update(self.root_payload.borrow_mut().svg_mut());
        }
    }

    // The input a nested viewport or resource context is laid out with: it inherits this context's
    // available space and quirks-mode percentage basis, and participates as an item.
    fn nested_layout_input(&self) -> LayoutInput {
        LayoutInput::new(
            self.available_space.unwrap(),
            ContainingBlockConstraints {
                quirks_mode_percentage_basis_block_size: self.quirks_mode_percentage_basis_block_size,
                ..Default::default()
            },
            ParticipationInParentFormattingContext::Item,
        )
    }

    fn first_child(&self, node: Node) -> Node {
        self.run.pass.node_data(node).first_child.get()
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.run.pass.node_data(node).next_sibling.get()
    }

    fn parent(&self, node: Node) -> Node {
        self.run.pass.node_data(node).parent.get()
    }

    fn node_kind(&self, node: Node) -> NodeKind {
        self.run.pass.node_data(node).kind.get()
    }
}

/// The viewport a shape's geometry resolves its lengths against. Layout and the DOM's geometry
/// queries both build a shape's path through it, so the two cannot disagree on a shape.
#[derive(Clone, Copy)]
struct SvgShapeGeometry {
    viewport_width: CssPixels,
    viewport_height: CssPixels,
}

impl SvgShapeGeometry {
    // https://svgwg.org/svg2-draft/shapes.html#RectElement
    // The used values for rx and ry are determined from the computed values by following these
    // steps in order.
    fn svg_rect_corner_radii(style: StyleValues<'_>, used_width: f32, used_height: f32) -> (f32, f32) {
        let svg_reset = style.svg_reset();
        let computed_rx = &svg_reset.rx;
        let computed_ry = &svg_reset.ry;

        // 1. If both rx and ry have a computed value of auto, then the used value of both is 0.
        if computed_rx.is_auto() && computed_ry.is_auto() {
            return (0.0, 0.0);
        }

        // 2. Otherwise, convert specified values to absolute values, resolving an rx percentage
        //    against the used width and an ry percentage against the used height, and letting an
        //    auto radius take the other one.
        let absolute_rx = computed_rx
            .to_px(CssPixels::nearest_value_for_f32(used_width))
            .to_float();
        let absolute_ry = computed_ry
            .to_px(CssPixels::nearest_value_for_f32(used_height))
            .to_float();
        let (mut used_rx, mut used_ry) = match (computed_rx.is_auto(), computed_ry.is_auto()) {
            (false, true) => (absolute_rx, absolute_rx),
            (true, false) => (absolute_ry, absolute_ry),
            _ => (absolute_rx, absolute_ry),
        };

        // 3. Finally, apply clamping: neither radius may exceed half of the used size on its axis.
        if used_rx > used_width / 2.0 {
            used_rx = used_width / 2.0;
        }
        if used_ry > used_height / 2.0 {
            used_ry = used_height / 2.0;
        }
        (used_rx, used_ry)
    }

    fn svg_rect_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::Path {
        let mut path = libgfx_rust::path::Path::new();
        let computed_width = style.width();
        let computed_height = style.height();
        // FIXME: to_px rounds prematurely here - we shouldn't round to fixed point CSSPixels until
        //        converting to CSS pixel space from SVG user space - this likely extends to other
        //        SVG geometry elements as well.
        let width = if computed_width.is_length_percentage() {
            computed_width
                .length_percentage()
                .to_px(self.viewport_width)
                .to_double()
        } else {
            0.0
        };
        let height = if computed_height.is_length_percentage() {
            computed_height
                .length_percentage()
                .to_px(self.viewport_height)
                .to_double()
        } else {
            0.0
        };
        let x = style.x().to_px(self.viewport_width).to_double();
        let y = style.y().to_px(self.viewport_height).to_double();

        // Non-positive dimensions disable rendering. In particular, a negative width or height is
        // an invalid geometry value rather than a rectangle extending in the opposite direction.
        if width <= 0.0 || height <= 0.0 {
            return path;
        }

        let (rx, ry) = Self::svg_rect_corner_radii(style, width as f32, height as f32);
        let radii = FloatSize { width: rx, height: ry };
        let corner = (rx > 0.0) && (ry > 0.0);
        let x_axis_rotation = 0.0;
        let large_arc_flag = false;
        let sweep_flag = true;

        // 1. perform an absolute moveto operation to location (x+rx,y);
        path.move_to(FloatPoint {
            x: (x + rx as f64) as f32,
            y: y as f32,
        });
        // 2. perform an absolute horizontal lineto with parameter x+width-rx;
        path.line_to(FloatPoint {
            x: (x + width - rx as f64) as f32,
            y: y as f32,
        });
        // 3. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    to coordinate (x+width,y+ry), where rx and ry are used as the equivalent parameters
        //    to the elliptical arc command, the x-axis-rotation and large-arc-flag are set to
        //    zero, the sweep-flag is set to one;
        if corner {
            path.elliptical_arc_to(
                FloatPoint {
                    x: (x + width) as f32,
                    y: (y + ry as f64) as f32,
                },
                radii,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        // 4. perform an absolute vertical lineto parameter y+height-ry;
        path.line_to(FloatPoint {
            x: (x + width) as f32,
            y: (y + height - ry as f64) as f32,
        });
        // 5. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    to coordinate (x+width-rx,y+height), using the same parameters as previously;
        if corner {
            path.elliptical_arc_to(
                FloatPoint {
                    x: (x + width - rx as f64) as f32,
                    y: (y + height) as f32,
                },
                radii,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        // 6. perform an absolute horizontal lineto parameter x+rx;
        path.line_to(FloatPoint {
            x: (x + rx as f64) as f32,
            y: (y + height) as f32,
        });
        // 7. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    to coordinate (x,y+height-ry), using the same parameters as previously;
        if corner {
            path.elliptical_arc_to(
                FloatPoint {
                    x: x as f32,
                    y: (y + height - ry as f64) as f32,
                },
                radii,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        // 8. perform an absolute vertical lineto parameter y+ry
        path.line_to(FloatPoint {
            x: x as f32,
            y: (y + ry as f64) as f32,
        });
        // 9. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    with a segment-completing close path operation, using the same parameters as
        //    previously.
        if corner {
            path.elliptical_arc_to(
                FloatPoint {
                    x: (x + rx as f64) as f32,
                    y: y as f32,
                },
                radii,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        path.close();
        path
    }

    fn svg_circle_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::Path {
        let mut path = libgfx_rust::path::Path::new();
        let svg_reset = style.svg_reset();
        let to_px = |handle: &ComputedStyleValueHandle, basis: CssPixels| {
            handle
                .length_percentage()
                .map_or(CssPixels::default(), |value| value.to_px(basis))
        };
        let cx = to_px(&svg_reset.cx, self.viewport_width).to_float();
        let cy = to_px(&svg_reset.cy, self.viewport_height).to_float();
        // Percentages refer to the normalized diagonal of the current SVG viewport
        // (see Units: https://svgwg.org/svg2-draft/coords.html#Units)
        let normalized_diagonal = crate::svg::stroke_style::percentage_basis(FfiCssPixelSize {
            width: self.viewport_width,
            height: self.viewport_height,
        });
        let r = to_px(&svg_reset.r, normalized_diagonal).to_float();

        // A zero radius disables rendering.
        if r == 0.0 {
            return path;
        }
        let large_arc = false;
        let sweep = true;
        // 1. A move-to command to the point cx+r,cy;
        path.move_to(FloatPoint { x: cx + r, y: cy });
        // 2. arc to cx,cy+r;
        path.arc_to(FloatPoint { x: cx, y: cy + r }, r, large_arc, sweep);
        // 3. arc to cx-r,cy;
        path.arc_to(FloatPoint { x: cx - r, y: cy }, r, large_arc, sweep);
        // 4. arc to cx,cy-r;
        path.arc_to(FloatPoint { x: cx, y: cy - r }, r, large_arc, sweep);
        // 5. arc with a segment-completing close path operation.
        path.arc_to(FloatPoint { x: cx + r, y: cy }, r, large_arc, sweep);
        path
    }

    fn svg_ellipse_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::Path {
        let mut path = libgfx_rust::path::Path::new();
        let svg_reset = style.svg_reset();
        let computed_rx = &svg_reset.rx;
        let computed_ry = &svg_reset.ry;
        let mut rx = computed_rx.to_px(self.viewport_width).to_float();
        let mut ry = computed_ry.to_px(self.viewport_height).to_float();

        // https://svgwg.org/svg2-draft/geometry.html#RxProperty
        // When the computed value of 'rx' is auto, the used radius is equal to the absolute length
        // used for ry, creating a circular arc. If both 'rx' and 'ry' have a computed value of
        // auto, the used value is 0. The same holds the other way around.
        if computed_rx.is_auto() {
            rx = computed_ry.to_px(self.viewport_height).to_float();
        }
        if computed_ry.is_auto() {
            ry = computed_rx.to_px(self.viewport_width).to_float();
        }

        let to_px = |handle: &ComputedStyleValueHandle, basis: CssPixels| {
            handle
                .length_percentage()
                .map_or(CssPixels::default(), |value| value.to_px(basis))
        };
        let cx = to_px(&svg_reset.cx, self.viewport_width).to_float();
        let cy = to_px(&svg_reset.cy, self.viewport_height).to_float();

        // A negative radius is invalid. If only one radius is invalid, SVG uses the other valid
        // radius for both axes; if both are invalid, rendering is disabled. A computed value of
        // zero for either dimension also disables rendering.
        if rx < 0.0 && ry >= 0.0 {
            rx = ry;
        } else if ry < 0.0 && rx >= 0.0 {
            ry = rx;
        }
        if rx <= 0.0 || ry <= 0.0 {
            return path;
        }
        let radii = FloatSize { width: rx, height: ry };

        let x_axis_rotation = 0.0;
        let large_arc = false;
        // NB: Spec says sweep should be false, but it's wrong. https://github.com/w3c/svgwg/issues/765
        let sweep = true;
        // 1. A move-to command to the point cx+rx,cy;
        path.move_to(FloatPoint { x: cx + rx, y: cy });
        // 2. arc to cx,cy+ry;
        path.elliptical_arc_to(
            FloatPoint { x: cx, y: cy + ry },
            radii,
            x_axis_rotation,
            large_arc,
            sweep,
        );
        // 3. arc to cx-rx,cy;
        path.elliptical_arc_to(
            FloatPoint { x: cx - rx, y: cy },
            radii,
            x_axis_rotation,
            large_arc,
            sweep,
        );
        // 4. arc to cx,cy-ry;
        path.elliptical_arc_to(
            FloatPoint { x: cx, y: cy - ry },
            radii,
            x_axis_rotation,
            large_arc,
            sweep,
        );
        // 5. arc with a segment-completing close path operation.
        path.elliptical_arc_to(
            FloatPoint { x: cx + rx, y: cy },
            radii,
            x_axis_rotation,
            large_arc,
            sweep,
        );
        path
    }

    fn svg_line_path(&self, attributes: FfiSvgAttributeFacts) -> libgfx_rust::path::Path {
        let mut path = libgfx_rust::path::Path::new();
        let viewport_width = self.viewport_width.to_float();
        let viewport_height = self.viewport_height.to_float();
        // 1. perform an absolute moveto operation to absolute location (x1,y1)
        path.move_to(FloatPoint {
            x: attributes.line_x1.resolve_relative_to(viewport_width),
            y: attributes.line_y1.resolve_relative_to(viewport_height),
        });
        // 2. perform an absolute lineto operation to absolute location (x2,y2)
        path.line_to(FloatPoint {
            x: attributes.line_x2.resolve_relative_to(viewport_width),
            y: attributes.line_y2.resolve_relative_to(viewport_height),
        });
        path
    }

    fn svg_poly_path(points: Option<&[FfiFloatPoint]>, close: bool) -> libgfx_rust::path::Path {
        let mut path = libgfx_rust::path::Path::new();
        let Some(points) = points.filter(|points| !points.is_empty()) else {
            return path;
        };
        // 1. perform an absolute moveto operation to the first coordinate pair in the list of points
        path.move_to(FloatPoint {
            x: points[0].x,
            y: points[0].y,
        });
        // 2. for each subsequent coordinate pair, perform an absolute lineto operation to that
        //    coordinate pair.
        for point in &points[1..] {
            path.line_to(FloatPoint { x: point.x, y: point.y });
        }
        // 3. a polygon performs a closepath command; a polyline does not.
        if close {
            path.close();
        }
        path
    }

    /// The geometry a shape element draws, in the user units of this viewport, from the shape its
    /// attributes name and its computed style.
    fn path(
        &self,
        attributes: FfiSvgAttributeFacts,
        style: StyleValues<'_>,
        points: Option<&[FfiFloatPoint]>,
    ) -> libgfx_rust::path::Path {
        match attributes.geometry_kind {
            SVG_GEOMETRY_KIND_PATH => self.svg_path_element_path(style),
            SVG_GEOMETRY_KIND_RECT => self.svg_rect_path(style),
            SVG_GEOMETRY_KIND_CIRCLE => self.svg_circle_path(style),
            SVG_GEOMETRY_KIND_ELLIPSE => self.svg_ellipse_path(style),
            SVG_GEOMETRY_KIND_LINE => self.svg_line_path(attributes),
            SVG_GEOMETRY_KIND_POLYLINE => Self::svg_poly_path(points, false),
            SVG_GEOMETRY_KIND_POLYGON => Self::svg_poly_path(points, true),
            _ => libgfx_rust::path::Path::new(),
        }
    }

    /// The `d` property's path. It is already a shared, immutable blob on the computed style, so
    /// the pass only has to realize it as geometry and stamp the shape's fill rule onto it.
    fn svg_path_element_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::Path {
        let Some(shape) = style
            .svg_reset()
            .d
            .style_value()
            .and_then(crate::css::style_value::StyleValueData::basic_shape)
        else {
            return libgfx_rust::path::Path::new();
        };
        // The C++ BasicShape variant `d` holds is a path().
        const BASIC_SHAPE_PATH: u8 = 6;
        debug_assert_eq!(shape.kind, BASIC_SHAPE_PATH, "the d property holds a path()");
        let mut path = shape.path.to_gfx_path();
        path.set_fill_rule(libgfx_rust::WindingRule::from_raw(i32::from(shape.fill_rule)));
        path
    }
}

/// A shape element whose geometry the DOM measures: the shape `geometry` names, the computed style in `payloads`, the
/// points of a polyline or polygon, and the size of the viewport the element sits in. Only the `geometry_kind` and
/// `line_*` fields of `geometry` are read.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSvgGeometryElementShape {
    pub geometry: FfiSvgAttributeFacts,
    pub style: *const c_void,
    pub points: *const FfiFloatPoint,
    pub point_count: usize,
    pub viewport_size: FfiCssPixelSize,
}

impl FfiSvgGeometryElementShape {
    /// What `measure` answers of the geometry the element draws, in the user units of its viewport, built by the same
    /// shape builders layout uses, and of the element's style.
    ///
    /// # Safety
    /// `style` must point at the groups of a live style record, and `points` to `point_count` points.
    unsafe fn measure<R>(&self, measure: impl FnOnce(&libgfx_rust::path::Path, StyleValues<'_>) -> R) -> R {
        // SAFETY: Guaranteed by the caller.
        let style = unsafe { crate::layout::node_data::LayoutStyleRef::from_raw(self.style) }
            .expect("an SVG geometry element's style record is not null");
        // SAFETY: Guaranteed by the caller.
        let points = unsafe { ak::slice_from_raw_parts(self.points, self.point_count) };
        let shape_geometry = SvgShapeGeometry {
            viewport_width: self.viewport_size.width,
            viewport_height: self.viewport_size.height,
        };
        // SAFETY: Guaranteed by the caller.
        let style = StyleValues::new(unsafe { style.view() });
        measure(&shape_geometry.path(self.geometry, style, Some(points)), style)
    }
}

/// A vertex of polylines through a path, at `distance` along it, where the path heads in `direction`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiPathVertex {
    pub point: FfiFloatPoint,
    pub distance: f32,
    pub direction: FfiFloatPoint,
}

/// Calls `push` once with the vertices of polylines through `path`, as close to its curves as its length is measured.
///
/// # Safety
/// `context` must be valid for `push` for the duration of this call.
unsafe fn push_path_vertices(
    path: &libgfx_rust::path::Path,
    context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, *const FfiPathVertex, usize),
) {
    let vertices: Vec<FfiPathVertex> = path
        .vertices()
        .into_iter()
        .map(|vertex| FfiPathVertex {
            point: FfiFloatPoint {
                x: vertex.point.x,
                y: vertex.point.y,
            },
            distance: vertex.distance,
            direction: FfiFloatPoint {
                x: vertex.direction.x,
                y: vertex.direction.y,
            },
        })
        .collect();
    // SAFETY: Guaranteed by the caller.
    unsafe { push(context, vertices.as_ptr(), vertices.len()) };
}

/// Whether the point is inside the geometry of a shape element, as the winding rule its `fill-rule` names counts it.
/// Open subpaths are closed for the test, and points on the path count as inside.
///
/// # Safety
/// As for [`FfiSvgGeometryElementShape::measure`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn svg_geometry_element_fill_contains(
    shape: &FfiSvgGeometryElementShape,
    x: f32,
    y: f32,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        shape.measure(|path, style| {
            let winding_rule = if style.inherited_svg().fill_rule == crate::css::css_enums::fill_rule::EVENODD {
                libgfx_rust::WindingRule::EvenOdd
            } else {
                libgfx_rust::WindingRule::Nonzero
            };
            path.contains(x, y, winding_rule)
        })
    }
}

/// The length of the geometry of a shape element.
///
/// # Safety
/// As for [`FfiSvgGeometryElementShape::measure`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn svg_geometry_element_total_length(shape: &FfiSvgGeometryElementShape) -> f32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { shape.measure(|path, _| path.length()) }
}

/// Writes the point at `distance` along the geometry of a shape element to `point`, with the distance clamped to the
/// ends of the geometry. Answers false, writing nothing, for a geometry without segments.
///
/// # Safety
/// As for [`FfiSvgGeometryElementShape::measure`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn svg_geometry_element_point_at_length(
    shape: &FfiSvgGeometryElementShape,
    distance: f32,
    point: &mut FfiFloatPoint,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let Some([x, y, _, _]) = (unsafe { shape.measure(|path, _| path.point_at_length(distance)) }) else {
        return false;
    };
    *point = FfiFloatPoint { x, y };
    true
}

/// Calls `push` once with the vertices of polylines through the geometry of a shape element (see
/// [`push_path_vertices`]).
///
/// # Safety
/// As for [`FfiSvgGeometryElementShape::measure`]. `context` must be valid for `push` for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn svg_geometry_element_vertices(
    shape: &FfiSvgGeometryElementShape,
    context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, *const FfiPathVertex, usize),
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { shape.measure(|path, _| push_path_vertices(path, context, push)) }
}

/// Calls `push` once with the vertices of polylines through the path that `path` draws (see [`push_path_vertices`]).
///
/// # Safety
/// `context` must be valid for `push` for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn svg_path_data_vertices(
    path: &crate::svg::ParsedPath,
    context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, *const FfiPathVertex, usize),
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { push_path_vertices(&path.to_gfx_path(), context, push) }
}

/// Whether the point is inside the area that stroking the geometry of a shape element covers, with the stroke drawn
/// from the element's computed style. Distances along the path are scaled by the dash scale. The host scale is the
/// scale from the element's user space to its host coordinate space, in which a non-scaling stroke is drawn.
///
/// # Safety
/// As for [`FfiSvgGeometryElementShape::measure`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn svg_geometry_element_stroke_contains(
    shape: &FfiSvgGeometryElementShape,
    dash_scale: f32,
    host_scale: f32,
    x: f32,
    y: f32,
) -> bool {
    let viewport_size = shape.viewport_size;
    // SAFETY: Guaranteed by the caller.
    unsafe {
        shape.measure(|path, style| {
            let stroke = crate::svg::stroke_style::svg_stroke_style_for_percentage_basis(
                &style,
                crate::svg::stroke_style::percentage_basis(viewport_size),
            );
            // https://svgwg.org/svg2-draft/painting.html#PaintingVectorEffects
            // With the non-scaling-stroke vector effect, stroke outline shall be calculated in the "host" coordinate
            // space instead of user coordinate system.
            let stroke_scale = if stroke.non_scaling_stroke && host_scale > 0.0 {
                1.0 / host_scale
            } else {
                1.0
            };
            let stroke_style = libgfx_rust::path::StrokeStyle {
                thickness: stroke.width * stroke_scale,
                cap_style: stroke.cap_style,
                join_style: stroke.join_style,
                miter_limit: stroke.miter_limit,
                // An infinite dash scale makes every dash infinitely long, so the stroke draws solid.
                dash_array: if dash_scale.is_finite() {
                    stroke
                        .dash_array
                        .iter()
                        .map(|dash| dash * dash_scale * stroke_scale)
                        .collect()
                } else {
                    Vec::new()
                },
                dash_offset: if dash_scale.is_finite() {
                    stroke.dash_offset * dash_scale * stroke_scale
                } else {
                    0.0
                },
            };
            if stroke_style.thickness <= 0.0 {
                return false;
            }
            path.stroke_to_fill(&stroke_style)
                .contains(x, y, libgfx_rust::WindingRule::Nonzero)
        })
    }
}

impl<'pass> SvgFormattingContext<'pass> {
    fn shape_geometry(&self) -> SvgShapeGeometry {
        SvgShapeGeometry {
            viewport_width: self.viewport_width,
            viewport_height: self.viewport_height,
        }
    }

    /// The geometry a shape element draws, in the user units of the viewport it sits in.
    fn svg_geometry_path(&self, node: Node) -> libgfx_rust::path::Path {
        let points = self.run.pass.arena().svg_points(node);
        self.shape_geometry()
            .path(self.svg_attributes(node), self.style(node), points.as_deref())
    }

    /// The pixel value one of the text positioning attributes resolves to. A relative length is
    /// absolutized against the element's own style, exactly as the attribute's parse used to be.
    fn resolve_svg_text_length(&self, node: Node, value: FfiSvgLengthValue, reference: CssPixels) -> f32 {
        match value.kind {
            SVG_LENGTH_KIND_NUMBER => value.value as f32,
            SVG_LENGTH_KIND_PERCENTAGE => {
                CssPixels::truncated_value_for(reference.to_double() * value.value / 100.0).to_float()
            }
            SVG_LENGTH_KIND_LENGTH => resolve_svg_length(&self.run.pass, node, value).to_float(),
            _ => 0.0,
        }
    }

    /// Calls `visit` with each text node below `root` that renders, in document order: The text nodes of `root`, and
    /// of the text content elements below it. Text in any other element inside a <text> isn't rendered.
    fn for_each_svg_text_node(&self, root: Node, visit: impl FnMut(Node)) {
        self.for_each_svg_text_node_in(root, true, visit);
    }

    /// As for_each_svg_text_node(), but leaves out the text on a path below `root` where `include_text_on_paths` isn't
    /// set.
    fn for_each_svg_text_node_in(&self, root: Node, include_text_on_paths: bool, mut visit: impl FnMut(Node)) {
        let mut current = root;
        loop {
            let kind = self.node_kind(current);
            if node_facts::kind_is_text(kind) {
                visit(current);
            }
            let descend = kind == NodeKind::SVGTextBox
                || (kind == NodeKind::SVGTextPathBox && (include_text_on_paths || current == root));
            if descend {
                let child = self.first_child(current);
                if !child.is_invalid() {
                    current = child;
                    continue;
                }
            }
            loop {
                if current == root {
                    return;
                }
                let sibling = self.next_sibling(current);
                if !sibling.is_invalid() {
                    current = sibling;
                    break;
                }
                current = self.parent(current);
            }
        }
    }

    /// The characters the given text node of a text content element renders: its character data, with its white space
    /// processed as the 'white-space-collapse' of the element it's in directs. White space collapses across element
    /// boundaries, so the text of the whole text content block element is processed in one go, in document order.
    /// SVG shapes the element's own characters, not the text a line box would lay out, so this reads the source text
    /// the row kept beside its rendering.
    fn processed_svg_text(&self, text_node: Node) -> std::sync::Arc<[u16]> {
        if let Some(text) = self.processed_svg_text.borrow().get(&text_node) {
            return text.clone();
        }
        let mut root = self.parent(text_node);
        loop {
            let parent = self.parent(root);
            if parent.is_invalid() || !matches!(self.node_kind(parent), NodeKind::SVGTextBox | NodeKind::SVGTextPathBox)
            {
                break;
            }
            root = parent;
        }
        let mut text_nodes = Vec::new();
        self.for_each_svg_text_node(root, |node| text_nodes.push(node));
        let sources: Vec<(Vec<u16>, u8)> = text_nodes
            .iter()
            .map(|&node| {
                let mut source = Vec::new();
                self.append_svg_source_text(node, &mut source);
                (source, self.style(self.parent(node)).white_space_collapse())
            })
            .collect();
        let borrowed_sources: Vec<(&[u16], u8)> = sources
            .iter()
            .map(|(source, white_space_collapse)| (source.as_slice(), *white_space_collapse))
            .collect();
        let mut processed_text = self.processed_svg_text.borrow_mut();
        let mut element_offsets = std::collections::HashMap::<Node, usize>::new();
        for (node, text) in text_nodes.iter().zip(process_svg_text_white_space(&borrowed_sources)) {
            let mut positions = vec![SvgTextPosition::default(); text.len()];
            let mut element = self.parent(*node);
            // https://svgwg.org/svg2-draft/text.html#TextLayoutAlgorithm
            // Adjustments specified in descendant nodes, however, override adjustments from ancestor nodes.
            // NB: Walk outward so each coordinate takes the innermost specified value. Every code point consumes an
            //     entry, including a combining mark that shares its base character's position. The second code unit
            //     of a surrogate pair is part of the code point before it, and takes no entry of its own.
            let starts_code_point: Vec<bool> = (0..text.len())
                .map(|index| {
                    !(index > 0
                        && (0xdc00..=0xdfff).contains(&text[index])
                        && (0xd800..=0xdbff).contains(&text[index - 1]))
                })
                .collect();
            let code_point_count = starts_code_point.iter().filter(|&&starts| starts).count();
            loop {
                let offset = element_offsets.entry(element).or_default();
                if let Some(values) = self.run.pass.arena().svg_text_positions(element) {
                    let code_point_positions = positions
                        .iter_mut()
                        .zip(&starts_code_point)
                        .filter_map(|(position, &starts)| starts.then_some(position));
                    for (position, value) in code_point_positions.zip(values.iter().skip(*offset)) {
                        for (coordinate, length, reference) in [
                            (&mut position.x, value.x, self.viewport_width),
                            (&mut position.y, value.y, self.viewport_height),
                            (&mut position.dx, value.dx, self.viewport_width),
                            (&mut position.dy, value.dy, self.viewport_height),
                        ] {
                            if coordinate.is_none() && length.kind != SVG_LENGTH_KIND_NONE {
                                *coordinate = Some(self.resolve_svg_text_length(element, length, reference));
                            }
                        }
                    }
                }
                *offset += code_point_count;
                if element == root {
                    break;
                }
                element = self.parent(element);
            }
            let text = self.apply_svg_text_transform(*node, text);
            if let Some(edits) = self.svg_text_transform_edits.borrow().get(node) {
                let mut transformed = Vec::with_capacity(text.len());
                let mut cursor = 0;
                for edit in edits {
                    let unchanged = edit.rendered_start_offset - transformed.len();
                    transformed.extend_from_slice(&positions[cursor..cursor + unchanged]);
                    cursor += unchanged;
                    if edit.rendered_length_in_code_units != 0 {
                        transformed.push(positions.get(cursor).copied().unwrap_or_default());
                        transformed.extend(std::iter::repeat_n(
                            SvgTextPosition::default(),
                            edit.rendered_length_in_code_units - 1,
                        ));
                    }
                    cursor += edit.dom_length_in_code_units;
                }
                transformed.extend_from_slice(&positions[cursor..]);
                positions = transformed;
            }
            self.text_positions.borrow_mut().insert(*node, positions);
            processed_text.insert(*node, std::sync::Arc::from(text));
        }
        processed_text
            .get(&text_node)
            .cloned()
            .unwrap_or_else(|| std::sync::Arc::from(Vec::new()))
    }

    /// The characters of a text node once its white space is processed, with its text-transform applied. The transform
    /// comes after the white space processing, so that the characters it changes map to those SVG addresses.
    /// NB: Masked text was transformed before it was masked, and has no white space left to process.
    fn apply_svg_text_transform(&self, text_node: Node, text: Vec<u16>) -> Vec<u16> {
        let inherited_text = self.style(self.parent(text_node)).inherited_text();
        if inherited_text.text_transform == crate::css::css_enums::text_transform::NONE
            || inherited_text.text_security != crate::css::css_enums::text_security::NONE
        {
            return text;
        }
        let uses_locale = super::rendered_text::transform_uses_locale(inherited_text.text_transform);
        let locale = self
            .run
            .pass
            .arena()
            .published_text_source(text_node, uses_locale)
            .locale;
        let length = text.len();
        let transformed = super::text_transform::render_text(
            text,
            locale.as_deref(),
            super::text_transform::TextRenderingOptions {
                text_transform: inherited_text.text_transform,
                text_security: crate::css::css_enums::text_security::NONE,
                white_space_collapse: crate::css::css_enums::white_space_collapse::PRESERVE,
                is_password_input: false,
                dom_start_offset: 0,
                dom_length_in_code_units: length,
            },
        );
        if !transformed.edits.is_empty() {
            self.svg_text_transform_edits
                .borrow_mut()
                .insert(text_node, transformed.edits);
        }
        transformed.text
    }

    /// The characters of the run of adjacent text nodes that starts at `text_node`: its own, and those of the text
    /// nodes that come right after it. Where one text node ends and the next starts is no boundary to the text, so the
    /// run is shaped as one. None where `text_node` comes right after another text node, whose run it belongs to.
    fn svg_text_run_starting_at(&self, text_node: Node) -> Option<std::sync::Arc<[u16]>> {
        let is_text = |node: Node| !node.is_invalid() && node_facts::kind_is_text(self.node_kind(node));
        if is_text(self.run.pass.previous_sibling(text_node)) {
            return None;
        }
        let text = self.processed_svg_text(text_node);
        let mut next = self.next_sibling(text_node);
        if !is_text(next) {
            return Some(text);
        }
        let mut run = text.to_vec();
        while is_text(next) {
            run.extend_from_slice(&self.processed_svg_text(next));
            next = self.next_sibling(next);
        }
        Some(std::sync::Arc::from(run))
    }

    // SVG DOM indices address UTF-16 code units before text transformation. Keep the painted glyphs and their
    // spacing, and publish the cell occupied by each source code unit of a masked or transformed run.
    // https://svgwg.org/svg2-draft/text.html#TermAddressableCharacter
    fn append_svg_run_dom_cells(
        &self,
        mut node: Node,
        cells: Vec<FfiSvgTextCharacterCell>,
        out: &mut Vec<FfiSvgTextCharacterCell>,
    ) {
        let inherited_text = self.style(self.parent(node)).inherited_text();
        let is_masked = inherited_text.text_security != crate::css::css_enums::text_security::NONE;
        if !is_masked && inherited_text.text_transform == crate::css::css_enums::text_transform::NONE {
            out.extend(cells);
            return;
        }
        let mut cells = cells.as_slice();
        while !node.is_invalid() && node_facts::kind_is_text(self.node_kind(node)) {
            let current = node;
            node = self.next_sibling(node);
            if is_masked {
                let content = self.run.pass.text_content(current);
                let Some(masked_text) = &content.svg_source_text else {
                    continue;
                };
                let (node_cells, rest) = cells.split_at(masked_text.len());
                append_svg_text_cells_through_edits(node_cells, content.rendered_text_edits(), out);
                cells = rest;
                continue;
            }
            let (node_cells, rest) = cells.split_at(self.processed_svg_text(current).len());
            match self.svg_text_transform_edits.borrow().get(&current) {
                Some(edits) => append_svg_text_cells_through_edits(node_cells, edits, out),
                None => out.extend_from_slice(node_cells),
            }
            cells = rest;
        }
    }

    fn append_svg_source_text(&self, text_node: Node, out: &mut Vec<u16>) {
        let content = self.run.pass.arena().text_content(text_node);
        debug_assert!(content.is_some(), "SVG text child was not synced before layout");
        if let Some(source) = content.and_then(|content| content.svg_source_text.as_deref()) {
            out.extend_from_slice(source);
        }
    }

    /// The first text node below `root` that renders a character, if there is one.
    fn first_rendered_svg_text_node(&self, root: Node) -> Option<Node> {
        let mut first = None;
        self.for_each_svg_text_node(root, |node| {
            if first.is_none() && !self.processed_svg_text(node).is_empty() {
                first = Some(node);
            }
        });
        first
    }

    fn svg_text_run_positions(&self, mut node: Node) -> Vec<SvgTextPosition> {
        self.processed_svg_text(node);
        let positions = self.text_positions.borrow();
        let mut run = Vec::new();
        while !node.is_invalid() && node_facts::kind_is_text(self.node_kind(node)) {
            if let Some(values) = positions.get(&node) {
                run.extend_from_slice(values);
            }
            node = self.next_sibling(node);
        }
        run
    }

    // NB: A combining mark consumes a positioning entry but shares its base's cell. Its relative shift belongs to
    //     the next typographic character, or to the cursor after the run if no character follows in this run.
    fn svg_text_character_positions(
        &self,
        node: Node,
        cells: &[FfiSvgTextCharacterCell],
    ) -> (Vec<SvgTextPosition>, FfiFloatPoint) {
        let mut positions = self.svg_text_run_positions(node);
        let mut pending = FfiFloatPoint::default();
        for (position, cell) in positions.iter_mut().zip(cells) {
            if cell.starts_typographic_character {
                if pending.x != 0.0 {
                    position.dx = Some(position.dx.unwrap_or(0.0) + pending.x);
                }
                if pending.y != 0.0 {
                    position.dy = Some(position.dy.unwrap_or(0.0) + pending.y);
                }
                pending = FfiFloatPoint::default();
            } else {
                pending.x += position.dx.take().unwrap_or(0.0);
                pending.y += position.dy.take().unwrap_or(0.0);
            }
        }
        (positions, pending)
    }

    fn svg_text_is_first(&self, node: Node, offset: usize, element: Node) -> bool {
        if offset != 0 {
            return false;
        }
        if let Some(first) = self.first_text_runs.borrow().get(&element) {
            return *first == Some(node);
        }
        let first = self.first_rendered_svg_text_node(element).map(|mut first| {
            loop {
                let previous = self.run.pass.previous_sibling(first);
                if previous.is_invalid() || !node_facts::kind_is_text(self.node_kind(previous)) {
                    return first;
                }
                first = previous;
            }
        });
        self.first_text_runs.borrow_mut().insert(element, first);
        first == Some(node)
    }

    /// Positions typographic characters without separating the glyphs that shaping assigned to one character.
    fn position_svg_text_run(&mut self, node: Node, shaped: &mut ShapedSvgText) {
        let (positions, trailing_shift) = self.svg_text_character_positions(node, &shaped.character_cells);
        let glyph_scale = self.svg_text_glyph_scale(self.parent(node));
        let origin = self.current_text_position;
        let mut shift = FfiFloatPoint::default();
        let mut shifts = Vec::with_capacity(shaped.character_cells.len());
        shaped.glyph_cells = SvgCssPixelRect::default();
        for (index, cell) in shaped.character_cells.iter_mut().enumerate() {
            if cell.starts_typographic_character {
                let position = positions[index];
                self.current_text_position = FfiFloatPoint {
                    x: cell.x + shift.x,
                    y: origin.y + shift.y,
                };
                if let Some(x) = position.x {
                    self.current_text_position.x = x / glyph_scale;
                }
                if let Some(y) = position.y {
                    self.current_text_position.y = y;
                }
                if let Some(dx) = position.dx {
                    self.current_text_position.x +=
                        dx * self.svg_text_dx_scale(node, index, position.x.is_some()) / glyph_scale;
                }
                self.current_text_position.y += position.dy.unwrap_or(0.0);
                if let Some(chunk) = self.svg_text_chunk(node, index) {
                    // NB: 'text-anchor' shifts the characters of one chunk only, so a chunk that starts without an
                    //     'x' continues from where the characters before it end without the shift of their chunk.
                    if position.x.is_none() {
                        self.current_text_position.x += self.text_anchor_shift / glyph_scale;
                    }
                    let anchor_shift = match chunk.anchor {
                        text_anchor::START => chunk.start,
                        text_anchor::MIDDLE => (chunk.start + chunk.end) / 2.0,
                        text_anchor::END => chunk.end,
                        _ => unreachable!("invalid text-anchor value"),
                    };
                    self.current_text_position.x -= anchor_shift;
                    self.text_anchor_shift = anchor_shift * glyph_scale;
                }
                shift.x = self.current_text_position.x - cell.x;
                shift.y = self.current_text_position.y - origin.y;
            }
            shifts.push(shift);
            cell.x += shift.x;
            cell.y += shift.y;
            cell.alignment_point_y = origin.y + shift.y;
            shaped.glyph_cells.unite(float_rect_to_css_pixels(FloatRect::new(
                cell.x,
                cell.y,
                cell.width,
                cell.height,
            )));
        }
        shaped.shift_glyphs(&shifts);
        if trailing_shift.x != 0.0 {
            shift.x += trailing_shift.x * self.svg_text_dx_scale(node, positions.len(), false) / glyph_scale;
        }
        self.current_text_position = FfiFloatPoint {
            x: origin.x + shaped.advance + shift.x,
            y: origin.y + shift.y + trailing_shift.y,
        };
    }

    /// https://drafts.csswg.org/css-inline/#dominant-baseline-property
    /// https://drafts.csswg.org/css-inline/#alignment-baseline-property
    /// NB: The text of an element sits on the baseline its alignment-baseline names, where that's other than 'baseline',
    ///     and on its dominant baseline otherwise.
    fn svg_dominant_baseline_offset(&self, style: StyleValues<'_>) -> f32 {
        let svg = style.inherited_svg();
        let svg_reset = style.svg_reset();
        let metric = if svg_reset.has_alignment_baseline {
            svg_reset.alignment_baseline
        } else if svg.has_dominant_baseline {
            svg.dominant_baseline
        } else {
            // https://drafts.csswg.org/css-inline/#valdef-dominant-baseline-auto
            // Equivalent to alphabetic in horizontal writing modes and in vertical writing modes
            // when text-orientation is sideways. Equivalent to central in vertical writing modes
            // when text-orientation is mixed or upright.
            // FIXME: Take text-orientation into account once it is implemented.
            match style.writing_mode() {
                writing_mode::VERTICAL_RL | writing_mode::VERTICAL_LR => baseline_metric::CENTRAL,
                _ => baseline_metric::ALPHABETIC,
            }
        };
        // NB: The dominant-baseline offset is resolved against the metrics of the first available
        //     font - while each glyph is rendered with the first font in the cascade that contains
        //     its code point.
        let font = style.font().first_available_font_metrics();
        match metric {
            baseline_metric::CENTRAL => (font.ascent - font.descent) / 2.0,
            baseline_metric::MIDDLE => font.x_height / 2.0,
            // FIXME: Read the hanging baseline from the font's BASE table.
            baseline_metric::HANGING => font.ascent * 0.8,
            // FIXME: Read the ideographic baseline from the font's BASE table.
            baseline_metric::IDEOGRAPHIC => -font.descent,
            // FIXME: Read the math baseline from the font's BASE table.
            baseline_metric::MATHEMATICAL => font.ascent * 0.5,
            // https://drafts.csswg.org/css-inline/#text-over-baseline
            // Corresponds to the metric used as the line-over edge of an inline's content box per [CSS2].
            baseline_metric::TEXT_TOP => font.ascent,
            // https://drafts.csswg.org/css-inline/#text-under-baseline
            // Corresponds to the metric used as the line-under edge of an inline's content box per [CSS2].
            baseline_metric::TEXT_BOTTOM => -font.descent,
            _ => 0.0,
        }
    }

    /// Calls `run` with each stretch of `text` the cascade resolves to one font, in order.
    fn for_each_svg_font_run(
        style: StyleValues<'_>,
        text: &[u16],
        mut run: impl FnMut(&libgfx_rust::font::FontHandle, &[u16]),
    ) {
        if text.is_empty() {
            return;
        }
        const ZERO_WIDTH_JOINER: u32 = 0x200d;
        let frozen_font_list = style.frozen_font_list();
        // https://drafts.csswg.org/css-fonts-4/#font-variant-emoji-prop
        // FIXME: Take 'font-variant-emoji' into account.
        let presentation_at = |offset: usize| {
            let code_point = code_point_at(text, offset);
            let next_offset = offset + code_point_length_at(text, offset);
            let next_code_point = (next_offset < text.len()).then(|| code_point_at(text, next_offset));
            let offset_after_next =
                next_offset + next_code_point.map_or(0, |_| code_point_length_at(text, next_offset));
            let code_point_after_next = (next_code_point.is_some() && offset_after_next < text.len())
                .then(|| code_point_at(text, offset_after_next));
            libgfx_rust::font::emoji_presentation_for_code_point(code_point, next_code_point, code_point_after_next)
        };
        let font_for =
            |offset: usize| frozen_font_list.font_for_code_point(code_point_at(text, offset), presentation_at(offset));
        let mut last_font = font_for(0);
        let mut run_start = 0;
        let mut previous_code_point = code_point_at(text, 0);
        let mut joiner_joins_emoji = false;
        let mut offset = code_point_length_at(text, 0);
        while offset < text.len() {
            let code_point = code_point_at(text, offset);
            // NB: A variation selector and a joiner go with the character they follow. And the font that draws that
            //     character draws the rest of its sequence too, as far as it has the glyphs: What extends the
            //     sequence, and what a joiner joins to it. A joiner after a character that's no emoji joins nothing,
            //     and neither does one before a character with a variation selector that asks for its text
            //     presentation: Those are no part of an emoji sequence.
            if code_point == ZERO_WIDTH_JOINER {
                // NB: A pictograph that defaults to text presentation ends an emoji ZWJ element too, when the joiner
                //     joins it to another pictograph.
                let joined_offset = offset + code_point_length_at(text, offset);
                let joined_code_point = (joined_offset < text.len()).then(|| code_point_at(text, joined_offset));
                joiner_joins_emoji = libgfx_rust::font::code_point_ends_emoji_zwj_element(previous_code_point)
                    || libgfx_rust::font::emoji_presentation_for_code_point(
                        previous_code_point,
                        Some(code_point),
                        joined_code_point,
                    )
                    .is_emoji;
            }
            let presentation = presentation_at(offset);
            let is_joined = previous_code_point == ZERO_WIDTH_JOINER
                && joiner_joins_emoji
                && (presentation.is_emoji || !presentation.forced);
            let stays_in_run = matches!(code_point, 0xfe0e | 0xfe0f | ZERO_WIDTH_JOINER)
                || ((libgfx_rust::font::code_point_extends_emoji_sequence(code_point) || is_joined)
                    && last_font.contains_glyph(code_point));
            if !stays_in_run {
                let font = font_for(offset);
                if font != last_font {
                    run(&last_font, &text[run_start..offset]);
                    last_font = font;
                    run_start = offset;
                }
            }
            previous_code_point = code_point;
            offset += code_point_length_at(text, offset);
        }
        run(&last_font, &text[run_start..]);
    }

    /// Mirrors Gfx::shape_text(baseline_start, text, font_cascade_list): one run per stretch of
    /// text the cascade resolves to the same font, each starting where the last one ended. Also
    /// returns the union of the runs' glyph cells, each run's measured with its own font, and the
    /// glyph cell of every code unit of the text.
    fn shape_svg_text(
        &self,
        style: StyleValues<'_>,
        text: &[u16],
        baseline_start: FfiFloatPoint,
        text_length_spacing: f32,
    ) -> ShapedSvgText {
        let mut shaped_text = ShapedSvgText {
            runs: Vec::new(),
            advance: 0.0,
            glyph_cells: SvgCssPixelRect::default(),
            character_cells: Vec::with_capacity(text.len()),
        };
        // NB: The space 'textLength' adds after each typographic character is laid out as letter spacing is, but isn't
        //     part of the character's cell.
        let letter_spacing = style.letter_spacing().to_double() as f32 + text_length_spacing;
        let word_spacing = style.word_spacing().to_double() as f32;
        Self::for_each_svg_font_run(style, text, |font, run_text| {
            let run_start_x = baseline_start.x + shaped_text.advance;
            let shaped = libgfx_rust::text_layout::shape_text(
                font,
                run_text,
                libgfx_rust::text_layout::TextType::Common,
                run_start_x,
                letter_spacing,
                word_spacing,
            );
            // https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
            // The full glyph cell must have width equal to the horizontal advance and height equal to the EM box for
            // horizontal text.
            // For example, for horizontal text, the calculations must assume that each glyph extends vertically to the
            // full ascent and descent values for the font.
            // NB: The glyphs of one run sit side by side on one baseline, so the union of their cells is a single rect.
            // FIXME: Take writing mode into account.
            let facts = font.facts();
            let cell_top = baseline_start.y - facts.ascent;
            let cell_height = facts.ascent + facts.descent;
            shaped_text.glyph_cells.unite(float_rect_to_css_pixels(FloatRect::new(
                run_start_x,
                cell_top,
                shaped.width() - text_length_spacing,
                cell_height,
            )));
            append_svg_text_character_cells(
                &mut shaped_text.character_cells,
                shaped.glyphs(),
                run_text.len(),
                run_start_x + shaped.width(),
                cell_top,
                cell_height,
                text_length_spacing,
            );
            shaped_text.advance += shaped.width();
            let mut glyphs = shaped.into_glyphs();
            if baseline_start.y != 0.0 {
                glyphs.shift_baseline(baseline_start.y);
            }
            shaped_text.runs.push(libgfx_rust::path::GlyphRun {
                font: font.clone(),
                glyphs,
            });
        });
        shaped_text
    }

    fn shape_svg_text_run(
        &self,
        node: Node,
        text: &[u16],
        baseline_start: FfiFloatPoint,
        spacing: f32,
    ) -> ShapedSvgText {
        // A positioning boundary prevents an optional ligature from joining characters across it. A nonzero
        // letter spacing disables those ligatures while retaining combining marks in their shaping clusters.
        let spacing = if spacing == 0.0
            && self
                .svg_text_run_positions(node)
                .iter()
                .skip(1)
                .any(|position| position.is_specified())
        {
            f32::MIN_POSITIVE
        } else {
            spacing
        };
        self.shape_svg_text(self.style(self.parent(node)), text, baseline_start, spacing)
    }

    /// Measures every chunk once, using the same character boundaries and adjustments as glyph placement.
    fn svg_text_chunk(&self, node: Node, offset: usize) -> Option<SvgTextChunkMeasurement> {
        let mut root = self.parent(node);
        while !self.parent(root).is_invalid() && self.node_kind(self.parent(root)) == NodeKind::SVGTextBox {
            root = self.parent(root);
        }
        // The root entry marks that even characters which don't start a chunk have been considered.
        if !self.text_chunks.borrow().contains_key(&(root, 0)) {
            let mut chunks = std::collections::HashMap::new();
            let mut chunk_start = None;
            let mut chunk = SvgTextChunkMeasurement::default();
            let mut glyph_scale = 1.0;
            let mut position = 0.0;
            let mut current = root;
            'walk: loop {
                let kind = self.node_kind(current);
                if node_facts::kind_is_text(kind)
                    && let Some(text) = self.svg_text_run_starting_at(current)
                    && !text.is_empty()
                {
                    let text_box = self.parent(current);
                    let spacing = self.svg_text_length_adjustment_for(text_box).spacing();
                    let shaped = self.shape_svg_text_run(current, &text, FfiFloatPoint::default(), spacing);
                    let (positions, trailing_shift) =
                        self.svg_text_character_positions(current, &shaped.character_cells);
                    let run_scale = self.svg_text_glyph_scale(text_box);
                    for (index, cell) in shaped.character_cells.iter().enumerate() {
                        if !cell.starts_typographic_character {
                            continue;
                        }
                        let adjustment = positions[index];
                        if chunk_start.is_none() || adjustment.starts_chunk() {
                            if let Some(start) = chunk_start {
                                chunks.insert(start, chunk);
                            }
                            chunk_start = Some((current, index));
                            chunk = SvgTextChunkMeasurement {
                                anchor: self.style(text_box).inherited_svg().text_anchor,
                                ..SvgTextChunkMeasurement::default()
                            };
                            glyph_scale = run_scale;
                            position = 0.0;
                        } else if let Some(dx) = adjustment.dx {
                            position += dx * self.svg_text_dx_scale(current, index, false) / glyph_scale;
                        }
                        let end = position + cell.width * run_scale / glyph_scale;
                        chunk.start = chunk.start.min(position).min(end);
                        chunk.end = chunk.end.max(position).max(end);
                        position = end + spacing * run_scale / glyph_scale;
                    }
                    if trailing_shift.x != 0.0 {
                        position +=
                            trailing_shift.x * self.svg_text_dx_scale(current, positions.len(), false) / glyph_scale;
                    }
                }
                // AD-HOC: Text on a path is laid out independently; see svg_text_path_box_path().
                if kind == NodeKind::SVGTextBox {
                    let child = self.first_child(current);
                    if !child.is_invalid() {
                        current = child;
                        continue;
                    }
                }
                loop {
                    if current == root {
                        break 'walk;
                    }
                    if self.node_kind(current) == NodeKind::SVGTextBox {
                        position += self.svg_text_length_trailing_adjustment(current) / glyph_scale;
                    }
                    let sibling = self.next_sibling(current);
                    if !sibling.is_invalid() {
                        current = sibling;
                        break;
                    }
                    current = self.parent(current);
                }
            }
            if let Some(start) = chunk_start {
                chunks.insert(start, chunk);
            }
            chunks.insert((root, 0), SvgTextChunkMeasurement::default());
            self.text_chunks.borrow_mut().extend(chunks);
        }
        self.text_chunks.borrow().get(&(node, offset)).copied()
    }

    /// Lays out what a <text>, a <tspan>, or an <a> inside one renders, in document order: its text nodes, which it
    /// shapes itself, and its text content child elements. Every character advances the current text position.
    fn layout_svg_text_box_contents(
        &mut self,
        run: &FormattingContextRun<'pass>,
        text_box: Node,
    ) -> SvgTextBoxContents {
        let attributes = self.svg_attributes(text_box);
        // https://svgwg.org/svg2-draft/text.html#TextElementXAttribute
        // the starting X (Y) coordinate for rendering the glyphs corresponding to the given
        // character is the X (Y) coordinate of the resulting current text position from the most
        // recently rendered glyph for the current 'text' element.
        // NB: The initial value of 'x' and 'y' is "0 for 'text'; (none) for 'tspan'": a <text>
        //     element starts at (0, 0) regardless of the current text position, while a <tspan>
        //     without 'x'/'y' continues at the current text position.
        if attributes.is_text_element {
            self.current_text_position = FfiFloatPoint::default();
            self.text_anchor_shift = 0.0;
            self.text_length_adjustments.borrow_mut().clear();
            self.text_chunks.borrow_mut().clear();
            self.first_text_runs.borrow_mut().clear();
        }
        let parent_scale = if attributes.is_text_element {
            1.0
        } else {
            self.svg_text_glyph_scale(self.parent(text_box))
        };
        let glyph_scale = self.svg_text_glyph_scale(text_box);
        // Each box shapes before its glyph scale is applied. Keep the physical cursor at the same position across
        // element boundaries, even when the parent and child scale their glyphs differently.
        self.current_text_position.x *= parent_scale / glyph_scale;
        let dominant_baseline_offset = self.svg_dominant_baseline_offset(self.style(text_box));
        let scale_glyphs = |path: libgfx_rust::path::Path| {
            if glyph_scale == 1.0 {
                path
            } else {
                path.copy_transformed([glyph_scale, 0.0, 0.0, 1.0, 0.0, 0.0])
            }
        };
        let mut contents = SvgTextBoxContents::default();
        // https://svgwg.org/svg2-draft/text.html#TextDecorationProperties
        // The fill and stroke of the text decoration are given by the fill and stroke of the text at the point where
        // the text decoration is declared
        // NB: So, the element that declares a line draws it along all of the text in its subtree.
        let decoration_lines = self.svg_text_decoration_lines(text_box);
        let (font_ascent, font_size) = {
            let style = self.style(text_box);
            (
                style.font().first_available_font_metrics().ascent,
                style.font_size().to_double() as f32,
            )
        };
        let mut spans: Vec<SvgTextRunSpan> = Vec::new();
        // Text that follows a child element paints after that child, so it's kept apart from the text before it.
        // And an underline or an overline paints before the text it runs along, and a line-through after it.
        let mut paint_parts = SvgTextPaintParts::default();
        let mut after_child = Node::INVALID;
        for child in self.run.pass.children(text_box) {
            let kind = self.node_kind(child);
            if node_facts::kind_is_text(kind) {
                let Some(text) = self.svg_text_run_starting_at(child) else {
                    continue;
                };
                let mut text_offset = FfiFloatPoint {
                    x: self.current_text_position.x,
                    y: self.current_text_position.y + dominant_baseline_offset,
                };
                let mut shaped = self.shape_svg_text_run(
                    child,
                    &text,
                    text_offset,
                    self.svg_text_length_adjustment_for(text_box).spacing(),
                );
                self.position_svg_text_run(child, &mut shaped);
                if let Some(first) = shaped.character_cells.first() {
                    text_offset.x = first.x;
                    text_offset.y = first.alignment_point_y + dominant_baseline_offset;
                }
                if glyph_scale != 1.0 {
                    let glyph_cells = shaped.glyph_cells.to_float_rect();
                    shaped.glyph_cells = float_rect_to_css_pixels(FloatRect::new(
                        glyph_cells.x * glyph_scale,
                        glyph_cells.y,
                        glyph_cells.width * glyph_scale,
                        glyph_cells.height,
                    ));
                    for cell in &mut shaped.character_cells {
                        cell.x *= glyph_scale;
                        cell.width *= glyph_scale;
                    }
                }

                // NB: Decoration spans follow character cells so gaps, overlaps, and baseline changes stay separate.
                let mut run_spans: Vec<SvgTextRunSpan> = if text.is_empty() {
                    Vec::new()
                } else if self.svg_text_length_adjustment_for(text_box).spacing() != 0.0
                    || self
                        .svg_text_run_positions(child)
                        .iter()
                        .skip(1)
                        .any(|position| position.is_specified())
                {
                    shaped
                        .character_cells
                        .iter()
                        .filter(|cell| cell.starts_typographic_character)
                        .map(|cell| SvgTextRunSpan {
                            x: cell.x / glyph_scale,
                            advance: cell.width / glyph_scale,
                            baseline: cell.alignment_point_y + dominant_baseline_offset,
                            ascent: font_ascent,
                            font_size,
                        })
                        .collect()
                } else {
                    vec![SvgTextRunSpan {
                        x: text_offset.x,
                        advance: shaped.advance,
                        baseline: text_offset.y,
                        ascent: font_ascent,
                        font_size,
                    }]
                };
                run_spans.dedup_by(|span, previous| {
                    if previous.baseline == span.baseline && previous.x + previous.advance == span.x {
                        previous.advance += span.advance;
                        true
                    } else {
                        false
                    }
                });
                let mut lines_under = libgfx_rust::path::Path::new();
                let mut lines_over = libgfx_rust::path::Path::new();
                if let Some(lines) = &decoration_lines {
                    for span in &run_spans {
                        lines.add_lines_under_text(span, &mut lines_under);
                        lines.add_lines_over_text(span, &mut lines_over);
                    }
                }
                spans.extend(run_spans);
                paint_parts.add_decoration_lines(after_child, lines_under);
                for run in shaped.runs {
                    if !run.font.is_emoji_font() {
                        paint_parts.add_glyphs(after_child, vec![run]);
                        continue;
                    }
                    // NB: The glyphs of a color font have no outlines to fill or stroke, so they're kept as glyphs.
                    //     Where 'textLength' scales the glyphs, they move to where they'd land, but aren't stretched.
                    let facts = run.font.facts();
                    let mut bounds: Option<FloatRect> = None;
                    let glyphs = run
                        .glyphs
                        .placed()
                        .filter(|placed| placed.glyph.should_paint())
                        .map(|placed| {
                            let x = placed.x * glyph_scale;
                            let cell = FloatRect::new(x, placed.y, placed.glyph.advance, facts.ascent + facts.descent);
                            bounds = Some(united_even_if_empty(bounds, cell));
                            fragment_tree::SvgColorGlyph {
                                x,
                                y: placed.y,
                                glyph_id: u32::from(placed.glyph.glyph_id),
                            }
                        })
                        .collect();
                    paint_parts.add_color_glyphs(
                        after_child,
                        fragment_tree::SvgColorGlyphRun {
                            font: run.font,
                            glyphs,
                            bounds: bounds.unwrap_or_default(),
                        },
                    );
                }
                paint_parts.add_decoration_lines(after_child, lines_over);

                contents.glyph_cells.unite(shaped.glyph_cells);
                let first_own_cell = contents.character_cells.len();
                self.append_svg_run_dom_cells(child, shaped.character_cells, &mut contents.character_cells);
                contents
                    .own_character_cells
                    .extend_from_slice(&contents.character_cells[first_own_cell..]);
            } else if matches!(kind, NodeKind::SVGTextBox | NodeKind::SVGTextPathBox) {
                // <text> and <tspan> elements can contain more text elements, whose glyph cells count toward this
                // box too.
                self.last_text_subtree_spans.clear();
                let child_geometry = self.layout_graphics_element(run, child);
                contents
                    .children_bounding_box
                    .unite(float_rect_to_css_pixels(child_geometry.stroke_bounding_box));
                if let Some(cells) = self.last_text_subtree_cells.take() {
                    contents.character_cells.extend(cells.iter().copied());
                }
                let child_spans = std::mem::take(&mut self.last_text_subtree_spans);
                let mut lines_under = libgfx_rust::path::Path::new();
                let mut lines_over = libgfx_rust::path::Path::new();
                if let Some(lines) = decoration_lines {
                    for span in &child_spans {
                        lines.add_lines_under_text(span, &mut lines_under);
                        lines.add_lines_over_text(span, &mut lines_over);
                    }
                }
                paint_parts.add_decoration_lines(after_child, lines_under);
                after_child = child;
                paint_parts.add_decoration_lines(after_child, lines_over);
                spans.extend(child_spans);
            }
        }
        self.last_text_subtree_spans = spans;

        // NB: The element's path is the outlines of its glyphs alone. Where that's all there is to paint, and nothing
        //     has to paint between two parts of it, the path paints as it is.
        let paints_as_one_path = paint_parts.are_one_path();
        let mut glyph_outlines = libgfx_rust::path::Path::new();
        let mut color_glyph_cells = libgfx_rust::path::Path::new();
        let mut segments = Vec::new();
        for (after_child, part) in paint_parts.parts {
            let mut color_glyph_runs = Vec::new();
            let path = match part {
                SvgTextPaintPart::Glyphs(runs) => {
                    let path = libgfx_rust::path::Path::from_glyph_runs(&runs);
                    if paints_as_one_path {
                        glyph_outlines = path;
                        break;
                    }
                    glyph_outlines.append_path(&path);
                    path
                }
                SvgTextPaintPart::ColorGlyphs(runs) => {
                    // NB: A glyph without an outline takes hits anywhere in its cell.
                    for run in &runs {
                        color_glyph_cells.append_path(&path_of_rects(&[run.bounds]));
                    }
                    color_glyph_runs = runs;
                    libgfx_rust::path::Path::new()
                }
                SvgTextPaintPart::DecorationLines(lines) => lines,
            };
            segments.push(fragment_tree::SvgTextSegment {
                after_child,
                path: scale_glyphs(path),
                color_glyph_runs,
            });
        }
        contents.path = scale_glyphs(glyph_outlines);
        // NB: The cells of the color glyphs are where 'textLength' moves the glyphs to already.
        contents.path.append_path(&color_glyph_cells);
        if !paints_as_one_path {
            contents.segments = Some(segments);
        }
        self.current_text_position.x *= glyph_scale / parent_scale;
        self.current_text_position.x += self.svg_text_length_trailing_adjustment(text_box) / parent_scale;
        for span in &mut self.last_text_subtree_spans {
            span.x *= glyph_scale / parent_scale;
            span.advance *= glyph_scale / parent_scale;
        }
        if attributes.is_text_element {
            self.text_length_adjustments.borrow_mut().clear();
            self.text_chunks.borrow_mut().clear();
            self.first_text_runs.borrow_mut().clear();
        }
        contents
    }

    /// The text decoration lines the given text content element declares, if it declares any.
    fn svg_text_decoration_lines(&self, text_box: Node) -> Option<SvgTextDecorationLines> {
        let style = self.style(text_box);
        let text_reset = style.text_reset();
        let mut lines = SvgTextDecorationLines::default();
        for line in text_reset.text_decoration_lines.as_slice() {
            match *line {
                text_decoration_line::UNDERLINE => lines.underline = true,
                text_decoration_line::OVERLINE => lines.overline = true,
                text_decoration_line::LINE_THROUGH => lines.line_through = true,
                _ => {}
            }
        }
        if !(lines.underline || lines.overline || lines.line_through) {
            return None;
        }
        lines.style = text_reset.text_decoration_style;
        // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-length-percentage
        const TEXT_DECORATION_THICKNESS_KIND_LENGTH_PERCENTAGE: u8 = 2;
        if text_reset.text_decoration_thickness_kind == TEXT_DECORATION_THICKNESS_KIND_LENGTH_PERCENTAGE {
            lines.thickness = text_reset
                .text_decoration_thickness
                .length_percentage()
                .map(|value| value.to_px(style.font_size()).to_double() as f32);
        }
        Some(lines)
    }

    /// The 'textLength' of the given text content element, where it gives a valid one.
    /// https://svgwg.org/svg2-draft/text.html#TextElementTextLengthAttribute
    /// The author's computation of the total sum of all of the advance values that correspond to character data
    /// within this element, including the advance value on the glyph (horizontal or vertical), the effect of
    /// properties letter-spacing and word-spacing and adjustments due to attributes 'dx' and 'dy' on this 'text' or
    /// 'tspan' element or any descendants. This value is used to calibrate the user agent's own calculations with
    /// that of the author.
    /// NB: A 'dx' on the element's first character moves where its text starts, so it's no part of the length. And
    ///     the length is what the characters cover: from the least position one of them starts at to the greatest one
    ///     ends at, which is past where the last one ends where a negative 'dx' takes a character back.
    fn svg_text_length(&self, text_box: Node) -> Option<f32> {
        let attributes = self.svg_attributes(text_box);
        if attributes.text_length.kind == SVG_LENGTH_KIND_NONE {
            return None;
        }
        let length = self.resolve_svg_text_length(text_box, attributes.text_length, self.viewport_width);
        (length.is_finite() && length > 0.0).then_some(length)
    }

    fn svg_text_length_adjustment_for(&self, mut text_box: Node) -> SvgTextLengthAdjustment {
        while !text_box.is_invalid() && self.node_kind(text_box) == NodeKind::SVGTextBox {
            if self.svg_text_length(text_box).is_some() {
                return self.resolve_svg_text_length_adjustment(text_box).adjustment;
            }
            text_box = self.parent(text_box);
        }
        SvgTextLengthAdjustment::None
    }

    fn svg_text_glyph_scale(&self, text_box: Node) -> f32 {
        self.svg_text_length_adjustment_for(text_box).glyph_scale()
    }

    /// A leading 'dx' or one at an absolute 'x' moves the origin; only inline shifts are scaled by 'textLength'.
    fn svg_text_dx_scale(&self, text_node: Node, offset: usize, has_x: bool) -> f32 {
        if has_x {
            return 1.0;
        }
        let mut element = self.parent(text_node);
        while !element.is_invalid() && self.node_kind(element) == NodeKind::SVGTextBox {
            // NB: Only an element with a 'textLength' can scale the shift.
            if self.svg_text_length(element).is_some() && !self.svg_text_is_first(text_node, offset, element) {
                return self
                    .resolve_svg_text_length_adjustment(element)
                    .adjustment
                    .glyph_scale();
            }
            element = self.parent(element);
        }
        1.0
    }

    /// https://svgwg.org/svg2-draft/text.html#TextLayoutAlgorithm
    /// Each resolved descendant node is treated as if it were a single typographic character in this context.
    /// NB: Scale its advance for following text, but keep its internal positions and glyphs at their own scale.
    fn svg_text_length_trailing_adjustment(&self, text_box: Node) -> f32 {
        if self.svg_text_length(text_box).is_none() {
            return 0.0;
        }
        let measurement = self.resolve_svg_text_length_adjustment(text_box).measurement;
        if measurement.characters == 0 {
            return 0.0;
        }
        let parent = self.parent(text_box);
        let parent_scale = self.svg_text_glyph_scale(parent);
        self.svg_text_length_adjustment_for(parent).spacing() * parent_scale
            + measurement.advance * (parent_scale - 1.0)
    }

    /// Measures a subtree with the given spacing, taking each resolved descendant's extent and advance as a unit.
    /// AD-HOC: Text on a path isn't adjusted, since it's laid out independently.
    fn measure_svg_text_length(&self, text_box: Node, spacing: f32) -> SvgTextLengthMeasurement {
        let mut measurement = SvgTextLengthMeasurement::default();
        let mut found_first = false;
        let mut last_resolved = None;
        self.for_each_svg_text_node_in(text_box, false, |node| {
            let mut resolved = None;
            let mut ancestor = self.parent(node);
            while ancestor != text_box {
                if self.svg_text_length(ancestor).is_some() {
                    resolved = Some(ancestor);
                }
                ancestor = self.parent(ancestor);
            }
            if let Some(element) = resolved {
                if last_resolved == resolved {
                    return;
                }
                last_resolved = resolved;
                let child = self.resolve_svg_text_length_adjustment(element).measurement;
                if child.characters == 0 {
                    return;
                }
                if found_first && let Some(first) = self.first_rendered_svg_text_node(element) {
                    let position = self.text_positions.borrow()[&first][0];
                    measurement.advance += position.dx.unwrap_or(0.0);
                }
                found_first = true;
                measurement.start = measurement.start.min(measurement.advance + child.start);
                measurement.end = measurement.end.max(measurement.advance + child.end);
                measurement.advance += child.advance + spacing;
                measurement.characters += 1;
                return;
            }
            last_resolved = None;
            let Some(text) = self.svg_text_run_starting_at(node) else {
                return;
            };
            if text.is_empty() {
                return;
            }
            let shaped = self.shape_svg_text_run(node, &text, FfiFloatPoint::default(), spacing);
            let (positions, trailing_shift) = self.svg_text_character_positions(node, &shaped.character_cells);
            for (index, cell) in shaped.character_cells.iter().enumerate() {
                if !cell.starts_typographic_character {
                    continue;
                }
                let position = positions[index];
                // NB: The 'textLength' adjustment happens before an 'x' or a 'y' starts a new chunk, so every 'dx' but
                //     the first character's is part of the length, at the start of a chunk too.
                if found_first {
                    measurement.advance += position.dx.unwrap_or(0.0);
                }
                found_first = true;
                let start = measurement.advance;
                let end = start + cell.width;
                measurement.start = measurement.start.min(start).min(end);
                measurement.end = measurement.end.max(start).max(end);
                measurement.advance = end + spacing;
                measurement.characters += 1;
            }
            measurement.advance += trailing_shift.x;
        });
        measurement
    }

    /// https://svgwg.org/svg2-draft/text.html#TextLayoutAlgorithm
    /// Child nodes are adjusted before parent nodes.
    /// NB: Measuring a resolved descendant asks for its adjustment first, and counts its whole subtree as one unit.
    fn resolve_svg_text_length_adjustment(&self, text_box: Node) -> SvgResolvedTextLength {
        if let Some(resolved) = self.text_length_adjustments.borrow().get(&text_box).copied() {
            return resolved;
        }
        let text_length = self.svg_text_length(text_box).expect("a valid text length");
        let attributes = self.svg_attributes(text_box);
        // Text with space between its characters is shaped without optional ligatures. Measure it with spacing too
        // small to affect its length, so measurement and shaping count the same typographic characters.
        let spacing_while_measuring = if attributes.text_length_adjusts_glyphs {
            0.0
        } else {
            f32::MIN_POSITIVE
        };
        let natural = self.measure_svg_text_length(text_box, spacing_while_measuring);
        let natural_length = natural.end - natural.start;
        let glyph_scale = text_length / natural_length;
        // NB: The layout divides by the scale, so a scale too small to divide by (from a subnormal 'textLength', e.g.)
        //     leaves the glyphs as they are.
        let adjustment = if attributes.text_length_adjusts_glyphs && natural_length > 0.0 && glyph_scale.is_normal() {
            SvgTextLengthAdjustment::SpacingAndGlyphs(glyph_scale)
        } else if !attributes.text_length_adjusts_glyphs && natural.characters > 1 {
            SvgTextLengthAdjustment::Spacing((text_length - natural_length) / (natural.characters - 1) as f32)
        } else {
            SvgTextLengthAdjustment::None
        };
        let mut measurement = self.measure_svg_text_length(text_box, adjustment.spacing());
        let scale = adjustment.glyph_scale();
        measurement.start *= scale;
        measurement.end *= scale;
        measurement.advance *= scale;
        let resolved = SvgResolvedTextLength {
            adjustment,
            measurement,
        };
        self.text_length_adjustments.borrow_mut().insert(text_box, resolved);
        resolved
    }

    /// The glyph outlines a <textPath> renders: its whole subtree's text, shaped from the origin
    /// and then laid along the shape its `href` names. Each run of text is shaped in the style of
    /// the element it's in. Also returns the glyph cell of each of its characters.
    /// https://svgwg.org/svg2-draft/text.html#TextPathElement
    /// FIXME: The character cells are those of the text shaped from the origin; they aren't moved
    ///        onto the path or rotated along it, so only their advances are meaningful.
    /// Where some of its own text follows a child element, also returns the parts its path paints in.
    fn svg_text_path_box_path(
        &self,
        text_path_box: Node,
    ) -> (
        libgfx_rust::path::Path,
        Vec<FfiSvgTextCharacterCell>,
        Option<Vec<fragment_tree::SvgTextSegment>>,
    ) {
        let empty = || libgfx_rust::path::Path::new();
        let Some(shape_path) = self.svg_referenced_shape_path(text_path_box) else {
            self.text_on_path_glyphs.borrow_mut().clear();
            return (empty(), Vec::new(), None);
        };

        let style = self.style(text_path_box);
        // The runs of the <textPath>'s own text, each with the child element it follows, or the invalid node for the
        // text that comes before every child element: Text that follows a child paints after it.
        let mut runs: Vec<(Node, Vec<libgfx_rust::path::GlyphRun>)> = Vec::new();
        let mut after_child = Node::INVALID;
        let mut character_cells = Vec::new();
        // NB: Each text content element inside the <textPath> paints its own text, in its own style.
        let mut runs_of_elements: Vec<(Node, Vec<libgfx_rust::path::GlyphRun>)> = Vec::new();
        let mut cells_of_elements: std::collections::HashMap<Node, Vec<FfiSvgTextCharacterCell>> =
            std::collections::HashMap::new();
        let mut advance = 0.0f32;
        // https://svgwg.org/svg2-draft/text.html#TextLayoutPath
        // Align the glyph vertically relative to the midpoint-on-the-path based on property alignment-baseline and any
        // specified values for attribute 'dy' on a 'tspan' element.
        // NB: A 'dx' moves a character along the path, and a 'dy' moves it off the path. Both shifts accumulate for
        //     the characters that follow, across text nodes and text content elements.
        let mut shift_off_path = 0.0f32;
        // NB: A 'dx' on the first character moves the text along the path once it's anchored, so it's no part of the
        //     extent the anchoring takes.
        let mut leading_dx = None;
        // The span the characters cover, which text-anchor places, as for ordinary text: A 'dx' can move a character
        // back past the ones before it.
        let (mut extent_start, mut extent_end) = (f32::INFINITY, f32::NEG_INFINITY);
        self.for_each_svg_text_node(text_path_box, |node| {
            let Some(text) = self.svg_text_run_starting_at(node) else {
                return;
            };
            let run_style = self.style(self.parent(node));
            // AD-HOC: Text on a path is laid out independently, so the 'textLength' of the <text> it's in doesn't
            //         adjust it.
            let mut shaped = self.shape_svg_text_run(
                node,
                &text,
                FfiFloatPoint {
                    x: advance,
                    y: self.svg_dominant_baseline_offset(run_style) + shift_off_path,
                },
                0.0,
            );
            let (positions, trailing_shift) = self.svg_text_character_positions(node, &shaped.character_cells);
            let mut shift = FfiFloatPoint::default();
            let mut shifts = Vec::with_capacity(shaped.character_cells.len());
            for (position, cell) in positions.iter().zip(&mut shaped.character_cells) {
                if cell.starts_typographic_character {
                    if let Some(x) = position.x {
                        shift.x = x - cell.x;
                    }
                    shift.x += position.dx.unwrap_or(0.0);
                    shift.y += position.dy.unwrap_or(0.0);
                    leading_dx.get_or_insert(advance + shift.x);
                }
                shifts.push(shift);
                cell.x += shift.x;
                cell.y += shift.y;
                let end = cell.x + cell.width;
                extent_start = extent_start.min(cell.x).min(end);
                extent_end = extent_end.max(cell.x).max(end);
            }
            shaped.shift_glyphs(&shifts);
            advance += shaped.advance + shift.x + trailing_shift.x;
            shift_off_path += shift.y + trailing_shift.y;
            let first_cell = character_cells.len();
            self.append_svg_run_dom_cells(node, shaped.character_cells, &mut character_cells);
            let owner = self.parent(node);
            if owner == text_path_box || self.node_kind(owner) != NodeKind::SVGTextBox {
                match runs.last_mut() {
                    Some((child, own_runs)) if *child == after_child => own_runs.extend(shaped.runs),
                    _ => runs.push((after_child, shaped.runs)),
                }
                return;
            }
            let mut child = owner;
            while self.parent(child) != text_path_box {
                child = self.parent(child);
            }
            after_child = child;
            match runs_of_elements.iter_mut().find(|(element, _)| *element == owner) {
                Some((_, element_runs)) => element_runs.extend(shaped.runs),
                None => runs_of_elements.push((owner, shaped.runs)),
            }
            // The cells of an element's characters count toward each element it's in, as they do in a <text>.
            let mut element = owner;
            while element != text_path_box && self.node_kind(element) == NodeKind::SVGTextBox {
                cells_of_elements
                    .entry(element)
                    .or_default()
                    .extend_from_slice(&character_cells[first_cell..]);
                element = self.parent(element);
            }
        });

        // https://svgwg.org/svg2-draft/text.html#TextPathElementStartOffsetAttribute
        let mut start_offset = self
            .svg_attributes(text_path_box)
            .text_path_start_offset
            .resolve_relative_to(shape_path.length());
        // FIXME: Take writing mode and text direction into account.
        if extent_start <= extent_end {
            let leading_dx = leading_dx.unwrap_or(0.0);
            match style.inherited_svg().text_anchor {
                text_anchor::START => start_offset -= extent_start - leading_dx,
                text_anchor::MIDDLE => start_offset -= (extent_start + extent_end) / 2.0 - leading_dx,
                text_anchor::END => start_offset -= extent_end - leading_dx,
                _ => unreachable!("invalid text-anchor value"),
            }
        }

        let mut text_on_path_glyphs = self.text_on_path_glyphs.borrow_mut();
        text_on_path_glyphs.clear();
        for (element, element_runs) in runs_of_elements {
            let cells = cells_of_elements.remove(&element).unwrap_or_default();
            text_on_path_glyphs.insert(
                element,
                (shape_path.place_glyph_runs_along(&element_runs, start_offset), cells),
            );
        }
        for (element, cells) in cells_of_elements {
            text_on_path_glyphs.insert(element, (libgfx_rust::path::Path::new(), cells));
        }
        let mut path = empty();
        let mut segments = Vec::new();
        let paints_as_one_path = runs.iter().all(|(after_child, _)| after_child.is_invalid());
        for (after_child, own_runs) in runs {
            let part = shape_path.place_glyph_runs_along(&own_runs, start_offset);
            path.append_path(&part);
            if !paints_as_one_path {
                segments.push(fragment_tree::SvgTextSegment {
                    after_child,
                    path: part,
                    color_glyph_runs: Vec::new(),
                });
            }
        }
        (path, character_cells, (!paints_as_one_path).then_some(segments))
    }

    /// The geometry of the shape element an SVG reference names, read from what that element
    /// published rather than from a box: a `<textPath>`'s target is normally a `<path>` inside a
    /// `<defs>`, and a `<defs>` builds no box at all.
    fn svg_referenced_shape_path(&self, referring_box: Node) -> Option<libgfx_rust::path::Path> {
        let arena = self.run.pass.arena();
        let referrer = arena.node_style_node(referring_box)?;
        let atom = self.svg_attributes(referring_box).reference_fragment_atom;
        let shape = arena.element_by_svg_reference(referrer, atom)?;
        let attributes = arena.style_node_svg_attribute_facts(shape);
        // Only a geometry element is followed; every other element answers with no shape at all.
        if attributes.geometry_kind == SVG_GEOMETRY_KIND_NONE {
            return None;
        }
        let style = arena.style_node_style(shape)?;
        let points = arena.style_node_svg_points(shape);
        // SAFETY: The record the element published is retained for the pass.
        let style = StyleValues::new(unsafe { style.view() });
        let path = self.shape_geometry().path(attributes, style, points.as_deref());
        // https://svgwg.org/svg2-draft/text.html#TextPathElementHrefAttribute
        // The 'transform' attribute on the referenced 'path element' element or basic shape element represents a
        // supplemental transformation relative to the current user coordinate system for the current 'text' element,
        // including any adjustments to the current user coordinate system due to a possible 'transform' property on
        // the current 'text' element.
        // NB: Only the element's own transform applies, not those of its ancestors.
        if style.transform().resolved_transforms.length == 0 {
            return Some(path);
        }
        let [x, y, width, height] = path.bounding_box();
        let reference_box = match used_transform::used_transform_box(*style, true) {
            transform_box::VIEW_BOX => {
                FloatRect::new(0.0, 0.0, self.viewport_user_rect.width, self.viewport_user_rect.height)
            }
            transform_box::FILL_BOX => FloatRect::new(x, y, width, height),
            _ => {
                // NB: The stroke box covers the stroke's outline, so its caps and joins too.
                let stroke = crate::svg::stroke_style::svg_stroke_style_for_percentage_basis(
                    &style,
                    crate::svg::stroke_style::percentage_basis(FfiCssPixelSize {
                        width: self.viewport_width,
                        height: self.viewport_height,
                    }),
                );
                let thickness = self.visible_stroke_width_in(style);
                let mut stroke_box = FloatRect::new(x, y, width, height);
                if thickness > 0.0 {
                    let outline = path.stroke_to_fill(&libgfx_rust::path::StrokeStyle {
                        thickness,
                        cap_style: stroke.cap_style,
                        join_style: stroke.join_style,
                        miter_limit: stroke.miter_limit,
                        dash_array: Vec::new(),
                        dash_offset: 0.0,
                    });
                    stroke_box = stroke_box.united(FloatRect::from_array(outline.bounding_box()));
                }
                stroke_box
            }
        };
        let transform =
            used_transform::transform_about_origin(*style, css_pixel_rect_of(reference_box), |_| None, None)
                .to_affine();
        Some(path.copy_transformed(transform.values))
    }

    fn svg_attributes(&self, node: Node) -> FfiSvgAttributeFacts {
        self.run.pass.arena().svg_attribute_facts(node)
    }

    fn svg_facts(&self, node: Node) -> SvgElementFacts {
        let data = self.run.pass.node_data(node);
        let mut facts = SvgElementFacts {
            is_document_element: node_facts::has_flag(data, NodeFlag::IsDocumentElement),
            document_is_decoded_svg: self.run.pass.arena().document_is_decoded_svg(),
            ..Default::default()
        };
        // Everything below is a graphics element's business; a <mask> or <clipPath> box answers
        // the defaults.
        if self.svg_attributes(node).is_graphics_element {
            facts.additional_element_transform = self.svg_additional_element_transform(node);
            facts.visible_stroke_width = self.visible_stroke_width(node);
        }
        facts
    }

    // https://svgwg.org/svg2-draft/struct.html#UseElement
    // The x and y properties define an additional transformation (translate(x,y), where x and y
    // represent the computed value of the corresponding property) to be applied to the 'use'
    // element, after any transformations specified with other properties.
    fn svg_additional_element_transform(&self, node: Node) -> FfiAffineTransform {
        if !self.svg_attributes(node).is_use_element {
            return FfiAffineTransform::default();
        }
        let style = self.style(node);
        FfiAffineTransform::default().translated(
            style.x().to_px(self.viewport_width).to_float(),
            style.y().to_px(self.viewport_height).to_float(),
        )
    }

    /// The stroke width the path's bounding box has to grow by: an invisible stroke takes up no
    /// room.
    // NB: CSS geometry-effect metadata relies on this reading only stroke color and width.
    //     If SVG bounds begin accounting for caps, joins, miter limits, or stroke opacity,
    //     mark those properties as affecting layout geometry as well.
    fn visible_stroke_width(&self, node: Node) -> f32 {
        self.visible_stroke_width_in(self.style(node))
    }

    /// The stroke width the bounding box of a path in the given style has to grow by, as visible_stroke_width() says.
    fn visible_stroke_width_in(&self, style: StyleValues<'_>) -> f32 {
        let svg = style.inherited_svg();
        let stroke_is_visible = crate::css::svg_paint::svg_paint_color(&svg.stroke, style.current_color())
            .is_some_and(|color| color >> 24 != 0);
        if !stroke_is_visible {
            return 0.0;
        }
        let percentage_basis = crate::svg::stroke_style::percentage_basis(FfiCssPixelSize {
            width: self.viewport_width,
            height: self.viewport_height,
        });
        svg.stroke_width
            .length_percentage()
            .map_or(0.0, |value| value.to_px(percentage_basis).to_double() as f32)
    }

    #[inline]
    fn style(&self, node: Node) -> StyleValues<'_> {
        StyleValues::for_node(&self.run.pass, node)
    }

    /// Cells for `node`, a box this context lays out in them.
    fn cells_of_svg_box(&self, node: Node) -> Cell<used_values::RootInputCells> {
        // SVG descendants deliberately carry no percentage basis.
        // SVG layout resolves percentages against the SVG viewport, not a CSS containing
        // block, so boxes inside the SVG subtree carry no percentage basis.
        Cell::new(used_values::cells_before_sizing(
            &self.run.pass,
            node,
            ContainingBlockConstraints::default(),
        ))
    }

    fn set_svg_viewport_transform(&self, node: Node, transform: FfiAffineTransform) {
        self.update_svg_payload_of(node, |svg| svg.facts.viewport_transform = Some(transform));
    }

    fn commit_svg_element_facts(&self, node: Node, facts: SvgElementFacts, attributes: FfiSvgAttributeFacts) {
        self.update_svg_payload_of(node, |svg| {
            let svg = &mut svg.facts;
            svg.additional_element_transform =
                (!facts.additional_element_transform.is_identity()).then_some(facts.additional_element_transform);
            svg.resource_content_units_are_object_bounding_box =
                attributes.content_units == SVG_UNITS_OBJECT_BOUNDING_BOX;
            svg.author_path_length = attributes.has_path_length.then_some(attributes.path_length);
        });
    }

    /// Commits what painting reads of how `resource` applies to the element it is laid out for, whose object bounding
    /// box is `target_bounding_box`.
    fn commit_resource_facts_for_target(
        &self,
        resource: Node,
        attributes: &FfiSvgAttributeFacts,
        target_bounding_box: FloatRect,
    ) {
        let mask_area = (self.node_kind(resource) == NodeKind::SVGMaskBox)
            .then(|| self.mask_area_in_target_user_space(attributes, target_bounding_box));
        self.update_svg_payload_of(resource, |svg| {
            svg.facts.object_bounding_box_of_resource_target = Some(target_bounding_box);
            svg.facts.mask_area_in_target_user_space = mask_area;
        });
    }

    /// The masking area of a mask, in the user space of the element it masks, whose object bounding box is
    /// `target_bounding_box`. Percentages in a userSpaceOnUse masking area resolve against the SVG viewport. A negative
    /// value or a value of zero disables rendering of the element, which an empty area tells.
    fn mask_area_in_target_user_space(
        &self,
        attributes: &FfiSvgAttributeFacts,
        target_bounding_box: FloatRect,
    ) -> FloatRect {
        let masking_area = if attributes.mask_units == SVG_UNITS_OBJECT_BOUNDING_BOX {
            let target_bounding_box = css_pixel_rect_of(target_bounding_box);
            let target_width = target_bounding_box.width.to_float();
            let target_height = target_bounding_box.height.to_float();
            FloatRect::new(
                target_bounding_box.x.to_float() + (attributes.mask_x.value * target_width),
                target_bounding_box.y.to_float() + (attributes.mask_y.value * target_height),
                attributes.mask_width.value * target_width,
                attributes.mask_height.value * target_height,
            )
        } else {
            let viewport = self.viewport_user_rect;
            FloatRect::new(
                attributes.mask_x.resolve_relative_to(viewport.width),
                attributes.mask_y.resolve_relative_to(viewport.height),
                attributes.mask_width.resolve_relative_to(viewport.width),
                attributes.mask_height.resolve_relative_to(viewport.height),
            )
        };
        if masking_area.is_empty() {
            return FloatRect::default();
        }
        masking_area
    }

    /// The geometry of `node`, whose fill covers `object_bounding_box` and whose box is `stroke_bounding_box`, in this
    /// context's user units.
    fn geometry_of(&self, node: Node, object_bounding_box: FloatRect, stroke_bounding_box: FloatRect) -> SvgGeometry {
        let mut geometry = SvgGeometry {
            object_bounding_box,
            stroke_bounding_box,
            viewport_user_rect: self.viewport_user_rect,
            ..Default::default()
        };
        geometry.local_transform = self.local_transform(node, &geometry);
        geometry
    }

    /// The transform of `node` about its transform origin, against the boxes of `geometry`, with a <use> element's
    /// translation.
    fn local_transform(&self, node: Node, geometry: &SvgGeometry) -> FfiAffineTransform {
        if !self.svg_attributes(node).is_graphics_element {
            return FfiAffineTransform::default();
        }
        let style = self.style(node);
        let additional_transform = self.svg_additional_element_transform(node);
        if style.transform().resolved_transforms.length == 0 && additional_transform.is_identity() {
            return FfiAffineTransform::default();
        }
        let transform_box =
            used_transform::used_transform_box(*style, node_facts::kind_is_svg_box(self.node_kind(node)));
        // FIXME: Apply the element's offset path.
        used_transform::transform_about_origin(
            *style,
            geometry.transform_reference_box(transform_box),
            |_| None,
            Some(additional_transform.into()),
        )
        .to_affine()
        .into()
    }

    /// Hands `node` its geometry, keeping the SVG viewport it establishes.
    fn commit_svg_geometry(&self, node: Node, geometry: SvgGeometry) {
        self.update_svg_payload_of(node, |svg| {
            svg.geometry = SvgGeometry {
                establishes_viewport: svg.geometry.establishes_viewport,
                established_viewport_user_rect: svg.geometry.established_viewport_user_rect,
                ..geometry
            };
        });
    }

    /// Places `node`, a box this context lays out in `cells`, whose metrics are final once the box is placed, and
    /// whose fill covers `object_bounding_box`, and returns its geometry.
    fn place_child(
        &self,
        node: Node,
        cells: &Cell<used_values::RootInputCells>,
        x: CssPixels,
        y: CssPixels,
        object_bounding_box: FloatRect,
    ) -> SvgGeometry {
        let stroke_bounding_box = SvgCssPixelRect {
            x,
            y,
            width: cells.content_inline_size(),
            height: cells.content_block_size(),
        };
        let geometry = self.geometry_of(node, object_bounding_box, stroke_bounding_box.to_float_rect());
        self.commit_svg_geometry(node, geometry);
        let placed_child = formatting_context::PlacedChild::laid_out_here(cells.get(), None, &self.run);
        self.place_laid_out_child(node, placed_child, x, y, None);
        geometry
    }

    fn place_laid_out_child(
        &self,
        node: Node,
        placed_child: formatting_context::PlacedChild,
        x: CssPixels,
        y: CssPixels,
        fragment_arrival: Option<fragment_tree::RunRootFragment>,
    ) {
        formatting_context::place_child(
            &self.run,
            node,
            placed_child,
            FfiCssPixelPoint { x, y },
            None,
            fragment_arrival,
        );
    }

    /// Gives the box laid out in `cells` a content size that is definite in both axes.
    fn set_definite_content_size(cells: &Cell<used_values::RootInputCells>, width: CssPixels, height: CssPixels) {
        cells.set_content_inline_size(width);
        cells.set_content_block_size(height);
        cells.update_cells(|cells| cells.has_definite_block_size = true);
    }

    fn set_content_rect(
        &self,
        node: Node,
        cells: &Cell<used_values::RootInputCells>,
        rect: SvgCssPixelRect,
        object_bounding_box: FloatRect,
    ) -> SvgGeometry {
        Self::set_definite_content_size(cells, rect.width, rect.height);
        self.place_child(node, cells, rect.x, rect.y, object_bounding_box)
    }

    fn first_child_of_kind(&self, node: Node, kind: NodeKind) -> Option<Node> {
        self.run
            .pass
            .children(node)
            .find(|&child| self.node_kind(child) == kind)
    }

    /// Lays out the contents of this context's box, which is laid out in `cells`.
    fn lay_out_contents(
        &mut self,
        run: &FormattingContextRun<'pass>,
        input: LayoutInput,
        cells: &Cell<used_values::RootInputCells>,
    ) {
        // NOTE: SVG doesn't have a "formatting context" in the spec, but this is the most
        //       obvious way to drive SVG layout in our engine at the moment.
        let kind = self.node_kind(self.run.box_);
        let facts = self.svg_facts(self.run.box_);
        let attributes = self.svg_attributes(self.run.box_);
        self.commit_svg_element_facts(self.run.box_, facts, attributes);

        // NOTE: We consider all SVG root elements to have definite size in both axes.
        //       I'm not sure if this is good or bad, but our viewport transform logic depends on it.
        cells.update_cells(|cells| {
            cells.has_definite_inline_size = true;
            cells.has_definite_block_size = true;
        });
        if self.run.box_ == run.box_ {
            self.percentage_block_insets_of_run_root_children_have_definite_basis =
                run.percentage_block_insets_of_root_children_have_definite_basis(cells);
        }

        // An embedded SVG viewport's outer size does not depend on its descendants. Measuring it
        // only needs the root sizing above; shapes, text, and foreignObject content wait for layout.
        if self.run.fragments.is_none() && kind == NodeKind::SVGSVGBox && !facts.is_document_element {
            return;
        }

        // Viewport-establishing boxes publish their viewBox/preserveAspectRatio transform for the
        // visual context tree; content below lays out in the viewport's user units. The value is
        // published even without a viewBox so viewBox changes stay value-only for the tree. A
        // pattern's used size is its tile size, so the same computation maps its viewBox onto the
        // tile.
        let box_establishes_viewport = kind == NodeKind::SVGSVGBox
            || (kind_is_svg_graphics_box(kind) && attributes.is_fit_to_view_box)
            || (kind == NodeKind::SVGPatternBox && attributes.is_fit_to_view_box);

        let mut active_view_box = attributes.has_active_view_box.then_some(attributes.active_view_box);
        // https://svgwg.org/svg2-draft/coords.html#ViewBoxAttribute
        if let Some(view_box) = active_view_box {
            if view_box.width < 0.0 || view_box.height < 0.0 {
                // A negative value for <width> or <height> is an error and invalidates the ‘viewBox’ attribute.
                active_view_box = None;
            } else if view_box.width == 0.0 || view_box.height == 0.0 {
                // A value of zero disables rendering of the element.
                if box_establishes_viewport {
                    self.set_svg_viewport_transform(self.run.box_, FfiAffineTransform::default());
                }
                return;
            }
        }

        if box_establishes_viewport {
            // FIXME: This should allow just one of width or height to be specified.
            // E.g. We should be able to layout <svg width="100%"> where height is unspecified/auto.
            // The initial value for preserveAspectRatio is xMidYMid meet.
            let viewport_transform = active_view_box.map_or_else(FfiAffineTransform::default, |view_box| {
                view_box_transform(
                    attributes.preserve_aspect_ratio_align,
                    attributes.preserve_aspect_ratio_meet_or_slice,
                    view_box,
                    (cells.content_inline_size(), cells.content_block_size()),
                )
            });
            self.set_svg_viewport_transform(self.run.box_, viewport_transform);
        }

        self.viewport_width = active_view_box.map_or(cells.content_inline_size(), |view_box| {
            CssPixels::nearest_value_for(view_box.width)
        });
        self.viewport_height = active_view_box.map_or(cells.content_block_size(), |view_box| {
            CssPixels::nearest_value_for(view_box.height)
        });
        let viewport_user_rect = FloatRect::new(
            active_view_box.map_or(0.0, |view_box| view_box.min_x as f32),
            active_view_box.map_or(0.0, |view_box| view_box.min_y as f32),
            self.viewport_width.to_float(),
            self.viewport_height.to_float(),
        );
        self.viewport_user_rect = viewport_user_rect;
        self.viewport_is_committed_viewport = kind == NodeKind::SVGSVGBox
            && self.run.fragments.is_some()
            && crate::painting::paint_read::GeometryRead::with_committed_fragment_link(
                self.run.pass.arena(),
                self.run.box_,
                |link| {
                    link.and_then(|link| link.fragment.svg().map(|svg| svg.geometry))
                        .is_some_and(|committed| {
                            committed.establishes_viewport
                                && committed.established_viewport_user_rect == viewport_user_rect
                        })
                },
            );
        self.update_svg_payload_of(self.run.box_, |svg| {
            svg.geometry.establishes_viewport = true;
            svg.geometry.established_viewport_user_rect = viewport_user_rect;
        });
        self.available_space = Some(input.available_space);
        self.quirks_mode_percentage_basis_block_size = input
            .containing_block_constraints
            .quirks_mode_percentage_basis_block_size;

        let mut children_bounding_box = FloatRect::default();
        for child in self.run.pass.children(self.run.box_) {
            if NodeFacts::new(&self.run.pass, child).is_box()
                && let Some(geometry) = self.layout_svg_element(run, child)
                && !node_facts::kind_is_svg_resource_box(self.node_kind(child))
            {
                children_bounding_box = children_bounding_box.united(
                    libgfx_rust::AffineTransform::from(geometry.local_transform).map_rect(geometry.object_bounding_box),
                );
            }
        }
        // The run's root is an <svg> that its own formatting context does not place, and its bounding box is the union
        // of its children's in its own user units.
        if self.run.box_ == run.box_ {
            self.commit_svg_geometry(
                self.run.box_,
                SvgGeometry {
                    object_bounding_box: children_bounding_box,
                    stroke_bounding_box: children_bounding_box,
                    viewport_user_rect: FloatRect::new(
                        0.0,
                        0.0,
                        cells.content_inline_size().to_float(),
                        cells.content_block_size().to_float(),
                    ),
                    ..Default::default()
                },
            );
        }
    }

    /// Lays out and places `child`, and returns its geometry, if it laid it out.
    fn layout_svg_element(&mut self, run: &FormattingContextRun<'pass>, child: Node) -> Option<SvgGeometry> {
        let kind = self.node_kind(child);
        if self.svg_attributes(child).is_fit_to_view_box {
            return Some(self.layout_nested_viewport(run, child));
        } else if kind == NodeKind::SVGForeignObjectBox {
            // SVG descendants carry no percentage basis, as `cells_of_svg_box` explains.
            let mut child_cells =
                used_values::cells_before_sizing(&self.run.pass, child, ContainingBlockConstraints::default());
            let style = self.style(child);
            let rect = SvgCssPixelRect {
                x: style.x().to_px(self.viewport_width),
                y: style.y().to_px(self.viewport_height),
                width: style.width().to_px(self.viewport_width),
                height: style.height().to_px(self.viewport_height),
            };
            child_cells.set_content_inline_size(rect.width);
            child_cells.set_content_block_size(rect.height);

            let child_input = LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::definite(child_cells.content_inline_size),
                    block_size: AvailableSize::definite(child_cells.content_block_size),
                },
                ..self.nested_layout_input()
            };
            let laid_out = formatting_context::layout_inside_child_with_input_cells(
                run,
                None,
                None,
                child,
                child_input,
                child_cells,
                None,
                true,
                self.percentage_block_insets_of_run_root_children_have_definite_basis,
            );
            laid_out.report_percentage_block_size_dependency_of(run, child);
            assert!(
                laid_out.result().is_some(),
                "SVG foreign object did not create an independent formatting context"
            );
            // Masks and clips may use this offset for objectBoundingBox units.
            self.place_laid_out_child(
                child,
                laid_out.placed_child(),
                rect.x,
                rect.y,
                laid_out.into_fragment_arrival(run),
            );
            let rect = rect.to_float_rect();
            if let Some(mask) = self.first_child_of_kind(child, NodeKind::SVGMaskBox) {
                self.layout_mask_or_clip(run, mask, rect);
            }
            if let Some(clip) = self.first_child_of_kind(child, NodeKind::SVGClipBox) {
                self.layout_mask_or_clip(run, clip, rect);
            }
            // NB: The foreign object's fragment arrives from its own run, so its geometry goes to its parent alone.
            return Some(self.geometry_of(child, rect, rect));
        } else if kind_is_svg_graphics_box(kind) {
            return Some(self.layout_graphics_element(run, child));
        }
        None
    }

    fn layout_nested_viewport(&mut self, run: &FormattingContextRun<'pass>, viewport: Node) -> SvgGeometry {
        // Layout for a nested SVG viewport.
        // https://svgwg.org/svg2-draft/coords.html#EstablishingANewSVGViewport.
        let viewport_cells = self.cells_of_svg_box(viewport);
        let style = self.style(viewport);
        let nested_viewport_x = style.x().to_px(self.viewport_width);
        let nested_viewport_y = style.y().to_px(self.viewport_height);
        // The value auto for width and height on the ‘svg’ element is treated as 100%.
        // https://svgwg.org/svg2-draft/geometry.html#Sizing
        let nested_viewport_width = if style.width().is_auto() {
            self.viewport_width
        } else {
            style.width().to_px(self.viewport_width)
        };
        let nested_viewport_height = if style.height().is_auto() {
            self.viewport_height
        } else {
            style.height().to_px(self.viewport_height)
        };

        Self::set_definite_content_size(&viewport_cells, nested_viewport_width, nested_viewport_height);

        let mut nested_context = Self::new_nested(
            run,
            viewport,
            self.percentage_block_insets_of_run_root_children_have_definite_basis,
        );
        nested_context.lay_out_contents(run, self.nested_layout_input(), &viewport_cells);
        let viewport_rect = SvgCssPixelRect {
            x: nested_viewport_x,
            y: nested_viewport_y,
            width: nested_viewport_width,
            height: nested_viewport_height,
        };
        self.place_child(
            viewport,
            &viewport_cells,
            nested_viewport_x,
            nested_viewport_y,
            viewport_rect.to_float_rect(),
        )
    }

    fn layout_graphics_element(&mut self, run: &FormattingContextRun<'pass>, graphics_box: Node) -> SvgGeometry {
        let graphics_box_cells = self.cells_of_svg_box(graphics_box);
        if let Some(geometry) = self.place_unchanged_committed_shape(graphics_box, &graphics_box_cells) {
            return geometry;
        }
        let facts = self.svg_facts(graphics_box);
        let attributes = self.svg_attributes(graphics_box);
        self.commit_svg_element_facts(graphics_box, facts, attributes);
        let kind = self.node_kind(graphics_box);
        // NB: A mask box laid out as a group is the mask of the resource box it is a child of, whose object bounding
        //     box is the viewport that box gives its content.
        if kind == NodeKind::SVGMaskBox {
            self.commit_resource_facts_for_target(graphics_box, &attributes, self.viewport_user_rect);
        }

        // https://svgwg.org/svg2-draft/struct.html#GroupsOverview
        // container element
        // An element which can have graphics elements and other container elements as child elements.
        // Specifically: ‘a’, ‘clipPath’, ‘defs’, ‘g’, ‘marker’, ‘mask’, ‘pattern’, ‘svg’, ‘switch’ and ‘symbol’.
        let geometry = if kind_is_svg_container_element(kind) {
            // https://svgwg.org/svg2-draft/struct.html#Groups
            // 5.2. Grouping: the ‘g’ element
            // The ‘g’ element is a container element for grouping together related graphics elements.
            self.layout_container_element(run, graphics_box, &graphics_box_cells)
        } else if kind == NodeKind::SVGImageBox {
            self.layout_image_element(graphics_box, &graphics_box_cells)
        } else {
            // Assume this is a path-like element.
            self.layout_path_like_element(run, graphics_box, &graphics_box_cells, facts)
        };

        if let Some(mask) = self.first_child_of_kind(graphics_box, NodeKind::SVGMaskBox) {
            self.layout_mask_or_clip(run, mask, geometry.object_bounding_box);
        }
        if let Some(clip) = self.first_child_of_kind(graphics_box, NodeKind::SVGClipBox) {
            self.layout_mask_or_clip(run, clip, geometry.object_bounding_box);
        }
        for child in self.run.pass.children(graphics_box) {
            if self.node_kind(child) == NodeKind::SVGPatternBox {
                self.layout_mask_or_clip(run, child, geometry.object_bounding_box);
            }
        }
        geometry
    }

    /// Places a shape with the fragment it last committed, where laying it out again would produce that fragment: the
    /// shape has no boxes below it, nothing marked it for layout since that commit, and it resolves against the viewport
    /// it was committed in. Then neither this pass nor the commit redoes a shape that a change to a sibling did not
    /// touch. Returns the geometry of the shape, if it placed the shape, which is laid out in `cells`.
    fn place_unchanged_committed_shape(
        &self,
        shape: Node,
        cells: &Cell<used_values::RootInputCells>,
    ) -> Option<SvgGeometry> {
        if !self.viewport_is_committed_viewport
            || self.node_kind(shape) != NodeKind::SVGGeometryBox
            || !self.first_child(shape).is_invalid()
        {
            return None;
        }
        // A line's coordinates are attribute lengths resolved here against the initial containing block, the root font
        // and size query containers, and a change to any of those does not mark the line for layout.
        if self.svg_attributes(shape).geometry_kind == SVG_GEOMETRY_KIND_LINE {
            return None;
        }
        self.run.fragments.as_ref()?;
        // A committed fragment stops being current when something marks its box for layout.
        let link = self
            .run
            .pass
            .arena()
            .with_current_committed_fragment_link(shape, |link| link.clone())?;
        let fragment = link.fragment.clone();
        fragment.svg()?.path.as_ref()?;
        let committed_offset = link.offset_in_frame(
            self.run.pass.frame,
            fragment_tree::committed_content_size_of(&self.run.pass, link.placement.containing_block),
        );
        // The transform and the viewport are what the shape's geometry takes from outside its own boxes.
        let committed_geometry = fragment.svg()?.geometry;
        let geometry = self.geometry_of(
            shape,
            committed_geometry.object_bounding_box,
            committed_geometry.stroke_bounding_box,
        );
        if geometry != committed_geometry {
            return None;
        }
        cells.update_cells(|cells| {
            cells.take_box_metrics_of_committed_link(&link, self.run.pass.frame);
            cells.has_definite_block_size = true;
        });
        self.place_laid_out_child(
            shape,
            formatting_context::PlacedChild::laid_out_here(cells.get(), None, &self.run),
            committed_offset.x,
            committed_offset.y,
            Some(fragment_tree::RunRootFragment::propagating_nothing(fragment)),
        );
        Some(geometry)
    }

    fn layout_path_like_element(
        &mut self,
        run: &FormattingContextRun<'pass>,
        graphics_box: Node,
        cells: &Cell<used_values::RootInputCells>,
        facts: SvgElementFacts,
    ) -> SvgGeometry {
        // A shape's geometry is its own attributes and computed style against the viewport, and
        // text is its own character data shaped with its own font cascade, so the pass draws both.
        let kind = self.node_kind(graphics_box);
        let mut text_segments = None;
        let mut text_children_bounding_box = SvgCssPixelRect::default();
        let mut own_character_cells = None;
        let (path, glyph_cells, character_cells) = match kind {
            NodeKind::SVGGeometryBox => (self.svg_geometry_path(graphics_box), None, None),
            NodeKind::SVGTextBox if self.text_on_path_glyphs.borrow().contains_key(&graphics_box) => {
                let (path, cells) = self
                    .text_on_path_glyphs
                    .borrow_mut()
                    .remove(&graphics_box)
                    .expect("the element's glyphs on the path");
                text_children_bounding_box = self.layout_text_content_children_on_path(run, graphics_box);
                (path, None, Some(cells))
            }
            NodeKind::SVGTextBox => {
                let contents = self.layout_svg_text_box_contents(run, graphics_box);
                text_segments = contents.segments;
                text_children_bounding_box = contents.children_bounding_box;
                own_character_cells = Some(std::sync::Arc::new(contents.own_character_cells));
                (
                    contents.path,
                    Some(contents.glyph_cells),
                    Some(contents.character_cells),
                )
            }
            NodeKind::SVGTextPathBox => {
                let (path, character_cells, segments) = self.svg_text_path_box_path(graphics_box);
                text_segments = segments;
                text_children_bounding_box = self.layout_text_content_children_on_path(run, graphics_box);
                (path, None, Some(character_cells))
            }
            _ => (libgfx_rust::path::Path::new(), None, None),
        };

        // https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
        // Let fill-shape be the equivalent path of element if it is a shape, or a shape that includes each of the glyph
        // cells corresponding to the text within the elements otherwise.
        // NB: A text content element's box is its bounding box: The box getBBox() and getBoundingClientRect() report.
        let is_text_content_box = matches!(kind, NodeKind::SVGTextBox | NodeKind::SVGTextPathBox);
        let path_bounding_box = FloatRect::from_array(path.bounding_box());
        let mut bounding_box = float_rect_to_css_pixels(path_bounding_box);
        // AD-HOC: The spec's fill-shape for text is just its glyph cells. We follow what other engines do: Cover the
        //         glyph outlines, so a glyph that overflows its cell (an italic's overhang, e.g.) stays within the box.
        // FIXME: A <textPath>'s box covers its glyph outlines, not the glyph cells laid along its path.
        if let Some(glyph_cells) = glyph_cells {
            bounding_box.unite(glyph_cells);
        }
        if !is_text_content_box {
            // Stroke increases the path's size by stroke_width/2 per side.
            let stroke_width = CssPixels::nearest_value_for_f32(facts.visible_stroke_width);
            bounding_box.inflate(stroke_width, stroke_width);
        }

        bounding_box.unite(text_children_bounding_box);

        let character_cells = character_cells.map(std::sync::Arc::new);
        self.last_text_subtree_cells = character_cells.clone();
        self.update_svg_payload_of(graphics_box, |svg| {
            svg.path = Some(std::sync::Arc::new(fragment_tree::CommittedSvgPath::new(path)));
            svg.text_segments = text_segments.map(std::sync::Arc::new);
            svg.text_character_cells = character_cells;
            svg.text_own_character_cells = own_character_cells;
        });
        let object_bounding_box = if is_text_content_box {
            bounding_box.to_float_rect()
        } else {
            path_bounding_box
        };
        self.set_content_rect(graphics_box, cells, bounding_box, object_bounding_box)
    }

    /// Lays out the text content elements inside a <textPath>, or inside one of those, each with the glyphs its own text
    /// has on the path, and returns the box they cover.
    fn layout_text_content_children_on_path(
        &mut self,
        run: &FormattingContextRun<'pass>,
        element: Node,
    ) -> SvgCssPixelRect {
        let mut bounding_box = SvgCssPixelRect::default();
        let children: Vec<Node> = self.run.pass.children(element).collect();
        for child in children {
            if self.node_kind(child) != NodeKind::SVGTextBox {
                continue;
            }
            // NB: An element with no text on the path, such as one in a <textPath> whose path doesn't resolve, renders
            //     nothing, rather than its text laid out as if it weren't on a path.
            self.text_on_path_glyphs
                .borrow_mut()
                .entry(child)
                .or_insert_with(|| (libgfx_rust::path::Path::new(), Vec::new()));
            let geometry = self.layout_graphics_element(run, child);
            bounding_box.unite(float_rect_to_css_pixels(geometry.stroke_bounding_box));
        }
        bounding_box
    }

    // https://w3c.github.io/svgwg/svg2-draft/embedded.html#Placement
    // Computation of automatically-sized values follows the Default Sizing Algorithm defined for
    // replaced elements in CSS layout [css-images-3]. In particular, when the referenced resource
    // does not have an intrinsic size (such as image types with no defined dimensions), it is
    // assumed to have a width of 300px and a height of 150px.
    fn svg_image_bounding_box(&self, image_box: Node) -> SvgCssPixelRect {
        use crate::layout::replaced_sizing::{Fraction, SizeWithAspectRatio, run_default_sizing_algorithm};
        let style = self.style(image_box);
        let specified_width = style
            .width()
            .is_length_percentage()
            .then(|| style.width().to_px(self.viewport_width));
        let specified_height = style
            .height()
            .is_length_percentage()
            .then(|| style.height().to_px(self.viewport_height));

        let facts = self
            .run
            .pass
            .arena()
            .replaced_content_facts(image_box)
            .unwrap_or_default();
        let natural = SizeWithAspectRatio {
            width: facts.auto_content_size.width,
            height: facts.auto_content_size.height,
            aspect_ratio: facts
                .auto_content_size
                .aspect_ratio
                .map(|ratio| Fraction::of(ratio.numerator, ratio.denominator)),
        };
        // The default object size applies only once something has decoded; until then the image
        // has no size at all.
        let default_size = facts.svg_image_default_object_size;

        let sizing = run_default_sizing_algorithm(specified_width, specified_height, &natural, default_size);
        SvgCssPixelRect {
            x: style.x().to_px(self.viewport_width),
            y: style.y().to_px(self.viewport_height),
            width: sizing.width,
            height: sizing.height,
        }
    }

    fn layout_image_element(&self, image_box: Node, cells: &Cell<used_values::RootInputCells>) -> SvgGeometry {
        let bounding_box = self.svg_image_bounding_box(image_box);
        self.set_content_rect(image_box, cells, bounding_box, bounding_box.to_float_rect())
    }

    /// Lays out `resource`, a mask, clip or pattern of an element whose object bounding box is `target_bounding_box`.
    fn layout_mask_or_clip(
        &mut self,
        run: &FormattingContextRun<'pass>,
        resource: Node,
        target_bounding_box: FloatRect,
    ) {
        let kind = self.node_kind(resource);
        let facts = self.svg_facts(resource);
        let attributes = self.svg_attributes(resource);
        assert!(node_facts::kind_is_svg_resource_box(kind));
        // FIXME: Somehow limit <clipPath> contents to: shape elements, <text>, and <use>.
        let resource_cells = self.cells_of_svg_box(resource);
        self.commit_svg_element_facts(resource, facts, attributes);
        self.commit_resource_facts_for_target(resource, &attributes, target_bounding_box);

        let (width, height) = if kind == NodeKind::SVGPatternBox && attributes.has_active_view_box {
            if attributes.pattern_units == SVG_UNITS_USER_SPACE_ON_USE {
                (
                    CssPixels::nearest_value_for_f32(
                        attributes
                            .pattern_width
                            .resolve_relative_to(self.viewport_width.to_float()),
                    ),
                    CssPixels::nearest_value_for_f32(
                        attributes
                            .pattern_height
                            .resolve_relative_to(self.viewport_height.to_float()),
                    ),
                )
            } else {
                let parent = self.parent(resource);
                assert!(!parent.is_invalid());
                (
                    CssPixels::nearest_value_for_f32(attributes.pattern_width.value * target_bounding_box.width),
                    CssPixels::nearest_value_for_f32(attributes.pattern_height.value * target_bounding_box.height),
                )
            }
        } else if attributes.content_units == SVG_UNITS_OBJECT_BOUNDING_BOX {
            let parent = self.parent(resource);
            assert!(!parent.is_invalid());
            (
                CssPixels::nearest_value_for_f32(target_bounding_box.width),
                CssPixels::nearest_value_for_f32(target_bounding_box.height),
            )
        } else {
            (self.viewport_width, self.viewport_height)
        };
        Self::set_definite_content_size(&resource_cells, width, height);
        // Resource content lays out in its own user space; objectBoundingBox content scaling is a
        // paint-time transform in the resource's nested visual context tree.
        let mut nested_context = Self::new_nested(
            run,
            resource,
            self.percentage_block_insets_of_run_root_children_have_definite_basis,
        );
        nested_context.lay_out_contents(run, self.nested_layout_input(), &resource_cells);
        let resource_rect = FloatRect::new(0.0, 0.0, width.to_float(), height.to_float());
        self.place_child(
            resource,
            &resource_cells,
            CssPixels::default(),
            CssPixels::default(),
            resource_rect,
        );
    }

    fn layout_container_element(
        &mut self,
        run: &FormattingContextRun<'pass>,
        container: Node,
        cells: &Cell<used_values::RootInputCells>,
    ) -> SvgGeometry {
        let mut object_bounding_box = None;
        let mut stroke_bounding_box = None;
        for child in self.run.pass.children(container) {
            // Masks/clips/patterns do not change the bounding box of their parents.
            if NodeFacts::new(&self.run.pass, child).is_box()
                && !node_facts::kind_is_svg_resource_box(self.node_kind(child))
            {
                let child_geometry = self
                    .layout_svg_element(run, child)
                    .expect("a box below an SVG container element is laid out");
                // The container's bounding boxes include descendants' transforms; children lay out
                // untransformed, so each child's boxes map through the child's own transform here.
                let local_transform = libgfx_rust::AffineTransform::from(child_geometry.local_transform);
                object_bounding_box = Some(united_even_if_empty(
                    object_bounding_box,
                    local_transform.map_rect(child_geometry.object_bounding_box),
                ));
                stroke_bounding_box = Some(united_even_if_empty(
                    stroke_bounding_box,
                    float_rect_to_css_pixels(local_transform.map_rect(child_geometry.stroke_bounding_box))
                        .to_float_rect(),
                ));
            }
        }
        self.set_content_rect(
            container,
            cells,
            float_rect_to_css_pixels(stroke_bounding_box.unwrap_or_default()),
            object_bounding_box.unwrap_or_default(),
        )
    }
}

fn font_metrics_for_length_resolution(style: StyleValues<'_>) -> crate::css::style_compute::FfiFontMetrics {
    let font = style.font().first_available_font_metrics();
    crate::css::style_compute::FfiFontMetrics {
        font_size: style.font().font_size_for_lengths(),
        x_height: CssPixels::nearest_value_for_f32(font.x_height).to_double(),
        cap_height: CssPixels::nearest_value_for_f32(font.cap_height).to_double(),
        zero_advance: CssPixels::nearest_value_for_f32(font.zero_advance).to_double(),
        line_height: style.line_height().to_double(),
    }
}

/// What a length one of an SVG element's attributes gives resolves against: the element's own style,
/// the document element's, and the viewport.
fn length_resolution_context(
    pass: &LayoutPass<'_>,
    node: Node,
    unit: u8,
) -> crate::css::style_compute::FfiLengthResolutionContext {
    let style = StyleValues::for_node(pass, node);
    let root_style = document_element_style(pass, node).unwrap_or(style);
    let viewport_width = pass.initial_containing_block_inline_size.to_double();
    let viewport_height = pass.initial_containing_block_block_size.to_double();
    // A container unit resolves against the committed content box of the nearest size query container on its axis, or
    // the viewport without one. Finding that container is style's business, so it is asked only for a length that
    // needs it, and the host hears which containers the lengths depend on once the layout is committed.
    let container_bases = crate::css::style_compute::length_unit_is_container_relative(unit).then(|| {
        let element = pass
            .arena()
            .dom_node_style_node(node)
            .expect("an SVG element's attribute length is resolved for the element's own box");
        let bases = pass
            .arena()
            .with_style_store(|engine| engine.container_unit_bases(element, false));
        pass.note_host_findings(|findings| findings.note_container_length_dependencies(node, element, &bases));
        bases
    });
    let mut context = crate::css::style_compute::FfiLengthResolutionContext {
        viewport_width,
        viewport_height,
        font_metrics: font_metrics_for_length_resolution(style),
        root_font_metrics: font_metrics_for_length_resolution(root_style),
        // Only the out-flag below consumes these, and this resolution reports no dependency.
        font_metrics_depend_on_viewport_metrics: false,
        root_font_metrics_depend_on_viewport_metrics: false,
        has_container_width_basis: false,
        has_container_height_basis: false,
        container_width_basis: 0.0,
        container_height_basis: 0.0,
        container_width_basis_depends_on_viewport_metrics: false,
        container_height_basis_depends_on_viewport_metrics: false,
        subject_inline_axis_is_horizontal: style.writing_mode() == writing_mode::HORIZONTAL_TB,
        resolved_viewport_relative_length: std::ptr::null_mut(),
        zoom: 1.0,
    };
    if let Some(bases) = container_bases {
        bases.apply_to(&mut context);
    }
    context
}

/// The style of the document element, which a root-relative length resolves against.
fn document_element_style<'pass>(pass: &LayoutPass<'pass>, node: Node) -> Option<StyleValues<'pass>> {
    let mut ancestor = node;
    while !ancestor.is_invalid() {
        let data = pass.node_data(ancestor);
        if node_facts::has_flag(data, NodeFlag::IsDocumentElement) {
            return pass.arena().style_view(ancestor).map(StyleValues::new);
        }
        ancestor = data.parent.get();
    }
    None
}

/// The pixels an absolute or relative `<length>` an SVG element's attribute gives resolves to.
fn resolve_svg_length(pass: &LayoutPass<'_>, node: Node, length: FfiSvgLengthValue) -> CssPixels {
    let context = length_resolution_context(pass, node, length.unit);
    let absolutized =
        crate::css::style_compute::absolutize_length_for_calc(length.value, length.unit as usize, &context);
    CssPixels::nearest_value_for(absolutized.px)
}

// https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
/// The natural size of an `<svg>` root's box, negotiated from what its element published.
pub(crate) fn svg_root_natural_size(
    pass: &LayoutPass<'_>,
    node: Node,
) -> (Option<CssPixels>, Option<CssPixels>, Option<(CssPixels, CssPixels)>) {
    let attributes = pass.arena().svg_attribute_facts(node);
    // The intrinsic dimensions must also be determined from the width and height sizing properties. If either width or
    // height are not specified, the used value is the initial value 'auto'. 'auto' and percentage lengths must not be
    // used to determine an intrinsic width or intrinsic height.
    let resolve = |length: FfiSvgLengthValue| {
        (length.kind == SVG_LENGTH_KIND_LENGTH).then(|| resolve_svg_length(pass, node, length))
    };
    let width = resolve(attributes.natural_width);
    let height = resolve(attributes.natural_height);
    let view_box_aspect_ratio = attributes.has_view_box_aspect_ratio.then_some((
        attributes.view_box_aspect_ratio_numerator,
        attributes.view_box_aspect_ratio_denominator,
    ));
    (
        width,
        height,
        svg_root_natural_aspect_ratio(width, height, view_box_aspect_ratio),
    )
}

/// Overwrites the content size of the document element's `<svg>`, which the layout entry sized as the viewport in
/// `cells`, with its styled width and height (from <svg width height ...>) where they are lengths. A decoded SVG image
/// keeps the size of the viewport it is drawn into.
pub(crate) fn size_document_element_svg_from_its_width_and_height(
    pass: &LayoutPass<'_>,
    svg: Node,
    cells: &mut used_values::RootInputCells,
) {
    if !node_facts::has_flag(pass.node_data(svg), NodeFlag::IsDocumentElement) || pass.arena().document_is_decoded_svg()
    {
        return;
    }
    // NOTE: If a height had not been provided by the svg element, it was set to the height of the container
    let style = StyleValues::for_node(pass, svg);
    if style.width().is_length() {
        cells.set_content_inline_size(style.width().to_px(CssPixels::default()));
    }
    if style.height().is_length() {
        cells.set_content_block_size(style.height().to_px(CssPixels::default()));
    }
    // FIXME: In SVG 2, length can also be a percentage. We'll need to support that.
}

/// The intrinsic aspect ratio must be calculated using the following algorithm. If the algorithm returns null, then
/// there is no intrinsic aspect ratio.
fn svg_root_natural_aspect_ratio(
    width: Option<CssPixels>,
    height: Option<CssPixels>,
    view_box_aspect_ratio: Option<(CssPixels, CssPixels)>,
) -> Option<(CssPixels, CssPixels)> {
    match (width, height) {
        // 1. If the width and height sizing properties on the ‘svg’ element are both absolute values: return width /
        //    height.
        (Some(width), Some(height)) => {
            (width != CssPixels::default() && height != CssPixels::default()).then_some((width, height))
        }
        // 2.-4. What the active SVG view or the viewBox gives it, if anything.
        _ => view_box_aspect_ratio,
    }
}

#[cfg(test)]
mod svg_root_natural_size_tests {
    use super::svg_root_natural_aspect_ratio;
    use crate::layout::CssPixels;

    #[test]
    fn an_svg_root_takes_its_aspect_ratio_from_its_size_before_its_view_box() {
        let px = CssPixels::from_integer;
        let view_box = Some((px(4), px(3)));
        assert_eq!(
            svg_root_natural_aspect_ratio(Some(px(200)), Some(px(100)), view_box),
            Some((px(200), px(100)))
        );
        // A zero width or height gives no ratio, and the view box is not consulted.
        assert_eq!(
            svg_root_natural_aspect_ratio(Some(px(0)), Some(px(100)), view_box),
            None
        );
        assert_eq!(svg_root_natural_aspect_ratio(Some(px(200)), None, view_box), view_box);
        assert_eq!(svg_root_natural_aspect_ratio(None, None, None), None);
    }
}
