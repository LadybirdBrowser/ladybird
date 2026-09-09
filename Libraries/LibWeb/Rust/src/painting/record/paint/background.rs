/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::{Observer, Operation};

use crate::css::css_enums;
use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::commands::{OptionalAffineTransform, Repeat};
use crate::painting::display_list::recorder::{FillPathParams, PaintStyle, PaintStyleOrColor};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::node_painting;
use crate::painting::paintable_data::FfiPixelBox;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background_resolution::{
    BackgroundPaintInputs, ResolvedBackgroundLayer, operator_erases_destination_outside_the_drawn_geometry,
    resolve_background_for_paint, resolve_background_layers,
};
use crate::painting::record::paint::gradient_resolution::{gradient_paint_value, record_gradient_fill};
use crate::painting::record::paint::table_backgrounds;
use libgfx_rust::{
    AffineTransform, CompositingAndBlendingOperator, FloatRect, IntRect, IntSize, MaskKind, ScalingMode,
    ShouldAntiAlias, WindingRule,
};

#[derive(Clone, Copy, Debug)]
pub(crate) struct BackgroundBox {
    pub rect: CssPixelRect,
    pub radii: BorderRadii,
}

impl BackgroundBox {
    fn shrink(&mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) {
        self.rect.shrink(top, right, bottom, left);
        self.radii.shrink(top, right, bottom, left);
    }
}

#[derive(Clone, Copy)]
enum LayerBackdrop {
    Canvas,
    OpaqueColorUnderLoneLayer(libgfx_rust::Color),
    IsolatedGroup,
}

impl LayerBackdrop {
    fn is_isolated_group(self) -> bool {
        matches!(self, Self::IsolatedGroup)
    }

    fn opaque_color_under_lone_layer(self) -> Option<libgfx_rust::Color> {
        match self {
            Self::OpaqueColorUnderLoneLayer(color) => Some(color),
            Self::Canvas | Self::IsolatedGroup => None,
        }
    }
}

pub(crate) fn paint_background<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    if table_backgrounds::paints_background_in_cells(recorder.display(paintable)) {
        table_backgrounds::paint_table_part_background(recorder, paintable);
        return;
    }
    let Some(inputs) = resolve_background_for_paint(recorder, paintable) else {
        return;
    };
    paint_resolved_background(recorder, paintable, &inputs);
}

pub(crate) fn paint_background_within<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    background_rect: CssPixelRect,
    border_radii: BorderRadii,
) {
    let layout_arena = recorder.layout_arena;
    let Some(style) = layout_arena.node_style_if_live(paintable) else {
        return;
    };
    let resolved = resolve_background_layers(
        recorder,
        paintable,
        style,
        paintable,
        libgfx_rust::Color(style.background().background_color),
        style.background().background_color_clip,
        background_rect,
        border_radii,
    );
    let inputs = BackgroundPaintInputs {
        resolved,
        border_radii,
        image_rendering: style.image_rendering(),
        is_root_element: false,
    };
    paint_resolved_background(recorder, paintable, &inputs);
}

pub(crate) fn paint_resolved_background<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    inputs: &BackgroundPaintInputs<'_>,
) {
    // https://www.w3.org/TR/css-backgrounds-3/#backgrounds
    let converter = recorder.converter;
    let resolved = &inputs.resolved;
    let color = resolved.color;
    let background_rect = resolved.background_rect;
    let color_box = resolved.color_box;
    let layers = &resolved.layers;
    // https://drafts.fxtf.org/compositing/#background-blend-mode
    // Background layers must not blend with the content that is behind the element, instead they
    // must act as if they are rendered into an isolated group.
    let some_layer_blends = layers
        .iter()
        .any(|layer| layer.compositing_and_blending_operator != CompositingAndBlendingOperator::Normal);
    let backdrop = if !some_layer_blends {
        LayerBackdrop::Canvas
    } else if resolved.paintable_layer_count == 1 && color.alpha() == 255 && color_box.rect == background_rect {
        LayerBackdrop::OpaqueColorUnderLoneLayer(color)
    } else {
        LayerBackdrop::IsolatedGroup
    };

    // https://drafts.csswg.org/css-backgrounds-4/#valdef-background-clip-text
    let needs_text_clip = resolved.needs_text_clip && !inputs.is_root_element;
    if !backdrop.is_isolated_group() && !needs_text_clip {
        paint_background_layers(recorder, paintable, inputs, backdrop);
        return;
    }

    // https://drafts.fxtf.org/compositing/#background-blend-mode
    // Blending layers render into an isolated group, and background-clip: text masks the whole
    // stack by its glyphs, so the layers are recorded as one group command that plays them inside
    // its own saveLayer, masked by the glyph runs recorded after them.
    let group_device_rect = if needs_text_clip {
        converter.rounded_device_rect(background_rect)
    } else {
        converter
            .rounded_device_rect(background_rect)
            .united(converter.enclosing_device_rect(color_box.rect))
    };
    let mut group = recorder.recorder.begin_isolated_group();
    recorder.trace_paint(Operation::Producer(Some(paintable), "background-group"), |recorder| {
        paint_background_layers(recorder, paintable, inputs, backdrop);
    });
    if needs_text_clip {
        recorder.recorder.begin_group_mask(&mut group);
        recorder.trace_paint(
            Operation::Producer(Some(paintable), "background-text-mask"),
            |recorder| {
                append_text_clip_paths(recorder, paintable);
            },
        );
    }
    recorder.recorder.finish_isolated_group(
        group,
        group_device_rect.to_float(),
        CompositingAndBlendingOperator::Normal,
        MaskKind::Alpha,
    );
}

