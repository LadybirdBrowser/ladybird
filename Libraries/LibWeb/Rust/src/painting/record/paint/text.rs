/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::record::trace::Observer;

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::svg_paint::svg_paint_color;
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::display_list::commands::{DisplayListGlyph, FontResourceId, PaintOuterBoxShadow, TextShadowLayer};
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::{FillPathParams, GlyphRunForRecording, PaintStyleOrColor};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paintable_data::{FragmentRecord, SELECTION_STATE_START_AND_END};
use crate::painting::record::PaintRecorder;
use crate::painting::record::inputs::CaretTarget;
use crate::painting::selection::{HighlightPseudoElement, SearchTextHighlights, SelectionRange};
use crate::painting::text_fragment::{self, SelectionOffsets};
use libgfx_rust::path::Path;
use libgfx_rust::{
    AffineTransform, Color, CornerRadii, FloatPoint, IntPoint, IntRect, Orientation, ShouldAntiAlias, WindingRule,
};
use std::collections::HashSet;
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
    pub metrics: Option<HighlightDecorationMetrics>,
}

#[derive(Clone, Copy)]
pub(crate) struct HighlightDecorationMetrics {
    pub thickness: Option<CssPixels>,
    pub underline_offset: CssPixels,
    pub underline_position_horizontal: u8,
    pub skip_ink: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum HighlightLayer {
    Custom(u32),
    BuiltIn(HighlightPseudoElement),
    TextFragmentIndication,
}

impl HighlightLayer {
    // https://drafts.csswg.org/css-pseudo-4/#highlight-backgrounds
    // The ::selection overlay is drawn over the ::target-text overlay which is drawn over the ::spelling-error overlay
    // which is drawn over the ::grammar-error overlay which is drawn over the ::highlight overlays.
    // The ::search-text overlay is drawn directly over or below the ::selection overlay depending on the UA, and drawn
    // over all other overlays.
    fn stacking_order(self) -> (u8, u32) {
        match self {
            Self::Custom(layer) => (0, layer),
            Self::BuiltIn(HighlightPseudoElement::SpellingError) => (1, 0),
            Self::TextFragmentIndication => (2, 0),
            Self::BuiltIn(HighlightPseudoElement::Selection) => (3, 0),
            Self::BuiltIn(HighlightPseudoElement::SearchText) => (4, 0),
            Self::BuiltIn(HighlightPseudoElement::SearchTextCurrent) => (5, 0),
        }
    }
}

pub(crate) struct HighlightOverlay {
    pub highlight: HighlightLayer,
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
            && self.text_color == other.text_color
    }
}

pub(crate) struct SelectionStyleAnswer {
    pub facts: crate::painting::host::FfiSelectionStyleFacts,
    pub shadows: Vec<HighlightShadowLayer>,
    pub decoration_metrics: Option<HighlightDecorationMetrics>,
}

const BUILT_IN_HIGHLIGHT_LAYERS: [HighlightLayer; 5] = [
    HighlightLayer::BuiltIn(HighlightPseudoElement::SpellingError),
    HighlightLayer::TextFragmentIndication,
    HighlightLayer::BuiltIn(HighlightPseudoElement::Selection),
    HighlightLayer::BuiltIn(HighlightPseudoElement::SearchText),
    HighlightLayer::BuiltIn(HighlightPseudoElement::SearchTextCurrent),
];

