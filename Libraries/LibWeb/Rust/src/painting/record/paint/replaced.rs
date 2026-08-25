/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{image_rendering, object_fit};
use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelRect, CssPixelSize};
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::{
    CanvasId, CompositorContextId, ImageFrameResourceId, VideoSinkResourceId,
};
use crate::painting::host::FfiReplacedPaintFacts;
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background::{paint_image, to_gfx_scaling_mode};
use crate::painting::record::paint::{begin_corner_clip, border_radii_shrunk_for_borders, end_corner_clip};
use libgfx_rust::{Color, IntRect, ScalingMode};

#[derive(Clone, Copy)]
pub(crate) struct Fraction {
    pub numerator: CssPixels,
    pub denominator: CssPixels,
}

impl Fraction {
    fn one() -> Self {
        Self {
            numerator: CssPixels::from_integer(1),
            denominator: CssPixels::from_integer(1),
        }
    }

    fn of(numerator: CssPixels, denominator: CssPixels) -> Self {
        Self { numerator, denominator }
    }

    fn to_css_pixels(self) -> CssPixels {
        self.numerator.div_as_fraction(self.denominator)
    }

    fn might_be_saturated(self) -> bool {
        let raw = self.to_css_pixels().raw_value();
        raw == i32::MAX || raw == i32::MIN
    }

    fn to_double(self) -> f64 {
        self.numerator.to_double() / self.denominator.to_double()
    }

