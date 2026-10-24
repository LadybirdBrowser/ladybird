/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The recorder of a 2D canvas, which writes a record for each command of the canvas and hands it, with the fonts, the
//! image frames and the video frames that it names, to the stream that WebContent shares between its canvases.

use super::canvas_2d::{Compositing, Paint, Stroke, TapeWriter};
use super::commands::*;
use libgfx_rust::{AffineTransform, Color, FloatPoint, FloatRect, IntRect, IntSize, ScalingMode, WindingRule};

/// The number of commands that the shared stream carries to the Compositor at most in one batch. A batch holds the
/// image frames of its commands, each a bitmap that an IPC attachment carries, so it has to stay small.
pub const MAX_PENDING_COMMANDS: usize = 64;

// The values of Gfx::AlphaType.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum AlphaType {
    Premultiplied,
    Unpremultiplied,
}

/// The context of a canvas in the Compositor, and the stream that the commands of every canvas of WebContent share.
/// In the product that is a RemoteCanvas2DTransport of LibWeb.
pub trait Transport {
    /// A Gfx::Font, lent to the call that takes it.
    type Font: ?Sized;
    /// A Gfx::DecodedImageFrame, lent to the call that takes it.
    type ImageFrame: ?Sized;
    /// A Compositing::VideoFrameHold, lent to the call that takes it.
    type VideoFrameHold: ?Sized;
    type Bitmap;

    fn create_context(&self, size: IntSize, alpha: bool, will_read_frequently: bool) -> bool;
    fn canvas_id(&self) -> Option<CanvasId>;
    fn destroy_context(&self);

    /// Appends one record for the canvas of the context to the shared stream.
    fn append_command(&self, record: &[u8]);
    fn add_font(&self, font: &Self::Font) -> FontResourceId;
    fn add_image_frame(&self, frame: &Self::ImageFrame) -> ImageFrameResourceId;
    /// Keeps the held video frame until the Compositor has drawn the records that name it.
    fn retain_video_frame_hold(&self, hold: &Self::VideoFrameHold);
    fn record_present(&self);
    /// The number of records that the shared stream holds for every canvas.
    fn pending_command_count(&self) -> usize;
    fn flush(&self);

    /// Unpremultiplied pixels come back as RGBA8888, premultiplied pixels as BGRA8888.
    fn read_back_pixels(&self, rect: IntRect, alpha_type: AlphaType) -> Option<Self::Bitmap>;
}

pub struct Recorder<T: Transport> {
    transport: T,
    tape: TapeWriter,
}

impl<T: Transport> Recorder<T> {
    pub fn new(transport: T) -> Self {
        Self {
            transport,
            tape: TapeWriter::default(),
        }
    }

    pub fn create_context(&self, size: IntSize, alpha: bool, will_read_frequently: bool) -> bool {
        self.transport.create_context(size, alpha, will_read_frequently)
    }

    pub fn canvas_id(&self) -> Option<CanvasId> {
        self.transport.canvas_id()
    }

    pub fn destroy_context(&self) {
        self.transport.destroy_context();
    }

    pub fn flush(&self) {
        self.transport.flush();
    }

    pub fn read_back_pixels(&self, rect: IntRect, alpha_type: AlphaType) -> Option<T::Bitmap> {
        self.transport.read_back_pixels(rect, alpha_type)
    }

    /// Makes what the canvas has drawn so far the content that the page shows.
    pub fn present(&self) {
        self.transport.record_present();
    }

    /// Sends the shared stream once it holds a batch. A caller does this before it adds the resources of what it
    /// records, so that they travel in the batch of the records that name them.
    pub fn prepare_to_record(&self) {
        if self.transport.pending_command_count() >= MAX_PENDING_COMMANDS {
            self.transport.flush();
        }
    }

    /// Adds the image of a pattern to the stream, for a paint to name.
    pub fn add_image_frame(&self, frame: &T::ImageFrame) -> ImageFrameResourceId {
        self.transport.add_image_frame(frame)
    }

    fn record(&mut self, write: impl FnOnce(&mut TapeWriter)) {
        self.tape.clear();
        write(&mut self.tape);
        self.transport.append_command(self.tape.bytes());
    }

    /// `path` is a serialized Gfx::Path.
    pub fn fill_path(
        &mut self,
        path: &[u8],
        paint: &Paint,
        winding_rule: WindingRule,
        blur_radius: f32,
        compositing: &Compositing,
    ) {
        self.record(|tape| tape.fill_path(path, paint, winding_rule, blur_radius, compositing));
    }

    /// `path` is a serialized Gfx::Path.
    pub fn stroke_path(
        &mut self,
        path: &[u8],
        paint: &Paint,
        stroke: &Stroke,
        blur_radius: f32,
        compositing: &Compositing,
    ) {
        self.record(|tape| tape.stroke_path(path, paint, stroke, blur_radius, compositing));
    }

