/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::builder::{
    CommandRange, ContextRewrite, DisplayListBuilder, HEADER_SIZE, OpenGroup, PendingInlineClip, RecordedDisplayList,
};
use super::commands::*;
use crate::painting::display_list::ffi_bytes::FfiBytes;
use crate::painting::force_dark::{ForceDarkResolver, ForceDarkRole, ForceDarkSettings};
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
        tile_records: std::rc::Rc<Vec<u8>>,
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
                tile_records,
                tile_rect,
                content_scale,
                pattern_transform,
            } => {
                style.paint_style_type = DisplayListPaintStyleType::Pattern;
                style.pattern_tile = payload.append_data(tile_records, super::builder::COMMAND_ALIGNMENT);
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
    pub force_dark_role: ForceDarkRole,
    pub path: &'a libgfx_rust::path::OwnedPath,
    pub opacity: f32,
    pub paint_style_or_color: PaintStyleOrColor,
    pub winding_rule: WindingRule,
    pub should_anti_alias: ShouldAntiAlias,
}

pub struct StrokePathParams<'a> {
    pub force_dark_role: ForceDarkRole,
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
    pub font_smoothing: u8,
    pub font_id: FontResourceId,
    pub glyphs: &'a [DisplayListGlyph],
}

pub struct IsolatedGroupEffects {
    pub clip_rect: Option<FloatRect>,
    pub opacity: f32,
    pub filter: Option<std::rc::Rc<Vec<u8>>>,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub mask_kind: MaskKind,
}

pub struct DetachedRecords {
    outer_builder: DisplayListBuilder,
    outer_ambient_inline_clips: Vec<PendingInlineClip>,
    outer_ambient_inline_transform: Option<AffineTransform>,
}

pub struct OpenRecorderGroup {
    group: OpenGroup,
    context: ContextRef,
    suspended_ambient_inline_clips: Vec<PendingInlineClip>,
}

#[derive(Default)]
pub struct DisplayListRecorder {
    builder: DisplayListBuilder,
    // Present only while force-dark is on for this page; every color-bearing command resolves through it.
    force_dark: Option<ForceDarkResolver>,
    // The color behind whatever is being drawn right now, as authored: the element's own background, set around
    // the draws whose force-dark result is judged against it (borders and selections). None outside those scopes.
    contrast_backdrop: Option<Color>,
    context: ContextRef,
    ambient_inline_clips: Vec<PendingInlineClip>,
    ambient_inline_transform: Option<AffineTransform>,
}

impl DisplayListRecorder {
    pub fn new(force_dark_settings: Option<ForceDarkSettings>) -> Self {
        Self {
            force_dark: force_dark_settings.map(ForceDarkResolver::new),
            ..Self::default()
        }
    }

    pub fn record_clipped_to(&mut self, clip: IntRect, record: impl FnOnce(&mut Self)) {
        self.record_with_inline_clips(&[PendingInlineClip::intersecting_device_rect(clip)], record);
    }

    pub fn record_with_inline_clips(&mut self, inline_clips: &[PendingInlineClip], record: impl FnOnce(&mut Self)) {
        let enclosing_scope_clip_count = self.ambient_inline_clip_depth();
        self.push_ambient_inline_clips(inline_clips);
        record(self);
        self.truncate_ambient_inline_clips(enclosing_scope_clip_count);
    }

    pub fn ambient_inline_clip_depth(&self) -> usize {
        self.ambient_inline_clips.len()
    }

    pub fn ambient_inline_transform(&self) -> Option<AffineTransform> {
        self.ambient_inline_transform
    }

    pub fn set_ambient_inline_transform(&mut self, transform: Option<AffineTransform>) -> Option<AffineTransform> {
        std::mem::replace(&mut self.ambient_inline_transform, transform)
    }

    pub fn push_ambient_inline_clips(&mut self, inline_clips: &[PendingInlineClip]) {
        self.ambient_inline_clips.extend_from_slice(inline_clips);
    }

