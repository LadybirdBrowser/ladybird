/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tape of a 2D canvas: the records that a recorder writes for the commands of the canvas, and the entry points
//! through which the compositor checks and reads a tape that it received.

use super::builder::{append_record, for_each_command};
use super::commands::*;
use super::ffi_bytes::FfiBytes;
use super::validate::validate_canvas_2d_tape;
use libgfx_rust::{
    AffineTransform, CapStyle, Color, CompositingAndBlendingOperator, FloatPoint, FloatRect, IntPoint, JoinStyle,
    ScalingMode, WindingRule,
};
use std::ffi::c_void;

/// The stops of a gradient, as parallel arrays of colors and positions.
#[derive(Clone, Copy, Debug, Default)]
pub struct ColorStops<'a> {
    pub colors: &'a [Color],
    pub positions: &'a [f32],
}

/// The fill or stroke style of a draw. A pattern names its image by the id it travels with beside the tape.
#[derive(Clone, Copy, Debug)]
pub enum Paint<'a> {
    Color(Color),
    LinearGradient {
        start_point: FloatPoint,
        end_point: FloatPoint,
        color_stops: ColorStops<'a>,
    },
    RadialGradient {
        start_center: FloatPoint,
        start_radius: f32,
        end_center: FloatPoint,
        end_radius: f32,
        color_stops: ColorStops<'a>,
    },
    ConicGradient {
        center: FloatPoint,
        start_angle: f32,
        color_stops: ColorStops<'a>,
    },
    Pattern {
        image: Option<ImageFrameResourceId>,
        repetition: Canvas2DPatternRepetition,
        transform: Option<AffineTransform>,
    },
}

/// How a draw composites: the serialized filter graph of the context if it has one, the global alpha, the
/// compositing and blending operator, and the shadow that the draw casts if it casts one.
#[derive(Clone, Copy, Debug)]
pub struct Compositing<'a> {
    pub filter: Option<&'a [u8]>,
    pub global_alpha: f32,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub shadow: Option<Canvas2DShadow>,
}