    pub fn draw_glyph_run(
        &mut self,
        font: &T::Font,
        glyphs: &[DisplayListGlyph],
        translation: FloatPoint,
        paint: &Paint,
        compositing: &Compositing,
    ) {
        let font_id = self.transport.add_font(font);
        self.record(|tape| tape.draw_glyph_run(font_id, glyphs, translation, paint, compositing));
    }

    pub fn clear_rect(&mut self, rect: FloatRect, color: Color) {
        self.record(|tape| tape.clear_rect(rect, color));
    }

    pub fn draw_image_frame(
        &mut self,
        frame: &T::ImageFrame,
        dst_rect: FloatRect,
        src_rect: IntRect,
        scaling_mode: ScalingMode,
        compositing: &Compositing,
    ) {
        let frame_id = self.transport.add_image_frame(frame);
        self.record(|tape| tape.draw_image_frame(frame_id, dst_rect, src_rect, scaling_mode, compositing));
    }

    pub fn draw_canvas(
        &mut self,
        source_canvas_id: CanvasId,
        dst_rect: FloatRect,
        src_rect: IntRect,
        scaling_mode: ScalingMode,
        compositing: &Compositing,
    ) {
        self.record(|tape| tape.draw_canvas(source_canvas_id, dst_rect, src_rect, scaling_mode, compositing));
    }

