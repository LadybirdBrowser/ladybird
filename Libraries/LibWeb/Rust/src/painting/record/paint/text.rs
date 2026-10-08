/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::record::trace::Observer;

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::painting::display_list::commands::{DisplayListGlyph, FontResourceId, TextShadowLayer};
use crate::painting::display_list::recorder::GlyphRunForRecording;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paintable_data::{FragmentRecord, SELECTION_STATE_START_AND_END};
use crate::painting::record::PaintRecorder;
use crate::painting::record::inputs::CaretTarget;
use crate::painting::selection::{HighlightPseudoElement, SelectionRange};
use crate::painting::text_fragment::{self, SelectionOffsets};
use libgfx_rust::{Color, FloatPoint, IntPoint, IntRect, Orientation};
use std::sync::Arc;

#[derive(Clone, Copy)]
pub(crate) struct ShadowLayer {
    pub color: u32,
    pub offset_x: CssPixels,
    pub offset_y: CssPixels,
    pub blur_radius: CssPixels,
}

#[derive(Clone, Copy)]
pub(crate) struct HighlightShadowLayer {
    pub layer: ShadowLayer,
    pub color_is_current_color: bool,
}

pub(crate) struct SpanTextDecoration {
    pub lines: [u8; 8],
    pub line_count: u32,
    pub style: u8,
    pub color: u32,
}

pub(crate) struct HighlightOverlay {
    pub highlight: HighlightPseudoElement,
    pub background_color: u32,
    pub shadow_layers: Vec<ShadowLayer>,
    pub text_decoration: Option<SpanTextDecoration>,
}

pub(crate) struct RenderSpan {
    pub fragment_index: u32,
    pub start_code_unit: usize,
    pub end_code_unit: usize,
    pub text_color: u32,
    /// The text's own `text-shadow`, which a highlight overlay paints over.
    pub shadow_layers: Vec<ShadowLayer>,
    /// The highlight overlays over the span, from bottom to top.
    pub highlights: Vec<HighlightOverlay>,
    /// The highlight's color when it has one of its own, which its redraw of the text's original
    /// decorations takes as well.
    pub decoration_color: Option<u32>,
}

impl RenderSpan {
    fn has_same_highlights_as(&self, other: &RenderSpan) -> bool {
        self.highlights
            .iter()
            .map(|overlay| overlay.highlight)
            .eq(other.highlights.iter().map(|overlay| overlay.highlight))
    }
}

pub(crate) struct SelectionStyleAnswer {
    pub facts: crate::painting::host::FfiSelectionStyleFacts,
    pub shadows: Vec<HighlightShadowLayer>,
}

// https://drafts.csswg.org/css-pseudo-4/#highlight-backgrounds
// The ::search-text overlay is drawn directly over or below the ::selection overlay depending on the UA, and drawn
// over all other overlays.
const HIGHLIGHT_OVERLAY_ORDER: [HighlightPseudoElement; 3] = [
    HighlightPseudoElement::Selection,
    HighlightPseudoElement::SearchText,
    HighlightPseudoElement::SearchTextCurrent,
];

fn range_offsets_for_fragment<O: Observer>(
    recorder: &PaintRecorder<'_, O>,
    range: &SelectionRange,
    fragment: &FragmentRecord,
) -> Option<SelectionOffsets> {
    let selection_state = *range.text_states.get(&fragment.layout_node)?;
    text_fragment::compute_selection_offsets(
        recorder.source,
        fragment,
        selection_state,
        range.start_offset,
        range.end_offset,
    )
}

