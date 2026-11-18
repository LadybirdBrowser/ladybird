/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::Observer;

use crate::css::computed_value_types::SVG_PAINT_URL;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::paint_order;
use crate::css::svg_paint::svg_paint_color;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::display_list::commands::DisplayListGradientSpreadMethod;
use crate::painting::display_list::recorder::{
    ColorStops, FillPathParams, PaintStyle, PaintStyleOrColor, StrokePathParams,
};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::host::{FfiSvgGradientKind, FfiSvgGradientSpreadMethod};
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::svg_paint_resources::{
    PublishedSvgGradient, PublishedSvgPaintServer, PublishedSvgPattern, SvgPaintResourceKind,
};
use crate::svg::stroke_style::{SvgStrokeStyle, svg_stroke_style_for_percentage_basis};
use libgfx_rust::{AffineTransform, CapStyle, Color, FloatRect, JoinStyle, ShouldAntiAlias, WindingRule};

#[derive(Clone, Copy, Default)]
struct SvgPaintFacts {
    contributes_to_clip_path: bool,
    should_anti_alias: bool,
    clip_rule_winding: i32,
    fill_winding: i32,
    fill_opacity: f32,
    fill_color: Option<u32>,
    stroke_color: Option<u32>,
    stroke_opacity: f32,
    cap_style: CapStyle,
    join_style: JoinStyle,
    miter_limit: f32,
    stroke_width: f32,
    stroke_dashoffset: f32,
    non_scaling_stroke: bool,
    paint_order: [u8; 3],
    paint_order_count: u32,
    references_paint_server: bool,
}

fn svg_paint_facts<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
) -> (SvgPaintFacts, Vec<f32>) {
    use crate::css::css_enums::fill_rule;
    let layout_arena = recorder.source;
    let mut facts = SvgPaintFacts::default();
    let Some(style) = layout_arena.node_style_if_live(paintable) else {
        return (facts, Vec::new());
    };
    let svg = style.inherited_svg();

    facts.contributes_to_clip_path =
        style.visibility() == crate::css::css_enums::visibility::VISIBLE && !style.display().is_none();
    facts.should_anti_alias = !matches!(
        svg.shape_rendering,
        crate::css::css_enums::shape_rendering::OPTIMIZESPEED | crate::css::css_enums::shape_rendering::CRISPEDGES
    );

    let to_winding = |rule: u8| {
        if rule == fill_rule::EVENODD {
            WindingRule::EvenOdd as i32
        } else {
            WindingRule::Nonzero as i32
        }
    };
    facts.clip_rule_winding = to_winding(svg.clip_rule);
    facts.fill_winding = to_winding(svg.fill_rule);
    facts.fill_opacity = svg.fill_opacity;
    facts.stroke_opacity = svg.stroke_opacity;
    facts.fill_color = svg_paint_color(&svg.fill, style.current_color());
    facts.stroke_color = svg_paint_color(&svg.stroke, style.current_color());
    facts.references_paint_server = svg.fill.kind == SVG_PAINT_URL || svg.stroke.kind == SVG_PAINT_URL;
    let stroke = svg_stroke_style(layout_arena, paintable, &style);
    facts.cap_style = stroke.cap_style;
    facts.join_style = stroke.join_style;
    facts.miter_limit = stroke.miter_limit;
    facts.non_scaling_stroke = stroke.non_scaling_stroke;
    facts.paint_order = svg.paint_order;
    facts.paint_order_count = 3;
    facts.stroke_width = stroke.width;
    facts.stroke_dashoffset = stroke.dash_offset;
    (facts, stroke.dash_array)
}