fn highlight_layers_for_fragment<O: Observer>(
    recorder: &PaintRecorder<'_, O>,
    fragment: &FragmentRecord,
) -> Vec<HighlightLayer> {
    let mut layers: Vec<HighlightLayer> = recorder
        .paint_state
        .custom_highlights
        .text_runs
        .get(&fragment.layout_node)
        .into_iter()
        .flatten()
        .map(|&(layer, ..)| HighlightLayer::Custom(layer))
        .collect();
    layers.dedup();
    layers.extend(BUILT_IN_HIGHLIGHT_LAYERS);
    layers
}

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
    layer: HighlightLayer,
    fragment: &FragmentRecord,
) -> Vec<SelectionOffsets> {
    let highlight = match layer {
        HighlightLayer::BuiltIn(highlight) => highlight,
        HighlightLayer::Custom(layer) => {
            let Some(runs) = recorder
                .paint_state
                .custom_highlights
                .text_runs
                .get(&fragment.layout_node)
            else {
                return Vec::new();
            };
            return runs
                .iter()
                .filter(|&&(run_layer, start, end)| {
                    run_layer == layer
                        && end >= fragment.dom_start_offset_in_node
                        && start <= fragment.dom_end_offset_with_trailing_whitespace
                })
                .filter_map(|&(_, start, end)| {
                    text_fragment::compute_selection_offsets(
                        recorder.source,
                        fragment,
                        SELECTION_STATE_START_AND_END,
                        start,
                        end,
                    )
                })
                .map(|offsets| SelectionOffsets {
                    start: offsets.start.min(fragment.length_in_code_units),
                    end: offsets.end.min(fragment.length_in_code_units),
                })
                .filter(|offsets| offsets.start != offsets.end)
                .collect();
        }
        HighlightLayer::TextFragmentIndication => {
            let mut offsets: Vec<SelectionOffsets> = recorder
                .inputs
                .text_fragment_indications
                .iter()
                .filter(|range| range.text_node == fragment.layout_node)
                .filter_map(|range| {
                    text_fragment::selection_offsets_for_dom_range(recorder.source, fragment, range.start, range.end)
                })
                .filter(|offsets| offsets.start != offsets.end)
                .collect();
            offsets.sort_unstable_by_key(|offsets| offsets.start);
            let mut merged: Vec<SelectionOffsets> = Vec::with_capacity(offsets.len());
            for offsets in offsets {
                match merged.last_mut() {
                    Some(last) if offsets.start <= last.end => last.end = last.end.max(offsets.end),
                    _ => merged.push(offsets),
                }
            }
            return merged;
        }
    };
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
        HighlightPseudoElement::SpellingError => {
            let Some(runs) = recorder.paint_state.spelling_errors.by_node.get(&fragment.layout_node) else {
                return Vec::new();
            };
            // A run that ends before the fragment starts, or starts after it ends, has no part in it.
            runs.iter()
                .filter(|&&(start, end)| {
                    end >= fragment.dom_start_offset_in_node
                        && start <= fragment.dom_end_offset_with_trailing_whitespace
                })
                .filter_map(|&(start, end)| {
                    text_fragment::compute_selection_offsets(
                        recorder.source,
                        fragment,
                        SELECTION_STATE_START_AND_END,
                        start,
                        end,
                    )
                })
                .collect()
        }
        HighlightPseudoElement::SearchText | HighlightPseudoElement::SearchTextCurrent => {
            let is_current = highlight == HighlightPseudoElement::SearchTextCurrent;
            let highlights = &recorder.paint_state.search_text;
            search_text_matches_touching_fragment(highlights, fragment)
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

pub(crate) fn search_text_matches_touching_fragment<'a>(
    highlights: &'a SearchTextHighlights,
    fragment: &FragmentRecord,
) -> &'a [(u32, u8)] {
    let Some(touching) = highlights.by_node.get(&fragment.layout_node) else {
        return &[];
    };
    let offsets_in_node =
        |&(match_index, state): &(u32, u8)| highlights.matches[match_index as usize].offsets_in_node(state);
    // A match that ends before the fragment starts, or starts after it ends, has no part in it.
    let first = touching.partition_point(|entry| offsets_in_node(entry).1 < fragment.dom_start_offset_in_node);
    let end =
        touching.partition_point(|entry| offsets_in_node(entry).0 <= fragment.dom_end_offset_with_trailing_whitespace);
    &touching[first..end.max(first)]
}

