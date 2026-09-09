/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::Observer;

use crate::css::computed_value_types::{ComputedSvgPaint, SVG_PAINT_COLOR, SVG_PAINT_NONE, SVG_PAINT_URL};
use crate::css::css_enums::paint_order;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::display_list::commands::DisplayListGradientSpreadMethod;
use crate::painting::display_list::recorder::{
    ColorStops, FillPathParams, PaintStyle, PaintStyleOrColor, StrokePathParams,
};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::host::{
    FfiSvgGradientKind, FfiSvgGradientSpreadMethod, FfiSvgPaintContext, FfiSvgPaintStyle, FfiSvgPaintStyleKind,
};
use crate::painting::node_painting;
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::svg_paint_resources::{PublishedSvgGradient, PublishedSvgPaintServer, SvgPaintResourceKind};
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
    has_viewport: bool,
    viewport: [f32; 4],
    references_paint_server: bool,
}

pub(crate) fn svg_paint_color(paint: &ComputedSvgPaint) -> Option<u32> {
    match paint.kind {
        SVG_PAINT_NONE => None,
        SVG_PAINT_COLOR => Some(paint.color),
        _ => paint.has_color.then_some(paint.color),
    }
}

fn svg_paint_facts<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
) -> (SvgPaintFacts, Vec<f32>) {
    use crate::css::css_enums::{fill_rule, stroke_linecap, stroke_linejoin, vector_effect};
    let layout_arena = recorder.layout_arena;
    let viewport = layout_arena
        .node_kind_if_live(paintable)
        .is_some_and(node_painting::is_svg_path)
        .then(|| crate::painting::svg_viewport::nearest_svg_viewport_user_rect(layout_arena, paintable))
        .flatten();
    let mut facts = SvgPaintFacts {
        has_viewport: viewport.is_some(),
        viewport: viewport.map_or([0.0; 4], |rect| [rect.x, rect.y, rect.width, rect.height]),
        ..SvgPaintFacts::default()
    };
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
    facts.fill_color = svg_paint_color(&svg.fill);
    facts.stroke_color = svg_paint_color(&svg.stroke);
    facts.references_paint_server = svg.fill.kind == SVG_PAINT_URL || svg.stroke.kind == SVG_PAINT_URL;
    facts.cap_style = match svg.stroke_linecap {
        stroke_linecap::ROUND => CapStyle::Round,
        stroke_linecap::SQUARE => CapStyle::Square,
        _ => CapStyle::Butt,
    };
    facts.join_style = match svg.stroke_linejoin {
        stroke_linejoin::ROUND => JoinStyle::Round,
        stroke_linejoin::BEVEL => JoinStyle::Bevel,
        _ => JoinStyle::Miter,
    };
    facts.miter_limit = svg.stroke_miterlimit as f32;
    facts.non_scaling_stroke = style.svg_reset().vector_effect == vector_effect::NON_SCALING_STROKE;
    facts.paint_order = svg.paint_order;
    facts.paint_order_count = 3;

    let basis = crate::painting::paintable_geometry::committed_svg_viewport_percentage_basis(layout_arena, paintable);
    let resolve = |handle: &crate::css::computed_value_types::ComputedStyleValueHandle, default: f32| {
        handle
            .length_percentage()
            .map_or(default, |value| value.to_px(basis).to_double() as f32)
    };
    facts.stroke_width = resolve(&svg.stroke_width, 1.0);
    facts.stroke_dashoffset = resolve(&svg.stroke_dashoffset, 0.0);

    let mut dash_array: Vec<f32> = svg
        .stroke_dasharray
        .as_slice()
        .iter()
        .map(|dash| {
            if dash.is_number {
                dash.number as f32
            } else {
                resolve(&dash.value, 0.0)
            }
        })
        .collect();
    if dash_array.len() % 2 == 1 {
        dash_array.extend_from_within(..);
    }
    if dash_array.iter().any(|value| *value < 0.0) || dash_array.iter().all(|value| *value == 0.0) {
        dash_array.clear();
    }
    (facts, dash_array)
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

/// The transform that takes the gradient's coordinates, which are in the painted path's user
/// space, into the recorded space: the path's bounding box origin moves to zero, the paint
/// transform scales to device pixels, and gradientTransform applies last.
fn gradient_paint_transform(
    gradient: &PublishedSvgGradient,
    paint_context: &FfiSvgPaintContext,
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

/// Builds the paint style of a gradient a fill or stroke references, resolving its coordinates
/// the way SVG defines them: fractions of the painted path's bounding box for
/// objectBoundingBox units, and user units with percentages of the viewport otherwise.
fn gradient_paint_style(gradient: &PublishedSvgGradient, paint_context: &FfiSvgPaintContext) -> PaintStyle {
    let description = &gradient.description;
    let bounding_box = paint_context.path_bounding_box;
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

fn pattern_paint_style_from_ffi<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    style: &FfiSvgPaintStyle,
) -> Option<PaintStyle> {
    match style.kind {
        FfiSvgPaintStyleKind::Pattern => Some(PaintStyle::Pattern {
            tile_records: recorder.pattern_tile_records(style.pattern_paintable, style.tile_content_transform),
            tile_rect: style.tile_rect,
            content_scale: style.content_scale,
            pattern_transform: style.pattern_transform,
        }),
        _ => None,
    }
}

/// The paint style a fill or stroke url() resolves to, from the description the sync pass
/// published; a pattern's geometry still comes from the host.
fn paint_server_style<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    is_stroke: bool,
    paint_context: &FfiSvgPaintContext,
) -> Option<PaintStyle> {
    let kind = if is_stroke {
        SvgPaintResourceKind::Stroke
    } else {
        SvgPaintResourceKind::Fill
    };
    let published = recorder
        .layout_arena
        .svg_paint_resources()
        .published_paint_server(paintable, kind)?;
    match &*published {
        PublishedSvgPaintServer::Gradient(gradient) => Some(gradient_paint_style(gradient, paint_context)),
        PublishedSvgPaintServer::Pattern => {
            let style =
                recorder
                    .paint_host
                    .svg_paint_style(recorder.layout_node_shell(paintable), is_stroke, paint_context);
            pattern_paint_style_from_ffi(recorder, &style)
        }
        PublishedSvgPaintServer::None => None,
    }
}

pub(crate) fn paint_path<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId, phase: PaintPhase) {
    let Some(computed_path) = crate::painting::paintable_geometry::committed_svg_path(recorder.layout_arena, paintable)
    else {
        return;
    };
    let (facts, dash_array) = svg_paint_facts(recorder, paintable);
    let output_is_resolved_through_another_element = facts.references_paint_server
        || recorder.layout_arena.node_kind_if_live(paintable) == Some(NodeKind::SVGTextPathBox);
    if output_is_resolved_through_another_element {
        recorder.mark_open_captures_unsplicable();
    }
    if recorder.draws_clip_path_geometry() {
        if !facts.contributes_to_clip_path {
            return;
        }
    } else if !recorder.is_visible(paintable) {
        return;
    }

    super::paint_base(recorder, paintable, phase);

    if phase != PaintPhase::Foreground {
        return;
    }

    // Content below the viewport transform node records in user units scaled by the device pixel
    // ratio; the visual context tree applies the viewport and element transforms at replay.
    let device_scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    let paint_transform = [device_scale, 0.0, 0.0, device_scale, 0.0, 0.0];
    let path = computed_path.copy_transformed(paint_transform);
    let anti_alias = should_anti_alias(facts.should_anti_alias);

    if recorder.draws_clip_path_geometry() {
        // https://drafts.fxtf.org/css-masking/#ClipPathElement: the raw geometry of each child
        // element, exclusive of rendering properties, defines a 1-bit mask.
        recorder.recorder.fill_path(FillPathParams {
            force_dark_role: ForceDarkRole::Svg,
            path: &path,
            opacity: 1.0,
            paint_style_or_color: PaintStyleOrColor::Color(Color::from_rgb(0, 0, 0)),
            winding_rule: WindingRule::from_raw(facts.clip_rule_winding),
            should_anti_alias: anti_alias,
        });
        return;
    }

    let paint_context = crate::painting::host::FfiSvgPaintContext {
        viewport: if facts.has_viewport {
            FloatRect::from_array(facts.viewport)
        } else {
            FloatRect::default()
        },
        path_bounding_box: FloatRect::from_array(computed_path.bounding_box()),
        paint_transform: affine(paint_transform),
        content_scale: recorder.own_accumulated_2d_scale(paintable),
    };

    for index in 0..facts.paint_order_count as usize {
        match facts.paint_order[index] {
            paint_order::FILL => {
                let fill_opacity = facts.fill_opacity;
                let fill_winding = WindingRule::from_raw(facts.fill_winding);
                if let Some(paint_style) = paint_server_style(recorder, paintable, false, &paint_context) {
                    recorder.recorder.fill_path(FillPathParams {
                        force_dark_role: ForceDarkRole::Svg,
                        path: &path,
                        opacity: fill_opacity,
                        paint_style_or_color: PaintStyleOrColor::PaintStyle(paint_style),
                        winding_rule: fill_winding,
                        should_anti_alias: anti_alias,
                    });
                } else if let Some(fill_color) = facts.fill_color {
                    recorder.recorder.fill_path(FillPathParams {
                        force_dark_role: ForceDarkRole::Svg,
                        path: &path,
                        opacity: 1.0,
                        paint_style_or_color: PaintStyleOrColor::Color(Color(fill_color).with_opacity(fill_opacity)),
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

                if let Some(paint_style) = paint_server_style(recorder, paintable, true, &paint_context) {
                    recorder.recorder.stroke_path(StrokePathParams {
                        force_dark_role: ForceDarkRole::Svg,
                        cap_style: facts.cap_style,
                        join_style: facts.join_style,
                        miter_limit: facts.miter_limit,
                        dash_array: stroke_dasharray,
                        dash_offset: stroke_dashoffset,
                        path: &path,
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
                        path: &path,
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

pub(crate) fn svg_image_unquantized_device_rect(
    layout_arena: &impl PaintableRowsRead,
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
        .layout_arena
        .replaced_paint_facts(paintable)
        .and_then(|facts| facts.image())
        .unwrap_or_default();
    if !image.has_decoded_image_data {
        return;
    }

    let image_rect = svg_image_unquantized_device_rect(
        recorder.layout_arena,
        paintable,
        recorder.inputs.device_pixels_per_css_pixel,
    );
    let natural_size = if image.natural_width.has_value && image.natural_height.has_value {
        (
            image.natural_width.value.to_float(),
            image.natural_height.value.to_float(),
        )
    } else {
        (image_rect.width, image_rect.height)
    };
    // FIXME: Respect the preserveAspectRatio attribute instead of assuming its default value.
    let mut draw_rect = image_rect;
    if natural_size.0 > 0.0 && natural_size.1 > 0.0 {
        let contain_scale = (image_rect.width / natural_size.0).min(image_rect.height / natural_size.1);
        let width = natural_size.0 * contain_scale;
        let height = natural_size.1 * contain_scale;
        draw_rect = FloatRect::new(
            image_rect.x + image_rect.width / 2.0 - width / 2.0,
            image_rect.y + image_rect.height / 2.0 - height / 2.0,
            width,
            height,
        );
    }
    if draw_rect.is_empty() {
        return;
    }

    // https://svgwg.org/svg2-draft/embedded.html#ImageElement
    // Unless over-ridden by the author, images will therefore be clipped to the positioning
    // rectangle defined by the geometry properties.
    let (overflow_is_visible, image_rendering) =
        recorder
            .layout_arena
            .node_style_if_live(paintable)
            .map_or((false, 0), |style| {
                use crate::css::css_enums::overflow;
                (
                    style.box_values().overflow_x == overflow::VISIBLE
                        && style.box_values().overflow_y == overflow::VISIBLE,
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