pub(crate) fn svg_stroke_style(
    layout_arena: &impl GeometryRead,
    paintable: NodeSlotId,
    style: &ComputedValuesView<'_>,
) -> SvgStrokeStyle {
    let basis = crate::painting::paintable_geometry::committed_svg_geometry(layout_arena, paintable)
        .map_or_else(Default::default, |geometry| geometry.percentage_basis());
    let mut stroke = svg_stroke_style_for_percentage_basis(style, basis);
    if !stroke.dash_array.is_empty()
        && let Some(path_length) =
            crate::painting::paintable_geometry::committed_svg_author_path_length(layout_arena, paintable)
    {
        // https://svgwg.org/svg2-draft/paths.html#PathLengthAttribute
        // The user agent will scale all distance-along-a-path computations by the ratio of 'pathLength' to the user
        // agent's own computed value for total path length.
        // AD-HOC: The spec says "'pathLength' has no effect on percentage distance-along-a-path calculations", but a
        //         percentage in the dash array or offset is resolved first and then scaled like any other length, for
        //         web compatibility.
        if path_length == 0.0 {
            // A value of zero is valid and must be treated as a scaling factor of infinity. A value of zero scaled
            // infinitely must remain zero, while any non-percentage value greater than zero must become +Infinity.
            // AD-HOC: The stroke draws solid, for web compatibility, rather than keeping each zero-length dash or
            //         gap at zero; that's the same stroke for any dash array without a zero in it.
            stroke.dash_array.clear();
            stroke.dash_offset = 0.0;
        } else if path_length > 0.0
            && let Some(path) = crate::painting::paintable_geometry::committed_svg_path(layout_arena, paintable)
        {
            let scale = path.length() / path_length;
            stroke.dash_array.iter_mut().for_each(|dash| *dash *= scale);
            stroke.dash_offset *= scale;
        }
    }
    stroke
}

fn should_anti_alias(value: bool) -> ShouldAntiAlias {
    if value {
        ShouldAntiAlias::Yes
    } else {
        ShouldAntiAlias::No
    }
}

fn affine(values: [f32; 6]) -> AffineTransform {
    AffineTransform { values }
}

fn spread_method_of(spread_method: FfiSvgGradientSpreadMethod) -> DisplayListGradientSpreadMethod {
    match spread_method {
        FfiSvgGradientSpreadMethod::Pad => DisplayListGradientSpreadMethod::Pad,
        FfiSvgGradientSpreadMethod::Repeat => DisplayListGradientSpreadMethod::Repeat,
        FfiSvgGradientSpreadMethod::Reflect => DisplayListGradientSpreadMethod::Reflect,
    }
}

fn gradient_paint_transform(
    gradient: &PublishedSvgGradient,
    paint_context: &SvgPaintContext,
) -> crate::painting::display_list::commands::OptionalAffineTransform {
    use libgfx_rust::matrix::multiply_affine;
    let mapped_bounding_box = paint_context.paint_transform.map_rect(paint_context.path_bounding_box);
    let mut transform = AffineTransform::identity();
    transform.values[4] = -mapped_bounding_box.x;
    transform.values[5] = -mapped_bounding_box.y;
    transform = multiply_affine(transform, paint_context.paint_transform);
    if gradient.description.gradient_transform.has_value {
        transform = multiply_affine(transform, gradient.description.gradient_transform.value);
    }
    crate::painting::display_list::commands::OptionalAffineTransform {
        value: transform,
        has_value: true,
    }
}