fn highlight_style<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    layer: HighlightLayer,
    node: NodeSlotId,
) -> Option<Arc<SelectionStyleAnswer>> {
    Some(match layer {
        HighlightLayer::BuiltIn(HighlightPseudoElement::Selection) => recorder.selection_style(node),
        HighlightLayer::BuiltIn(
            highlight @ (HighlightPseudoElement::SearchText | HighlightPseudoElement::SearchTextCurrent),
        ) => recorder.search_text_style(node, highlight)?,
        HighlightLayer::BuiltIn(HighlightPseudoElement::SpellingError) => recorder.spelling_error_style(node),
        HighlightLayer::Custom(layer) => recorder.custom_highlight_style(node, layer)?,
        HighlightLayer::TextFragmentIndication => Arc::new(SelectionStyleAnswer {
            facts: crate::painting::host::FfiSelectionStyleFacts {
                colors_authored: true,
                background_color: recorder.inputs.mark_color,
                text_color: crate::painting::display_list::commands::OptionalColor::some(
                    recorder.inputs.mark_text_color,
                ),
                ..Default::default()
            },
            shadows: Vec::new(),
            decoration_metrics: None,
        }),
    })
}

fn compute_render_spans<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    owned_fragment_indices: &[u32],
    paints_under_own_effects: bool,
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
            || (paints_under_own_effects && parent_style.effects().opacity == 0.0 && !parent_retains_animated_content)
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
                    color: shadow.color.resolve(parent_style.current_color()),
                    offset_x: CssPixels::from_raw(shadow.offset_x),
                    offset_y: CssPixels::from_raw(shadow.offset_y),
                    blur_radius: CssPixels::from_raw(shadow.blur_radius),
                })
                .collect()
        };

        let mut highlights = Vec::new();
        for layer in highlight_layers_for_fragment(recorder, fragment) {
            let ranges = highlight_offsets_for_fragment(recorder, layer, fragment);
            if ranges.is_empty() {
                continue;
            }
            if let Some(answer) = highlight_style(recorder, layer, fragment.layout_node) {
                highlights.push((layer, ranges, answer));
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
        let mut next_ranges = vec![0usize; highlights.len()];
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
                        metrics: answer.decoration_metrics,
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

pub(crate) struct GlyphRunEmission {
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
        baseline_start,
        orientation,
        glyph_bounding_rect,
    }
}

pub(crate) fn paint_fragments_foreground<O: Observer>(recorder: &mut PaintRecorder<'_, O>, block: NodeSlotId) {
    let filter = crate::painting::fragment_ownership::effective_filter(recorder.source, block);
    let fragment_count = recorder.source.committed_side_data(block).fragments().len();
    let mut indices = Vec::with_capacity(fragment_count);
    filter.for_each_owned_fragment_index(fragment_count, |index| indices.push(index as u32));
    paint_fragments(recorder, block, &indices);
}

pub(crate) fn paint_fragments<O: Observer>(recorder: &mut PaintRecorder<'_, O>, block: NodeSlotId, indices: &[u32]) {
    let spans = compute_render_spans(recorder, block, indices, true);

    // https://drafts.csswg.org/css-pseudo-4/#highlight-painting
    // A highlight pseudo-element suppresses the normal drawing of any associated text, and the text
    // decorations (other than shadows) that had been applied to that text. Instead the topmost active
    // highlight overlay redraws that text (and those decorations) over all the highlight overlay
    // backgrounds using that highlight's own color.
    for span in &spans {
        paint_text_shadow(recorder, block, span, &span.shadow_layers);
    }
    // Glyph strokes can extend beyond a highlight rectangle. Keep the normal glyphs outside the highlights, then
    // let each overlay redraw the text inside its rectangle without a stroke.
    let mut stroked_highlights = HashSet::new();
    for fragment_spans in spans.chunk_by(|a, b| a.fragment_index == b.fragment_index) {
        let Some(span) = fragment_spans.iter().find(|span| !span.highlights.is_empty()) else {
            continue;
        };
        let side = recorder.source.committed_side_data(block);
        let fragment = &side.fragments()[span.fragment_index as usize];
        let style = recorder.source.node_style_if_live(fragment.style_source).unwrap();
        let text = style.inherited_text();
        if text.webkit_text_stroke_width <= 0.0 {
            continue;
        }
        let normal_span = RenderSpan {
            fragment_index: span.fragment_index,
            start_code_unit: 0,
            end_code_unit: fragment.length_in_code_units,
            text_color: text.webkit_text_fill_color,
            shadow_layers: Vec::new(),
            highlights: Vec::new(),
            decoration_color: None,
        };
        let mut highlight_path = Path::new();
        for rect in fragment_spans
            .iter()
            .filter(|other| !other.highlights.is_empty())
            .flat_map(|other| highlight_rects(recorder, block, other))
        {
            let rect = recorder.converter.rounded_device_rect(rect).to_float();
            highlight_path.move_to(FloatPoint { x: rect.x, y: rect.y });
            highlight_path.line_to(FloatPoint {
                x: rect.x + rect.width,
                y: rect.y,
            });
            highlight_path.line_to(FloatPoint {
                x: rect.x + rect.width,
                y: rect.y + rect.height,
            });
            highlight_path.line_to(FloatPoint {
                x: rect.x,
                y: rect.y + rect.height,
            });
            highlight_path.close();
        }
        let clip = PendingInlineClip::subtracting_path(&highlight_path, WindingRule::Nonzero);
        let clip_depth = recorder.recorder.ambient_inline_clip_depth();
        recorder.recorder.push_ambient_inline_clips(&[clip]);
        paint_text_fragment(recorder, block, &normal_span, &[], true, None);
        recorder.recorder.truncate_ambient_inline_clips(clip_depth);
        stroked_highlights.insert(span.fragment_index);
    }
    for span in spans.iter().filter(|span| span.highlights.is_empty()) {
        let sets =
            crate::painting::record::paint::text_decoration::decoration_sets_for_span(recorder.source, block, span);
        paint_text_fragment(
            recorder,
            block,
            span,
            &sets,
            !stroked_highlights.contains(&span.fragment_index),
            None,
        );
    }

    let highlight_backdrop = recorder
        .source
        .node_style_if_live(block)
        .map(|style| Color(style.background_color()));
    let mut layers: Vec<HighlightLayer> = spans
        .iter()
        .flat_map(|span| span.highlights.iter().map(|overlay| overlay.highlight))
        .collect();
    layers.sort_unstable_by_key(|layer| layer.stacking_order());
    layers.dedup();
    // AD-HOC: While find-in-page dims the page, its current match is painted over the dim.
    let paints_current_search_text = !recorder.inputs.dims_page_for_find_in_page;
    if !paints_current_search_text {
        layers.retain(|layer| *layer != HighlightLayer::BuiltIn(HighlightPseudoElement::SearchTextCurrent));
    }
    for highlight in layers {
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
                let converter = recorder.converter;
                let previous = recorder.recorder.set_contrast_backdrop(highlight_backdrop);
                for highlight_rect in highlight_rects(recorder, block, span) {
                    recorder.recorder.fill_rect(
                        converter.rounded_device_rect(highlight_rect),
                        Color(overlay.background_color),
                        ForceDarkRole::Selection,
                    );
                }
                recorder.recorder.set_contrast_backdrop(previous);
            }
        }

        // Any text-shadow applying to a highlight pseudo-element is drawn over its corresponding
        // highlight overlay background.
        for (span, overlay) in overlays() {
            paint_text_shadow(recorder, block, span, &overlay.shadow_layers);
        }
    }

    for span in spans
        .iter()
        .filter(|span| !span.highlights.is_empty())
        .filter(|span| paints_current_search_text || current_search_text_overlay(span).is_none())
    {
        let sets =
            crate::painting::record::paint::text_decoration::decoration_sets_for_span(recorder.source, block, span);
        paint_text_fragment(recorder, block, span, &sets, true, None);
    }
}