fn highlight_offsets_for_fragment<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    highlight: HighlightPseudoElement,
    fragment: &FragmentRecord,
) -> Vec<SelectionOffsets> {
    let offsets: Vec<SelectionOffsets> = match highlight {
        HighlightPseudoElement::Selection => {
            let offsets = if let Some((start, end)) = recorder.text_control_selection(fragment.layout_node) {
                text_fragment::compute_selection_offsets(
                    recorder.source,
                    fragment,
                    SELECTION_STATE_START_AND_END,
                    start,
                    end,
                )
            } else {
                recorder
                    .paint_state
                    .selection
                    .as_ref()
                    .and_then(|range| range_offsets_for_fragment(recorder, range, fragment))
            };
            offsets.into_iter().collect()
        }
        HighlightPseudoElement::SearchText | HighlightPseudoElement::SearchTextCurrent => {
            let is_current = highlight == HighlightPseudoElement::SearchTextCurrent;
            let highlights = &recorder.paint_state.search_text;
            let Some(touching) = highlights.by_node.get(&fragment.layout_node) else {
                return Vec::new();
            };
            let offsets_in_node =
                |&(match_index, state): &(u32, u8)| highlights.matches[match_index as usize].offsets_in_node(state);
            // A match that ends before the fragment starts, or starts after it ends, has no part in it.
            let first = touching.partition_point(|entry| offsets_in_node(entry).1 < fragment.dom_start_offset_in_node);
            let end = touching
                .partition_point(|entry| offsets_in_node(entry).0 <= fragment.dom_end_offset_with_trailing_whitespace);
            touching[first..end.max(first)]
                .iter()
                .filter_map(|&(match_index, state)| {
                    let found = &highlights.matches[match_index as usize];
                    if found.is_current != is_current {
                        return None;
                    }
                    text_fragment::compute_selection_offsets(
                        recorder.source,
                        fragment,
                        state,
                        found.start_offset,
                        found.end_offset,
                    )
                })
                .collect()
        }
    };
    offsets
        .into_iter()
        .filter(|offsets| offsets.start != offsets.end)
        .collect()
}

fn highlight_style<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    highlight: HighlightPseudoElement,
    node: NodeSlotId,
) -> Arc<SelectionStyleAnswer> {
    match highlight {
        HighlightPseudoElement::Selection => recorder.selection_style(node),
        HighlightPseudoElement::SearchText | HighlightPseudoElement::SearchTextCurrent => {
            recorder.search_text_style(node, highlight)
        }
    }
}

fn compute_render_spans<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    owned_fragment_indices: &[u32],
) -> Vec<RenderSpan> {
    let arena = recorder.source;
    let layout_arena = recorder.source;
    let mut spans: Vec<RenderSpan> = Vec::new();
    for &fragment_index in owned_fragment_indices {
        let side = layout_arena.committed_side_data(block);
        let fragment = &side.fragments()[fragment_index as usize];
        if fragment.glyph_run.is_none() {
            continue;
        }
        let Some(parent_style) = arena.node_style_if_live(fragment.style_source) else {
            continue;
        };
        let parent_retains_animated_content =
            arena.node_flags_if_live(fragment.style_source) & NodeFlag::HasAnimatedOpacityOrTransform as u32 != 0;
        if parent_style.visibility() != crate::css::css_enums::visibility::VISIBLE
            || (parent_style.effects().opacity == 0.0 && !parent_retains_animated_content)
        {
            continue;
        }
        let inherited_text = parent_style.inherited_text();
        let text_color = inherited_text.webkit_text_fill_color;
        let base_shadows = || -> Vec<ShadowLayer> {
            inherited_text
                .text_shadow
                .as_slice()
                .iter()
                .map(|shadow| ShadowLayer {
                    color: shadow.color,
                    offset_x: CssPixels::from_raw(shadow.offset_x),
                    offset_y: CssPixels::from_raw(shadow.offset_y),
                    blur_radius: CssPixels::from_raw(shadow.blur_radius),
                })
                .collect()
        };

        let mut highlights = Vec::new();
        for highlight in HIGHLIGHT_OVERLAY_ORDER {
            let ranges = highlight_offsets_for_fragment(recorder, highlight, fragment);
            if !ranges.is_empty() {
                let answer = highlight_style(recorder, highlight, fragment.layout_node);
                highlights.push((highlight, ranges, answer));
            }
        }
        if highlights.is_empty() {
            spans.push(RenderSpan {
                fragment_index,
                start_code_unit: 0,
                end_code_unit: fragment.length_in_code_units,
                text_color,
                shadow_layers: base_shadows(),
                highlights: Vec::new(),
                decoration_color: None,
            });
            continue;
        }

        let mut boundaries = vec![0, fragment.length_in_code_units];
        for (_, ranges, _) in &highlights {
            boundaries.extend(ranges.iter().flat_map(|offsets| [offsets.start, offsets.end]));
        }
        boundaries.sort_unstable();
        boundaries.dedup();
        let first_span_of_fragment = spans.len();
        // The ranges of one highlight come in order without overlapping, and so do the pieces, so each highlight
        // only needs to look at the first of its ranges that does not end before the piece.
        let mut next_ranges = [0usize; HIGHLIGHT_OVERLAY_ORDER.len()];
        for piece in boundaries.windows(2) {
            let (start, end) = (piece[0], piece[1]);
            let mut span = RenderSpan {
                fragment_index,
                start_code_unit: start,
                end_code_unit: end,
                text_color,
                shadow_layers: base_shadows(),
                highlights: Vec::new(),
                decoration_color: None,
            };
            for ((highlight, ranges, answer), next_range) in highlights.iter().zip(&mut next_ranges) {
                while ranges.get(*next_range).is_some_and(|offsets| offsets.end <= start) {
                    *next_range += 1;
                }
                let Some(offsets) = ranges.get(*next_range) else {
                    continue;
                };
                if start < offsets.start || end > offsets.end {
                    continue;
                }
                let facts = &answer.facts;
                if facts.text_color.has_value {
                    span.text_color = facts.text_color.value.0;
                    span.decoration_color = Some(facts.text_color.value.0);
                }
                let current_color = span.text_color;
                let resolve_color = |color: u32, is_current_color: bool| {
                    if is_current_color { current_color } else { color }
                };
                span.highlights.push(HighlightOverlay {
                    highlight: *highlight,
                    background_color: resolve_color(facts.background_color.0, facts.background_color_is_current_color),
                    shadow_layers: if facts.has_text_shadow {
                        answer
                            .shadows
                            .iter()
                            .map(|shadow| ShadowLayer {
                                color: resolve_color(shadow.layer.color, shadow.color_is_current_color),
                                ..shadow.layer
                            })
                            .collect()
                    } else {
                        Vec::new()
                    },
                    text_decoration: facts.has_text_decoration.then_some(SpanTextDecoration {
                        lines: facts.text_decoration_lines,
                        line_count: facts.text_decoration_line_count,
                        style: facts.text_decoration_style,
                        color: resolve_color(
                            facts.text_decoration_color.0,
                            facts.text_decoration_color_is_current_color,
                        ),
                    }),
                });
            }
            if let Some(previous) = spans[first_span_of_fragment..].last_mut()
                && previous.has_same_highlights_as(&span)
            {
                previous.end_code_unit = end;
                continue;
            }
            spans.push(span);
        }
    }
    spans
}

