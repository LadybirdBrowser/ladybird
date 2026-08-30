/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cmp::Ordering;

use crate::{FloatMatrix4x4, FloatRect, FloatVector3};

const ON_PLANE_THRESHOLD: f32 = 0.05;

// A convex polygon in the shared post-projection space of a three-dimensional scene, where x and y
// are surface coordinates and the positive z-axis points toward the viewer. The plane index
// identifies the plane the polygon was built from and is preserved on pieces produced by splitting.
pub struct BspPolygon {
    pub vertices: Vec<FloatVector3>,
    pub plane_index: usize,
    pub clipped: bool,
}

pub fn map_rect_through_projection(matrix: FloatMatrix4x4, rect: FloatRect) -> Vec<FloatVector3> {
    // Projecting a point divides it by w, which only works in front of the eye plane where w is positive.
    // Edges crossing behind the eye are clipped at this small positive w so the divide stays finite.
    const MINIMUM_PROJECTION_W: f32 = 0.00001;

    let corners = [
        matrix.map_vector4([rect.x, rect.y, 0.0, 1.0]),
        matrix.map_vector4([rect.right(), rect.y, 0.0, 1.0]),
        matrix.map_vector4([rect.right(), rect.bottom(), 0.0, 1.0]),
        matrix.map_vector4([rect.x, rect.bottom(), 0.0, 1.0]),
    ];

    let mut result = Vec::with_capacity(8);
    let append_projected = |result: &mut Vec<FloatVector3>, vertex: [f32; 4]| {
        result.push(FloatVector3 {
            x: vertex[0] / vertex[3],
            y: vertex[1] / vertex[3],
            z: vertex[2] / vertex[3],
        });
    };
    for (index, current) in corners.iter().enumerate() {
        let next = corners[(index + 1) % corners.len()];
        let current_in_front_of_eye = current[3] > MINIMUM_PROJECTION_W;
        let next_in_front_of_eye = next[3] > MINIMUM_PROJECTION_W;
        if current_in_front_of_eye {
            append_projected(&mut result, *current);
        }
        if current_in_front_of_eye != next_in_front_of_eye {
            let t = (MINIMUM_PROJECTION_W - current[3]) / (next[3] - current[3]);
            let intersection = [
                current[0] + (next[0] - current[0]) * t,
                current[1] + (next[1] - current[1]) * t,
                current[2] + (next[2] - current[2]) * t,
                current[3] + (next[3] - current[3]) * t,
            ];
            append_projected(&mut result, intersection);
        }
    }
    result
}

fn polygon_normal(vertices: &[FloatVector3]) -> Option<FloatVector3> {
    if vertices.len() < 3 {
        return None;
    }
    let mut normal = FloatVector3::default();
    for index in 1..vertices.len() - 1 {
        normal = normal + (vertices[index] - vertices[0]).cross(vertices[index + 1] - vertices[0]);
    }
    let length = normal.length();
    // Returns no value for polygons that enclose no area, as they do not define a plane.
    if length == 0.0 {
        return None;
    }
    Some(normal * (1.0 / length))
}

struct PartitionedPolygon {
    polygon: BspPolygon,
    plane_normal: FloatVector3,
    plane_distance: f32,
}

const NO_BSP_NODE: usize = usize::MAX;

struct BspTreeNode {
    plane_normal: FloatVector3,
    coplanar_polygons: Vec<BspPolygon>,
    front: usize,
    back: usize,
}

// The polygons of a subtree that has not been built yet, and the parent slot that will reference its node.
struct PendingSubtree {
    polygons: Vec<PartitionedPolygon>,
    parent: usize,
    is_front_child: bool,
}

struct PolygonSplit {
    front_piece: Option<PartitionedPolygon>,
    back_piece: Option<PartitionedPolygon>,
}

