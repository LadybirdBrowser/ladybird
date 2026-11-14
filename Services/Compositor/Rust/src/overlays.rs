/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the compositor draws over the page of a context: the scrollbars it paints itself, and the overlays the browser
//! asks for. They are recorded as display list records and played through the same painter as the page.

use crate::frame_inputs::ScrollbarOfFrame;
use libcompositing_rust::display_list::builder::{DisplayListBuilder, HEADER_SIZE, PendingInlineClip};
use libcompositing_rust::display_list::commands::{
    ContextRef, DisplayListCommand, DisplayListDataSpan, DisplayListGlyph, DisplayListPaintStyle, DrawGlyphRun,
    FillPath, FillRect, FontResourceId, OptionalFloatRect, PathPaintKind, ReplayLayer, StrokePath,
};
use libcompositing_rust::display_list::ffi_bytes::FfiBytes;
use libcompositing_rust::display_list::replay::{
    ListReplay, NoNestedDisplayLists, RecordsContext, ReplayFrame, ReplayPainter, replay_records,
};
use libcompositing_rust::host::PresentedScrollbar;
use libcompositing_rust::overlays::{
    PausedDebuggerOverlayGeometry, paused_debugger_overlay_geometry, picture_in_picture_controls_geometry,
    scaled_overlay_length,
};
use libcompositing_rust::scrolling::overlay_fade::OverlayFade;
use libcompositing_rust::time::MonotonicTime;
use libcompositing_rust::visual_context::{TransformData, TransformDataRole, VisualContextTree};
use libgfx_rust::path::Path;
use libgfx_rust::{
    CapStyle, Color, CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, FloatRect, IntRect, IntSize,
    JoinStyle, Orientation, ShouldAntiAlias, WindingRule, enclosing_int_rect,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PausedDebuggerOverlayAction {
    StepOver,
    Continue,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PictureInPictureControl {
    BackToTab,
    Close,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PausedDebuggerOverlayState {
    pub font_family: Option<String>,
    pub hovered_action: Option<PausedDebuggerOverlayAction>,
}

// The controls stay present while they are hidden, so that they fade out.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PictureInPictureControlsState {
    pub shown: bool,
    pub hovered_control: Option<PictureInPictureControl>,
}

// What the browser draws over a context's page, with each overlay the view has present.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct UIOverlays {
    pub paused_debugger: Option<PausedDebuggerOverlayState>,
    pub picture_in_picture_controls: Option<PictureInPictureControlsState>,
}

/// A label of an overlay as the host shapes it: the font it resolved, in the overlays' resources, and where each glyph
/// goes along the line, in device pixels.
#[derive(Clone, Debug, PartialEq)]
pub struct OverlayLabel {
    pub font_id: u64,
    pub glyph_ids: Vec<u32>,
    pub glyph_x_positions: Vec<f32>,
    pub width: f32,
    // Of the font at the size the label is shaped at: the ascent above the baseline, which is negative, and the descent.
    pub ascent: f32,
    pub descent: f32,
    // The ascent the painter places glyphs below, at the font's own size.
    pub font_ascent: f32,
}

/// Shapes the label of an overlay, in the font family given or the default one, at a device pixel ratio.
pub trait OverlayLabelShaper {
    fn shape_overlay_label(
        &self,
        font_family: Option<&str>,
        device_pixel_ratio: f64,
        text: &str,
    ) -> Option<OverlayLabel>;
}

const PAUSED_DEBUGGER_LABEL: &str = "Paused in debugger";

#[derive(Default)]
pub struct ContextOverlays {
    ui_overlays: UIOverlays,
    picture_in_picture_controls_fade: OverlayFade,
    // The label of the paused debugger overlay, for the font family and device pixel ratio it was shaped for.
    paused_debugger_label: Option<(Option<String>, f64, Option<OverlayLabel>)>,
}

impl ContextOverlays {
    pub fn ui_overlays(&self) -> &UIOverlays {
        &self.ui_overlays
    }

    /// Returns false, changing nothing, for the overlays already shown.
    pub fn set_ui_overlays(&mut self, ui_overlays: UIOverlays, now: MonotonicTime) -> bool {
        if self.ui_overlays == ui_overlays {
            return false;
        }
        self.ui_overlays = ui_overlays;
        let shows_picture_in_picture_controls = self
            .ui_overlays
            .picture_in_picture_controls
            .is_some_and(|controls| controls.shown);
        self.picture_in_picture_controls_fade
            .fade_to(if shows_picture_in_picture_controls { 1.0 } else { 0.0 }, now);
        true
    }

    pub fn fades_need_frame(&self) -> bool {
        self.picture_in_picture_controls_fade.is_fading()
    }

    /// Advances the fades to a frame, answering what of the viewport they repaint.
    pub fn advance_fades(
        &mut self,
        frame_time: MonotonicTime,
        viewport_size: IntSize,
        device_pixel_ratio: f64,
    ) -> Option<IntRect> {
        if !self.picture_in_picture_controls_fade.advance_to(frame_time) {
            return None;
        }
        Some(picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio).buttons_bounds())
    }

    /// What the overlays draw over a frame of the page, with the scrollbars the frame shows.
    pub fn picture_of_frame(
        &mut self,
        scrollbars: &[ScrollbarOfFrame],
        viewport_size: IntSize,
        device_pixel_ratio: f64,
        label_shaper: &dyn OverlayLabelShaper,
    ) -> OverlayPicture {
        let viewport_rect = IntRect::new(0, 0, viewport_size.width, viewport_size.height);
        let mut bounds = IntRect::default();
        let mut presented_scrollbars = Vec::new();
        for scrollbar in scrollbars.iter().filter(|scrollbar| scrollbar.is_painted_by_compositor) {
            presented_scrollbars.push(presented_compositor_scrollbar(scrollbar));
            if scrollbar.opacity <= 0.0 {
                continue;
            }
            // The thumb's outline is drawn around the gutter, which grows into the expanded gutter as it is hovered.
            bounds = bounds.united(
                enclosing_int_rect(scrollbar.gutter_rect)
                    .united(scrollbar.thin_and_expanded_gutter_rect)
                    .inflated(2, 2),
            );
        }

        let mut ui_overlay_records = DisplayListBuilder::new();
        if let Some(paused_debugger) = self.ui_overlays.paused_debugger.clone() {
            let label = self.paused_debugger_label(&paused_debugger, device_pixel_ratio, label_shaper);
            record_paused_debugger_overlay(
                &mut ui_overlay_records,
                viewport_size,
                device_pixel_ratio,
                &paused_debugger,
                label,
            );
        }

        let mut picture_in_picture_controls = None;
        let opacity = self.picture_in_picture_controls_fade.opacity();
        if let Some(controls) = &self.ui_overlays.picture_in_picture_controls
            && opacity > 0.0
        {
            let mut records = DisplayListBuilder::new();
            let controls_bounds =
                record_picture_in_picture_controls(&mut records, viewport_size, device_pixel_ratio, controls);
            picture_in_picture_controls = Some(PictureInPictureControlsPicture {
                opacity,
                bounds: controls_bounds,
                records: AlignedRecords::new(&records.finish().bytes),
            });
        }

        if self.ui_overlays.paused_debugger.is_some() || picture_in_picture_controls.is_some() {
            bounds = viewport_rect;
        }
        OverlayPicture {
            scrollbars: presented_scrollbars,
            ui_overlay_records: AlignedRecords::new(&ui_overlay_records.finish().bytes),
            picture_in_picture_controls,
            bounds: bounds.intersected(viewport_rect),
        }
    }

    fn paused_debugger_label(
        &mut self,
        paused_debugger: &PausedDebuggerOverlayState,
        device_pixel_ratio: f64,
        label_shaper: &dyn OverlayLabelShaper,
    ) -> Option<&OverlayLabel> {
        let is_cached = self
            .paused_debugger_label
            .as_ref()
            .is_some_and(|(font_family, ratio, _)| {
                *font_family == paused_debugger.font_family && *ratio == device_pixel_ratio
            });
        if !is_cached {
            let label = label_shaper.shape_overlay_label(
                paused_debugger.font_family.as_deref(),
                device_pixel_ratio,
                PAUSED_DEBUGGER_LABEL,
            );
            self.paused_debugger_label = Some((paused_debugger.font_family.clone(), device_pixel_ratio, label));
        }
        self.paused_debugger_label
            .as_ref()
            .and_then(|(_, _, label)| label.as_ref())
    }
}