fn glyphs_of(run: &crate::painting::paintable_data::GlyphRunRecord) -> Vec<DisplayListGlyph> {
    run.glyphs
        .iter()
        .map(|glyph| DisplayListGlyph {
            position: FloatPoint { x: glyph.x, y: glyph.y },
            glyph_id: glyph.glyph_id,
        })
        .collect()
}

pub(crate) struct GlyphRunEmission {
    pub glyphs: Vec<DisplayListGlyph>,
    pub baseline_start: FloatPoint,
    pub orientation: Orientation,
    pub glyph_bounding_rect: IntRect,
}

/// The player draws a vertical run by shifting the horizontal blob right by the fragment rect's
/// width and turning it a quarter turn clockwise about the rect's top-left corner, so the blob's
/// bounds follow the same mapping.
fn vertical_glyph_bounds(horizontal_bounds: IntRect, fragment_device_rect: IntRect) -> IntRect {
    IntRect::new(
        fragment_device_rect.x + fragment_device_rect.width
            - (horizontal_bounds.y + horizontal_bounds.height - fragment_device_rect.y),
        fragment_device_rect.y + (horizontal_bounds.x - fragment_device_rect.x),
        horizontal_bounds.height,
        horizontal_bounds.width,
    )
}

pub(crate) fn glyph_run_emission(
    fragment: &crate::painting::paintable_data::FragmentRecord,
    run: &crate::painting::paintable_data::GlyphRunRecord,
    fragment_absolute_rect: CssPixelRect,
    fragment_device_rect: IntRect,
    scale: f64,
) -> GlyphRunEmission {
    let bounds = run.bounding_box;
    let baseline_start = FloatPoint {
        x: (fragment_absolute_rect.x.to_float() as f64 * scale) as f32,
        y: ((fragment_absolute_rect.y.to_float() + fragment.baseline.to_float()) as f64 * scale) as f32,
    };
    let orientation = if fragment.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB {
        Orientation::Horizontal
    } else {
        Orientation::Vertical
    };
    let horizontal_bounds = IntRect::new(
        (bounds.x * scale as f32 + baseline_start.x).round_ties_even() as i32,
        (bounds.y * scale as f32 + baseline_start.y).round_ties_even() as i32,
        (bounds.width * scale as f32).round_ties_even() as i32,
        (bounds.height * scale as f32).round_ties_even() as i32,
    );
    let glyph_bounding_rect = match orientation {
        Orientation::Horizontal => horizontal_bounds,
        Orientation::Vertical => vertical_glyph_bounds(horizontal_bounds, fragment_device_rect),
    };
    GlyphRunEmission {
        glyphs: glyphs_of(run),
        baseline_start,
        orientation,
        glyph_bounding_rect,
    }
}