fn gradient_paint_style(gradient: &PublishedSvgGradient, paint_context: &SvgPaintContext) -> PaintStyle {
    let description = &gradient.description;
    let bounding_box = paint_context.object_bounding_box;
    let viewport = paint_context.viewport;
    let point_in_bounding_box = |x: f32, y: f32| libgfx_rust::FloatPoint {
        x: bounding_box.x + x * bounding_box.width,
        y: bounding_box.y + y * bounding_box.height,
    };
    let point_in_user_space =
        |x: crate::layout::svg_formatting_context::FfiSvgNumberPercentage,
         y: crate::layout::svg_formatting_context::FfiSvgNumberPercentage| {
            libgfx_rust::FloatPoint {
                x: x.resolve_relative_to(viewport.width),
                y: y.resolve_relative_to(viewport.height),
            }
        };
    let color_stops = ColorStops {
        colors: gradient.stops.iter().map(|stop| stop.color).collect(),
        positions: gradient.stops.iter().map(|stop| stop.position).collect(),
        repeating: false,
    };
    let gradient_transform = gradient_paint_transform(gradient, paint_context);
    let spread_method = spread_method_of(description.spread_method);
    let color_space = description.color_space;
    match description.kind {
        FfiSvgGradientKind::Radial => {
            let (start_center, start_radius, end_center, end_radius) = if description.units_are_object_bounding_box {
                (
                    point_in_bounding_box(description.fx.value, description.fy.value),
                    description.fr.value * bounding_box.width,
                    point_in_bounding_box(description.cx.value, description.cy.value),
                    description.r.value * bounding_box.width,
                )
            } else {
                (
                    point_in_user_space(description.fx, description.fy),
                    description.fr.resolve_relative_to(viewport.width),
                    point_in_user_space(description.cx, description.cy),
                    description.r.resolve_relative_to(viewport.width),
                )
            };
            PaintStyle::RadialGradient {
                gradient_transform,
                spread_method,
                color_space,
                color_stops,
                start_center,
                start_radius,
                end_center,
                end_radius,
            }
        }
        FfiSvgGradientKind::Linear => {
            let (start_point, end_point) = if description.units_are_object_bounding_box {
                (
                    point_in_bounding_box(description.x1.value, description.y1.value),
                    point_in_bounding_box(description.x2.value, description.y2.value),
                )
            } else {
                (
                    point_in_user_space(description.x1, description.y1),
                    point_in_user_space(description.x2, description.y2),
                )
            };
            PaintStyle::LinearGradient {
                gradient_transform,
                spread_method,
                color_space,
                color_stops,
                start_point,
                end_point,
            }
        }
    }
}

pub(crate) struct SvgPaintContext {
    pub viewport: FloatRect,
    /// The bounds of the path as recorded, from which a gradient is positioned when it is drawn.
    pub path_bounding_box: FloatRect,
    pub object_bounding_box: FloatRect,
    pub paint_transform: AffineTransform,
    pub content_scale: libgfx_rust::FloatSize,
}

fn pattern_paint_style<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    pattern: &PublishedSvgPattern,
    paint_context: &SvgPaintContext,
) -> Option<PaintStyle> {
    use libgfx_rust::matrix::{affine_to_matrix, multiply_affine};
    let description = &pattern.description;
    let pattern_box = description.pattern_box;
    if !recorder.source.paintable_row_is_populated(pattern_box) {
        return None;
    }
    let bounding_box = paint_context.object_bounding_box;
    let viewport = paint_context.viewport;
    let (tile_x, tile_y, tile_width, tile_height) = if description.units_are_object_bounding_box {
        (
            description.x.value * bounding_box.width + bounding_box.x,
            description.y.value * bounding_box.height + bounding_box.y,
            description.width.value * bounding_box.width,
            description.height.value * bounding_box.height,
        )
    } else {
        (
            description.x.resolve_relative_to(viewport.width),
            description.y.resolve_relative_to(viewport.height),
            description.width.resolve_relative_to(viewport.width),
            description.height.resolve_relative_to(viewport.height),
        )
    };
    if tile_width <= 0.0 || tile_height <= 0.0 {
        return None;
    }
    let tile_rect = paint_context
        .paint_transform
        .map_rect(FloatRect::new(tile_x, tile_y, tile_width, tile_height));
    if tile_rect.is_empty() {
        return None;
    }
    let mut content_scale = paint_context.content_scale;
    if !(content_scale.width > 0.0 && content_scale.height > 0.0) {
        content_scale = libgfx_rust::FloatSize {
            width: 1.0,
            height: 1.0,
        };
    }
    let device_scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    let mut recorded_to_surface = AffineTransform::identity().scaled(content_scale.width, content_scale.height);
    if !description.has_view_box {
        let mut content_to_tile_transform = AffineTransform::identity();
        if description.content_units_are_object_bounding_box {
            content_to_tile_transform = AffineTransform::identity()
                .translated(bounding_box.x * device_scale, bounding_box.y * device_scale)
                .scaled(bounding_box.width, bounding_box.height);
        }
        recorded_to_surface = multiply_affine(
            recorded_to_surface.translated(-tile_rect.x, -tile_rect.y),
            content_to_tile_transform,
        );
    }
    let tile_content_transform = affine_to_matrix(recorded_to_surface);
    let mut pattern_transform = crate::painting::display_list::commands::OptionalAffineTransform::default();
    let user_space_pattern_transform = if pattern.css_transform.is_empty() {
        description
            .pattern_transform_attribute
            .has_value
            .then_some(description.pattern_transform_attribute.value)
    } else {
        let pattern_box_style = recorder.source.node_style_if_live(pattern_box)?;
        let reference_box = crate::painting::visual_context::node_values::transform_reference_box(
            pattern_box_style,
            recorder.source,
            pattern_box,
        );
        Some(
            crate::css::transform_functions::multiply_transform_functions(
                libgfx_rust::FloatMatrix4x4::identity(),
                &pattern.css_transform,
                reference_box,
            )
            .extract_2d_affine(),
        )
    };
    if let Some(user_space_pattern_transform) = user_space_pattern_transform {
        user_space_pattern_transform.inverse()?;
        if let Some(inverse) = paint_context.paint_transform.inverse() {
            pattern_transform = crate::painting::display_list::commands::OptionalAffineTransform {
                value: multiply_affine(
                    multiply_affine(paint_context.paint_transform, user_space_pattern_transform),
                    inverse,
                ),
                has_value: true,
            };
        }
    }
    // The player rasterizes the tile records into a surface of this size.
    let tile_surface_rect = FloatRect::new(
        0.0,
        0.0,
        (tile_rect.width * content_scale.width).ceil(),
        (tile_rect.height * content_scale.height).ceil(),
    );
    Some(PaintStyle::Pattern {
        tile_records: recorder.pattern_tile_records(pattern_box, tile_content_transform, tile_surface_rect),
        tile_rect,
        content_scale,
        pattern_transform,
    })
}