fn split_polygon(polygon: PartitionedPolygon, vertex_distances: &[f32]) -> PolygonSplit {
    let mut front_vertices = Vec::with_capacity(8);
    let mut back_vertices = Vec::with_capacity(8);
    let vertices = &polygon.polygon.vertices;
    for index in 0..vertices.len() {
        let next_index = (index + 1) % vertices.len();
        let current_distance = vertex_distances[index];
        let next_distance = vertex_distances[next_index];
        if current_distance >= -ON_PLANE_THRESHOLD {
            front_vertices.push(vertices[index]);
        }
        if current_distance <= ON_PLANE_THRESHOLD {
            back_vertices.push(vertices[index]);
        }
        let edge_crosses_plane = (current_distance > ON_PLANE_THRESHOLD && next_distance < -ON_PLANE_THRESHOLD)
            || (current_distance < -ON_PLANE_THRESHOLD && next_distance > ON_PLANE_THRESHOLD);
        if edge_crosses_plane {
            let t = current_distance / (current_distance - next_distance);
            let intersection = vertices[index] + (vertices[next_index] - vertices[index]) * t;
            front_vertices.push(intersection);
            back_vertices.push(intersection);
        }
    }

    let make_piece = |vertices: Vec<FloatVector3>| {
        if vertices.len() < 3 {
            return None;
        }
        Some(PartitionedPolygon {
            polygon: BspPolygon {
                vertices,
                plane_index: polygon.polygon.plane_index,
                clipped: true,
            },
            plane_normal: polygon.plane_normal,
            plane_distance: polygon.plane_distance,
        })
    };
    PolygonSplit {
        front_piece: make_piece(front_vertices),
        back_piece: make_piece(back_vertices),
    }
}

fn build_bsp_tree(polygons: Vec<PartitionedPolygon>) -> Vec<BspTreeNode> {
    let mut nodes: Vec<BspTreeNode> = Vec::new();
    let mut pending_subtrees = Vec::new();
    if !polygons.is_empty() {
        pending_subtrees.push(PendingSubtree {
            polygons,
            parent: NO_BSP_NODE,
            is_front_child: false,
        });
    }

    let mut vertex_distances = Vec::with_capacity(8);
    while let Some(subtree) = pending_subtrees.pop() {
        let node_index = nodes.len();
        if subtree.parent != NO_BSP_NODE {
            if subtree.is_front_child {
                nodes[subtree.parent].front = node_index;
            } else {
                nodes[subtree.parent].back = node_index;
            }
        }

        let splitter_index = subtree.polygons.len() / 2;
        let plane_normal = subtree.polygons[splitter_index].plane_normal;
        let plane_distance = subtree.polygons[splitter_index].plane_distance;

        let mut coplanar_polygons = Vec::new();
        let mut front_list = Vec::new();
        let mut back_list = Vec::new();
        for (polygon_index, polygon) in subtree.polygons.into_iter().enumerate() {
            if polygon_index == splitter_index {
                coplanar_polygons.push(polygon.polygon);
                continue;
            }
            vertex_distances.clear();
            let mut front_count = 0;
            let mut back_count = 0;
            for vertex in &polygon.polygon.vertices {
                let distance = plane_normal.dot(*vertex) - plane_distance;
                vertex_distances.push(distance);
                if distance > ON_PLANE_THRESHOLD {
                    front_count += 1;
                } else if distance < -ON_PLANE_THRESHOLD {
                    back_count += 1;
                }
            }

            if front_count == 0 && back_count == 0 {
                coplanar_polygons.push(polygon.polygon);
            } else if back_count == 0 {
                front_list.push(polygon);
            } else if front_count == 0 {
                back_list.push(polygon);
            } else {
                let split = split_polygon(polygon, &vertex_distances);
                if let Some(front_piece) = split.front_piece {
                    front_list.push(front_piece);
                }
                if let Some(back_piece) = split.back_piece {
                    back_list.push(back_piece);
                }
            }
        }
        nodes.push(BspTreeNode {
            plane_normal,
            coplanar_polygons,
            front: NO_BSP_NODE,
            back: NO_BSP_NODE,
        });

        if !front_list.is_empty() {
            pending_subtrees.push(PendingSubtree {
                polygons: front_list,
                parent: node_index,
                is_front_child: true,
            });
        }
        if !back_list.is_empty() {
            pending_subtrees.push(PendingSubtree {
                polygons: back_list,
                parent: node_index,
                is_front_child: false,
            });
        }
    }
    nodes
}

