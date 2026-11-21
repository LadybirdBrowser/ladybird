/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libgfx_rust::bsp_tree::{
    BspPolygon, map_polygon_through_projection, map_rect_through_projection, polygon_crosses_eye_plane,
    split_and_sort_polygons_back_to_front,
};
use libgfx_rust::{FloatMatrix4x4, FloatRect, FloatVector3};

use super::commands::{DisplayListCommandRun, SpatialNodeIndex};
use crate::visual_context::queries::TreeCullingScratch;
use crate::visual_context::{NO_SORTING_CONTEXT, SortingContexts};

struct LeafBounds {
    leaf: SpatialNodeIndex,
    bounds: FloatRect,
    unbounded: bool,
}

struct PlaneRunSpan {
    first_run: u32,
    run_count: u32,
    leaf: SpatialNodeIndex,
    context: SpatialNodeIndex,
    bounds_in_leaf_of_each_enclosing_context: Vec<LeafBounds>,
}

#[derive(Clone, Copy)]
struct RunSpanPlacement {
    child_context: SpatialNodeIndex,
    leaf: SpatialNodeIndex,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DepthSortedReplayStepKind {
    RunSpan,
    PushPlaneClip,
    PopPlaneClip,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct DepthSortedReplayStep {
    pub kind: DepthSortedReplayStepKind,
    pub first_run: u32,
    pub run_count: u32,
    pub vertex_offset: u32,
    pub vertex_count: u32,
}

pub struct DepthSortedReplayPlan {
    pub steps: Vec<DepthSortedReplayStep>,
    pub vertices: Vec<FloatVector3>,
}

struct DepthSortedPlanBuilder<'a> {
    contexts: &'a SortingContexts,
    transform_palette: &'a [FloatMatrix4x4],
    leaf_to_context_palette: &'a [FloatMatrix4x4],
    steps: Vec<DepthSortedReplayStep>,
    vertices: Vec<FloatVector3>,
}

impl DepthSortedPlanBuilder<'_> {
    fn append_run_span(&mut self, run_span: &PlaneRunSpan) {
        self.steps.push(DepthSortedReplayStep {
            kind: DepthSortedReplayStepKind::RunSpan,
            first_run: run_span.first_run,
            run_count: run_span.run_count,
            vertex_offset: 0,
            vertex_count: 0,
        });
    }

    fn append_plane_clip(&mut self, vertices: Vec<FloatVector3>) {
        let vertex_offset = self.vertices.len() as u32;
        let vertex_count = vertices.len() as u32;
        self.vertices.extend(vertices);
        self.steps.push(DepthSortedReplayStep {
            kind: DepthSortedReplayStepKind::PushPlaneClip,
            first_run: 0,
            run_count: 0,
            vertex_offset,
            vertex_count,
        });
    }

    fn append_pop_plane_clip(&mut self) {
        self.steps.push(DepthSortedReplayStep {
            kind: DepthSortedReplayStepKind::PopPlaneClip,
            first_run: 0,
            run_count: 0,
            vertex_offset: 0,
            vertex_count: 0,
        });
    }

    fn emit_run_spans(&mut self, run_spans: &[PlaneRunSpan], enclosing_context: SpatialNodeIndex) {
        let mut index = 0;
        while index < run_spans.len() {
            let child_context =
                place_run_span_within(&run_spans[index], enclosing_context, self.contexts).child_context;
            if child_context == NO_SORTING_CONTEXT {
                self.append_run_span(&run_spans[index]);
                index += 1;
                continue;
            }
            let mut end_of_spans_in_child_context = index + 1;
            while end_of_spans_in_child_context < run_spans.len()
                && place_run_span_within(
                    &run_spans[end_of_spans_in_child_context],
                    enclosing_context,
                    self.contexts,
                )
                .child_context
                    == child_context
            {
                end_of_spans_in_child_context += 1;
            }
            self.sort_and_emit_context(&run_spans[index..end_of_spans_in_child_context], child_context);
            index = end_of_spans_in_child_context;
        }
    }

