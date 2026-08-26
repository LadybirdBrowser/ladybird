/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/HashTable.h>
#include <AK/StringBuilder.h>
#include <LibGfx/Matrix4x4.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

bool ClipData::contains(DevicePixelPoint point) const
{
    return corner_radii.contains(point.to_type<int>(), rect.to_type<int>());
}

static Atomic<u64> s_next_accumulated_visual_context_tree_version { 1 };

static TransformData identity_visual_viewport_transform()
{
    return { Gfx::FloatMatrix4x4::identity(), { 0.f, 0.f } };
}

static u64 next_accumulated_visual_context_tree_version()
{
    return s_next_accumulated_visual_context_tree_version.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

// Whole-tree transform root: the visual viewport transform for document trees, the content
// placement for nested display list trees, identity otherwise.
AccumulatedVisualContextTree::AccumulatedVisualContextTree(u64 version, TransformData root_transform, bool root_is_visual_viewport)
    : m_version(version)
    , m_root_is_visual_viewport(root_is_visual_viewport)
{
    m_spatial_nodes.append({ move(root_transform), VISUAL_VIEWPORT_NODE_INDEX });
}

AccumulatedVisualContextTree::AccumulatedVisualContextTree(u64 version, Vector<SpatialNode>&& spatial_nodes, Vector<FrameNode>&& frame_nodes, bool root_is_visual_viewport)
    : m_version(version)
    , m_spatial_nodes(move(spatial_nodes))
    , m_frame_nodes(move(frame_nodes))
    , m_root_is_visual_viewport(root_is_visual_viewport)
{
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create()
{
    return create(identity_visual_viewport_transform());
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create(TransformData visual_viewport_transform)
{
    return AccumulatedVisualContextTree { next_accumulated_visual_context_tree_version(), move(visual_viewport_transform), true };
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create_with_content_root(TransformData content_transform)
{
    return AccumulatedVisualContextTree { next_accumulated_visual_context_tree_version(), move(content_transform), false };
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create_with_content_offset(Gfx::IntPoint content_offset)
{
    return create_with_content_root(TransformData {
        Gfx::translation_matrix(Vector3<float>(static_cast<float>(content_offset.x()), static_cast<float>(content_offset.y()), 0)),
        {},
    });
}

CSSPixelRect apply_css_transform_to_rect(Layout::Node const& box, CSSPixelRect const& rect)
{
    auto transform_data = rust_compute_css_transform(box, 1.0);
    if (!transform_data.has_value())
        return rect;

    auto affine_transform = Gfx::extract_2d_affine_transform(transform_data->matrix);
    auto transformed_rect = rect.to_type<float>();
    transformed_rect.translate_by(-transform_data->origin);
    transformed_rect = affine_transform.map(transformed_rect);
    transformed_rect.translate_by(transform_data->origin);
    return transformed_rect.to_type<CSSPixels>();
}

SpatialNodeIndex AccumulatedVisualContextTree::append_spatial(SpatialData data, SpatialNodeIndex parent)
{
    VERIFY(parent.value() < m_spatial_nodes.size());
    auto index = SpatialNodeIndex(m_spatial_nodes.size());
    m_spatial_nodes.append({ move(data), parent });
    return index;
}

FrameNodeIndex AccumulatedVisualContextTree::append_frame(FrameData data, FrameNodeIndex parent, SpatialNodeIndex spatial)
{
    VERIFY(spatial.value() < m_spatial_nodes.size());
    bool empty_clip = false;
    if (parent != NO_FRAME_NODE) {
        VERIFY(parent.value() < m_frame_nodes.size());
        empty_clip = m_frame_nodes[parent.value()].has_empty_effective_clip;
    }
    if (!empty_clip) {
        if (data.has<ClipData>())
            empty_clip = data.get<ClipData>().rect.is_empty();
        else if (data.has<ClipPathData>())
            empty_clip = data.get<ClipPathData>().path.bounding_box().is_empty();
        else if (data.has<MaskData>())
            empty_clip = data.get<MaskData>().rect.is_empty();
    }

    auto index = FrameNodeIndex(m_frame_nodes.size());
    m_frame_nodes.append({ move(data), parent, spatial, empty_clip });
    return index;
}

void AccumulatedVisualContextTree::set_visual_viewport_transform(TransformData transform)
{
    VERIFY(!m_spatial_nodes.is_empty());
    VERIFY(m_spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.value()].data.has<TransformData>());
    m_spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.value()].data = move(transform);
}

bool AccumulatedVisualContextTree::is_compatible_with(AccumulatedVisualContextTree const& other) const
{
    if (m_spatial_nodes.size() != other.m_spatial_nodes.size() || m_frame_nodes.size() != other.m_frame_nodes.size())
        return false;

    for (size_t i = 0; i < m_spatial_nodes.size(); ++i) {
        auto const& node = m_spatial_nodes[i];
        auto const& other_node = other.m_spatial_nodes[i];
        if (node.parent != other_node.parent || node.data.index() != other_node.data.index())
            return false;
    }
    for (size_t i = 0; i < m_frame_nodes.size(); ++i) {
        auto const& node = m_frame_nodes[i];
        auto const& other_node = other.m_frame_nodes[i];
        if (node.parent != other_node.parent || node.spatial != other_node.spatial)
            return false;
        if (node.has_empty_effective_clip != other_node.has_empty_effective_clip)
            return false;
        if (node.data.index() != other_node.data.index())
            return false;
    }
    return true;
}

void AccumulatedVisualContextTree::reuse_version_from(AccumulatedVisualContextTree const& other)
{
    VERIFY(is_compatible_with(other));
    m_version = other.m_version;
}

Vector<SpatialNodeIndex, 8> AccumulatedVisualContextTree::build_ancestor_chain(SpatialNodeIndex index) const
{
    VERIFY(index.value() < m_spatial_nodes.size());
    Vector<SpatialNodeIndex, 8> chain;
    for (auto i = index;; i = m_spatial_nodes[i.value()].parent) {
        chain.append(i);
        if (i == VISUAL_VIEWPORT_NODE_INDEX)
            break;
    }
    return chain;
}

struct LocalSpatialMatrix {
    Gfx::FloatMatrix4x4 matrix;
    bool flattens_inherited_transform { false };
};

static LocalSpatialMatrix local_spatial_matrix(SpatialNode const& node, SpatialNodeIndex node_index, ScrollStateSnapshot const& scroll_state)
{
    return node.data.visit(
        [&](TransformData const& transform) {
            return LocalSpatialMatrix { transform.matrix_including_origin(), transform.flattens_inherited_transform };
        },
        [&](PerspectiveData const& perspective) {
            return LocalSpatialMatrix { perspective.matrix, perspective.flattens_inherited_transform };
        },
        [&](ScrollData const&) {
            auto offset = scroll_state.device_offset_for_index(node_index);
            return LocalSpatialMatrix { Gfx::translation_matrix(Vector3 { offset.x(), offset.y(), 0.f }) };
        },
        [&](ScrollCompensation const& compensation) {
            auto offset = scroll_state.device_offset_for_index(compensation.scroll_node_index);
            return LocalSpatialMatrix { Gfx::translation_matrix(Vector3 { -offset.x(), -offset.y(), 0.f }) };
        },
        [&](AnchorScrollShift const& shift) {
            auto offset = shift.masked_offset(scroll_state);
            return LocalSpatialMatrix { Gfx::translation_matrix(Vector3 { offset.x(), offset.y(), 0.f }) };
        },
        [&](BackfaceVisibilityData const& backface) { return LocalSpatialMatrix { Gfx::FloatMatrix4x4::identity(), backface.flattens_inherited_transform }; });
}

bool AccumulatedVisualContextTree::chain_contains_3d_transform(SpatialNodeIndex index) const
{
    for (auto i = index;; i = m_spatial_nodes[i.value()].parent) {
        auto const& node = m_spatial_nodes[i.value()];
        if (auto const* transform = node.data.get_pointer<TransformData>()) {
            if (!Gfx::is_2d_affine_transform(transform->matrix))
                return true;
        } else if (node.data.has<PerspectiveData>()) {
            return true;
        }
        if (i == VISUAL_VIEWPORT_NODE_INDEX)
            break;
    }
    return false;
}

// Homogeneous coordinates this close to the eye plane have no meaningful projection.
static constexpr float minimum_projection_w = 0.0001f;

static Gfx::FloatRect map_rect_through_matrix(Gfx::FloatMatrix4x4 const& matrix, Gfx::FloatRect const& rect)
{
    auto map_corner = [&](Gfx::FloatPoint point) {
        return matrix * Gfx::FloatVector4 { point.x(), point.y(), 0, 1 };
    };
    Array<Gfx::FloatVector4, 4> mapped_corners {
        map_corner(rect.top_left()),
        map_corner(rect.top_right()),
        map_corner(rect.bottom_left()),
        map_corner(rect.bottom_right()),
    };
    bool all_corners_behind_eye_plane = all_of(mapped_corners, [](auto const& corner) { return corner.w() <= 0; });
    // A rect entirely behind the eye plane divides through its negative homogeneous coordinates, matching the
    // geometry other engines report. A rect that crosses the eye plane projects without bound, so the corners
    // behind it are clamped to keep the result conservatively covering the rendered region.
    auto project_corner = [&](Gfx::FloatVector4 const& corner) -> Gfx::FloatPoint {
        auto w = all_corners_behind_eye_plane ? min(corner.w(), -minimum_projection_w) : max(corner.w(), minimum_projection_w);
        return { corner.x() / w, corner.y() / w };
    };
    auto top_left = project_corner(mapped_corners[0]);
    auto top_right = project_corner(mapped_corners[1]);
    auto bottom_left = project_corner(mapped_corners[2]);
    auto bottom_right = project_corner(mapped_corners[3]);
    auto left = min(min(top_left.x(), top_right.x()), min(bottom_left.x(), bottom_right.x()));
    auto right = max(max(top_left.x(), top_right.x()), max(bottom_left.x(), bottom_right.x()));
    auto top = min(min(top_left.y(), top_right.y()), min(bottom_left.y(), bottom_right.y()));
    auto bottom = max(max(top_left.y(), top_right.y()), max(bottom_left.y(), bottom_right.y()));
    return { left, top, right - left, bottom - top };
}

Optional<Gfx::FloatPoint> AccumulatedVisualContextTree::transform_point_for_hit_test(ContextRef context, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state, ClipBehavior clip_behavior) const
{
    auto spatial_chain = build_ancestor_chain(context.spatial);
    spatial_chain.reverse();

    Vector<FrameNodeIndex, 8> frame_chain;
    for (auto frame = context.frame; frame != NO_FRAME_NODE; frame = m_frame_nodes[frame.value()].parent)
        frame_chain.append(frame);
    frame_chain.reverse();

    // The backface test needs forward matrices, but this walk only applies inverses. When the chain contains
    // backface markers, we accumulate the forward matrices as we walk, from the root down, so a marker can look up
    // the matrix at its plane root by chain position.
    bool chain_has_backface_marker = any_of(spatial_chain, [&](SpatialNodeIndex index) {
        return m_spatial_nodes[index.value()].data.has<BackfaceVisibilityData>();
    });
    bool chain_has_3d_transform = chain_contains_3d_transform(context.spatial);
    bool needs_accumulated_matrices = chain_has_backface_marker || chain_has_3d_transform;
    Vector<Gfx::FloatMatrix4x4, 8> accumulated_matrices;
    if (needs_accumulated_matrices)
        accumulated_matrices.ensure_capacity(spatial_chain.size());

    auto point = screen_point;
    auto apply_spatial_step = [&](size_t position) -> bool {
        auto node_index = spatial_chain[position];
        auto const& node = m_spatial_nodes[node_index.value()];

        if (needs_accumulated_matrices) {
            auto local = local_spatial_matrix(node, node_index, scroll_state);
            if (position == 0) {
                accumulated_matrices.unchecked_append(local.matrix);
            } else {
                auto const& parent_matrix = accumulated_matrices.last();
                accumulated_matrices.unchecked_append((local.flattens_inherited_transform ? Gfx::flattened(parent_matrix) : parent_matrix) * local.matrix);
            }
        }

        if (chain_has_3d_transform && !node.data.has<BackfaceVisibilityData>()) {
            auto inverse = Gfx::flattened(accumulated_matrices.last()).inverse();
            if (!inverse.has_value())
                return false;
            auto mapped = *inverse * Gfx::FloatVector4 { screen_point.x(), screen_point.y(), 0, 1 };
            if (mapped.w() < minimum_projection_w)
                return false;
            point = { mapped.x() / mapped.w(), mapped.y() / mapped.w() };
            return true;
        }

        return node.data.visit(
            [&](PerspectiveData const& perspective) {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return false;
                point = inverse->map(point);
                return true;
            },
            [&](BackfaceVisibilityData const& backface) {
                auto plane_root_position = spatial_chain.find_first_index(backface.plane_root_index).value();
                return !should_cull_back_face(accumulated_matrices.last(), accumulated_matrices[plane_root_position]);
            },
            [&](ScrollData const&) {
                point.translate_by(-scroll_state.device_offset_for_index(node_index));
                return true;
            },
            [&](TransformData const& transform) {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return false;

                auto offset_point = point - transform.origin;
                auto transformed = inverse->map(offset_point);
                point = transformed + transform.origin;
                return true;
            },
            [&](ScrollCompensation const& compensation) {
                point.translate_by(scroll_state.device_offset_for_index(compensation.scroll_node_index));
                return true;
            },
            [&](AnchorScrollShift const& shift) {
                point.translate_by(-shift.masked_offset(scroll_state));
                return true;
            });
    };

    // Frames along the chain attach to spatial nodes in non-decreasing depth order, so the steps
    // still to apply before a frame always start where the previous frame left off.
    size_t applied_spatial_steps = 0;
    auto apply_spatial_steps_through_node = [&](SpatialNodeIndex node_index) -> bool {
        if (applied_spatial_steps > 0 && spatial_chain[applied_spatial_steps - 1] == node_index)
            return true;
        for (;; ++applied_spatial_steps) {
            VERIFY(applied_spatial_steps < spatial_chain.size());
            if (!apply_spatial_step(applied_spatial_steps))
                return false;
            if (spatial_chain[applied_spatial_steps] == node_index) {
                ++applied_spatial_steps;
                return true;
            }
        }
    };

    auto point_passes_frame = [&](FrameNode const& frame) {
        return frame.data.visit(
            [&](ClipData const& clip) {
                if (clip_behavior == ClipBehavior::Ignore)
                    return true;
                // NOTE: The clip rect is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                return clip.contains(point.to_type<int>().to_type<DevicePixels>());
            },
            [&](ClipPathData const& clip_path) {
                if (clip_behavior == ClipBehavior::Ignore)
                    return true;
                // NOTE: The clip path is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                if (!clip_path.bounding_rect.contains(point.to_type<int>().to_type<DevicePixels>()))
                    return false;
                return clip_path.path.contains(point, clip_path.fill_rule);
            },
            [&](EffectsData const&) { return true; },
            [&](MaskData const&) { return true; });
    };

    for (auto frame_index : frame_chain) {
        auto const& frame = m_frame_nodes[frame_index.value()];
        if (!apply_spatial_steps_through_node(frame.spatial))
            return {};
        if (!point_passes_frame(frame))
            return {};
    }
    if (!apply_spatial_steps_through_node(context.spatial))
        return {};

    return point;
}

SpatialNodeIndex SortingContexts::outermost_context_of(SpatialNodeIndex context) const
{
    for (;;) {
        auto link = links.get(context.value());
        if (!link.has_value() || link->parent_context == NO_SORTING_CONTEXT)
            return context;
        context = link->parent_context;
    }
}

SortingContexts AccumulatedVisualContextTree::resolve_sorting_contexts() const
{
    auto node_count = m_spatial_nodes.size();

    Vector<bool> is_sorting_context_root;
    is_sorting_context_root.resize(node_count);
    bool has_sorting_context_roots = false;
    for (auto const& node : m_spatial_nodes) {
        if (auto const* transform = node.data.get_pointer<TransformData>(); transform && transform->sorting_context_root_index.has_value()) {
            is_sorting_context_root[transform->sorting_context_root_index->value()] = true;
            has_sorting_context_roots = true;
        }
    }
    if (!has_sorting_context_roots)
        return {};

    // Roots always precede their contexts' nodes, so a single forward walk resolves every node.
    SortingContexts contexts;
    contexts.leaf_by_node.ensure_capacity(node_count);
    contexts.context_by_node.ensure_capacity(node_count);
    for (size_t i = 0; i < node_count; ++i) {
        auto index = SpatialNodeIndex { static_cast<u32>(i) };
        auto parent = m_spatial_nodes[i].parent.value();
        auto inherited_leaf = i == 0 ? NO_SORTING_CONTEXT : contexts.leaf_by_node[parent];
        auto inherited_context = i == 0 ? NO_SORTING_CONTEXT : contexts.context_by_node[parent];
        auto const* transform = m_spatial_nodes[i].data.get_pointer<TransformData>();
        if (transform && transform->sorting_context_root_index.has_value()) {
            contexts.leaf_by_node.unchecked_append(index);
            contexts.context_by_node.unchecked_append(*transform->sorting_context_root_index);
        } else if (is_sorting_context_root[i]) {
            contexts.links.set(index.value(), { inherited_context, inherited_leaf });
            contexts.leaf_by_node.unchecked_append(index);
            contexts.context_by_node.unchecked_append(index);
        } else {
            contexts.leaf_by_node.unchecked_append(inherited_leaf);
            contexts.context_by_node.unchecked_append(inherited_context);
        }
    }
    return contexts;
}

Optional<float> AccumulatedVisualContextTree::plane_depth_at_point_for_hit_test(SpatialNodeIndex plane_node_index, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state) const
{
    auto chain = build_ancestor_chain(plane_node_index);
    auto accumulated_matrix = Gfx::FloatMatrix4x4::identity();
    for (size_t i = chain.size(); i > 0; --i) {
        auto node_index = chain[i - 1];
        auto local = local_spatial_matrix(m_spatial_nodes[node_index.value()], node_index, scroll_state);
        accumulated_matrix = (local.flattens_inherited_transform ? Gfx::flattened(accumulated_matrix) : accumulated_matrix) * local.matrix;
    }

    auto inverse = accumulated_matrix.inverse();
    if (!inverse.has_value())
        return {};

    auto const& matrix = *inverse;
    auto depth = -(screen_point.x() * matrix[2, 0] + screen_point.y() * matrix[2, 1] + matrix[2, 3]) / matrix[2, 2];
    if (!isfinite(depth))
        return {};
    return depth;
}

Gfx::FloatPoint AccumulatedVisualContextTree::inverse_transform_point(SpatialNodeIndex index, Gfx::FloatPoint screen_point) const
{
    auto chain = build_ancestor_chain(index);

    // This walk deliberately skips translation-only nodes. Callers resolve offsets and scroll positions
    // themselves. The per-node inverses below are only exact for chains of 2D transforms, so a chain containing a
    // 3D transform inverts the flattened accumulated matrix, mapping the screen point onto the plane the content
    // was rendered into.
    if (chain_contains_3d_transform(index)) {
        auto matrix = Gfx::FloatMatrix4x4::identity();
        for (size_t i = chain.size(); i > 0; --i) {
            auto const& node = m_spatial_nodes[chain[i - 1].value()];
            node.data.visit(
                [&](TransformData const& transform) {
                    matrix = (transform.flattens_inherited_transform ? Gfx::flattened(matrix) : matrix) * transform.matrix_including_origin();
                },
                [&](PerspectiveData const& perspective) {
                    matrix = (perspective.flattens_inherited_transform ? Gfx::flattened(matrix) : matrix) * perspective.matrix;
                },
                [&](auto const&) {});
        }
        auto inverse = Gfx::flattened(matrix).inverse();
        if (!inverse.has_value())
            return screen_point;
        auto mapped = *inverse * Gfx::FloatVector4 { screen_point.x(), screen_point.y(), 0, 1 };
        if (mapped.w() < minimum_projection_w)
            return screen_point;
        return { mapped.x() / mapped.w(), mapped.y() / mapped.w() };
    }

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const& node = m_spatial_nodes[chain[i - 1].value()];

        node.data.visit(
            [&](PerspectiveData const& perspective) {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (inverse.has_value())
                    point = inverse->map(point);
            },
            [&](TransformData const& transform) {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (inverse.has_value()) {
                    auto offset_point = point - transform.origin;
                    auto transformed = inverse->map(offset_point);
                    point = transformed + transform.origin;
                }
            },
            [&](auto const&) {});
    }

    return point;
}

Gfx::FloatMatrix4x4 AccumulatedVisualContextTree::accumulated_matrix(SpatialNodeIndex index, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto chain = build_ancestor_chain(index);
    auto matrix = Gfx::FloatMatrix4x4::identity();
    for (size_t i = chain.size(); i > 0; --i) {
        auto node_index = chain[i - 1];
        if (node_index == VISUAL_VIEWPORT_NODE_INDEX && m_root_is_visual_viewport && include_visual_viewport_transform == IncludeVisualViewportTransform::No)
            continue;
        auto local = local_spatial_matrix(m_spatial_nodes[node_index.value()], node_index, scroll_state);
        matrix = (local.flattens_inherited_transform ? Gfx::flattened(matrix) : matrix) * local.matrix;
    }
    return matrix;
}

Gfx::FloatSize AccumulatedVisualContextTree::accumulated_2d_scale(SpatialNodeIndex index, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto affine = Gfx::extract_2d_affine_transform(accumulated_matrix(index, scroll_state, include_visual_viewport_transform));
    return { affine.x_scale(), affine.y_scale() };
}

Gfx::FloatRect AccumulatedVisualContextTree::transform_rect_to_viewport(SpatialNodeIndex index, Gfx::FloatRect const& source_rect, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    // A chain with three-dimensional transforms cannot be applied one two-dimensional projection at a time.
    if (chain_contains_3d_transform(index)) {
        return map_rect_through_matrix(accumulated_matrix(index, scroll_state, include_visual_viewport_transform), source_rect);
    }

    auto rect = source_rect;
    for (auto i = index;; i = m_spatial_nodes[i.value()].parent) {
        auto const& node = m_spatial_nodes[i.value()];
        if (i != VISUAL_VIEWPORT_NODE_INDEX || !m_root_is_visual_viewport || include_visual_viewport_transform == IncludeVisualViewportTransform::Yes) {
            node.data.visit(
                [&](TransformData const& transform) {
                    auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                    rect.translate_by(-transform.origin);
                    rect = affine.map(rect);
                    rect.translate_by(transform.origin);
                },
                [&](PerspectiveData const& perspective) {
                    auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                    rect = affine.map(rect);
                },
                [&](ScrollData const&) {
                    rect.translate_by(scroll_state.device_offset_for_index(i));
                },
                [&](ScrollCompensation const& compensation) {
                    rect.translate_by(-scroll_state.device_offset_for_index(compensation.scroll_node_index));
                },
                [&](AnchorScrollShift const& shift) {
                    rect.translate_by(shift.masked_offset(scroll_state));
                },
                [&](BackfaceVisibilityData const&) {});
        }
        if (i == VISUAL_VIEWPORT_NODE_INDEX)
            break;
    }

    return rect;
}

Gfx::FloatMatrix4x4 TransformData::matrix_including_origin() const
{
    auto origin_translation = Gfx::translation_matrix(Gfx::Vector3<float> { origin.x(), origin.y(), 0 });
    auto inverse_origin_translation = Gfx::translation_matrix(Gfx::Vector3<float> { -origin.x(), -origin.y(), 0 });
    return origin_translation * matrix * inverse_origin_translation;
}

bool should_cull_back_face(Gfx::FloatMatrix4x4 const& accumulated_matrix, Gfx::FloatMatrix4x4 const& plane_root_matrix)
{
    auto inverse_plane_root_matrix = Gfx::flattened(plane_root_matrix).inverse();
    if (!inverse_plane_root_matrix.has_value())
        return false;
    return Gfx::is_back_face_visible(*inverse_plane_root_matrix * accumulated_matrix);
}

Gfx::FloatPoint AnchorScrollShift::masked_offset(ScrollStateSnapshot const& scroll_state) const
{
    auto offset = scroll_state.device_offset_for_index(scroll_node_index);
    if (!compensate_horizontal_scroll)
        offset.set_x(0);
    if (!compensate_vertical_scroll)
        offset.set_y(0);
    return negate ? -offset : offset;
}

void AccumulatedVisualContextTree::dump_spatial_node(SpatialNodeIndex index, StringBuilder& builder) const
{
    m_spatial_nodes[index.value()].data.visit(
        [&](PerspectiveData const&) {
            builder.append("perspective"sv);
        },
        [&](BackfaceVisibilityData const& backface) {
            builder.appendff("backface-hidden plane_root={}", backface.plane_root_index);
        },
        [&](ScrollData const& scroll) {
            builder.append("scroll"sv);
            if (scroll.is_sticky)
                builder.append(" (sticky)"sv);
        },
        [&](TransformData const& transform) {
            auto const& matrix = transform.matrix.elements();
            auto const& origin = transform.origin;
            builder.appendff("{}=[{},{},{},{},{},{}] origin=({},{})",
                transform.role == TransformDataRole::SvgViewportTransform ? "svg-viewport-transform"sv : "transform"sv,
                matrix[0][0], matrix[0][1], matrix[1][0], matrix[1][1], matrix[0][3], matrix[1][3], origin.x(), origin.y());
        },
        [&](ScrollCompensation const& compensation) {
            builder.appendff("scroll_compensation(node_index={})", compensation.scroll_node_index);
        },
        [&](AnchorScrollShift const& shift) {
            builder.appendff("anchor_scroll_shift(node_index={}{}{}{})", shift.scroll_node_index,
                shift.negate ? ", negate"sv : ""sv,
                shift.compensate_horizontal_scroll ? ""sv : ", no-x"sv,
                shift.compensate_vertical_scroll ? ""sv : ", no-y"sv);
        });
}

void AccumulatedVisualContextTree::dump_frame_node(FrameNodeIndex index, StringBuilder& builder) const
{
    m_frame_nodes[index.value()].data.visit(
        [&](ClipData const& clip) {
            auto const& rect = clip.rect;
            builder.appendff("clip=[{},{} {}x{}]", rect.x(), rect.y(), rect.width(), rect.height());

            if (clip.corner_radii.has_any_radius()) {
                auto const& corner_radii = clip.corner_radii;
                builder.appendff(" radii=({},{},{},{})", corner_radii.top_left.horizontal_radius, corner_radii.top_right.horizontal_radius, corner_radii.bottom_right.horizontal_radius, corner_radii.bottom_left.horizontal_radius);
            }
        },
        [&](ClipPathData const& clip_path) {
            auto const& rect = clip_path.bounding_rect;
            builder.appendff("clip_path=[bounds: {},{} {}x{}, path: {}]", rect.x(), rect.y(), rect.width(), rect.height(), clip_path.path.to_svg_string());
        },
        [&](EffectsData const& effects) {
            builder.append("effects=["sv);
            bool has_content = false;
            if (effects.opacity < 1.0f) {
                builder.appendff("opacity={}", effects.opacity);
                has_content = true;
            }
            if (effects.blend_mode != Gfx::CompositingAndBlendingOperator::Normal) {
                if (has_content)
                    builder.append(' ');
                builder.appendff("blend_mode={}", static_cast<int>(effects.blend_mode));
                has_content = true;
            }
            if (effects.gfx_filter.has_value()) {
                if (has_content)
                    builder.append(' ');
                builder.append("filter"sv);
                has_content = true;
            }
            builder.append("]"sv);
        },
        [&](MaskData const& mask) {
            auto const& rect = mask.rect;
            auto kind = mask.kind == Gfx::MaskKind::Alpha ? "alpha"sv : "luminance"sv;
            auto origin = [&] {
                switch (mask.origin) {
                case MaskLayerOrigin::CssMaskLayers:
                    return "css-mask-layers"sv;
                case MaskLayerOrigin::SvgMask:
                    return "svg-mask"sv;
                case MaskLayerOrigin::SvgClip:
                    return "svg-clip"sv;
                }
                VERIFY_NOT_REACHED();
            }();
            builder.appendff("mask=[{},{} {}x{}] kind={} origin={}", rect.x(), rect.y(), rect.width(), rect.height(), kind, origin);
        });
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollData const& data)
{
    TRY(encoder.encode(data.is_sticky));
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollData> decode(Decoder& decoder)
{
    return Web::Painting::ScrollData {
        .is_sticky = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ClipData const& data)
{
    TRY(encoder.encode(data.rect));
    TRY(encoder.encode(data.corner_radii));
    return {};
}

template<>
ErrorOr<Web::Painting::ClipData> decode(Decoder& decoder)
{
    return Web::Painting::ClipData {
        TRY(decoder.decode<Web::DevicePixelRect>()),
        TRY(decoder.decode<Gfx::CornerRadii>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::TransformData const& data)
{
    TRY(encoder.encode(data.matrix));
    TRY(encoder.encode(data.origin));
    TRY(encoder.encode(data.sorting_context_root_index));
    TRY(encoder.encode(data.flattens_inherited_transform));
    TRY(encoder.encode(data.role));
    TRY(encoder.encode(data.synthetic_plane));
    return {};
}

template<>
ErrorOr<Web::Painting::TransformData> decode(Decoder& decoder)
{
    return Web::Painting::TransformData {
        .matrix = TRY(decoder.decode<Gfx::FloatMatrix4x4>()),
        .origin = TRY(decoder.decode<Gfx::FloatPoint>()),
        .sorting_context_root_index = TRY(decoder.decode<Optional<Web::Painting::SpatialNodeIndex>>()),
        .flattens_inherited_transform = TRY(decoder.decode<bool>()),
        .role = TRY(decoder.decode<Web::Painting::TransformDataRole>()),
        .synthetic_plane = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::PerspectiveData const& data)
{
    TRY(encoder.encode(data.matrix));
    TRY(encoder.encode(data.flattens_inherited_transform));
    return {};
}

template<>
ErrorOr<Web::Painting::PerspectiveData> decode(Decoder& decoder)
{
    return Web::Painting::PerspectiveData {
        .matrix = TRY(decoder.decode<Gfx::FloatMatrix4x4>()),
        .flattens_inherited_transform = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::BackfaceVisibilityData const& data)
{
    TRY(encoder.encode(data.plane_root_index));
    TRY(encoder.encode(data.flattens_inherited_transform));
    return {};
}

template<>
ErrorOr<Web::Painting::BackfaceVisibilityData> decode(Decoder& decoder)
{
    return Web::Painting::BackfaceVisibilityData {
        .plane_root_index = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>()),
        .flattens_inherited_transform = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ClipPathData const& data)
{
    TRY(encoder.encode(data.path));
    TRY(encoder.encode(data.bounding_rect));
    TRY(encoder.encode(data.fill_rule));
    return {};
}

template<>
ErrorOr<Web::Painting::ClipPathData> decode(Decoder& decoder)
{
    return Web::Painting::ClipPathData {
        .path = TRY(decoder.decode<Gfx::Path>()),
        .bounding_rect = TRY(decoder.decode<Web::DevicePixelRect>()),
        .fill_rule = TRY(decoder.decode<Gfx::WindingRule>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::EffectsData const& data)
{
    TRY(encoder.encode(data.opacity));
    TRY(encoder.encode(data.blend_mode));
    TRY(encoder.encode(data.gfx_filter));
    return {};
}

template<>
ErrorOr<Web::Painting::EffectsData> decode(Decoder& decoder)
{
    return Web::Painting::EffectsData {
        .opacity = TRY(decoder.decode<float>()),
        .blend_mode = TRY(decoder.decode<Gfx::CompositingAndBlendingOperator>()),
        .gfx_filter = TRY(decoder.decode<Optional<Gfx::Filter>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::MaskData const& data)
{
    TRY(encoder.encode(data.rect));
    TRY(encoder.encode(data.kind));
    TRY(encoder.encode(data.origin));
    return {};
}

template<>
ErrorOr<Web::Painting::MaskData> decode(Decoder& decoder)
{
    return Web::Painting::MaskData {
        .rect = TRY(decoder.decode<Web::DevicePixelRect>()),
        .kind = TRY(decoder.decode<Gfx::MaskKind>()),
        .origin = TRY(decoder.decode<Web::Painting::MaskLayerOrigin>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollCompensation const& data)
{
    TRY(encoder.encode(data.scroll_node_index));
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollCompensation> decode(Decoder& decoder)
{
    return Web::Painting::ScrollCompensation {
        .scroll_node_index = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::AnchorScrollShift const& data)
{
    TRY(encoder.encode(data.scroll_node_index));
    TRY(encoder.encode(data.negate));
    TRY(encoder.encode(data.compensate_horizontal_scroll));
    TRY(encoder.encode(data.compensate_vertical_scroll));
    return {};
}

template<>
ErrorOr<Web::Painting::AnchorScrollShift> decode(Decoder& decoder)
{
    return Web::Painting::AnchorScrollShift {
        .scroll_node_index = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>()),
        .negate = TRY(decoder.decode<bool>()),
        .compensate_horizontal_scroll = TRY(decoder.decode<bool>()),
        .compensate_vertical_scroll = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::SpatialNode const& node)
{
    TRY(encoder.encode(node.data));
    TRY(encoder.encode(node.parent));
    return {};
}

template<>
ErrorOr<Web::Painting::SpatialNode> decode(Decoder& decoder)
{
    return Web::Painting::SpatialNode {
        .data = TRY(decoder.decode<Web::Painting::SpatialData>()),
        .parent = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::FrameNode const& node)
{
    TRY(encoder.encode(node.data));
    TRY(encoder.encode(node.parent));
    TRY(encoder.encode(node.spatial));
    TRY(encoder.encode(node.has_empty_effective_clip));
    return {};
}

template<>
ErrorOr<Web::Painting::FrameNode> decode(Decoder& decoder)
{
    return Web::Painting::FrameNode {
        .data = TRY(decoder.decode<Web::Painting::FrameData>()),
        .parent = TRY(decoder.decode<Web::Painting::FrameNodeIndex>()),
        .spatial = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>()),
        .has_empty_effective_clip = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ContextRef const& context)
{
    TRY(encoder.encode(context.spatial));
    TRY(encoder.encode(context.frame));
    return {};
}

template<>
ErrorOr<Web::Painting::ContextRef> decode(Decoder& decoder)
{
    return Web::Painting::ContextRef {
        .spatial = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>()),
        .frame = TRY(decoder.decode<Web::Painting::FrameNodeIndex>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::AccumulatedVisualContextTree const& tree)
{
    TRY(encoder.encode(tree.m_version));
    TRY(encoder.encode(tree.m_spatial_nodes));
    TRY(encoder.encode(tree.m_frame_nodes));
    TRY(encoder.encode(tree.m_root_is_visual_viewport));
    return {};
}

template<>
ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder& decoder)
{
    using namespace Web::Painting;
    auto version = TRY(decoder.decode<u64>());
    auto spatial_nodes = TRY(decoder.decode<Vector<SpatialNode>>());
    auto frame_nodes = TRY(decoder.decode<Vector<FrameNode>>());
    auto root_is_visual_viewport = TRY(decoder.decode<bool>());
    if (spatial_nodes.is_empty())
        return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree missing visual viewport node");
    if (!spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.value()].data.has<TransformData>())
        return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree visual viewport node is not a transform");
    auto spatial_count = spatial_nodes.size();
    for (size_t i = 0; i < spatial_count; ++i) {
        if (spatial_nodes[i].parent.value() >= max(i, static_cast<size_t>(1)))
            return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree spatial node parent does not precede it");
        auto referenced_spatial_precedes_node = spatial_nodes[i].data.visit(
            [&](TransformData const& transform) { return !transform.sorting_context_root_index.has_value() || transform.sorting_context_root_index->value() < i; },
            [&](BackfaceVisibilityData const& backface) { return backface.plane_root_index.value() < i; },
            [&](ScrollCompensation const& compensation) { return compensation.scroll_node_index.value() < i; },
            [&](AnchorScrollShift const& shift) { return shift.scroll_node_index.value() < i; },
            [&](auto const&) { return true; });
        if (!referenced_spatial_precedes_node)
            return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree spatial node references a node that does not precede it");
    }
    for (size_t i = 0; i < frame_nodes.size(); ++i) {
        if (frame_nodes[i].parent != NO_FRAME_NODE && frame_nodes[i].parent.value() >= i)
            return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree frame node parent does not precede it");
        if (frame_nodes[i].spatial.value() >= spatial_count)
            return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree frame node spatial index out of range");
    }
    return AccumulatedVisualContextTree { version, move(spatial_nodes), move(frame_nodes), root_is_visual_viewport };
}

}