fn collect_back_to_front(mut nodes: Vec<BspTreeNode>) -> Vec<BspPolygon> {
    if nodes.is_empty() {
        return Vec::new();
    }

    let polygon_count = nodes.iter().map(|node| node.coplanar_polygons.len()).sum();
    let mut ordered = Vec::with_capacity(polygon_count);

    struct TraversalStep {
        node_index: usize,
        ready_to_emit: bool,
    }
    let mut traversal_stack = vec![TraversalStep {
        node_index: 0,
        ready_to_emit: false,
    }];
    while let Some(step) = traversal_stack.pop() {
        let node = &nodes[step.node_index];
        // The subtree on the side of the plane the viewer is on paints last. Coplanar polygons paint in their stored
        // paint order regardless of which way the plane faces.
        let far_subtree = if node.plane_normal.z > 0.0 {
            node.back
        } else {
            node.front
        };
        let near_subtree = if node.plane_normal.z > 0.0 {
            node.front
        } else {
            node.back
        };
        if !step.ready_to_emit {
            traversal_stack.push(TraversalStep {
                node_index: step.node_index,
                ready_to_emit: true,
            });
            if far_subtree != NO_BSP_NODE {
                traversal_stack.push(TraversalStep {
                    node_index: far_subtree,
                    ready_to_emit: false,
                });
            }
            continue;
        }
        ordered.append(&mut nodes[step.node_index].coplanar_polygons);
        if near_subtree != NO_BSP_NODE {
            traversal_stack.push(TraversalStep {
                node_index: near_subtree,
                ready_to_emit: false,
            });
        }
    }
    ordered
}

fn all_planes_are_parallel(polygons: &[PartitionedPolygon]) -> bool {
    // The cross product of two unit normals has the sine of the angle between the planes as its length.
    // Below a microradian of tilt the planes are treated as parallel.
    const MAXIMUM_PARALLEL_CROSS_LENGTH_SQUARED: f32 = 1e-12;
    let first_normal = polygons[0].plane_normal;
    for polygon in &polygons[1..] {
        let cross = polygon.plane_normal.cross(first_normal);
        if cross.dot(cross) > MAXIMUM_PARALLEL_CROSS_LENGTH_SQUARED {
            return false;
        }
    }
    true
}

fn sort_parallel_polygons_back_to_front(mut polygons: Vec<PartitionedPolygon>) -> Vec<BspPolygon> {
    // Fast path for parallel planes, where no splitting is needed.

    let mut axis = polygons[0].plane_normal;
    if axis.z < 0.0 {
        axis = -axis;
    }

    struct DepthOrderedPolygon {
        depth: f32,
        input_index: usize,
    }
    let mut order = Vec::with_capacity(polygons.len());
    for (input_index, polygon) in polygons.iter().enumerate() {
        let depth = if polygon.plane_normal.dot(axis) > 0.0 {
            polygon.plane_distance
        } else {
            -polygon.plane_distance
        };
        order.push(DepthOrderedPolygon { depth, input_index });
    }
    order.sort_by(|a, b| {
        a.depth
            .partial_cmp(&b.depth)
            .unwrap_or(Ordering::Equal)
            .then_with(|| a.input_index.cmp(&b.input_index))
    });

    let mut ordered = Vec::with_capacity(order.len());
    let mut polygons_by_index: Vec<Option<PartitionedPolygon>> = polygons.drain(..).map(Some).collect();
    for entry in order {
        ordered.push(polygons_by_index[entry.input_index].take().unwrap().polygon);
    }
    ordered
}