fn paint_background_layers<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    inputs: &BackgroundPaintInputs<'_>,
    backdrop: LayerBackdrop,
) {
    let converter = recorder.converter;
    let resolved = &inputs.resolved;
    let is_root_element = inputs.is_root_element;
    let color = resolved.color;
    let background_rect = resolved.background_rect;
    let color_box = resolved.color_box;
    let layers = &resolved.layers;
    let background_color_animation_effect = recorder
        .layout_arena
        .node_has_compositor_animation_frame(
            paintable,
            crate::layout::node_data::CompositorAnimationFrameKind::BackgroundColor,
        )
        .then_some(recorder.recorder.accumulated_visual_context().effect)
        .filter(|frame| !frame.is_none());

    let border_box = BackgroundBox {
        rect: background_rect,
        radii: inputs.border_radii,
    };
    let padding = crate::painting::paintable_geometry::committed_padding(recorder.layout_arena, paintable);
    let border = crate::painting::paintable_geometry::committed_border_box_edges(recorder.layout_arena, paintable);

    if is_root_element {
        recorder.recorder.fill_animated_background_color(
            converter.enclosing_device_rect(color_box.rect),
            color,
            libgfx_rust::CornerRadii::default(),
            background_color_animation_effect,
            ForceDarkRole::Background,
        );
    } else {
        recorder.recorder.fill_animated_background_color(
            converter.rounded_device_rect(color_box.rect),
            color,
            color_box.radii.as_corners(&converter),
            background_color_animation_effect,
            ForceDarkRole::Background,
        );
    }

    // Shrink the effective clip rect to account for the bits the borders will definitely paint
    // over (if they all have alpha == 255).
    let (border_widths, borders_opaque) = {
        let style = recorder.layout_arena.node_style_if_live(paintable);
        match style {
            Some(style) => {
                let opaque = libgfx_rust::Color(style.border_top_color()).alpha() == 255
                    && libgfx_rust::Color(style.border_bottom_color()).alpha() == 255
                    && libgfx_rust::Color(style.border_left_color()).alpha() == 255
                    && libgfx_rust::Color(style.border_right_color()).alpha() == 255;
                (
                    (
                        style.border_top_width(),
                        style.border_right_width(),
                        style.border_bottom_width(),
                        style.border_left_width(),
                    ),
                    opaque,
                )
            }
            None => (
                (
                    CssPixels::from_raw(0),
                    CssPixels::from_raw(0),
                    CssPixels::from_raw(0),
                    CssPixels::from_raw(0),
                ),
                false,
            ),
        }
    };
    let clip_shrink = if borders_opaque {
        (
            converter.rounded_device_pixels(border_widths.0),
            converter.rounded_device_pixels(border_widths.1),
            converter.rounded_device_pixels(border_widths.2),
            converter.rounded_device_pixels(border_widths.3),
        )
    } else {
        (0, 0, 0, 0)
    };

    let mut painted_mask_layer = false;

    // Background layers are ordered front-to-back, so we paint them in reverse.
    for layer in layers.iter().rev() {
        let clip_box = background_box_for(layer.clip, border_box, padding, border);
        let css_clip_rect = clip_box.rect;
        let unshrunken_clip_rect = converter.rounded_device_rect(css_clip_rect);
        let mut clip_rect = unshrunken_clip_rect;
        if !is_root_element && layer.clip == css_enums::background_box::BORDER_BOX {
            clip_rect = clip_rect.shrunken(clip_shrink.0, clip_shrink.1, clip_shrink.2, clip_shrink.3);
        }

        let mut compositing_and_blending_operator = layer.compositing_and_blending_operator;
        // https://drafts.fxtf.org/css-masking-1/#the-mask-composite
        // If there is no further mask layer, the compositing operator must be ignored.
        if let Some(mask_composite) = layer.mask_composite {
            if painted_mask_layer {
                compositing_and_blending_operator = mask_composite;
            }
            painted_mask_layer = true;
        }

        let layer_erases_uncovered_destination =
            operator_erases_destination_outside_the_drawn_geometry(compositing_and_blending_operator);
        if layer_erases_uncovered_destination {
            // https://drafts.fxtf.org/css-masking-1/#the-mask-composite
            // The composite must erase the accumulated mask outside the drawn geometry, but only
            // within the layer's clip: the layer plays as an isolated group composited with the
            // operator, and the command's own clip bounds the erase.
            let group = recorder.recorder.begin_isolated_group();
            recorder.trace_paint(Operation::Producer(Some(paintable), "mask-layer"), |recorder| {
                if layer.image.is_some() {
                    paint_image_layer(
                        recorder,
                        paintable,
                        layer,
                        inputs.image_rendering,
                        css_clip_rect,
                        clip_rect,
                        CompositingAndBlendingOperator::Normal,
                        backdrop,
                    );
                }
            });
            recorder.recorder.finish_isolated_group(
                group,
                unshrunken_clip_rect.to_float(),
                compositing_and_blending_operator,
                MaskKind::Alpha,
            );
            continue;
        }

        let paint_layer = |recorder: &mut PaintRecorder<'_, O>| {
            if layer.image.is_some() {
                paint_image_layer(
                    recorder,
                    paintable,
                    layer,
                    inputs.image_rendering,
                    css_clip_rect,
                    clip_rect,
                    compositing_and_blending_operator,
                    backdrop,
                );
            }
        };

        if is_root_element {
            paint_layer(recorder);
        } else {
            let unshrunken_clip_float_rect = unshrunken_clip_rect.to_float();
            let corner_radii = clip_box.radii.corners_unconditionally(&converter);
            let mut layer_inline_clips = Vec::new();
            if corner_radii.has_any_radius() {
                layer_inline_clips.push(PendingInlineClip::intersecting_rounded_rect(
                    unshrunken_clip_float_rect,
                    corner_radii,
                ));
            }
            layer_inline_clips.push(PendingInlineClip::intersecting_float_rect(unshrunken_clip_float_rect));
            recorder.record_with_inline_clips(&layer_inline_clips, paint_layer);
        }
    }
}