    pub fn truncate_ambient_inline_clips(&mut self, enclosing_scope_clip_count: usize) {
        self.ambient_inline_clips.truncate(enclosing_scope_clip_count);
    }

    fn resolve_color(&mut self, color: Color, force_dark_role: ForceDarkRole) -> Color {
        let backdrop = self.contrast_backdrop;
        match &mut self.force_dark {
            Some(resolver) => resolver.resolve_against_backdrop(color, force_dark_role, backdrop),
            None => color,
        }
    }

    /// Swaps in the backdrop for a scope of draws and hands back what was there, for the caller to restore.
    pub fn set_contrast_backdrop(&mut self, backdrop: Option<Color>) -> Option<Color> {
        std::mem::replace(&mut self.contrast_backdrop, backdrop)
    }

    // Images use only this much of the role; what happens after is decided by classifying the image itself.
    fn force_dark_applies(&self, force_dark_role: ForceDarkRole) -> bool {
        self.force_dark.is_some() && force_dark_role != ForceDarkRole::None
    }

    /// Resolves a fill or stroke paint through force-dark; None means the authored paint goes out unchanged.
    fn resolve_paint_style_or_color(
        &mut self,
        paint: &PaintStyleOrColor,
        force_dark_role: ForceDarkRole,
    ) -> Option<PaintStyleOrColor> {
        match paint {
            PaintStyleOrColor::Color(color) => {
                Some(PaintStyleOrColor::Color(self.resolve_color(*color, force_dark_role)))
            }
            PaintStyleOrColor::PaintStyle(style) => self
                .resolve_paint_style(style, force_dark_role)
                .map(PaintStyleOrColor::PaintStyle),
        }
    }

    /// A gradient paint style resolves stop by stop, like a gradient-painted rectangle; a pattern's colors live in
    /// its tile records, which were already filtered when they were recorded.
    fn resolve_paint_style(&mut self, style: &PaintStyle, force_dark_role: ForceDarkRole) -> Option<PaintStyle> {
        match style {
            PaintStyle::LinearGradient {
                gradient_transform,
                spread_method,
                color_space,
                color_stops,
                start_point,
                end_point,
            } => Some(PaintStyle::LinearGradient {
                gradient_transform: *gradient_transform,
                spread_method: *spread_method,
                color_space: *color_space,
                color_stops: self.resolve_color_stops(color_stops, force_dark_role)?,
                start_point: *start_point,
                end_point: *end_point,
            }),
            PaintStyle::RadialGradient {
                gradient_transform,
                spread_method,
                color_space,
                color_stops,
                start_center,
                start_radius,
                end_center,
                end_radius,
            } => Some(PaintStyle::RadialGradient {
                gradient_transform: *gradient_transform,
                spread_method: *spread_method,
                color_space: *color_space,
                color_stops: self.resolve_color_stops(color_stops, force_dark_role)?,
                start_center: *start_center,
                start_radius: *start_radius,
                end_center: *end_center,
                end_radius: *end_radius,
            }),
            PaintStyle::Pattern { .. } => None,
        }
    }

    fn resolve_color_stops(&mut self, stops: &ColorStops, force_dark_role: ForceDarkRole) -> Option<ColorStops> {
        if force_dark_role == ForceDarkRole::None {
            return None;
        }
        let resolver = self.force_dark.as_mut()?;
        Some(ColorStops {
            colors: resolver.resolve_each(&stops.colors, force_dark_role),
            positions: stops.positions.clone(),
            repeating: stops.repeating,
        })
    }

    pub fn builder(&self) -> &DisplayListBuilder {
        &self.builder
    }

    pub fn into_builder(self) -> DisplayListBuilder {
        debug_assert!(self.ambient_inline_clips.is_empty());
        self.builder
    }

    pub fn byte_size(&self) -> usize {
        self.builder.byte_size()
    }