    /// Draws the frame that `hold` keeps, of the video sink with the handle, at the generation.
    #[expect(
        clippy::too_many_arguments,
        reason = "the arguments of drawImage() for a video frame"
    )]
    pub fn draw_video_frame(
        &mut self,
        hold: &T::VideoFrameHold,
        video_sink_handle: u64,
        generation: u64,
        dst_rect: FloatRect,
        src_rect: IntRect,
        scaling_mode: ScalingMode,
        compositing: &Compositing,
    ) {
        self.record(|tape| {
            tape.draw_video_frame(
                video_sink_handle,
                generation,
                dst_rect,
                src_rect,
                scaling_mode,
                compositing,
            );
        });
        self.transport.retain_video_frame_hold(hold);
    }

    pub fn set_transform(&mut self, transform: AffineTransform) {
        self.record(|tape| tape.set_transform(transform));
    }

    pub fn save(&mut self) {
        self.record(TapeWriter::save);
    }

    pub fn restore(&mut self) {
        self.record(TapeWriter::restore);
    }

    /// `path` is a serialized Gfx::Path.
    pub fn clip_path(&mut self, path: &[u8], winding_rule: WindingRule) {
        self.record(|tape| tape.clip_path(path, winding_rule));
    }

    pub fn reset(&mut self) {
        self.record(TapeWriter::reset);
    }

    pub fn clear_canvas(&mut self, color: Color) {
        self.record(|tape| tape.clear_canvas(color));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::{for_each_command, read_command};
    use crate::display_list::validate::validate_canvas_2d_tape;
    use std::cell::{Cell, RefCell};

    #[derive(Debug, PartialEq)]
    enum Event {
        Record(DisplayListCommandType),
        AddFont(u64),
        AddImageFrame(u64),
        RetainVideoFrameHold(u32),
        Present,
        Flush,
    }

    // Plays the stream: a font and an image frame are their ids, a video frame hold is a number, and a bitmap is the
    // rect it was read back from.
    #[derive(Default)]
    struct FakeTransport {
        events: RefCell<Vec<Event>>,
        records: RefCell<Vec<Vec<u8>>>,
        pending_command_count: Cell<usize>,
    }

    impl Transport for FakeTransport {
        type Font = u64;
        type ImageFrame = u64;
        type VideoFrameHold = u32;
        type Bitmap = IntRect;

        fn create_context(&self, size: IntSize, _alpha: bool, _will_read_frequently: bool) -> bool {
            size.width > 0 && size.height > 0
        }

        fn canvas_id(&self) -> Option<CanvasId> {
            Some(CanvasId(5))
        }

        fn destroy_context(&self) {}

        fn append_command(&self, record: &[u8]) {
            assert_eq!(validate_canvas_2d_tape(record), Ok(()));
            let mut command_types = Vec::new();
            for_each_command(record, |header, _, _| command_types.push(header.command_type));
            assert_eq!(command_types.len(), 1, "a record is one command");
            self.events.borrow_mut().push(Event::Record(command_types[0]));
            self.records.borrow_mut().push(record.to_vec());
            self.pending_command_count.set(self.pending_command_count.get() + 1);
        }

        fn add_font(&self, font: &u64) -> FontResourceId {
            self.events.borrow_mut().push(Event::AddFont(*font));
            FontResourceId(*font)
        }

        fn add_image_frame(&self, frame: &u64) -> ImageFrameResourceId {
            self.events.borrow_mut().push(Event::AddImageFrame(*frame));
            ImageFrameResourceId(*frame)
        }

        fn retain_video_frame_hold(&self, hold: &u32) {
            self.events.borrow_mut().push(Event::RetainVideoFrameHold(*hold));
        }

        fn record_present(&self) {
            self.events.borrow_mut().push(Event::Present);
        }

        fn pending_command_count(&self) -> usize {
            self.pending_command_count.get()
        }

        fn flush(&self) {
            self.events.borrow_mut().push(Event::Flush);
            self.pending_command_count.set(0);
        }

        fn read_back_pixels(&self, rect: IntRect, _alpha_type: AlphaType) -> Option<IntRect> {
            Some(rect)
        }
    }

    fn payload_of(record: &[u8]) -> Vec<u8> {
        let mut payload = Vec::new();
        for_each_command(record, |_, _, bytes| payload = bytes.to_vec());
        payload
    }

    #[test]
    fn a_glyph_run_adds_its_font_before_its_record_names_it() {
        let mut recorder = Recorder::new(FakeTransport::default());
        recorder.draw_glyph_run(
            &77,
            &[DisplayListGlyph {
                position: FloatPoint { x: 0.0, y: 0.0 },
                glyph_id: 3,
            }],
            FloatPoint { x: 1.0, y: 2.0 },
            &Paint::Color(Color(0xff00_0000)),
            &Compositing::default(),
        );
        assert_eq!(
            *recorder.transport.events.borrow(),
            [
                Event::AddFont(77),
                Event::Record(DisplayListCommandType::Canvas2DDrawGlyphRun)
            ]
        );
        let command = read_command::<Canvas2DDrawGlyphRun>(&payload_of(&recorder.transport.records.borrow()[0]));
        assert_eq!(command.font_id, FontResourceId(77));
        assert_eq!(command.translation, FloatPoint { x: 1.0, y: 2.0 });
    }

    #[test]
    fn an_image_and_a_video_frame_travel_beside_the_records_that_draw_them() {
        let mut recorder = Recorder::new(FakeTransport::default());
        let dst_rect = FloatRect::new(0.0, 0.0, 8.0, 8.0);
        let src_rect = IntRect::new(0, 0, 4, 4);
        recorder.draw_image_frame(
            &9,
            dst_rect,
            src_rect,
            ScalingMode::BilinearMipmap,
            &Compositing::default(),
        );
        recorder.draw_video_frame(
            &4,
            11,
            12,
            dst_rect,
            src_rect,
            ScalingMode::NearestNeighbor,
            &Compositing::default(),
        );
        assert_eq!(
            *recorder.transport.events.borrow(),
            [
                Event::AddImageFrame(9),
                Event::Record(DisplayListCommandType::Canvas2DDrawImageFrame),
                Event::Record(DisplayListCommandType::Canvas2DDrawVideoFrame),
                Event::RetainVideoFrameHold(4),
            ]
        );
        let records = recorder.transport.records.borrow();
        let image = read_command::<Canvas2DDrawImageFrame>(&payload_of(&records[0]));
        assert_eq!(image.frame_id, ImageFrameResourceId(9));
        assert_eq!(image.scaling_mode, ScalingMode::BilinearMipmap);
        let video = read_command::<Canvas2DDrawVideoFrame>(&payload_of(&records[1]));
        assert_eq!((video.video_sink_handle, video.generation), (11, 12));
    }

    #[test]
    fn the_stream_is_sent_once_it_holds_a_batch() {
        let mut recorder = Recorder::new(FakeTransport::default());
        for _ in 0..MAX_PENDING_COMMANDS - 1 {
            recorder.prepare_to_record();
            recorder.save();
        }
        recorder.prepare_to_record();
        assert!(!recorder.transport.events.borrow().contains(&Event::Flush));
        recorder.restore();
        recorder.prepare_to_record();
        assert_eq!(recorder.transport.events.borrow().last(), Some(&Event::Flush));
        assert_eq!(recorder.transport.pending_command_count(), 0);
    }

    #[test]
    fn the_calls_of_the_context_reach_the_transport() {
        let recorder = Recorder::new(FakeTransport::default());
        assert!(recorder.create_context(IntSize { width: 2, height: 2 }, true, false));
        assert_eq!(recorder.canvas_id(), Some(CanvasId(5)));
        recorder.present();
        recorder.flush();
        let rect = IntRect::new(1, 1, 1, 1);
        assert_eq!(recorder.read_back_pixels(rect, AlphaType::Premultiplied), Some(rect));
        recorder.destroy_context();
        assert_eq!(*recorder.transport.events.borrow(), [Event::Present, Event::Flush]);
    }
}