// Records in memory aligned for every command and payload object they hold, which the painter checks and a Vec<u8>
// does not promise.
struct AlignedRecords {
    words: Vec<u64>,
    size: usize,
}

impl AlignedRecords {
    fn new(bytes: &[u8]) -> Self {
        let mut words = vec![0u64; bytes.len().div_ceil(8)];
        for (word, chunk) in words.iter_mut().zip(bytes.chunks(8)) {
            let mut word_bytes = [0u8; 8];
            word_bytes[..chunk.len()].copy_from_slice(chunk);
            *word = u64::from_ne_bytes(word_bytes);
        }
        Self {
            words,
            size: bytes.len(),
        }
    }

    fn as_bytes(&self) -> &[u8] {
        // SAFETY: The words hold `size` initialized bytes.
        unsafe { ak::slice_from_raw_parts(self.words.as_ptr().cast(), self.size) }
    }
}

struct PictureInPictureControlsPicture {
    opacity: f32,
    bounds: IntRect,
    records: AlignedRecords,
}

/// What the overlays draw over one frame, in the order they are drawn, and the part of the viewport they cover.
pub struct OverlayPicture {
    scrollbars: Vec<PresentedScrollbar>,
    ui_overlay_records: AlignedRecords,
    picture_in_picture_controls: Option<PictureInPictureControlsPicture>,
    pub bounds: IntRect,
}