fn paint_server_style<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    is_stroke: bool,
    paint_context: &SvgPaintContext,
) -> Option<PaintStyle> {
    let kind = if is_stroke {
        SvgPaintResourceKind::Stroke
    } else {
        SvgPaintResourceKind::Fill
    };
    let published = recorder.source.published_svg_paint_server(paintable, kind)?;
    match &*published {
        PublishedSvgPaintServer::Gradient(gradient) => Some(gradient_paint_style(gradient, paint_context)),
        PublishedSvgPaintServer::Pattern(pattern) => pattern_paint_style(recorder, pattern, paint_context),
        PublishedSvgPaintServer::None => None,
    }
}

fn references_pattern<O: Observer>(recorder: &PaintRecorder<'_, O>, paintable: NodeSlotId) -> bool {
    [SvgPaintResourceKind::Fill, SvgPaintResourceKind::Stroke]
        .iter()
        .any(|kind| {
            recorder
                .source
                .published_svg_paint_server(paintable, *kind)
                .is_some_and(|published| matches!(&*published, PublishedSvgPaintServer::Pattern(_)))
        })
}

pub(crate) fn paint_path<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId, phase: PaintPhase) {
    paint_path_parts(recorder, paintable, phase, PathParts::BeforeChildren);
}

/// Which parts of a path to paint. A text content element paints in document order, so its text that follows a child
/// element paints after that child, apart from the text before it.
enum PathParts {
    /// All of a path that paints in one go, or the parts that come before every child element.
    BeforeChildren,
    /// The parts in the given range of the parts the box committed.
    Range(std::ops::Range<usize>),
}

/// How many parts the text content element `paintable` paints before its first child element; None where its path
/// paints in one go.
pub(crate) fn text_parts_before_children<O: Observer>(
    recorder: &PaintRecorder<'_, O>,
    paintable: NodeSlotId,
) -> Option<usize> {
    let segments = crate::painting::paintable_geometry::committed_svg_text_segments(recorder.source, paintable)?;
    Some(
        segments
            .iter()
            .take_while(|segment| segment.after_child.is_invalid())
            .count(),
    )
}

