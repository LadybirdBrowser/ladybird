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
}

pub const SVG_GEOMETRY_KIND_NONE: u8 = 0;
pub const SVG_GEOMETRY_KIND_PATH: u8 = 1;
pub const SVG_GEOMETRY_KIND_RECT: u8 = 2;
pub const SVG_GEOMETRY_KIND_CIRCLE: u8 = 3;
pub const SVG_GEOMETRY_KIND_ELLIPSE: u8 = 4;
pub const SVG_GEOMETRY_KIND_LINE: u8 = 5;
pub const SVG_GEOMETRY_KIND_POLYLINE: u8 = 6;
pub const SVG_GEOMETRY_KIND_POLYGON: u8 = 7;

/// Publishes what the SVG element `style_node` names parses to. A <polyline> or <polygon> passes
/// its `points` list beside the facts, since it is the one geometry attribute that is not a fixed
/// number of values.
///
/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread outside
/// any layout pass, and `points` must address `count` points for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_style_node_svg_attribute_facts(
    arena: *mut c_void,
    style_node: u32,
    facts: FfiSvgAttributeFacts,
    points: *const FfiFloatPoint,
    count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let arena = unsafe { LayoutNodeArena::from_handle_mut(arena) };
    let Some(style_node) = crate::css::style::tree::StyleNodeID::from_raw(style_node) else {
        return;
    };
    let points = if count == 0 {
        &[][..]
    } else {
        // SAFETY: The caller keeps the list alive for this synchronous call.
        unsafe { std::slice::from_raw_parts(points, count) }
    };
    arena.set_style_node_svg_attribute_facts(style_node, facts, points);
}