    fn multiply(self, value: CssPixels) -> CssPixels {
        let wide = value.raw_value() as i64 * self.numerator.raw_value() as i64;
        CssPixels::from_raw((wide / self.denominator.raw_value() as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32)
    }

    fn divide(self, value: CssPixels) -> CssPixels {
        let wide = value.raw_value() as i64 * self.denominator.raw_value() as i64;
        CssPixels::from_raw((wide / self.numerator.raw_value() as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32)
    }

    fn ge(self, other: Self) -> bool {
        let left = self.numerator.raw_value() as i64 * other.denominator.raw_value() as i64;
        let right = other.numerator.raw_value() as i64 * self.denominator.raw_value() as i64;
        left >= right
    }
}

pub(crate) struct SizeWithAspectRatio {
    pub width: Option<CssPixels>,
    pub height: Option<CssPixels>,
    pub aspect_ratio: Option<Fraction>,
}

pub(crate) fn run_default_sizing_algorithm(
    specified_width: Option<CssPixels>,
    specified_height: Option<CssPixels>,
    natural: &SizeWithAspectRatio,
    default_size: CssPixelSize,
) -> CssPixelSize {
    if let (Some(width), Some(height)) = (specified_width, specified_height) {
        return CssPixelSize::new(width, height);
    }
    if specified_width.is_some() || specified_height.is_some() {
        if let Some(aspect_ratio) = natural.aspect_ratio
            && !aspect_ratio.might_be_saturated()
        {
            if let Some(width) = specified_width {
                return CssPixelSize::new(width, aspect_ratio.divide(width));
            }
            if let Some(height) = specified_height {
                return CssPixelSize::new(aspect_ratio.multiply(height), height);
            }
        }
        if let (Some(height), Some(natural_width)) = (specified_height, natural.width) {
            return CssPixelSize::new(natural_width, height);
        }
        if let (Some(width), Some(natural_height)) = (specified_width, natural.height) {
            return CssPixelSize::new(width, natural_height);
        }
        if let Some(height) = specified_height {
            return CssPixelSize::new(default_size.width, height);
        }
        if let Some(width) = specified_width {
            return CssPixelSize::new(width, default_size.height);
        }
        unreachable!();
    }
    if natural.width.is_some() || natural.height.is_some() {
        return run_default_sizing_algorithm(natural.width, natural.height, natural, default_size);
    }
    if let Some(aspect_ratio) = natural.aspect_ratio
        && !aspect_ratio.might_be_saturated()
    {
        if default_size.is_empty() {
            return default_size;
        }
        let default_width = default_size.width.to_double();
        let default_height = default_size.height.to_double();
        if aspect_ratio.to_double() >= default_width / default_height {
            return CssPixelSize::new(
                default_size.width,
                CssPixels::nearest_value_for(default_width / aspect_ratio.to_double()),
            );
        }
        return CssPixelSize::new(
            CssPixels::nearest_value_for(default_height * aspect_ratio.to_double()),
            default_size.height,
        );
    }
    default_size
}

pub(crate) fn get_replaced_box_painting_area(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    mut object_fit: u8,
    content_size: CssPixelSize,
) -> IntRect {
    if content_size.is_empty() {
        return IntRect::default();
    }
    let paintable_rect = absolute_rect(recorder.layout_arena, paintable);
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
        let style = recorder.layout_arena.node_style_if_live(paintable);
        let zero = crate::css::css_pixels::CssPixels::from_raw(0);
        match style {
            Some(style) => {
                let misc = style.misc_reset();
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

fn replaced_facts(recorder: &PaintRecorder<'_>, paintable: NodeSlotId) -> FfiReplacedPaintFacts {
    recorder
        .paint_host
        .replaced_paint_facts(recorder.layout_node_shell(paintable))
}

fn replaced_style(recorder: &PaintRecorder<'_>, paintable: NodeSlotId) -> (u8, u8) {
    recorder
        .layout_arena
        .node_style_if_live(paintable)
        .map_or((object_fit::FILL, image_rendering::AUTO), |style| {
            (style.misc_reset().object_fit, style.image_rendering())
        })
}

pub(crate) fn paint_image_foreground(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let facts = replaced_facts(recorder, paintable);
    let (object_fit, image_rendering) = replaced_style(recorder, paintable);
    let image_rect = absolute_rect(recorder.layout_arena, paintable);
    let image_rect_device_pixels = recorder.converter.rounded_device_rect(image_rect);
    if facts.has_decoded_image_data {
        let radii = border_radii_shrunk_for_borders(recorder, paintable);
        let corner_clip = begin_corner_clip(
            recorder,
            image_rect_device_pixels,
            &radii,
            libgfx_rust::CornerClip::Outside,
        );

        // https://drafts.csswg.org/css-images/#the-object-fit
        let natural_size = SizeWithAspectRatio {
            width: facts.natural_width.has_value.then_some(facts.natural_width.value),
            height: facts.natural_height.has_value.then_some(facts.natural_height.value),
            aspect_ratio: facts.has_natural_aspect_ratio.then(|| {
                Fraction::of(
                    facts.natural_aspect_ratio_numerator,
                    facts.natural_aspect_ratio_denominator,
                )
            }),
        };
        let concrete_object_size = run_default_sizing_algorithm(None, None, &natural_size, image_rect.size());

        let draw_rect = get_replaced_box_painting_area(recorder, paintable, object_fit, concrete_object_size);
        if !draw_rect.is_empty() {
            let draw_rect_needs_clip = !image_rect_device_pixels.contains_rect(draw_rect);
            if draw_rect_needs_clip {
                recorder.recorder.save();
                recorder.recorder.add_clip_rect_int(image_rect_device_pixels);
            }
            let dest_rect = draw_rect.to_float();
            let accumulated_scale = recorder.accumulated_2d_scale_at(recorder.recorder.accumulated_visual_context().0);
            let paint = recorder.paint_host.replaced_image_paint(
                recorder.layout_node_shell(paintable),
                dest_rect,
                accumulated_scale,
            );
            if paint.image_paint_kind != crate::painting::host::FfiImagePaintKind::None {
                paint_image(recorder, &paint, dest_rect, image_rendering);
            }
            if draw_rect_needs_clip {
                recorder.recorder.restore();
            }
        }
        end_corner_clip(recorder, corner_clip);
    }

    if recorder.data(paintable).selection_state != 0 {
        let selection_background_color = facts.selection_background_color;
        if selection_background_color.alpha() > 0 {
            recorder
                .recorder
                .fill_rect(image_rect_device_pixels, selection_background_color);
        }
    }
}

pub(crate) fn paint_canvas_foreground(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let facts = replaced_facts(recorder, paintable);
    let (_, image_rendering) = replaced_style(recorder, paintable);
    let canvas_rect = recorder
        .converter
        .rounded_device_rect(absolute_rect(recorder.layout_arena, paintable));
    let radii = border_radii_shrunk_for_borders(recorder, paintable);
    let corner_clip = begin_corner_clip(recorder, canvas_rect, &radii, libgfx_rust::CornerClip::Outside);
    if facts.has_canvas_content {
        let scaling_mode = to_gfx_scaling_mode(
            image_rendering,
            (facts.canvas_content_width, facts.canvas_content_height),
            (canvas_rect.width, canvas_rect.height),
        );
        recorder.recorder.draw_canvas(
            canvas_rect,
            CanvasId(facts.canvas_id),
            facts.canvas_content_generation,
            scaling_mode,
        );
    }
    end_corner_clip(recorder, corner_clip);
}

pub(crate) fn paint_video_foreground(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let facts = replaced_facts(recorder, paintable);
    let (object_fit, image_rendering) = replaced_style(recorder, paintable);
    recorder.recorder.save();
    let video_rect = recorder
        .converter
        .rounded_device_rect(absolute_rect(recorder.layout_arena, paintable));
    recorder.recorder.add_clip_rect_int(video_rect);
    let radii = border_radii_shrunk_for_borders(recorder, paintable);
    let corner_clip = begin_corner_clip(recorder, video_rect, &radii, libgfx_rust::CornerClip::Outside);

    match facts.video_representation {
        crate::painting::host::FfiVideoRepresentation::VideoFrame => {
            if facts.has_video_frame {
                let src_size = (facts.video_src_width, facts.video_src_height);
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
                    let scaling_mode =
                        to_gfx_scaling_mode(image_rendering, src_size, (dst_rect.width, dst_rect.height));
                    recorder.recorder.draw_video_frame(
                        dst_rect,
                        VideoSinkResourceId(facts.video_sink_storage_id),
                        scaling_mode,
                    );
                }
            }
        }
        crate::painting::host::FfiVideoRepresentation::PosterFrame => {
            if facts.has_poster_frame {
                let frame_size = (facts.poster_width, facts.poster_height);
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
                    let scaling_mode =
                        to_gfx_scaling_mode(image_rendering, frame_size, (dst_rect.width, dst_rect.height));
                    recorder.recorder.draw_scaled_decoded_image_frame(
                        dst_rect.to_float(),
                        None,
                        ImageFrameResourceId(facts.poster_frame_id),
                        scaling_mode,
                        libgfx_rust::CompositingAndBlendingOperator::Normal,
                        None,
                    );
                }
            }
        }
        crate::painting::host::FfiVideoRepresentation::TransparentBlack => {
            recorder.recorder.fill_rect(video_rect, Color::TRANSPARENT);
        }
    }

    end_corner_clip(recorder, corner_clip);
    recorder.recorder.restore();
}

pub(crate) fn paint_navigable_container_foreground(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let facts = replaced_facts(recorder, paintable);
    let absolute_rect = absolute_rect(recorder.layout_arena, paintable);
    let clip_rect = recorder.converter.rounded_device_rect(absolute_rect);
    let radii = border_radii_shrunk_for_borders(recorder, paintable);
    let corner_clip = begin_corner_clip(recorder, clip_rect, &radii, libgfx_rust::CornerClip::Outside);
    if facts.has_composited_context {
        recorder.recorder.save();
        recorder.recorder.add_clip_rect_int(clip_rect);
        recorder.recorder.draw_composited_context(
            recorder.converter.enclosing_device_rect(absolute_rect),
            CompositorContextId(facts.composited_context_id),
            ScalingMode::NearestNeighbor,
        );
        recorder.recorder.restore();
    }
    end_corner_clip(recorder, corner_clip);
}