fn current_search_text_overlay(span: &RenderSpan) -> Option<&HighlightOverlay> {
    span.highlights
        .iter()
        .find(|overlay| overlay.highlight == HighlightLayer::BuiltIn(HighlightPseudoElement::SearchTextCurrent))
}

pub(crate) fn joined_along_lines<Key: Copy + PartialEq>(mut rects: Vec<(IntRect, Key)>) -> Vec<(IntRect, Key)> {
    rects.sort_by_key(|(rect, _)| (rect.x, rect.y));
    let mut joined: Vec<(IntRect, Key)> = Vec::new();
    let mut joinable: Vec<usize> = Vec::new();
    for (rect, key) in rects {
        joinable.retain(|&index| joined[index].0.right() >= rect.x);
        let continued = joinable.iter().copied().find(|&index| {
            let (other, other_key) = joined[index];
            let overlap = other.bottom().min(rect.bottom()) - other.y.max(rect.y);
            // Text of two font sizes on one line gives rects of different heights.
            other_key == key && overlap * 2 >= other.height.min(rect.height)
        });
        match continued {
            Some(index) => joined[index].0 = joined[index].0.united(rect),
            None => {
                joinable.push(joined.len());
                joined.push((rect, key));
            }
        }
    }
    joined.sort_by_key(|(rect, _)| (rect.y, rect.x));
    joined
}