pub(crate) fn background_box_for(
    box_clip: u8,
    border_box: BackgroundBox,
    padding: FfiPixelBox,
    border: FfiPixelBox,
) -> BackgroundBox {
    let mut background_box = border_box;
    if box_clip == css_enums::background_box::CONTENT_BOX {
        background_box.shrink(padding.top, padding.right, padding.bottom, padding.left);
    }
    if box_clip == css_enums::background_box::CONTENT_BOX || box_clip == css_enums::background_box::PADDING_BOX {
        background_box.shrink(border.top, border.right, border.bottom, border.left);
    }
    background_box
}

pub(crate) fn to_gfx_scaling_mode(image_rendering: u8, source: (i32, i32), target: (i32, i32)) -> ScalingMode {
    match image_rendering {
        css_enums::image_rendering::AUTO
        | css_enums::image_rendering::HIGH_QUALITY
        | css_enums::image_rendering::OPTIMIZEQUALITY
        | css_enums::image_rendering::SMOOTH => {
            if target.0 < source.0 && target.1 < source.1 {
                ScalingMode::BilinearMipmap
            } else {
                ScalingMode::Bilinear
            }
        }
        _ => ScalingMode::NearestNeighbor,
    }
}

pub(crate) fn paint_decoded_image_frame<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    frame: &libgfx_rust::image_frame::ImageFrameHandle,
    dest_rect: FloatRect,
    image_rendering: u8,
    compositing_and_blending_operator: CompositingAndBlendingOperator,
    force_dark_role: ForceDarkRole,
) {
    let frame_id = recorder.register_image_frame(frame);
    let frame_size = (frame.width(), frame.height());
    let target = (
        dest_rect.width.round_ties_even() as i32,
        dest_rect.height.round_ties_even() as i32,
    );
    let scaling_mode = to_gfx_scaling_mode(image_rendering, frame_size, target);
    let force_dark_role = crate::painting::force_dark::role_for_image(
        force_dark_role,
        dest_rect.width,
        dest_rect.height,
        recorder.inputs.device_pixels_per_css_pixel,
        frame_size,
    );
    recorder.recorder.draw_scaled_decoded_image_frame(
        dest_rect,
        None,
        frame_id,
        scaling_mode,
        compositing_and_blending_operator,
        None,
        force_dark_role,
    );
}

