/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::PaintRead;
use crate::painting::record::trace::Observer;

use crate::css::css_enums::{image_rendering, object_fit};
use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelRect, CssPixelSize};
use crate::layout::node_data::NodeSlotId;
use crate::layout::replaced_sizing::{Fraction, run_default_sizing_algorithm};
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::display_list::commands::{CanvasId, CompositorContextId, CompositorNestedContextWheelRegion};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background::{paint_image_content, to_gfx_scaling_mode};
use crate::painting::replaced_paint_facts::VideoPaintFacts;
use crate::painting::visual_context::node_values::padding_edge_border_radii;
use libgfx_rust::{Color, CornerRadii, FloatRect, IntRect, ScalingMode};

fn replaced_content_clip_geometry<O: Observer>(
    recorder: &PaintRecorder<'_, O>,
    paintable: NodeSlotId,
) -> (FloatRect, Option<CornerRadii>) {
    let content_rect = recorder
        .converter
        .rounded_device_rect(absolute_rect(recorder.source, paintable))
        .to_float();
    let corner_radii = recorder
        .source
        .node_style_if_live(paintable)
        .map(|style| {
            padding_edge_border_radii(style, recorder.source, paintable).corners_unconditionally(&recorder.converter)
        })
        .filter(|corner_radii| corner_radii.has_any_radius());
    (content_rect, corner_radii)
}

pub(crate) fn get_replaced_box_painting_area<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    mut object_fit: u8,
    content_size: CssPixelSize,
) -> IntRect {
    if content_size.is_empty() {
        return IntRect::default();
    }
    let paintable_rect = absolute_rect(recorder.source, paintable);
    if paintable_rect.is_empty() {
        return IntRect::default();
    }
    let converter = recorder.converter;

    let bitmap_aspect_ratio = Fraction::of(content_size.height, content_size.width);
    let image_aspect_ratio = Fraction::of(paintable_rect.height, paintable_rect.width);

    let mut scale_x = Fraction::one();
    let mut scale_y = Fraction::one();

    if object_fit == object_fit::SCALE_DOWN {
        if content_size.width > paintable_rect.width || content_size.height > paintable_rect.height {
            object_fit = object_fit::CONTAIN;
        } else {
            object_fit = object_fit::NONE;
        }
    }

    match object_fit {
        object_fit::FILL => {
            scale_x = Fraction::of(paintable_rect.width, content_size.width);
            scale_y = Fraction::of(paintable_rect.height, content_size.height);
        }
        object_fit::CONTAIN => {
            if bitmap_aspect_ratio.ge(image_aspect_ratio) {
                scale_x = Fraction::of(paintable_rect.height, content_size.height);
            } else {
                scale_x = Fraction::of(paintable_rect.width, content_size.width);
            }
            scale_y = scale_x;
        }
        object_fit::COVER => {
            if bitmap_aspect_ratio.ge(image_aspect_ratio) {
                scale_x = Fraction::of(paintable_rect.width, content_size.width);
            } else {
                scale_x = Fraction::of(paintable_rect.height, content_size.height);
            }
            scale_y = scale_x;
        }
        _ => {}
    }

    let scaled_bitmap_width = scale_x.multiply(content_size.width);
    let scaled_bitmap_height = scale_y.multiply(content_size.height);

    let residual_horizontal = paintable_rect.width - scaled_bitmap_width;
    let residual_vertical = paintable_rect.height - scaled_bitmap_height;

    // https://drafts.csswg.org/css-images/#the-object-position
    // The computed object-position stores offsets normalized to the left/top edges.
    let (offset_x, offset_y) = {
        let style = recorder.source.node_style_if_live(paintable);
        let zero = crate::css::css_pixels::CssPixels::from_raw(0);
        match style {
            Some(style) => {
                let misc = style.misc_layout();
                (
                    misc.object_position_x
                        .length_percentage()
                        .map_or(zero, |offset| offset.to_px(residual_horizontal)),
                    misc.object_position_y
                        .length_percentage()
                        .map_or(zero, |offset| offset.to_px(residual_vertical)),
                )
            }
            None => (zero, zero),
        }
    };

    converter.rounded_device_rect(CssPixelRect::new(
        paintable_rect.x + offset_x,
        paintable_rect.y + offset_y,
        scaled_bitmap_width,
        scaled_bitmap_height,
    ))
}