    fn sort_and_emit_context(&mut self, run_spans: &[PlaneRunSpan], sorting_context: SpatialNodeIndex) {
        // Not whole planes: separate spans of one plane interleaved with a coplanar sibling keep their recorded
        // positions relative to it, as coplanar planes render in painting order.
        struct ConsecutiveSpansOfOnePlane {
            leaf: SpatialNodeIndex,
            begin: usize,
            end: usize,
            bounds: FloatRect,
            unbounded: bool,
        }

        let mut sorting_units: Vec<ConsecutiveSpansOfOnePlane> = Vec::new();
        for (index, run_span) in run_spans.iter().enumerate() {
            let leaf = place_run_span_within(run_span, sorting_context, self.contexts).leaf;
            if sorting_units.last().is_none_or(|unit| unit.leaf != leaf) {
                sorting_units.push(ConsecutiveSpansOfOnePlane {
                    leaf,
                    begin: index,
                    end: index + 1,
                    bounds: FloatRect::default(),
                    unbounded: false,
                });
            } else {
                sorting_units.last_mut().unwrap().end = index + 1;
            }
            for leaf_bounds in &run_span.bounds_in_leaf_of_each_enclosing_context {
                if leaf_bounds.leaf == leaf {
                    let unit = sorting_units.last_mut().unwrap();
                    unit.bounds = unit.bounds.united(leaf_bounds.bounds);
                    unit.unbounded |= leaf_bounds.unbounded;
                }
            }
        }

        if sorting_units.iter().any(|unit| unit.unbounded) {
            self.emit_run_spans(run_spans, sorting_context);
            return;
        }

        let context_to_device = self.transform_palette[sorting_context.0 as usize];
        let Some(device_to_context) = context_to_device.inverse() else {
            self.emit_run_spans(run_spans, sorting_context);
            return;
        };
        let mut eye = device_to_context.map_vector4([0.0, 0.0, 1.0, 0.0]);
        if eye[3] < 0.0 {
            eye = [-eye[0], -eye[1], -eye[2], -eye[3]];
        }

        const MARGIN_KEEPING_SPLIT_CLIPS_CLEAR_OF_ANTIALIASED_EDGES: f32 = 4.0;
        let mut polygons = Vec::new();
        for (unit_index, unit) in sorting_units.iter().enumerate() {
            if unit.bounds.is_empty() {
                continue;
            }
            let leaf_to_context = self.leaf_to_context_palette[unit.leaf.0 as usize];
            let vertices = map_rect_through_projection(
                leaf_to_context,
                unit.bounds.inflated(
                    MARGIN_KEEPING_SPLIT_CLIPS_CLEAR_OF_ANTIALIASED_EDGES,
                    MARGIN_KEEPING_SPLIT_CLIPS_CLEAR_OF_ANTIALIASED_EDGES,
                ),
            );
            let lies_behind_eye_in_context = vertices.len() < 3;
            if lies_behind_eye_in_context {
                continue;
            }
            let lies_behind_eye_on_device = map_polygon_through_projection(context_to_device, &vertices).len() < 3;
            if lies_behind_eye_on_device {
                continue;
            }
            polygons.push(BspPolygon {
                vertices,
                plane_index: unit_index,
                clipped: false,
            });
        }

        // FIXME: Pieces of a split plane whose content carries filter effects render incorrectly: each piece filters its
        //        clipped content independently, truncating filter output at the piece boundary and seaming it along the cut.
        for polygon in split_and_sort_polygons_back_to_front(polygons, eye, context_to_device) {
            let unit = &sorting_units[polygon.plane_index];
            if polygon.clipped || polygon_crosses_eye_plane(context_to_device, &polygon.vertices) {
                let clip_vertices = map_polygon_through_projection(context_to_device, &polygon.vertices);
                if clip_vertices.len() < 3 {
                    continue;
                }
                self.append_plane_clip(clip_vertices);
                self.emit_run_spans(&run_spans[unit.begin..unit.end], sorting_context);
                self.append_pop_plane_clip();
            } else {
                self.emit_run_spans(&run_spans[unit.begin..unit.end], sorting_context);
            }
        }
    }
}