pub(crate) fn paint_fragments_foreground<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    owner: Option<NodeSlotId>,
) {
    let filter = crate::painting::fragment_ownership::effective_filter(recorder.source, owner.unwrap_or(block));
    let fragment_count = recorder.source.committed_side_data(block).fragments().len();
    let mut indices = Vec::with_capacity(fragment_count);
    filter.for_each_owned_fragment_index(fragment_count, |index| indices.push(index as u32));
    paint_fragments(recorder, block, &indices);
}

pub(crate) fn paint_fragments<O: Observer>(recorder: &mut PaintRecorder<'_, O>, block: NodeSlotId, indices: &[u32]) {
    let spans = compute_render_spans(recorder, block, indices);

    // https://drafts.csswg.org/css-pseudo-4/#highlight-painting
    // A highlight pseudo-element suppresses the normal drawing of any associated text, and the text
    // decorations (other than shadows) that had been applied to that text. Instead the topmost active
    // highlight overlay redraws that text (and those decorations) over all the highlight overlay
    // backgrounds using that highlight's own color.
    for span in &spans {
        paint_text_shadow(recorder, block, span, &span.shadow_layers);
    }
    for span in spans.iter().filter(|span| span.highlights.is_empty()) {
        let sets = crate::painting::record::paint::text_decoration::decoration_sets_for_span(recorder, block, span);
        paint_text_fragment(recorder, block, span, &sets);
    }

    let highlight_backdrop = recorder
        .source
        .node_style_if_live(block)
        .map(|style| Color(style.background().background_color));
    for highlight in HIGHLIGHT_OVERLAY_ORDER {
        let overlays = || {
            spans.iter().flat_map(move |span| {
                span.highlights
                    .iter()
                    .filter(move |overlay| overlay.highlight == highlight)
                    .map(move |overlay| (span, overlay))
            })
        };

        // Each highlight pseudo-element draws its background over the corresponding portion of the
        // highlight overlay, painting it immediately below any positioned descendants.
        for (span, overlay) in overlays() {
            if Color(overlay.background_color).alpha() > 0 {
                let highlight_rect = highlight_rect(recorder, block, span);
                let converter = recorder.converter;
                let previous = recorder.recorder.set_contrast_backdrop(highlight_backdrop);
                recorder.recorder.fill_rect(
                    converter.rounded_device_rect(highlight_rect),
                    Color(overlay.background_color),
                    ForceDarkRole::Selection,
                );
                recorder.recorder.set_contrast_backdrop(previous);
            }
        }

        // Any text-shadow applying to a highlight pseudo-element is drawn over its corresponding
        // highlight overlay background.
        for (span, overlay) in overlays() {
            paint_text_shadow(recorder, block, span, &overlay.shadow_layers);
        }
    }

    for span in spans.iter().filter(|span| !span.highlights.is_empty()) {
        let sets = crate::painting::record::paint::text_decoration::decoration_sets_for_span(recorder, block, span);
        paint_text_fragment(recorder, block, span, &sets);
    }
}

fn highlight_rect<O: Observer>(recorder: &PaintRecorder<'_, O>, block: NodeSlotId, span: &RenderSpan) -> CssPixelRect {
    let side = recorder.source.committed_side_data(block);
    let fragment = &side.fragments()[span.fragment_index as usize];
    let offsets = SelectionOffsets {
        start: span.start_code_unit,
        end: span.end_code_unit,
    };
    text_fragment::rect_for_selection_offsets(recorder.source, fragment, offsets, || {
        text_fragment::first_available_font(recorder.source, fragment)
    })
}

