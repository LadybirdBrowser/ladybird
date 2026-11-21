/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where each effect's content lies and where its output lands, in the effect's own space.
//!
//! A filter applies in its effect node's space, while what the effect composites can be drawn in
//! descendant spaces, or in the space of a positioned descendant that escapes the effect's
//! subtree. Content is therefore mapped into each effect's space before that effect's filter
//! spreads it, and an effect's output becomes part of its parent's content.

use crate::display_list::animated_transform_spaces::easing_stays_within_unit_interval;
use crate::display_list::commands::{DisplayListCommandRun, EffectNodeIndex, SpatialNodeIndex};
use crate::display_list::placement::{
    ANTIALIASING_MARGIN_IN_DEVICE_PIXELS, RasterSpaceGeometry, enclosing_int_rect_within_extent_limit, is_finite_rect,
};
use crate::display_list::reach::Reach;
use crate::filter_bytes::{self, filter_functions_graph};
use crate::host::FfiVisualAnimationTargetKind;
use crate::visual_animation::VisualAnimationValue;
use crate::visual_context::{EffectNode, EffectNodeData, VisualContextTree, WorldTransforms};
use libgfx_rust::filter::Filter;
use libgfx_rust::{FloatMatrix4x4, FloatRect, IntRect};
use std::collections::HashMap;

/// Where content lies in an effect's space. It reaches anywhere through a filter that paints everywhere or through a
/// space that does not map onto the effect's as a two-dimensional transform.
pub type Extent = Reach<FloatRect>;

pub(crate) const LARGEST_FILTER_READ_REACH_IN_DEVICE_PIXELS: f32 = 2048.0;

pub(crate) fn filter_read_reach_in_device_pixels(filter: &[u8], effect_to_device: &FloatMatrix4x4) -> Option<i32> {
    if !effect_to_device.is_2d_affine() {
        return None;
    }
    let probe = FloatRect::new(0.0, 0.0, 1.0, 1.0);
    let changed_output_bounds_of_probe = filter_bytes::changed_output_bounds(filter, probe)?;
    let reach_in_effect_space = [
        probe.x - changed_output_bounds_of_probe.x,
        changed_output_bounds_of_probe.right() - probe.right(),
        probe.y - changed_output_bounds_of_probe.y,
        changed_output_bounds_of_probe.bottom() - probe.bottom(),
    ]
    .into_iter()
    .fold(0.0f32, f32::max);
    let affine = effect_to_device.extract_2d_affine();
    let largest_stretch = affine.a().abs() + affine.b().abs() + affine.c().abs() + affine.d().abs();
    let reach_in_device_pixels =
        (reach_in_effect_space * largest_stretch).ceil() + ANTIALIASING_MARGIN_IN_DEVICE_PIXELS as f32;
    (reach_in_device_pixels.is_finite() && reach_in_device_pixels <= LARGEST_FILTER_READ_REACH_IN_DEVICE_PIXELS)
        .then_some(reach_in_device_pixels as i32)
}

/// How far past each pixel the filter of an effect the compositor composites reads what is composited inside it, in
/// device pixels. Placing a list composites no filter that reads farther than is followed, so that limit stands in only
/// while a transform the compositor animates scales a filter past what placing saw.
pub fn composited_filter_read_reach_in_device_pixels(filter: &[u8], effect_to_device: &FloatMatrix4x4) -> i32 {
    filter_read_reach_in_device_pixels(filter, effect_to_device)
        .unwrap_or(LARGEST_FILTER_READ_REACH_IN_DEVICE_PIXELS as i32)
}

