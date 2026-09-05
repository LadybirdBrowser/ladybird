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

use crate::css::computed_value_views::ComputedValuesView;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::node_painting;
use crate::painting::record::{BasePaintFacts, PaintPhase, PaintRecorder};
use crate::painting::style_queries;

pub(crate) fn paint_phase_mask(
    recorder: &PaintRecorder<'_>,
    paintable: NodeSlotId,
    style: ComputedValuesView<'_>,
    facts: &BasePaintFacts,
) -> u8 {
    let kind = recorder.layout_arena.node_kind_if_live(paintable);
    // SVG painters also track dependencies on other elements, even in phases that
    // produce no commands. Keep those painters on their regular path.
    if kind.is_some_and(node_painting::is_svg) {
        return (1 << PaintPhase::COUNT) - 1;
    }

    // Foreground painting includes visible descendants of hidden line containers.
    let mut phases = PaintPhase::Foreground.bit() | PaintPhase::TableCollapsedBorder.bit();
    if !facts.is_visible {
        return phases;
    }
    if !facts.empty_cells_property_applies {
        if facts.has_backdrop_filter
            || facts.has_box_shadow
            || background_resolution::has_background_to_paint(
                recorder.layout_arena,
                paintable,
                recorder.inputs.root_background_source,
            )
        {
            phases |= PaintPhase::Background.bit();
        }
        if facts.paints_border_image || style_queries::has_css_borders(style) {
            phases |= PaintPhase::Border.bit();
        }
    }
    // Images paint focused image-map area outlines independently of their own outline, and the
    // assistive-technology focus target paints its ring whether or not it has a CSS outline.
    if kind == Some(NodeKind::ImageBox)
        || paintable == recorder.inputs.accessibility_focus_target
        || style_queries::outline_geometry(style).is_some()
    {
        phases |= PaintPhase::Outline.bit();
    }
    if kind == Some(NodeKind::Viewport)
        || recorder.data(paintable).own_scroll_node_index
            != crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX
        || crate::painting::chrome_geometry::has_resizer(recorder.layout_arena, paintable)
    {
        phases |= PaintPhase::Overlay.bit();
    }
    phases
}