pub(crate) fn paint_replaced_image_content<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    content: &crate::painting::image_content::ImageContent,
    dest_rect: FloatRect,
    image_rendering: u8,
) {
    let accumulated_scale = recorder.accumulated_2d_scale_at(recorder.recorder.accumulated_visual_context().spatial);
    paint_image_content(
        recorder,
        crate::painting::record::vector_images::VectorImageSource::ReplacedContent { owner: paintable },
        content,
        dest_rect,
        image_rendering,
        accumulated_scale,
        libgfx_rust::CompositingAndBlendingOperator::Normal,
    );
}

fn replaced_style<O: Observer>(recorder: &PaintRecorder<'_, O>, paintable: NodeSlotId) -> (u8, u8) {
    recorder
        .source
        .node_style_if_live(paintable)
        .map_or((object_fit::FILL, image_rendering::AUTO), |style| {
            (style.misc_layout().object_fit, style.image_rendering())
        })
}

pub(crate) fn paint_image_foreground<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    let facts = recorder
        .source
        .replaced_paint_facts(paintable)
        .and_then(|facts| facts.image())
        .unwrap_or_default();
    let (object_fit, image_rendering) = replaced_style(recorder, paintable);
    let image_rect = absolute_rect(recorder.source, paintable);
    let image_rect_device_pixels = recorder.converter.rounded_device_rect(image_rect);
    if facts.content != crate::painting::image_content::ImageContent::None {
        // https://drafts.csswg.org/css-images/#the-object-fit
        let concrete_object_size = run_default_sizing_algorithm(None, None, &facts.natural, image_rect.size());

        let draw_rect = get_replaced_box_painting_area(recorder, paintable, object_fit, concrete_object_size);
        if !draw_rect.is_empty() {
            let (content_rect, corner_radii) = replaced_content_clip_geometry(recorder, paintable);
            let mut inline_clips = Vec::new();
            if let Some(corner_radii) = corner_radii {
                inline_clips.push(PendingInlineClip::intersecting_rounded_rect(content_rect, corner_radii));
            }
            if !image_rect_device_pixels.contains_rect(draw_rect) {
                inline_clips.push(PendingInlineClip::intersecting_float_rect(content_rect));
            }
            let dest_rect = draw_rect.to_float();
            recorder.record_with_inline_clips(&inline_clips, |recorder| {
                paint_replaced_image_content(recorder, paintable, &facts.content, dest_rect, image_rendering);
            });
        }
    }

    paint_custom_highlight_washes(recorder, paintable, image_rect_device_pixels);

    if recorder.data(paintable).selection_state != 0 {
        let selection_wash_color = recorder.element_selection_style(paintable).facts.wash_color;
        if selection_wash_color.alpha() > 0 {
            let backdrop = recorder
                .source
                .node_style_if_live(paintable)
                .map(|style| libgfx_rust::Color(style.background_color()));
            let previous = recorder.recorder.set_contrast_backdrop(backdrop);
            recorder
                .recorder
                .fill_rect(image_rect_device_pixels, selection_wash_color, ForceDarkRole::Selection);
            recorder.recorder.set_contrast_backdrop(previous);
        }
    }
}

