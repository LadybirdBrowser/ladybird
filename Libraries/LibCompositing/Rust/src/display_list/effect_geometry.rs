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

use crate::display_list::commands::{DisplayListCommandRun, EffectNodeIndex, SpatialNodeIndex};
use crate::filter_bytes;
use crate::visual_context::{EffectNode, EffectNodeData, VisualContextTree};
use libgfx_rust::{FloatMatrix4x4, FloatRect, multiply_affine};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Extent {
    Nothing,
    Within(FloatRect),
    /// Reaches anywhere, through a filter that paints everywhere or through a space that does not
    /// map onto the effect's as a two-dimensional transform.
    Anywhere,
}

impl Extent {
    pub fn united(self, other: Self) -> Self {
        match (self, other) {
            (Self::Nothing, extent) | (extent, Self::Nothing) => extent,
            (Self::Within(rect), Self::Within(other)) => Self::Within(rect.united(other)),
            _ => Self::Anywhere,
        }
    }

    fn mapped_between_spaces(
        self,
        spaces: &mut impl SpaceMatrices,
        from: SpatialNodeIndex,
        to: SpatialNodeIndex,
    ) -> Self {
        match self {
            Self::Within(rect) => map_rect_between_spaces(spaces, rect, from, to).map_or(Self::Anywhere, Self::Within),
            extent => extent,
        }
    }
}

pub trait SpaceMatrices {
    /// The matrix that takes coordinates in `space` to the root's.
    fn to_root_matrix(&mut self, space: SpatialNodeIndex) -> FloatMatrix4x4;
}

