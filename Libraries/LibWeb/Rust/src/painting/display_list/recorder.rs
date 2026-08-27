/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::builder::{AppendContext, CommandRange, ContextRewrite, DisplayListBuilder, HEADER_SIZE};
use super::commands::*;
use crate::painting::display_list::ffi_bytes::FfiBytes;
use libgfx_rust::*;

pub struct CommandPayloadBuilder {
    payload_start_offset: usize,
    payload_size: usize,
    inline_payload: Vec<u8>,
}

impl CommandPayloadBuilder {
    pub fn new<C: DisplayListCommand>(builder: &DisplayListBuilder) -> Self {
        debug_assert_eq!(builder.byte_size() % super::builder::COMMAND_ALIGNMENT, 0);
        Self {
            payload_start_offset: builder.byte_size() + HEADER_SIZE,
            payload_size: std::mem::size_of::<C>(),
            inline_payload: Vec::new(),
        }
    }

    pub fn append_data(&mut self, bytes: &[u8], alignment: usize) -> DisplayListDataSpan {
        assert!(alignment > 0);
        let absolute_offset = self.payload_start_offset + self.payload_size;
        let padded_offset = absolute_offset.next_multiple_of(alignment);
        let padding = padded_offset - absolute_offset;
        let payload_relative_offset = self.payload_size + padding;
        self.inline_payload.resize(self.inline_payload.len() + padding, 0);
        self.inline_payload.extend_from_slice(bytes);
        self.payload_size = payload_relative_offset + bytes.len();
        DisplayListDataSpan {
            offset: u32::try_from(payload_relative_offset).expect("display list payload exceeds u32"),
            size: u32::try_from(bytes.len()).expect("display list payload exceeds u32"),
        }
    }

    pub fn append_objects<T: FfiBytes>(&mut self, objects: &[T]) -> DisplayListDataSpan {
        let size = std::mem::size_of::<T>();
        let mut bytes = vec![0u8; std::mem::size_of_val(objects)];
        for (index, object) in objects.iter().enumerate() {
            object.write_ffi_bytes(&mut bytes[index * size..(index + 1) * size]);
        }
        self.append_data(&bytes, std::mem::align_of::<T>())
    }

    pub fn append_f32s(&mut self, values: &[f32]) -> DisplayListDataSpan {
        let mut bytes = Vec::with_capacity(values.len() * 4);
        for value in values {
            bytes.extend_from_slice(&value.to_ne_bytes());
        }
        self.append_data(&bytes, std::mem::align_of::<f32>())
    }

    pub fn append_colors(&mut self, colors: &[Color]) -> DisplayListDataSpan {
        let mut bytes = Vec::with_capacity(colors.len() * 4);
        for color in colors {
            bytes.extend_from_slice(&color.0.to_ne_bytes());
        }
        self.append_data(&bytes, std::mem::align_of::<u32>())
    }

