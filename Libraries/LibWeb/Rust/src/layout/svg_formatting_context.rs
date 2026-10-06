/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::style::tree::StyleNodeID;

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
    /// Whether this code unit is the first, in document order, of the typographic character whose
    /// cell this is. A later code unit of the same character carries the same cell, unmarked.
    pub starts_typographic_character: bool,
}

/// What shaping one SVG text run yields: its glyphs, its total advance, the union of its glyph
/// cells, and the glyph cell of every one of its code units.
struct ShapedSvgText {
    runs: Vec<libgfx_rust::path::GlyphRun>,
    advance: f32,
    glyph_cells: SvgCssPixelRect,
    character_cells: Vec<FfiSvgTextCharacterCell>,
}

/// Appends the glyph cell of each code unit of one shaped run, in code unit order. A typographic
/// character's cell spans its advance; every code unit of it carries that cell, and the first one
/// is marked as the one that starts it.
/// https://svgwg.org/svg2-draft/text.html#TermTypographicCharacterUnit
/// A unit of a writing system - such as a Latin alphabetic letter (including its diacritics),
/// Hangul syllable, Chinese ideographic character, Myanmar syllable cluster - that is indivisible
/// with respect to a particular typographic operation [...]
/// FIXME: The glyphs are taken to be in code unit order, which holds for the left-to-right text
///        this layout shapes.
fn append_svg_text_character_cells(
    cells: &mut Vec<FfiSvgTextCharacterCell>,
    glyphs: &[libgfx_rust::text_layout::DrawGlyph],
    length_in_code_units: usize,
    run_end_x: f32,
    top: f32,
    height: f32,
) {
    let mut covered = 0usize;
    let mut index = 0usize;
    while index < glyphs.len() && covered < length_in_code_units {
        let first = glyphs[index];
        // A glyph that maps to no code unit of its own (a combining mark's, e.g.) belongs to the
        // typographic character before it, so it extends that character's cell. One at the start
        // of the run has no character before it, so it joins the character after it instead.
        let mut next = index + 1;
        let mut code_units = first.length_in_code_units;
        while next < glyphs.len() && (glyphs[next].length_in_code_units == 0 || code_units == 0) {
            code_units += glyphs[next].length_in_code_units;
            next += 1;
        }
        let end_x = glyphs.get(next).map_or(run_end_x, |glyph| glyph.x);
        let code_units = code_units.max(1).min(length_in_code_units - covered);
        let cell = FfiSvgTextCharacterCell {
            x: first.x,
            y: top,
            width: end_x - first.x,
            height,
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

/// The sizes a container-relative length on an element resolves against, as style answers them
/// for that element: per axis, the committed content box of its nearest size query container, or
/// the small viewport size.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiContainerLengthBases {
    pub width: f64,
    pub height: f64,
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
    element_transform: FfiAffineTransform,
    additional_element_transform: FfiAffineTransform,
    visible_stroke_width: f32,
    viewport_percentage_basis: CssPixels,
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
    pub is_svg_svg_element: bool,
    pub is_symbol_element: bool,
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
    /// The text positioning attributes of a `<text>` or `<tspan>`.
    /// FIXME: These only carry a single value each, not a list.
    pub text_x: FfiSvgLengthValue,
    pub text_y: FfiSvgLengthValue,
    pub text_dx: FfiSvgLengthValue,
    pub text_dy: FfiSvgLengthValue,
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

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct SvgMaskAreaFacts {
    pub units_are_object_bounding_box: bool,
    pub x: FfiSvgNumberPercentage,
    pub y: FfiSvgNumberPercentage,
    pub width: FfiSvgNumberPercentage,
    pub height: FfiSvgNumberPercentage,
}

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
const PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN: u8 = 1;
const PRESERVE_ASPECT_RATIO_X_MID_Y_MIN: u8 = 2;
const PRESERVE_ASPECT_RATIO_X_MAX_Y_MIN: u8 = 3;
const PRESERVE_ASPECT_RATIO_X_MIN_Y_MID: u8 = 4;
pub(crate) const PRESERVE_ASPECT_RATIO_X_MID_Y_MID: u8 = 5;
const PRESERVE_ASPECT_RATIO_X_MAX_Y_MID: u8 = 6;
const PRESERVE_ASPECT_RATIO_X_MIN_Y_MAX: u8 = 7;
pub(crate) const PRESERVE_ASPECT_RATIO_X_MID_Y_MAX: u8 = 8;
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

    pub(crate) fn map_rect(self, rect: FfiFloatRect) -> FfiFloatRect {
        let rect = libgfx_rust::AffineTransform::from(self).map_rect(libgfx_rust::FloatRect::new(
            rect.x,
            rect.y,
            rect.width,
            rect.height,
        ));
        FfiFloatRect {
            x: rect.x,
            y: rect.y,
            width: rect.width,
            height: rect.height,
        }
    }
}

fn float_rect_to_css_pixels(rect: FfiFloatRect) -> SvgCssPixelRect {
    SvgCssPixelRect {
        x: CssPixels::nearest_value_for_f32(rect.x),
        y: CssPixels::nearest_value_for_f32(rect.y),
        width: CssPixels::nearest_value_for_f32(rect.width),
        height: CssPixels::nearest_value_for_f32(rect.height),
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct ViewBoxTransform {
    pub(crate) offset: FfiCssPixelPoint,
    pub(crate) scale_factor_x: f64,
    pub(crate) scale_factor_y: f64,
}

// https://svgwg.org/svg2-draft/coords.html#PreserveAspectRatioAttribute
pub(crate) fn scale_and_align_viewbox_content(
    align: u8,
    meet_or_slice: u8,
    view_box: FfiSvgViewBox,
    viewbox_scale_x: f32,
    viewbox_scale_y: f32,
    content_size: (CssPixels, CssPixels),
    has_definite_inline_size: bool,
) -> ViewBoxTransform {
    let viewbox_scale_x = f64::from(viewbox_scale_x);
    let viewbox_scale_y = f64::from(viewbox_scale_y);
    if align == PRESERVE_ASPECT_RATIO_NONE {
        // Do not force uniform scaling. Scale the graphic content of the given element non-uniformly
        // if necessary such that the element's bounding box exactly matches the SVG viewport rectangle.
        return ViewBoxTransform {
            scale_factor_x: viewbox_scale_x,
            scale_factor_y: viewbox_scale_y,
            ..Default::default()
        };
    }

    let scale = match meet_or_slice {
        // meet (the default) - Scale the graphic such that:
        // - aspect ratio is preserved
        // - the entire ‘viewBox’ is visible within the SVG viewport
        // - the ‘viewBox’ is scaled up as much as possible, while still meeting the other criteria
        MEET_OR_SLICE_MEET => viewbox_scale_x.min(viewbox_scale_y),
        // slice - Scale the graphic such that:
        // aspect ratio is preserved
        // the entire SVG viewport is covered by the ‘viewBox’
        // the ‘viewBox’ is scaled down as much as possible, while still meeting the other criteria
        MEET_OR_SLICE_SLICE => viewbox_scale_x.max(viewbox_scale_y),
        _ => panic!("invalid preserveAspectRatio meet-or-slice value"),
    };
    let mut result = ViewBoxTransform {
        scale_factor_x: scale,
        scale_factor_y: scale,
        ..Default::default()
    };

    // Handle X alignment:
    if has_definite_inline_size {
        match align {
            // Align the <min-x> of the element's ‘viewBox’ with the smallest X value of the SVG viewport.
            PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN
            | PRESERVE_ASPECT_RATIO_X_MIN_Y_MID
            | PRESERVE_ASPECT_RATIO_X_MIN_Y_MAX => {}
            // Align the midpoint X value of the element's ‘viewBox’ with the midpoint X value of the SVG viewport.
            PRESERVE_ASPECT_RATIO_X_MID_Y_MIN
            | PRESERVE_ASPECT_RATIO_X_MID_Y_MID
            | PRESERVE_ASPECT_RATIO_X_MID_Y_MAX => {
                result.offset.x =
                    (content_size.0 - CssPixels::nearest_value_for(view_box.width * result.scale_factor_x)) / 2;
            }
            // Align the <min-x>+<width> of the element's ‘viewBox’ with the maximum X value of the SVG viewport.
            PRESERVE_ASPECT_RATIO_X_MAX_Y_MIN
            | PRESERVE_ASPECT_RATIO_X_MAX_Y_MID
            | PRESERVE_ASPECT_RATIO_X_MAX_Y_MAX => {
                result.offset.x = content_size.0 - CssPixels::nearest_value_for(view_box.width * result.scale_factor_x);
            }
            _ => panic!("invalid preserveAspectRatio alignment value"),
        }
    }

    // This intentionally checks inline-size definiteness, matching the C++
    // algorithm being ported.
    if has_definite_inline_size {
        match align {
            // Align the <min-y> of the element's ‘viewBox’ with the smallest Y value of the SVG viewport.
            PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN
            | PRESERVE_ASPECT_RATIO_X_MID_Y_MIN
            | PRESERVE_ASPECT_RATIO_X_MAX_Y_MIN => {}
            // Align the midpoint Y value of the element's ‘viewBox’ with the midpoint Y value of the SVG viewport.
            PRESERVE_ASPECT_RATIO_X_MIN_Y_MID
            | PRESERVE_ASPECT_RATIO_X_MID_Y_MID
            | PRESERVE_ASPECT_RATIO_X_MAX_Y_MID => {
                result.offset.y =
                    (content_size.1 - CssPixels::nearest_value_for(view_box.height * result.scale_factor_y)) / 2;
            }
            // Align the <min-y>+<height> of the element's ‘viewBox’ with the maximum Y value of the SVG viewport.
            PRESERVE_ASPECT_RATIO_X_MIN_Y_MAX
            | PRESERVE_ASPECT_RATIO_X_MID_Y_MAX
            | PRESERVE_ASPECT_RATIO_X_MAX_Y_MAX => {
                result.offset.y =
                    content_size.1 - CssPixels::nearest_value_for(view_box.height * result.scale_factor_y);
            }
            _ => panic!("invalid preserveAspectRatio alignment value"),
        }
    }

    result
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct SvgTextChunkMeasurement {
    advance: f32,
    anchor: u8,
}

impl Default for SvgTextChunkMeasurement {
    fn default() -> Self {
        Self {
            advance: 0.0,
            anchor: text_anchor::START,
        }
    }
}

/// Strips the ASCII whitespace AK's Utf16String::trim_ascii_whitespace() strips from both ends.
fn trim_ascii_whitespace(text: &mut Vec<u16>) {
    let is_whitespace = |unit: &u16| matches!(unit, 0x09..=0x0d | 0x20);
    let end = text
        .iter()
        .rposition(|unit| !is_whitespace(unit))
        .map_or(0, |last| last + 1);
    text.truncate(end);
    let start = text.iter().position(|unit| !is_whitespace(unit)).unwrap_or(end);
    text.drain(..start);
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
    current_text_position: FfiFloatPoint,
}

impl<'pass> SvgFormattingContext<'pass> {
    pub(super) fn new(run: &FormattingContextRun<'pass>) -> Self {
        Self::new_nested(run, run.box_)
    }

    fn new_nested(run: &FormattingContextRun<'pass>, box_: Node) -> Self {
        Self {
            run: FormattingContextRun {
                box_,
                previous_line_data: None,
                ..run.clone()
            },
            available_space: None,
            quirks_mode_percentage_basis_block_size: None,
            viewport_width: CssPixels::default(),
            viewport_height: CssPixels::default(),
            current_text_position: FfiFloatPoint::default(),
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
        self.run.callbacks.node_data(node).first_child.get()
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.run.callbacks.node_data(node).next_sibling.get()
    }

    fn parent(&self, node: Node) -> Node {
        self.run.callbacks.node_data(node).parent.get()
    }

    fn node_kind(&self, node: Node) -> NodeKind {
        self.run.callbacks.node_data(node).kind.get()
    }

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

    fn svg_rect_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::PathBuilder {
        let mut path = libgfx_rust::path::PathBuilder::new();
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
        let corner = (rx > 0.0) && (ry > 0.0);
        let x_axis_rotation = 0.0;
        let large_arc_flag = false;
        let sweep_flag = true;

        // 1. perform an absolute moveto operation to location (x+rx,y);
        path.move_to((x + rx as f64) as f32, y as f32);
        // 2. perform an absolute horizontal lineto with parameter x+width-rx;
        path.line_to((x + width - rx as f64) as f32, y as f32);
        // 3. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    to coordinate (x+width,y+ry), where rx and ry are used as the equivalent parameters
        //    to the elliptical arc command, the x-axis-rotation and large-arc-flag are set to
        //    zero, the sweep-flag is set to one;
        if corner {
            path.elliptical_arc_to(
                (x + width) as f32,
                (y + ry as f64) as f32,
                rx,
                ry,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        // 4. perform an absolute vertical lineto parameter y+height-ry;
        path.line_to((x + width) as f32, (y + height - ry as f64) as f32);
        // 5. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    to coordinate (x+width-rx,y+height), using the same parameters as previously;
        if corner {
            path.elliptical_arc_to(
                (x + width - rx as f64) as f32,
                (y + height) as f32,
                rx,
                ry,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        // 6. perform an absolute horizontal lineto parameter x+rx;
        path.line_to((x + rx as f64) as f32, (y + height) as f32);
        // 7. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    to coordinate (x,y+height-ry), using the same parameters as previously;
        if corner {
            path.elliptical_arc_to(
                x as f32,
                (y + height - ry as f64) as f32,
                rx,
                ry,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        // 8. perform an absolute vertical lineto parameter y+ry
        path.line_to(x as f32, (y + ry as f64) as f32);
        // 9. if both rx and ry are greater than zero, perform an absolute elliptical arc operation
        //    with a segment-completing close path operation, using the same parameters as
        //    previously.
        if corner {
            path.elliptical_arc_to(
                (x + rx as f64) as f32,
                y as f32,
                rx,
                ry,
                x_axis_rotation,
                large_arc_flag,
                sweep_flag,
            );
        }
        path.close();
        path
    }

    fn svg_circle_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::PathBuilder {
        let mut path = libgfx_rust::path::PathBuilder::new();
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
        let r = to_px(&svg_reset.r, self.normalized_diagonal_length()).to_float();

        // A zero radius disables rendering.
        if r == 0.0 {
            return path;
        }
        let large_arc = false;
        let sweep = true;
        // 1. A move-to command to the point cx+r,cy;
        path.move_to(cx + r, cy);
        // 2. arc to cx,cy+r;
        path.arc_to(cx, cy + r, r, large_arc, sweep);
        // 3. arc to cx-r,cy;
        path.arc_to(cx - r, cy, r, large_arc, sweep);
        // 4. arc to cx,cy-r;
        path.arc_to(cx, cy - r, r, large_arc, sweep);
        // 5. arc with a segment-completing close path operation.
        path.arc_to(cx + r, cy, r, large_arc, sweep);
        path
    }

    fn normalized_diagonal_length(&self) -> CssPixels {
        if self.viewport_width == self.viewport_height {
            return self.viewport_width;
        }
        let sum_of_squares = self.viewport_width * self.viewport_width + self.viewport_height * self.viewport_height;
        CssPixels::nearest_value_for_f32(
            sum_of_squares
                .div_as_fraction(CssPixels::from_integer(2))
                .to_float()
                .sqrt(),
        )
    }

    fn svg_ellipse_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::PathBuilder {
        let mut path = libgfx_rust::path::PathBuilder::new();
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

        let x_axis_rotation = 0.0;
        let large_arc = false;
        // NB: Spec says sweep should be false, but it's wrong. https://github.com/w3c/svgwg/issues/765
        let sweep = true;
        // 1. A move-to command to the point cx+rx,cy;
        path.move_to(cx + rx, cy);
        // 2. arc to cx,cy+ry;
        path.elliptical_arc_to(cx, cy + ry, rx, ry, x_axis_rotation, large_arc, sweep);
        // 3. arc to cx-rx,cy;
        path.elliptical_arc_to(cx - rx, cy, rx, ry, x_axis_rotation, large_arc, sweep);
        // 4. arc to cx,cy-ry;
        path.elliptical_arc_to(cx, cy - ry, rx, ry, x_axis_rotation, large_arc, sweep);
        // 5. arc with a segment-completing close path operation.
        path.elliptical_arc_to(cx + rx, cy, rx, ry, x_axis_rotation, large_arc, sweep);
        path
    }

    fn svg_line_path(&self, attributes: FfiSvgAttributeFacts) -> libgfx_rust::path::PathBuilder {
        let mut path = libgfx_rust::path::PathBuilder::new();
        let viewport_width = self.viewport_width.to_float();
        let viewport_height = self.viewport_height.to_float();
        // 1. perform an absolute moveto operation to absolute location (x1,y1)
        path.move_to(
            attributes.line_x1.resolve_relative_to(viewport_width),
            attributes.line_y1.resolve_relative_to(viewport_height),
        );
        // 2. perform an absolute lineto operation to absolute location (x2,y2)
        path.line_to(
            attributes.line_x2.resolve_relative_to(viewport_width),
            attributes.line_y2.resolve_relative_to(viewport_height),
        );
        path
    }

    fn svg_poly_path(points: Option<&[FfiFloatPoint]>, close: bool) -> libgfx_rust::path::PathBuilder {
        let mut path = libgfx_rust::path::PathBuilder::new();
        let Some(points) = points.filter(|points| !points.is_empty()) else {
            return path;
        };
        // 1. perform an absolute moveto operation to the first coordinate pair in the list of points
        path.move_to(points[0].x, points[0].y);
        // 2. for each subsequent coordinate pair, perform an absolute lineto operation to that
        //    coordinate pair.
        for point in &points[1..] {
            path.line_to(point.x, point.y);
        }
        // 3. a polygon performs a closepath command; a polyline does not.
        if close {
            path.close();
        }
        path
    }

    /// The geometry a shape element draws, in the user units of the viewport it sits in.
    fn svg_geometry_path(&self, node: Node) -> libgfx_rust::path::OwnedPath {
        let points = self.run.callbacks.arena().svg_points(node);
        self.svg_geometry_path_of(self.svg_attributes(node), self.style(node), points.as_deref())
    }

    /// As above, from an element's published attributes and computed style rather than from its
    /// box.
    // FIXME: The DOM's getTotalLength() still builds the same geometry from the C++
    //        SVG*Element::get_path() implementations. Route it through this, so a fix to one copy
    //        cannot miss the other.
    fn svg_geometry_path_of(
        &self,
        attributes: FfiSvgAttributeFacts,
        style: StyleValues<'_>,
        points: Option<&[FfiFloatPoint]>,
    ) -> libgfx_rust::path::OwnedPath {
        let builder = match attributes.geometry_kind {
            SVG_GEOMETRY_KIND_PATH => return self.svg_path_element_path(style),
            SVG_GEOMETRY_KIND_RECT => self.svg_rect_path(style),
            SVG_GEOMETRY_KIND_CIRCLE => self.svg_circle_path(style),
            SVG_GEOMETRY_KIND_ELLIPSE => self.svg_ellipse_path(style),
            SVG_GEOMETRY_KIND_LINE => self.svg_line_path(attributes),
            SVG_GEOMETRY_KIND_POLYLINE => Self::svg_poly_path(points, false),
            SVG_GEOMETRY_KIND_POLYGON => Self::svg_poly_path(points, true),
            _ => libgfx_rust::path::PathBuilder::new(),
        };
        builder.build()
    }

    /// The `d` property's path. It is already a shared, immutable blob on the computed style, so
    /// the pass only has to realize it as geometry and stamp the shape's fill rule onto it.
    fn svg_path_element_path(&self, style: StyleValues<'_>) -> libgfx_rust::path::OwnedPath {
        let Some(shape) = style
            .svg_reset()
            .d
            .style_value()
            .and_then(crate::css::style_value::StyleValueData::basic_shape)
        else {
            return libgfx_rust::path::PathBuilder::new().build();
        };
        // The C++ BasicShape variant `d` holds is a path().
        const BASIC_SHAPE_PATH: u8 = 6;
        debug_assert_eq!(shape.kind, BASIC_SHAPE_PATH, "the d property holds a path()");
        let mut path = shape.path.to_gfx_path();
        path.set_fill_type(i32::from(shape.fill_rule));
        path
    }

    /// The pixel value one of the text positioning attributes resolves to. A relative length is
    /// absolutized against the element's own style, exactly as the attribute's parse used to be.
    fn resolve_svg_text_length(&self, node: Node, value: FfiSvgLengthValue, reference: CssPixels) -> f32 {
        match value.kind {
            SVG_LENGTH_KIND_NUMBER => value.value as f32,
            SVG_LENGTH_KIND_PERCENTAGE => {
                CssPixels::truncated_value_for(reference.to_double() * value.value / 100.0).to_float()
            }
            SVG_LENGTH_KIND_LENGTH => resolve_svg_length(&self.run.callbacks, node, value).to_float(),
            _ => 0.0,
        }
    }

    /// The character data an SVG text content element renders: its direct child text, as written.
    /// SVG shapes the element's own characters, not the white-space-collapsed text a line box
    /// would lay out, so this reads the source text the row kept beside its rendering.
    fn svg_text_contents(&self, node: Node) -> Vec<u16> {
        let mut text: Vec<u16> = Vec::new();
        for child in self.run.callbacks.children(node) {
            if node_facts::kind_is_text(self.node_kind(child)) {
                self.append_svg_source_text(child, &mut text);
            }
        }
        trim_ascii_whitespace(&mut text);
        text
    }

    /// The character data text on a path renders: the source text of every text node in its
    /// subtree whose parent has a box, rather than of its direct children alone.
    fn rendered_svg_text_contents(&self, root: Node) -> Vec<u16> {
        let mut text: Vec<u16> = Vec::new();
        let mut current = root;
        loop {
            if node_facts::kind_is_text(self.node_kind(current)) {
                self.append_svg_source_text(current, &mut text);
            }
            let child = self.first_child(current);
            if !child.is_invalid() {
                current = child;
                continue;
            }
            loop {
                if current == root {
                    trim_ascii_whitespace(&mut text);
                    return text;
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

    fn append_svg_source_text(&self, text_node: Node, out: &mut Vec<u16>) {
        let content = self.run.callbacks.arena().text_content(text_node);
        debug_assert!(content.is_some(), "SVG text child was not synced before layout");
        if let Some(source) = content.and_then(|content| content.svg_source_text.as_deref()) {
            out.extend_from_slice(source);
        }
    }

    /// https://svgwg.org/svg2-draft/text.html#TermTextChunk
    /// Each new absolute positioning adjustment (due to an 'x' or 'y' attribute, or forced line
    /// break) creates a new text chunk.
    /// https://svgwg.org/svg2-draft/text.html#TextElementXAttribute
    /// NB: The initial value of 'x' and 'y' is "0 for 'text'; (none) for 'tspan'". So, a <text>
    ///     element always positions its first character absolutely, and so always starts a chunk.
    fn svg_text_box_starts_text_chunk(&self, text_box: Node) -> bool {
        let attributes = self.svg_attributes(text_box);
        attributes.is_text_element
            || attributes.text_x.kind != SVG_LENGTH_KIND_NONE
            || attributes.text_y.kind != SVG_LENGTH_KIND_NONE
    }

    fn apply_svg_text_positioning(&mut self, text_box: Node, attributes: FfiSvgAttributeFacts) {
        let viewport_width = self.viewport_width;
        let viewport_height = self.viewport_height;
        if attributes.text_x.kind != SVG_LENGTH_KIND_NONE {
            self.current_text_position.x = self.resolve_svg_text_length(text_box, attributes.text_x, viewport_width);
        }
        if attributes.text_y.kind != SVG_LENGTH_KIND_NONE {
            self.current_text_position.y = self.resolve_svg_text_length(text_box, attributes.text_y, viewport_height);
        }
        self.current_text_position.x += self.resolve_svg_text_length(text_box, attributes.text_dx, viewport_width);
        self.current_text_position.y += self.resolve_svg_text_length(text_box, attributes.text_dy, viewport_height);
    }

    /// https://drafts.csswg.org/css-inline/#dominant-baseline-property
    fn svg_dominant_baseline_offset(&self, style: StyleValues<'_>) -> f32 {
        let svg = style.inherited_svg();
        let metric = if svg.has_dominant_baseline {
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
        let font = style.font();
        match metric {
            baseline_metric::CENTRAL => (font.font_ascent - font.font_descent) / 2.0,
            baseline_metric::MIDDLE => font.font_x_height / 2.0,
            // FIXME: Read the hanging baseline from the font's BASE table.
            baseline_metric::HANGING => font.font_ascent * 0.8,
            // FIXME: Read the ideographic baseline from the font's BASE table.
            baseline_metric::IDEOGRAPHIC => -font.font_descent,
            // FIXME: Read the math baseline from the font's BASE table.
            baseline_metric::MATHEMATICAL => font.font_ascent * 0.5,
            // FIXME: Support text-top and text-bottom.
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
        let frozen_font_list = style.frozen_font_list();
        let font_for = |offset: usize| {
            frozen_font_list.font_for_code_point(
                code_point_at(text, offset),
                libgfx_rust::font::EmojiPresentation {
                    is_emoji: false,
                    forced: false,
                },
            )
        };
        let mut last_font = font_for(0);
        let mut run_start = 0;
        let mut offset = code_point_length_at(text, 0);
        while offset < text.len() {
            let font = font_for(offset);
            if font != last_font {
                run(&last_font, &text[run_start..offset]);
                last_font = font;
                run_start = offset;
            }
            offset += code_point_length_at(text, offset);
        }
        run(&last_font, &text[run_start..]);
    }

    /// Mirrors Gfx::shape_text(baseline_start, text, font_cascade_list): one run per stretch of
    /// text the cascade resolves to the same font, each starting where the last one ended. Also
    /// returns the union of the runs' glyph cells, each run's measured with its own font, and the
    /// glyph cell of every code unit of the text.
    fn shape_svg_text(&self, style: StyleValues<'_>, text: &[u16], baseline_start: FfiFloatPoint) -> ShapedSvgText {
        let mut shaped_text = ShapedSvgText {
            runs: Vec::new(),
            advance: 0.0,
            glyph_cells: SvgCssPixelRect::default(),
            character_cells: Vec::with_capacity(text.len()),
        };
        Self::for_each_svg_font_run(style, text, |font, run_text| {
            let run_start_x = baseline_start.x + shaped_text.advance;
            let shaped = libgfx_rust::text_layout::shape_text(
                font,
                run_text,
                libgfx_rust::text_layout::TextType::Common,
                run_start_x,
                0.0,
                0.0,
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
            shaped_text.glyph_cells.unite(float_rect_to_css_pixels(FfiFloatRect {
                x: run_start_x,
                y: cell_top,
                width: shaped.width(),
                height: cell_height,
            }));
            append_svg_text_character_cells(
                &mut shaped_text.character_cells,
                shaped.glyphs(),
                run_text.len(),
                run_start_x + shaped.width(),
                cell_top,
                cell_height,
            );
            shaped_text.advance += shaped.width();
            let mut glyph_buffer = shaped.into_glyphs();
            let glyphs = glyph_buffer.to_mut();
            if baseline_start.y != 0.0 {
                for glyph in glyphs.iter_mut() {
                    glyph.y += baseline_start.y;
                }
            }
            shaped_text.runs.push(libgfx_rust::path::GlyphRun {
                font: font.clone(),
                glyphs: std::mem::take(glyphs),
            });
        });
        shaped_text
    }

    /// The advance of the text run rendered by the given box; that is, of its direct child text.
    fn svg_text_run_advance(&self, text_box: Node, text: &[u16]) -> f32 {
        let mut advance = 0.0f32;
        Self::for_each_svg_font_run(self.style(text_box), text, |font, run_text| {
            advance += libgfx_rust::text_layout::shape_text(
                font,
                run_text,
                libgfx_rust::text_layout::TextType::Common,
                0.0,
                0.0,
                0.0,
            )
            .width();
        });
        advance
    }

    /// Measures the total advance of the text chunk that starts at the given box, and determines
    /// the 'text-anchor' value that applies to the chunk. The chunk extends in document order
    /// through the subtree of the containing <text> element until the next box that starts a chunk
    /// of its own.
    fn measure_svg_text_chunk(&self, chunk_start_box: Node) -> SvgTextChunkMeasurement {
        let mut subtree_root = chunk_start_box;
        let mut ancestor = self.parent(chunk_start_box);
        while !ancestor.is_invalid() && self.node_kind(ancestor) == NodeKind::SVGTextBox {
            subtree_root = ancestor;
            ancestor = self.parent(ancestor);
        }

        let mut measurement = SvgTextChunkMeasurement::default();
        let mut found_chunk_start = false;
        let mut found_first_rendered_text = false;
        let mut current = subtree_root;
        'walk: loop {
            let kind = self.node_kind(current);
            // AD-HOC: Text on a path is laid out independently; see compute_path_for_svg_text_path().
            let descend = kind != NodeKind::SVGTextPathBox;
            if kind == NodeKind::SVGTextBox {
                if current == chunk_start_box {
                    found_chunk_start = true;
                } else if found_chunk_start && self.svg_text_box_starts_text_chunk(current) {
                    break 'walk;
                }
                if found_chunk_start {
                    let text = self.svg_text_contents(current);
                    if !text.is_empty() {
                        if !found_first_rendered_text {
                            // https://svgwg.org/svg2-draft/text.html#TextLayoutAlgorithm
                            // Adjust shift based on the value of 'text-anchor' and 'direction' of
                            // the element the character at index i.
                            // FIXME: Take text direction into account.
                            measurement.anchor = self.style(current).inherited_svg().text_anchor;
                            found_first_rendered_text = true;
                        }
                        measurement.advance += self.svg_text_run_advance(current, &text);
                    }
                }
            }
            if descend {
                let child = self.first_child(current);
                if !child.is_invalid() {
                    current = child;
                    continue;
                }
            }
            loop {
                if current == subtree_root {
                    break 'walk;
                }
                let sibling = self.next_sibling(current);
                if !sibling.is_invalid() {
                    current = sibling;
                    break;
                }
                current = self.parent(current);
            }
        }
        measurement
    }

    /// The glyph outlines a <text> or <tspan> renders, the cells they occupy, and the glyph cell of each of its
    /// characters, advancing the current text position.
    fn svg_text_box_path(
        &mut self,
        text_box: Node,
    ) -> (
        libgfx_rust::path::OwnedPath,
        SvgCssPixelRect,
        Vec<FfiSvgTextCharacterCell>,
    ) {
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
        }
        self.apply_svg_text_positioning(text_box, attributes);
        if self.svg_text_box_starts_text_chunk(text_box) {
            // https://svgwg.org/svg2-draft/text.html#TextAnchoringProperties
            // The 'text-anchor' property is applied to each individual text chunk within a given
            // 'text' element.
            // AD-HOC: The spec applies 'text-anchor' as a shift of the chunk's rendered glyphs
            //         after layout; shifting the chunk's starting position up front by the chunk's
            //         total advance is equivalent for horizontal text - since every run in the
            //         chunk is laid out sequentially from this position.
            let chunk = self.measure_svg_text_chunk(text_box);
            match chunk.anchor {
                // The rendered characters are aligned such that the start of the resulting
                // rendered text is at the initial current text position.
                text_anchor::START => {}
                // The rendered characters are shifted such that the geometric middle of the
                // resulting rendered text (determined from the initial and final current text
                // position before applying the 'text-anchor' property) is at the initial current
                // text position.
                text_anchor::MIDDLE => self.current_text_position.x -= chunk.advance / 2.0,
                // The rendered characters are shifted such that the end of the resulting rendered
                // text (final current text position before applying the 'text-anchor' property) is
                // at the initial current text position.
                text_anchor::END => self.current_text_position.x -= chunk.advance,
                _ => unreachable!("invalid text-anchor value"),
            }
        }

        let style = self.style(text_box);
        let text_offset = FfiFloatPoint {
            x: self.current_text_position.x,
            y: self.current_text_position.y + self.svg_dominant_baseline_offset(style),
        };
        let text = self.svg_text_contents(text_box);
        let shaped = self.shape_svg_text(style, &text, text_offset);

        // https://svgwg.org/svg2-draft/text.html#TextLayoutIntroduction
        // After each glyph is placed, the current text position is advanced by the glyph's advance
        // value (typically the width for horizontal text or height for vertical text).
        // FIXME: Take writing mode and text direction into account.
        self.current_text_position.x += shaped.advance;

        (
            libgfx_rust::path::OwnedPath::from_glyph_runs(&shaped.runs),
            shaped.glyph_cells,
            shaped.character_cells,
        )
    }

    /// The glyph outlines a <textPath> renders: its whole subtree's text, shaped from the origin
    /// and then laid along the shape its `href` names. Also returns the glyph cell of each of its
    /// characters.
    /// https://svgwg.org/svg2-draft/text.html#TextPathElement
    /// FIXME: The character cells are those of the text shaped from the origin; they aren't moved
    ///        onto the path or rotated along it, so only their advances are meaningful.
    fn svg_text_path_box_path(
        &self,
        text_path_box: Node,
    ) -> (libgfx_rust::path::OwnedPath, Vec<FfiSvgTextCharacterCell>) {
        let empty = || libgfx_rust::path::PathBuilder::new().build();
        let Some(shape_path) = self.svg_referenced_shape_path(text_path_box) else {
            return (empty(), Vec::new());
        };

        let style = self.style(text_path_box);
        let text = self.rendered_svg_text_contents(text_path_box);
        let shaped = self.shape_svg_text(style, &text, FfiFloatPoint::default());

        // https://svgwg.org/svg2-draft/text.html#TextPathElementStartOffsetAttribute
        let mut start_offset = self
            .svg_attributes(text_path_box)
            .text_path_start_offset
            .resolve_relative_to(shape_path.length());
        // FIXME: Take writing mode and text direction into account.
        match style.inherited_svg().text_anchor {
            text_anchor::START => {}
            text_anchor::MIDDLE => start_offset -= shaped.advance / 2.0,
            text_anchor::END => start_offset -= shaped.advance,
            _ => unreachable!("invalid text-anchor value"),
        }

        (
            shape_path.place_glyph_runs_along(&shaped.runs, start_offset),
            shaped.character_cells,
        )
    }

    /// The geometry of the shape element an SVG reference names, read from what that element
    /// published rather than from a box: a `<textPath>`'s target is normally a `<path>` inside a
    /// `<defs>`, and a `<defs>` builds no box at all.
    fn svg_referenced_shape_path(&self, referring_box: Node) -> Option<libgfx_rust::path::OwnedPath> {
        let arena = self.run.callbacks.arena();
        let referrer = arena.node_style_node(referring_box)?;
        let atom = self.svg_attributes(referring_box).reference_fragment_atom;
        let shape = arena.element_by_svg_reference(referrer, atom)?;
        let attributes = arena.style_node_svg_attribute_facts(shape);
        // Only a geometry element is followed; every other element answers with no shape at all.
        if attributes.geometry_kind == SVG_GEOMETRY_KIND_NONE {
            return None;
        }
        let payloads = arena.style_node_style_payloads(shape)?;
        let points = arena.style_node_svg_points(shape);
        Some(self.svg_geometry_path_of(attributes, StyleValues::new(&payloads), points.as_deref()))
    }

    fn svg_attributes(&self, node: Node) -> FfiSvgAttributeFacts {
        self.run.callbacks.arena().svg_attribute_facts(node)
    }

    fn svg_facts(&self, node: Node) -> SvgElementFacts {
        let data = self.run.callbacks.node_data(node);
        let mut facts = SvgElementFacts {
            is_document_element: node_facts::has_flag(data, NodeFlag::IsDocumentElement),
            document_is_decoded_svg: self.run.callbacks.arena().document_is_decoded_svg(),
            ..Default::default()
        };
        // Everything below is a graphics element's business; a <mask> or <clipPath> box answers
        // the defaults.
        if self.svg_attributes(node).is_graphics_element {
            facts.additional_element_transform = self.svg_additional_element_transform(node);
            facts.element_transform = self.svg_graphics_element_transform(node, facts.additional_element_transform);
            facts.viewport_percentage_basis = self.viewport_percentage_basis(node);
            facts.visible_stroke_width = self.visible_stroke_width(node, facts.viewport_percentage_basis);
        }
        facts
    }

    fn svg_element_transform(&self, node: Node) -> FfiAffineTransform {
        if !self.svg_attributes(node).is_graphics_element {
            return FfiAffineTransform::default();
        }
        self.svg_graphics_element_transform(node, self.svg_additional_element_transform(node))
    }

    /// The nearest flat-tree ancestor of `node` whose published attributes `matches` accepts, named
    /// by its style node. The flat tree is the ancestry SVG resolves a viewport against, and it is
    /// not the layout tree: a <mask> box hangs under the element that references it, and an <svg>
    /// inside a <foreignObject> sits under a box that establishes no SVG viewport at all. An
    /// ancestor answers from its publication whether or not it has a box.
    fn nearest_flat_tree_ancestor(
        &self,
        node: Node,
        matches: impl Fn(FfiSvgAttributeFacts) -> bool,
    ) -> Option<StyleNodeID> {
        let arena = self.run.callbacks.arena();
        let mut style_node = arena
            .node_style_node(node)
            .and_then(|style_node| arena.flat_tree_parent(style_node));
        while let Some(current) = style_node {
            if matches(arena.style_node_svg_attribute_facts(current)) {
                return Some(current);
            }
            style_node = arena.flat_tree_parent(current);
        }
        None
    }

    /// The user-unit size of the viewport a viewport-establishing element sets up: its view box,
    /// or the computed width and height of its box, with nothing for an element that has none.
    /// Percentages resolve against nothing here, since layout has not sized the element yet when a
    /// descendant asks.
    fn svg_viewport_element_size(&self, element: StyleNodeID) -> FfiCssPixelSize {
        let arena = self.run.callbacks.arena();
        let attributes = arena.style_node_svg_attribute_facts(element);
        if attributes.has_active_view_box {
            return FfiCssPixelSize {
                width: CssPixels::nearest_value_for(attributes.active_view_box.width),
                height: CssPixels::nearest_value_for(attributes.active_view_box.height),
            };
        }
        let row = arena.bound_row(element);
        if row.is_invalid() {
            return FfiCssPixelSize::default();
        }
        let style = self.style(row);
        FfiCssPixelSize {
            width: style.width().to_px(CssPixels::default()),
            height: style.height().to_px(CssPixels::default()),
        }
    }

    // Resolved relative to the "Scaled viewport size": https://www.w3.org/TR/2017/WD-fill-stroke-3-20170413/#scaled-viewport-size
    // FIXME: The spec formula is the normalized diagonal sqrt((width² + height²) / 2); this keeps
    //        the historical (width + height) / 2 approximation.
    // <symbol> instances establish nested viewports; percentages inside one resolve against it,
    // not the enclosing <svg>.
    fn viewport_percentage_basis(&self, node: Node) -> CssPixels {
        let Some(element) =
            self.nearest_flat_tree_ancestor(node, |facts| facts.is_svg_svg_element || facts.is_symbol_element)
        else {
            return CssPixels::default();
        };
        let viewport = self.svg_viewport_element_size(element);
        (viewport.width + viewport.height) * CssPixels::nearest_value_for(0.5)
    }

    // https://svgwg.org/svg2-draft/struct.html#UseElement
    // The x and y properties define an additional transformation (translate(x,y), where x and y
    // represent the computed value of the corresponding property) to be applied to the 'use'
    // element, after any transformations specified with other properties.
    fn svg_additional_element_transform(&self, node: Node) -> FfiAffineTransform {
        if !self.svg_attributes(node).is_use_element {
            return FfiAffineTransform::default();
        }
        let viewport = self
            .nearest_flat_tree_ancestor(node, |facts| facts.is_svg_svg_element)
            .map_or_else(FfiCssPixelSize::default, |element| {
                self.svg_viewport_element_size(element)
            });
        let style = self.style(node);
        FfiAffineTransform::default().translated(
            style.x().to_px(viewport.width).to_float(),
            style.y().to_px(viewport.height).to_float(),
        )
    }

    /// The element's own CSS transform, reduced to the 2D affine SVG geometry works in, with
    /// `additional_element_transform`, a <use> element's translation, multiplied in.
    fn svg_graphics_element_transform(
        &self,
        node: Node,
        additional_element_transform: FfiAffineTransform,
    ) -> FfiAffineTransform {
        let style = self.style(node);
        let matrix = crate::painting::visual_context::node_values::multiply_transform_functions(
            libgfx_rust::FloatMatrix4x4::identity(),
            style.transform().resolved_transforms.as_slice(),
            CssPixelRect::default(),
        );
        libgfx_rust::multiply_affine(matrix.extract_2d_affine(), additional_element_transform.into()).into()
    }

    /// The stroke width the path's bounding box has to grow by: an invisible stroke takes up no
    /// room.
    // NB: CSS geometry-effect metadata relies on this reading only stroke color and width.
    //     If SVG bounds begin accounting for caps, joins, miter limits, or stroke opacity,
    //     mark those properties as affecting layout geometry as well.
    fn visible_stroke_width(&self, node: Node, viewport_percentage_basis: CssPixels) -> f32 {
        let style = self.style(node);
        let svg = style.inherited_svg();
        let stroke_is_visible =
            crate::painting::record::paint::svg::svg_paint_color(&svg.stroke).is_some_and(|color| color >> 24 != 0);
        if !stroke_is_visible {
            return 0.0;
        }
        svg.stroke_width
            .length_percentage()
            .map_or(0.0, |value| value.to_px(viewport_percentage_basis).to_double() as f32)
    }

    fn style(&self, node: Node) -> StyleValues<'_> {
        StyleValues::for_node(&self.run.callbacks, node)
    }

    #[track_caller]
    fn used_values(&self, node: Node) -> &'pass UsedValues {
        self.run.records.used_values(node)
    }

    fn create_used_values(&self, node: Node) -> &'pass UsedValues {
        // SVG descendants deliberately carry no percentage basis.
        // SVG layout resolves percentages against the SVG viewport, not a CSS containing
        // block, so boxes inside the SVG subtree carry no percentage basis.
        self.run
            .records
            .create_used_values(&self.run.callbacks, node, ContainingBlockConstraints::default())
    }

    fn set_svg_viewport_transform(&self, node: Node, transform: FfiAffineTransform) {
        self.used_values(node).rare_data_mut().svg.viewport_transform = Some(transform);
    }

    fn set_svg_viewport_size(&self, node: Node, viewport_size: FfiCssPixelSize) {
        self.used_values(node).rare_data_mut().svg.viewport_size = Some(viewport_size);
    }

    fn commit_svg_element_facts(&self, node: Node, facts: SvgElementFacts, attributes: FfiSvgAttributeFacts) {
        let used = self.used_values(node);
        let mut rare = used.rare_data_mut();
        rare.svg.view_box = attributes.has_active_view_box.then_some(attributes.active_view_box);
        rare.svg.element_transform = (!facts.element_transform.is_identity()).then_some(facts.element_transform);
        rare.svg.additional_element_transform =
            (!facts.additional_element_transform.is_identity()).then_some(facts.additional_element_transform);
        rare.svg.mask_area_facts = (self.node_kind(node) == NodeKind::SVGMaskBox).then_some(SvgMaskAreaFacts {
            units_are_object_bounding_box: attributes.mask_units == SVG_UNITS_OBJECT_BOUNDING_BOX,
            x: attributes.mask_x,
            y: attributes.mask_y,
            width: attributes.mask_width,
            height: attributes.mask_height,
        });
        rare.svg.viewport_percentage_basis = facts.viewport_percentage_basis;
        rare.svg.resource_content_units_are_object_bounding_box =
            attributes.content_units == SVG_UNITS_OBJECT_BOUNDING_BOX;
    }

    fn place_child(&self, node: Node, x: CssPixels, y: CssPixels) {
        formatting_context::place_child(&self.run, node, FfiCssPixelPoint { x, y }, None);
    }

    fn first_child_of_kind(&self, node: Node, kind: NodeKind) -> Option<Node> {
        self.run
            .callbacks
            .children(node)
            .find(|&child| self.node_kind(child) == kind)
    }

    pub(super) fn run(&mut self, run: &FormattingContextRun<'pass>, input: LayoutInput) {
        // NOTE: SVG doesn't have a "formatting context" in the spec, but this is the most
        //       obvious way to drive SVG layout in our engine at the moment.
        let kind = self.node_kind(self.run.box_);
        let facts = self.svg_facts(self.run.box_);
        let attributes = self.svg_attributes(self.run.box_);
        let used_pointer = self.used_values(self.run.box_);
        let used = &used_pointer;
        self.commit_svg_element_facts(self.run.box_, facts, attributes);

        if facts.is_document_element && !facts.document_is_decoded_svg && !used.has_content_offset.get() {
            // Overwrite the content width/height with the styled node width/height (from <svg width height ...>)
            //
            // NOTE: If a height had not been provided by the svg element, it was set to the height of the container
            let style = self.style(self.run.box_);
            if style.width().is_length() {
                used.set_content_inline_size(style.width().to_px(CssPixels::default()));
            }
            if style.height().is_length() {
                used.set_content_block_size(style.height().to_px(CssPixels::default()));
            }
            // FIXME: In SVG 2, length can also be a percentage. We'll need to support that.
        }

        // NOTE: We consider all SVG root elements to have definite size in both axes.
        //       I'm not sure if this is good or bad, but our viewport transform logic depends on it.
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);

        // An embedded SVG viewport's outer size does not depend on its descendants. Measuring it
        // only needs the root sizing above; shapes, text, and foreignObject content wait for layout.
        if self.run.purpose.is_measurement() && kind == NodeKind::SVGSVGBox && !facts.is_document_element {
            return;
        }

        if kind == NodeKind::SVGSVGBox {
            self.set_svg_viewport_size(
                self.run.box_,
                FfiCssPixelSize {
                    width: used.content_inline_size.get(),
                    height: used.content_block_size.get(),
                },
            );
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
            let mut viewport_transform = FfiAffineTransform::default();
            if let Some(view_box) = active_view_box {
                // FIXME: This should allow just one of width or height to be specified.
                // E.g. We should be able to layout <svg width="100%"> where height is unspecified/auto.
                let scale_width = if used.has_definite_inline_size() {
                    used.content_inline_size.get().to_double() / view_box.width
                } else {
                    1.0
                };
                let scale_height = if used.has_definite_block_size() {
                    used.content_block_size.get().to_double() / view_box.height
                } else {
                    1.0
                };
                // The initial value for preserveAspectRatio is xMidYMid meet.
                let transform = scale_and_align_viewbox_content(
                    attributes.preserve_aspect_ratio_align,
                    attributes.preserve_aspect_ratio_meet_or_slice,
                    view_box,
                    scale_width as f32,
                    scale_height as f32,
                    (used.content_inline_size.get(), used.content_block_size.get()),
                    used.has_definite_inline_size(),
                );
                viewport_transform = viewport_transform
                    .translated(
                        transform.offset.x.raw_value() as f32 / 64.0,
                        transform.offset.y.raw_value() as f32 / 64.0,
                    )
                    .scaled(transform.scale_factor_x as f32, transform.scale_factor_y as f32)
                    .translated(-view_box.min_x as f32, -view_box.min_y as f32);
            }
            self.set_svg_viewport_transform(self.run.box_, viewport_transform);
        }

        self.viewport_width = active_view_box.map_or_else(
            || {
                if used.has_definite_inline_size() {
                    used.content_inline_size.get()
                } else {
                    CssPixels::default()
                }
            },
            |view_box| CssPixels::nearest_value_for(view_box.width),
        );
        self.viewport_height = active_view_box.map_or_else(
            || {
                if used.has_definite_block_size() {
                    used.content_block_size.get()
                } else {
                    CssPixels::default()
                }
            },
            |view_box| CssPixels::nearest_value_for(view_box.height),
        );
        self.available_space = Some(input.available_space);
        self.quirks_mode_percentage_basis_block_size = input
            .containing_block_constraints
            .quirks_mode_percentage_basis_block_size;

        for child in self.run.callbacks.children(self.run.box_) {
            if NodeFacts::new(&self.run.callbacks, child).is_box() {
                self.layout_svg_element(run, child, input);
            }
        }
    }

    fn layout_svg_element(&mut self, run: &FormattingContextRun<'pass>, child: Node, input: LayoutInput) {
        let kind = self.node_kind(child);
        if self.svg_attributes(child).is_fit_to_view_box {
            self.layout_nested_viewport(run, child);
        } else if kind == NodeKind::SVGForeignObjectBox {
            let child_used_pointer = self.create_used_values(child);
            let style = self.style(child);
            let rect = SvgCssPixelRect {
                x: style.x().to_px(self.viewport_width),
                y: style.y().to_px(self.viewport_height),
                width: style.width().to_px(self.viewport_width),
                height: style.height().to_px(self.viewport_height),
            };
            let child_used = child_used_pointer;
            child_used.set_content_inline_size(rect.width);
            child_used.set_content_block_size(rect.height);

            let child_input = LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::definite(child_used.content_inline_size.get()),
                    block_size: AvailableSize::definite(child_used.content_block_size.get()),
                },
                containing_block_constraints: ContainingBlockConstraints {
                    quirks_mode_percentage_basis_block_size: self.quirks_mode_percentage_basis_block_size,
                    ..Default::default()
                },
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives::default(),
                participation: ParticipationInParentFormattingContext::Item,
            };
            match formatting_context::layout_inside_child(
                run,
                None,
                None,
                child,
                self.run.layout_mode,
                child_input,
                true,
            ) {
                ChildLayoutOutcome::Created(_) => {}
                ChildLayoutOutcome::Skipped | ChildLayoutOutcome::ReenterCurrent => {
                    panic!("SVG foreign object did not create an independent formatting context")
                }
            };

            // Masks and clips may use this offset for objectBoundingBox units.
            self.place_child(child, rect.x, rect.y);
            if let Some(mask) = self.first_child_of_kind(child, NodeKind::SVGMaskBox) {
                self.layout_mask_or_clip(run, mask);
            }
            if let Some(clip) = self.first_child_of_kind(child, NodeKind::SVGClipBox) {
                self.layout_mask_or_clip(run, clip);
            }
        } else if kind_is_svg_graphics_box(kind) {
            self.layout_graphics_element(run, child, input);
        }
    }

    fn layout_nested_viewport(&mut self, run: &FormattingContextRun<'pass>, viewport: Node) {
        // Layout for a nested SVG viewport.
        // https://svgwg.org/svg2-draft/coords.html#EstablishingANewSVGViewport.
        let used_pointer = self.create_used_values(viewport);
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

        let used = &used_pointer;
        used.set_content_inline_size(nested_viewport_width);
        used.set_content_block_size(nested_viewport_height);
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);

        let mut nested_context = Self::new_nested(run, viewport);
        nested_context.run(run, self.nested_layout_input());
        self.set_svg_viewport_size(
            viewport,
            FfiCssPixelSize {
                width: nested_viewport_width,
                height: nested_viewport_height,
            },
        );
        self.place_child(viewport, nested_viewport_x, nested_viewport_y);
    }

    fn layout_graphics_element(&mut self, run: &FormattingContextRun<'pass>, graphics_box: Node, input: LayoutInput) {
        self.create_used_values(graphics_box);
        let facts = self.svg_facts(graphics_box);
        self.commit_svg_element_facts(graphics_box, facts, self.svg_attributes(graphics_box));
        let kind = self.node_kind(graphics_box);

        // https://svgwg.org/svg2-draft/struct.html#GroupsOverview
        // container element
        // An element which can have graphics elements and other container elements as child elements.
        // Specifically: ‘a’, ‘clipPath’, ‘defs’, ‘g’, ‘marker’, ‘mask’, ‘pattern’, ‘svg’, ‘switch’ and ‘symbol’.
        if kind_is_svg_container_element(kind) {
            // https://svgwg.org/svg2-draft/struct.html#Groups
            // 5.2. Grouping: the ‘g’ element
            // The ‘g’ element is a container element for grouping together related graphics elements.
            self.layout_container_element(run, graphics_box, input);
        } else if kind == NodeKind::SVGImageBox {
            self.layout_image_element(graphics_box);
        } else {
            // Assume this is a path-like element.
            self.layout_path_like_element(run, graphics_box, input, facts);
        }

        if let Some(mask) = self.first_child_of_kind(graphics_box, NodeKind::SVGMaskBox) {
            self.layout_mask_or_clip(run, mask);
        }
        if let Some(clip) = self.first_child_of_kind(graphics_box, NodeKind::SVGClipBox) {
            self.layout_mask_or_clip(run, clip);
        }
        for child in self.run.callbacks.children(graphics_box) {
            if self.node_kind(child) == NodeKind::SVGPatternBox {
                self.layout_mask_or_clip(run, child);
            }
        }
    }

    fn layout_path_like_element(
        &mut self,
        run: &FormattingContextRun<'pass>,
        graphics_box: Node,
        input: LayoutInput,
        facts: SvgElementFacts,
    ) {
        // A shape's geometry is its own attributes and computed style against the viewport, and
        // text is its own character data shaped with its own font cascade, so the pass draws both.
        let kind = self.node_kind(graphics_box);
        let (path, glyph_cells, character_cells) = match kind {
            NodeKind::SVGGeometryBox => (self.svg_geometry_path(graphics_box), None, None),
            NodeKind::SVGTextBox => {
                let (path, glyph_cells, character_cells) = self.svg_text_box_path(graphics_box);
                (path, Some(glyph_cells), Some(character_cells))
            }
            NodeKind::SVGTextPathBox => {
                let (path, character_cells) = self.svg_text_path_box_path(graphics_box);
                (path, None, Some(character_cells))
            }
            _ => (libgfx_rust::path::PathBuilder::new().build(), None, None),
        };

        // https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
        // Let fill-shape be the equivalent path of element if it is a shape, or a shape that includes each of the glyph
        // cells corresponding to the text within the elements otherwise.
        // NB: A text content element's box is its bounding box: The box getBBox() and getBoundingClientRect() report.
        let is_text_content_box = matches!(kind, NodeKind::SVGTextBox | NodeKind::SVGTextPathBox);
        let [x, y, width, height] = path.bounding_box();
        let mut bounding_box = float_rect_to_css_pixels(FfiFloatRect { x, y, width, height });
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

        if kind == NodeKind::SVGTextBox {
            // <text> and <tspan> elements can contain more text elements, whose glyph cells count toward this box too.
            for child in self.run.callbacks.children(graphics_box) {
                if matches!(self.node_kind(child), NodeKind::SVGTextBox | NodeKind::SVGTextPathBox) {
                    self.layout_graphics_element(run, child, input);
                    let child_used = self.used_values(child);
                    let offset = child_used.content_offset.get();
                    bounding_box.unite(SvgCssPixelRect {
                        x: offset.x,
                        y: offset.y,
                        width: child_used.content_inline_size.get(),
                        height: child_used.content_block_size.get(),
                    });
                }
            }
        }

        let used_pointer = self.used_values(graphics_box);
        let used = &used_pointer;
        used.set_content_inline_size(bounding_box.width);
        used.set_content_block_size(bounding_box.height);
        {
            let mut rare_data = used.rare_data_mut();
            rare_data.computed_svg_path = Some(std::sync::Arc::new(path));
            rare_data.svg_text_character_cells = character_cells.map(std::sync::Arc::new);
        }
        self.place_child(graphics_box, bounding_box.x, bounding_box.y);
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);
    }

    // https://w3c.github.io/svgwg/svg2-draft/embedded.html#Placement
    // Computation of automatically-sized values follows the Default Sizing Algorithm defined for
    // replaced elements in CSS layout [css-images-3]. In particular, when the referenced resource
    // does not have an intrinsic size (such as image types with no defined dimensions), it is
    // assumed to have a width of 300px and a height of 150px.
    fn svg_image_bounding_box(&self, image_box: Node) -> SvgCssPixelRect {
        use crate::painting::record::paint::replaced::{Fraction, SizeWithAspectRatio, run_default_sizing_algorithm};
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
            .callbacks
            .arena()
            .replaced_content_facts(image_box)
            .unwrap_or_default();
        let natural = SizeWithAspectRatio {
            width: facts.has_auto_content_width.then_some(facts.auto_content_width),
            height: facts.has_auto_content_height.then_some(facts.auto_content_height),
            aspect_ratio: (facts.auto_content_aspect_ratio_denominator != CssPixels::default()).then(|| {
                Fraction::of(
                    facts.auto_content_aspect_ratio_numerator,
                    facts.auto_content_aspect_ratio_denominator,
                )
            }),
        };
        // The default object size applies only once something has decoded; until then the image
        // has no size at all.
        let default_size = CssPixelSize::new(facts.default_preferred_width, facts.default_preferred_height);

        let sizing = run_default_sizing_algorithm(specified_width, specified_height, &natural, default_size);
        SvgCssPixelRect {
            x: style.x().to_px(self.viewport_width),
            y: style.y().to_px(self.viewport_height),
            width: sizing.width,
            height: sizing.height,
        }
    }

    fn layout_image_element(&self, image_box: Node) {
        let bounding_box = self.svg_image_bounding_box(image_box);
        let used_pointer = self.used_values(image_box);
        let used = &used_pointer;
        used.set_content_inline_size(bounding_box.width);
        used.set_content_block_size(bounding_box.height);
        self.place_child(image_box, bounding_box.x, bounding_box.y);
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);
    }

    fn layout_mask_or_clip(&mut self, run: &FormattingContextRun<'pass>, resource: Node) {
        let kind = self.node_kind(resource);
        let facts = self.svg_facts(resource);
        let attributes = self.svg_attributes(resource);
        assert!(node_facts::kind_is_svg_resource_box(kind));
        // FIXME: Somehow limit <clipPath> contents to: shape elements, <text>, and <use>.
        let used_pointer = self.create_used_values(resource);
        self.commit_svg_element_facts(resource, facts, attributes);

        if kind == NodeKind::SVGPatternBox && attributes.has_active_view_box {
            if attributes.pattern_units == SVG_UNITS_USER_SPACE_ON_USE {
                let width = if attributes.pattern_width.is_percentage {
                    attributes.pattern_width.value * (self.viewport_width.raw_value() as f32 / 64.0)
                } else {
                    attributes.pattern_width.value
                };
                let height = if attributes.pattern_height.is_percentage {
                    attributes.pattern_height.value * (self.viewport_height.raw_value() as f32 / 64.0)
                } else {
                    attributes.pattern_height.value
                };
                let used = &used_pointer;
                used.set_content_inline_size(CssPixels::nearest_value_for_f32(width));
                used.set_content_block_size(CssPixels::nearest_value_for_f32(height));
            } else {
                let parent = self.parent(resource);
                assert!(!parent.is_invalid());
                let parent_used_pointer = self.used_values(parent);
                let parent_used = parent_used_pointer;
                let used = &used_pointer;
                used.set_content_inline_size(CssPixels::nearest_value_for(
                    attributes.pattern_width.value as f64 * parent_used.content_inline_size.get().to_double(),
                ));
                used.set_content_block_size(CssPixels::nearest_value_for(
                    attributes.pattern_height.value as f64 * parent_used.content_block_size.get().to_double(),
                ));
            }
        } else if attributes.content_units == SVG_UNITS_OBJECT_BOUNDING_BOX {
            let parent = self.parent(resource);
            assert!(!parent.is_invalid());
            let parent_used_pointer = self.used_values(parent);
            let parent_used = parent_used_pointer;
            let used = &used_pointer;
            used.set_content_inline_size(parent_used.content_inline_size.get());
            used.set_content_block_size(parent_used.content_block_size.get());
        } else {
            let used = &used_pointer;
            used.set_content_inline_size(self.viewport_width);
            used.set_content_block_size(self.viewport_height);
        }

        let used = &used_pointer;
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);
        // Resource content lays out in its own user space; objectBoundingBox content scaling is a
        // paint-time transform in the resource's nested visual context tree.
        let mut nested_context = Self::new_nested(run, resource);
        nested_context.run(run, self.nested_layout_input());
        self.place_child(resource, CssPixels::default(), CssPixels::default());
    }

    fn layout_container_element(&mut self, run: &FormattingContextRun<'pass>, container: Node, input: LayoutInput) {
        let mut has_points = false;
        let mut min_x = CssPixels::default();
        let mut min_y = CssPixels::default();
        let mut max_x = CssPixels::default();
        let mut max_y = CssPixels::default();
        for child in self.run.callbacks.children(container) {
            // Masks/clips/patterns do not change the bounding box of their parents.
            if NodeFacts::new(&self.run.callbacks, child).is_box()
                && !node_facts::kind_is_svg_resource_box(self.node_kind(child))
            {
                self.layout_svg_element(run, child, input);
                let child_used_pointer = self.used_values(child);
                let child_used = child_used_pointer;
                // The container's bounding box includes descendants' transforms; children lay out
                // untransformed, so each child rect maps through the child's own transform here.
                let mapped_child_rect = self.svg_element_transform(child).map_rect(FfiFloatRect {
                    x: child_used.content_offset.get().x.raw_value() as f32 / 64.0,
                    y: child_used.content_offset.get().y.raw_value() as f32 / 64.0,
                    width: child_used.content_inline_size.get().raw_value() as f32 / 64.0,
                    height: child_used.content_block_size.get().raw_value() as f32 / 64.0,
                });
                let left = CssPixels::nearest_value_for_f32(mapped_child_rect.x);
                let top = CssPixels::nearest_value_for_f32(mapped_child_rect.y);
                let right = left + CssPixels::nearest_value_for_f32(mapped_child_rect.width);
                let bottom = top + CssPixels::nearest_value_for_f32(mapped_child_rect.height);
                if has_points {
                    min_x = min_x.min(left);
                    min_y = min_y.min(top);
                    max_x = max_x.max(right);
                    max_y = max_y.max(bottom);
                } else {
                    min_x = left;
                    min_y = top;
                    max_x = right;
                    max_y = bottom;
                    has_points = true;
                }
            }
        }

        let used_pointer = self.used_values(container);
        let used = &used_pointer;
        used.set_content_inline_size(max_x - min_x);
        used.set_content_block_size(max_y - min_y);
        self.place_child(container, min_x, min_y);
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);
    }
}

fn font_metrics_for_length_resolution(style: StyleValues<'_>) -> crate::css::style_compute::FfiFontMetrics {
    let font = style.font();
    crate::css::style_compute::FfiFontMetrics {
        font_size: style.font_size().to_double(),
        x_height: CssPixels::nearest_value_for_f32(font.font_x_height).to_double(),
        // FIXME: This is only approximately the cap height, exactly as Length::FontMetrics has it.
        cap_height: CssPixels::nearest_value_for_f32(font.font_ascent).to_double(),
        zero_advance: CssPixels::nearest_value_for_f32(font.font_zero_advance).to_double(),
        line_height: style.line_height().to_double(),
    }
}

/// What a length one of an SVG element's attributes gives resolves against: the element's own style,
/// the document element's, and the viewport.
fn length_resolution_context(
    callbacks: &LayoutPass<'_>,
    node: Node,
    unit: u8,
) -> crate::css::style_compute::FfiLengthResolutionContext {
    let style = StyleValues::for_node(callbacks, node);
    let root_style = document_element_style(callbacks, node).unwrap_or(style);
    let viewport_width = callbacks.initial_containing_block_inline_size.to_double();
    let viewport_height = callbacks.initial_containing_block_block_size.to_double();
    // A container unit resolves against the nearest size query container on its axis, or the small viewport size
    // without one. Finding that container is style's business, so it is asked only for a length that needs it. A pass
    // that cannot ask resolves it to zero, and leaves the node for the host to lay out again.
    let container_bases = if crate::css::style_compute::length_unit_is_container_relative(unit) {
        let element = callbacks
            .arena()
            .dom_node_style_node(node)
            .expect("an SVG element's attribute length is resolved for the element's own box");
        let bases = callbacks.container_length_bases.bases(element);
        if bases.is_none() {
            callbacks.arena().note_unresolved_container_lengths(node);
        }
        bases
    } else {
        None
    };
    crate::css::style_compute::FfiLengthResolutionContext {
        viewport_width,
        viewport_height,
        font_metrics: font_metrics_for_length_resolution(style),
        root_font_metrics: font_metrics_for_length_resolution(root_style),
        // Only the out-flag below consumes these, and this resolution reports no dependency.
        font_metrics_depend_on_viewport_metrics: false,
        root_font_metrics_depend_on_viewport_metrics: false,
        has_container_width_basis: container_bases.is_some(),
        has_container_height_basis: container_bases.is_some(),
        container_width_basis: container_bases.map_or(0.0, |bases| bases.width),
        container_height_basis: container_bases.map_or(0.0, |bases| bases.height),
        container_width_basis_depends_on_viewport_metrics: false,
        container_height_basis_depends_on_viewport_metrics: false,
        subject_inline_axis_is_horizontal: style.writing_mode() == writing_mode::HORIZONTAL_TB,
        resolved_viewport_relative_length: std::ptr::null_mut(),
    }
}

/// The style of the document element, which a root-relative length resolves against.
fn document_element_style<'pass>(callbacks: &LayoutPass<'pass>, node: Node) -> Option<StyleValues<'pass>> {
    let mut ancestor = node;
    while !ancestor.is_invalid() {
        let data = callbacks.node_data(ancestor);
        if node_facts::has_flag(data, NodeFlag::IsDocumentElement) {
            return callbacks.arena().style_payloads(ancestor).map(StyleValues::new);
        }
        ancestor = data.parent.get();
    }
    None
}

/// The pixels an absolute or relative `<length>` an SVG element's attribute gives resolves to.
fn resolve_svg_length(callbacks: &LayoutPass<'_>, node: Node, length: FfiSvgLengthValue) -> CssPixels {
    let context = length_resolution_context(callbacks, node, length.unit);
    let absolutized =
        crate::css::style_compute::absolutize_length_for_calc(length.value, length.unit as usize, &context);
    CssPixels::nearest_value_for(absolutized.px)
}

// https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
/// The natural size of an `<svg>` root's box, negotiated from what its element published.
pub(crate) fn svg_root_natural_size(
    callbacks: &LayoutPass<'_>,
    node: Node,
) -> (Option<CssPixels>, Option<CssPixels>, Option<(CssPixels, CssPixels)>) {
    let attributes = callbacks.arena().svg_attribute_facts(node);
    // The intrinsic dimensions must also be determined from the width and height sizing properties. If either width or
    // height are not specified, the used value is the initial value 'auto'. 'auto' and percentage lengths must not be
    // used to determine an intrinsic width or intrinsic height.
    let resolve = |length: FfiSvgLengthValue| {
        (length.kind == SVG_LENGTH_KIND_LENGTH).then(|| resolve_svg_length(callbacks, node, length))
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