pub(crate) fn search_text_match_rects<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    fragments: &[(NodeSlotId, u32)],
) -> Vec<IntRect> {
    let converter = recorder.converter;
    let source = recorder.source;
    let mut rects: Vec<(IntRect, ())> = Vec::new();
    for &(block, fragment_index) in fragments {
        let side = source.committed_side_data(block);
        let fragment = &side.fragments()[fragment_index as usize];
        for highlight in [
            HighlightPseudoElement::SearchText,
            HighlightPseudoElement::SearchTextCurrent,
        ] {
            for offsets in highlight_offsets_for_fragment(recorder, HighlightLayer::BuiltIn(highlight), fragment) {
                for rect in text_fragment::rects_for_selection_offsets(source, fragment, offsets) {
                    let rect = converter.rounded_device_rect(rect);
                    if !rect.is_empty() {
                        rects.push((rect, ()));
                    }
                }
            }
        }
    }
    joined_along_lines(rects).into_iter().map(|(rect, ())| rect).collect()
}

pub(crate) fn search_text_corner_radius(converter: DevicePixelConverter) -> i32 {
    converter.rounded_device_pixels(CssPixels::from_integer(3))
}

pub(crate) fn paint_search_text_ring<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    rect: IntRect,
    color: Color,
    blur_radius: i32,
    offset_y: i32,
) {
    let ring_width = recorder.converter.rounded_device_pixels(CssPixels::from_integer(1));
    let radius = search_text_corner_radius(recorder.converter);
    let mut shadow_rect = rect.inflated_edges(ring_width, ring_width, ring_width, ring_width);
    shadow_rect.y += offset_y;
    recorder.recorder.paint_outer_box_shadow(
        PaintOuterBoxShadow {
            color,
            blur_radius,
            device_content_rect: rect,
            content_corner_radii: CornerRadii::uniform(radius),
            shadow_rect,
            shadow_corner_radii: CornerRadii::uniform(radius + ring_width),
        },
        ForceDarkRole::None,
    );
}

struct CurrentMatchBackground {
    rect: IntRect,
    color: Color,
}