#[derive(Clone, Copy)]
struct LeafMapping {
    leaf: SpatialNodeIndex,
    to_leaf: Option<FloatMatrix4x4>,
    unbounded: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct LeafMappingsKey {
    leaf: SpatialNodeIndex,
    context: SpatialNodeIndex,
    spatial_node: SpatialNodeIndex,
}

fn ensure_mappings_to_enclosing_leaves(
    key: LeafMappingsKey,
    mappings_key: &mut Option<LeafMappingsKey>,
    mappings: &mut Vec<LeafMapping>,
    contexts: &SortingContexts,
    transform_palette: &[FloatMatrix4x4],
) {
    if *mappings_key == Some(key) {
        return;
    }
    *mappings_key = Some(key);
    mappings.clear();
    let mut leaf = key.leaf;
    let mut sorting_context = key.context;
    while sorting_context != NO_SORTING_CONTEXT {
        if key.spatial_node == leaf {
            mappings.push(LeafMapping {
                leaf,
                to_leaf: None,
                unbounded: false,
            });
        } else if let Some(inverse) = transform_palette[leaf.0 as usize].inverse() {
            mappings.push(LeafMapping {
                leaf,
                to_leaf: Some(inverse.multiplied(transform_palette[key.spatial_node.0 as usize])),
                unbounded: false,
            });
        } else {
            mappings.push(LeafMapping {
                leaf,
                to_leaf: None,
                unbounded: true,
            });
        }
        let Some(link) = contexts.links.get(&sorting_context.0) else {
            break;
        };
        leaf = link.parent_leaf;
        sorting_context = link.parent_context;
    }
}

fn bounds_of_mapped_rect(matrix: FloatMatrix4x4, rect: FloatRect) -> FloatRect {
    let mut vertices = map_rect_through_projection(matrix, rect).into_iter();
    let Some(first) = vertices.next() else {
        return FloatRect::default();
    };
    let mut min_x = first.x;
    let mut min_y = first.y;
    let mut max_x = first.x;
    let mut max_y = first.y;
    for vertex in vertices {
        if vertex.x < min_x {
            min_x = vertex.x;
        }
        if vertex.y < min_y {
            min_y = vertex.y;
        }
        if max_x < vertex.x {
            max_x = vertex.x;
        }
        if max_y < vertex.y {
            max_y = vertex.y;
        }
    }
    FloatRect::new(min_x, min_y, max_x - min_x, max_y - min_y)
}

fn partition_command_runs_into_plane_run_spans<'a>(
    command_runs: impl IntoIterator<Item = &'a DisplayListCommandRun>,
    contexts: &SortingContexts,
    transform_palette: &[FloatMatrix4x4],
    draw_space: &[SpatialNodeIndex],
    backface_culled: &[bool],
    culling: &TreeCullingScratch,
) -> Vec<PlaneRunSpan> {
    let mut mappings = Vec::new();
    let mut mappings_key = None;

    // Ink bounds leave out clips, so mapping their union once per enclosing context only widens a plane's bounds.
    let mut run_spans: Vec<PlaneRunSpan> = Vec::new();
    for (run_index, run) in command_runs.into_iter().enumerate() {
        let spatial = run.context.spatial;
        let leaf = contexts.leaf_by_node[spatial.0 as usize];
        let sorting_context = contexts.context_by_node[spatial.0 as usize];
        if run_spans
            .last()
            .is_none_or(|run_span| run_span.leaf != leaf || run_span.context != sorting_context)
        {
            run_spans.push(PlaneRunSpan {
                first_run: run_index as u32,
                run_count: 1,
                leaf,
                context: sorting_context,
                bounds_in_leaf_of_each_enclosing_context: Vec::new(),
            });
        } else {
            run_spans.last_mut().unwrap().run_count += 1;
        }

        if run.ink_bounds.is_empty() || sorting_context == NO_SORTING_CONTEXT || backface_culled[spatial.0 as usize] {
            continue;
        }
        if culling.context_culls_everything(run.context) {
            continue;
        }
        ensure_mappings_to_enclosing_leaves(
            LeafMappingsKey {
                leaf,
                context: sorting_context,
                spatial_node: draw_space[spatial.0 as usize],
            },
            &mut mappings_key,
            &mut mappings,
            contexts,
            transform_palette,
        );
        let rect = run.ink_bounds.to_float();
        let bounds_in_enclosing_leaves = &mut run_spans.last_mut().unwrap().bounds_in_leaf_of_each_enclosing_context;
        for mapping in &mappings {
            let existing_index = bounds_in_enclosing_leaves
                .iter()
                .position(|leaf_bounds| leaf_bounds.leaf == mapping.leaf);
            let leaf_bounds = if let Some(index) = existing_index {
                &mut bounds_in_enclosing_leaves[index]
            } else {
                bounds_in_enclosing_leaves.push(LeafBounds {
                    leaf: mapping.leaf,
                    bounds: FloatRect::default(),
                    unbounded: false,
                });
                bounds_in_enclosing_leaves.last_mut().unwrap()
            };
            if mapping.unbounded {
                leaf_bounds.unbounded = true;
            } else if let Some(to_leaf) = mapping.to_leaf {
                leaf_bounds.bounds = leaf_bounds.bounds.united(bounds_of_mapped_rect(to_leaf, rect));
            } else {
                leaf_bounds.bounds = leaf_bounds.bounds.united(rect);
            }
        }
    }
    run_spans
}