/// Where a backdrop filter reads, in the raster coordinates of the space: its region, and how far its filter reaches
/// past it.
pub fn backdrop_filter_read_reach(
    tree: &VisualContextTree,
    geometry: &RasterSpaceGeometry,
    effect: EffectNodeIndex,
) -> Reach<IntRect> {
    let effect_node = &tree.effect_nodes[effect.0 as usize];
    let EffectNodeData::Effects(effects) = &effect_node.data else {
        return Reach::Anywhere;
    };
    let Some(backdrop) = &effects.backdrop_filter else {
        return Reach::Anywhere;
    };
    let effect_to_device = geometry.world.draw_matrix(effect_node.spatial);
    let Some(read_reach_in_device_pixels) = filter_read_reach_in_device_pixels(&backdrop.filter, &effect_to_device)
    else {
        return Reach::Anywhere;
    };
    let backdrop_region = geometry
        .world
        .map_rect_to_root(effect_node.spatial, backdrop.region.to_float());
    if !is_finite_rect(backdrop_region) {
        return Reach::Anywhere;
    }
    Reach::Within(enclosing_int_rect_within_extent_limit(backdrop_region).inflated_edges(
        read_reach_in_device_pixels,
        read_reach_in_device_pixels,
        read_reach_in_device_pixels,
        read_reach_in_device_pixels,
    ))
}

fn extent_mapped_between_spaces(
    extent: Extent,
    world: &WorldTransforms,
    from: SpatialNodeIndex,
    to: SpatialNodeIndex,
) -> Extent {
    match extent {
        Extent::Within(rect) => world
            .map_rect_between_spaces(rect, from, to)
            .map_or(Extent::Anywhere, Extent::Within),
        extent => extent,
    }
}

/// Where an effect paints what it composites, given that content in the effect's space.
pub fn effect_output_extent(node: &EffectNode, content: Extent) -> Extent {
    let Extent::Within(rect) = content else {
        return content;
    };
    let EffectNodeData::Effects(effects) = &node.data else {
        return content;
    };
    let Some(filter) = effects.filter.as_deref() else {
        return content;
    };
    if !filter_bytes::may_affect_output_bounds(filter) {
        return content;
    }
    filter_bytes::output_bounds(filter, rect).map_or(Extent::Anywhere, Extent::Within)
}

fn output_extent_of_content_change(node: &EffectNode, content_change_in_effect_space: Extent) -> Extent {
    let Extent::Within(rect) = content_change_in_effect_space else {
        return content_change_in_effect_space;
    };
    let EffectNodeData::Effects(effects) = &node.data else {
        return content_change_in_effect_space;
    };
    let Some(filter) = effects.filter.as_deref() else {
        return content_change_in_effect_space;
    };
    filter_bytes::changed_output_bounds(filter, rect).map_or(Extent::Anywhere, Extent::Within)
}

/// The effect's filter, where it can spread its content outside its bounds.
pub fn spreading_filter_of(node: &EffectNode) -> Option<&[u8]> {
    let EffectNodeData::Effects(effects) = &node.data else {
        return None;
    };
    effects
        .filter
        .as_deref()
        .map(|filter| &filter[..])
        .filter(|filter| filter_bytes::may_affect_output_bounds(filter))
}

/// Whether the filter can show its content away from where the content is drawn, as an offset does.
pub fn filter_moves_what_it_filters(node: &EffectNode) -> bool {
    matches!(&node.data, EffectNodeData::Effects(effects)
        if effects.filter.as_deref().is_some_and(|filter| filter_bytes::may_move_what_it_filters(filter)))
}

/// Whether the effect's filter can spread its content outside its bounds at some sample of the animations of the tree:
/// the filter the tree holds or one a keyframe of a filter animation of the effect gives it can.
pub fn filter_may_spread_at_some_sample(tree: &VisualContextTree, effect: EffectNodeIndex) -> bool {
    spreading_filter_of(&tree.effect_nodes[effect.0 as usize]).is_some()
        || tree.some_keyframe_filter_of_effect_spreads(effect)
}

/// The filters the keyframes of the filter animations of a tree give the effects they animate, which placing follows
/// along with the filters the tree holds, so that what it decides of a filter holds for every sample.
pub struct KeyframeFiltersOfEffects {
    by_effect_index: HashMap<u32, KeyframeFilters>,
}