    pub fn bytes(&self) -> &[u8] {
        self.builder.bytes()
    }

    pub fn accumulated_visual_context(&self) -> ContextRef {
        self.context
    }

    pub fn set_accumulated_visual_context(&mut self, context: ContextRef) {
        self.context = context;
    }

    fn append_command<C: DisplayListCommand>(&mut self, command: &C, inline_data: &[u8]) {
        self.builder.append_with_inline_state(
            command,
            inline_data,
            self.context,
            &self.ambient_inline_clips,
            self.ambient_inline_transform
                .filter(|transform| !transform.is_identity()),
        );
    }

    pub fn append_cached_command_range(
        &mut self,
        source: &RecordedDisplayList,
        range: CommandRange,
        recorded_context: ContextRef,
    ) -> CommandRange {
        let offset = self.builder.append_command_range(
            source,
            range,
            Some(ContextRewrite {
                recorded_context,
                current_context: self.context,
            }),
        );
        CommandRange {
            offset,
            size: range.size,
        }
    }

    /// Copies a cached command range without rewriting visual-context indices. The caller must
    /// establish that the recorded indices are still valid for the current visual context tree.
    pub fn append_cached_command_range_verbatim(
        &mut self,
        source: &RecordedDisplayList,
        range: CommandRange,
    ) -> CommandRange {
        let offset = self.builder.append_command_range(source, range, None);
        CommandRange {
            offset,
            size: range.size,
        }
    }

    pub fn fill_rect(&mut self, rect: IntRect, color: Color, force_dark_role: ForceDarkRole) {
        if color.alpha() == 0 {
            return;
        }
        self.fill_rect_with_compositing_and_blending_operator(
            rect,
            color,
            CompositingAndBlendingOperator::Normal,
            force_dark_role,
        );
    }

