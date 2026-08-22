/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::paint_order;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::{DisplayListGradientSpreadMethod, OptionalAffineTransform};
use crate::painting::display_list::recorder::{
    ColorStops, FillPathParams, PaintStyle, PaintStyleOrColor, StrokePathParams,
};
use crate::painting::host::{FfiSvgGradientSpreadMethod, FfiSvgPaintStyle, FfiSvgPaintStyleKind};
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::record::paint::background::paint_image;
use crate::painting::record::{PaintPhase, PaintRecorder};
use libgfx_rust::{
    AffineTransform, CapStyle, Color, FloatRect, FloatSize, InterpolationColorSpace, JoinStyle, ShouldAntiAlias,
    WindingRule,
};

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
    has_decoded_image_data: bool,
    has_natural_size: bool,
    natural_width: f32,
    natural_height: f32,
    overflow_is_visible: bool,
    image_rendering: u8,
}

fn svg_paint_color(paint: &crate::css::computed_value_types::ComputedSvgPaint) -> Option<u32> {
    match paint.kind {
        0 => None,
        1 => Some(paint.color),
        _ => paint.has_color.then_some(paint.color),
    }
}

fn svg_paint_facts(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) -> (SvgPaintFacts, Vec<f32>) {
    use crate::css::css_enums::{fill_rule, overflow, stroke_linecap, stroke_linejoin, vector_effect};
    let host = recorder
        .paint_host
        .svg_host_facts(recorder.layout_node_shell(paintable));
    let mut facts = SvgPaintFacts {
        has_viewport: host.has_viewport,
        viewport: host.viewport,
        has_decoded_image_data: host.has_decoded_image_data,
        has_natural_size: host.has_natural_size,
        natural_width: host.natural_width,
        natural_height: host.natural_height,
        ..SvgPaintFacts::default()
    };
    let layout_arena = recorder.layout_arena;
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
    facts.overflow_is_visible =
        style.box_values().overflow_x == overflow::VISIBLE && style.box_values().overflow_y == overflow::VISIBLE;
    facts.image_rendering = style.image_rendering();

    let basis = crate::css::css_pixels::CssPixels::from_raw(host.percentage_basis);
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

fn paint_style_from_ffi(
    recorder: &mut PaintRecorder<'_>,
    style: &FfiSvgPaintStyle,
    stops: &crate::painting::host::ColorStopSink,
) -> Option<PaintStyle> {
    let color_stops = ColorStops {
        colors: stops.colors.iter().map(|color| Color(*color)).collect(),
        positions: stops.positions.clone(),
        repeating: false,
    };
    let gradient_transform = if style.has_gradient_transform {
        OptionalAffineTransform::from(Some(affine(style.gradient_transform)))
    } else {
        OptionalAffineTransform::none()
    };
    let spread_method = match style.spread_method {
        FfiSvgGradientSpreadMethod::Pad => DisplayListGradientSpreadMethod::Pad,
        FfiSvgGradientSpreadMethod::Repeat => DisplayListGradientSpreadMethod::Repeat,
        FfiSvgGradientSpreadMethod::Reflect => DisplayListGradientSpreadMethod::Reflect,
    };
    let color_space = if style.color_space == 0 {
        InterpolationColorSpace::LinearRGB
    } else {
        InterpolationColorSpace::SRGB
    };
    match style.kind {
        FfiSvgPaintStyleKind::LinearGradient => Some(PaintStyle::LinearGradient {
            gradient_transform,
            spread_method,
            color_space,
            color_stops,
            start_point: libgfx_rust::FloatPoint {
                x: style.start[0],
                y: style.start[1],
            },
            end_point: libgfx_rust::FloatPoint {
                x: style.end[0],
                y: style.end[1],
            },
        }),
        FfiSvgPaintStyleKind::RadialGradient => Some(PaintStyle::RadialGradient {
            gradient_transform,
            spread_method,
            color_space,
            color_stops,
            start_center: libgfx_rust::FloatPoint {
                x: style.start[0],
                y: style.start[1],
            },
            start_radius: style.start_radius,
            end_center: libgfx_rust::FloatPoint {
                x: style.end[0],
                y: style.end[1],
            },
            end_radius: style.end_radius,
        }),
        FfiSvgPaintStyleKind::Pattern => Some(
            recorder
                .prerecorded
                .pattern_paint_styles
                .get(&style.pattern_paintable.index)
                .expect("a resolved pattern without a prerecorded paint style")
                .clone(),
        ),
        FfiSvgPaintStyleKind::None => None,
    }
}

pub(crate) fn record_pattern_paint_styles(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    if recorder.data(paintable).kind != crate::painting::paintable_data::PaintableKind::SVGPathPaintable {
        return;
    }
    if recorder.draw_svg_geometry_for_clip_path {
        return;
    }
    let Some(computed_path) = crate::painting::paintable_geometry::committed_svg_path(recorder.layout_arena, paintable)
    else {
        return;
    };
    if !recorder.is_visible(paintable) {
        return;
    }
    let (facts, _dash_array) = svg_paint_facts(recorder, paintable);
    let device_scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    let paint_transform = [device_scale, 0.0, 0.0, device_scale, 0.0, 0.0];
    let content_scale = recorder.own_accumulated_2d_scale(paintable);
    let paint_context = crate::painting::host::FfiSvgPaintContext {
        viewport: if facts.has_viewport { facts.viewport } else { [0.0; 4] },
        path_bounding_box: computed_path.bounding_box(),
        paint_transform,
        content_scale: [content_scale.width, content_scale.height],
    };
    for is_stroke in [false, true] {
        let (style, _stops) =
            recorder
                .paint_host
                .svg_paint_style(recorder.layout_node_shell(paintable), is_stroke, &paint_context);
        if style.kind != FfiSvgPaintStyleKind::Pattern {
            continue;
        }
        if recorder
            .prerecorded
            .pattern_paint_styles
            .contains_key(&style.pattern_paintable.index)
        {
            continue;
        }
        let paint_style = record_pattern_paint_style(recorder, &style);
        recorder
            .prerecorded
            .pattern_paint_styles
            .insert(style.pattern_paintable.index, paint_style);
    }
}

fn record_pattern_paint_style(recorder: &mut PaintRecorder<'_>, style: &FfiSvgPaintStyle) -> PaintStyle {
    let mut elements = [[0.0f32; 4]; 4];
    for (index, value) in style.tile_content_transform.into_iter().enumerate() {
        elements[index / 4][index % 4] = value;
    }
    let root_transform = crate::painting::visual_context::TransformData {
        matrix: libgfx_rust::FloatMatrix4x4 { elements },
        origin: libgfx_rust::FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: crate::painting::visual_context::TransformDataRole::CssTransform,
        synthetic_plane: false,
    };
    // Pattern tiles exclude the root's own transform: patternTransform reaches the replay-side tile
    // shader instead, so the tiling grid repeats under it rather than the content scaling twice.
    let tile_display_list_id =
        recorder.record_nested_svg_display_list(style.pattern_paintable, root_transform, false, false);
    PaintStyle::Pattern {
        tile_display_list_id,
        tile_rect: FloatRect::from_array(style.tile_rect),
        content_scale: FloatSize {
            width: style.content_scale[0],
            height: style.content_scale[1],
        },
        pattern_transform: if style.has_pattern_transform {
            OptionalAffineTransform::from(Some(affine(style.pattern_transform)))
        } else {
            OptionalAffineTransform::none()
        },
    }
}

pub(crate) fn paint_path(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, phase: PaintPhase) {
    let Some(computed_path) = crate::painting::paintable_geometry::committed_svg_path(recorder.layout_arena, paintable)
    else {
        return;
    };
    let (facts, dash_array) = svg_paint_facts(recorder, paintable);
    if recorder.draw_svg_geometry_for_clip_path {
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

    if recorder.draw_svg_geometry_for_clip_path {
        // https://drafts.fxtf.org/css-masking/#ClipPathElement: the raw geometry of each child
        // element, exclusive of rendering properties, defines a 1-bit mask.
        recorder.recorder.fill_path(FillPathParams {
            path: &path,
            opacity: 1.0,
            paint_style_or_color: PaintStyleOrColor::Color(Color::from_rgb(0, 0, 0)),
            winding_rule: WindingRule::from_raw(facts.clip_rule_winding),
            should_anti_alias: anti_alias,
        });
        return;
    }

    let paint_context = crate::painting::host::FfiSvgPaintContext {
        viewport: if facts.has_viewport { facts.viewport } else { [0.0; 4] },
        path_bounding_box: computed_path.bounding_box(),
        paint_transform,
        content_scale: {
            let scale = recorder.own_accumulated_2d_scale(paintable);
            [scale.width, scale.height]
        },
    };

    for index in 0..facts.paint_order_count as usize {
        match facts.paint_order[index] {
            paint_order::FILL => {
                let fill_opacity = facts.fill_opacity;
                let fill_winding = WindingRule::from_raw(facts.fill_winding);
                let (style, stops) =
                    recorder
                        .paint_host
                        .svg_paint_style(recorder.layout_node_shell(paintable), false, &paint_context);
                if let Some(paint_style) = paint_style_from_ffi(recorder, &style, &stops) {
                    recorder.recorder.fill_path(FillPathParams {
                        path: &path,
                        opacity: fill_opacity,
                        paint_style_or_color: PaintStyleOrColor::PaintStyle(paint_style),
                        winding_rule: fill_winding,
                        should_anti_alias: anti_alias,
                    });
                } else if let Some(fill_color) = facts.fill_color {
                    recorder.recorder.fill_path(FillPathParams {
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

                let (style, stops) =
                    recorder
                        .paint_host
                        .svg_paint_style(recorder.layout_node_shell(paintable), true, &paint_context);
                if let Some(paint_style) = paint_style_from_ffi(recorder, &style, &stops) {
                    recorder.recorder.stroke_path(StrokePathParams {
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

pub(crate) fn paint_image_element(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, phase: PaintPhase) {
    // NB: An image has no geometry, so it contributes nothing to a clipping path.
    if recorder.draw_svg_geometry_for_clip_path {
        return;
    }
    if !recorder.is_visible(paintable) {
        return;
    }

    super::paint_base(recorder, paintable, phase);

    if phase != PaintPhase::Foreground {
        return;
    }

    let (facts, _) = svg_paint_facts(recorder, paintable);
    if !facts.has_decoded_image_data {
        return;
    }

    // The image rect is in the viewport's user units; a fraction of a unit can span many device
    // pixels, so the positioning math stays in float and only the overflow clip quantizes.
    let device_scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    let css_rect = absolute_rect(recorder.layout_arena, paintable);
    let image_rect = FloatRect::new(
        css_rect.x.to_float() * device_scale,
        css_rect.y.to_float() * device_scale,
        css_rect.width.to_float() * device_scale,
        css_rect.height.to_float() * device_scale,
    );
    let natural_size = if facts.has_natural_size {
        (facts.natural_width, facts.natural_height)
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
    let draw_rect_needs_clip = !facts.overflow_is_visible && !image_rect.contains_rect(draw_rect);
    if draw_rect_needs_clip {
        recorder.recorder.save();
        recorder.recorder.add_clip_rect(image_rect);
    }

    let accumulated_scale = recorder.accumulated_2d_scale_at(recorder.recorder.accumulated_visual_context().0);
    let paint = recorder.paint_host.replaced_image_paint(
        recorder.layout_node_shell(paintable),
        [draw_rect.x, draw_rect.y, draw_rect.width, draw_rect.height],
        accumulated_scale,
    );
    if paint.image_paint_kind != crate::painting::host::FfiImagePaintKind::None {
        paint_image(recorder, &paint, draw_rect, facts.image_rendering);
    }

    if draw_rect_needs_clip {
        recorder.recorder.restore();
    }
}