thread_local! {
    // The overlays are recorded in the space of the viewport, which no node of a tree transforms.
    static OVERLAY_TREE: VisualContextTree = VisualContextTree::create(TransformData {
        matrix: FloatMatrix4x4::identity(),
        origin: FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: TransformDataRole::CssTransform,
        synthetic_plane: false,
        establishes_sorting_context: false,
        bounds: FloatRect::default(),
    });
}

impl OverlayPicture {
    /// Paints the overlays over a frame: the scrollbars the compositor paints, and unless they are left out, the
    /// overlays of the browser.
    pub fn paint(&self, painter: &mut impl ReplayPainter, paints_ui_overlays: bool) {
        for scrollbar in &self.scrollbars {
            painter.paint_scrollbar(scrollbar);
        }
        if !paints_ui_overlays {
            return;
        }
        OVERLAY_TREE.with(|tree| {
            let list = ListReplay::new(tree);
            let frame = ReplayFrame::unscrolled(0, &NoNestedDisplayLists);
            let context = RecordsContext {
                list: &list,
                frame: &frame,
                clip_space_matrix: &|_| None,
            };
            replay_records(self.ui_overlay_records.as_bytes(), &context, painter);
            if let Some(controls) = &self.picture_in_picture_controls {
                painter.push_layer(&ReplayLayer {
                    opacity: controls.opacity,
                    blend_mode: CompositingAndBlendingOperator::Normal,
                    filter_bytes: std::ptr::null(),
                    filter_bytes_size: 0,
                    backdrop_filter_bytes: std::ptr::null(),
                    backdrop_filter_bytes_size: 0,
                    backdrop_region: IntRect::default(),
                    backdrop_corner_radii: Default::default(),
                    backdrop_base_color: Color::TRANSPARENT,
                    backdrop_blur_edge: OptionalFloatRect::none(),
                    filter_cache_key: 0,
                    content_bounds: OptionalFloatRect::from(Some(controls.bounds.to_float())),
                });
                replay_records(controls.records.as_bytes(), &context, painter);
                painter.pop();
            }
        });
    }
}

fn presented_compositor_scrollbar(scrollbar: &ScrollbarOfFrame) -> PresentedScrollbar {
    libcompositing_rust::display_list::replay::presented_scrollbar_with_colors(
        scrollbar.gutter_rect,
        scrollbar.thumb_rect,
        scrollbar.expansion,
        scrollbar.opacity,
        scrollbar.thumb_color,
        scrollbar.thumb_outline_color,
        scrollbar.track_color,
    )
}

// The records of one overlay, with the payload of each command laid out after the command as the painter reads it.
struct OverlayRecorder<'a> {
    builder: &'a mut DisplayListBuilder,
}