    pub fn inline_data(&self) -> &[u8] {
        &self.inline_payload
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ColorStops {
    pub colors: Vec<Color>,
    pub positions: Vec<f32>,
    pub repeating: bool,
}

impl ColorStops {
    fn append_to(&self, payload: &mut CommandPayloadBuilder) -> DisplayListGradientColorStops {
        assert_eq!(self.colors.len(), self.positions.len());
        DisplayListGradientColorStops {
            colors: payload.append_colors(&self.colors),
            positions: payload.append_f32s(&self.positions),
            repeating: self.repeating,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct LinearGradientData {
    pub gradient_angle: f32,
    pub color_stops: ColorStops,
    pub first_stop_position: f32,
    pub repeat_length: f32,
    pub interpolation_method: GradientInterpolationMethod,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ConicGradientData {
    pub start_angle: f32,
    pub color_stops: ColorStops,
    pub interpolation_method: GradientInterpolationMethod,
}

#[derive(Clone, Debug, PartialEq)]
pub struct RadialGradientData {
    pub color_stops: ColorStops,
    pub interpolation_method: GradientInterpolationMethod,
}

#[derive(Clone, Debug, PartialEq)]
pub enum PaintStyle {
    LinearGradient {
        gradient_transform: OptionalAffineTransform,
        spread_method: DisplayListGradientSpreadMethod,
        color_space: InterpolationColorSpace,
        color_stops: ColorStops,
        start_point: FloatPoint,
        end_point: FloatPoint,
    },
    RadialGradient {
        gradient_transform: OptionalAffineTransform,
        spread_method: DisplayListGradientSpreadMethod,
        color_space: InterpolationColorSpace,
        color_stops: ColorStops,
        start_center: FloatPoint,
        start_radius: f32,
        end_center: FloatPoint,
        end_radius: f32,
    },
    Pattern {
        tile_display_list_id: DisplayListResourceId,
        tile_rect: FloatRect,
        content_scale: FloatSize,
        pattern_transform: OptionalAffineTransform,
    },
}

impl PaintStyle {
    fn append_to(&self, payload: &mut CommandPayloadBuilder) -> DisplayListPaintStyle {
        let mut style = DisplayListPaintStyle::default();
        match self {
            PaintStyle::LinearGradient {
                gradient_transform,
                spread_method,
                color_space,
                color_stops,
                start_point,
                end_point,
            } => {
                style.paint_style_type = DisplayListPaintStyleType::LinearGradient;
                style.gradient = DisplayListGradientPaintStyle {
                    gradient_transform: *gradient_transform,
                    spread_method: *spread_method,
                    color_space: *color_space,
                    color_stops: color_stops.append_to(payload),
                };
                style.linear_gradient_start_point = *start_point;
                style.linear_gradient_end_point = *end_point;
            }
            PaintStyle::RadialGradient {
                gradient_transform,
                spread_method,
                color_space,
                color_stops,
                start_center,
                start_radius,
                end_center,
                end_radius,
            } => {
                style.paint_style_type = DisplayListPaintStyleType::RadialGradient;
                style.gradient = DisplayListGradientPaintStyle {
                    gradient_transform: *gradient_transform,
                    spread_method: *spread_method,
                    color_space: *color_space,
                    color_stops: color_stops.append_to(payload),
                };
                style.radial_gradient_start_center = *start_center;
                style.radial_gradient_start_radius = *start_radius;
                style.radial_gradient_end_center = *end_center;
                style.radial_gradient_end_radius = *end_radius;
            }
            PaintStyle::Pattern {
                tile_display_list_id,
                tile_rect,
                content_scale,
                pattern_transform,
            } => {
                style.paint_style_type = DisplayListPaintStyleType::Pattern;
                style.pattern_tile_display_list_id = *tile_display_list_id;
                style.pattern_tile_rect = *tile_rect;
                style.pattern_content_scale = *content_scale;
                style.pattern_transform = *pattern_transform;
            }
        }
        style
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum PaintStyleOrColor {
    Color(Color),
    PaintStyle(PaintStyle),
}

pub struct FillPathParams<'a> {
    pub path: &'a libgfx_rust::path::OwnedPath,
    pub opacity: f32,
    pub paint_style_or_color: PaintStyleOrColor,
    pub winding_rule: WindingRule,
    pub should_anti_alias: ShouldAntiAlias,
}

pub struct StrokePathParams<'a> {
    pub cap_style: CapStyle,
    pub join_style: JoinStyle,
    pub miter_limit: f32,
    pub dash_array: Vec<f32>,
    pub dash_offset: f32,
    pub path: &'a libgfx_rust::path::OwnedPath,
    pub opacity: f32,
    pub paint_style_or_color: PaintStyleOrColor,
    pub thickness: f32,
    pub should_anti_alias: ShouldAntiAlias,
}

pub struct GlyphRunForRecording<'a> {
    pub font_id: FontResourceId,
    pub glyphs: &'a [DisplayListGlyph],
}

pub struct DisplayListRecorder {
    builder: DisplayListBuilder,
    empty_effective_clips_by_frame: Vec<bool>,
    context: ContextRef,
    mask_display_lists: Vec<(FrameNodeIndex, DisplayListResourceId)>,
    confining_clip: Option<IntRect>,
}

impl DisplayListRecorder {
    pub fn new(empty_effective_clips_by_frame: Vec<bool>) -> Self {
        Self {
            builder: DisplayListBuilder::new(),
            empty_effective_clips_by_frame,
            context: ContextRef::default(),
            mask_display_lists: Vec::new(),
            confining_clip: None,
        }
    }

    pub fn record_clipped_to(&mut self, clip: IntRect, record: impl FnOnce(&mut Self)) {
        let outer_clip = self.confining_clip;
        self.confining_clip = Some(outer_clip.map_or(clip, |outer| outer.intersected(clip)));
        record(self);
        self.confining_clip = outer_clip;
    }

    pub fn register_mask_display_list(&mut self, frames: &[FrameNodeIndex], display_list_id: DisplayListResourceId) {
        for frame in frames {
            self.mask_display_lists.push((*frame, display_list_id));
        }
    }

    pub fn take_mask_display_lists(&mut self) -> Vec<(FrameNodeIndex, DisplayListResourceId)> {
        std::mem::take(&mut self.mask_display_lists)
    }

    pub fn builder(&self) -> &DisplayListBuilder {
        &self.builder
    }

    pub fn mask_display_lists(&self) -> &[(FrameNodeIndex, DisplayListResourceId)] {
        &self.mask_display_lists
    }

    pub fn into_builder(self) -> DisplayListBuilder {
        self.builder
    }

    pub fn byte_size(&self) -> usize {
        self.builder.byte_size()
    }

    pub fn accumulated_visual_context(&self) -> ContextRef {
        self.context
    }

    pub fn set_accumulated_visual_context(&mut self, context: ContextRef) {
        self.context = context;
    }

    pub fn has_empty_effective_clip(&self, frame: FrameNodeIndex) -> bool {
        self.empty_effective_clips_by_frame
            .get(frame.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    fn append_context(&self) -> AppendContext {
        AppendContext {
            context: self.context,
            has_empty_effective_clip: self.has_empty_effective_clip(self.context.frame),
        }
    }

    fn append_command<C: DisplayListCommand>(&mut self, command: &C, inline_data: &[u8]) {
        let context = self.append_context();
        self.builder
            .append_confined_to_clip(command, inline_data, context, self.confining_clip);
    }

    pub fn append_cached_command_range(
        &mut self,
        source: &[u8],
        range: CommandRange,
        recorded_context: ContextRef,
        recorded_local_frame_range: (u32, u32),
        current_local_frame_range: (u32, u32),
    ) -> CommandRange {
        let offset = self.builder.append_command_range(
            source,
            range,
            Some(ContextRewrite {
                recorded_context,
                current_context: self.context,
                recorded_local_frame_range,
                current_local_frame_range,
            }),
        );
        CommandRange {
            offset,
            size: range.size,
        }
    }

    /// Copies a cached command range without rewriting visual-context indices. The caller must
    /// establish that the recorded indices are still valid for the current visual context tree.
    pub fn append_cached_command_range_verbatim(&mut self, source: &[u8], range: CommandRange) -> CommandRange {
        let offset = self.builder.append_command_range(source, range, None);
        CommandRange {
            offset,
            size: range.size,
        }
    }

    pub fn fill_rect(&mut self, rect: IntRect, color: Color) {
        if rect.is_empty() || color.alpha() == 0 {
            return;
        }
        self.append_command(&FillRect { rect, color }, &[]);
    }

    pub fn fill_rect_transparent(&mut self, rect: IntRect) {
        if rect.is_empty() {
            return;
        }
        self.append_command(
            &FillRect {
                rect,
                color: Color::TRANSPARENT,
            },
            &[],
        );
    }

    fn resolve_paint(
        payload: &mut CommandPayloadBuilder,
        paint: &PaintStyleOrColor,
    ) -> (PathPaintKind, Color, DisplayListPaintStyle) {
        match paint {
            PaintStyleOrColor::Color(color) => (PathPaintKind::Color, *color, DisplayListPaintStyle::default()),
            PaintStyleOrColor::PaintStyle(style) => {
                (PathPaintKind::PaintStyle, Color::default(), style.append_to(payload))
            }
        }
    }

    pub fn fill_path(&mut self, params: FillPathParams<'_>) {
        if let PaintStyleOrColor::Color(color) = &params.paint_style_or_color
            && color.alpha() == 0
        {
            return;
        }
        let path_bounding_rect = FloatRect::from_array(params.path.bounding_box());
        if path_bounding_rect.is_empty() {
            return;
        }
        let mut payload = CommandPayloadBuilder::new::<FillPath>(&self.builder);
        let path_data = payload.append_data(&params.path.serialize_to_bytes(), std::mem::align_of::<u32>());
        let (paint_kind, color, paint_style) = Self::resolve_paint(&mut payload, &params.paint_style_or_color);
        let command = FillPath {
            path_bounding_rect,
            path_data,
            opacity: params.opacity,
            paint_kind,
            color,
            paint_style,
            winding_rule: params.winding_rule,
            should_anti_alias: params.should_anti_alias,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn stroke_path(&mut self, params: StrokePathParams<'_>) {
        // Skia treats zero thickness as a special case and will draw a hairline, while we want to
        // draw nothing.
        if params.thickness == 0.0 {
            return;
        }
        if let PaintStyleOrColor::Color(color) = &params.paint_style_or_color
            && color.alpha() == 0
        {
            return;
        }
        let mut path_bounding_rect = FloatRect::from_array(params.path.bounding_box());
        // Increase path bounding box by `thickness` to account for stroke.
        path_bounding_rect = path_bounding_rect.inflated(params.thickness, params.thickness);
        if path_bounding_rect.is_empty() {
            return;
        }
        let mut payload = CommandPayloadBuilder::new::<StrokePath>(&self.builder);
        let path_data = payload.append_data(&params.path.serialize_to_bytes(), std::mem::align_of::<u32>());
        let dash_array = payload.append_f32s(&params.dash_array);
        let (paint_kind, color, paint_style) = Self::resolve_paint(&mut payload, &params.paint_style_or_color);
        let command = StrokePath {
            cap_style: params.cap_style,
            join_style: params.join_style,
            miter_limit: params.miter_limit,
            dash_array,
            dash_offset: params.dash_offset,
            path_bounding_rect,
            path_data,
            opacity: params.opacity,
            paint_kind,
            color,
            paint_style,
            thickness: params.thickness,
            should_anti_alias: params.should_anti_alias,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn draw_ellipse(&mut self, rect: IntRect, color: Color, thickness: i32) {
        if rect.is_empty() || color.alpha() == 0 || thickness == 0 {
            return;
        }
        self.append_command(&DrawEllipse { rect, color, thickness }, &[]);
    }

    pub fn fill_rect_with_linear_gradient(&mut self, gradient_rect: IntRect, data: &LinearGradientData) {
        if gradient_rect.is_empty() {
            return;
        }
        let mut payload = CommandPayloadBuilder::new::<PaintLinearGradient>(&self.builder);
        let color_stops = data.color_stops.append_to(&mut payload);
        let command = PaintLinearGradient {
            gradient_rect,
            gradient_angle: data.gradient_angle,
            color_stops,
            first_stop_position: data.first_stop_position,
            repeat_length: data.repeat_length,
            interpolation_method: data.interpolation_method,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn fill_rect_with_conic_gradient(&mut self, rect: IntRect, data: &ConicGradientData, position: IntPoint) {
        if rect.is_empty() {
            return;
        }
        let mut payload = CommandPayloadBuilder::new::<PaintConicGradient>(&self.builder);
        let color_stops = data.color_stops.append_to(&mut payload);
        let command = PaintConicGradient {
            rect,
            start_angle: data.start_angle,
            color_stops,
            interpolation_method: data.interpolation_method,
            position,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn fill_rect_with_radial_gradient(
        &mut self,
        rect: IntRect,
        data: &RadialGradientData,
        center: IntPoint,
        size: IntSize,
    ) {
        if rect.is_empty() {
            return;
        }
        let mut payload = CommandPayloadBuilder::new::<PaintRadialGradient>(&self.builder);
        let color_stops = data.color_stops.append_to(&mut payload);
        let command = PaintRadialGradient {
            rect,
            color_stops,
            interpolation_method: data.interpolation_method,
            center,
            size,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn draw_rect(&mut self, rect: IntRect, color: Color, rough: bool) {
        if rect.is_empty() || color.alpha() == 0 {
            return;
        }
        self.append_command(&DrawRect { rect, color, rough }, &[]);
    }

    pub fn draw_composited_context(
        &mut self,
        dst_rect: IntRect,
        child_context_id: CompositorContextId,
        scaling_mode: ScalingMode,
    ) {
        if dst_rect.is_empty() {
            return;
        }
        self.append_command(
            &DrawCompositedContext {
                dst_rect,
                child_context_id,
                scaling_mode,
            },
            &[],
        );
    }

    pub fn draw_canvas(
        &mut self,
        dst_rect: IntRect,
        canvas_id: CanvasId,
        content_generation: u64,
        scaling_mode: ScalingMode,
    ) {
        if dst_rect.is_empty() {
            return;
        }
        self.append_command(
            &DrawCanvas {
                dst_rect,
                canvas_id,
                content_generation,
                scaling_mode,
            },
            &[],
        );
    }

    pub fn draw_video_frame(
        &mut self,
        dst_rect: IntRect,
        video_sink_id: VideoSinkResourceId,
        scaling_mode: ScalingMode,
    ) {
        if dst_rect.is_empty() {
            return;
        }
        self.append_command(
            &DrawVideoFrame {
                dst_rect,
                video_sink_id,
                scaling_mode,
            },
            &[],
        );
    }

    pub fn draw_scaled_decoded_image_frame(
        &mut self,
        dst_rect: FloatRect,
        src_rect: Option<FloatRect>,
        frame_id: ImageFrameResourceId,
        scaling_mode: ScalingMode,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        isolated_backdrop_color: Option<Color>,
    ) {
        if dst_rect.is_empty() {
            return;
        }
        if src_rect.is_some_and(|src| src.is_empty()) {
            return;
        }
        self.append_command(
            &DrawScaledDecodedImageFrame {
                dst_rect,
                src_rect: OptionalFloatRect::from(src_rect),
                frame_id,
                scaling_mode,
                compositing_and_blending_operator,
                isolated_backdrop_color: OptionalColor::from(isolated_backdrop_color),
            },
            &[],
        );
    }

    #[allow(clippy::too_many_arguments)]
    pub fn draw_repeated_decoded_image_frame(
        &mut self,
        dst_rect: IntRect,
        clip_rect: IntRect,
        frame_id: ImageFrameResourceId,
        scaling_mode: ScalingMode,
        repeat_x: bool,
        repeat_y: bool,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        isolated_backdrop_color: Option<Color>,
    ) {
        if dst_rect.is_empty() || clip_rect.is_empty() {
            return;
        }
        self.append_command(
            &DrawRepeatedDecodedImageFrame {
                dst_rect,
                clip_rect,
                frame_id,
                scaling_mode,
                repeat: Repeat {
                    x: repeat_x,
                    y: repeat_y,
                },
                compositing_and_blending_operator,
                isolated_backdrop_color: OptionalColor::from(isolated_backdrop_color),
            },
            &[],
        );
    }

    pub fn draw_repeated_display_list(
        &mut self,
        dst_rect: IntRect,
        clip_rect: IntRect,
        display_list_id: DisplayListResourceId,
        scaling_mode: ScalingMode,
        repeat_x: bool,
        repeat_y: bool,
    ) {
        if dst_rect.is_empty() || clip_rect.is_empty() {
            return;
        }
        self.record_clipped_to(clip_rect, |recorder| {
            recorder.append_command(
                &DrawRepeatedDisplayList {
                    dst_rect,
                    clip_rect,
                    display_list_id,
                    scaling_mode,
                    repeat: Repeat {
                        x: repeat_x,
                        y: repeat_y,
                    },
                },
                &[],
            );
        });
    }

    #[allow(clippy::too_many_arguments)]
    pub fn draw_tiled_decoded_image_frame(
        &mut self,
        tile_rect: FloatRect,
        clip_rect: IntRect,
        src_rect: FloatRect,
        tile_step: FloatSize,
        frame_id: ImageFrameResourceId,
        scaling_mode: ScalingMode,
        tile_count_x: Option<u32>,
        tile_count_y: Option<u32>,
    ) {
        if tile_rect.is_empty() || clip_rect.is_empty() || src_rect.is_empty() {
            return;
        }
        self.append_command(
            &DrawTiledDecodedImageFrame {
                tile_rect,
                clip_rect,
                src_rect,
                tile_step,
                frame_id,
                scaling_mode,
                tile_count_x: OptionalU32::from(tile_count_x),
                tile_count_y: OptionalU32::from(tile_count_y),
            },
            &[],
        );
    }

    pub fn draw_line(
        &mut self,
        from: IntPoint,
        to: IntPoint,
        color: Color,
        thickness: i32,
        style: LineStyle,
        alternate_color: Color,
    ) {
        if color.alpha() == 0 || thickness == 0 {
            return;
        }
        self.append_command(
            &DrawLine {
                color,
                from,
                to,
                thickness,
                style,
                alternate_color,
            },
            &[],
        );
    }

    #[allow(clippy::too_many_arguments)]
    // Streamlined text drawing routine that does no wrapping/elision/alignment.
    pub fn draw_glyph_run(
        &mut self,
        baseline_start: FloatPoint,
        run: GlyphRunForRecording<'_>,
        color: Color,
        rect: IntRect,
        scale: f64,
        orientation: Orientation,
        glyph_bounding_rect: IntRect,
    ) {
        if color.alpha() == 0 {
            return;
        }
        let mut payload = CommandPayloadBuilder::new::<DrawGlyphRun>(&self.builder);
        let glyphs = payload.append_objects(run.glyphs);
        let command = DrawGlyphRun {
            font_id: run.font_id,
            glyphs,
            rect,
            glyph_bounding_rect,
            translation: baseline_start,
            scale: scale as f32,
            color,
            orientation,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn apply_backdrop_filter(&mut self, backdrop_region: IntRect, corner_radii: CornerRadii, filter_bytes: &[u8]) {
        if backdrop_region.is_empty() {
            return;
        }
        let mut payload = CommandPayloadBuilder::new::<ApplyBackdropFilter>(&self.builder);
        let filter_data = payload.append_data(filter_bytes, std::mem::align_of::<u32>());
        let command = ApplyBackdropFilter {
            backdrop_region,
            corner_radii,
            has_backdrop_filter: true,
            backdrop_filter_data: filter_data,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn paint_outer_box_shadow(&mut self, shadow: PaintOuterBoxShadow) {
        self.append_command(&shadow, &[]);
    }

    pub fn paint_inner_box_shadow(&mut self, shadow: PaintInnerBoxShadow) {
        self.append_command(&shadow, &[]);
    }

    #[allow(clippy::too_many_arguments)]
    pub fn paint_text_shadow(
        &mut self,
        blur_radius: i32,
        bounding_rect: IntRect,
        text_rect: IntRect,
        run: GlyphRunForRecording<'_>,
        glyph_run_scale: f64,
        color: Color,
        draw_location: FloatPoint,
    ) {
        let mut payload = CommandPayloadBuilder::new::<PaintTextShadow>(&self.builder);
        let glyphs = payload.append_objects(run.glyphs);
        let command = PaintTextShadow {
            font_id: run.font_id,
            glyphs,
            shadow_bounding_rect: bounding_rect,
            text_rect,
            draw_location,
            scale: glyph_run_scale as f32,
            blur_radius,
            color,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn fill_rect_with_rounded_corners(&mut self, rect: IntRect, color: Color, corner_radii: CornerRadii) {
        if rect.is_empty() || color.alpha() == 0 {
            return;
        }
        if !corner_radii.has_any_radius() {
            self.fill_rect(rect, color);
            return;
        }
        self.append_command(
            &FillRectWithRoundedCorners {
                rect,
                color,
                corner_radii,
            },
            &[],
        );
    }

    pub fn fill_rect_with_uniform_rounded_corners(&mut self, rect: IntRect, color: Color, radius: i32) {
        self.fill_rect_with_rounded_corners(rect, color, CornerRadii::uniform(radius));
    }

    #[allow(clippy::too_many_arguments)]
    pub fn paint_scrollbar(
        &mut self,
        scroll_node_index: SpatialNodeIndex,
        gutter_rect: IntRect,
        thumb_rect: IntRect,
        track_rect: IntRect,
        scroll_size: f64,
        thumb_color: Color,
        track_color: Color,
        vertical: bool,
    ) {
        self.append_command(
            &PaintScrollBar {
                scroll_node_index,
                gutter_rect,
                thumb_rect,
                track_rect,
                scroll_size,
                thumb_color,
                track_color,
                vertical,
            },
            &[],
        );
    }

    pub fn paint_nested_display_list(
        &mut self,
        display_list_id: DisplayListResourceId,
        rect: FloatRect,
        list_size: IntSize,
    ) {
        self.append_command(
            &PaintNestedDisplayList {
                display_list_id,
                rect,
                list_size,
            },
            &[],
        );
    }

    pub fn compositor_scroll_node(&mut self, node: CompositorScrollNode) {
        self.append_command(&node, &[]);
    }

    pub fn compositor_wheel_hit_test_target(&mut self, target: CompositorWheelHitTestTarget) {
        self.append_command(&target, &[]);
    }

    pub fn compositor_wheel_hit_test_target_with_corner_radii(
        &mut self,
        target: CompositorWheelHitTestTargetWithCornerRadii,
    ) {
        self.append_command(&target, &[]);
    }

    pub fn compositor_main_thread_wheel_event_region(&mut self, region: CompositorMainThreadWheelEventRegion) {
        self.append_command(&region, &[]);
    }

    pub fn compositor_viewport_scrollbar(&mut self, scrollbar: CompositorViewportScrollbar) {
        self.append_command(&scrollbar, &[]);
    }

    pub fn compositor_blocking_wheel_event_region(&mut self, region: CompositorBlockingWheelEventRegion) {
        self.append_command(&region, &[]);
    }
}
