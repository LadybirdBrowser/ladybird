/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::record::trace::Observer;

pub mod background;
pub mod background_resolution;
pub mod border;
pub mod border_image;
pub mod column_rule;
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
pub mod table_backgrounds;
pub mod table_borders;
pub mod text;
pub mod text_decoration;

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::node_painting;
use crate::painting::record::{BasePaintFacts, PaintPhase, PaintRecorder};
use crate::painting::style_queries;

pub(crate) fn paint_phase_mask<O: Observer>(
    recorder: &PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    style: ComputedValuesView<'_>,
    facts: &BasePaintFacts,
) -> u8 {
    let kind = recorder.source.node_kind_if_live(paintable);
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
                recorder.source,
                paintable,
                recorder.inputs.uncaptured.root_background_source,
            )
        {
            phases |= PaintPhase::Background.bit();
        }
        // A border shape fills the area between its two shapes even without border widths.
        if facts.paints_border_image
            || style_queries::has_css_borders(style)
            || crate::painting::border_shape::has_border_shape(style)
            || facts.paints_column_rules
        {
            phases |= PaintPhase::Border.bit();
        }
    }
    // Images paint focused image-map area outlines independently of their own outline, and the assistive-technology
    // focus target paints its ring whether or not it has a CSS outline.
    if kind == Some(NodeKind::ImageBox)
        || Some(paintable) == recorder.inputs.accessibility_focus_target
        || style_queries::outline_geometry(style).is_some()
    {
        phases |= PaintPhase::Outline.bit();
    }
    if kind == Some(NodeKind::Viewport)
        || recorder.data(paintable).own_scroll_node_index
            != crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX
        || crate::painting::chrome_geometry::has_resizer(recorder.source, paintable)
    {
        phases |= PaintPhase::Overlay.bit();
    }
    phases
}