/// Paints the parts of the text content element `paintable` that follow `child`, one of its child elements.
/// `next_part` is the first part that hasn't painted yet, which this moves past the parts it paints.
pub(crate) fn paint_text_after_child<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    child: NodeSlotId,
    next_part: &mut usize,
) {
    let Some(segments) = crate::painting::paintable_geometry::committed_svg_text_segments(recorder.source, paintable)
    else {
        return;
    };
    let start = (*next_part).min(segments.len());
    let end = start
        + segments[start..]
            .iter()
            .take_while(|segment| segment.after_child == child)
            .count();
    if end > start {
        paint_path_parts(
            recorder,
            paintable,
            PaintPhase::Foreground,
            PathParts::Range(start..end),
        );
        *next_part = end;
    }
}

/// Paints the parts of the text content element `paintable` from `next_part` on: those left once its child elements
/// have painted, which follow a child that didn't.
pub(crate) fn paint_text_after_children<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    next_part: usize,
) {
    let Some(segments) = crate::painting::paintable_geometry::committed_svg_text_segments(recorder.source, paintable)
    else {
        return;
    };
    if next_part < segments.len() {
        paint_path_parts(
            recorder,
            paintable,
            PaintPhase::Foreground,
            PathParts::Range(next_part..segments.len()),
        );
    }
}