    pub fn fill_rect_with_compositing_and_blending_operator(
        &mut self,
        rect: IntRect,
        color: Color,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        force_dark_role: ForceDarkRole,
    ) {
        if rect.is_empty() {
            return;
        }
        let color = self.resolve_color(color, force_dark_role);
        self.append_command(
            &FillRect {
                rect,
                color,
                compositing_and_blending_operator,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
        );
    }

    pub fn paint_caret(&mut self, rect: IntRect, color: Color, blink_cycle_start_time_ns: i64, should_blink: bool) {
        if rect.is_empty() || color.alpha() == 0 {
            return;
        }
        // A caret is text, so it always takes the foreground rule — no caller context could make it anything else.
        let color = self.resolve_color(color, ForceDarkRole::Foreground);
        self.append_command(
            &PaintCaret {
                rect,
                color,
                blink_cycle_start_time_ns,
                should_blink,
            },
            &[],
        );
    }

    pub fn fill_rect_transparent(&mut self, rect: IntRect) {
        if rect.is_empty() {
            return;
        }
        self.append_command(
            &FillRect {
                rect,
                color: Color::TRANSPARENT,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
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
        self.fill_path_with_compositing_and_blending_operator(params, CompositingAndBlendingOperator::Normal);
    }

    pub fn fill_path_with_compositing_and_blending_operator(
        &mut self,
        params: FillPathParams<'_>,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
    ) {
        if compositing_and_blending_operator == CompositingAndBlendingOperator::Normal
            && let PaintStyleOrColor::Color(color) = &params.paint_style_or_color
            && color.alpha() == 0
        {
            return;
        }
        let path_bounding_rect = FloatRect::from_array(params.path.bounding_box());
        if path_bounding_rect.is_empty() {
            return;
        }
        let resolved = self.resolve_paint_style_or_color(&params.paint_style_or_color, params.force_dark_role);
        let resolved_paint = resolved.as_ref().unwrap_or(&params.paint_style_or_color);
        let mut payload = CommandPayloadBuilder::new::<FillPath>(&self.builder);
        let path_data = payload.append_data(&params.path.serialize_to_bytes(), std::mem::align_of::<u32>());
        let (paint_kind, color, paint_style) = Self::resolve_paint(&mut payload, resolved_paint);
        let command = FillPath {
            path_bounding_rect,
            path_data,
            opacity: params.opacity,
            paint_kind,
            color,
            paint_style,
            winding_rule: params.winding_rule,
            should_anti_alias: params.should_anti_alias,
            compositing_and_blending_operator,
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
        let resolved = self.resolve_paint_style_or_color(&params.paint_style_or_color, params.force_dark_role);
        let resolved_paint = resolved.as_ref().unwrap_or(&params.paint_style_or_color);
        let mut payload = CommandPayloadBuilder::new::<StrokePath>(&self.builder);
        let path_data = payload.append_data(&params.path.serialize_to_bytes(), std::mem::align_of::<u32>());
        let dash_array = payload.append_f32s(&params.dash_array);
        let (paint_kind, color, paint_style) = Self::resolve_paint(&mut payload, resolved_paint);
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

    pub fn draw_ellipse(&mut self, rect: IntRect, color: Color, thickness: i32, force_dark_role: ForceDarkRole) {
        if rect.is_empty() || color.alpha() == 0 || thickness == 0 {
            return;
        }
        let color = self.resolve_color(color, force_dark_role);
        self.append_command(&DrawEllipse { rect, color, thickness }, &[]);
    }

    pub fn fill_rect_with_linear_gradient(
        &mut self,
        gradient_rect: IntRect,
        data: &LinearGradientData,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        force_dark_role: ForceDarkRole,
    ) {
        if gradient_rect.is_empty() {
            return;
        }
        let resolved_stops = self.resolve_color_stops(&data.color_stops, force_dark_role);
        let stops = resolved_stops.as_ref().unwrap_or(&data.color_stops);
        let mut payload = CommandPayloadBuilder::new::<PaintLinearGradient>(&self.builder);
        let color_stops = stops.append_to(&mut payload);
        let command = PaintLinearGradient {
            gradient_rect,
            gradient_angle: data.gradient_angle,
            color_stops,
            first_stop_position: data.first_stop_position,
            repeat_length: data.repeat_length,
            interpolation_method: data.interpolation_method,
            compositing_and_blending_operator,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn fill_rect_with_conic_gradient(
        &mut self,
        rect: IntRect,
        data: &ConicGradientData,
        position: IntPoint,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        force_dark_role: ForceDarkRole,
    ) {
        if rect.is_empty() {
            return;
        }
        let resolved_stops = self.resolve_color_stops(&data.color_stops, force_dark_role);
        let stops = resolved_stops.as_ref().unwrap_or(&data.color_stops);
        let mut payload = CommandPayloadBuilder::new::<PaintConicGradient>(&self.builder);
        let color_stops = stops.append_to(&mut payload);
        let command = PaintConicGradient {
            rect,
            start_angle: data.start_angle,
            color_stops,
            interpolation_method: data.interpolation_method,
            position,
            compositing_and_blending_operator,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn fill_rect_with_radial_gradient(
        &mut self,
        rect: IntRect,
        data: &RadialGradientData,
        center: IntPoint,
        size: IntSize,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        force_dark_role: ForceDarkRole,
    ) {
        if rect.is_empty() {
            return;
        }
        let resolved_stops = self.resolve_color_stops(&data.color_stops, force_dark_role);
        let stops = resolved_stops.as_ref().unwrap_or(&data.color_stops);
        let mut payload = CommandPayloadBuilder::new::<PaintRadialGradient>(&self.builder);
        let color_stops = stops.append_to(&mut payload);
        let command = PaintRadialGradient {
            rect,
            color_stops,
            interpolation_method: data.interpolation_method,
            center,
            size,
            compositing_and_blending_operator,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn draw_rect(&mut self, rect: IntRect, color: Color, rough: bool, force_dark_role: ForceDarkRole) {
        if rect.is_empty() || color.alpha() == 0 {
            return;
        }
        let color = self.resolve_color(color, force_dark_role);
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

    #[allow(clippy::too_many_arguments)]
    pub fn draw_scaled_decoded_image_frame(
        &mut self,
        dst_rect: FloatRect,
        src_rect: Option<FloatRect>,
        frame_id: ImageFrameResourceId,
        scaling_mode: ScalingMode,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        isolated_backdrop_color: Option<Color>,
        force_dark_role: ForceDarkRole,
    ) {
        if dst_rect.is_empty() {
            return;
        }
        if src_rect.is_some_and(|src| src.is_empty()) {
            return;
        }
        // The lone-layer backdrop paints as this element's background, so it resolves like one; the player then
        // blends it as handed over and filters only the image.
        let isolated_backdrop_color =
            isolated_backdrop_color.map(|color| self.resolve_color(color, ForceDarkRole::Background));
        let apply_force_dark = self.force_dark_applies(force_dark_role);
        self.append_command(
            &DrawScaledDecodedImageFrame {
                dst_rect,
                src_rect: OptionalFloatRect::from(src_rect),
                frame_id,
                scaling_mode,
                compositing_and_blending_operator,
                isolated_backdrop_color: OptionalColor::from(isolated_backdrop_color),
                apply_force_dark,
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
        force_dark_role: ForceDarkRole,
    ) {
        if dst_rect.is_empty() || clip_rect.is_empty() {
            return;
        }
        // The lone-layer backdrop paints as this element's background, so it resolves like one; the player then
        // blends it as handed over and filters only the image.
        let isolated_backdrop_color =
            isolated_backdrop_color.map(|color| self.resolve_color(color, ForceDarkRole::Background));
        let apply_force_dark = self.force_dark_applies(force_dark_role);
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
                apply_force_dark,
            },
            &[],
        );
    }

    pub fn begin_isolated_group(&mut self) -> OpenRecorderGroup {
        self.begin_group::<DrawIsolatedGroup>()
    }

    pub fn begin_mask_content(&mut self) -> OpenRecorderGroup {
        self.begin_group::<DeclareMaskContent>()
    }

    pub fn finish_mask_content(&mut self, group: OpenRecorderGroup, effect: EffectNodeIndex, rect: IntRect) {
        debug_assert_eq!(self.context, group.context);
        let command = DeclareMaskContent {
            rect,
            effect,
            content: self.builder.group_content_span(&group.group),
        };
        self.builder.finish_group(group.group, &command, group.context);
        self.ambient_inline_clips = group.suspended_ambient_inline_clips;
    }

    pub fn begin_detached_records(&mut self) -> DetachedRecords {
        DetachedRecords {
            outer_builder: std::mem::take(&mut self.builder),
            outer_ambient_inline_clips: std::mem::take(&mut self.ambient_inline_clips),
            outer_ambient_inline_transform: self.ambient_inline_transform.take(),
        }
    }

    pub fn finish_detached_records(&mut self, detached: DetachedRecords) -> Vec<u8> {
        debug_assert!(self.ambient_inline_clips.is_empty());
        self.ambient_inline_transform = detached.outer_ambient_inline_transform;
        self.ambient_inline_clips = detached.outer_ambient_inline_clips;
        std::mem::replace(&mut self.builder, detached.outer_builder)
            .finish()
            .bytes
    }

    pub fn suspend_force_dark(&mut self) -> Option<ForceDarkResolver> {
        self.force_dark.take()
    }

    pub fn restore_force_dark(&mut self, resolver: Option<ForceDarkResolver>) {
        self.force_dark = resolver;
    }

    pub fn begin_repeated_tile(&mut self) -> OpenRecorderGroup {
        self.begin_group::<DrawRepeatedTile>()
    }

    fn begin_group<C: DisplayListCommand>(&mut self) -> OpenRecorderGroup {
        let suspended_ambient_inline_clips = std::mem::take(&mut self.ambient_inline_clips);
        OpenRecorderGroup {
            group: self.builder.begin_group::<C>(suspended_ambient_inline_clips.clone()),
            context: self.context,
            suspended_ambient_inline_clips,
        }
    }

    pub fn begin_group_mask(&mut self, group: &mut OpenRecorderGroup) {
        self.builder.begin_group_mask(&mut group.group);
    }

    pub fn is_recording_inside_group(&self) -> bool {
        self.builder.open_group_depth() > 0
    }

    pub fn finish_isolated_group(
        &mut self,
        group: OpenRecorderGroup,
        clip_rect: FloatRect,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        mask_kind: MaskKind,
    ) {
        self.finish_group_with_effects(
            group,
            IsolatedGroupEffects {
                clip_rect: Some(clip_rect),
                opacity: 1.0,
                filter: None,
                compositing_and_blending_operator,
                mask_kind,
            },
        );
    }

    pub fn finish_group_with_effects(&mut self, mut group: OpenRecorderGroup, effects: IsolatedGroupEffects) {
        debug_assert_eq!(self.context, group.context);
        let content = self.builder.group_content_span(&group.group);
        let mask = self.builder.group_mask_span(&group.group);
        let filter = match &effects.filter {
            Some(filter_bytes) => self.builder.append_group_inline_data(&mut group.group, filter_bytes),
            None => DisplayListDataSpan::default(),
        };
        let command = DrawIsolatedGroup {
            clip_rect: effects.clip_rect.into(),
            content,
            mask,
            filter,
            opacity: effects.opacity,
            compositing_and_blending_operator: effects.compositing_and_blending_operator,
            mask_kind: effects.mask_kind,
        };
        self.builder.finish_group(group.group, &command, group.context);
        self.ambient_inline_clips = group.suspended_ambient_inline_clips;
    }

    pub fn finish_repeated_tile(
        &mut self,
        group: OpenRecorderGroup,
        dst_rect: IntRect,
        clip_rect: IntRect,
        scaling_mode: ScalingMode,
        compositing_and_blending_operator: CompositingAndBlendingOperator,
        repeat: Repeat,
    ) {
        debug_assert_eq!(self.context, group.context);
        debug_assert!(!dst_rect.is_empty() && !clip_rect.is_empty());
        let command = DrawRepeatedTile {
            dst_rect,
            clip_rect,
            tile: self.builder.group_content_span(&group.group),
            scaling_mode,
            compositing_and_blending_operator,
            repeat,
        };
        self.builder
            .finish_group_clipped_to(group.group, &command, group.context, clip_rect);
        self.ambient_inline_clips = group.suspended_ambient_inline_clips;
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
        force_dark_role: ForceDarkRole,
    ) {
        if tile_rect.is_empty() || clip_rect.is_empty() || src_rect.is_empty() {
            return;
        }
        let apply_force_dark = self.force_dark_applies(force_dark_role);
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
                apply_force_dark,
            },
            &[],
        );
    }

    #[allow(clippy::too_many_arguments)]
    pub fn draw_line(
        &mut self,
        from: IntPoint,
        to: IntPoint,
        color: Color,
        thickness: i32,
        style: LineStyle,
        alternate_color: Color,
        force_dark_role: ForceDarkRole,
    ) {
        if color.alpha() == 0 || thickness == 0 {
            return;
        }
        let color = self.resolve_color(color, force_dark_role);
        let alternate_color = self.resolve_color(alternate_color, force_dark_role);
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
        force_dark_role: ForceDarkRole,
    ) {
        if color.alpha() == 0 {
            return;
        }
        let color = self.resolve_color(color, force_dark_role);
        let mut payload = CommandPayloadBuilder::new::<DrawGlyphRun>(&self.builder);
        let glyphs = payload.append_objects(run.glyphs);
        let command = DrawGlyphRun {
            font_smoothing: run.font_smoothing,
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

    pub fn backdrop_filter_region(&mut self, rect: IntRect) {
        if rect.is_empty() {
            return;
        }
        self.append_command(&BackdropFilterRegion { rect }, &[]);
    }

    pub fn paint_outer_box_shadow(&mut self, mut shadow: PaintOuterBoxShadow, force_dark_role: ForceDarkRole) {
        shadow.color = self.resolve_color(shadow.color, force_dark_role);
        self.append_command(&shadow, &[]);
    }

    pub fn paint_inner_box_shadow(&mut self, mut shadow: PaintInnerBoxShadow, force_dark_role: ForceDarkRole) {
        shadow.color = self.resolve_color(shadow.color, force_dark_role);
        self.append_command(&shadow, &[]);
    }

    #[allow(clippy::too_many_arguments)]
    pub fn paint_text_shadow(
        &mut self,
        blur_radius: i32,
        shadow_bounding_rect: IntRect,
        rect: IntRect,
        translation: FloatPoint,
        run: GlyphRunForRecording<'_>,
        glyph_run_scale: f64,
        color: Color,
        orientation: Orientation,
        force_dark_role: ForceDarkRole,
    ) {
        let color = self.resolve_color(color, force_dark_role);
        let mut payload = CommandPayloadBuilder::new::<PaintTextShadow>(&self.builder);
        let glyphs = payload.append_objects(run.glyphs);
        let command = PaintTextShadow {
            font_smoothing: run.font_smoothing,
            font_id: run.font_id,
            glyphs,
            shadow_bounding_rect,
            rect,
            translation,
            scale: glyph_run_scale as f32,
            blur_radius,
            color,
            orientation,
        };
        self.append_command(&command, payload.inline_data());
    }

    pub fn fill_rect_with_rounded_corners(
        &mut self,
        rect: IntRect,
        color: Color,
        corner_radii: CornerRadii,
        force_dark_role: ForceDarkRole,
    ) {
        if rect.is_empty() || color.alpha() == 0 {
            return;
        }
        if !corner_radii.has_any_radius() {
            self.fill_rect(rect, color, force_dark_role);
            return;
        }
        let color = self.resolve_color(color, force_dark_role);
        self.append_command(
            &FillRectWithRoundedCorners {
                rect,
                color,
                corner_radii,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
        );
    }

    pub fn fill_animated_background_color(
        &mut self,
        rect: IntRect,
        color: Color,
        corner_radii: CornerRadii,
        animation_effect: Option<EffectNodeIndex>,
        force_dark_role: ForceDarkRole,
    ) {
        if rect.is_empty() || (color.alpha() == 0 && animation_effect.is_none()) {
            return;
        }
        // NB: An effect means the player samples the animated color at play time, past this resolution. So while
        // force-dark applies, Document::update_compositor_animations() keeps background colors off the compositor.
        let animation_effect = animation_effect.unwrap_or(EffectNodeIndex::NONE);
        let color = self.resolve_color(color, force_dark_role);
        if !corner_radii.has_any_radius() {
            self.append_command(
                &FillRect {
                    rect,
                    color,
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    background_color_animation_effect: animation_effect,
                },
                &[],
            );
            return;
        }
        self.append_command(
            &FillRectWithRoundedCorners {
                rect,
                color,
                corner_radii,
                background_color_animation_effect: animation_effect,
            },
            &[],
        );
    }

    pub fn fill_rect_with_uniform_rounded_corners(
        &mut self,
        rect: IntRect,
        color: Color,
        radius: i32,
        force_dark_role: ForceDarkRole,
    ) {
        self.fill_rect_with_rounded_corners(rect, color, CornerRadii::uniform(radius), force_dark_role);
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
        force_dark_role: ForceDarkRole,
    ) {
        let thumb_color = self.resolve_color(thumb_color, force_dark_role);
        let track_color = self.resolve_color(track_color, force_dark_role);
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