fn place_run_span_within(
    run_span: &PlaneRunSpan,
    enclosing_context: SpatialNodeIndex,
    contexts: &SortingContexts,
) -> RunSpanPlacement {
    if run_span.context == enclosing_context {
        return RunSpanPlacement {
            child_context: NO_SORTING_CONTEXT,
            leaf: run_span.leaf,
        };
    }
    let mut current = run_span.context;
    while current != NO_SORTING_CONTEXT {
        let Some(link) = contexts.links.get(&current.0) else {
            break;
        };
        if link.parent_context == enclosing_context {
            return RunSpanPlacement {
                child_context: current,
                leaf: link.parent_leaf,
            };
        }
        current = link.parent_context;
    }
    RunSpanPlacement {
        child_context: NO_SORTING_CONTEXT,
        leaf: run_span.leaf,
    }
}

// https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
// The element establishing the 3D rendering context, and each other 3D transformed element participating in the
// 3D rendering context, is rendered into its own plane. Intersection is performed between this set of planes,
// according to Newell's algorithm, with the planes transformed by the accumulated 3D transformation matrix.
// Coplanar 3D transformed elements are rendered in painting order.
//
// The command runs are partitioned into contiguous run spans that share a plane. Each 3D rendering context's
// planes are ordered back to front with a BSP tree over their content bounds; a plane cut by another's plane is
// replayed once per piece under a device-space polygon clip. Run spans in a nested context sort among themselves
// inside the plane of the outer context they render into.
pub fn build_depth_sorted_replay_plan<'a>(
    command_runs: impl IntoIterator<Item = &'a DisplayListCommandRun>,
    contexts: &SortingContexts,
    transform_palette: &[FloatMatrix4x4],
    leaf_to_context_palette: &[FloatMatrix4x4],
    draw_space: &[SpatialNodeIndex],
    backface_culled: &[bool],
    culling: &TreeCullingScratch,
) -> DepthSortedReplayPlan {
    let run_spans = partition_command_runs_into_plane_run_spans(
        command_runs,
        contexts,
        transform_palette,
        draw_space,
        backface_culled,
        culling,
    );
    let mut builder = DepthSortedPlanBuilder {
        contexts,
        transform_palette,
        leaf_to_context_palette,
        steps: Vec::new(),
        vertices: Vec::new(),
    };
    builder.emit_run_spans(&run_spans, NO_SORTING_CONTEXT);
    DepthSortedReplayPlan {
        steps: builder.steps,
        vertices: builder.vertices,
    }
}

#[cfg(test)]
mod tests {
    use ak::math;
    use libgfx_rust::{FloatMatrix4x4, IntRect, scale_matrix, translation_matrix};

    use super::*;
    use crate::display_list::commands::ContextRef;
    use crate::visual_context::resolve_sorting_contexts_over_nodes;

    fn sorting_contexts(parents: &[u32], sorting_context_roots: &[Option<u32>]) -> SortingContexts {
        let index_order: Vec<u32> = (0..parents.len() as u32).collect();
        resolve_sorting_contexts_over_nodes(parents.len(), &index_order, |index| {
            (
                SpatialNodeIndex(parents[index]),
                sorting_context_roots[index].map(SpatialNodeIndex),
            )
        })
    }