fn paint_path_parts<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    phase: PaintPhase,
    path_parts: PathParts,
) {
    let Some(computed_path) = crate::painting::paintable_geometry::committed_svg_path(recorder.source, paintable)
    else {
        return;
    };
    let (facts, dash_array) = svg_paint_facts(recorder, paintable);
    // A pattern tile is rendered from the pattern's own subtree, and a textPath follows another
    // element's geometry; neither stamps this shape when it changes. A gradient is
    // published by the paint resource sync, which stamps the shapes using it.
    let output_is_resolved_through_another_element = (facts.references_paint_server
        && references_pattern(recorder, paintable))
        || recorder.source.node_kind_if_live(paintable) == Some(NodeKind::SVGTextPathBox);
    if output_is_resolved_through_another_element {
        recorder.mark_live_producer();
    }
    if recorder.draws_clip_path_geometry() {
        if !facts.contributes_to_clip_path {
            return;
        }
    } else if !recorder.is_visible(paintable) {
        return;
    }

    if matches!(path_parts, PathParts::BeforeChildren) {
        super::paint_base(recorder, paintable, phase);
    }

    if phase != PaintPhase::Foreground {
        return;
    }

    // Content below the viewport transform node records in user units scaled by the device pixel
    // ratio; the visual context tree applies the viewport and element transforms at replay.
    let device_scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    let paint_transform = [device_scale, 0.0, 0.0, device_scale, 0.0, 0.0];
    let segments = crate::painting::paintable_geometry::committed_svg_text_segments(recorder.source, paintable);
    // Each part of the path to paint, with its bounding box in user units, and the glyphs of color fonts that paint
    // along with it.
    type Part<'a> = (
        libgfx_rust::path::Path,
        FloatRect,
        &'a [crate::layout::fragment_tree::SvgColorGlyphRun],
    );
    let parts: Vec<Part<'_>> = match &segments {
        Some(segments) => {
            let range = match path_parts {
                PathParts::BeforeChildren => {
                    0..segments
                        .iter()
                        .take_while(|segment| segment.after_child.is_invalid())
                        .count()
                }
                PathParts::Range(range) => range,
            };
            segments[range]
                .iter()
                .map(|segment| {
                    (
                        segment.path.copy_transformed(paint_transform),
                        FloatRect::from_array(segment.path.bounding_box()),
                        segment.color_glyph_runs.as_slice(),
                    )
                })
                .collect()
        }
        None => vec![(
            computed_path.copy_transformed(paint_transform),
            FloatRect::from_array(computed_path.bounding_box()),
            &[][..],
        )],
    };
    let anti_alias = should_anti_alias(facts.should_anti_alias);

    if recorder.draws_clip_path_geometry() {
        // https://drafts.fxtf.org/css-masking/#ClipPathElement: the raw geometry of each child
        // element, exclusive of rendering properties, defines a 1-bit mask.
        // AD-HOC: The glyphs of a color font have no outlines, so they're left out of the clipping path.
        for (path, _, _) in &parts {
            recorder.recorder.fill_path(FillPathParams {
                force_dark_role: ForceDarkRole::Svg,
                path,
                opacity: 1.0,
                paint_style_or_color: PaintStyleOrColor::Color(Color::from_rgb(0, 0, 0)),
                winding_rule: WindingRule::from_raw(facts.clip_rule_winding),
                should_anti_alias: anti_alias,
            });
        }
        return;
    }

    let geometry =
        crate::painting::paintable_geometry::committed_svg_geometry(recorder.source, paintable).unwrap_or_default();
    for (path, path_bounding_box, color_glyph_runs) in &parts {
        // NB: A paint server is placed from the bounding box of the path it paints, so each part is painted with its
        //     own.
        let paint_context = SvgPaintContext {
            viewport: geometry.viewport_user_rect,
            path_bounding_box: *path_bounding_box,
            object_bounding_box: geometry.object_bounding_box,
            paint_transform: affine(paint_transform),
            content_scale: recorder.own_accumulated_2d_scale(paintable),
        };
        for index in 0..facts.paint_order_count as usize {
            match facts.paint_order[index] {
                paint_order::FILL => {
                    let fill_opacity = facts.fill_opacity;
                    let fill_winding = WindingRule::from_raw(facts.fill_winding);
                    // NB: The glyphs of a color font paint with their own colors, wherever the text has a fill.
                    if !color_glyph_runs.is_empty() {
                        let opacity = if paint_server_style(recorder, paintable, false, &paint_context).is_some() {
                            Some(fill_opacity)
                        } else {
                            facts
                                .fill_color
                                .map(|fill_color| fill_opacity * f32::from(Color(fill_color).alpha()) / 255.0)
                        };
                        if let Some(opacity) = opacity {
                            paint_color_glyph_runs(recorder, paintable, color_glyph_runs, opacity, device_scale);
                        }
                    }
                    if let Some(paint_style) = paint_server_style(recorder, paintable, false, &paint_context) {
                        recorder.recorder.fill_path(FillPathParams {
                            force_dark_role: ForceDarkRole::Svg,
                            path,
                            opacity: fill_opacity,
                            paint_style_or_color: PaintStyleOrColor::PaintStyle(paint_style),
                            winding_rule: fill_winding,
                            should_anti_alias: anti_alias,
                        });
                    } else if let Some(fill_color) = facts.fill_color {
                        recorder.recorder.fill_path(FillPathParams {
                            force_dark_role: ForceDarkRole::Svg,
                            path,
                            opacity: 1.0,
                            paint_style_or_color: PaintStyleOrColor::Color(
                                Color(fill_color).with_opacity(fill_opacity),
                            ),
                            winding_rule: fill_winding,
                            should_anti_alias: anti_alias,
                        });
                    }
                }
                paint_order::STROKE => {
                    // https://svgwg.org/svg2-draft/painting.html#PaintingVectorEffects
                    // With the non-scaling-stroke vector effect, stroke outline shall be calculated in
                    // the "host" coordinate space instead of user coordinate system.
                    let mut stroke_scale = device_scale;
                    if facts.non_scaling_stroke {
                        let accumulated_scale = recorder.own_accumulated_2d_scale(paintable);
                        let accumulated_scale_area = accumulated_scale.width * accumulated_scale.height;
                        if accumulated_scale_area > 0.0 {
                            stroke_scale = device_scale / accumulated_scale_area.sqrt();
                        }
                    }
                    let stroke_thickness = facts.stroke_width * stroke_scale;
                    let stroke_dasharray: Vec<f32> = dash_array.iter().map(|value| value * stroke_scale).collect();
                    let stroke_dashoffset = facts.stroke_dashoffset * stroke_scale;
                    let stroke_opacity = facts.stroke_opacity;

                    // NB: A glyph of a color font has no outline to stroke, so a stroke paints the glyph itself.
                    if !color_glyph_runs.is_empty() && stroke_thickness > 0.0 {
                        let opacity = if paint_server_style(recorder, paintable, true, &paint_context).is_some() {
                            Some(stroke_opacity)
                        } else {
                            facts
                                .stroke_color
                                .map(|stroke_color| stroke_opacity * f32::from(Color(stroke_color).alpha()) / 255.0)
                        };
                        if let Some(opacity) = opacity {
                            paint_color_glyph_runs(recorder, paintable, color_glyph_runs, opacity, device_scale);
                        }
                    }

                    if let Some(paint_style) = paint_server_style(recorder, paintable, true, &paint_context) {
                        recorder.recorder.stroke_path(StrokePathParams {
                            force_dark_role: ForceDarkRole::Svg,
                            cap_style: facts.cap_style,
                            join_style: facts.join_style,
                            miter_limit: facts.miter_limit,
                            dash_array: stroke_dasharray,
                            dash_offset: stroke_dashoffset,
                            path,
                            opacity: stroke_opacity,
                            paint_style_or_color: PaintStyleOrColor::PaintStyle(paint_style),
                            thickness: stroke_thickness,
                            should_anti_alias: anti_alias,
                        });
                    } else if let Some(stroke_color) = facts.stroke_color {
                        recorder.recorder.stroke_path(StrokePathParams {
                            force_dark_role: ForceDarkRole::Svg,
                            cap_style: facts.cap_style,
                            join_style: facts.join_style,
                            miter_limit: facts.miter_limit,
                            dash_array: stroke_dasharray,
                            dash_offset: stroke_dashoffset,
                            path,
                            opacity: 1.0,
                            paint_style_or_color: PaintStyleOrColor::Color(
                                Color(stroke_color).with_opacity(stroke_opacity),
                            ),
                            thickness: stroke_thickness,
                            should_anti_alias: anti_alias,
                        });
                    }
                }
                // FIXME: Implement marker painting
                _ => {}
            }
        }
    }
}

