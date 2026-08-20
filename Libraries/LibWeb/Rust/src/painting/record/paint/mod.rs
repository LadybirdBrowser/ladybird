/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod background;
pub mod background_resolution;
pub mod border;
pub mod border_image;
pub mod fieldset;
pub mod form_controls;
pub mod gradient_resolution;
pub mod inline_box;
pub mod inspector_overlay;
pub mod outline;
pub mod overlay;
pub mod replaced;
pub mod shadow;
pub mod svg;
pub mod table_borders;
pub mod text;
pub mod text_decoration;

use crate::painting::paintable_data::{PaintableKind, PaintableSlotId};
use crate::painting::record::{PaintPhase, PaintRecorder};

pub(crate) fn paint(recorder: &mut PaintRecorder<'_>, paintable: PaintableSlotId, phase: PaintPhase) {
    let kind = recorder.data(paintable).kind;
    match kind {
        PaintableKind::PaintableWithLines
        | PaintableKind::ViewportPaintable
        | PaintableKind::SVGForeignObjectPaintable => {
            if kind == PaintableKind::SVGForeignObjectPaintable && !recorder.is_visible(paintable) {
                return;
            }
            paint_base(recorder, paintable, phase);
            if phase == PaintPhase::Foreground {
                // visibility: hidden on this block does not hide descendants that set visibility:
                // visible again, so fragments (and the caret between their glyphs) are filtered by
                // their own node's visibility instead.
                text::paint_fragments_foreground(recorder, paintable, None);
                text::paint_cursor(recorder, paintable, None);
            }
        }
        PaintableKind::Paintable => paint_base(recorder, paintable, phase),
        PaintableKind::InlinePaintable => inline_box::paint(recorder, paintable, phase),
        PaintableKind::ImagePaintable
        | PaintableKind::CanvasPaintable
        | PaintableKind::VideoPaintable
        | PaintableKind::CheckBoxPaintable
        | PaintableKind::RadioButtonPaintable
        | PaintableKind::NavigableContainerViewportPaintable => {
            if !recorder.is_visible(paintable) {
                return;
            }
            paint_base(recorder, paintable, phase);
            if phase == PaintPhase::Foreground {
                match kind {
                    PaintableKind::ImagePaintable => replaced::paint_image_foreground(recorder, paintable),
                    PaintableKind::CanvasPaintable => replaced::paint_canvas_foreground(recorder, paintable),
                    PaintableKind::VideoPaintable => replaced::paint_video_foreground(recorder, paintable),
                    PaintableKind::NavigableContainerViewportPaintable => {
                        replaced::paint_navigable_container_foreground(recorder, paintable);
                    }
                    PaintableKind::CheckBoxPaintable => form_controls::paint_check_box_foreground(recorder, paintable),
                    PaintableKind::RadioButtonPaintable => {
                        form_controls::paint_radio_button_foreground(recorder, paintable);
                    }
                    _ => unreachable!(),
                }
            }
        }
        PaintableKind::FieldSetPaintable => {
            if !recorder.is_visible(paintable) {
                return;
            }
            if phase != PaintPhase::Border {
                paint_base_with(recorder, paintable, phase, fieldset::paint_background);
                return;
            }
            fieldset::paint_border(recorder, paintable);
        }
        PaintableKind::SVGSVGPaintable
        | PaintableKind::SVGGraphicsPaintable
        | PaintableKind::SVGMaskPaintable
        | PaintableKind::SVGClipPaintable
        | PaintableKind::SVGPatternPaintable => paint_base(recorder, paintable, phase),
        PaintableKind::SVGPathPaintable => svg::paint_path(recorder, paintable, phase),
        PaintableKind::SVGImagePaintable => svg::paint_image_element(recorder, paintable, phase),
        PaintableKind::None => {}
    }
}