    fn leaf_to_context_palette(contexts: &SortingContexts, palette: &[FloatMatrix4x4]) -> Vec<FloatMatrix4x4> {
        palette
            .iter()
            .enumerate()
            .map(|(index, matrix)| {
                let context = contexts.context_by_node[index];
                if context == NO_SORTING_CONTEXT {
                    return *matrix;
                }
                palette[context.0 as usize]
                    .inverse()
                    .unwrap_or(FloatMatrix4x4::identity())
                    .multiplied(*matrix)
            })
            .collect()
    }

    fn command_run(spatial: u32) -> DisplayListCommandRun {
        DisplayListCommandRun {
            context: ContextRef::spatial_only(SpatialNodeIndex(spatial)),
            ink_bounds: IntRect::new(-10, -10, 20, 20),
            ..Default::default()
        }
    }

    fn identity_inputs(node_count: usize) -> (Vec<FloatMatrix4x4>, Vec<SpatialNodeIndex>, Vec<bool>) {
        (
            vec![FloatMatrix4x4::identity(); node_count],
            (0..node_count).map(|index| SpatialNodeIndex(index as u32)).collect(),
            vec![false; node_count],
        )
    }

    fn run_spans(plan: &DepthSortedReplayPlan) -> Vec<(u32, u32)> {
        plan.steps
            .iter()
            .filter(|step| step.kind == DepthSortedReplayStepKind::RunSpan)
            .map(|step| (step.first_run, step.run_count))
            .collect()
    }