fn paint_color_glyph_runs<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    runs: &[crate::layout::fragment_tree::SvgColorGlyphRun],
    opacity: f32,
    device_scale: f32,
) {
    use crate::painting::display_list::commands::{DisplayListGlyph, FontResourceId};
    use crate::painting::display_list::recorder::GlyphRunForRecording;

    let Some(style) = recorder.source.node_style_if_live(paintable) else {
        return;
    };
    let font_smoothing = style.inherited_text().font_smoothing;
    for run in runs {
        let font_id = recorder.register_font(&run.font);
        let left = (run.bounds.x * device_scale).floor();
        let top = (run.bounds.y * device_scale).floor();
        let device_rect = libgfx_rust::IntRect::new(
            left as i32,
            top as i32,
            ((run.bounds.right() * device_scale).ceil() - left) as i32,
            ((run.bounds.bottom() * device_scale).ceil() - top) as i32,
        );
        recorder.recorder.draw_glyph_run(
            libgfx_rust::FloatPoint { x: 0.0, y: 0.0 },
            GlyphRunForRecording {
                font_smoothing,
                font_id: FontResourceId(font_id),
                glyphs: run.glyphs.iter().map(|glyph| DisplayListGlyph {
                    position: libgfx_rust::FloatPoint { x: glyph.x, y: glyph.y },
                    glyph_id: glyph.glyph_id,
                }),
            },
            Color::from_rgb(0, 0, 0).with_opacity(opacity),
            0.0,
            device_rect,
            f64::from(device_scale),
            libgfx_rust::Orientation::Horizontal,
            device_rect,
            ForceDarkRole::Svg,
        );
    }
}

pub(crate) fn svg_image_unquantized_device_rect(
    layout_arena: &impl PaintRead,
    paintable: NodeSlotId,
    pixel_ratio: f64,
) -> FloatRect {
    let device_scale = pixel_ratio as f32;
    let css_rect = absolute_rect(layout_arena, paintable);
    FloatRect::new(
        css_rect.x.to_float() * device_scale,
        css_rect.y.to_float() * device_scale,
        css_rect.width.to_float() * device_scale,
        css_rect.height.to_float() * device_scale,
    )
}