pub(crate) fn paint(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, phase: PaintPhase) {
    let Some(kind) = recorder.layout_arena.node_kind_if_live(paintable) else {
        return;
    };
    if node_painting::is_inline(recorder.layout_arena, paintable) {
        inline_box::paint(recorder, paintable, phase);
        return;
    }
    if node_painting::has_lines(recorder.layout_arena, paintable) {
        if kind == NodeKind::SVGForeignObjectBox && !recorder.is_visible(paintable) {
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
        return;
    }
    match kind {
        NodeKind::Box | NodeKind::ReplacedBox | NodeKind::AudioBox | NodeKind::SVGBox => {
            paint_base(recorder, paintable, phase);
        }
        NodeKind::ImageBox
        | NodeKind::CanvasBox
        | NodeKind::VideoBox
        | NodeKind::CheckBox
        | NodeKind::RadioButton
        | NodeKind::NavigableContainerViewport => {
            if !recorder.is_visible(paintable) {
                return;
            }
            paint_base(recorder, paintable, phase);
            if phase == PaintPhase::Foreground {
                match kind {
                    NodeKind::ImageBox => replaced::paint_image_foreground(recorder, paintable),
                    NodeKind::CanvasBox => replaced::paint_canvas_foreground(recorder, paintable),
                    NodeKind::VideoBox => replaced::paint_video_foreground(recorder, paintable),
                    NodeKind::NavigableContainerViewport => {
                        replaced::paint_navigable_container_foreground(recorder, paintable);
                    }
                    NodeKind::CheckBox => form_controls::paint_check_box_foreground(recorder, paintable),
                    NodeKind::RadioButton => {
                        form_controls::paint_radio_button_foreground(recorder, paintable);
                    }
                    _ => unreachable!(),
                }
            }
        }
        NodeKind::FieldSetBox => {
            if !recorder.is_visible(paintable) {
                return;
            }
            if phase != PaintPhase::Border {
                paint_base_with(recorder, paintable, phase, fieldset::paint_background);
                return;
            }
            let previous = set_own_background_as_contrast_backdrop(recorder, paintable);
            fieldset::paint_border(recorder, paintable);
            recorder.recorder.set_contrast_backdrop(previous);
        }
        NodeKind::SVGSVGBox
        | NodeKind::SVGGraphicsBox
        | NodeKind::SVGMaskBox
        | NodeKind::SVGClipBox
        | NodeKind::SVGPatternBox => paint_base(recorder, paintable, phase),
        NodeKind::SVGGeometryBox | NodeKind::SVGTextBox | NodeKind::SVGTextPathBox => {
            svg::paint_path(recorder, paintable, phase);
        }
        NodeKind::SVGImageBox => svg::paint_image_element(recorder, paintable, phase),
        NodeKind::Unset
        | NodeKind::BreakNode
        | NodeKind::GeneratedTextNode
        | NodeKind::Node
        | NodeKind::NodeWithStyle
        | NodeKind::TextNode => {}
        NodeKind::Viewport
        | NodeKind::BlockContainer
        | NodeKind::LegendBox
        | NodeKind::TableWrapper
        | NodeKind::TextAreaBox
        | NodeKind::TextInputBox
        | NodeKind::RangeInputBox
        | NodeKind::ListItemMarkerBox
        | NodeKind::SVGForeignObjectBox
        | NodeKind::ListItemBox
        | NodeKind::InlineNode => unreachable!("line and inline paintables are handled before kind dispatch"),
    }
}

pub(crate) fn paint_base(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, phase: PaintPhase) {
    paint_base_with(recorder, paintable, phase, background::paint_background);
}

pub(crate) fn paint_base_with(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    phase: PaintPhase,
    paint_background: fn(&mut PaintRecorder<'_>, NodeSlotId),
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
        if facts.has_box_shadow {
            let border_box_rect =
                crate::painting::paintable_geometry::absolute_border_box_rect(recorder.layout_arena, paintable);
            let padding_box_rect =
                crate::painting::paintable_geometry::absolute_padding_box_rect(recorder.layout_arena, paintable);
            let border_radii = recorder.border_radii(paintable);
            shadow::paint_box_shadow(recorder, paintable, border_box_rect, padding_box_rect, border_radii);
        }
    }
    if phase == PaintPhase::Border
        && !crate::painting::paintable_geometry::committed_uses_collapsing_borders_model(
            recorder.layout_arena,
            paintable,
        )
        && !facts.empty_cells_property_applies
    {
        let previous = set_own_background_as_contrast_backdrop(recorder, paintable);
        border::paint_box_borders_from_style(recorder, paintable, &facts);
        recorder.recorder.set_contrast_backdrop(previous);
    }
    if phase == PaintPhase::TableCollapsedBorder {
        let previous = set_own_background_as_contrast_backdrop(recorder, paintable);
        table_borders::paint_table_borders(recorder, paintable);
        recorder.recorder.set_contrast_backdrop(previous);
    }
    if phase == PaintPhase::Outline {
        let previous = set_own_background_as_contrast_backdrop(recorder, paintable);
        outline::paint_outline_phase(recorder, paintable);
        recorder.recorder.set_contrast_backdrop(previous);
    }
    if phase == PaintPhase::Overlay {
        overlay::paint_overlay(recorder, paintable);
    }
}

/// Hands force-dark the color the box's borders and selections sit against: its own background as authored.
/// Returns the previous scope for the caller to restore.
fn set_own_background_as_contrast_backdrop(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
) -> Option<libgfx_rust::Color> {
    let backdrop = recorder
        .layout_arena
        .node_style_if_live(paintable)
        .map(|style| libgfx_rust::Color(style.background().background_color));
    recorder.recorder.set_contrast_backdrop(backdrop)
}

pub(crate) fn paint_backdrop_filter(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    facts: &crate::painting::record::BasePaintFacts,
) {
    if !facts.has_backdrop_filter {
        return;
    }
    let backdrop_region =
        recorder
            .converter
            .rounded_device_rect(crate::painting::paintable_geometry::absolute_border_box_rect(
                recorder.layout_arena,
                paintable,
            ));
    let border_radii = recorder.border_radii(paintable);
    let filter_bytes = recorder.layout_arena.node_style_if_live(paintable).and_then(|style| {
        let backdrop_filter = &style.effects().backdrop_filter;
        if crate::painting::filter_bytes::contains_url(backdrop_filter) {
            recorder
                .paint_host
                .backdrop_filter_bytes(recorder.layout_node_shell(paintable))
        } else {
            crate::painting::filter_bytes::serialize_non_url_filter(
                backdrop_filter,
                recorder.inputs.device_pixels_per_css_pixel,
            )
        }
    });
    if let Some(filter_bytes) = filter_bytes {
        let corner_radii = border_radii.as_corners(&recorder.converter);
        recorder
            .recorder
            .apply_backdrop_filter(backdrop_region, corner_radii, &filter_bytes);
    }
}