/// Retires what the SVG element `style_node` named published, once that identity is retired.
///
/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread outside
/// any layout pass.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_style_node_svg_attribute_facts(arena: *mut c_void, style_node: u32) {
    // SAFETY: Guaranteed by the caller.
    let arena = unsafe { LayoutNodeArena::from_handle_mut(arena) };
    let Some(style_node) = crate::css::style::tree::StyleNodeID::from_raw(style_node) else {
        return;
    };
    arena.clear_style_node_svg_attribute_facts(style_node);
}

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

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgPathRequest {
    pub viewport_width: CssPixels,
    pub viewport_height: CssPixels,
    pub current_text_position: FfiFloatPoint,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSvgPathResult {
    pub path_handle: *mut c_void,
    pub text_position_after: FfiFloatPoint,
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

fn kind_is_svg_resource_box(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGMaskBox | NodeKind::SVGClipBox | NodeKind::SVGPatternBox
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

pub(super) struct SvgFormattingContext<'pass> {
    purpose: formatting_context::LayoutPurpose,
    records: &'pass RunRecords<'pass>,
    box_: Node,
    layout_mode: LayoutMode,
    callbacks: LayoutPass<'pass>,
    available_space: Option<AvailableSpace>,
    quirks_mode_percentage_basis_block_size: Option<CssPixels>,
    viewport_width: CssPixels,
    viewport_height: CssPixels,
    current_text_position: FfiFloatPoint,
    fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
    should_collect_devtools_layout_data: bool,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
}

impl<'pass> SvgFormattingContext<'pass> {
    pub(super) fn new(run: &FormattingContextRun<'pass>) -> Self {
        Self::new_nested(run, run.box_)
    }

    fn new_nested(run: &FormattingContextRun<'pass>, box_: Node) -> Self {
        Self {
            purpose: run.purpose,
            records: run.records,
            box_,
            layout_mode: run.layout_mode,
            callbacks: run.callbacks,
            fragments: run.fragments.clone(),
            available_space: None,
            quirks_mode_percentage_basis_block_size: None,
            viewport_width: CssPixels::default(),
            viewport_height: CssPixels::default(),
            current_text_position: FfiFloatPoint::default(),
            should_collect_devtools_layout_data: run.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: run
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
        }
    }

    fn formatting_context_run(&self) -> FormattingContextRun<'pass> {
        FormattingContextRun {
            purpose: self.purpose,
            records: self.records,
            box_: self.box_,
            layout_mode: self.layout_mode,
            callbacks: self.callbacks,
            should_collect_devtools_layout_data: self.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: self
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            fragments: self.fragments.clone(),
            previous_line_data: None,
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
        self.callbacks.node_data(node).first_child.get()
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.callbacks.node_data(node).next_sibling.get()
    }

    fn parent(&self, node: Node) -> Node {
        self.callbacks.node_data(node).parent.get()
    }

    fn node_kind(&self, node: Node) -> NodeKind {
        self.callbacks.node_data(node).kind.get()
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
        let points = self.callbacks.arena().svg_points(node);
        self.svg_geometry_path_of(self.svg_attributes(node), self.style(node), points)
    }

    /// As above, from an element's published attributes and computed style rather than from its
    /// box.
    // FIXME: The DOM's getTotalLength() and the layout of a <textPath> still build the same geometry
    //        from the C++ SVG*Element::get_path() implementations. Route them through this, so a fix
    //        to one copy cannot miss the other.
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

    fn svg_attributes(&self, node: Node) -> FfiSvgAttributeFacts {
        self.callbacks.arena().svg_attribute_facts(node)
    }

    fn svg_facts(&self, node: Node) -> SvgElementFacts {
        let data = self.callbacks.node_data(node);
        let mut facts = SvgElementFacts {
            is_document_element: node_facts::has_flag(data, NodeFlag::IsDocumentElement),
            document_is_decoded_svg: self.callbacks.arena().document_is_decoded_svg(),
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
        let arena = self.callbacks.arena();
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
        let arena = self.callbacks.arena();
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
        StyleValues::for_node(&self.callbacks, node)
    }

    #[track_caller]
    fn used_values(&self, node: Node) -> &'pass UsedValues {
        self.records.used_values(node)
    }

    fn create_used_values(&self, node: Node) -> &'pass UsedValues {
        // SVG descendants deliberately carry no percentage basis.
        // SVG layout resolves percentages against the SVG viewport, not a CSS containing
        // block, so boxes inside the SVG subtree carry no percentage basis.
        self.records
            .create_used_values(&self.callbacks, node, ContainingBlockConstraints::default())
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
        formatting_context::place_child(&self.formatting_context_run(), node, FfiCssPixelPoint { x, y }, None);
    }

    fn for_each_child(&self, node: Node, mut callback: impl FnMut(Node)) {
        let mut child = self.first_child(node);
        while !child.is_invalid() {
            let next = self.next_sibling(child);
            callback(child);
            child = next;
        }
    }

    fn first_child_of_kind(&self, node: Node, kind: NodeKind) -> Option<Node> {
        let mut result = None;
        self.for_each_child(node, |child| {
            if result.is_none() && self.node_kind(child) == kind {
                result = Some(child);
            }
        });
        result
    }

    pub(super) fn run(&mut self, run: &FormattingContextRun<'pass>, input: LayoutInput) {
        // NOTE: SVG doesn't have a "formatting context" in the spec, but this is the most
        //       obvious way to drive SVG layout in our engine at the moment.
        let kind = self.node_kind(self.box_);
        let facts = self.svg_facts(self.box_);
        let attributes = self.svg_attributes(self.box_);
        let used_pointer = self.used_values(self.box_);
        let used = &used_pointer;
        self.commit_svg_element_facts(self.box_, facts, attributes);

        if facts.is_document_element && !facts.document_is_decoded_svg && !used.has_content_offset.get() {
            // Overwrite the content width/height with the styled node width/height (from <svg width height ...>)
            //
            // NOTE: If a height had not been provided by the svg element, it was set to the height of the container
            let style = self.style(self.box_);
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
        if self.purpose.is_measurement() && kind == NodeKind::SVGSVGBox && !facts.is_document_element {
            return;
        }

        if kind == NodeKind::SVGSVGBox {
            self.set_svg_viewport_size(
                self.box_,
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
                    self.set_svg_viewport_transform(self.box_, FfiAffineTransform::default());
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
            self.set_svg_viewport_transform(self.box_, viewport_transform);
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

        let mut child = self.first_child(self.box_);
        while !child.is_invalid() {
            let next = self.next_sibling(child);
            if NodeFacts::new(&self.callbacks, child).is_box() {
                self.layout_svg_element(run, child, input);
            }
            child = next;
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
            match formatting_context::layout_inside_child(run, None, None, child, self.layout_mode, child_input, true) {
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
        let mut child = self.first_child(graphics_box);
        while !child.is_invalid() {
            let next = self.next_sibling(child);
            if self.node_kind(child) == NodeKind::SVGPatternBox {
                self.layout_mask_or_clip(run, child);
            }
            child = next;
        }
    }

    fn layout_path_like_element(
        &mut self,
        run: &FormattingContextRun<'pass>,
        graphics_box: Node,
        input: LayoutInput,
        facts: SvgElementFacts,
    ) {
        // A shape's geometry is its own attributes and computed style against the viewport, so the
        // pass draws it. Text still asks the document: shaping it needs the font cascade and the
        // running text position, which is a computation to move rather than data to publish.
        let path = if self.node_kind(graphics_box) == NodeKind::SVGGeometryBox {
            self.svg_geometry_path(graphics_box)
        } else {
            // SAFETY: The callback computes geometry synchronously and transfers
            // sole ownership of a heap-allocated path into the result.
            let result = unsafe {
                (self.callbacks.host.compute_svg_path)(
                    self.callbacks.host.context,
                    self.callbacks.shell(graphics_box),
                    FfiSvgPathRequest {
                        viewport_width: self.viewport_width,
                        viewport_height: self.viewport_height,
                        current_text_position: self.current_text_position,
                    },
                )
            };
            // Descendant and following text elements continue from the text position after this
            // element's own text run.
            self.current_text_position = result.text_position_after;
            // SAFETY: The callback just handed over its one owning pointer.
            unsafe { libgfx_rust::path::OwnedPath::adopt(result.path_handle) }
        };

        if self.node_kind(graphics_box) == NodeKind::SVGTextBox {
            // <text> and <tspan> elements can contain more text elements.
            let mut child = self.first_child(graphics_box);
            while !child.is_invalid() {
                let next = self.next_sibling(child);
                if matches!(self.node_kind(child), NodeKind::SVGTextBox | NodeKind::SVGTextPathBox) {
                    self.layout_graphics_element(run, child, input);
                }
                child = next;
            }
        }
        let [x, y, width, height] = path.bounding_box();
        let mut bounding_box = float_rect_to_css_pixels(FfiFloatRect { x, y, width, height });
        // Stroke increases the path's size by stroke_width/2 per side.
        let stroke_width = CssPixels::nearest_value_for_f32(facts.visible_stroke_width);
        bounding_box.inflate(stroke_width, stroke_width);

        let used_pointer = self.used_values(graphics_box);
        let used = &used_pointer;
        used.set_content_inline_size(bounding_box.width);
        used.set_content_block_size(bounding_box.height);
        self.used_values(graphics_box).rare_data_mut().computed_svg_path = Some(std::rc::Rc::new(path));
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
        assert!(kind_is_svg_resource_box(kind));
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
        let mut child = self.first_child(container);
        while !child.is_invalid() {
            let next = self.next_sibling(child);
            // Masks/clips/patterns do not change the bounding box of their parents.
            if NodeFacts::new(&self.callbacks, child).is_box() && !kind_is_svg_resource_box(self.node_kind(child)) {
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
            child = next;
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