pub(crate) fn border_radii_shrunk_for_borders(
    recorder: &mut PaintRecorder<'_>,
    paintable: PaintableSlotId,
) -> crate::painting::border_radii::BorderRadii {
    let mut radii =
        crate::painting::border_radii::BorderRadii::from_raw(recorder.hit_test_facts(paintable).border_radii);
    let layout_node = recorder.data(paintable).layout_node;
    if let Some(style) = recorder.layout_arena.node_style_if_live(layout_node) {
        radii.shrink(
            style.border_top_width(),
            style.border_right_width(),
            style.border_bottom_width(),
            style.border_left_width(),
        );
    }
    radii
}

pub(crate) fn begin_corner_clip(
    recorder: &mut PaintRecorder<'_>,
    rect: libgfx_rust::IntRect,
    radii: &crate::painting::border_radii::BorderRadii,
    corner_clip: libgfx_rust::CornerClip,
) -> bool {
    let corner_radii = radii.corners_unconditionally(&recorder.converter);
    if !corner_radii.has_any_radius() {
        return false;
    }
    recorder.recorder.save();
    recorder.recorder.add_rounded_rect_clip(corner_radii, rect, corner_clip);
    true
}

pub(crate) fn end_corner_clip(recorder: &mut PaintRecorder<'_>, applied: bool) {
    if applied {
        recorder.recorder.restore();
    }
}

pub(crate) fn paint_base(recorder: &mut PaintRecorder<'_>, paintable: PaintableSlotId, phase: PaintPhase) {
    paint_base_with(recorder, paintable, phase, background::paint_background);
}

pub(crate) fn paint_base_with(
    recorder: &mut PaintRecorder<'_>,
    paintable: PaintableSlotId,
    phase: PaintPhase,
    paint_background: fn(&mut PaintRecorder<'_>, PaintableSlotId),
) {
    if phase == PaintPhase::Foreground {
        return;
    }
    let facts = recorder.base_paint_facts(paintable);
    if !facts.is_visible {
        return;
    }
    if phase == PaintPhase::Background && !facts.empty_cells_property_applies {
        paint_backdrop_filter(recorder, paintable, &facts);
        paint_background(recorder, paintable);
        let border_box_rect =
            crate::painting::paintable_geometry::absolute_border_box_rect(recorder.paintables, paintable);
        let padding_box_rect =
            crate::painting::paintable_geometry::absolute_padding_box_rect(recorder.paintables, paintable);
        let border_radii =
            crate::painting::border_radii::BorderRadii::from_raw(recorder.hit_test_facts(paintable).border_radii);
        shadow::paint_box_shadow(recorder, paintable, border_box_rect, padding_box_rect, border_radii);
    }
    if phase == PaintPhase::Border
        && !recorder.data(paintable).uses_collapsing_borders_model
        && !facts.empty_cells_property_applies
    {
        border::paint_box_borders_from_style(recorder, paintable, &facts);
    }
    if phase == PaintPhase::TableCollapsedBorder {
        table_borders::paint_table_borders(recorder, paintable);
    }
    if phase == PaintPhase::Outline {
        outline::paint_outline_phase(recorder, paintable);
    }
    if phase == PaintPhase::Overlay {
        overlay::paint_overlay(recorder, paintable);
    }
}

pub(crate) fn paint_backdrop_filter(
    recorder: &mut PaintRecorder<'_>,
    paintable: PaintableSlotId,
    facts: &crate::painting::record::BasePaintFacts,
) {
    if !facts.has_backdrop_filter {
        return;
    }
    let backdrop_region =
        recorder
            .converter
            .rounded_device_rect(crate::painting::paintable_geometry::absolute_border_box_rect(
                recorder.paintables,
                paintable,
            ));
    let border_radii =
        crate::painting::border_radii::BorderRadii::from_raw(recorder.hit_test_facts(paintable).border_radii);
    let corner_clip = begin_corner_clip(
        recorder,
        backdrop_region,
        &border_radii,
        libgfx_rust::CornerClip::Outside,
    );
    if let Some(filter_bytes) = recorder.paint_host.backdrop_filter_bytes(recorder.shell(paintable)) {
        let corner_radii = border_radii.as_corners(&recorder.converter);
        recorder
            .recorder
            .apply_backdrop_filter(backdrop_region, corner_radii, &filter_bytes);
    }
    end_corner_clip(recorder, corner_clip);
}
