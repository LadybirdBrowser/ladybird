/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums;
use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId, OptionalAffineTransform};
use crate::painting::display_list::recorder::{DisplayListRecorder, FillPathParams, PaintStyle, PaintStyleOrColor};
use crate::painting::host::{FfiImagePaintFacts, FfiLayerImagePrepareFacts};
use crate::painting::node_painting;
use crate::painting::paintable_data::FfiPixelBox;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background_resolution::{
    BackgroundPaintInputs, ResolvedBackgroundLayer, resolve_background_for_paint, resolve_background_layers,
};
use crate::painting::record::paint::gradient_resolution::{gradient_paint_value, record_gradient_fill};
use crate::painting::visual_context::{FrameRole, PieceKey};
use libgfx_rust::{
    CompositingAndBlendingOperator, FloatRect, IntPoint, IntRect, IntSize, ScalingMode, ShouldAntiAlias, WindingRule,
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

fn fill_transparent_so_replay_pushes_the_layer(recorder: &mut PaintRecorder<'_>, rect: IntRect) {
    recorder.recorder.fill_rect_transparent(rect);
}

pub(crate) fn paint_background(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let Some(inputs) = resolve_background_for_paint(recorder, paintable) else {
        return;
    };
    paint_resolved_background(recorder, paintable, PieceKey::Box, &inputs);
}

pub(crate) fn paint_background_within(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    piece: PieceKey,
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
        crate::painting::host::FfiLayerImageList::Background,
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
    paint_resolved_background(recorder, paintable, piece, &inputs);
}

pub(crate) fn paint_resolved_background(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    piece: PieceKey,
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
    let layers_context = if backdrop.is_isolated_group() {
        Some(recorder.expected_local_context(paintable, FrameRole::BackgroundIsolation { piece }))
    } else if needs_text_clip {
        Some(recorder.expected_local_context(paintable, FrameRole::BackgroundTextContentLayer { piece }))
    } else {
        None
    };
    recorder.with_optional_context(layers_context, |recorder| {
        paint_background_layers(recorder, paintable, piece, inputs, backdrop);
    });

    if needs_text_clip {
        let mask_context = recorder.expected_local_context(paintable, FrameRole::BackgroundTextMask { piece });
        recorder.with_context(mask_context, |recorder| {
            fill_transparent_so_replay_pushes_the_layer(recorder, converter.rounded_device_rect(background_rect));
            append_text_clip_paths(recorder, paintable);
        });
    }
}

fn paint_background_layers(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    piece: PieceKey,
    inputs: &BackgroundPaintInputs<'_>,
    backdrop: LayerBackdrop,
) {
    let converter = recorder.converter;
    let resolved = &inputs.resolved;
    let is_root_element = inputs.is_root_element;
    let isolated = backdrop.is_isolated_group();
    let color = resolved.color;
    let background_rect = resolved.background_rect;
    let color_box = resolved.color_box;
    let layers = &resolved.layers;

    let border_box = BackgroundBox {
        rect: background_rect,
        radii: inputs.border_radii,
    };
    let padding = crate::painting::paintable_geometry::committed_padding(recorder.layout_arena, paintable);
    let border = crate::painting::paintable_geometry::committed_border(recorder.layout_arena, paintable);

    if is_root_element {
        recorder
            .recorder
            .fill_rect(converter.enclosing_device_rect(color_box.rect), color);
    } else {
        recorder.recorder.fill_rect_with_rounded_corners(
            converter.rounded_device_rect(color_box.rect),
            color,
            color_box.radii.as_corners(&converter),
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
        let mut clip_rect = converter.rounded_device_rect(css_clip_rect);
        let layer_context = if is_root_element {
            recorder.recorder.accumulated_visual_context()
        } else {
            if layer.clip == css_enums::background_box::BORDER_BOX {
                clip_rect = clip_rect.shrunken(clip_shrink.0, clip_shrink.1, clip_shrink.2, clip_shrink.3);
            }
            recorder.expected_local_context(
                paintable,
                FrameRole::BackgroundLayerClip {
                    piece,
                    layer: layer.computed_index as u16,
                    isolated,
                },
            )
        };

        let mut compositing_and_blending_operator = layer.compositing_and_blending_operator;
        // https://drafts.fxtf.org/css-masking-1/#the-mask-composite
        // If there is no further mask layer, the compositing operator must be ignored.
        if let Some(mask_composite) = layer.mask_composite {
            if painted_mask_layer {
                compositing_and_blending_operator = mask_composite;
            }
            painted_mask_layer = true;
        }

        recorder.with_context(layer_context, |recorder| {
            if layer.image.is_none() {
                if compositing_and_blending_operator != CompositingAndBlendingOperator::Normal {
                    let blend_context = recorder.expected_local_context(
                        paintable,
                        FrameRole::BackgroundLayerBlend {
                            piece,
                            layer: layer.computed_index as u16,
                            isolated,
                        },
                    );
                    recorder.with_context(blend_context, |recorder| {
                        fill_transparent_so_replay_pushes_the_layer(recorder, clip_rect);
                    });
                }
            } else {
                paint_image_layer(
                    recorder,
                    paintable,
                    piece,
                    layer,
                    inputs.image_rendering,
                    css_clip_rect,
                    clip_rect,
                    compositing_and_blending_operator,
                    backdrop,
                );
            }
        });
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

pub(crate) fn paint_image(
    recorder: &mut PaintRecorder<'_>,
    facts: &FfiImagePaintFacts,
    dest_rect: FloatRect,
    image_rendering: u8,
) {
    match facts.image_paint_kind {
        crate::painting::host::FfiImagePaintKind::DecodedFrame => {
            let target = (
                dest_rect.width.round_ties_even() as i32,
                dest_rect.height.round_ties_even() as i32,
            );
            let scaling_mode =
                to_gfx_scaling_mode(image_rendering, (facts.natural_width, facts.natural_height), target);
            recorder.recorder.draw_scaled_decoded_image_frame(
                dest_rect,
                None,
                ImageFrameResourceId(facts.frame_id),
                scaling_mode,
                CompositingAndBlendingOperator::Normal,
                None,
            );
        }
        crate::painting::host::FfiImagePaintKind::NestedDisplayList => {
            recorder.recorder.paint_nested_display_list(
                DisplayListResourceId(facts.nested_display_list_id),
                dest_rect,
                IntSize {
                    width: facts.list_width,
                    height: facts.list_height,
                },
            );
        }
        crate::painting::host::FfiImagePaintKind::None => {}
    }
}

#[allow(clippy::too_many_arguments)]
fn paint_image_layer(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    piece: PieceKey,
    layer: &ResolvedBackgroundLayer<'_>,
    image_rendering: u8,
    css_clip_rect: CssPixelRect,
    clip_rect: IntRect,
    compositing_and_blending_operator: CompositingAndBlendingOperator,
    backdrop: LayerBackdrop,
) {
    let converter = recorder.converter;
    let shell = recorder.layout_node_shell(paintable);
    let isolated = backdrop.is_isolated_group();
    let image = layer.image.expect("an imageless layer never reaches the image paint");
    let mut image_rect = layer.image_rect;
    let mut background_positioning_area = layer.background_positioning_area;

    match layer.attachment {
        css_enums::background_attachment::FIXED => {
            let data = recorder.data(paintable);
            if data.has_fixed_background_visual_context {
                let frame = recorder.recorder.accumulated_visual_context().frame;
                recorder.recorder.set_accumulated_visual_context(ContextRef {
                    spatial: data.fixed_background_visual_context.spatial,
                    frame,
                });
            }
        }
        css_enums::background_attachment::LOCAL
            if recorder.layout_arena.node_kind_if_live(paintable) != Some(NodeKind::Viewport) =>
        {
            let scroll_offset = CssPixelPoint::from(recorder.visual_context_host.scroll_offset(shell));
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

    // An SVG used as an image resolves `prefers-color-scheme` from the used `color-scheme` of
    // the element referencing it.
    let prepare = if resolved_gradient.is_some() {
        FfiLayerImagePrepareFacts::default()
    } else {
        recorder
            .paint_host
            .layer_image_prepare(shell, image.list, image.computed_index)
    };

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

    let enter_blend_layer = |recorder: &mut PaintRecorder<'_>| {
        if compositing_and_blending_operator == CompositingAndBlendingOperator::Normal {
            return;
        }
        let blend_context = recorder.expected_local_context(
            paintable,
            FrameRole::BackgroundLayerBlend {
                piece,
                layer: layer.computed_index as u16,
                isolated,
            },
        );
        recorder.recorder.set_accumulated_visual_context(blend_context);
    };

    if prepare.single_pixel_color.has_value {
        enter_blend_layer(recorder);
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
        recorder
            .recorder
            .fill_rect(fill_rect.unwrap_or_default(), prepare.single_pixel_color.value);
    } else if prepare.is_image_style_value
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
        let nested =
            recorder
                .paint_host
                .layer_image_nested_display_list(shell, image.list, image.computed_index, dest_rect);
        if nested.has_nested_display_list {
            enter_blend_layer(recorder);
            let scaling_mode = to_gfx_scaling_mode(
                image_rendering,
                (dest_rect.width, dest_rect.height),
                (dest_rect.width, dest_rect.height),
            );
            recorder.recorder.draw_repeated_display_list(
                dest_rect,
                clip_rect,
                DisplayListResourceId(nested.nested_display_list_id),
                scaling_mode,
                repeat_x,
                repeat_y,
            );
        } else {
            let frame =
                recorder
                    .paint_host
                    .layer_image_current_frame(shell, image.list, image.computed_index, dest_rect);
            if !frame.has_frame {
                return;
            }
            let tile_device_rect = dest_rect;
            let clip_device_rect = clip_rect;
            let visible_rect = tile_device_rect.intersected(clip_device_rect);
            if tile_count == 1.0 {
                let source_rect = source_rect_for_visible_image_part(
                    visible_rect,
                    tile_device_rect,
                    (frame.frame_width, frame.frame_height),
                );
                let scaling_mode = to_gfx_scaling_mode(
                    image_rendering,
                    (
                        source_rect.width.round_ties_even() as i32,
                        source_rect.height.round_ties_even() as i32,
                    ),
                    (visible_rect.width, visible_rect.height),
                );
                recorder.recorder.draw_scaled_decoded_image_frame(
                    visible_rect.to_float(),
                    Some(source_rect),
                    ImageFrameResourceId(frame.frame_id),
                    scaling_mode,
                    compositing_and_blending_operator,
                    backdrop.opaque_color_under_lone_layer(),
                );
            } else if tile_count > 1.0 {
                let scaling_mode = to_gfx_scaling_mode(
                    image_rendering,
                    (frame.frame_width, frame.frame_height),
                    (tile_device_rect.width, tile_device_rect.height),
                );
                recorder.recorder.draw_repeated_decoded_image_frame(
                    tile_device_rect,
                    clip_device_rect,
                    ImageFrameResourceId(frame.frame_id),
                    scaling_mode,
                    repeat_x,
                    repeat_y,
                    compositing_and_blending_operator,
                    backdrop.opaque_color_under_lone_layer(),
                );
            }
        }
    } else if (repeat_x || repeat_y)
        && !repeat_x_has_gap
        && !repeat_y_has_gap
        && tile_count > MAX_TILES_BEFORE_PATTERN_FALLBACK
    {
        enter_blend_layer(recorder);
        // A not-decoded-image repeating background otherwise records a separate painting command
        // for every tile — which for very-large tile counts can lead to enough commands that we
        // crash. So, instead record a single tile into a nested display list, and fill the area
        // with a repeating pattern. The painter does the tiling.
        let mut tile_device_rect = converter.rounded_device_rect(image_rect);
        // If the tile's dimensions were rounded to zero then they need to be restored to avoid a crash.
        if tile_device_rect.width == 0 {
            tile_device_rect.width = 1;
        }
        if tile_device_rect.height == 0 {
            tile_device_rect.height = 1;
        }

        let outer_recorder = std::mem::replace(&mut recorder.recorder, DisplayListRecorder::new());
        let tile_dest_rect = tile_device_rect.to_float();
        if let Some(gradient) = &resolved_gradient {
            record_gradient_fill(recorder, gradient, tile_dest_rect);
        } else {
            let paint = recorder.paint_host.layer_image_paint(
                shell,
                image.list,
                image.computed_index,
                tile_dest_rect,
                image_rect.size().into(),
                image_rendering,
                libgfx_rust::FloatSize {
                    width: 1.0,
                    height: 1.0,
                },
            );
            if paint.image_paint_kind != crate::painting::host::FfiImagePaintKind::None {
                paint_image(recorder, &paint, tile_dest_rect, image_rendering);
            }
        }
        let tile_recorder = std::mem::replace(&mut recorder.recorder, outer_recorder);
        let tile = tile_recorder.into_builder().finish();
        let tile_display_list_id = recorder.paint_host.nested_display_list_from_bytes(
            &tile,
            IntPoint {
                x: -tile_device_rect.x,
                y: -tile_device_rect.y,
            },
        );

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
        recorder.recorder.fill_path(FillPathParams {
            path: &path,
            opacity: 1.0,
            paint_style_or_color: PaintStyleOrColor::PaintStyle(PaintStyle::Pattern {
                tile_display_list_id,
                tile_rect: tile_dest_rect,
                content_scale: libgfx_rust::FloatSize {
                    width: 1.0,
                    height: 1.0,
                },
                pattern_transform: OptionalAffineTransform::default(),
            }),
            winding_rule: WindingRule::Nonzero,
            should_anti_alias: ShouldAntiAlias::Yes,
        });
    } else {
        enter_blend_layer(recorder);
        for image_device_rect in device_rects(image_rect) {
            let dest_rect = image_device_rect.to_float();
            if let Some(gradient) = &resolved_gradient {
                record_gradient_fill(recorder, gradient, dest_rect);
                continue;
            }
            let accumulated_scale =
                recorder.accumulated_2d_scale_at(recorder.recorder.accumulated_visual_context().spatial);
            let paint = recorder.paint_host.layer_image_paint(
                shell,
                image.list,
                image.computed_index,
                dest_rect,
                image_rect.size().into(),
                image_rendering,
                accumulated_scale,
            );
            if paint.image_paint_kind != crate::painting::host::FfiImagePaintKind::None {
                paint_image(recorder, &paint, dest_rect, image_rendering);
            }
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

fn append_text_clip_paths(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;

    let append_fragment = |recorder: &mut PaintRecorder<'_>, owner: NodeSlotId, fragment_index: usize| {
        let fragment = &recorder.layout_arena.paintable_side_data(owner).fragments[fragment_index];
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
        let font_id = recorder.register_font(run.font.as_raw());
        let emission =
            crate::painting::record::paint::text::glyph_run_emission(fragment, run, fragment_absolute_rect, scale);
        recorder.recorder.draw_glyph_run(
            emission.baseline_start,
            crate::painting::display_list::recorder::GlyphRunForRecording {
                font_id: crate::painting::display_list::commands::FontResourceId(font_id),
                glyphs: &emission.glyphs,
            },
            libgfx_rust::Color::from_rgb(0, 0, 0),
            fragment_absolute_device_rect,
            scale,
            emission.orientation,
            emission.glyph_bounding_rect,
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
                let piece = &layout_arena.paintable_side_data(root).inline_box_pieces[*piece_index as usize];
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
            let count = recorder.layout_arena.paintable_side_data(current).fragments.len();
            for fragment_index in 0..count {
                append_fragment(recorder, current, fragment_index);
            }
        }
    }
}