pub(crate) fn paint<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId, phase: PaintPhase) {
    let Some(kind) = recorder.source.node_kind_if_live(paintable) else {
        return;
    };
    if node_painting::is_fragmented_inline(recorder.source, paintable) {
        inline_box::paint(recorder, paintable, phase);
        return;
    }
    if node_painting::has_lines(recorder.source, paintable) {
        if kind == NodeKind::SVGForeignObjectBox && !recorder.is_visible(paintable) {
            return;
        }
        paint_base(recorder, paintable, phase);
        if phase == PaintPhase::Foreground {
            if recorder.is_recording_svg_resource_content()
                || recorder
                    .source
                    .committed_side_data(paintable)
                    .inline_content
                    .as_ref()
                    .is_none_or(|content| content.items.is_empty())
            {
                text::paint_fragments_foreground(recorder, paintable, None);
            }
            // visibility: hidden on this block does not hide descendants that set visibility:
            // visible again, so fragments (and the caret between their glyphs) are filtered by
            // their own node's visibility instead.
            text::paint_cursor(recorder, paintable, None);
        }
        return;
    }
    match kind {
        NodeKind::Box | NodeKind::AudioBox => {
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
            if crate::layout::node_facts::kind_is_non_devolvable_widget(kind) {
                // A non-devolvable widget keeps its native appearance whatever background it has. Layout already gave
                // it zero border widths, so only its background is left to skip.
                // https://drafts.csswg.org/css-ui/#non-devolvable
                paint_base_with(recorder, paintable, phase, |_, _| {});
            } else {
                paint_base(recorder, paintable, phase);
            }
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
        NodeKind::Unset | NodeKind::BreakNode | NodeKind::GeneratedTextNode | NodeKind::Node | NodeKind::TextNode => {}
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

pub(crate) fn paint_base<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId, phase: PaintPhase) {
    paint_base_with(recorder, paintable, phase, background::paint_background);
}

pub(crate) fn paint_base_with<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    phase: PaintPhase,
    paint_background: fn(&mut PaintRecorder<'_, O>, NodeSlotId),
) {
    if phase == PaintPhase::Foreground {
        return;
    }
    let facts = recorder.base_paint_facts(paintable);
    if !facts.is_visible {
        return;
    }
    if matches!(phase, PaintPhase::Background | PaintPhase::Border | PaintPhase::Outline)
        && crate::painting::paintable_geometry::fragment_count(recorder.source, paintable) > 1
    {
        paint_box_fragments(recorder, paintable, phase, &facts);
        return;
    }
    if phase == PaintPhase::Background && !facts.empty_cells_property_applies {
        paint_backdrop_filter(recorder, paintable, &facts);
        paint_background(recorder, paintable);
        if facts.has_box_shadow {
            let border_box_rect =
                crate::painting::paintable_geometry::absolute_border_box_rect(recorder.source, paintable);
            let padding_box_rect =
                crate::painting::paintable_geometry::absolute_padding_box_rect(recorder.source, paintable);
            let border_radii = recorder.border_radii(paintable);
            shadow::paint_box_shadow(recorder, paintable, border_box_rect, padding_box_rect, border_radii);
        }
    }
    if phase == PaintPhase::Border && !facts.empty_cells_property_applies {
        let previous = set_own_background_as_contrast_backdrop(recorder, paintable);
        if !crate::painting::paintable_geometry::committed_uses_collapsing_borders_model(recorder.source, paintable) {
            border::paint_box_borders_from_style(recorder, paintable, &facts);
        }
        if facts.paints_column_rules {
            let context = recorder.for_descendants_context(paintable);
            recorder.with_context(context, |recorder| column_rule::paint_column_rules(recorder, paintable));
        }
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

fn paint_box_fragments<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    phase: PaintPhase,
    facts: &BasePaintFacts,
) {
    use crate::painting::paintable_geometry;
    if facts.empty_cells_property_applies && phase != PaintPhase::Outline {
        return;
    }
    let source = recorder.source;
    let Some(style) = source.node_style_if_live(paintable) else {
        return;
    };
    let fragment_count = paintable_geometry::fragment_count(source, paintable);
    match phase {
        PaintPhase::Background => {
            paint_backdrop_filter(recorder, paintable, facts);
            let paints_background = background_resolution::has_background_to_paint(
                source,
                paintable,
                recorder.inputs.uncaptured.root_background_source,
            );
            let unbroken_edges = {
                let (_, padding, border) = paintable_geometry::unbroken_box_model(source, paintable);
                (padding, border)
            };
            let unbroken_block_size = style_queries::background_layers_have_image(style)
                .then(|| paintable_geometry::unbroken_border_box_size(source, paintable).height);
            let mut block_offset_in_unbroken_box = CssPixels::default();
            for index in 0..fragment_count {
                let (border_box_rect, border_radii) = recorder.fragment_border_box(paintable, index);
                if paints_background && let Some(unbroken_block_size) = unbroken_block_size {
                    let unbroken_border_box_rect = CssPixelRect::new(
                        border_box_rect.x,
                        border_box_rect.y - block_offset_in_unbroken_box,
                        border_box_rect.width,
                        unbroken_block_size,
                    );
                    block_offset_in_unbroken_box += border_box_rect.height;
                    let unbroken_border_radii = crate::painting::visual_context::node_values::piece_border_radii_data(
                        style,
                        unbroken_border_box_rect.width,
                        unbroken_border_box_rect.height,
                        border::ALL_PIECE_EDGES,
                    );
                    let fragment_clip =
                        crate::painting::display_list::builder::PendingInlineClip::intersecting_float_rect(
                            recorder.converter.rounded_device_rect(border_box_rect).to_float(),
                        );
                    recorder.record_with_inline_clips(&[fragment_clip], |recorder| {
                        background::paint_background_within(
                            recorder,
                            paintable,
                            unbroken_border_box_rect,
                            unbroken_border_radii,
                            Some(unbroken_edges),
                            Some(border_box_rect),
                        );
                    });
                } else if paints_background {
                    let edges = (
                        paintable_geometry::committed_fragment_padding(source, paintable, index),
                        paintable_geometry::committed_fragment_border(source, paintable, index),
                    );
                    background::paint_background_within(
                        recorder,
                        paintable,
                        border_box_rect,
                        border_radii,
                        Some(edges),
                        None,
                    );
                }
                if facts.has_box_shadow {
                    let padding_box_rect =
                        paintable_geometry::absolute_fragment_padding_box_rect(source, paintable, index);
                    shadow::paint_box_shadow(recorder, paintable, border_box_rect, padding_box_rect, border_radii);
                }
            }
        }
        PaintPhase::Border => {
            let previous = set_own_background_as_contrast_backdrop(recorder, paintable);
            for index in 0..fragment_count {
                let (border_box_rect, border_radii) = recorder.fragment_border_box(paintable, index);
                border::paint_borders_from_style(
                    recorder,
                    paintable,
                    facts,
                    paintable_geometry::committed_fragment_border(source, paintable, index),
                    border_box_rect,
                    border_radii,
                    paintable_geometry::committed_fragment_present_edges(source, paintable, index),
                );
            }
            recorder.recorder.set_contrast_backdrop(previous);
        }
        PaintPhase::Outline => {
            let outline = outline::outline_data_for_paint(recorder, paintable);
            if outline.is_some() {
                let outline_offset = style_queries::outline_offset(source, paintable);
                let previous = set_own_background_as_contrast_backdrop(recorder, paintable);
                for index in 0..fragment_count {
                    let (border_box_rect, border_radii) = recorder.fragment_border_box(paintable, index);
                    outline::paint_outline(recorder, outline, outline_offset, border_box_rect, border_radii);
                }
                recorder.recorder.set_contrast_backdrop(previous);
            }
            outline::paint_focused_area_outline(recorder, paintable);
        }
        _ => {}
    }
}

/// Hands force-dark the color the box's borders and selections sit against: its own background as authored.
/// Returns the previous scope for the caller to restore.
fn set_own_background_as_contrast_backdrop<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
) -> Option<libgfx_rust::Color> {
    let backdrop = recorder
        .source
        .node_style_if_live(paintable)
        .map(|style| libgfx_rust::Color(style.background().background_color));
    recorder.recorder.set_contrast_backdrop(backdrop)
}

pub(crate) fn paint_backdrop_filter<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    facts: &crate::painting::record::BasePaintFacts,
) {
    if !facts.has_backdrop_filter {
        return;
    }
    let region = recorder
        .converter
        .rounded_device_rect(crate::painting::paintable_geometry::absolute_border_box_rect(
            recorder.source,
            paintable,
        ));
    recorder.recorder.backdrop_filter_region(region);
}