impl Default for Compositing<'_> {
    fn default() -> Self {
        Self {
            filter: None,
            global_alpha: 1.0,
            compositing_and_blending_operator: CompositingAndBlendingOperator::SourceOver,
            shadow: None,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Stroke<'a> {
    pub thickness: f32,
    pub cap_style: CapStyle,
    pub join_style: JoinStyle,
    pub miter_limit: f32,
    pub dash_array: &'a [f32],
    pub dash_offset: f32,
}

// The data a record carries after its command struct. Its spans are offsets into the payload, which starts with the
// struct, at a multiple of the command alignment.
struct InlineData {
    struct_size: usize,
    bytes: Vec<u8>,
}

impl InlineData {
    fn new<C: DisplayListCommand>() -> Self {
        Self {
            struct_size: std::mem::size_of::<C>(),
            bytes: Vec::new(),
        }
    }

    fn append(&mut self, data: &[u8], alignment: usize) -> DisplayListDataSpan {
        if data.is_empty() {
            return DisplayListDataSpan::default();
        }
        let offset = (self.struct_size + self.bytes.len()).next_multiple_of(alignment);
        self.bytes.resize(offset - self.struct_size, 0);
        self.bytes.extend_from_slice(data);
        DisplayListDataSpan {
            offset: u32::try_from(offset).expect("canvas 2D record exceeds u32"),
            size: u32::try_from(data.len()).expect("canvas 2D record exceeds u32"),
        }
    }

    fn append_objects<T: FfiBytes>(&mut self, objects: &[T]) -> DisplayListDataSpan {
        let size = std::mem::size_of::<T>();
        let mut bytes = vec![0u8; std::mem::size_of_val(objects)];
        for (object, out) in objects.iter().zip(bytes.chunks_exact_mut(size)) {
            object.write_ffi_bytes(out);
        }
        self.append(&bytes, std::mem::align_of::<T>())
    }

    fn append_paint(&mut self, paint: &Paint) -> Canvas2DPaint {
        let mut record = Canvas2DPaint::default();
        let append_color_stops = |inline_data: &mut Self, color_stops: &ColorStops| {
            debug_assert_eq!(color_stops.colors.len(), color_stops.positions.len());
            DisplayListGradientColorStops {
                colors: inline_data.append_objects(color_stops.colors),
                positions: inline_data.append_objects(color_stops.positions),
                repeating: false,
            }
        };
        match *paint {
            Paint::Color(color) => {
                record.kind = Canvas2DPaintKind::Color;
                record.color = color;
            }
            Paint::LinearGradient {
                start_point,
                end_point,
                color_stops,
            } => {
                record.kind = Canvas2DPaintKind::LinearGradient;
                record.start_point = start_point;
                record.end_point = end_point;
                record.color_stops = append_color_stops(self, &color_stops);
            }
            Paint::RadialGradient {
                start_center,
                start_radius,
                end_center,
                end_radius,
                color_stops,
            } => {
                record.kind = Canvas2DPaintKind::RadialGradient;
                record.start_point = start_center;
                record.start_radius = start_radius;
                record.end_point = end_center;
                record.end_radius = end_radius;
                record.color_stops = append_color_stops(self, &color_stops);
            }
            Paint::ConicGradient {
                center,
                start_angle,
                color_stops,
            } => {
                record.kind = Canvas2DPaintKind::ConicGradient;
                record.start_point = center;
                record.start_angle = start_angle;
                record.color_stops = append_color_stops(self, &color_stops);
            }
            Paint::Pattern {
                image,
                repetition,
                transform,
            } => {
                record.kind = Canvas2DPaintKind::Pattern;
                record.pattern_has_image = image.is_some();
                record.pattern_image_id = image.unwrap_or_default();
                record.pattern_repetition = repetition;
                record.pattern_transform = transform.into();
            }
        }
        record
    }

    fn append_compositing(&mut self, compositing: &Compositing) -> Canvas2DCompositing {
        Canvas2DCompositing {
            filter: compositing
                .filter
                .map_or_else(DisplayListDataSpan::default, |filter| self.append(filter, 1)),
            has_filter: compositing.filter.is_some(),
            global_alpha: compositing.global_alpha,
            compositing_and_blending_operator: compositing.compositing_and_blending_operator,
            has_shadow: compositing.shadow.is_some(),
            shadow: compositing.shadow.unwrap_or_default(),
        }
    }

    fn append_path(&mut self, path: &[u8]) -> DisplayListDataSpan {
        self.append(path, std::mem::align_of::<u32>())
    }
}

/// Writes the records of the commands of a 2D canvas to a tape, which a recorder hands on and clears.
#[derive(Default)]
pub struct TapeWriter {
    bytes: Vec<u8>,
}

impl TapeWriter {
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn clear(&mut self) {
        self.bytes.clear();
    }

    fn append<C: DisplayListCommand>(&mut self, command: &C, inline_data: &InlineData) {
        append_record(&mut self.bytes, command, &inline_data.bytes);
    }

    fn append_plain<C: DisplayListCommand>(&mut self, command: &C) {
        append_record(&mut self.bytes, command, &[]);
    }

    /// `path` is a serialized Gfx::Path.
    pub fn fill_path(&mut self, path: &[u8], paint: &Paint, winding_rule: WindingRule, compositing: &Compositing) {
        let mut inline_data = InlineData::new::<Canvas2DFillPath>();
        let command = Canvas2DFillPath {
            path_data: inline_data.append_path(path),
            paint: inline_data.append_paint(paint),
            winding_rule,
            compositing: inline_data.append_compositing(compositing),
        };
        self.append(&command, &inline_data);
    }

    /// `path` is a serialized Gfx::Path.
    pub fn stroke_path(&mut self, path: &[u8], paint: &Paint, stroke: &Stroke, compositing: &Compositing) {
        let mut inline_data = InlineData::new::<Canvas2DStrokePath>();
        let command = Canvas2DStrokePath {
            path_data: inline_data.append_path(path),
            paint: inline_data.append_paint(paint),
            thickness: stroke.thickness,
            cap_style: stroke.cap_style,
            join_style: stroke.join_style,
            miter_limit: stroke.miter_limit,
            dash_array: inline_data.append_objects(stroke.dash_array),
            dash_offset: stroke.dash_offset,
            compositing: inline_data.append_compositing(compositing),
        };
        self.append(&command, &inline_data);
    }

    pub fn draw_glyph_run(
        &mut self,
        font_id: FontResourceId,
        glyphs: &[DisplayListGlyph],
        translation: FloatPoint,
        paint: &Paint,
        compositing: &Compositing,
    ) {
        let mut inline_data = InlineData::new::<Canvas2DDrawGlyphRun>();
        let command = Canvas2DDrawGlyphRun {
            font_id,
            glyphs: inline_data.append_objects(glyphs),
            translation,
            paint: inline_data.append_paint(paint),
            compositing: inline_data.append_compositing(compositing),
        };
        self.append(&command, &inline_data);
    }

    pub fn clear_rect(&mut self, rect: FloatRect, color: Color) {
        self.append_plain(&Canvas2DClearRect { rect, color });
    }

    pub fn draw_image_frame(
        &mut self,
        frame_id: ImageFrameResourceId,
        dst_rect: FloatRect,
        src_rect: FloatRect,
        scaling_mode: ScalingMode,
        compositing: &Compositing,
    ) {
        let mut inline_data = InlineData::new::<Canvas2DDrawImageFrame>();
        let command = Canvas2DDrawImageFrame {
            frame_id,
            dst_rect,
            src_rect,
            scaling_mode,
            compositing: inline_data.append_compositing(compositing),
        };
        self.append(&command, &inline_data);
    }

    pub fn draw_canvas(
        &mut self,
        source_canvas_id: CanvasId,
        dst_rect: FloatRect,
        src_rect: FloatRect,
        scaling_mode: ScalingMode,
        compositing: &Compositing,
    ) {
        let mut inline_data = InlineData::new::<Canvas2DDrawCanvas>();
        let command = Canvas2DDrawCanvas {
            source_canvas_id,
            dst_rect,
            src_rect,
            scaling_mode,
            compositing: inline_data.append_compositing(compositing),
        };
        self.append(&command, &inline_data);
    }

    pub fn draw_video_frame(
        &mut self,
        video_sink_handle: u64,
        generation: u64,
        dst_rect: FloatRect,
        src_rect: FloatRect,
        scaling_mode: ScalingMode,
        compositing: &Compositing,
    ) {
        let mut inline_data = InlineData::new::<Canvas2DDrawVideoFrame>();
        let command = Canvas2DDrawVideoFrame {
            video_sink_handle,
            generation,
            dst_rect,
            src_rect,
            scaling_mode,
            compositing: inline_data.append_compositing(compositing),
        };
        self.append(&command, &inline_data);
    }

    pub fn set_transform(&mut self, transform: AffineTransform) {
        self.append_plain(&Canvas2DSetTransform { transform });
    }

    pub fn save(&mut self) {
        self.append_plain(&Canvas2DSave::default());
    }

    pub fn restore(&mut self) {
        self.append_plain(&Canvas2DRestore::default());
    }

    /// `path` is a serialized Gfx::Path.
    pub fn clip_path(&mut self, path: &[u8], winding_rule: WindingRule) {
        let mut inline_data = InlineData::new::<Canvas2DClipPath>();
        let command = Canvas2DClipPath {
            path_data: inline_data.append_path(path),
            winding_rule,
        };
        self.append(&command, &inline_data);
    }

    pub fn reset(&mut self) {
        self.append_plain(&Canvas2DReset::default());
    }

    pub fn clear_canvas(&mut self, color: Color) {
        self.append_plain(&Canvas2DClearCanvas { color });
    }

    pub fn put_image_frame(&mut self, frame_id: ImageFrameResourceId, position: IntPoint) {
        self.append_plain(&Canvas2DPutImageFrame { frame_id, position });
    }
}

/// Checks the tape of a segment of the canvas 2D stream that came from another process. Returns false with a static
/// error message, whose address and size it writes, if the tape does not pass.
#[unsafe(no_mangle)]
pub extern "C" fn canvas_2d_tape_validate(
    tape: ak::Span<'_, u8>,
    error: &mut *const u8,
    error_size: &mut usize,
) -> bool {
    match validate_canvas_2d_tape(tape.as_slice()) {
        Ok(()) => true,
        Err(message) => {
            *error = message.as_ptr();
            *error_size = message.len();
            false
        }
    }
}

/// Calls `visit` with the type and the payload of each record of a tape that passed `canvas_2d_tape_validate`, in
/// order.
///
/// # Safety
/// `visit` runs synchronously and reads the payload only while it runs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn canvas_2d_tape_for_each_command(
    tape: ak::Span<'_, u8>,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, DisplayListCommandType, ak::Span<'_, u8>),
) {
    for_each_command(tape.as_slice(), |header, _, payload| {
        debug_assert!(header.command_type.is_canvas_2d());
        // SAFETY: The caller reads the payload synchronously.
        unsafe { visit(context, header.command_type, ak::Span::new(payload)) };
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::{HEADER_SIZE, read_command, read_header};
    use crate::display_list::nested_records::span_bytes;
    use crate::display_list::storage::write_run;
    use crate::display_list::validate::validate_tape;
    use libgfx_rust::IntRect;

    fn records(tape: &[u8]) -> Vec<(DisplayListCommandType, Vec<u8>)> {
        let mut records = Vec::new();
        for_each_command(tape, |header, _, payload| {
            records.push((header.command_type, payload.to_vec()));
        });
        records
    }

    fn linear_gradient<'a>(colors: &'a [Color], positions: &'a [f32]) -> Paint<'a> {
        Paint::LinearGradient {
            start_point: FloatPoint { x: 1.0, y: 2.0 },
            end_point: FloatPoint { x: 3.0, y: 4.0 },
            color_stops: ColorStops { colors, positions },
        }
    }

    #[test]
    fn a_fill_carries_its_path_its_paint_and_its_filter_inline() {
        let colors = [Color(0xff00_00ff), Color(0xffff_0000)];
        let positions = [0.0, 1.0];
        let path = [1u8, 2, 3, 4, 5];
        let filter = [9u8, 8, 7];
        let mut writer = TapeWriter::default();
        writer.fill_path(
            &path,
            &linear_gradient(&colors, &positions),
            WindingRule::EvenOdd,
            &Compositing {
                filter: Some(&filter),
                global_alpha: 0.5,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Multiply,
                shadow: None,
            },
        );
        assert_eq!(validate_canvas_2d_tape(writer.bytes()), Ok(()));
        let header = read_header(writer.bytes());
        assert!(!header.has_bounding_rect);
        assert_eq!(header.inline_clip_count, 0);
        let records = records(writer.bytes());
        assert_eq!(records.len(), 1);
        let (command_type, payload) = &records[0];
        assert_eq!(*command_type, DisplayListCommandType::Canvas2DFillPath);
        let command = read_command::<Canvas2DFillPath>(payload);
        assert_eq!(span_bytes(payload, command.path_data), path);
        assert_eq!(command.paint.kind, Canvas2DPaintKind::LinearGradient);
        assert_eq!(command.paint.end_point, FloatPoint { x: 3.0, y: 4.0 });
        let stop_colors: Vec<u32> = span_bytes(payload, command.paint.color_stops.colors)
            .as_chunks::<4>()
            .0
            .iter()
            .map(|bytes| u32::from_ne_bytes(*bytes))
            .collect();
        assert_eq!(stop_colors, [0xff00_00ff, 0xffff_0000]);
        assert_eq!(command.paint.color_stops.positions.size, 8);
        assert_eq!(command.paint.color_stops.positions.offset % 4, 0);
        assert_eq!(command.winding_rule, WindingRule::EvenOdd);
        assert!(command.compositing.has_filter);
        assert_eq!(span_bytes(payload, command.compositing.filter), filter);
        assert_eq!(command.compositing.global_alpha, 0.5);
        assert_eq!(
            command.compositing.compositing_and_blending_operator,
            CompositingAndBlendingOperator::Multiply
        );
    }

    #[test]
    fn records_follow_each_other_in_recording_order() {
        let mut writer = TapeWriter::default();
        writer.save();
        writer.set_transform(AffineTransform {
            values: [2.0, 0.0, 0.0, 2.0, 5.0, 6.0],
        });
        writer.clear_rect(FloatRect::new(0.0, 0.0, 10.0, 10.0), Color(0));
        writer.clip_path(&[4, 4, 4, 4], WindingRule::Nonzero);
        writer.draw_glyph_run(
            FontResourceId(7),
            &[DisplayListGlyph {
                position: FloatPoint { x: 1.0, y: 0.0 },
                glyph_id: 42,
            }],
            FloatPoint { x: 10.0, y: 20.0 },
            &Paint::Color(Color(0xff00_ff00)),
            &Compositing::default(),
        );
        writer.restore();
        writer.reset();
        writer.clear_canvas(Color(0xff00_0000));
        assert_eq!(validate_canvas_2d_tape(writer.bytes()), Ok(()));
        let types: Vec<_> = records(writer.bytes())
            .into_iter()
            .map(|(command_type, _)| command_type)
            .collect();
        assert_eq!(
            types,
            [
                DisplayListCommandType::Canvas2DSave,
                DisplayListCommandType::Canvas2DSetTransform,
                DisplayListCommandType::Canvas2DClearRect,
                DisplayListCommandType::Canvas2DClipPath,
                DisplayListCommandType::Canvas2DDrawGlyphRun,
                DisplayListCommandType::Canvas2DRestore,
                DisplayListCommandType::Canvas2DReset,
                DisplayListCommandType::Canvas2DClearCanvas,
            ]
        );
        assert_eq!(writer.bytes().len() % 8, 0);
    }

    #[test]
    fn a_draw_without_a_filter_carries_no_filter_bytes() {
        let mut writer = TapeWriter::default();
        writer.draw_image_frame(
            ImageFrameResourceId(3),
            FloatRect::new(0.0, 0.0, 4.0, 4.0),
            FloatRect::new(0.0, 0.0, 2.0, 2.0),
            ScalingMode::NearestNeighbor,
            &Compositing::default(),
        );
        let records = records(writer.bytes());
        let command = read_command::<Canvas2DDrawImageFrame>(&records[0].1);
        assert!(!command.compositing.has_filter);
        assert!(command.compositing.filter.is_empty());
        assert_eq!(command.frame_id, ImageFrameResourceId(3));
        assert_eq!(
            records[0].1.len(),
            size_of::<Canvas2DDrawImageFrame>().next_multiple_of(8)
        );
    }

    #[test]
    fn a_pattern_without_an_image_says_so() {
        let mut writer = TapeWriter::default();
        writer.stroke_path(
            &[1],
            &Paint::Pattern {
                image: None,
                repetition: Canvas2DPatternRepetition::RepeatY,
                transform: Some(AffineTransform {
                    values: [1.0, 0.0, 0.0, 1.0, 3.0, 4.0],
                }),
            },
            &Stroke {
                thickness: 2.0,
                cap_style: CapStyle::Round,
                join_style: JoinStyle::Bevel,
                miter_limit: 10.0,
                dash_array: &[1.0, 2.0],
                dash_offset: 0.5,
            },
            &Compositing::default(),
        );
        assert_eq!(validate_canvas_2d_tape(writer.bytes()), Ok(()));
        let records = records(writer.bytes());
        let command = read_command::<Canvas2DStrokePath>(&records[0].1);
        assert!(!command.paint.pattern_has_image);
        assert_eq!(command.paint.pattern_repetition, Canvas2DPatternRepetition::RepeatY);
        assert_eq!(command.paint.pattern_transform.get().unwrap().values[4], 3.0);
        assert_eq!(command.dash_array.size, 8);
    }

    #[test]
    fn a_tape_holds_only_the_commands_of_its_kind() {
        let mut writer = TapeWriter::default();
        writer.save();
        let mut run_bytes = vec![0u8; size_of::<DisplayListCommandRun>()];
        write_run(
            &DisplayListCommandRun {
                offset: 0,
                size: u32::try_from(writer.bytes().len()).unwrap(),
                ..Default::default()
            },
            &mut run_bytes,
        );
        assert_eq!(
            validate_tape(writer.bytes(), &run_bytes),
            Err("Display list holds a command of the canvas 2D stream")
        );
        let mut page_records = Vec::new();
        append_record(
            &mut page_records,
            &FillRect {
                rect: IntRect::new(0, 0, 1, 1),
                color: Color(0),
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
        );
        assert_eq!(
            validate_canvas_2d_tape(&page_records),
            Err("Canvas 2D stream holds a command of a display list")
        );
    }

    #[test]
    fn a_global_alpha_outside_0_to_1_is_rejected() {
        for (global_alpha, valid) in [
            (0.0, true),
            (1.0, true),
            (-0.5, false),
            (1.5, false),
            (f32::NAN, false),
            (f32::INFINITY, false),
        ] {
            let mut writer = TapeWriter::default();
            writer.fill_path(
                &[1],
                &Paint::Color(Color(0xff00_0000)),
                WindingRule::Nonzero,
                &Compositing {
                    global_alpha,
                    ..Compositing::default()
                },
            );
            let expected = if valid {
                Ok(())
            } else {
                Err("Canvas 2D stream command has a global alpha outside 0 to 1")
            };
            assert_eq!(validate_canvas_2d_tape(writer.bytes()), expected);
        }
    }

    #[test]
    fn a_command_with_a_bounding_rect_is_rejected() {
        let mut writer = TapeWriter::default();
        writer.save();
        writer.clear_rect(FloatRect::new(0.0, 0.0, 1.0, 1.0), Color(0));
        let mut tape = writer.bytes().to_vec();
        let second_record = read_header(&tape).payload_size as usize + HEADER_SIZE;
        tape[second_record + std::mem::offset_of!(DisplayListCommandHeader, has_bounding_rect)] = 1;
        assert_eq!(
            validate_canvas_2d_tape(&tape),
            Err("Canvas 2D stream command has a bounding rect")
        );
    }

    #[test]
    fn a_span_outside_its_payload_is_rejected() {
        let mut writer = TapeWriter::default();
        writer.clip_path(&[1, 2, 3, 4], WindingRule::Nonzero);
        let mut tape = writer.bytes().to_vec();
        let size_offset = HEADER_SIZE
            + std::mem::offset_of!(Canvas2DClipPath, path_data)
            + std::mem::offset_of!(DisplayListDataSpan, size);
        tape[size_offset..size_offset + 4].copy_from_slice(&1000u32.to_ne_bytes());
        assert_eq!(
            validate_canvas_2d_tape(&tape),
            Err("Display list span exceeds its command payload")
        );
        let mut tape = writer.bytes().to_vec();
        tape[HEADER_SIZE + std::mem::offset_of!(Canvas2DClipPath, winding_rule)] = 7;
        assert_eq!(
            validate_canvas_2d_tape(&tape),
            Err("Display list command has an invalid enum value")
        );
    }
}