// https://drafts.csswg.org/css-pseudo-4/#highlight-replaced
// [...] for replaced content, the UA should create a semi-transparent wash to coat the content so that it can show
// through the selection. This wash should be of the specified background-color if that is not transparent, else of the
// specified color; however the UA may adjust the alpha channel.
fn paint_custom_highlight_washes<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    rect: libgfx_rust::IntRect,
) {
    let Some(layers) = recorder.paint_state.custom_highlights.replaced.get(&paintable).cloned() else {
        return;
    };
    let Some(style) = recorder.source.node_style_if_live(paintable) else {
        return;
    };
    let backdrop = libgfx_rust::Color(style.background_color());
    // https://drafts.csswg.org/css-pseudo-4/#highlight-text
    // [...] currentColor on a highlight pseudo-element’s color property represents the color of the next active
    // highlight pseudo-element layer below, falling back finally to the colors that would otherwise have been
    // used [...].
    let mut current_color = libgfx_rust::Color(style.inherited_text().color);
    for layer in layers {
        let Some(answer) = recorder.custom_highlight_style(paintable, layer) else {
            continue;
        };
        let facts = &answer.facts;
        if !facts.colors_authored {
            continue;
        }
        let color = facts.text_color.get().unwrap_or(current_color);
        current_color = color;
        let background_color = if facts.background_color_is_current_color {
            color
        } else {
            facts.background_color
        };
        let wash_color = if background_color.alpha() > 0 {
            background_color
        } else {
            color
        };
        if wash_color.alpha() == 0 {
            continue;
        }
        let previous = recorder.recorder.set_contrast_backdrop(Some(backdrop));
        recorder.recorder.fill_rect(rect, wash_color, ForceDarkRole::Selection);
        recorder.recorder.set_contrast_backdrop(previous);
    }
}

pub(crate) fn paint_canvas_foreground<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    let replaced_facts = recorder.source.replaced_paint_facts(paintable);
    let (_, image_rendering) = replaced_style(recorder, paintable);
    let canvas_rect = recorder
        .converter
        .rounded_device_rect(absolute_rect(recorder.source, paintable));

    // NB: A canvas whose context is an ImageBitmapRenderingContext paints the output bitmap of the context as its image.
    // AD-HOC: The spec has the canvas take the natural dimensions of its output bitmap. Instead, the canvas keeps the
    //         natural dimensions its width and height attributes give it, and the output bitmap is stretched to fill
    //         its content box — so a transfer never changes the layout of the page.
    //         https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-element
    // FIXME: Composite the output bitmap onto opaque black when the alpha flag of the context is false.
    if let Some(image_facts) = replaced_facts.clone().and_then(|facts| facts.image()) {
        if image_facts.content == crate::painting::image_content::ImageContent::None {
            return;
        }
        let (content_rect, corner_radii) = replaced_content_clip_geometry(recorder, paintable);
        let corner_clip =
            corner_radii.map(|corner_radii| PendingInlineClip::intersecting_rounded_rect(content_rect, corner_radii));
        recorder.record_with_inline_clips(corner_clip.as_slice(), |recorder| {
            paint_replaced_image_content(
                recorder,
                paintable,
                &image_facts.content,
                canvas_rect.to_float(),
                image_rendering,
            );
        });
        return;
    }

    let facts = replaced_facts.and_then(|facts| facts.canvas()).unwrap_or_default();
    if !facts.has_content {
        return;
    }
    let (content_rect, corner_radii) = replaced_content_clip_geometry(recorder, paintable);
    let corner_clip =
        corner_radii.map(|corner_radii| PendingInlineClip::intersecting_rounded_rect(content_rect, corner_radii));
    recorder.record_with_inline_clips(corner_clip.as_slice(), |recorder| {
        let scaling_mode = to_gfx_scaling_mode(
            image_rendering,
            (facts.content_width, facts.content_height),
            (canvas_rect.width, canvas_rect.height),
        );
        recorder.recorder.draw_canvas(
            canvas_rect,
            CanvasId(facts.canvas_id),
            facts.content_generation,
            scaling_mode,
        );
    });
}