#[derive(Default)]
struct KeyframeFilters {
    /// One per keyframe that gives the effect a filter.
    filters: Vec<Vec<u8>>,
    /// Whether a sample can take a value past those of the two keyframes it is between, as an easing that overshoots
    /// does.
    samples_can_pass_keyframes: bool,
}

impl KeyframeFiltersOfEffects {
    pub fn of(tree: &VisualContextTree) -> Self {
        let mut by_effect_index: HashMap<u32, KeyframeFilters> = HashMap::new();
        for animation in tree
            .visual_animations()
            .iter()
            .filter(|animation| animation.target_kind == FfiVisualAnimationTargetKind::Filter)
        {
            let samples_can_pass_keyframes = !easing_stays_within_unit_interval(&animation.easing)
                || animation
                    .keyframes
                    .iter()
                    .any(|keyframe| !easing_stays_within_unit_interval(&keyframe.easing));
            let filters: Vec<Vec<u8>> = animation
                .keyframes
                .iter()
                .filter_map(|keyframe| match &keyframe.value {
                    VisualAnimationValue::Filter(functions) => {
                        filter_functions_graph(functions.iter().copied().map(Filter::from))
                            .map(|graph| graph.serialize())
                    }
                    _ => None,
                })
                .collect();
            for &effect_index in &animation.node_indices {
                let keyframe_filters = by_effect_index.entry(effect_index).or_default();
                keyframe_filters.filters.extend(filters.iter().cloned());
                keyframe_filters.samples_can_pass_keyframes |= samples_can_pass_keyframes;
            }
        }
        Self { by_effect_index }
    }

    /// Whether the effect's filter can show its content away from where the content is drawn at some sample.
    pub fn filter_may_move_what_it_filters(&self, tree: &VisualContextTree, effect: EffectNodeIndex) -> bool {
        filter_moves_what_it_filters(&tree.effect_nodes[effect.0 as usize])
            || self.by_effect_index.get(&effect.0).is_some_and(|keyframe_filters| {
                keyframe_filters
                    .filters
                    .iter()
                    .any(|filter| filter_bytes::may_move_what_it_filters(filter))
            })
    }

    /// How far past each pixel the effect's filter reads at any sample, in device pixels where `effect_to_device` maps
    /// the space of the effect, or none where that is not known or is farther than tiles are followed.
    pub fn filter_read_reach_in_device_pixels(
        &self,
        tree: &VisualContextTree,
        effect: EffectNodeIndex,
        effect_to_device: &FloatMatrix4x4,
    ) -> Option<i32> {
        let keyframe_filters = self.by_effect_index.get(&effect.0);
        let spreading_keyframe_filters = keyframe_filters
            .into_iter()
            .flat_map(|keyframe_filters| keyframe_filters.filters.iter())
            .map(|filter| &filter[..])
            .filter(|filter| filter_bytes::may_affect_output_bounds(filter));
        if keyframe_filters.is_some_and(|keyframe_filters| keyframe_filters.samples_can_pass_keyframes)
            && spreading_keyframe_filters.clone().next().is_some()
        {
            return None;
        }
        spreading_filter_of(&tree.effect_nodes[effect.0 as usize])
            .into_iter()
            .chain(spreading_keyframe_filters)
            .try_fold(0, |read_reach, filter| {
                Some(read_reach.max(filter_read_reach_in_device_pixels(filter, effect_to_device)?))
            })
    }
}

/// Per effect, the nearest effect at or above it for which `holds` holds, or `EffectNodeIndex::NONE`.
pub(crate) fn nearest_effects_at_or_above_where(
    tree: &VisualContextTree,
    holds: impl Fn(EffectNodeIndex, &EffectNode) -> bool,
) -> Vec<EffectNodeIndex> {
    let mut nearest = vec![EffectNodeIndex::NONE; tree.effect_nodes.len()];
    tree.visit_effect_nodes_parents_first(|index| {
        let node = &tree.effect_nodes[index];
        nearest[index] = if holds(EffectNodeIndex(index as u32), node) {
            EffectNodeIndex(index as u32)
        } else if node.parent.is_none() {
            EffectNodeIndex::NONE
        } else {
            nearest[node.parent.0 as usize]
        };
    });
    nearest
}

