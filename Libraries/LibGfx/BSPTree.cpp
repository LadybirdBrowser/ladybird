/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/Optional.h>
#include <AK/QuickSort.h>
#include <LibGfx/BSPTree.h>
#include <LibGfx/Vector4.h>

namespace Gfx {

// Vertices closer to a splitting plane than this distance count as lying on it, giving the plane a thickness that
// absorbs floating-point noise from the projection.
static constexpr float on_plane_threshold = 0.05f;

Vector<FloatVector3, 8> map_rect_through_projection(FloatMatrix4x4 const& matrix, FloatRect const& rect)
{
    // Projecting a point divides it by w, which only works in front of the eye plane where w is positive.
    // Edges crossing behind the eye are clipped at this small positive w so the divide stays finite.
    static constexpr float minimum_projection_w = 0.00001f;

    Array corners = {
        matrix * FloatVector4 { rect.left(), rect.top(), 0, 1 },
        matrix * FloatVector4 { rect.right(), rect.top(), 0, 1 },
        matrix * FloatVector4 { rect.right(), rect.bottom(), 0, 1 },
        matrix * FloatVector4 { rect.left(), rect.bottom(), 0, 1 },
    };

    Vector<FloatVector3, 8> result;
    auto append_projected = [&](FloatVector4 const& vertex) {
        result.append({ vertex.x() / vertex.w(), vertex.y() / vertex.w(), vertex.z() / vertex.w() });
    };
    for (size_t i = 0; i < corners.size(); ++i) {
        auto const& current = corners[i];
        auto const& next = corners[(i + 1) % corners.size()];
        bool current_in_front_of_eye = current.w() > minimum_projection_w;
        bool next_in_front_of_eye = next.w() > minimum_projection_w;
        if (current_in_front_of_eye)
            append_projected(current);
        if (current_in_front_of_eye != next_in_front_of_eye) {
            auto t = (minimum_projection_w - current.w()) / (next.w() - current.w());
            append_projected(current + (next - current) * t);
        }
    }
    return result;
}

static Optional<FloatVector3> polygon_normal(ReadonlySpan<FloatVector3> vertices)
{
    if (vertices.size() < 3)
        return {};
    FloatVector3 normal { 0, 0, 0 };
    for (size_t i = 1; i < vertices.size() - 1; ++i)
        normal += (vertices[i] - vertices[0]).cross(vertices[i + 1] - vertices[0]);
    auto length = normal.length();
    // Returns no value for polygons that enclose no area, as they do not define a plane.
    if (length == 0)
        return {};
    return normal / length;
}

namespace {

struct PartitionedPolygon {
    BSPPolygon polygon;
    FloatVector3 plane_normal;
    float plane_distance { 0 };
};

constexpr size_t no_bsp_node = NumericLimits<size_t>::max();

struct BSPTreeNode {
    FloatVector3 plane_normal;
    float plane_distance { 0 };
    Vector<BSPPolygon> coplanar_polygons;
    size_t front { no_bsp_node };
    size_t back { no_bsp_node };
};

// The polygons of a subtree that has not been built yet, and the parent slot that will reference its node.
struct PendingSubtree {
    Vector<PartitionedPolygon> polygons;
    size_t parent { no_bsp_node };
    bool is_front_child { false };
};

struct PolygonSplit {
    Optional<PartitionedPolygon> front_piece;
    Optional<PartitionedPolygon> back_piece;
};

}

static PolygonSplit split_polygon(PartitionedPolygon polygon, ReadonlySpan<float> vertex_distances)
{
    Vector<FloatVector3, 8> front_vertices;
    Vector<FloatVector3, 8> back_vertices;
    auto const& vertices = polygon.polygon.vertices;
    for (size_t i = 0; i < vertices.size(); ++i) {
        size_t next_index = (i + 1) % vertices.size();
        auto current_distance = vertex_distances[i];
        auto next_distance = vertex_distances[next_index];
        if (current_distance >= -on_plane_threshold)
            front_vertices.append(vertices[i]);
        if (current_distance <= on_plane_threshold)
            back_vertices.append(vertices[i]);
        bool edge_crosses_plane = (current_distance > on_plane_threshold && next_distance < -on_plane_threshold)
            || (current_distance < -on_plane_threshold && next_distance > on_plane_threshold);
        if (edge_crosses_plane) {
            auto t = current_distance / (current_distance - next_distance);
            auto intersection = vertices[i] + (vertices[next_index] - vertices[i]) * t;
            front_vertices.append(intersection);
            back_vertices.append(intersection);
        }
    }

    auto make_piece = [&](Vector<FloatVector3, 8> piece_vertices) -> Optional<PartitionedPolygon> {
        if (piece_vertices.size() < 3)
            return {};
        return PartitionedPolygon {
            BSPPolygon { move(piece_vertices), polygon.polygon.plane_index, true },
            polygon.plane_normal,
            polygon.plane_distance,
        };
    };
    return { make_piece(move(front_vertices)), make_piece(move(back_vertices)) };
}

static Vector<BSPTreeNode> build_bsp_tree(Vector<PartitionedPolygon> polygons)
{
    Vector<BSPTreeNode> nodes;
    Vector<PendingSubtree> pending_subtrees;
    if (!polygons.is_empty())
        pending_subtrees.append({ move(polygons), no_bsp_node, false });

    Vector<float, 8> vertex_distances;
    while (!pending_subtrees.is_empty()) {
        auto subtree = pending_subtrees.take_last();
        auto node_index = nodes.size();
        if (subtree.parent != no_bsp_node) {
            if (subtree.is_front_child) {
                nodes[subtree.parent].front = node_index;
            } else {
                nodes[subtree.parent].back = node_index;
            }
        }

        auto splitter_index = subtree.polygons.size() / 2;
        auto plane_normal = subtree.polygons[splitter_index].plane_normal;
        auto plane_distance = subtree.polygons[splitter_index].plane_distance;

        Vector<BSPPolygon> coplanar_polygons;
        Vector<PartitionedPolygon> front_list;
        Vector<PartitionedPolygon> back_list;
        for (size_t polygon_index = 0; polygon_index < subtree.polygons.size(); ++polygon_index) {
            auto& polygon = subtree.polygons[polygon_index];
            if (polygon_index == splitter_index) {
                coplanar_polygons.append(move(polygon.polygon));
                continue;
            }
            vertex_distances.clear_with_capacity();
            size_t front_count = 0;
            size_t back_count = 0;
            for (auto const& vertex : polygon.polygon.vertices) {
                auto distance = plane_normal.dot(vertex) - plane_distance;
                vertex_distances.append(distance);
                if (distance > on_plane_threshold) {
                    ++front_count;
                } else if (distance < -on_plane_threshold) {
                    ++back_count;
                }
            }

            if (front_count == 0 && back_count == 0) {
                coplanar_polygons.append(move(polygon.polygon));
            } else if (back_count == 0) {
                front_list.append(move(polygon));
            } else if (front_count == 0) {
                back_list.append(move(polygon));
            } else {
                auto [front_piece, back_piece] = split_polygon(move(polygon), vertex_distances);
                if (front_piece.has_value())
                    front_list.append(front_piece.release_value());
                if (back_piece.has_value())
                    back_list.append(back_piece.release_value());
            }
        }
        nodes.append({ plane_normal, plane_distance, move(coplanar_polygons), no_bsp_node, no_bsp_node });

        if (!front_list.is_empty())
            pending_subtrees.append({ move(front_list), node_index, true });
        if (!back_list.is_empty())
            pending_subtrees.append({ move(back_list), node_index, false });
    }
    return nodes;
}

static Vector<BSPPolygon> collect_back_to_front(Vector<BSPTreeNode> nodes)
{
    Vector<BSPPolygon> ordered;
    if (nodes.is_empty())
        return ordered;

    size_t polygon_count = 0;
    for (auto const& node : nodes)
        polygon_count += node.coplanar_polygons.size();
    ordered.ensure_capacity(polygon_count);

    struct TraversalStep {
        size_t node_index { 0 };
        bool ready_to_emit { false };
    };
    Vector<TraversalStep> traversal_stack;
    traversal_stack.append({ 0, false });
    while (!traversal_stack.is_empty()) {
        auto step = traversal_stack.take_last();
        auto& node = nodes[step.node_index];
        // The subtree on the side of the plane the viewer is on paints last. Coplanar polygons paint in their stored
        // paint order regardless of which way the plane faces.
        auto far_subtree = node.plane_normal.z() > 0 ? node.back : node.front;
        auto near_subtree = node.plane_normal.z() > 0 ? node.front : node.back;
        if (!step.ready_to_emit) {
            traversal_stack.append({ step.node_index, true });
            if (far_subtree != no_bsp_node)
                traversal_stack.append({ far_subtree, false });
            continue;
        }
        for (auto& polygon : node.coplanar_polygons)
            ordered.unchecked_append(move(polygon));
        if (near_subtree != no_bsp_node)
            traversal_stack.append({ near_subtree, false });
    }
    return ordered;
}

static bool all_planes_are_parallel(ReadonlySpan<PartitionedPolygon> polygons)
{
    // The cross product of two unit normals has the sine of the angle between the planes as its length.
    // Below a microradian of tilt the planes are treated as parallel.
    static constexpr float maximum_parallel_cross_length_squared = 1e-12f;
    auto const& first_normal = polygons.first().plane_normal;
    for (auto const& polygon : polygons.slice(1)) {
        auto cross = polygon.plane_normal.cross(first_normal);
        if (cross.dot(cross) > maximum_parallel_cross_length_squared)
            return false;
    }
    return true;
}

static Vector<BSPPolygon> sort_parallel_polygons_back_to_front(Vector<PartitionedPolygon> polygons)
{
    // Fast path for parallel planes, where no splitting is needed.

    auto axis = polygons.first().plane_normal;
    if (axis.z() < 0)
        axis = -axis;

    struct DepthOrderedPolygon {
        float depth { 0 };
        size_t input_index { 0 };
    };
    Vector<DepthOrderedPolygon> order;
    order.ensure_capacity(polygons.size());
    for (size_t i = 0; i < polygons.size(); ++i) {
        auto depth = polygons[i].plane_normal.dot(axis) > 0 ? polygons[i].plane_distance : -polygons[i].plane_distance;
        order.unchecked_append({ depth, i });
    }
    quick_sort(order, [](DepthOrderedPolygon const& a, DepthOrderedPolygon const& b) {
        if (a.depth != b.depth)
            return a.depth < b.depth;
        return a.input_index < b.input_index;
    });

    Vector<BSPPolygon> ordered;
    ordered.ensure_capacity(order.size());
    for (auto const& entry : order)
        ordered.unchecked_append(move(polygons[entry.input_index].polygon));
    return ordered;
}

Vector<BSPPolygon> split_and_sort_polygons_back_to_front(Vector<BSPPolygon> polygons)
{
    Vector<PartitionedPolygon> partitioned;
    partitioned.ensure_capacity(polygons.size());
    for (auto& polygon : polygons) {
        auto normal = polygon_normal(polygon.vertices);
        if (!normal.has_value())
            continue;
        auto distance = normal->dot(polygon.vertices.first());
        partitioned.unchecked_append({ move(polygon), *normal, distance });
    }

    if (!partitioned.is_empty() && all_planes_are_parallel(partitioned))
        return sort_parallel_polygons_back_to_front(move(partitioned));

    return collect_back_to_front(build_bsp_tree(move(partitioned)));
}

}