fn paint_current_match_backgrounds<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    background_rects: Vec<(IntRect, Color)>,
) {
    let converter = recorder.converter;
    let device_pixels = |css_pixels: i64| converter.rounded_device_pixels(CssPixels::from_integer(css_pixels));
    let outset = device_pixels(2);
    let backgrounds: Vec<CurrentMatchBackground> = joined_along_lines(background_rects)
        .into_iter()
        .map(|(rect, background_color)| CurrentMatchBackground {
            rect: rect.inflated_edges(outset, outset, outset, outset),
            color: background_color,
        })
        .collect();
    let scale = recorder.inputs.find_in_page_current_match_scale;
    if scale != 1.0
        && let Some(bounds) = backgrounds
            .iter()
            .map(|background| background.rect)
            .reduce(IntRect::united)
    {
        let center_x = bounds.x as f32 + bounds.width as f32 / 2.0;
        let center_y = bounds.y as f32 + bounds.height as f32 / 2.0;
        let unscaled_transform = recorder.recorder.ambient_inline_transform();
        recorder.recorder.set_ambient_inline_transform(Some(
            unscaled_transform
                .unwrap_or_else(AffineTransform::identity)
                .translated(center_x, center_y)
                .scaled(scale, scale)
                .translated(-center_x, -center_y),
        ));
    }
    for (color, blur_radius, offset_y) in [
        (Color::from_rgba(0, 0, 0, 102), device_pixels(4), device_pixels(1)),
        (Color::from_rgb(255, 255, 255), 0, 0),
    ] {
        for background in &backgrounds {
            paint_search_text_ring(recorder, background.rect, color, blur_radius, offset_y);
        }
    }
    let background_radii = CornerRadii::uniform(search_text_corner_radius(converter));
    for background in &backgrounds {
        recorder.recorder.fill_rect_with_rounded_corners(
            background.rect,
            background.color,
            background_radii,
            ForceDarkRole::Selection,
        );
    }
}

pub(crate) fn paint_current_search_text_match<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    fragments: &[(NodeSlotId, u32)],
) {
    let converter = recorder.converter;
    let mut background_rects: Vec<(IntRect, Color)> = Vec::new();
    let mut matched_spans = Vec::new();
    for &(block, fragment_index) in fragments {
        for span in compute_render_spans(recorder, block, &[fragment_index], false) {
            let Some(overlay) = current_search_text_overlay(&span) else {
                continue;
            };
            let background_color = Color(overlay.background_color);
            for rect in highlight_rects(recorder, block, &span) {
                let rect = converter.rounded_device_rect(rect);
                if !rect.is_empty() {
                    background_rects.push((rect, background_color));
                }
            }
            matched_spans.push((block, span));
        }
    }

    let unscaled_transform = recorder.recorder.ambient_inline_transform();
    paint_current_match_backgrounds(recorder, background_rects);
    let scale_transform = recorder.recorder.ambient_inline_transform();
    for (block, span) in &matched_spans {
        if let Some(overlay) = current_search_text_overlay(span) {
            paint_text_shadow(recorder, *block, span, &overlay.shadow_layers);
        }
    }
    for (block, span) in &matched_spans {
        let sets =
            crate::painting::record::paint::text_decoration::decoration_sets_for_span(recorder.source, *block, span);
        paint_text_fragment(recorder, *block, span, &sets, true, scale_transform);
    }
    recorder.recorder.set_ambient_inline_transform(unscaled_transform);
}

pub(crate) fn current_svg_search_text_colors<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    text_box: NodeSlotId,
    text_node: NodeSlotId,
) -> Option<(Color, Color)> {
    let own_color = recorder
        .source
        .node_style_if_live(text_box)
        .and_then(|style| svg_paint_color(&style.inherited_svg().fill, style.current_color()))
        .map_or(Color::from_rgb(0, 0, 0), Color);
    let answer = recorder.search_text_style(text_node, HighlightPseudoElement::SearchTextCurrent)?;
    let facts = &answer.facts;
    let text_color = if facts.text_color.has_value {
        facts.text_color.value
    } else {
        own_color
    };
    let background_color = if facts.background_color_is_current_color {
        text_color
    } else {
        facts.background_color
    };
    Some((background_color, text_color))
}

