/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::recorder::{FillPathParams, PaintStyleOrColor};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::fragment_ownership;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_geometry;
use crate::painting::record::assemble::AssemblyHost;
use crate::painting::record::order_tree::RecordedChunk;
use crate::painting::record::paint::text;
use crate::painting::record::trace::{Observer, Operation};
use crate::painting::record::window::InkBound;
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::selection::SearchTextHighlights;
use crate::painting::text_fragment;
use crate::painting::visual_context::build::FIND_IN_PAGE_DIM_EFFECT;
use libgfx_rust::path::Path;
use libgfx_rust::{
    AffineTransform, Color, CompositingAndBlendingOperator, FloatPoint, FloatRect, IntRect, ShouldAntiAlias,
    WindingRule, enclosing_int_rect,
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

fn with_context_and_inline_transform<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    context: ContextRef,
    inline_transform: Option<AffineTransform>,
    paint: impl FnOnce(&mut PaintRecorder<'_, O>),
) {
    recorder.with_context(context, |recorder| {
        let outer_transform = recorder.recorder.set_ambient_inline_transform(inline_transform);
        paint(recorder);
        recorder.recorder.set_ambient_inline_transform(outer_transform);
    });
}

struct MatchGroup {
    context: ContextRef,
    inline_transform: Option<AffineTransform>,
    fragments: Vec<(NodeSlotId, u32)>,
    holes: Vec<IntRect>,
    bounds: IntRect,
}

fn group_of<'a>(
    groups: &'a mut Vec<MatchGroup>,
    index_of_group: &mut HashMap<(ContextRef, i32, Option<[u32; 6]>), usize>,
    context: ContextRef,
    inline_transform: Option<AffineTransform>,
    bounds: IntRect,
) -> &'a mut MatchGroup {
    let band = bounds.y.div_euclid(BAND_HEIGHT);
    let index_of_group = *index_of_group
        .entry((
            context,
            band,
            inline_transform.map(|transform| transform.values.map(f32::to_bits)),
        ))
        .or_insert_with(|| {
            groups.push(MatchGroup {
                context,
                inline_transform,
                fragments: Vec::new(),
                holes: Vec::new(),
                bounds,
            });
            groups.len() - 1
        });
    let group = &mut groups[index_of_group];
    group.bounds = group.bounds.united(bounds);
    group
}

struct SvgTextMatches {
    context: ContextRef,
    inline_transform: AffineTransform,
    holes: Vec<IntRect>,
    current_match: Option<SvgCurrentMatch>,
}

struct SvgCurrentMatch {
    backgrounds: Vec<(IntRect, Color)>,
    glyphs: Vec<(Path, Color)>,
}

fn svg_text_element_and_outermost_svg_of(
    source: &impl PaintRead,
    text_node: NodeSlotId,
) -> Option<(NodeSlotId, NodeSlotId)> {
    let mut text_element = source.node_parent_if_live(text_node)?;
    if source.node_kind_if_live(text_element) != Some(NodeKind::SVGTextBox) {
        return None;
    }
    let mut ancestor = source.node_parent_if_live(text_element)?;
    while source.node_kind_if_live(ancestor) == Some(NodeKind::SVGTextBox) {
        text_element = ancestor;
        ancestor = source.node_parent_if_live(ancestor)?;
    }
    loop {
        let kind = source.node_kind_if_live(ancestor)?;
        if kind == NodeKind::SVGTextPathBox || node_facts::kind_is_svg_resource_box(kind) {
            return None;
        }
        let parent = source.node_parent_if_live(ancestor)?;
        if kind == NodeKind::SVGSVGBox
            && !source
                .node_kind_if_live(parent)
                .is_some_and(node_facts::kind_is_svg_content)
        {
            let are_populated =
                source.paintable_row_is_populated(text_element) && source.paintable_row_is_populated(ancestor);
            return are_populated.then_some((text_element, ancestor));
        }
        ancestor = parent;
    }
}