/// Per effect, the effects at and above it whose filters a raster of content under it draws: the nearest of them whose
/// filter spreads its content, and, where any does, the outermost of them that holds a filter or a backdrop filter,
/// whose output clip bounds how far they spread it.
#[derive(Default)]
pub struct FiltersOfEffects {
    nearest_spreading_effect_by_effect_index: Vec<EffectNodeIndex>,
    outermost_effect_with_filter_if_spread_by_effect_index: Vec<EffectNodeIndex>,
}

impl FiltersOfEffects {
    /// With `innermost_composited_effect_by_effect_index`, only the filters inside the innermost composited effect at or
    /// above each effect, which a raster leaves out with the effects around it. With `at_some_sample`, the filters of
    /// every sample of the animations of the tree: a filter that may spread at some sample spreads, and an effect whose
    /// filter an animation animates holds a filter.
    pub fn of(
        tree: &VisualContextTree,
        innermost_composited_effect_by_effect_index: Option<&[EffectNodeIndex]>,
        at_some_sample: bool,
    ) -> Self {
        let effect_count = tree.effect_nodes.len();
        let mut nearest_spreading_effect_by_effect_index = vec![EffectNodeIndex::NONE; effect_count];
        let mut outermost_effect_with_filter_by_effect_index = vec![EffectNodeIndex::NONE; effect_count];
        tree.visit_effect_nodes_parents_first(|index| {
            let node = &tree.effect_nodes[index];
            let effect = EffectNodeIndex(index as u32);
            let innermost_composited_effect = innermost_composited_effect_by_effect_index
                .map_or(EffectNodeIndex::NONE, |innermost_composited_effects| {
                    innermost_composited_effects[index]
                });
            if innermost_composited_effect == effect {
                return;
            }
            let parent_is_inside_same_composited_effect =
                !node.parent.is_none() && node.parent != innermost_composited_effect;
            let value_of_parent = |values: &[EffectNodeIndex]| {
                if parent_is_inside_same_composited_effect {
                    values[node.parent.0 as usize]
                } else {
                    EffectNodeIndex::NONE
                }
            };
            let spreads = if at_some_sample {
                filter_may_spread_at_some_sample(tree, effect)
            } else {
                spreading_filter_of(node).is_some()
            };
            let holds_filter = matches!(&node.data, EffectNodeData::Effects(effects)
                if effects.filter.is_some() || effects.backdrop_filter.is_some())
                || (at_some_sample
                    && matches!(node.data, EffectNodeData::Effects(_))
                    && tree.effect_filter_is_animated(effect));
            nearest_spreading_effect_by_effect_index[index] = if spreads {
                effect
            } else {
                value_of_parent(&nearest_spreading_effect_by_effect_index)
            };
            let outermost_effect_with_filter_above = value_of_parent(&outermost_effect_with_filter_by_effect_index);
            outermost_effect_with_filter_by_effect_index[index] = if !outermost_effect_with_filter_above.is_none() {
                outermost_effect_with_filter_above
            } else if holds_filter {
                effect
            } else {
                EffectNodeIndex::NONE
            };
        });
        let outermost_effect_with_filter_if_spread_by_effect_index = outermost_effect_with_filter_by_effect_index
            .into_iter()
            .zip(&nearest_spreading_effect_by_effect_index)
            .map(|(outermost, nearest_spreading)| {
                if nearest_spreading.is_none() {
                    EffectNodeIndex::NONE
                } else {
                    outermost
                }
            })
            .collect();
        Self {
            nearest_spreading_effect_by_effect_index,
            outermost_effect_with_filter_if_spread_by_effect_index,
        }
    }