/// Maps `rect` from `from`'s space into `to`'s, or gives `None` when either space is not a
/// two-dimensional transform of the root or `to`'s cannot be inverted.
pub fn map_rect_between_spaces(
    spaces: &mut impl SpaceMatrices,
    rect: FloatRect,
    from: SpatialNodeIndex,
    to: SpatialNodeIndex,
) -> Option<FloatRect> {
    if from == to {
        return Some(rect);
    }
    let from_matrix = spaces.to_root_matrix(from);
    let to_matrix = spaces.to_root_matrix(to);
    if !from_matrix.is_2d_affine() || !to_matrix.is_2d_affine() {
        return None;
    }
    let root_to_target = to_matrix.extract_2d_affine().inverse()?;
    Some(multiply_affine(root_to_target, from_matrix.extract_2d_affine()).map_rect(rect))
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

// Where an effect's filter can show a change to its content, given that change in the effect's space.
fn changed_output_extent(node: &EffectNode, change: Extent) -> Extent {
    let Extent::Within(rect) = change else {
        return change;
    };
    let EffectNodeData::Effects(effects) = &node.data else {
        return change;
    };
    let Some(filter) = effects.filter.as_deref() else {
        return change;
    };
    filter_bytes::changed_output_bounds(filter, rect).map_or(Extent::Anywhere, Extent::Within)
}

/// Per effect, the nearest effect at or above it whose filter can spread content outside its
/// bounds, or `EffectNodeIndex::NONE`.
pub fn nearest_spreading_effects(tree: &VisualContextTree) -> Vec<EffectNodeIndex> {
    let mut nearest = vec![EffectNodeIndex::NONE; tree.effect_nodes.len()];
    tree.visit_effect_nodes_parents_first(|index| {
        let node = &tree.effect_nodes[index];
        let spreads = matches!(&node.data, EffectNodeData::Effects(effects)
            if effects.filter.as_deref().is_some_and(|filter| filter_bytes::may_affect_output_bounds(filter)));
        nearest[index] = if spreads {
            EffectNodeIndex(index as u32)
        } else if node.parent.is_none() {
            EffectNodeIndex::NONE
        } else {
            nearest[node.parent.0 as usize]
        };
    });
    nearest
}

/// Where a change to content drawn within `rect` in `space` under `effect` can show once every
/// filter above it has spread it, in the space of the outermost of those filters. Changes to
/// separate parts of the content can be followed one at a time and united.
pub fn changed_extent_through_filters(
    tree: &VisualContextTree,
    nearest_spreading_effects: &[EffectNodeIndex],
    space: SpatialNodeIndex,
    rect: FloatRect,
    effect: EffectNodeIndex,
    spaces: &mut impl SpaceMatrices,
) -> (SpatialNodeIndex, Extent) {
    let nearest_at_or_above = |effect: EffectNodeIndex| {
        if effect.is_none() {
            EffectNodeIndex::NONE
        } else {
            nearest_spreading_effects[effect.0 as usize]
        }
    };
    let mut space = space;
    let mut extent = Extent::Within(rect);
    let mut effect = nearest_at_or_above(effect);
    while !effect.is_none() && extent != Extent::Anywhere {
        let node = &tree.effect_nodes[effect.0 as usize];
        extent = changed_output_extent(node, extent.mapped_between_spaces(spaces, space, node.spatial));
        space = node.spatial;
        effect = nearest_at_or_above(node.parent);
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

/// Fills `geometry` with every effect's geometry, indexed by effect.
pub fn compute_effect_geometry(
    tree: &VisualContextTree,
    command_runs: &[DisplayListCommandRun],
    spaces: &mut impl SpaceMatrices,
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

    // Neighboring runs mostly differ only in their clip, so their ink is united before it is
    // mapped into the effect's space.
    let mut pending: Option<(DisplayListCommandRun, FloatRect)> = None;
    let mut add_ink = |geometry: &mut Vec<EffectGeometry>, run: &DisplayListCommandRun, ink: FloatRect| {
        let node = &tree.effect_nodes[run.context.effect.0 as usize];
        let landed = Extent::Within(ink).mapped_between_spaces(spaces, run.context.spatial, node.spatial);
        let content = &mut geometry[run.context.effect.0 as usize].content;
        *content = content.united(landed);
    };
    for run in command_runs {
        if run.ink_bounds.is_empty() || run.context.effect.is_none() {
            continue;
        }
        let ink = run.ink_bounds.to_float();
        match &mut pending {
            Some((pending_run, pending_ink))
                if pending_run.context.effect == run.context.effect
                    && pending_run.context.spatial == run.context.spatial =>
            {
                *pending_ink = pending_ink.united(ink);
            }
            _ => {
                if let Some((pending_run, pending_ink)) = pending.replace((*run, ink)) {
                    add_ink(geometry, &pending_run, pending_ink);
                }
            }
        }
    }
    if let Some((pending_run, pending_ink)) = pending {
        add_ink(geometry, &pending_run, pending_ink);
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
        let landed = output.mapped_between_spaces(spaces, node.spatial, parent_spatial);
        let parent_content = &mut geometry[node.parent.0 as usize].content;
        *parent_content = parent_content.united(landed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::commands::{ClipNodeIndex, ContextRef, EffectNodeIndex, VISUAL_VIEWPORT_NODE_INDEX};
    use crate::visual_context::{
        EffectsData, IncludeVisualViewportTransform, SpatialData, TransformData, TransformDataRole,
    };
    use libgfx_rust::filter::Filter;
    use libgfx_rust::{
        Color, ColorFilterType, CompositingAndBlendingOperator, FloatPoint, IntRect, rotation_matrix, scale_matrix,
        translation_matrix,
    };
    use std::sync::Arc;

    struct TreeSpaces<'a>(&'a VisualContextTree);

    impl SpaceMatrices for TreeSpaces<'_> {
        fn to_root_matrix(&mut self, space: SpatialNodeIndex) -> FloatMatrix4x4 {
            self.0
                .accumulated_matrix(space, &[], IncludeVisualViewportTransform::Yes)
        }
    }

    fn transform(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
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
        compute_effect_geometry(tree, runs, &mut TreeSpaces(tree), &mut geometry);
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
        let nearest = nearest_spreading_effects(&tree);
        assert_eq!(nearest[opacity.0 as usize], blur);
        assert_eq!(nearest[plain.0 as usize], EffectNodeIndex::NONE);
        let content = FloatRect::new(10.0, 20.0, 30.0, 40.0);
        assert_eq!(
            changed_extent_through_filters(&tree, &nearest, scaled, content, shadow, &mut TreeSpaces(&tree)),
            (VISUAL_VIEWPORT_NODE_INDEX, within(-10.0, 10.0, 120.0, 160.0))
        );
        assert_eq!(
            changed_extent_through_filters(&tree, &nearest, scaled, content, plain, &mut TreeSpaces(&tree)),
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