// https://svgwg.org/svg2-draft/coords.html#PreserveAspectRatioAttribute
// Where an image of `natural_size` is drawn to fit `image_rect` as its preserveAspectRatio says, taking its own size as
// its view box.
pub(crate) fn image_draw_rect(
    image_rect: FloatRect,
    natural_size: (f32, f32),
    align: u8,
    meet_or_slice: u8,
) -> FloatRect {
    use crate::layout::svg_formatting_context::{
        MEET_OR_SLICE_SLICE, PRESERVE_ASPECT_RATIO_NONE, PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN,
    };
    // NB: With "none", the image is scaled non-uniformly to fill the rectangle exactly.
    if natural_size.0 <= 0.0 || natural_size.1 <= 0.0 || align == PRESERVE_ASPECT_RATIO_NONE {
        return image_rect;
    }
    let scale_x = image_rect.width / natural_size.0;
    let scale_y = image_rect.height / natural_size.1;
    // "meet" shows all of the image within the rectangle; "slice" covers all of the rectangle with the image.
    let scale = if meet_or_slice == MEET_OR_SLICE_SLICE {
        scale_x.max(scale_y)
    } else {
        scale_x.min(scale_y)
    };
    let width = natural_size.0 * scale;
    let height = natural_size.1 * scale;
    // NB: The alignments count up from xMinYMin through min, mid and max along x, then through min, mid and max along
    //     y, each putting none, half or all of the space left over before the image.
    let alignment = align.saturating_sub(PRESERVE_ASPECT_RATIO_X_MIN_Y_MIN);
    let offset = |alignment_in_axis: u8, free_space: f32| match alignment_in_axis {
        0 => 0.0,
        1 => free_space / 2.0,
        _ => free_space,
    };
    FloatRect::new(
        image_rect.x + offset(alignment % 3, image_rect.width - width),
        image_rect.y + offset(alignment / 3, image_rect.height - height),
        width,
        height,
    )
}

pub(crate) fn paint_image_element<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    phase: PaintPhase,
) {
    // NB: An image has no geometry, so it contributes nothing to a clipping path.
    if recorder.draws_clip_path_geometry() {
        return;
    }
    if !recorder.is_visible(paintable) {
        return;
    }

    super::paint_base(recorder, paintable, phase);

    if phase != PaintPhase::Foreground {
        return;
    }

    let image = recorder
        .source
        .replaced_paint_facts(paintable)
        .and_then(|facts| facts.image())
        .unwrap_or_default();
    if image.content == crate::painting::image_content::ImageContent::None {
        return;
    }

    let image_rect =
        svg_image_unquantized_device_rect(recorder.source, paintable, recorder.inputs.device_pixels_per_css_pixel);
    let natural_size = match (image.natural.width, image.natural.height) {
        (Some(width), Some(height)) => (width.to_float(), height.to_float()),
        _ => (image_rect.width, image_rect.height),
    };
    let draw_rect = image_draw_rect(
        image_rect,
        natural_size,
        image.preserve_aspect_ratio_align,
        image.preserve_aspect_ratio_meet_or_slice,
    );
    if draw_rect.is_empty() {
        return;
    }

    // https://svgwg.org/svg2-draft/embedded.html#ImageElement
    // Unless over-ridden by the author, images will therefore be clipped to the positioning
    // rectangle defined by the geometry properties.
    let (overflow_is_visible, image_rendering) =
        recorder
            .source
            .node_style_if_live(paintable)
            .map_or((false, 0), |style| {
                use crate::css::css_enums::overflow;
                (
                    style.overflow_x() == overflow::VISIBLE && style.overflow_y() == overflow::VISIBLE,
                    style.image_rendering(),
                )
            });
    let draw_rect_needs_clip = !overflow_is_visible && !image_rect.contains_rect(draw_rect);
    let image_rect_clip = draw_rect_needs_clip.then(|| PendingInlineClip::intersecting_float_rect(image_rect));
    recorder.record_with_inline_clips(image_rect_clip.as_slice(), |recorder| {
        crate::painting::record::paint::replaced::paint_replaced_image_content(
            recorder,
            paintable,
            &image.content,
            draw_rect,
            image_rendering,
        );
    });
}