pub fn split_and_sort_polygons_back_to_front(polygons: Vec<BspPolygon>) -> Vec<BspPolygon> {
    let mut partitioned = Vec::with_capacity(polygons.len());
    for polygon in polygons {
        let Some(normal) = polygon_normal(&polygon.vertices) else {
            continue;
        };
        let distance = normal.dot(polygon.vertices[0]);
        partitioned.push(PartitionedPolygon {
            polygon,
            plane_normal: normal,
            plane_distance: distance,
        });
    }

    if !partitioned.is_empty() && all_planes_are_parallel(&partitioned) {
        return sort_parallel_polygons_back_to_front(partitioned);
    }

    collect_back_to_front(build_bsp_tree(partitioned))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vector(x: f32, y: f32, z: f32) -> FloatVector3 {
        FloatVector3 { x, y, z }
    }

    fn make_z_plane_polygon(z: f32, plane_index: usize) -> BspPolygon {
        BspPolygon {
            vertices: vec![
                vector(-10.0, -10.0, z),
                vector(10.0, -10.0, z),
                vector(10.0, 10.0, z),
                vector(-10.0, 10.0, z),
            ],
            plane_index,
            clipped: false,
        }
    }

    fn centroid_of(polygon: &BspPolygon) -> FloatVector3 {
        let mut sum = FloatVector3::default();
        for vertex in &polygon.vertices {
            sum = sum + *vertex;
        }
        sum * (1.0 / polygon.vertices.len() as f32)
    }

    #[test]
    fn map_rect_through_projection_maps_corners_in_order() {
        let vertices = map_rect_through_projection(FloatMatrix4x4::identity(), FloatRect::new(10.0, 20.0, 30.0, 40.0));
        assert_eq!(vertices.len(), 4);
        assert_eq!(vertices[0], vector(10.0, 20.0, 0.0));
        assert_eq!(vertices[1], vector(40.0, 20.0, 0.0));
        assert_eq!(vertices[2], vector(40.0, 60.0, 0.0));
        assert_eq!(vertices[3], vector(10.0, 60.0, 0.0));
    }

    #[test]
    fn map_rect_through_projection_performs_the_perspective_divide() {
        let mut matrix = FloatMatrix4x4::identity();
        matrix.elements[3][0] = 0.015625;
        let vertices = map_rect_through_projection(matrix, FloatRect::new(0.0, 0.0, 64.0, 32.0));
        assert_eq!(vertices.len(), 4);
        assert_eq!(vertices[0], vector(0.0, 0.0, 0.0));
        assert_eq!(vertices[1], vector(32.0, 0.0, 0.0));
        assert_eq!(vertices[2], vector(32.0, 16.0, 0.0));
        assert_eq!(vertices[3], vector(0.0, 32.0, 0.0));
    }

    #[test]
    fn map_rect_through_projection_clips_the_region_behind_the_eye() {
        let mut matrix = FloatMatrix4x4::identity();
        matrix.elements[3][0] = -0.03125;
        let vertices = map_rect_through_projection(matrix, FloatRect::new(0.0, 0.0, 64.0, 32.0));
        assert_eq!(vertices.len(), 4);
        assert_eq!(vertices[0], vector(0.0, 0.0, 0.0));
        assert!(vertices[1].x > 100_000.0);
        assert!(vertices[2].x > 100_000.0);
        assert!(vertices[2].y > 100_000.0);
        assert_eq!(vertices[3], vector(0.0, 32.0, 0.0));
    }

    #[test]
    fn map_rect_through_projection_drops_a_rect_entirely_behind_the_eye() {
        let mut matrix = FloatMatrix4x4::identity();
        matrix.elements[3][3] = -1.0;
        assert!(map_rect_through_projection(matrix, FloatRect::new(0.0, 0.0, 64.0, 32.0)).is_empty());
    }

    #[test]
    fn parallel_planes_sort_back_to_front_for_any_paint_order() {
        let mut polygons = Vec::new();
        for index in 0..20 {
            polygons.push(make_z_plane_polygon(((index * 7) % 20) as f32, index));
        }

        // Painting proceeds in ascending z, where the largest z is nearest the viewer and paints last.
        let sorted = split_and_sort_polygons_back_to_front(polygons);
        assert_eq!(sorted.len(), 20);
        for (index, polygon) in sorted.iter().enumerate() {
            assert!(!polygon.clipped);
            assert_eq!(polygon.vertices[0].z, index as f32);
        }
    }

    #[test]
    fn reversed_winding_does_not_affect_depth_order() {
        let polygons = vec![
            BspPolygon {
                vertices: vec![
                    vector(-10.0, 10.0, 5.0),
                    vector(10.0, 10.0, 5.0),
                    vector(10.0, -10.0, 5.0),
                    vector(-10.0, -10.0, 5.0),
                ],
                plane_index: 0,
                clipped: false,
            },
            make_z_plane_polygon(-5.0, 1),
        ];

        let sorted = split_and_sort_polygons_back_to_front(polygons);
        assert_eq!(sorted.len(), 2);
        assert_eq!(sorted[0].plane_index, 1);
        assert_eq!(sorted[1].plane_index, 0);
    }

    #[test]
    fn coplanar_polygons_keep_paint_order() {
        let polygons = (0..3).map(|index| make_z_plane_polygon(0.0, index)).collect();

        let sorted = split_and_sort_polygons_back_to_front(polygons);
        assert_eq!(sorted.len(), 3);
        for (index, polygon) in sorted.iter().enumerate() {
            assert_eq!(polygon.plane_index, index);
        }
    }

    #[test]
    fn coplanar_polygons_keep_paint_order_when_sorted_against_other_planes() {
        // The third plane is tilted so the planes do not count as parallel and ordering runs through the tree.
        let polygons = vec![
            make_z_plane_polygon(0.0, 0),
            make_z_plane_polygon(0.0, 1),
            BspPolygon {
                vertices: vec![
                    vector(-10.0, -10.0, -50.1),
                    vector(10.0, -10.0, -49.9),
                    vector(10.0, 10.0, -49.9),
                    vector(-10.0, 10.0, -50.1),
                ],
                plane_index: 2,
                clipped: false,
            },
        ];

        let sorted = split_and_sort_polygons_back_to_front(polygons);
        assert_eq!(sorted.len(), 3);
        assert_eq!(sorted[0].plane_index, 2);
        assert_eq!(sorted[1].plane_index, 0);
        assert_eq!(sorted[2].plane_index, 1);
    }

    #[test]
    fn intersecting_planes_are_split_into_ordered_pieces() {
        let polygons = vec![
            make_z_plane_polygon(0.0, 0),
            // A polygon on the plane z = x, crossing the first polygon along the line x = 0.
            BspPolygon {
                vertices: vec![
                    vector(-10.0, -10.0, -10.0),
                    vector(10.0, -10.0, 10.0),
                    vector(10.0, 10.0, 10.0),
                    vector(-10.0, 10.0, -10.0),
                ],
                plane_index: 1,
                clipped: false,
            },
        ];

        let sorted = split_and_sort_polygons_back_to_front(polygons);
        assert_eq!(sorted.len(), 3);

        // The crossing polygon stays whole and the flat one is cut into a piece on either side of it. The
        // piece with positive x lies behind the crossing plane and paints first.
        assert_eq!(sorted[0].plane_index, 0);
        assert!(sorted[0].clipped);
        assert!(centroid_of(&sorted[0]).x > 0.0);

        assert_eq!(sorted[1].plane_index, 1);
        assert!(!sorted[1].clipped);

        assert_eq!(sorted[2].plane_index, 0);
        assert!(sorted[2].clipped);
        assert!(centroid_of(&sorted[2]).x < 0.0);
    }

    #[test]
    fn polygons_without_a_plane_are_omitted() {
        let polygons = vec![
            BspPolygon {
                vertices: vec![vector(0.0, 0.0, 0.0), vector(1.0, 0.0, 0.0), vector(2.0, 0.0, 0.0)],
                plane_index: 0,
                clipped: false,
            },
            make_z_plane_polygon(0.0, 1),
        ];

        let sorted = split_and_sort_polygons_back_to_front(polygons);
        assert_eq!(sorted.len(), 1);
        assert_eq!(sorted[0].plane_index, 1);
    }

    #[test]
    fn small_distant_parallel_planes_sort_without_splitting() {
        // Two 8x8 parallel quads far from the origin, two units apart along their shared normal.
        let polygons = vec![
            BspPolygon {
                vertices: vec![
                    vector(101230.914, 99866.41, 3141.205),
                    vector(101236.88, 99868.2, 3146.2156),
                    vector(101235.51, 99876.0, 3145.0588),
                    vector(101229.54, 99874.195, 3140.0483),
                ],
                plane_index: 0,
                clipped: false,
            },
            BspPolygon {
                vertices: vec![
                    vector(101232.2, 99866.41, 3139.673),
                    vector(101238.17, 99868.2, 3144.6836),
                    vector(101236.8, 99876.0, 3143.5269),
                    vector(101230.83, 99874.195, 3138.5164),
                ],
                plane_index: 1,
                clipped: false,
            },
        ];

        let sorted = split_and_sort_polygons_back_to_front(polygons);
        assert_eq!(sorted.len(), 2);
        assert_eq!(sorted[0].plane_index, 1);
        assert!(!sorted[0].clipped);
        assert_eq!(sorted[1].plane_index, 0);
        assert!(!sorted[1].clipped);
    }
}