fn paint_text_shadow<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    span: &RenderSpan,
    shadow_layers: &[ShadowLayer],
) {
    if shadow_layers.is_empty() {
        return;
    }
    let side = recorder.source.committed_side_data(block);
    let fragment = &side.fragments()[span.fragment_index as usize];
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    if run.glyphs.is_empty() {
        return;
    }

    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_absolute_rect = text_fragment::absolute_rect(recorder.source, fragment);
    let fragment_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
    let font_id = recorder.register_font(&run.font);
    let GlyphRunEmission {
        glyphs,
        baseline_start,
        orientation,
        ..
    } = glyph_run_emission(fragment, run, fragment_absolute_rect, fragment_device_rect, scale);

    // If this is a partial span, slice the glyph run to only include the relevant glyphs.
    let mut span_glyphs = glyphs.as_slice();
    if span.start_code_unit != 0 || span.end_code_unit != fragment.length_in_code_units {
        let mut start_glyph = 0usize;
        let mut glyph_count = 0usize;
        let mut code_unit_offset = 0usize;
        for (i, glyph) in run.glyphs.iter().enumerate() {
            if code_unit_offset == span.start_code_unit {
                start_glyph = i;
            }
            code_unit_offset += glyph.length_in_code_units;
            if code_unit_offset == span.end_code_unit {
                glyph_count = i - start_glyph + 1;
                break;
            }
        }
        if glyph_count > 0 {
            span_glyphs = &glyphs[start_glyph..start_glyph + glyph_count];
        }
    }

    // NB: Force-dark leaves text shadows alone. Light text keeps its color under force-dark, so inverting the dark
    // outline set behind it would put a light halo around light glyphs — which reads as blurred text. Blink doesn't
    // filter text shadows either: DarkModeFilter::ApplyToFlagsIfNeeded recolors only the paint flags, never the
    // shadow filter that TextShadowPainter puts on the text's layer.
    // Shadow layers are ordered front-to-back, so we paint them in reverse.
    let mut shadows_bounding_rect = IntRect::default();
    let layers: Vec<TextShadowLayer> = shadow_layers
        .iter()
        .rev()
        .map(|layer| {
            let blur_radius = converter.rounded_device_pixels(layer.blur_radius);
            let offset = FloatPoint {
                x: layer.offset_x.to_float() * scale as f32,
                y: layer.offset_y.to_float() * scale as f32,
            };
            let rounded_offset = IntPoint {
                x: offset.x.round() as i32,
                y: offset.y.round() as i32,
            };
            // Space around the painted text to allow it to blur.
            let margin = blur_radius * 2;
            shadows_bounding_rect = shadows_bounding_rect.united(IntRect::new(
                fragment_device_rect.x + rounded_offset.x - margin,
                fragment_device_rect.y + rounded_offset.y - margin,
                fragment_device_rect.width + margin * 2,
                fragment_device_rect.height + margin * 2,
            ));
            TextShadowLayer {
                offset,
                rounded_offset,
                blur_radius,
                color: Color(layer.color),
            }
        })
        .collect();
    recorder.recorder.paint_text_shadow(
        &layers,
        shadows_bounding_rect,
        fragment_device_rect,
        baseline_start,
        GlyphRunForRecording {
            font_smoothing: recorder
                .source
                .node_style_if_live(fragment.style_source)
                .unwrap()
                .inherited_text()
                .font_smoothing,
            font_id: FontResourceId(font_id),
            glyphs: span_glyphs,
        },
        scale,
        orientation,
        ForceDarkRole::None,
    );
}

