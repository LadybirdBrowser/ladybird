/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libgfx_rust::bsp_tree::{BspPolygon, map_rect_through_projection, split_and_sort_polygons_back_to_front};
use libgfx_rust::{FloatMatrix4x4, FloatRect, FloatVector3};

use super::commands::{DisplayListCommandRun, SpatialNodeIndex};
use crate::painting::visual_context::{NO_SORTING_CONTEXT, SortingContexts};

struct LeafBounds {
    leaf: SpatialNodeIndex,
    bounds: FloatRect,
    unbounded: bool,
}

struct CommandChunk {
    first_run: u32,
    run_count: u32,
    leaf: SpatialNodeIndex,
    context: SpatialNodeIndex,
    bounds_by_level: Vec<LeafBounds>,
}

#[derive(Clone, Copy)]
struct ChunkPlacement {
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
    steps: Vec<DepthSortedReplayStep>,
    vertices: Vec<FloatVector3>,
}

impl DepthSortedPlanBuilder<'_> {
    fn append_run_span(&mut self, chunk: &CommandChunk) {
        self.steps.push(DepthSortedReplayStep {
            kind: DepthSortedReplayStepKind::RunSpan,
            first_run: chunk.first_run,
            run_count: chunk.run_count,
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

    fn emit_chunks(&mut self, chunks: &[CommandChunk], enclosing_context: SpatialNodeIndex) {
        let mut index = 0;
        while index < chunks.len() {
            let child_context = place_chunk_within(&chunks[index], enclosing_context, self.contexts).child_context;
            if child_context == NO_SORTING_CONTEXT {
                self.append_run_span(&chunks[index]);
                index += 1;
                continue;
            }
            let mut run_end = index + 1;
            while run_end < chunks.len()
                && place_chunk_within(&chunks[run_end], enclosing_context, self.contexts).child_context == child_context
            {
                run_end += 1;
            }
            self.sort_and_emit_context(&chunks[index..run_end], child_context);
            index = run_end;
        }
    }

    fn sort_and_emit_context(&mut self, chunks: &[CommandChunk], sorting_context: SpatialNodeIndex) {
        // The unit of sorting is a run of consecutive chunks sharing a plane, not the whole plane. Coplanar planes render
        // in painting order, and separate runs of one plane interleaved with a coplanar sibling must keep their recorded
        // positions relative to it.
        struct PlaneRun {
            leaf: SpatialNodeIndex,
            begin: usize,
            end: usize,
            bounds: FloatRect,
            unbounded: bool,
        }

        let mut runs: Vec<PlaneRun> = Vec::new();
        for (index, chunk) in chunks.iter().enumerate() {
            let leaf = place_chunk_within(chunk, sorting_context, self.contexts).leaf;
            if runs.last().is_none_or(|run| run.leaf != leaf) {
                runs.push(PlaneRun {
                    leaf,
                    begin: index,
                    end: index + 1,
                    bounds: FloatRect::default(),
                    unbounded: false,
                });
            } else {
                runs.last_mut().unwrap().end = index + 1;
            }
            for entry in &chunk.bounds_by_level {
                if entry.leaf == leaf {
                    let run = runs.last_mut().unwrap();
                    run.bounds = run.bounds.united(entry.bounds);
                    run.unbounded |= entry.unbounded;
                }
            }
        }

        if runs.iter().any(|run| run.unbounded) {
            self.emit_chunks(chunks, sorting_context);
            return;
        }

        // Runs that are fully backface-culled, that project entirely behind the eye, or whose content bounds are empty
        // draw nothing and are dropped rather than sorted. The bounds are inflated so a split piece's clip stays clear of
        // the anti-aliased fringe of the content's own edges.
        let mut polygons = Vec::new();
        for (run_index, run) in runs.iter().enumerate() {
            if run.bounds.is_empty() {
                continue;
            }
            let vertices = map_rect_through_projection(
                self.transform_palette[run.leaf.0 as usize],
                run.bounds.inflated(4.0, 4.0),
            );
            if vertices.len() < 3 {
                continue;
            }
            polygons.push(BspPolygon {
                vertices,
                plane_index: run_index,
                clipped: false,
            });
        }

        // FIXME: Pieces of a split plane whose content carries filter effects render incorrectly: each piece filters its
        //        clipped content independently, truncating filter output at the piece boundary and seaming it along the cut.
        for polygon in split_and_sort_polygons_back_to_front(polygons) {
            let run = &runs[polygon.plane_index];
            if polygon.clipped {
                self.append_plane_clip(polygon.vertices);
            }
            self.emit_chunks(&chunks[run.begin..run.end], sorting_context);
            if polygon.clipped {
                self.append_pop_plane_clip();
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

fn ensure_mappings(
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

fn partition_command_runs_into_plane_chunks(
    command_runs: &[DisplayListCommandRun],
    contexts: &SortingContexts,
    transform_palette: &[FloatMatrix4x4],
    draw_space: &[SpatialNodeIndex],
    backface_culled: &[bool],
    frame_has_empty_effective_clip: &[bool],
) -> Vec<CommandChunk> {
    let mut mappings = Vec::new();
    let mut mappings_key = None;

    // A run's ink bounds already leave out clips and unbounded draws; mapping their union once per
    // level is conservative, as it can only widen a plane's bounds.
    let mut chunks: Vec<CommandChunk> = Vec::new();
    for (run_index, run) in command_runs.iter().enumerate() {
        let spatial = run.context.spatial;
        let leaf = contexts.leaf_by_node[spatial.0 as usize];
        let sorting_context = contexts.context_by_node[spatial.0 as usize];
        if chunks
            .last()
            .is_none_or(|chunk| chunk.leaf != leaf || chunk.context != sorting_context)
        {
            chunks.push(CommandChunk {
                first_run: run_index as u32,
                run_count: 1,
                leaf,
                context: sorting_context,
                bounds_by_level: Vec::new(),
            });
        } else {
            chunks.last_mut().unwrap().run_count += 1;
        }

        if run.ink_bounds.is_empty() || sorting_context == NO_SORTING_CONTEXT || backface_culled[spatial.0 as usize] {
            continue;
        }
        if !run.context.frame.is_none() && frame_has_empty_effective_clip[run.context.frame.0 as usize] {
            continue;
        }
        ensure_mappings(
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
        let level_entries = &mut chunks.last_mut().unwrap().bounds_by_level;
        for mapping in &mappings {
            let entry_index = level_entries.iter().position(|entry| entry.leaf == mapping.leaf);
            let entry = if let Some(index) = entry_index {
                &mut level_entries[index]
            } else {
                level_entries.push(LeafBounds {
                    leaf: mapping.leaf,
                    bounds: FloatRect::default(),
                    unbounded: false,
                });
                level_entries.last_mut().unwrap()
            };
            if mapping.unbounded {
                entry.unbounded = true;
            } else if let Some(to_leaf) = mapping.to_leaf {
                entry.bounds = entry.bounds.united(bounds_of_mapped_rect(to_leaf, rect));
            } else {
                entry.bounds = entry.bounds.united(rect);
            }
        }
    }
    chunks
}

fn place_chunk_within(
    chunk: &CommandChunk,
    enclosing_context: SpatialNodeIndex,
    contexts: &SortingContexts,
) -> ChunkPlacement {
    if chunk.context == enclosing_context {
        return ChunkPlacement {
            child_context: NO_SORTING_CONTEXT,
            leaf: chunk.leaf,
        };
    }
    let mut current = chunk.context;
    while current != NO_SORTING_CONTEXT {
        let Some(link) = contexts.links.get(&current.0) else {
            break;
        };
        if link.parent_context == enclosing_context {
            return ChunkPlacement {
                child_context: current,
                leaf: link.parent_leaf,
            };
        }
        current = link.parent_context;
    }
    ChunkPlacement {
        child_context: NO_SORTING_CONTEXT,
        leaf: chunk.leaf,
    }
}

// https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
// The element establishing the 3D rendering context, and each other 3D transformed element participating in the
// 3D rendering context, is rendered into its own plane. Intersection is performed between this set of planes,
// according to Newell's algorithm, with the planes transformed by the accumulated 3D transformation matrix.
// Coplanar 3D transformed elements are rendered in painting order.
//
// The command run table is partitioned into contiguous chunks that share a plane. Each 3D rendering context's
// planes are ordered back to front with a BSP tree over their content bounds; a plane cut by another's plane is
// replayed once per piece under a device-space polygon clip. Chunks in a nested context sort among themselves
// inside the plane of the outer context they render into.
pub fn build_depth_sorted_replay_plan(
    command_runs: &[DisplayListCommandRun],
    contexts: &SortingContexts,
    transform_palette: &[FloatMatrix4x4],
    draw_space: &[SpatialNodeIndex],
    backface_culled: &[bool],
    frame_has_empty_effective_clip: &[bool],
) -> DepthSortedReplayPlan {
    let chunks = partition_command_runs_into_plane_chunks(
        command_runs,
        contexts,
        transform_palette,
        draw_space,
        backface_culled,
        frame_has_empty_effective_clip,
    );
    let mut builder = DepthSortedPlanBuilder {
        contexts,
        transform_palette,
        steps: Vec::new(),
        vertices: Vec::new(),
    };
    builder.emit_chunks(&chunks, NO_SORTING_CONTEXT);
    DepthSortedReplayPlan {
        steps: builder.steps,
        vertices: builder.vertices,
    }
}

#[cfg(test)]
mod tests {
    use libgfx_rust::{FloatMatrix4x4, IntRect, scale_matrix, translation_matrix};

    use super::*;
    use crate::painting::display_list::commands::ContextRef;
    use crate::painting::visual_context::resolve_sorting_contexts_over_nodes;

    fn sorting_contexts(parents: &[u32], sorting_context_roots: &[Option<u32>]) -> SortingContexts {
        resolve_sorting_contexts_over_nodes(parents.len(), |index| {
            (
                SpatialNodeIndex(parents[index]),
                sorting_context_roots[index].map(SpatialNodeIndex),
            )
        })
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
        let cosine = radians.cos();
        let sine = radians.sin();
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
        let plan =
            build_depth_sorted_replay_plan(&command_runs, &contexts, &palette, &draw_space, &backface_culled, &[]);
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
        let plan =
            build_depth_sorted_replay_plan(&command_runs, &contexts, &palette, &draw_space, &backface_culled, &[]);
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
        let plan =
            build_depth_sorted_replay_plan(&command_runs, &contexts, &palette, &draw_space, &backface_culled, &[]);
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
    fn a_singular_leaf_palette_forces_recorded_order_without_clips() {
        let contexts = sorting_contexts(&[0, 0, 1, 2, 1], &[None, None, Some(1), None, Some(1)]);
        let command_runs = [command_run(3), command_run(4)];
        let (mut palette, draw_space, backface_culled) = identity_inputs(5);
        palette[2] = scale_matrix(0.0, 1.0, 1.0);
        let plan =
            build_depth_sorted_replay_plan(&command_runs, &contexts, &palette, &draw_space, &backface_culled, &[]);
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
        let plan =
            build_depth_sorted_replay_plan(&command_runs, &contexts, &palette, &draw_space, &backface_culled, &[]);
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
        let plan =
            build_depth_sorted_replay_plan(&command_runs, &contexts, &palette, &draw_space, &backface_culled, &[]);
        assert_eq!(run_spans(&plan), vec![(2, 1), (1, 1), (0, 1)]);
    }
}