impl OverlayRecorder<'_> {
    fn append<C: DisplayListCommand>(&mut self, command: &C, inline_data: &[u8]) {
        self.builder.append(command, inline_data, ContextRef::default());
    }

    // The span of `data` placed after a command of type `C`, at an offset aligned for the data, with the padding
    // that takes put in front of it.
    fn data_after_command<C: DisplayListCommand>(
        &self,
        data: &[u8],
        alignment: usize,
    ) -> (DisplayListDataSpan, Vec<u8>) {
        let payload_start = self.builder.byte_size() + HEADER_SIZE;
        let command_end = payload_start + std::mem::size_of::<C>();
        let padding = command_end.next_multiple_of(alignment) - command_end;
        let mut inline_data = vec![0u8; padding];
        inline_data.extend_from_slice(data);
        let span = DisplayListDataSpan {
            offset: (std::mem::size_of::<C>() + padding) as u32,
            size: data.len() as u32,
        };
        (span, inline_data)
    }

    fn fill_rect(&mut self, rect: IntRect, color: Color) {
        self.append(
            &FillRect {
                rect,
                color,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: libcompositing_rust::display_list::commands::EffectNodeIndex::NONE,
            },
            &[],
        );
    }

    fn fill_rect_clipped_to(&mut self, rect: IntRect, color: Color, clip: &Path) {
        self.builder.append_with_inline_clips(
            &FillRect {
                rect,
                color,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: libcompositing_rust::display_list::commands::EffectNodeIndex::NONE,
            },
            &[],
            ContextRef::default(),
            &[PendingInlineClip::intersecting_path(clip, WindingRule::Nonzero)],
        );
    }

    fn fill_path(&mut self, path: &Path, color: Color) {
        let path_bounding_rect = FloatRect::from_array(path.bounding_box());
        if path_bounding_rect.is_empty() {
            return;
        }
        let (path_data, inline_data) =
            self.data_after_command::<FillPath>(&path.serialize_to_bytes(), std::mem::align_of::<u32>());
        self.append(
            &FillPath {
                path_bounding_rect,
                path_data,
                opacity: 1.0,
                paint_kind: PathPaintKind::Color,
                color,
                paint_style: DisplayListPaintStyle::default(),
                winding_rule: WindingRule::Nonzero,
                should_anti_alias: ShouldAntiAlias::Yes,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            },
            &inline_data,
        );
    }

    fn stroke_path(&mut self, path: &Path, color: Color, thickness: f32, cap_style: CapStyle, join_style: JoinStyle) {
        let path_bounding_rect = FloatRect::from_array(path.bounding_box()).inflated(thickness, thickness);
        if path_bounding_rect.is_empty() {
            return;
        }
        let (path_data, inline_data) =
            self.data_after_command::<StrokePath>(&path.serialize_to_bytes(), std::mem::align_of::<u32>());
        self.append(
            &StrokePath {
                cap_style,
                join_style,
                miter_limit: 4.0,
                dash_array: DisplayListDataSpan::default(),
                dash_offset: 0.0,
                path_bounding_rect,
                path_data,
                opacity: 1.0,
                paint_kind: PathPaintKind::Color,
                color,
                paint_style: DisplayListPaintStyle::default(),
                thickness,
                should_anti_alias: ShouldAntiAlias::Yes,
            },
            &inline_data,
        );
    }

    fn draw_label(&mut self, label: &OverlayLabel, baseline_origin: FloatPoint, color: Color, scale: f32) {
        if label.glyph_ids.is_empty() {
            return;
        }
        let glyphs: Vec<DisplayListGlyph> = label
            .glyph_ids
            .iter()
            .zip(&label.glyph_x_positions)
            .map(|(glyph_id, x)| DisplayListGlyph {
                position: FloatPoint {
                    x: (baseline_origin.x + x) / scale,
                    y: baseline_origin.y / scale - label.font_ascent,
                },
                glyph_id: *glyph_id,
            })
            .collect();
        let mut glyph_bytes = vec![0u8; glyphs.len() * std::mem::size_of::<DisplayListGlyph>()];
        for (index, glyph) in glyphs.iter().enumerate() {
            let start = index * std::mem::size_of::<DisplayListGlyph>();
            glyph.write_ffi_bytes(&mut glyph_bytes[start..start + std::mem::size_of::<DisplayListGlyph>()]);
        }
        let (glyph_span, inline_data) =
            self.data_after_command::<DrawGlyphRun>(&glyph_bytes, std::mem::align_of::<DisplayListGlyph>());
        let rect = enclosing_int_rect(FloatRect::new(
            baseline_origin.x,
            baseline_origin.y + label.ascent,
            label.width,
            label.descent - label.ascent,
        ))
        .inflated(2, 2);
        self.append(
            &DrawGlyphRun {
                stroke_width: 0.0,
                font_id: FontResourceId(label.font_id),
                glyphs: glyph_span,
                rect,
                glyph_bounding_rect: rect,
                translation: FloatPoint::default(),
                scale,
                color,
                orientation: Orientation::Horizontal,
                font_smoothing: 0,
            },
            &inline_data,
        );
    }
}