fn collect_svg_text_matches<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    highlights: &SearchTextHighlights,
    nodes: &[NodeSlotId],
) -> Vec<SvgTextMatches> {
    let source = recorder.source;
    let mut text_nodes_of_elements: Vec<(NodeSlotId, NodeSlotId, NodeSlotId)> = nodes
        .iter()
        .filter_map(|&text_node| {
            let (text_element, outermost_svg) = svg_text_element_and_outermost_svg_of(source, text_node)?;
            source
                .node_style_if_live(source.node_parent_if_live(text_node)?)
                .is_some_and(|style| style.visibility() == crate::css::css_enums::visibility::VISIBLE)
                .then_some((text_element, outermost_svg, text_node))
        })
        .collect();
    text_nodes_of_elements.sort_by_key(|(text_element, _, _)| text_element.index);

    let world_transforms = recorder.paint_state.unscrolled_world_transforms();
    let device_scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    let mut matches_of_elements = Vec::new();
    for text_nodes in text_nodes_of_elements.chunk_by(|(text_element, _, _), (other, _, _)| text_element == other) {
        let (text_element, outermost_svg, _) = text_nodes[0];
        let context = recorder.own_context(text_element);
        let Some(to_outermost_svg) = world_transforms
            .draw_matrix(recorder.own_context(outermost_svg).spatial)
            .inverse()
            .map(|from_viewport| {
                from_viewport
                    .multiplied(world_transforms.draw_matrix(context.spatial))
                    .extract_2d_affine()
            })
        else {
            continue;
        };
        // Holes land on whole device pixels in the space of the content around the <svg>. Text that is rotated or
        // skewed against that space only has its scale undone.
        let to_holes = if to_outermost_svg.b() == 0.0 && to_outermost_svg.c() == 0.0 {
            to_outermost_svg
        } else {
            AffineTransform::identity().scaled(to_outermost_svg.x_scale(), to_outermost_svg.y_scale())
        };
        let Some(inline_transform) = to_holes.inverse() else {
            continue;
        };
        let to_holes = to_holes.scaled(device_scale, device_scale);

        let touching_of_text_nodes: HashMap<NodeSlotId, &[(u32, u8)]> = text_nodes
            .iter()
            .filter_map(|(_, _, text_node)| Some((*text_node, highlights.by_node.get(text_node)?.as_slice())))
            .collect();
        let offsets_in_node =
            |&(match_index, state): &(u32, u8)| highlights.matches[match_index as usize].offsets_in_node(state);
        let mut rects: Vec<(IntRect, u32)> = Vec::new();
        // A match can run through several text boxes, and each box has the outlines of its own character data only.
        let mut cells_of_current_match: Vec<(IntRect, NodeSlotId, NodeSlotId)> = Vec::new();
        for character in paintable_geometry::committed_svg_text_characters(source, text_element) {
            let offset = character.offset_in_text_node;
            let Some(&(match_index, _)) = touching_of_text_nodes
                .get(&character.text_node)
                .and_then(|touching| touching.get(touching.partition_point(|entry| offsets_in_node(entry).1 <= offset)))
                .filter(|entry| offsets_in_node(entry).0 <= offset)
            else {
                continue;
            };
            let cell = character.cell;
            let rect = to_holes.map_rect(FloatRect::new(cell.x, cell.y, cell.width, cell.height));
            let (left, top) = (rect.x.round(), rect.y.round());
            let rect = IntRect::new(
                left as i32,
                top as i32,
                (rect.right().round() - left) as i32,
                (rect.bottom().round() - top) as i32,
            );
            let Some(text_box) = source.node_parent_if_live(character.text_node) else {
                continue;
            };
            if rect.is_empty() {
                continue;
            }
            rects.push((rect, match_index));
            if highlights.matches[match_index as usize].is_current {
                cells_of_current_match.push((rect, text_box, character.text_node));
            }
        }

        let holes = text::joined_along_lines(rects)
            .into_iter()
            .map(|(rect, _)| rect)
            .collect();
        let mut colors_of_text_boxes: Vec<(NodeSlotId, Option<(Color, Color)>)> = Vec::new();
        let mut backgrounds = Vec::new();
        for &(cell, text_box, text_node) in &cells_of_current_match {
            let colors = match colors_of_text_boxes.iter().find(|(other, _)| *other == text_box) {
                Some(&(_, colors)) => colors,
                None => {
                    let colors = text::current_svg_search_text_colors(recorder, text_box, text_node);
                    colors_of_text_boxes.push((text_box, colors));
                    colors
                }
            };
            if let Some((background_color, _)) = colors {
                backgrounds.push((cell, background_color));
            }
        }
        let current_match = (!backgrounds.is_empty()).then(|| {
            let glyphs = colors_of_text_boxes
                .iter()
                .filter_map(|&(text_box, colors)| {
                    let (_, text_color) = colors?;
                    let mut glyphs = Path::new();
                    if let Some(segments) = paintable_geometry::committed_svg_text_segments(source, text_box) {
                        for segment in segments.iter() {
                            glyphs.append_path(&segment.path);
                        }
                    } else if let Some(path) = paintable_geometry::committed_svg_path(source, text_box) {
                        glyphs.append_path(&path.path);
                    }
                    Some((glyphs.copy_transformed(to_holes.values), text_color))
                })
                .collect();
            SvgCurrentMatch { backgrounds, glyphs }
        });
        matches_of_elements.push(SvgTextMatches {
            context,
            inline_transform,
            holes,
            current_match,
        });
    }
    matches_of_elements
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
    let mut index_of_group: HashMap<(ContextRef, i32, Option<[u32; 6]>), usize> = HashMap::new();
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
        group_of(&mut groups, &mut index_of_group, context, None, rect)
            .fragments
            .push((block, index));
        true
    });

    let device_pixels = |css_pixels: i64| converter.rounded_device_pixels(CssPixels::from_integer(css_pixels));
    let hole_radius = text::search_text_corner_radius(converter);
    let reach_past_fragments = device_pixels(8) as f32;

    let matches_in_svg_text = if has_dim_effect {
        collect_svg_text_matches(recorder, &highlights, &nodes)
    } else {
        Vec::new()
    };
    for found in &matches_in_svg_text {
        for &hole in &found.holes {
            let bounds = enclosing_int_rect(
                found
                    .inline_transform
                    .map_rect(hole.to_float().inflated(reach_past_fragments, reach_past_fragments)),
            );
            group_of(
                &mut groups,
                &mut index_of_group,
                in_dim_effect(found.context),
                Some(found.inline_transform),
                bounds,
            )
            .holes
            .push(hole);
        }
    }
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

    let faded =
        |color: Color| color.with_alpha((f32::from(color.alpha()) * inputs.find_in_page_dim_opacity).round() as u8);
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
            recorder
                .recorder
                .fill_rect(dim_rect, faded(DIM_COLOR), ForceDarkRole::None);
        });
    });

    let mut holes_of_groups: Vec<Vec<IntRect>> = Vec::with_capacity(groups.len());
    for group in &groups {
        let mut holes = text::search_text_match_rects(recorder, &group.fragments);
        holes.extend(&group.holes);
        record_chunk(recorder, &mut chunks, |recorder| {
            with_context_and_inline_transform(recorder, group.context, group.inline_transform, |recorder| {
                for (color, blur_radius) in [
                    (Color::from_rgba(0, 0, 0, 128), device_pixels(1)),
                    (Color::from_rgb(255, 255, 255), 0),
                ] {
                    for hole in &holes {
                        text::paint_search_text_ring(recorder, *hole, faded(color), blur_radius, 0);
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
            with_context_and_inline_transform(recorder, group.context, group.inline_transform, |recorder| {
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
        for found in &matches_in_svg_text {
            if let Some(current_match) = &found.current_match {
                with_context_and_inline_transform(
                    recorder,
                    in_dim_effect(found.context),
                    Some(found.inline_transform),
                    |recorder| {
                        text::paint_current_svg_search_text_match(
                            recorder,
                            &current_match.backgrounds,
                            &current_match.glyphs,
                        );
                    },
                );
            }
        }
    });
    chunks
}