pub(crate) fn paint_current_svg_search_text_match<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    backgrounds: &[(IntRect, Color)],
    glyphs: &[(Path, Color)],
) {
    let unscaled_transform = recorder.recorder.ambient_inline_transform();
    paint_current_match_backgrounds(recorder, backgrounds.to_vec());
    let transform = recorder
        .recorder
        .ambient_inline_transform()
        .unwrap_or_else(AffineTransform::identity);
    let rects = joined_along_lines(backgrounds.iter().map(|&(rect, _)| (rect, ())).collect());
    for (rect, ()) in rects {
        recorder.recorder.record_with_inline_clips(
            &[PendingInlineClip::intersecting_float_rect(
                transform.map_rect(rect.to_float()),
            )],
            |recorder| {
                for (path, color) in glyphs {
                    recorder.fill_path(FillPathParams {
                        force_dark_role: ForceDarkRole::Foreground,
                        path,
                        opacity: 1.0,
                        paint_style_or_color: PaintStyleOrColor::Color(*color),
                        winding_rule: WindingRule::Nonzero,
                        should_anti_alias: ShouldAntiAlias::Yes,
                    });
                }
            },
        );
    }
    recorder.recorder.set_ambient_inline_transform(unscaled_transform);
}

fn highlight_rects<O: Observer>(
    recorder: &PaintRecorder<'_, O>,
    block: NodeSlotId,
    span: &RenderSpan,
) -> Vec<CssPixelRect> {
    let side = recorder.source.committed_side_data(block);
    let fragment = &side.fragments()[span.fragment_index as usize];
    let offsets = SelectionOffsets {
        start: span.start_code_unit,
        end: span.end_code_unit,
    };
    text_fragment::rects_for_selection_offsets(recorder.source, fragment, offsets)
}

pub(crate) fn text_shadow_layers_and_bounds(
    shadow_layers: &[ShadowLayer],
    fragment_device_rect: IntRect,
    scale: f64,
) -> (Vec<TextShadowLayer>, IntRect) {
    let converter = crate::painting::display_list::device_pixels::DevicePixelConverter::new(scale);
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
    (layers, shadows_bounding_rect)
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
        baseline_start,
        orientation,
        ..
    } = glyph_run_emission(fragment, run, fragment_absolute_rect, fragment_device_rect, scale);

    // If this is a partial span, slice the glyph run to only include the relevant glyphs.
    let mut span_glyphs = 0..run.glyphs.len();
    if span.start_code_unit != 0 || span.end_code_unit != fragment.length_in_code_units {
        let mut start_glyph = 0usize;
        let mut glyph_count = 0usize;
        let mut code_unit_offset = 0usize;
        for (i, glyph) in run.glyphs.iter().enumerate() {
            if code_unit_offset == span.start_code_unit {
                start_glyph = i;
            }
            code_unit_offset += glyph.length_in_code_units();
            if code_unit_offset == span.end_code_unit {
                glyph_count = i - start_glyph + 1;
                break;
            }
        }
        if glyph_count > 0 {
            span_glyphs = start_glyph..start_glyph + glyph_count;
        }
    }

    let text = recorder
        .recorder
        .notes_record_origins()
        .then(|| fragment_text(recorder.source, fragment, span.start_code_unit..span.end_code_unit));
    recorder.recorder.set_glyph_run_text(text);

    // NB: Force-dark leaves text shadows alone. Light text keeps its color under force-dark, so inverting the dark
    // outline set behind it would put a light halo around light glyphs — which reads as blurred text. Blink doesn't
    // filter text shadows either: DarkModeFilter::ApplyToFlagsIfNeeded recolors only the paint flags, never the
    // shadow filter that TextShadowPainter puts on the text's layer.
    // Shadow layers are ordered front-to-back, so we paint them in reverse.
    let (layers, shadows_bounding_rect) = text_shadow_layers_and_bounds(shadow_layers, fragment_device_rect, scale);
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
            glyphs: run
                .glyphs
                .placed()
                .skip(span_glyphs.start)
                .take(span_glyphs.len())
                .filter(|placed| placed.glyph.should_paint())
                .map(DisplayListGlyph::from),
        },
        scale,
        orientation,
        ForceDarkRole::None,
    );
    recorder.recorder.set_glyph_run_text(None);
}