fn rounded_rect_path(rect: FloatRect, radius: f32) -> Path {
    let radius = radius.min(rect.width / 2.0).min(rect.height / 2.0);
    let (left, top, right, bottom) = (rect.x, rect.y, rect.right(), rect.bottom());
    let mut path = Path::new();
    path.move_to(FloatPoint {
        x: left + radius,
        y: top,
    });
    path.line_to(FloatPoint {
        x: right - radius,
        y: top,
    });
    path.arc_to(
        FloatPoint {
            x: right,
            y: top + radius,
        },
        radius,
        false,
        true,
    );
    path.line_to(FloatPoint {
        x: right,
        y: bottom - radius,
    });
    path.arc_to(
        FloatPoint {
            x: right - radius,
            y: bottom,
        },
        radius,
        false,
        true,
    );
    path.line_to(FloatPoint {
        x: left + radius,
        y: bottom,
    });
    path.arc_to(
        FloatPoint {
            x: left,
            y: bottom - radius,
        },
        radius,
        false,
        true,
    );
    path.line_to(FloatPoint {
        x: left,
        y: top + radius,
    });
    path.arc_to(
        FloatPoint {
            x: left + radius,
            y: top,
        },
        radius,
        false,
        true,
    );
    path.close();
    path
}

fn circle_path(center: FloatPoint, radius: f32) -> Path {
    let mut path = Path::new();
    path.move_to(FloatPoint {
        x: center.x + radius,
        y: center.y,
    });
    path.arc_to(
        FloatPoint {
            x: center.x - radius,
            y: center.y,
        },
        radius,
        false,
        true,
    );
    path.arc_to(
        FloatPoint {
            x: center.x + radius,
            y: center.y,
        },
        radius,
        false,
        true,
    );
    path.close();
    path
}

fn rect_center(rect: IntRect) -> FloatPoint {
    // An odd size centers on the pixel below and to the right of the middle, as the integer rects of the painter do.
    FloatPoint {
        x: (rect.x + rect.width / 2) as f32,
        y: (rect.y + rect.height / 2) as f32,
    }
}

// These colors are equivalent to those produced by UI/Qt/ChromeStyle.cpp in light mode. The compositor cannot use
// those colors directly because it does not depend on Qt, so their resolved values are copied here.
const DEBUGGER_OVERLAY_COLOR: Color = Color::from_rgba(0xee, 0xee, 0xef, 168);
const DEBUGGER_TOOLBAR_COLOR: Color = Color::from_rgba(0xff, 0xff, 0xff, 255);
const DEBUGGER_TOOLBAR_BORDER_COLOR: Color = Color::from_rgba(0xce, 0xce, 0xcf, 255);
const DEBUGGER_BUTTON_HOVER_COLOR: Color = Color::from_rgba(0xf1, 0xf1, 0xf2, 255);
const DEBUGGER_TEXT_COLOR: Color = Color::from_rgba(0x18, 0x1d, 0x24, 255);
const DEBUGGER_SHADOW_COLOR: Color = Color::from_rgba(0x18, 0x1d, 0x24, 66);