fn paint_text_fragment<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    span: &RenderSpan,
    decoration_sets: &[crate::painting::record::paint::text_decoration::TextDecorationSet],
) {
    if span.start_code_unit == span.end_code_unit {
        return;
    }
    let side = recorder.source.committed_side_data(block);
    let fragment = &side.fragments()[span.fragment_index as usize];
    if recorder.inputs.should_show_line_box_borders {
        let converter = recorder.converter;
        let fragment_absolute_rect = text_fragment::absolute_rect(recorder.source, fragment);
        let fragment_absolute_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
        recorder.recorder.draw_rect(
            fragment_absolute_device_rect,
            Color::from_rgb(0, 255, 0),
            false,
            ForceDarkRole::None,
        );
        let one = crate::css::css_pixels::CssPixels::from_integer(1);
        let baseline_start = converter.rounded_device_point(
            fragment_absolute_rect
                .location()
                .translated(crate::css::css_pixels::CssPixels::default(), fragment.baseline),
        );
        let baseline_end = converter.rounded_device_point(
            crate::css::css_pixels::CssPixelPoint::new(fragment_absolute_rect.right(), fragment_absolute_rect.y)
                .translated(-one, fragment.baseline),
        );
        recorder.recorder.draw_line(
            baseline_start,
            baseline_end,
            Color::from_rgb(255, 0, 0),
            1,
            libgfx_rust::LineStyle::Solid,
            Color::TRANSPARENT,
            ForceDarkRole::None,
        );
    }
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_absolute_rect = text_fragment::absolute_rect(recorder.source, fragment);
    let fragment_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
    let font_id = recorder.register_font(&run.font);
    let GlyphRunEmission {
        glyphs,
        baseline_start,
        orientation,
        glyph_bounding_rect,
    } = glyph_run_emission(fragment, run, fragment_absolute_rect, fragment_device_rect, scale);
    let run_for_recording = GlyphRunForRecording {
        font_smoothing: recorder
            .source
            .node_style_if_live(fragment.style_source)
            .unwrap()
            .inherited_text()
            .font_smoothing,
        font_id: FontResourceId(font_id),
        glyphs: &glyphs,
    };

    // Paint text, clipped to span range if not full fragment.
    let is_full_fragment = span.start_code_unit == 0 && span.end_code_unit == fragment.length_in_code_units;
    let mut decoration_box = fragment_absolute_rect;
    if is_full_fragment {
        recorder.recorder.draw_glyph_run(
            baseline_start,
            run_for_recording,
            Color(span.text_color),
            fragment_device_rect,
            scale,
            orientation,
            glyph_bounding_rect,
            ForceDarkRole::Foreground,
        );
    } else {
        let range_rect = text_fragment::rect_for_selection_offsets(
            recorder.source,
            fragment,
            SelectionOffsets {
                start: span.start_code_unit,
                end: span.end_code_unit,
            },
            || text_fragment::first_available_font(recorder.source, fragment),
        );
        let span_rect = converter.rounded_device_rect(range_rect);
        recorder.recorder.record_clipped_to(span_rect, |recorder| {
            recorder.draw_glyph_run(
                baseline_start,
                run_for_recording,
                Color(span.text_color),
                fragment_device_rect,
                scale,
                orientation,
                glyph_bounding_rect,
                ForceDarkRole::Foreground,
            );
        });
        decoration_box.x = range_rect.x;
        decoration_box.width = range_rect.width;
    }

    for set in decoration_sets {
        crate::painting::record::paint::text_decoration::paint_decoration_lines(
            recorder,
            block,
            span.fragment_index,
            decoration_box,
            set,
        );
    }
}

// Paints the caret when it sits in a fragment owned by `owner`; the block itself
// (owner == None) also handles blank lines and empty editable elements.
pub(crate) fn paint_cursor<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    owner: Option<NodeSlotId>,
) {
    let Some(caret) = recorder.inputs.caret else {
        return;
    };
    if caret.target != (CaretTarget::InBlock { block, owner }) {
        return;
    }
    let color = caret.color;
    if color.alpha() == 0 {
        return;
    }
    let converter = recorder.converter;
    recorder.recorder.paint_caret(
        converter.rounded_device_rect(caret.rect),
        color,
        caret.blink_cycle_start_time_ns,
        caret.should_blink,
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vertical_glyph_bounds_follow_the_players_rotation() {
        let fragment = IntRect::new(100, 50, 20, 60);
        // A blob starting two pixels into the fragment, rising four pixels above its top.
        let horizontal = IntRect::new(102, 46, 40, 24);
        let vertical = vertical_glyph_bounds(horizontal, fragment);
        // Its advance runs down the fragment from two pixels in, and its ascent side ends up on the
        // right, four pixels short of the fragment's right edge.
        assert_eq!(vertical, IntRect::new(100, 52, 24, 40));
    }
}