/// The text a fragment holds between code unit offsets into it.
fn fragment_text(
    source: &crate::painting::paint_read::PaintSource<'_>,
    fragment: &FragmentRecord,
    offsets: std::ops::Range<usize>,
) -> String {
    let Some(rendered_text) = source.rendered_text(fragment.layout_node) else {
        return String::new();
    };
    let end = (fragment.start + offsets.end.min(fragment.length_in_code_units)).min(rendered_text.text.len());
    let start = (fragment.start + offsets.start).min(end);
    String::from_utf16_lossy(&rendered_text.text[start..end])
}

fn paint_text_fragment<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    span: &RenderSpan,
    decoration_sets: &[crate::painting::record::paint::text_decoration::TextDecorationSet],
    draw_glyphs: bool,
    inline_transform: Option<AffineTransform>,
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
        baseline_start,
        orientation,
        glyph_bounding_rect,
    } = glyph_run_emission(fragment, run, fragment_absolute_rect, fragment_device_rect, scale);
    let style = recorder.source.node_style_if_live(fragment.style_source).unwrap();
    let text = style.inherited_text();
    let font_smoothing = text.font_smoothing;
    let stroke_width = if span.highlights.is_empty() {
        (text.webkit_text_stroke_width * scale) as f32
    } else {
        0.0
    };
    let stroke_color = Color(text.webkit_text_stroke_color.resolve(style.current_color()));
    let paint_order = style.inherited_svg().paint_order;
    let run_for_recording = || GlyphRunForRecording {
        font_smoothing,
        font_id: FontResourceId(font_id),
        glyphs: run
            .glyphs
            .placed()
            .filter(|placed| placed.glyph.should_paint())
            .map(DisplayListGlyph::from),
    };

    let paint_glyphs = |recorder: &mut crate::painting::display_list::recorder::DisplayListRecorder| {
        for operation in paint_order {
            let (color, width) = match operation {
                crate::css::css_enums::paint_order::FILL => (Color(span.text_color), 0.0),
                crate::css::css_enums::paint_order::STROKE if stroke_width > 0.0 => (stroke_color, stroke_width),
                _ => continue,
            };
            recorder.draw_glyph_run(
                baseline_start,
                run_for_recording(),
                color,
                width,
                fragment_device_rect,
                scale,
                orientation,
                glyph_bounding_rect,
                ForceDarkRole::Foreground,
            );
        }
    };
    let text = recorder
        .recorder
        .notes_record_origins()
        .then(|| fragment_text(recorder.source, fragment, span.start_code_unit..span.end_code_unit));
    recorder.recorder.set_glyph_run_text(text);
    // Paint text, clipped to span range if not full fragment.
    let is_full_fragment = span.start_code_unit == 0 && span.end_code_unit == fragment.length_in_code_units;
    let mut decoration_box = fragment_absolute_rect;
    if is_full_fragment {
        if draw_glyphs {
            paint_glyphs(&mut recorder.recorder);
        }
    } else {
        let offsets = SelectionOffsets {
            start: span.start_code_unit,
            end: span.end_code_unit,
        };
        let range_rects = text_fragment::rects_for_selection_offsets(recorder.source, fragment, offsets);
        for range_rect in &range_rects {
            let span_rect = converter.rounded_device_rect(*range_rect);
            if !draw_glyphs {
                continue;
            }
            // An ambient inline transform does not move inline clips.
            match inline_transform {
                Some(transform) => recorder.recorder.record_with_inline_clips(
                    &[PendingInlineClip::intersecting_float_rect(
                        transform.map_rect(span_rect.to_float()),
                    )],
                    paint_glyphs,
                ),
                None => recorder.recorder.record_clipped_to(span_rect, paint_glyphs),
            }
        }
        let range_rect = text_fragment::rect_for_selection_offsets(recorder.source, fragment, offsets);
        decoration_box.x = range_rect.x;
        decoration_box.width = range_rect.width;
    }

    recorder.recorder.set_glyph_run_text(None);

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
    if caret.target != (CaretTarget::InBlock { block, owner }) || !recorder.window_is_focused() {
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