fn record_paused_debugger_overlay(
    builder: &mut DisplayListBuilder,
    viewport_size: IntSize,
    device_pixel_ratio: f64,
    state: &PausedDebuggerOverlayState,
    label: Option<&OverlayLabel>,
) {
    let mut recorder = OverlayRecorder { builder };
    let geometry: PausedDebuggerOverlayGeometry = paused_debugger_overlay_geometry(viewport_size, device_pixel_ratio);
    let radius = 1.0f32.max((4.0 * device_pixel_ratio) as f32);

    recorder.fill_rect(
        IntRect::new(0, 0, viewport_size.width, viewport_size.height),
        DEBUGGER_OVERLAY_COLOR,
    );

    let mut shadow_rect = geometry.toolbar;
    shadow_rect.y += scaled_overlay_length(2, device_pixel_ratio);
    recorder.fill_path(
        &rounded_rect_path(shadow_rect.to_float(), radius),
        DEBUGGER_SHADOW_COLOR,
    );

    let toolbar_path = rounded_rect_path(geometry.toolbar.to_float(), radius);
    recorder.fill_path(&toolbar_path, DEBUGGER_TOOLBAR_COLOR);

    if let Some(hovered_action) = state.hovered_action {
        let hovered_button = match hovered_action {
            PausedDebuggerOverlayAction::StepOver => geometry.step_over_button,
            PausedDebuggerOverlayAction::Continue => geometry.continue_button,
        };
        recorder.fill_rect_clipped_to(hovered_button, DEBUGGER_BUTTON_HOVER_COLOR, &toolbar_path);
    }

    let line_thickness = 1.0f64.max(device_pixel_ratio) as f32;
    recorder.stroke_path(
        &toolbar_path,
        DEBUGGER_TOOLBAR_BORDER_COLOR,
        line_thickness,
        CapStyle::Butt,
        JoinStyle::Miter,
    );

    let divider_inset = scaled_overlay_length(10, device_pixel_ratio) as f32;
    let divider_x = geometry.step_over_button.x as f32;
    let mut divider = Path::new();
    divider.move_to(FloatPoint {
        x: divider_x,
        y: geometry.toolbar.y as f32 + divider_inset,
    });
    divider.line_to(FloatPoint {
        x: divider_x,
        y: geometry.toolbar.bottom() as f32 - divider_inset,
    });
    recorder.stroke_path(
        &divider,
        DEBUGGER_TOOLBAR_BORDER_COLOR,
        line_thickness,
        CapStyle::Butt,
        JoinStyle::Miter,
    );

    if let Some(label) = label {
        let message = geometry.message;
        let label_x = message.x as f32 + (message.width as f32 - label.width) / 2.0;
        let label_y = rect_center(message).y - (label.ascent + label.descent) / 2.0;
        recorder.draw_label(
            label,
            FloatPoint { x: label_x, y: label_y },
            DEBUGGER_TEXT_COLOR,
            device_pixel_ratio as f32,
        );
    }

    let icon_size = 8.0f64.max(16.0 * device_pixel_ratio) as f32;

    // Step over: an arc over a dot, with an arrow at its end.
    let center = rect_center(geometry.step_over_button);
    let left = center.x - icon_size / 2.0;
    let top = center.y - icon_size / 2.0;
    let right = center.x + icon_size / 2.0;
    let mut arc = Path::new();
    arc.move_to(FloatPoint {
        x: left + icon_size / 8.0,
        y: top + icon_size * 0.55,
    });
    arc.cubic_bezier_curve_to(
        FloatPoint {
            x: left + icon_size / 8.0,
            y: top + icon_size * 0.15,
        },
        FloatPoint {
            x: right - icon_size / 5.0,
            y: top + icon_size * 0.15,
        },
        FloatPoint {
            x: right - icon_size / 5.0,
            y: top + icon_size * 0.55,
        },
    );
    recorder.stroke_path(
        &arc,
        DEBUGGER_TEXT_COLOR,
        line_thickness,
        CapStyle::Round,
        JoinStyle::Round,
    );
    let arrow_width = icon_size / 2.5;
    let mut arrow = Path::new();
    arrow.move_to(FloatPoint {
        x: right - arrow_width,
        y: top + icon_size * 0.55,
    });
    arrow.line_to(FloatPoint {
        x: right,
        y: top + icon_size * 0.55,
    });
    arrow.line_to(FloatPoint {
        x: right - arrow_width / 2.0,
        y: top + icon_size * 0.8,
    });
    arrow.close();
    recorder.fill_path(&arrow, DEBUGGER_TEXT_COLOR);
    let dot_size = 2.0f32.max(line_thickness * 2.0);
    recorder.fill_path(
        &circle_path(
            FloatPoint {
                x: center.x,
                y: center.y + icon_size / 3.0,
            },
            dot_size / 2.0,
        ),
        DEBUGGER_TEXT_COLOR,
    );

    // Continue: a triangle pointing along the line.
    let center = rect_center(geometry.continue_button);
    let icon_left = center.x - icon_size / 2.0;
    let icon_top = center.y - icon_size / 2.0;
    let mut triangle = Path::new();
    triangle.move_to(FloatPoint {
        x: icon_left + icon_size / 4.0,
        y: icon_top + icon_size / 8.0,
    });
    triangle.line_to(FloatPoint {
        x: icon_left + icon_size / 4.0,
        y: icon_top + icon_size - icon_size / 8.0,
    });
    triangle.line_to(FloatPoint {
        x: icon_left + icon_size - icon_size / 8.0,
        y: icon_top + icon_size / 2.0,
    });
    triangle.close();
    recorder.fill_path(&triangle, DEBUGGER_TEXT_COLOR);
}