    fn rotate_y(radians: f32) -> FloatMatrix4x4 {
        let cosine = math::cos(radians);
        let sine = math::sin(radians);
        FloatMatrix4x4 {
            elements: [
                [cosine, 0.0, sine, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [-sine, 0.0, cosine, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        }
    }

    #[test]
    fn runs_outside_sorting_contexts_pass_through_in_merged_record_order() {
        let contexts = sorting_contexts(&[0, 0, 1], &[None, None, Some(1)]);
        let command_runs = [command_run(0), command_run(0)];
        let (palette, draw_space, backface_culled) = identity_inputs(3);
        let plan = build_depth_sorted_replay_plan(
            &command_runs,
            &contexts,
            &palette,
            &leaf_to_context_palette(&contexts, &palette),
            &draw_space,
            &backface_culled,
            &TreeCullingScratch::default(),
        );
        assert_eq!(run_spans(&plan), vec![(0, 2)]);
        assert!(plan.vertices.is_empty());
    }

    #[test]
    fn parallel_planes_emit_back_to_front_regardless_of_record_order() {
        let contexts = sorting_contexts(&[0, 0, 1, 1], &[None, None, Some(1), Some(1)]);
        let command_runs = [command_run(2), command_run(3)];
        let (mut palette, draw_space, backface_culled) = identity_inputs(4);
        palette[2] = translation_matrix(0.0, 0.0, 100.0);
        palette[3] = translation_matrix(0.0, 0.0, -100.0);
        let plan = build_depth_sorted_replay_plan(
            &command_runs,
            &contexts,
            &palette,
            &leaf_to_context_palette(&contexts, &palette),
            &draw_space,
            &backface_culled,
            &TreeCullingScratch::default(),
        );
        assert_eq!(run_spans(&plan), vec![(1, 1), (0, 1)]);
        assert!(
            plan.steps
                .iter()
                .all(|step| step.kind == DepthSortedReplayStepKind::RunSpan)
        );
    }

    #[test]
    fn intersecting_planes_emit_clips_for_split_pieces() {
        let contexts = sorting_contexts(&[0, 0, 1, 1], &[None, None, Some(1), Some(1)]);
        let command_runs = [command_run(2), command_run(3)];
        let (mut palette, draw_space, backface_culled) = identity_inputs(4);
        palette[3] = rotate_y(std::f32::consts::FRAC_PI_4);
        let plan = build_depth_sorted_replay_plan(
            &command_runs,
            &contexts,
            &palette,
            &leaf_to_context_palette(&contexts, &palette),
            &draw_space,
            &backface_culled,
            &TreeCullingScratch::default(),
        );
        let push_steps: Vec<_> = plan
            .steps
            .iter()
            .filter(|step| step.kind == DepthSortedReplayStepKind::PushPlaneClip)
            .collect();
        let pop_count = plan
            .steps
            .iter()
            .filter(|step| step.kind == DepthSortedReplayStepKind::PopPlaneClip)
            .count();
        assert!(!push_steps.is_empty());
        assert_eq!(push_steps.len(), pop_count);
        assert!(push_steps.iter().all(|step| {
            step.vertex_count >= 3 && step.vertex_offset as usize + step.vertex_count as usize <= plan.vertices.len()
        }));
    }

    #[test]
    fn a_plane_crossing_the_eye_plane_emits_a_clip_without_splitting() {
        let contexts = sorting_contexts(&[0, 0, 1], &[None, None, Some(1)]);
        let command_runs = [command_run(2)];
        let (mut palette, draw_space, backface_culled) = identity_inputs(3);
        let mut perspective = FloatMatrix4x4::identity();
        perspective.elements[3][2] = -0.0025;
        palette[1] = perspective;
        palette[2] =
            perspective.multiplied(rotate_y(std::f32::consts::FRAC_PI_2).multiplied(scale_matrix(100.0, 100.0, 100.0)));
        let plan = build_depth_sorted_replay_plan(
            &command_runs,
            &contexts,
            &palette,
            &leaf_to_context_palette(&contexts, &palette),
            &draw_space,
            &backface_culled,
            &TreeCullingScratch::default(),
        );
        let push_count = plan
            .steps
            .iter()
            .filter(|step| step.kind == DepthSortedReplayStepKind::PushPlaneClip)
            .count();
        let pop_count = plan
            .steps
            .iter()
            .filter(|step| step.kind == DepthSortedReplayStepKind::PopPlaneClip)
            .count();
        assert_eq!(push_count, 1);
        assert_eq!(pop_count, 1);
        assert_eq!(run_spans(&plan), vec![(0, 1)]);
    }

    #[test]
    fn a_singular_leaf_palette_forces_recorded_order_without_clips() {
        let contexts = sorting_contexts(&[0, 0, 1, 2, 1], &[None, None, Some(1), None, Some(1)]);
        let command_runs = [command_run(3), command_run(4)];
        let (mut palette, draw_space, backface_culled) = identity_inputs(5);
        palette[2] = scale_matrix(0.0, 1.0, 1.0);
        let plan = build_depth_sorted_replay_plan(
            &command_runs,
            &contexts,
            &palette,
            &leaf_to_context_palette(&contexts, &palette),
            &draw_space,
            &backface_culled,
            &TreeCullingScratch::default(),
        );
        assert_eq!(run_spans(&plan), vec![(0, 1), (1, 1)]);
        assert!(
            plan.steps
                .iter()
                .all(|step| step.kind == DepthSortedReplayStepKind::RunSpan)
        );
    }

    #[test]
    fn coplanar_runs_keep_recorded_relative_order() {
        let contexts = sorting_contexts(&[0, 0, 1, 1], &[None, None, Some(1), Some(1)]);
        let command_runs = [command_run(3), command_run(2)];
        let (palette, draw_space, backface_culled) = identity_inputs(4);
        let plan = build_depth_sorted_replay_plan(
            &command_runs,
            &contexts,
            &palette,
            &leaf_to_context_palette(&contexts, &palette),
            &draw_space,
            &backface_culled,
            &TreeCullingScratch::default(),
        );
        assert_eq!(run_spans(&plan), vec![(0, 1), (1, 1)]);
    }

    #[test]
    fn a_nested_context_sorts_inside_its_outer_plane() {
        let contexts = sorting_contexts(
            &[0, 0, 1, 2, 3, 3, 1],
            &[None, None, Some(1), None, Some(3), Some(3), Some(1)],
        );
        let command_runs = [command_run(5), command_run(4), command_run(6)];
        let (mut palette, draw_space, backface_culled) = identity_inputs(7);
        palette[2] = translation_matrix(0.0, 0.0, 100.0);
        palette[3] = translation_matrix(0.0, 0.0, 100.0);
        palette[4] = translation_matrix(0.0, 0.0, 90.0);
        palette[5] = translation_matrix(0.0, 0.0, 110.0);
        palette[6] = translation_matrix(0.0, 0.0, -100.0);
        let plan = build_depth_sorted_replay_plan(
            &command_runs,
            &contexts,
            &palette,
            &leaf_to_context_palette(&contexts, &palette),
            &draw_space,
            &backface_culled,
            &TreeCullingScratch::default(),
        );
        assert_eq!(run_spans(&plan), vec![(2, 1), (1, 1), (0, 1)]);
    }
}
