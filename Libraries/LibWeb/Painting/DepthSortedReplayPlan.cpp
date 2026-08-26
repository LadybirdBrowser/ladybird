/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibGfx/BSPTree.h>
#include <LibGfx/BoundingBox.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DepthSortedReplayPlan.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

namespace {

struct LeafBounds {
    SpatialNodeIndex leaf;
    Gfx::FloatRect bounds;
    bool unbounded { false };
};

struct CommandChunk {
    u32 offset { 0 };
    u32 size { 0 };
    SpatialNodeIndex leaf;
    SpatialNodeIndex context;
    Vector<LeafBounds, 2> bounds_by_level;
};

struct ChunkPlacement {
    SpatialNodeIndex child_context;
    SpatialNodeIndex leaf;
};

struct DepthSortedPlanBuilder {
    SortingContexts const& contexts;
    ReadonlySpan<Gfx::FloatMatrix4x4> transform_palette;
    Vector<DepthSortedReplayStep> steps;

    void emit_chunks(ReadonlySpan<CommandChunk> chunks, SpatialNodeIndex enclosing_context);
    void sort_and_emit_context(ReadonlySpan<CommandChunk> chunks, SpatialNodeIndex sorting_context);
};

}

static Vector<CommandChunk> partition_commands_into_plane_chunks(
    ReadonlyBytes commands,
    SortingContexts const& contexts,
    ReadonlySpan<Gfx::FloatMatrix4x4> transform_palette,
    ReadonlySpan<SpatialNodeIndex> draw_space,
    ReadonlySpan<bool> backface_culled)
{
    struct LeafMapping {
        SpatialNodeIndex leaf;
        Optional<Gfx::FloatMatrix4x4> to_leaf;
        bool unbounded { false };
    };

    struct LeafMappingsKey {
        SpatialNodeIndex leaf;
        SpatialNodeIndex context;
        SpatialNodeIndex spatial_node;
        bool operator==(LeafMappingsKey const&) const = default;
    };

    Vector<LeafMapping, 4> mappings;
    Optional<LeafMappingsKey> mappings_key;
    auto ensure_mappings = [&](LeafMappingsKey key) {
        if (mappings_key == key)
            return;
        mappings_key = key;
        mappings.clear_with_capacity();
        auto leaf = key.leaf;
        auto sorting_context = key.context;
        while (sorting_context != NO_SORTING_CONTEXT) {
            if (key.spatial_node == leaf) {
                mappings.append({ leaf, {} });
            } else if (auto inverse = transform_palette[leaf.value()].inverse(); inverse.has_value()) {
                mappings.append({ leaf, *inverse * transform_palette[key.spatial_node.value()] });
            } else {
                mappings.append({ leaf, {}, true });
            }
            auto link = contexts.links.get(sorting_context.value());
            if (!link.has_value())
                break;
            leaf = link->parent_leaf;
            sorting_context = link->parent_context;
        }
    };

    auto bounds_of_mapped_rect = [](Gfx::FloatMatrix4x4 const& matrix, Gfx::FloatRect const& rect) {
        Gfx::FloatBoundingBox bounding_box;
        for (auto const& vertex : Gfx::map_rect_through_projection(matrix, rect))
            bounding_box.add_point(vertex.x(), vertex.y());
        return bounding_box.to_rect();
    };

    Vector<CommandChunk> chunks;
    DisplayList::for_each_command_header(commands, [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        auto offset = static_cast<u32>(payload.data() - commands.data() - sizeof(DisplayListCommandHeader));
        auto size = static_cast<u32>(sizeof(DisplayListCommandHeader) + header.payload_size);
        auto spatial = header.context.spatial;
        auto leaf = contexts.leaf_by_node[spatial.value()];
        auto sorting_context = contexts.context_by_node[spatial.value()];
        if (chunks.is_empty() || chunks.last().leaf != leaf || chunks.last().context != sorting_context)
            chunks.append({ offset, size, leaf, sorting_context, {} });
        else
            chunks.last().size += size;

        if (!header.has_bounding_rect || header.is_clip || sorting_context == NO_SORTING_CONTEXT || backface_culled[spatial.value()])
            return;
        ensure_mappings({ leaf, sorting_context, draw_space[spatial.value()] });
        auto rect = header.bounding_rect.to_type<float>();
        auto& level_entries = chunks.last().bounds_by_level;
        for (auto const& mapping : mappings) {
            auto entry = level_entries.find_if([&](auto const& existing_entry) { return existing_entry.leaf == mapping.leaf; });
            if (entry == level_entries.end()) {
                level_entries.append({ mapping.leaf, {} });
                entry = level_entries.end() - 1;
            }
            if (mapping.unbounded) {
                entry->unbounded = true;
            } else if (mapping.to_leaf.has_value()) {
                entry->bounds.unite(bounds_of_mapped_rect(*mapping.to_leaf, rect));
            } else {
                entry->bounds.unite(rect);
            }
        }
    });
    return chunks;
}