// A step of an icon's outline, on its grid of 16 units.
#[derive(Clone, Copy)]
enum IconStep {
    Move(f32, f32),
    Line(f32, f32),
    Quadratic(f32, f32, f32, f32),
}

const PICTURE_IN_PICTURE_BUTTON_COLOR: Color = Color::from_rgba(0, 0, 0, 115);
const PICTURE_IN_PICTURE_ICON_COLOR: Color = Color::from_rgba(0xff, 0xff, 0xff, 255);
const PICTURE_IN_PICTURE_HOVERED_ICON_COLOR: Color = Color::from_rgba(0xe0, 0xd4, 0xff, 255);

// Records the controls, answering the rect that holds them.
fn record_picture_in_picture_controls(
    builder: &mut DisplayListBuilder,
    viewport_size: IntSize,
    device_pixel_ratio: f64,
    state: &PictureInPictureControlsState,
) -> IntRect {
    let mut recorder = OverlayRecorder { builder };
    let geometry = picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio);

    // The icons are stroked on a grid of 16 units, and drawn at the given size in points.
    let mut record_button =
        |button: IntRect, control: PictureInPictureControl, icon: &[IconStep], icon_points: f32, stroke_units: f32| {
            let center = FloatPoint {
                x: button.x as f32 + button.width as f32 / 2.0,
                y: button.y as f32 + button.height as f32 / 2.0,
            };
            recorder.fill_path(
                &circle_path(center, button.width as f32 / 2.0),
                PICTURE_IN_PICTURE_BUTTON_COLOR,
            );

            let icon_size = icon_points * device_pixel_ratio as f32;
            let unit = icon_size / 16.0;
            let at = |x: f32, y: f32| FloatPoint {
                x: center.x - icon_size / 2.0 + x * unit,
                y: center.y - icon_size / 2.0 + y * unit,
            };
            let mut path = Path::new();
            for step in icon {
                match *step {
                    IconStep::Move(x, y) => path.move_to(at(x, y)),
                    IconStep::Line(x, y) => path.line_to(at(x, y)),
                    IconStep::Quadratic(control_x, control_y, x, y) => {
                        path.quadratic_bezier_curve_to(at(control_x, control_y), at(x, y));
                    }
                }
            }
            let color = if state.hovered_control == Some(control) {
                PICTURE_IN_PICTURE_HOVERED_ICON_COLOR
            } else {
                PICTURE_IN_PICTURE_ICON_COLOR
            };
            recorder.stroke_path(&path, color, stroke_units * unit, CapStyle::Round, JoinStyle::Round);
        };

    // An arrow out of a box, toward its top right corner.
    let back_to_tab_icon = [
        IconStep::Move(9.5, 2.5),
        IconStep::Line(13.5, 2.5),
        IconStep::Line(13.5, 6.5),
        IconStep::Move(13.5, 2.5),
        IconStep::Line(8.0, 8.0),
        IconStep::Move(12.0, 9.5),
        IconStep::Line(12.0, 12.5),
        IconStep::Quadratic(12.0, 13.5, 11.0, 13.5),
        IconStep::Line(3.5, 13.5),
        IconStep::Quadratic(2.5, 13.5, 2.5, 12.5),
        IconStep::Line(2.5, 5.0),
        IconStep::Quadratic(2.5, 4.0, 3.5, 4.0),
        IconStep::Line(6.5, 4.0),
    ];
    record_button(
        geometry.back_to_tab_button,
        PictureInPictureControl::BackToTab,
        &back_to_tab_icon,
        14.0,
        1.6,
    );

    let close_icon = [
        IconStep::Move(4.0, 4.0),
        IconStep::Line(12.0, 12.0),
        IconStep::Move(12.0, 4.0),
        IconStep::Line(4.0, 12.0),
    ];
    record_button(
        geometry.close_button,
        PictureInPictureControl::Close,
        &close_icon,
        11.0,
        2.0,
    );

    geometry.buttons_bounds()
}