pub(crate) fn paint_image_content<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    source: crate::painting::record::vector_images::VectorImageSource,
    content: &crate::painting::image_content::ImageContent,
    dest_rect: FloatRect,
    image_rendering: u8,
    accumulated_scale: libgfx_rust::FloatSize,
    compositing_and_blending_operator: CompositingAndBlendingOperator,
) {
    use crate::painting::image_content::ImageContent;
    match content {
        ImageContent::Raster(Some(frame)) => paint_decoded_image_frame(
            recorder,
            frame,
            dest_rect,
            image_rendering,
            compositing_and_blending_operator,
            ForceDarkRole::Background,
        ),
        ImageContent::Vector {
            has_active_view_box, ..
        } => recorder.paint_vector_image(
            source,
            *has_active_view_box,
            dest_rect,
            accumulated_scale,
            compositing_and_blending_operator,
        ),
        ImageContent::None | ImageContent::Raster(None) => {}
    }
}

#[allow(clippy::too_many_arguments)]
fn paint_image_layer<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    layer: &ResolvedBackgroundLayer<'_>,
    image_rendering: u8,
    css_clip_rect: CssPixelRect,
    clip_rect: IntRect,
    compositing_and_blending_operator: CompositingAndBlendingOperator,
    backdrop: LayerBackdrop,
) {
    let converter = recorder.converter;
    let image = layer.image.expect("an imageless layer never reaches the image paint");
    let vector_image_source = crate::painting::record::vector_images::VectorImageSource::Layer {
        owner: image.facts_owner,
        list: image.list,
        computed_index: image.computed_index,
    };
    let facts =
        crate::painting::record::paint::background_resolution::committed_layer_image_paint_facts(recorder, &image);
    let mut image_rect = layer.image_rect;
    let mut background_positioning_area = layer.background_positioning_area;

    match layer.attachment {
        css_enums::background_attachment::FIXED => {
            let data = recorder.data(paintable);
            if data.has_fixed_background_visual_context && !recorder.recorder.is_recording_inside_group() {
                let context = recorder.recorder.accumulated_visual_context();
                recorder.recorder.set_accumulated_visual_context(ContextRef {
                    spatial: data.fixed_background_visual_context.spatial,
                    ..context
                });
            }
        }
        css_enums::background_attachment::LOCAL
            if recorder.layout_arena.node_kind_if_live(paintable) != Some(NodeKind::Viewport) =>
        {
            let scroll_offset = recorder.own_scroll_container_offset(paintable);
            background_positioning_area = background_positioning_area.translated(-scroll_offset.x, -scroll_offset.y);
        }
        _ => {}
    }

    image_rect.x = background_positioning_area.left() + layer.position_x;
    image_rect.y = background_positioning_area.top() + layer.position_y;

    // Repetition
    let repeat_x;
    let repeat_y;
    let mut repeat_x_has_gap = false;
    let mut repeat_y_has_gap = false;
    let mut x_step = CssPixels::from_raw(0);
    let mut y_step = CssPixels::from_raw(0);
    let zero = CssPixels::from_raw(0);

    match layer.repeat_x {
        css_enums::repetition::ROUND => {
            x_step = image_rect.width;
            repeat_x = true;
        }
        css_enums::repetition::SPACE => {
            let whole_images = fraction_to_int(background_positioning_area.width, image_rect.width);
            if whole_images <= 1 {
                x_step = image_rect.width;
                repeat_x = false;
            } else {
                let space = background_positioning_area.width.to_double() % image_rect.width.to_double();
                x_step = image_rect.width + CssPixels::nearest_value_for(space / (whole_images - 1) as f64);
                repeat_x = true;
                repeat_x_has_gap = true;
            }
        }
        css_enums::repetition::REPEAT => {
            x_step = image_rect.width;
            repeat_x = true;
        }
        _ => repeat_x = false,
    }
    // Move image_rect to the left-most tile position that is still visible
    if repeat_x && image_rect.x > css_clip_rect.x {
        let x_delta = floor_css(x_step * ceil_fraction(image_rect.x - css_clip_rect.x, x_step));
        image_rect.x -= x_delta;
    }

    match layer.repeat_y {
        css_enums::repetition::ROUND => {
            y_step = image_rect.height;
            repeat_y = true;
        }
        css_enums::repetition::SPACE => {
            let whole_images = fraction_to_int(background_positioning_area.height, image_rect.height);
            if whole_images <= 1 {
                y_step = image_rect.height;
                repeat_y = false;
            } else {
                let space = (background_positioning_area.height.to_float() % image_rect.height.to_float()) as f64;
                y_step = image_rect.height + CssPixels::nearest_value_for(space / (whole_images - 1) as f64);
                repeat_y = true;
                repeat_y_has_gap = true;
            }
        }
        css_enums::repetition::REPEAT => {
            y_step = image_rect.height;
            repeat_y = true;
        }
        _ => repeat_y = false,
    }
    // Move image_rect to the top-most tile position that is still visible
    if repeat_y && image_rect.y > css_clip_rect.y {
        let y_delta = floor_css(y_step * ceil_fraction(image_rect.y - css_clip_rect.y, y_step));
        image_rect.y -= y_delta;
    }

    let initial_image_x = image_rect.x;
    let image_y = image_rect.y;

    let resolved_gradient = gradient_paint_value(&image).map(|gradient_value| {
        let style = recorder
            .layout_arena
            .node_style_if_live(paintable)
            .expect("a painted layer's layout node is live");
        crate::painting::record::paint::gradient_resolution::resolve_gradient_paint(
            style,
            gradient_value,
            crate::css::css_pixels::CssPixelSize::new(image_rect.width, image_rect.height),
        )
    });

    let device_rects = |image_rect: CssPixelRect| -> Vec<IntRect> {
        let mut rects = Vec::new();
        let mut rect = image_rect;
        let mut image_y = image_y;
        while image_y < css_clip_rect.bottom() {
            rect.y = image_y;
            let mut image_x = initial_image_x;
            while image_x < css_clip_rect.right() {
                rect.x = image_x;
                let mut image_device_rect = converter.rounded_device_rect(rect);
                // If the image's dimensions were rounded to zero then they need to be restored to avoid a crash.
                if image_device_rect.width == 0 {
                    image_device_rect.width = 1;
                }
                if image_device_rect.height == 0 {
                    image_device_rect.height = 1;
                }
                rects.push(image_device_rect);
                if !repeat_x {
                    break;
                }
                image_x += x_step;
            }
            if !repeat_y {
                break;
            }
            image_y += y_step;
        }
        rects
    };

    // Past this (super-large) tile count, the non-image branch below covers the area with a single
    // repeating pattern whose command count is independent of the tile count. Otherwise, recording
    // one painting command per tile for a super-large tile count can produce enough commands that we
    // overflow the display list and crash.
    const MAX_TILES_BEFORE_PATTERN_FALLBACK: f64 = 1000.0;
    let tile_columns = if repeat_x && x_step > zero {
        ((css_clip_rect.right() - initial_image_x).to_double() / x_step.to_double()).ceil()
    } else {
        1.0
    };
    let tile_rows = if repeat_y && y_step > zero {
        ((css_clip_rect.bottom() - image_y).to_double() / y_step.to_double()).ceil()
    } else {
        1.0
    };
    let tile_count = tile_columns * tile_rows;

    let inline_operator = compositing_and_blending_operator;

    if let (None, Some(single_pixel_color)) = (&resolved_gradient, facts.single_pixel_color) {
        // OPTIMIZATION: If the image is a single pixel, we can just fill the whole area with it.
        //               However, we must first figure out the real coverage area, taking repeat etc into account.

        // FIXME: This could be written in a far more efficient way.
        let mut fill_rect: Option<IntRect> = None;
        for image_device_rect in device_rects(image_rect) {
            fill_rect = Some(match fill_rect {
                None => image_device_rect,
                Some(current) => current.united(image_device_rect),
            });
        }
        if inline_operator == CompositingAndBlendingOperator::Normal {
            recorder.recorder.fill_rect(
                fill_rect.unwrap_or_default(),
                single_pixel_color,
                ForceDarkRole::Background,
            );
        } else {
            recorder.recorder.fill_rect_with_compositing_and_blending_operator(
                fill_rect.unwrap_or_default(),
                single_pixel_color,
                inline_operator,
                ForceDarkRole::Background,
            );
        }
    } else if facts.content != crate::painting::image_content::ImageContent::None
        && ((repeat_x || repeat_y) || compositing_and_blending_operator != CompositingAndBlendingOperator::Normal)
        && !repeat_x_has_gap
        && !repeat_y_has_gap
    {
        // Use a dedicated painting command for repeated images instead of recording a separate command for each
        // instance of a repeated background, so the painter has the opportunity to optimize the painting of
        // repeated images.
        let mut dest_rect = converter.rounded_device_rect(image_rect);
        // If the image's dimensions were rounded to zero then they need to be restored to avoid a crash.
        if dest_rect.width == 0 {
            dest_rect.width = 1;
        }
        if dest_rect.height == 0 {
            dest_rect.height = 1;
        }
        if let crate::painting::image_content::ImageContent::Vector { .. } = &facts.content {
            if clip_rect.is_empty() {
                return;
            }
            let scaling_mode = to_gfx_scaling_mode(
                image_rendering,
                (dest_rect.width, dest_rect.height),
                (dest_rect.width, dest_rect.height),
            );
            let display_list_id = recorder.resources.vector_image_placeholder(
                crate::painting::record::vector_images::VectorImageRenderRequest::new(
                    vector_image_source,
                    CssPixels::from_integer(i64::from(dest_rect.width)),
                    CssPixels::from_integer(i64::from(dest_rect.height)),
                    1.0,
                ),
            );
            let group = recorder.recorder.begin_repeated_tile();
            recorder.recorder.paint_nested_display_list(
                display_list_id,
                dest_rect.to_float(),
                IntSize {
                    width: dest_rect.width,
                    height: dest_rect.height,
                },
            );
            recorder.recorder.finish_repeated_tile(
                group,
                dest_rect,
                clip_rect,
                scaling_mode,
                inline_operator,
                Repeat {
                    x: repeat_x,
                    y: repeat_y,
                },
            );
        } else {
            let crate::painting::image_content::ImageContent::Raster(Some(frame)) = &facts.content else {
                return;
            };
            let frame_id = recorder.register_image_frame(frame);
            let frame_size = (frame.width(), frame.height());
            let tile_device_rect = dest_rect;
            let clip_device_rect = clip_rect;
            let visible_rect = tile_device_rect.intersected(clip_device_rect);
            if tile_count == 1.0 {
                let source_rect = source_rect_for_visible_image_part(visible_rect, tile_device_rect, frame_size);
                let scaling_mode = to_gfx_scaling_mode(
                    image_rendering,
                    (
                        source_rect.width.round_ties_even() as i32,
                        source_rect.height.round_ties_even() as i32,
                    ),
                    (visible_rect.width, visible_rect.height),
                );
                // Judge the tile's layout size and the full frame, like the multi-tile branch: the visible clip and
                // its source crop shrink at an edge, which would flip a photo-sized image into an invertible icon.
                let force_dark_role = crate::painting::force_dark::role_for_image(
                    ForceDarkRole::Background,
                    tile_device_rect.width as f32,
                    tile_device_rect.height as f32,
                    recorder.inputs.device_pixels_per_css_pixel,
                    frame_size,
                );
                recorder.recorder.draw_scaled_decoded_image_frame(
                    visible_rect.to_float(),
                    Some(source_rect),
                    frame_id,
                    scaling_mode,
                    compositing_and_blending_operator,
                    backdrop.opaque_color_under_lone_layer(),
                    force_dark_role,
                );
            } else if tile_count > 1.0 {
                let scaling_mode = to_gfx_scaling_mode(
                    image_rendering,
                    frame_size,
                    (tile_device_rect.width, tile_device_rect.height),
                );
                let force_dark_role = crate::painting::force_dark::role_for_image(
                    ForceDarkRole::Background,
                    tile_device_rect.width as f32,
                    tile_device_rect.height as f32,
                    recorder.inputs.device_pixels_per_css_pixel,
                    frame_size,
                );
                recorder.recorder.draw_repeated_decoded_image_frame(
                    tile_device_rect,
                    clip_device_rect,
                    frame_id,
                    scaling_mode,
                    repeat_x,
                    repeat_y,
                    compositing_and_blending_operator,
                    backdrop.opaque_color_under_lone_layer(),
                    force_dark_role,
                );
            }
        }
    } else if (repeat_x || repeat_y)
        && !repeat_x_has_gap
        && !repeat_y_has_gap
        && tile_count > MAX_TILES_BEFORE_PATTERN_FALLBACK
    {
        // A not-decoded-image repeating background otherwise records a separate painting command
        // for every tile — which for very-large tile counts can lead to enough commands that we
        // crash. So, instead record a single tile's records into the fill command, and fill the
        // area with a repeating pattern. The painter does the tiling.
        let mut tile_device_rect = converter.rounded_device_rect(image_rect);
        // If the tile's dimensions were rounded to zero then they need to be restored to avoid a crash.
        if tile_device_rect.width == 0 {
            tile_device_rect.width = 1;
        }
        if tile_device_rect.height == 0 {
            tile_device_rect.height = 1;
        }

        let tile_dest_rect = tile_device_rect.to_float();
        let detached = recorder.recorder.begin_detached_records();
        recorder
            .recorder
            .set_ambient_inline_transform(Some(AffineTransform::new(
                1.0,
                0.0,
                0.0,
                1.0,
                -tile_dest_rect.x,
                -tile_dest_rect.y,
            )));
        recorder.trace_paint(Operation::Producer(Some(paintable), "background-tile"), |recorder| {
            if let Some(gradient) = &resolved_gradient {
                record_gradient_fill(
                    recorder,
                    gradient,
                    tile_dest_rect,
                    CompositingAndBlendingOperator::Normal,
                );
            }
        });
        let tile_records = std::rc::Rc::new(recorder.recorder.finish_detached_records(detached));

        // A pattern repeats along both axes. On any non-repeating axis, constrain the coverage to a single tile.
        let mut coverage = clip_rect;
        if !repeat_x {
            coverage.x = tile_device_rect.x;
            coverage.width = tile_device_rect.width;
        }
        if !repeat_y {
            coverage.y = tile_device_rect.y;
            coverage.height = tile_device_rect.height;
        }

        let coverage_float = coverage.to_float();
        let mut path = libgfx_rust::path::PathBuilder::new();
        path.move_to(coverage_float.x, coverage_float.y);
        path.line_to(coverage_float.x + coverage_float.width, coverage_float.y);
        path.line_to(
            coverage_float.x + coverage_float.width,
            coverage_float.y + coverage_float.height,
        );
        path.line_to(coverage_float.x, coverage_float.y + coverage_float.height);
        path.close();
        let path = path.build();
        recorder.recorder.fill_path_with_compositing_and_blending_operator(
            FillPathParams {
                force_dark_role: ForceDarkRole::Background,
                path: &path,
                opacity: 1.0,
                paint_style_or_color: PaintStyleOrColor::PaintStyle(PaintStyle::Pattern {
                    tile_records,
                    tile_rect: tile_dest_rect,
                    content_scale: libgfx_rust::FloatSize {
                        width: 1.0,
                        height: 1.0,
                    },
                    pattern_transform: OptionalAffineTransform::default(),
                }),
                winding_rule: WindingRule::Nonzero,
                should_anti_alias: ShouldAntiAlias::Yes,
            },
            inline_operator,
        );
    } else {
        for image_device_rect in device_rects(image_rect) {
            let dest_rect = image_device_rect.to_float();
            if let Some(gradient) = &resolved_gradient {
                record_gradient_fill(recorder, gradient, dest_rect, inline_operator);
                continue;
            }
            let accumulated_scale =
                recorder.accumulated_2d_scale_at(recorder.recorder.accumulated_visual_context().spatial);
            paint_image_content(
                recorder,
                vector_image_source,
                &facts.content,
                dest_rect,
                image_rendering,
                accumulated_scale,
                inline_operator,
            );
        }
    }
}