static ChunkPlacement place_chunk_within(CommandChunk const& chunk, SpatialNodeIndex enclosing_context, SortingContexts const& contexts)
{
    if (chunk.context == enclosing_context)
        return { NO_SORTING_CONTEXT, chunk.leaf };
    for (auto current = chunk.context; current != NO_SORTING_CONTEXT;) {
        auto link = contexts.links.get(current.value());
        if (!link.has_value())
            break;
        if (link->parent_context == enclosing_context)
            return { current, link->parent_leaf };
        current = link->parent_context;
    }
    return { NO_SORTING_CONTEXT, chunk.leaf };
}

void DepthSortedPlanBuilder::emit_chunks(ReadonlySpan<CommandChunk> chunks, SpatialNodeIndex enclosing_context)
{
    size_t i = 0;
    while (i < chunks.size()) {
        auto child_context = place_chunk_within(chunks[i], enclosing_context, contexts).child_context;
        if (child_context == NO_SORTING_CONTEXT) {
            steps.append(DisplayListCommandRange { chunks[i].offset, chunks[i].size });
            ++i;
            continue;
        }
        size_t run_end = i + 1;
        while (run_end < chunks.size() && place_chunk_within(chunks[run_end], enclosing_context, contexts).child_context == child_context)
            ++run_end;
        sort_and_emit_context(chunks.slice(i, run_end - i), child_context);
        i = run_end;
    }
}

void DepthSortedPlanBuilder::sort_and_emit_context(ReadonlySpan<CommandChunk> chunks, SpatialNodeIndex sorting_context)
{
    // The unit of sorting is a run of consecutive chunks sharing a plane, not the whole plane. Coplanar planes render
    // in painting order, and separate runs of one plane interleaved with a coplanar sibling must keep their recorded
    // positions relative to it.
    struct PlaneRun {
        SpatialNodeIndex leaf;
        size_t begin { 0 };
        size_t end { 0 };
        Gfx::FloatRect bounds;
        bool unbounded { false };
    };
    Vector<PlaneRun, 16> runs;
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto leaf = place_chunk_within(chunks[i], sorting_context, contexts).leaf;
        if (runs.is_empty() || runs.last().leaf != leaf)
            runs.append({ leaf, i, i + 1, {} });
        else
            runs.last().end = i + 1;
        for (auto const& entry : chunks[i].bounds_by_level) {
            if (entry.leaf == leaf) {
                runs.last().bounds.unite(entry.bounds);
                runs.last().unbounded |= entry.unbounded;
            }
        }
    }

    if (any_of(runs, [](auto const& run) { return run.unbounded; })) {
        emit_chunks(chunks, sorting_context);
        return;
    }

    // Runs that are fully backface-culled, that project entirely behind the eye, or whose content bounds are empty
    // draw nothing and are dropped rather than sorted. The bounds are inflated so a split piece's clip stays clear of
    // the anti-aliased fringe of the content's own edges.
    Vector<Gfx::BSPPolygon> polygons;
    for (size_t run_index = 0; run_index < runs.size(); ++run_index) {
        auto const& run = runs[run_index];
        if (run.bounds.is_empty())
            continue;
        auto vertices = Gfx::map_rect_through_projection(transform_palette[run.leaf.value()], run.bounds.inflated(4, 4));
        if (vertices.size() < 3)
            continue;
        polygons.append({ move(vertices), run_index, false });
    }

    // FIXME: Pieces of a split plane whose content carries filter effects render incorrectly: each piece filters its
    //        clipped content independently, truncating filter output at the piece boundary and seaming it along the cut.
    for (auto& polygon : Gfx::split_and_sort_polygons_back_to_front(move(polygons))) {
        auto const& run = runs[polygon.plane_index];
        if (polygon.clipped)
            steps.append(PushPlaneClip { move(polygon.vertices) });
        emit_chunks(chunks.slice(run.begin, run.end - run.begin), sorting_context);
        if (polygon.clipped)
            steps.append(PopPlaneClip {});
    }
}

// https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
// The element establishing the 3D rendering context, and each other 3D transformed element participating in the
// 3D rendering context, is rendered into its own plane. Intersection is performed between this set of planes,
// according to Newell's algorithm, with the planes transformed by the accumulated 3D transformation matrix.
// Coplanar 3D transformed elements are rendered in painting order.
//
// The command stream is partitioned into contiguous chunks that share a plane. Each 3D rendering context's
// planes are ordered back to front with a BSP tree over their content bounds; a plane cut by another's plane is
// replayed once per piece under a device-space polygon clip. Chunks in a nested context sort among themselves
// inside the plane of the outer context they render into.
Vector<DepthSortedReplayStep> build_depth_sorted_replay_plan(
    ReadonlyBytes commands,
    AccumulatedVisualContextTree const& visual_context_tree,
    ReadonlySpan<Gfx::FloatMatrix4x4> transform_palette,
    ReadonlySpan<SpatialNodeIndex> draw_space,
    ReadonlySpan<bool> backface_culled)
{
    auto contexts = visual_context_tree.resolve_sorting_contexts();
    auto chunks = partition_commands_into_plane_chunks(commands, contexts, transform_palette, draw_space, backface_culled);
    DepthSortedPlanBuilder builder { contexts, transform_palette, {} };
    builder.emit_chunks(chunks, NO_SORTING_CONTEXT);
    return move(builder.steps);
}

}
