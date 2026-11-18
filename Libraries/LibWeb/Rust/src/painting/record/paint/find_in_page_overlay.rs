/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::recorder::{FillPathParams, PaintStyleOrColor};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::fragment_ownership;
use crate::painting::paint_read::PaintRead;
use crate::painting::record::assemble::AssemblyHost;
use crate::painting::record::order_tree::RecordedChunk;
use crate::painting::record::paint::text;
use crate::painting::record::trace::{Observer, Operation};
use crate::painting::record::window::InkBound;
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::text_fragment;
use crate::painting::visual_context::build::FIND_IN_PAGE_DIM_EFFECT;
use libgfx_rust::path::Path;
use libgfx_rust::{
    Color, CompositingAndBlendingOperator, FloatPoint, FloatRect, IntRect, ShouldAntiAlias, WindingRule,
};
use std::collections::HashMap;
use std::sync::Arc;

const DIM_COLOR: Color = Color::from_rgba(26, 26, 26, 64);

// Matches are recorded in groups by visual context and by band of this height in device pixels.
const BAND_HEIGHT: i32 = 1024;

fn add_rounded_rect(path: &mut Path, rect: FloatRect, radius: f32) {
    let radius = radius.min(rect.width / 2.0).min(rect.height / 2.0);
    let (left, top, right, bottom) = (rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
    let point = |x, y| FloatPoint { x, y };
    path.move_to(point(left + radius, top));
    path.line_to(point(right - radius, top));
    path.arc_to(point(right, top + radius), radius, false, true);
    path.line_to(point(right, bottom - radius));
    path.arc_to(point(right - radius, bottom), radius, false, true);
    path.line_to(point(left + radius, bottom));
    path.arc_to(point(left, bottom - radius), radius, false, true);
    path.line_to(point(left, top + radius));
    path.arc_to(point(left + radius, top), radius, false, true);
    path.close();
}

fn record_chunk<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    chunks: &mut Vec<Arc<RecordedChunk>>,
    paint: impl FnOnce(&mut PaintRecorder<'_, O>),
) {
    recorder.trace_paint(Operation::Named(None, "find-in-page-overlay"), paint);
    chunks.extend(recorder.take_run_chunk());
}

struct MatchGroup {
    context: ContextRef,
    fragments: Vec<(NodeSlotId, u32)>,
    bounds: IntRect,
}

pub(crate) fn record_find_in_page_overlay<O: Observer>(recorder: &mut PaintRecorder<'_, O>) -> Vec<Arc<RecordedChunk>> {
    let inputs = recorder.inputs;
    let converter = recorder.converter;
    let highlights = Arc::clone(&recorder.paint_state.search_text);
    let mut nodes: Vec<NodeSlotId> = highlights.by_node.keys().copied().collect();
    nodes.sort_unstable_by_key(|node| node.index);

    // The dim, the rings and the holes share one effect, and so one layer. A hole is erased from that layer in the
    // space and under the clips of its match.
    let has_dim_effect = recorder.paint_state.visual_context_tree.is_some();
    let in_dim_effect = |context: ContextRef| ContextRef {
        effect: FIND_IN_PAGE_DIM_EFFECT,
        ..context
    };

    let mut groups: Vec<MatchGroup> = Vec::new();
    let mut index_of_group: HashMap<(ContextRef, i32), usize> = HashMap::new();
    let mut fragments_with_current_match: Vec<(ContextRef, Vec<(NodeSlotId, u32)>)> = Vec::new();
    text_fragment::for_each_fragment_of_nodes(recorder.source, &nodes, |block, index, fragment| {
        let touching = text::search_text_matches_touching_fragment(&highlights, fragment);
        if touching.is_empty() {
            return true;
        }
        let is_visible = recorder
            .source
            .node_style_if_live(fragment.style_source)
            .is_some_and(|style| style.visibility() == crate::css::css_enums::visibility::VISIBLE);
        if !is_visible {
            return true;
        }
        let node = fragment.layout_node;
        let owner = fragment_ownership::nearest_self_painting_inline_box(recorder.source, node).unwrap_or(block);
        let context = recorder.context_for_phase(owner, PaintPhase::Foreground);
        let context = if has_dim_effect {
            in_dim_effect(context)
        } else {
            context
        };
        if touching
            .iter()
            .any(|&(match_index, _)| highlights.matches[match_index as usize].is_current)
        {
            match fragments_with_current_match
                .iter_mut()
                .find(|(other, _)| *other == context)
            {
                Some((_, fragments)) => fragments.push((block, index)),
                None => fragments_with_current_match.push((context, vec![(block, index)])),
            }
        }
        if !has_dim_effect {
            return true;
        }
        let rect = converter.enclosing_device_rect(text_fragment::absolute_rect(recorder.source, fragment));
        let band = rect.y.div_euclid(BAND_HEIGHT);
        let index_of_group = *index_of_group.entry((context, band)).or_insert_with(|| {
            groups.push(MatchGroup {
                context,
                fragments: Vec::new(),
                bounds: rect,
            });
            groups.len() - 1
        });
        let group = &mut groups[index_of_group];
        group.fragments.push((block, index));
        group.bounds = group.bounds.united(rect);
        true
    });

    let device_pixels = |css_pixels: i64| converter.rounded_device_pixels(CssPixels::from_integer(css_pixels));
    let hole_radius = text::search_text_corner_radius(converter);
    let reach_past_fragments = device_pixels(8) as f32;
    groups.retain(|group| {
        let bound = InkBound::in_space(
            group.context.spatial,
            group
                .bounds
                .to_float()
                .inflated(reach_past_fragments, reach_past_fragments),
        );
        !recorder
            .window_spaces
            .as_ref()
            .is_some_and(|window| window.is_outside(&bound))
    });

    let mut chunks = Vec::new();
    let dim_rect = inputs
        .canvas_fill_rect
        .map_or(inputs.bitmap_rect, |rect| rect.united(inputs.bitmap_rect));
    let viewport_context = recorder.own_context(recorder.viewport);
    let dim_context = if has_dim_effect {
        in_dim_effect(viewport_context)
    } else {
        viewport_context
    };
    record_chunk(recorder, &mut chunks, |recorder| {
        recorder.with_context(dim_context, |recorder| {
            recorder.recorder.fill_rect(dim_rect, DIM_COLOR, ForceDarkRole::None);
        });
    });

    let mut holes_of_groups: Vec<Vec<IntRect>> = Vec::with_capacity(groups.len());
    for group in &groups {
        let holes = text::search_text_match_rects(recorder, &group.fragments);
        record_chunk(recorder, &mut chunks, |recorder| {
            recorder.with_context(group.context, |recorder| {
                for (color, blur_radius) in [
                    (Color::from_rgba(0, 0, 0, 128), device_pixels(1)),
                    (Color::from_rgb(255, 255, 255), 0),
                ] {
                    for hole in &holes {
                        text::paint_search_text_ring(recorder, *hole, color, blur_radius, 0);
                    }
                }
            });
        });
        holes_of_groups.push(holes);
    }
    // A ring painted after a hole was erased would show inside that hole.
    for (group, holes) in groups.iter().zip(&holes_of_groups) {
        if holes.is_empty() {
            continue;
        }
        let mut path = Path::new();
        for hole in holes {
            add_rounded_rect(&mut path, hole.to_float(), hole_radius as f32);
        }
        record_chunk(recorder, &mut chunks, |recorder| {
            recorder.with_context(group.context, |recorder| {
                recorder.recorder.fill_path_with_compositing_and_blending_operator(
                    FillPathParams {
                        force_dark_role: ForceDarkRole::None,
                        path: &path,
                        opacity: 1.0,
                        paint_style_or_color: PaintStyleOrColor::Color(Color::from_rgb(0, 0, 0)),
                        winding_rule: WindingRule::Nonzero,
                        should_anti_alias: ShouldAntiAlias::Yes,
                    },
                    CompositingAndBlendingOperator::DestinationOut,
                );
            });
        });
    }

    record_chunk(recorder, &mut chunks, |recorder| {
        for (context, fragments) in &fragments_with_current_match {
            recorder.with_context(*context, |recorder| {
                text::paint_current_search_text_match(recorder, fragments);
            });
        }
    });
    chunks
}