pub(crate) fn paint_video_foreground<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    let facts = recorder
        .source
        .replaced_paint_facts(paintable)
        .and_then(|facts| facts.video())
        .unwrap_or_default();
    let (object_fit, image_rendering) = replaced_style(recorder, paintable);
    let video_rect = recorder
        .converter
        .rounded_device_rect(absolute_rect(recorder.source, paintable));
    let (content_rect, corner_radii) = replaced_content_clip_geometry(recorder, paintable);
    let mut inline_clips = vec![PendingInlineClip::intersecting_float_rect(content_rect)];
    if let Some(corner_radii) = corner_radii {
        inline_clips.push(PendingInlineClip::intersecting_rounded_rect(content_rect, corner_radii));
    }
    recorder.record_with_inline_clips(&inline_clips, |recorder| match &facts {
        VideoPaintFacts::VideoFrame(Some(video_frame)) => {
            let src_size = (video_frame.src_width, video_frame.src_height);
            let dst_rect = get_replaced_box_painting_area(
                recorder,
                paintable,
                object_fit,
                CssPixelSize::new(
                    CssPixels::from_integer(src_size.0 as i64),
                    CssPixels::from_integer(src_size.1 as i64),
                ),
            );
            if !dst_rect.is_empty() {
                let scaling_mode = to_gfx_scaling_mode(image_rendering, src_size, (dst_rect.width, dst_rect.height));
                let sink_id = recorder.register_video_sink(video_frame.sink_resource_id, video_frame.sink_handle);
                recorder.recorder.draw_video_frame(dst_rect, sink_id, scaling_mode);
            }
        }
        VideoPaintFacts::PosterFrame(Some(poster_frame)) => {
            let frame_size = (poster_frame.width(), poster_frame.height());
            let dst_rect = get_replaced_box_painting_area(
                recorder,
                paintable,
                object_fit,
                CssPixelSize::new(
                    CssPixels::from_integer(frame_size.0 as i64),
                    CssPixels::from_integer(frame_size.1 as i64),
                ),
            );
            if !dst_rect.is_empty() {
                crate::painting::record::paint::background::paint_decoded_image_frame(
                    recorder,
                    poster_frame,
                    dst_rect.to_float(),
                    image_rendering,
                    libgfx_rust::CompositingAndBlendingOperator::Normal,
                    ForceDarkRole::Foreground,
                );
            }
        }
        VideoPaintFacts::VideoFrame(None) | VideoPaintFacts::PosterFrame(None) => {}
        VideoPaintFacts::TransparentBlack => {
            recorder
                .recorder
                .fill_rect(video_rect, Color::TRANSPARENT, ForceDarkRole::Background);
        }
    });
}

pub(crate) fn paint_navigable_container_foreground<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
) {
    let facts = recorder
        .source
        .replaced_paint_facts(paintable)
        .and_then(|facts| facts.navigable_container())
        .unwrap_or_default();
    if !facts.has_composited_context {
        return;
    }
    let absolute_rect = absolute_rect(recorder.source, paintable);
    let (content_rect, corner_radii) = replaced_content_clip_geometry(recorder, paintable);
    let mut inline_clips = vec![PendingInlineClip::intersecting_float_rect(content_rect)];
    if let Some(corner_radii) = corner_radii {
        inline_clips.push(PendingInlineClip::intersecting_rounded_rect(content_rect, corner_radii));
    }

    let scale = recorder.converter.device_pixels_per_css_pixel();
    let destination_rect = FloatRect::new(
        (absolute_rect.x.to_double() * scale) as f32,
        (absolute_rect.y.to_double() * scale) as f32,
        (absolute_rect.width.to_double() * scale) as f32,
        (absolute_rect.height.to_double() * scale) as f32,
    );
    let child_context_id = CompositorContextId(facts.composited_context_id);
    recorder.record_with_inline_clips(&inline_clips, |recorder| {
        recorder
            .recorder
            .draw_composited_context(destination_rect, child_context_id, ScalingMode::NearestNeighbor);
    });

    // The compositor only reads the metadata outside groups, and a record under ambient clips would count as drawn.
    if destination_rect.is_empty()
        || !recorder.visible_for_hit_testing(paintable)
        || recorder.recorder.is_recording_inside_group()
        || recorder.is_recording_svg_resource_content()
    {
        return;
    }
    recorder
        .recorder
        .compositor_nested_context_wheel_region(CompositorNestedContextWheelRegion {
            rect: destination_rect,
            corner_radii: corner_radii.unwrap_or_default(),
            child_context_id,
        });
}