fn fraction_to_int(numerator: CssPixels, denominator: CssPixels) -> i32 {
    if denominator.raw_value() == 0 {
        return 0;
    }
    numerator.div_as_fraction(denominator).to_int()
}

fn ceil_fraction(numerator: CssPixels, denominator: CssPixels) -> CssPixels {
    if denominator.raw_value() == 0 {
        return CssPixels::from_raw(0);
    }
    numerator.div_as_fraction(denominator).ceil()
}

fn floor_css(value: CssPixels) -> CssPixels {
    value.floor()
}

fn source_rect_for_visible_image_part(
    visible_rect: IntRect,
    image_rect: IntRect,
    source_size: (i32, i32),
) -> FloatRect {
    let scale_x = source_size.0 as f32 / image_rect.width as f32;
    let scale_y = source_size.1 as f32 / image_rect.height as f32;
    FloatRect::new(
        (visible_rect.x - image_rect.x) as f32 * scale_x,
        (visible_rect.y - image_rect.y) as f32 * scale_y,
        visible_rect.width as f32 * scale_x,
        visible_rect.height as f32 * scale_y,
    )
}

fn append_text_clip_paths<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;

    let append_fragment = |recorder: &mut PaintRecorder<'_, O>, owner: NodeSlotId, fragment_index: usize| {
        let side = recorder.layout_arena.paintable_side_data(owner);
        let fragment = &side.fragments()[fragment_index];
        let is_text = recorder
            .layout_arena
            .node_kind_if_live(fragment.layout_node)
            .is_some_and(node_facts::kind_is_text);
        if !is_text {
            return;
        }
        let Some(run) = &fragment.glyph_run else {
            return;
        };
        if run.glyphs.is_empty() {
            return;
        }
        let fragment_absolute_rect = crate::painting::text_fragment::absolute_rect(recorder.layout_arena, fragment);
        let fragment_absolute_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
        let font_id = recorder.register_font(&run.font);
        let emission = crate::painting::record::paint::text::glyph_run_emission(
            fragment,
            run,
            fragment_absolute_rect,
            fragment_absolute_device_rect,
            scale,
        );
        recorder.recorder.draw_glyph_run(
            emission.baseline_start,
            crate::painting::display_list::recorder::GlyphRunForRecording {
                font_smoothing: recorder
                    .layout_arena
                    .node_style_if_live(fragment.style_source)
                    .unwrap()
                    .inherited_text()
                    .font_smoothing,
                font_id: crate::painting::display_list::commands::FontResourceId(font_id),
                glyphs: &emission.glyphs,
            },
            libgfx_rust::Color::from_rgb(0, 0, 0),
            fragment_absolute_device_rect,
            scale,
            emission.orientation,
            emission.glyph_bounding_rect,
            ForceDarkRole::None,
        );
    };

    let data = recorder.data(paintable);
    if node_painting::is_inline(recorder.layout_arena, paintable) {
        let root = data.containing_block;
        if !root.is_invalid()
            && recorder.layout_arena.paintable_row_is_populated(root)
            && node_painting::has_lines(recorder.layout_arena, root)
        {
            let layout_arena = recorder.layout_arena;
            for piece_index in &layout_arena.paintable_side_data(paintable).piece_indices {
                let side = layout_arena.paintable_side_data(root);
                let piece = &side.inline_box_pieces()[*piece_index as usize];
                for fragment_index in piece.first_fragment_index..piece.first_fragment_index + piece.fragment_count {
                    append_fragment(recorder, root, fragment_index as usize);
                }
            }
        }
    }

    let mut stack = vec![paintable];
    while let Some(current) = stack.pop() {
        if current != paintable {
            let out_of_flow_not_floating =
                matches!(
                    crate::painting::style_queries::position(recorder.layout_arena, current),
                    crate::css::css_enums::positioning::ABSOLUTE | crate::css::css_enums::positioning::FIXED
                ) && !crate::painting::style_queries::is_floating(recorder.layout_arena, current);
            if let Some(next) = crate::painting::paint_order::next_paint_sibling(recorder.layout_arena, current) {
                stack.push(next);
            }
            if out_of_flow_not_floating {
                continue;
            }
        }
        if let Some(first_child) = crate::painting::paint_order::first_paint_child(recorder.layout_arena, current) {
            stack.push(first_child);
        }
        if node_painting::has_lines(recorder.layout_arena, current) {
            let count = recorder.layout_arena.paintable_side_data(current).fragments().len();
            for fragment_index in 0..count {
                append_fragment(recorder, current, fragment_index);
            }
        }
    }
}