    pub(crate) fn nearest_spreading_effect_at_or_above(&self, effect: EffectNodeIndex) -> EffectNodeIndex {
        self.nearest_spreading_effect_by_effect_index
            .get(effect.0 as usize)
            .copied()
            .unwrap_or(EffectNodeIndex::NONE)
    }

    /// The effect whose output clip bounds how far the filters at and above `effect` spread its content, if any does.
    pub(crate) fn effect_bounding_spread_of(&self, effect: EffectNodeIndex) -> Option<EffectNodeIndex> {
        self.outermost_effect_with_filter_if_spread_by_effect_index
            .get(effect.0 as usize)
            .copied()
            .filter(|effect| !effect.is_none())
    }
}

/// Where a change to content drawn within `rect` in `space` under `effect` can show once every filter of `filters` above
/// it has spread it, in the space of the outermost of those filters. Changes to separate parts of the content can be
/// followed one at a time and united.
pub fn changed_extent_through_filters(
    tree: &VisualContextTree,
    filters: &FiltersOfEffects,
    space: SpatialNodeIndex,
    rect: FloatRect,
    effect: EffectNodeIndex,
    world: &WorldTransforms,
) -> (SpatialNodeIndex, Extent) {
    let mut space = space;
    let mut extent = Extent::Within(rect);
    let mut effect = filters.nearest_spreading_effect_at_or_above(effect);
    while !effect.is_none() && extent != Extent::Anywhere {
        let node = &tree.effect_nodes[effect.0 as usize];
        extent =
            output_extent_of_content_change(node, extent_mapped_between_spaces(extent, world, space, node.spatial));
        space = node.spatial;
        effect = filters.nearest_spreading_effect_at_or_above(node.parent);
    }
    (space, extent)
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EffectGeometry {
    /// Everything composited into the effect: its runs' ink and its child effects' output.
    pub content: Extent,
    /// The content once the effect's filter has spread it.
    pub output: Extent,
}

pub fn compute_effect_geometry<'a>(
    tree: &VisualContextTree,
    command_runs: impl IntoIterator<Item = &'a DisplayListCommandRun>,
    world: &WorldTransforms,
    geometry: &mut Vec<EffectGeometry>,
) {
    geometry.clear();
    geometry.resize(
        tree.effect_nodes.len(),
        EffectGeometry {
            content: Extent::Nothing,
            output: Extent::Nothing,
        },
    );

    let mut ink_of_consecutive_runs_in_one_effect_and_space: Option<(DisplayListCommandRun, FloatRect)> = None;
    let add_ink_to_effect_content =
        |geometry: &mut Vec<EffectGeometry>, run: &DisplayListCommandRun, ink: FloatRect| {
            let node = &tree.effect_nodes[run.context.effect.0 as usize];
            let ink_in_effect_space =
                extent_mapped_between_spaces(Extent::Within(ink), world, run.context.spatial, node.spatial);
            let content = &mut geometry[run.context.effect.0 as usize].content;
            *content = content.united(ink_in_effect_space);
        };
    for run in command_runs {
        if run.ink_bounds.is_empty() || run.context.effect.is_none() {
            continue;
        }
        let ink = run.ink_bounds.to_float();
        match &mut ink_of_consecutive_runs_in_one_effect_and_space {
            Some((first_run, united_ink))
                if first_run.context.effect == run.context.effect
                    && first_run.context.spatial == run.context.spatial =>
            {
                *united_ink = united_ink.united(ink);
            }
            _ => {
                if let Some((first_run, united_ink)) =
                    ink_of_consecutive_runs_in_one_effect_and_space.replace((*run, ink))
                {
                    add_ink_to_effect_content(geometry, &first_run, united_ink);
                }
            }
        }
    }
    if let Some((first_run, united_ink)) = ink_of_consecutive_runs_in_one_effect_and_space {
        add_ink_to_effect_content(geometry, &first_run, united_ink);
    }

    let mut effects_parents_first = Vec::with_capacity(tree.effect_nodes.len());
    tree.visit_effect_nodes_parents_first(|index| effects_parents_first.push(index));
    for &index in effects_parents_first.iter().rev() {
        let node = &tree.effect_nodes[index];
        let output = effect_output_extent(node, geometry[index].content);
        geometry[index].output = output;
        if node.parent.is_none() {
            continue;
        }
        let parent_spatial = tree.effect_nodes[node.parent.0 as usize].spatial;
        let output_in_parent_space = extent_mapped_between_spaces(output, world, node.spatial, parent_spatial);
        let parent_content = &mut geometry[node.parent.0 as usize].content;
        *parent_content = parent_content.united(output_in_parent_space);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::commands::{ClipNodeIndex, ContextRef, EffectNodeIndex, VISUAL_VIEWPORT_NODE_INDEX};
    use crate::visual_context::{EffectsData, SpatialData, TransformData, TransformDataRole};
    use libgfx_rust::filter::Filter;
    use libgfx_rust::{
        Color, ColorFilterType, CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, IntRect, rotation_matrix,
        scale_matrix, translation_matrix,
    };
    use std::sync::Arc;

    fn transform(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        }
    }

    fn identity_tree() -> VisualContextTree {
        VisualContextTree::create(transform(FloatMatrix4x4::identity()))
    }

    fn effect(
        tree: &mut VisualContextTree,
        filter: Option<Filter>,
        parent: EffectNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> EffectNodeIndex {
        tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: filter.map(|filter| Arc::new(filter.serialize())),
                backdrop_filter: None,
            }),
            parent,
            spatial,
            ClipNodeIndex::NONE,
        )
    }

    fn run(spatial: SpatialNodeIndex, effect: EffectNodeIndex, ink_bounds: IntRect) -> DisplayListCommandRun {
        DisplayListCommandRun {
            offset: 0,
            size: 0,
            context: ContextRef {
                spatial,
                clip: ClipNodeIndex::NONE,
                effect,
            },
            ink_bounds,
            has_compositor_metadata: false,
        }
    }

    fn geometry_of(tree: &VisualContextTree, runs: &[DisplayListCommandRun]) -> Vec<EffectGeometry> {
        let mut geometry = Vec::new();
        let world = WorldTransforms::new(tree, &[], Some(FloatMatrix4x4::identity()));
        compute_effect_geometry(tree, runs, &world, &mut geometry);
        geometry
    }

    fn within(x: f32, y: f32, width: f32, height: f32) -> Extent {
        Extent::Within(FloatRect::new(x, y, width, height))
    }

    #[test]
    fn an_effect_holds_its_runs_and_a_blur_spreads_them_into_its_output() {
        let mut tree = identity_tree();
        let blur = effect(
            &mut tree,
            Some(Filter::blur(2.0, 2.0, None)),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, blur, IntRect::new(10, 20, 30, 40)),
            run(VISUAL_VIEWPORT_NODE_INDEX, blur, IntRect::new(50, 20, 10, 10)),
        ];
        assert_eq!(
            geometry_of(&tree, &runs)[blur.0 as usize],
            EffectGeometry {
                content: within(10.0, 20.0, 50.0, 40.0),
                output: within(4.0, 14.0, 62.0, 52.0),
            }
        );
    }

    #[test]
    fn runs_in_transformed_spaces_are_mapped_into_the_effect_space() {
        let mut tree = identity_tree();
        let scaled = tree.append_spatial(
            SpatialData::Transform(transform(
                translation_matrix(5.0, 0.0, 0.0).multiplied(scale_matrix(2.0, 2.0, 1.0)),
            )),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let layer = effect(&mut tree, None, EffectNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let runs = [run(scaled, layer, IntRect::new(10, 20, 30, 40))];
        assert_eq!(
            geometry_of(&tree, &runs)[layer.0 as usize].content,
            within(25.0, 40.0, 60.0, 80.0)
        );
    }

    #[test]
    fn a_child_effect_contributes_its_output_to_its_parent() {
        let mut tree = identity_tree();
        let outer = effect(
            &mut tree,
            Some(Filter::color(ColorFilterType::Invert, 1.0, None)),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let inner = effect(
            &mut tree,
            Some(Filter::drop_shadow(
                0.0,
                10.0,
                0.0,
                Color::from_rgba(0, 0, 0, 0xff),
                None,
            )),
            outer,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, inner, IntRect::new(10, 20, 30, 40)),
            run(VISUAL_VIEWPORT_NODE_INDEX, outer, IntRect::new(0, 0, 5, 5)),
        ];
        let geometry = geometry_of(&tree, &runs);
        assert_eq!(geometry[inner.0 as usize].output, within(10.0, 20.0, 30.0, 50.0));
        assert_eq!(
            geometry[outer.0 as usize],
            EffectGeometry {
                content: within(0.0, 0.0, 40.0, 70.0),
                output: within(0.0, 0.0, 40.0, 70.0),
            }
        );
    }

    #[test]
    fn content_is_spread_by_every_filter_above_it_and_skips_effects_without_one() {
        let mut tree = identity_tree();
        let scaled = tree.append_spatial(
            SpatialData::Transform(transform(scale_matrix(2.0, 2.0, 1.0))),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let blur = effect(
            &mut tree,
            Some(Filter::blur(10.0, 10.0, None)),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let opacity = effect(&mut tree, None, blur, scaled);
        let shadow = effect(
            &mut tree,
            Some(Filter::drop_shadow(
                0.0,
                10.0,
                0.0,
                Color::from_rgba(0, 0, 0, 0xff),
                None,
            )),
            opacity,
            scaled,
        );
        let plain = effect(&mut tree, None, EffectNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let filters = FiltersOfEffects::of(&tree, None, false);
        assert_eq!(filters.nearest_spreading_effect_at_or_above(opacity), blur);
        assert_eq!(
            filters.nearest_spreading_effect_at_or_above(plain),
            EffectNodeIndex::NONE
        );
        let content = FloatRect::new(10.0, 20.0, 30.0, 40.0);
        let world = WorldTransforms::new(&tree, &[], Some(FloatMatrix4x4::identity()));
        assert_eq!(
            changed_extent_through_filters(&tree, &filters, scaled, content, shadow, &world),
            (VISUAL_VIEWPORT_NODE_INDEX, within(-10.0, 10.0, 120.0, 160.0))
        );
        assert_eq!(
            changed_extent_through_filters(&tree, &filters, scaled, content, plain, &world),
            (scaled, Extent::Within(content))
        );
    }

    #[test]
    fn unbounded_filters_and_three_dimensional_spaces_reach_anywhere() {
        let mut tree = identity_tree();
        let outer = effect(&mut tree, None, EffectNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let flood = effect(
            &mut tree,
            Some(Filter::Flood {
                color: Color::from_rgba(0, 0, 0, 0xff),
                opacity: 1.0,
            }),
            outer,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let tilted = tree.append_spatial(
            SpatialData::Transform(transform(rotation_matrix([0.0, 1.0, 0.0], 0.5))),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let flat = effect(&mut tree, None, EffectNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let unused = effect(&mut tree, None, EffectNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, flood, IntRect::new(0, 0, 10, 10)),
            run(tilted, flat, IntRect::new(0, 0, 10, 10)),
        ];
        let geometry = geometry_of(&tree, &runs);
        assert_eq!(geometry[flood.0 as usize].content, within(0.0, 0.0, 10.0, 10.0));
        assert_eq!(geometry[flood.0 as usize].output, Extent::Anywhere);
        assert_eq!(geometry[outer.0 as usize].output, Extent::Anywhere);
        assert_eq!(geometry[flat.0 as usize].content, Extent::Anywhere);
        assert_eq!(geometry[unused.0 as usize].output, Extent::Nothing);
    }
}
