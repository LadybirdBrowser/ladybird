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
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

bool ClipData::contains(Gfx::FloatPoint point) const
{
    auto integral_rect = rect.to_type<int>();
    if (integral_rect.to_type<float>() == rect)
        return corner_radii.contains(point.to_type<int>(), integral_rect);
    return corner_radii.contains(point, rect);
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

SpatialNodeIndex AccumulatedVisualContextTree::append_spatial(SpatialData data, SpatialNodeIndex parent)
{
    VERIFY(parent.value() < m_spatial_nodes.size());
    auto index = SpatialNodeIndex(m_spatial_nodes.size());
    m_spatial_nodes.append({ move(data), parent });
    return index;
}

static bool frame_data_clips_everything(FrameData const& data)
{
    return data.visit(
        [](ClipData const& clip) { return clip.mode == ClipMode::Intersect && clip.rect.is_empty(); },
        [](ClipPathData const& clip_path) { return clip_path.path.bounding_box().is_empty(); },
        [](EffectsData const&) { return false; },
        [](MaskData const& mask) { return mask.rect.is_empty(); });
}

FrameNodeIndex AccumulatedVisualContextTree::append_frame(FrameData data, FrameNodeIndex parent, SpatialNodeIndex spatial)
{
    VERIFY(spatial.value() < m_spatial_nodes.size());
    VERIFY(parent == NO_FRAME_NODE || parent.value() < m_frame_nodes.size());
    auto index = FrameNodeIndex(m_frame_nodes.size());
    bool clips_everything = frame_data_clips_everything(data);
    m_frame_nodes.append({ move(data), parent, spatial, clips_everything });
    return index;
}

void AccumulatedVisualContextTree::set_frame_data(FrameNodeIndex index, FrameData data)
{
    auto& node = m_frame_nodes[index.value()];
    node.clips_everything = frame_data_clips_everything(data);
    node.data = move(data);
}

Vector<bool> AccumulatedVisualContextTree::frames_with_empty_effective_clip() const
{
    Vector<bool> empty;
    empty.ensure_capacity(m_frame_nodes.size());
    for (auto const& node : m_frame_nodes)
        empty.unchecked_append(node.clips_everything || (node.parent != NO_FRAME_NODE && empty[node.parent.value()]));
    return empty;
}

void AccumulatedVisualContextTree::set_visual_viewport_transform(TransformData transform)
{
    VERIFY(!m_spatial_nodes.is_empty());
    VERIFY(m_spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.value()].data.has<TransformData>());
    m_spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.value()].data = move(transform);
}

void AccumulatedVisualContextTree::reuse_version_from(AccumulatedVisualContextTree const& other)
{
    VERIFY(m_spatial_nodes.size() == other.m_spatial_nodes.size());
    VERIFY(m_frame_nodes.size() == other.m_frame_nodes.size());
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
        [&](StickyData const&) {
            auto offset = scroll_state.device_offset_for_index(node_index);
            return LocalSpatialMatrix { Gfx::translation_matrix(Vector3 { offset.x(), offset.y(), 0.f }) };
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

// One root path of spatial nodes with the screen point mapped down it one inverse step at a time. Frames attach
// to a path in non-decreasing depth order, so the steps still to apply before a frame always start where the
// previous frame left off.
struct SpatialChainWalk {
    Vector<SpatialNodeIndex, 8> chain;
    bool needs_accumulated_matrices { false };
    bool has_3d_transform { false };
    Vector<Gfx::FloatMatrix4x4, 8> accumulated_matrices;
    Gfx::FloatPoint point;
    size_t applied_steps { 0 };
};

Optional<Gfx::FloatPoint> AccumulatedVisualContextTree::transform_point_for_hit_test(ContextRef context, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state, ClipBehavior clip_behavior) const
{
    Vector<FrameNodeIndex, 8> frame_chain;
    for (auto frame = context.frame; frame != NO_FRAME_NODE; frame = m_frame_nodes[frame.value()].parent)
        frame_chain.append(frame);
    frame_chain.reverse();

    // The backface test needs forward matrices, but this walk only applies inverses. When the chain contains
    // backface markers, we accumulate the forward matrices as we walk, from the root down, so a marker can look up
    // the matrix at its plane root by chain position.
    auto begin_walk = [&](SpatialNodeIndex leaf) {
        SpatialChainWalk walk;
        walk.chain = build_ancestor_chain(leaf);
        walk.chain.reverse();
        bool chain_has_backface_marker = any_of(walk.chain, [&](SpatialNodeIndex index) {
            return m_spatial_nodes[index.value()].data.has<BackfaceVisibilityData>();
        });
        walk.has_3d_transform = chain_contains_3d_transform(leaf);
        walk.needs_accumulated_matrices = chain_has_backface_marker || walk.has_3d_transform;
        if (walk.needs_accumulated_matrices)
            walk.accumulated_matrices.ensure_capacity(walk.chain.size());
        walk.point = screen_point;
        return walk;
    };

    auto apply_spatial_step = [&](SpatialChainWalk& walk, size_t position) -> bool {
        auto node_index = walk.chain[position];
        auto const& node = m_spatial_nodes[node_index.value()];

        if (walk.needs_accumulated_matrices) {
            auto local = local_spatial_matrix(node, node_index, scroll_state);
            if (position == 0) {
                walk.accumulated_matrices.unchecked_append(local.matrix);
            } else {
                auto const& parent_matrix = walk.accumulated_matrices.last();
                walk.accumulated_matrices.unchecked_append((local.flattens_inherited_transform ? Gfx::flattened(parent_matrix) : parent_matrix) * local.matrix);
            }
        }

        if (walk.has_3d_transform && !node.data.has<BackfaceVisibilityData>()) {
            auto inverse = Gfx::flattened(walk.accumulated_matrices.last()).inverse();
            if (!inverse.has_value())
                return false;
            auto mapped = *inverse * Gfx::FloatVector4 { screen_point.x(), screen_point.y(), 0, 1 };
            if (mapped.w() < minimum_projection_w)
                return false;
            walk.point = { mapped.x() / mapped.w(), mapped.y() / mapped.w() };
            return true;
        }

        return node.data.visit(
            [&](PerspectiveData const& perspective) {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return false;
                walk.point = inverse->map(walk.point);
                return true;
            },
            [&](BackfaceVisibilityData const& backface) {
                auto plane_root_position = walk.chain.find_first_index(backface.plane_root_index).value();
                return !should_cull_back_face(walk.accumulated_matrices.last(), walk.accumulated_matrices[plane_root_position]);
            },
            [&](ScrollData const&) {
                walk.point.translate_by(-scroll_state.device_offset_for_index(node_index));
                return true;
            },
            [&](StickyData const&) {
                walk.point.translate_by(-scroll_state.device_offset_for_index(node_index));
                return true;
            },
            [&](TransformData const& transform) {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return false;

                auto offset_point = walk.point - transform.origin;
                auto transformed = inverse->map(offset_point);
                walk.point = transformed + transform.origin;
                return true;
            },
            [&](AnchorScrollShift const& shift) {
                walk.point.translate_by(-shift.masked_offset(scroll_state));
                return true;
            });
    };

    auto apply_spatial_steps_through_node = [&](SpatialChainWalk& walk, SpatialNodeIndex node_index) -> bool {
        if (walk.applied_steps > 0 && walk.chain[walk.applied_steps - 1] == node_index)
            return true;
        for (;; ++walk.applied_steps) {
            VERIFY(walk.applied_steps < walk.chain.size());
            if (!apply_spatial_step(walk, walk.applied_steps))
                return false;
            if (walk.chain[walk.applied_steps] == node_index) {
                ++walk.applied_steps;
                return true;
            }
        }
    };

    auto point_passes_frame = [&](FrameNode const& frame, Gfx::FloatPoint point) {
        return frame.data.visit(
            [&](ClipData const& clip) {
                if (clip_behavior == ClipBehavior::Ignore)
                    return true;
                // NOTE: The clip rect is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                bool inside = clip.contains(point);
                return clip.mode == ClipMode::Intersect ? inside : !inside;
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

    auto context_walk = begin_walk(context.spatial);
    for (auto frame_index : frame_chain) {
        auto const& frame = m_frame_nodes[frame_index.value()];
        if (context_walk.chain.contains_slow(frame.spatial)) {
            if (!apply_spatial_steps_through_node(context_walk, frame.spatial) || !point_passes_frame(frame, context_walk.point))
                return {};
            continue;
        }
        // A fixed-background context is rooted above the scroll nodes its frames record in, so such a frame hangs
        // below the context's node and takes its own walk from the root.
        auto frame_walk = begin_walk(frame.spatial);
        if (!apply_spatial_steps_through_node(frame_walk, frame.spatial) || !point_passes_frame(frame, frame_walk.point))
            return {};
    }
    if (!apply_spatial_steps_through_node(context_walk, context.spatial))
        return {};

    return context_walk.point;
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
                [&](StickyData const&) {
                    rect.translate_by(scroll_state.device_offset_for_index(i));
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

Gfx::FloatPoint AccumulatedVisualContextTree::cumulative_scroll_chain_offset(SpatialNodeIndex index, ScrollStateSnapshot const& scroll_state) const
{
    auto nearest_scroll_like_ancestor = [&](SpatialNodeIndex from) -> Optional<SpatialNodeIndex> {
        for (auto i = from; i != VISUAL_VIEWPORT_NODE_INDEX;) {
            i = m_spatial_nodes[i.value()].parent;
            if (i == VISUAL_VIEWPORT_NODE_INDEX)
                return {};
            auto const& data = m_spatial_nodes[i.value()].data;
            if (data.has<ScrollData>() || data.has<StickyData>())
                return i;
        }
        return {};
    };

    Gfx::FloatPoint offset;
    for (Optional<SpatialNodeIndex> current = index; current.has_value() && *current != VISUAL_VIEWPORT_NODE_INDEX;) {
        offset.translate_by(scroll_state.device_offset_for_index(*current));
        if (auto const* sticky = m_spatial_nodes[current->value()].data.get_pointer<StickyData>())
            current = sticky->parent_sticky.has_value() ? sticky->parent_sticky : Optional<SpatialNodeIndex> { sticky->scroller };
        else
            current = nearest_scroll_like_ancestor(*current);
    }
    return offset;
}

void resolve_sticky_offsets(AccumulatedVisualContextTree const& tree, ScrollStateSnapshot& scroll_state)
{
    auto spatial_nodes = tree.spatial_nodes();
    for (size_t i = 0; i < spatial_nodes.size(); ++i) {
        auto const* sticky = spatial_nodes[i].data.get_pointer<StickyData>();
        if (!sticky)
            continue;
        auto node_index = SpatialNodeIndex { static_cast<u32>(i) };
        if (sticky->scroller == VISUAL_VIEWPORT_NODE_INDEX) {
            scroll_state.set_device_offset_for_index(node_index, {});
            continue;
        }
        VERIFY(sticky->scroller.value() < i);

        // Sticky ancestors along the containing block chain precede this node, so their entries are
        // already resolved in this pass.
        Gfx::FloatPoint parent_sticky_offset;
        for (auto ancestor = sticky->parent_sticky; ancestor.has_value(); ancestor = spatial_nodes[ancestor->value()].data.get<StickyData>().parent_sticky) {
            VERIFY(ancestor->value() < i);
            parent_sticky_offset.translate_by(scroll_state.device_offset_for_index(*ancestor));
        }

        auto position_in_scroller = sticky->position_relative_to_scroller.translated(parent_sticky_offset);
        auto containing_block_region = sticky->containing_block_region;
        if (sticky->needs_parent_offset_adjustment)
            containing_block_region.translate_by(parent_sticky_offset);
        auto min_offset_within_containing_block = containing_block_region.top_left();
        Gfx::FloatPoint max_offset_within_containing_block {
            containing_block_region.right() - sticky->border_box_size.width(),
            containing_block_region.bottom() - sticky->border_box_size.height()
        };

        // A scroll container's entry is its negated scroll offset.
        auto scroller_entry = scroll_state.device_offset_for_index(sticky->scroller);
        Gfx::FloatRect scrollport_rect { { -scroller_entry.x(), -scroller_entry.y() }, sticky->scrollport_size };

        Gfx::FloatPoint sticky_offset;
        if (sticky->inset_top.has_value() && scrollport_rect.top() > position_in_scroller.y() - *sticky->inset_top)
            sticky_offset.set_y(min(scrollport_rect.top() + *sticky->inset_top, max_offset_within_containing_block.y()) - position_in_scroller.y());
        if (sticky->inset_left.has_value() && scrollport_rect.left() > position_in_scroller.x() - *sticky->inset_left)
            sticky_offset.set_x(min(scrollport_rect.left() + *sticky->inset_left, max_offset_within_containing_block.x()) - position_in_scroller.x());
        if (sticky->inset_bottom.has_value() && scrollport_rect.bottom() < position_in_scroller.y() + sticky->border_box_size.height() + *sticky->inset_bottom)
            sticky_offset.set_y(max(scrollport_rect.bottom() - sticky->border_box_size.height() - *sticky->inset_bottom, min_offset_within_containing_block.y()) - position_in_scroller.y());
        if (sticky->inset_right.has_value() && scrollport_rect.right() < position_in_scroller.x() + sticky->border_box_size.width() + *sticky->inset_right)
            sticky_offset.set_x(max(scrollport_rect.right() - sticky->border_box_size.width() - *sticky->inset_right, min_offset_within_containing_block.x()) - position_in_scroller.x());

        scroll_state.set_device_offset_for_index(node_index, sticky_offset);
    }
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
        [&](ScrollData const&) {
            builder.append("scroll"sv);
        },
        [&](StickyData const& sticky) {
            builder.appendff("sticky scroller={}", sticky.scroller);
            if (sticky.parent_sticky.has_value())
                builder.appendff(" parent_sticky={}", *sticky.parent_sticky);
            builder.appendff(" position_relative_to_scroller={} border_box_size={} scrollport_size={} containing_block_region={} needs_parent_offset_adjustment={} insets=[",
                sticky.position_relative_to_scroller, sticky.border_box_size, sticky.scrollport_size, sticky.containing_block_region, sticky.needs_parent_offset_adjustment);
            bool is_first_inset = true;
            auto append_inset = [&](StringView side, Optional<float> inset) {
                if (!inset.has_value())
                    return;
                if (!is_first_inset)
                    builder.append(", "sv);
                builder.appendff("{}={}", side, *inset);
                is_first_inset = false;
            };
            append_inset("top"sv, sticky.inset_top);
            append_inset("right"sv, sticky.inset_right);
            append_inset("bottom"sv, sticky.inset_bottom);
            append_inset("left"sv, sticky.inset_left);
            builder.append(']');
        },
        [&](TransformData const& transform) {
            auto const& matrix = transform.matrix.elements();
            auto const& origin = transform.origin;
            builder.appendff("{}=[{},{},{},{},{},{}] origin=({},{})",
                transform.role == TransformDataRole::SvgViewportTransform ? "svg-viewport-transform"sv : "transform"sv,
                matrix[0][0], matrix[0][1], matrix[1][0], matrix[1][1], matrix[0][3], matrix[1][3], origin.x(), origin.y());
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
            if (clip.mode == ClipMode::Difference)
                builder.append(" mode=difference"sv);
        },
        [&](ClipPathData const& clip_path) {
            auto const& rect = clip_path.bounding_rect;
            auto svg_path = clip_path.path.to_svg_string();
            bool const has_curves_with_host_dependent_control_points = svg_path.contains('Q') || svg_path.contains('C');
            if (has_curves_with_host_dependent_control_points) {
                size_t command_count = 0;
                for (auto code_point : svg_path.code_points()) {
                    if (code_point == 'M' || code_point == 'L' || code_point == 'Q' || code_point == 'C' || code_point == 'Z')
                        ++command_count;
                }
                builder.appendff("clip_path=[bounds: {},{} {}x{}, curved path: {} commands]", rect.x(), rect.y(), rect.width(), rect.height(), command_count);
            } else {
                builder.appendff("clip_path=[bounds: {},{} {}x{}, path: {}]", rect.x(), rect.y(), rect.width(), rect.height(), svg_path);
            }
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
ErrorOr<void> encode(Encoder&, Web::Painting::ScrollData const&)
{
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollData> decode(Decoder&)
{
    return Web::Painting::ScrollData {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::StickyData const& data)
{
    TRY(encoder.encode(data.scroller));
    TRY(encoder.encode(data.parent_sticky));
    TRY(encoder.encode(data.position_relative_to_scroller));
    TRY(encoder.encode(data.border_box_size.width()));
    TRY(encoder.encode(data.border_box_size.height()));
    TRY(encoder.encode(data.scrollport_size.width()));
    TRY(encoder.encode(data.scrollport_size.height()));
    TRY(encoder.encode(data.containing_block_region));
    TRY(encoder.encode(data.needs_parent_offset_adjustment));
    TRY(encoder.encode(data.inset_top));
    TRY(encoder.encode(data.inset_right));
    TRY(encoder.encode(data.inset_bottom));
    TRY(encoder.encode(data.inset_left));
    return {};
}

template<>
ErrorOr<Web::Painting::StickyData> decode(Decoder& decoder)
{
    auto scroller = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>());
    auto parent_sticky = TRY(decoder.decode<Optional<Web::Painting::SpatialNodeIndex>>());
    auto position_relative_to_scroller = TRY(decoder.decode<Gfx::FloatPoint>());
    auto border_box_width = TRY(decoder.decode<float>());
    auto border_box_height = TRY(decoder.decode<float>());
    auto scrollport_width = TRY(decoder.decode<float>());
    auto scrollport_height = TRY(decoder.decode<float>());
    auto containing_block_region = TRY(decoder.decode<Gfx::FloatRect>());
    auto needs_parent_offset_adjustment = TRY(decoder.decode<bool>());
    auto inset_top = TRY(decoder.decode<Optional<float>>());
    auto inset_right = TRY(decoder.decode<Optional<float>>());
    auto inset_bottom = TRY(decoder.decode<Optional<float>>());
    auto inset_left = TRY(decoder.decode<Optional<float>>());
    return Web::Painting::StickyData {
        .scroller = scroller,
        .parent_sticky = parent_sticky,
        .position_relative_to_scroller = position_relative_to_scroller,
        .border_box_size = { border_box_width, border_box_height },
        .scrollport_size = { scrollport_width, scrollport_height },
        .containing_block_region = containing_block_region,
        .needs_parent_offset_adjustment = needs_parent_offset_adjustment,
        .inset_top = inset_top,
        .inset_right = inset_right,
        .inset_bottom = inset_bottom,
        .inset_left = inset_left,
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ClipData const& data)
{
    TRY(encoder.encode(data.rect));
    TRY(encoder.encode(data.corner_radii));
    TRY(encoder.encode(data.mode));
    return {};
}

template<>
ErrorOr<Web::Painting::ClipData> decode(Decoder& decoder)
{
    return Web::Painting::ClipData {
        .rect = TRY(decoder.decode<Gfx::FloatRect>()),
        .corner_radii = TRY(decoder.decode<Gfx::CornerRadii>()),
        .mode = TRY(decoder.decode<Web::Painting::ClipMode>()),
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
    return {};
}

template<>
ErrorOr<Web::Painting::FrameNode> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<Web::Painting::FrameData>());
    auto parent = TRY(decoder.decode<Web::Painting::FrameNodeIndex>());
    auto spatial = TRY(decoder.decode<Web::Painting::SpatialNodeIndex>());
    bool clips_everything = Web::Painting::frame_data_clips_everything(data);
    return Web::Painting::FrameNode {
        .data = move(data),
        .parent = parent,
        .spatial = spatial,
        .clips_everything = clips_everything,
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
    TRY(encoder.encode(tree.m_root_isolation_frame));
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
    auto root_isolation_frame = TRY(decoder.decode<Optional<FrameNodeIndex>>());
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
            [&](StickyData const& sticky) {
                // resolve_sticky_offsets() reads the referenced nodes by kind, so a hostile tree must not
                // get past this point with references of the wrong kind or order.
                bool scroller_is_valid = sticky.scroller.value() < i
                    && (sticky.scroller == VISUAL_VIEWPORT_NODE_INDEX || spatial_nodes[sticky.scroller.value()].data.has<ScrollData>());
                bool parent_sticky_is_valid = !sticky.parent_sticky.has_value()
                    || (sticky.parent_sticky->value() < i && spatial_nodes[sticky.parent_sticky->value()].data.has<StickyData>());
                return scroller_is_valid && parent_sticky_is_valid;
            },
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
    if (root_isolation_frame.has_value() && (root_isolation_frame->value() >= frame_nodes.size() || !frame_nodes[root_isolation_frame->value()].data.has<EffectsData>()))
        return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree root isolation frame is not an effects frame");
    AccumulatedVisualContextTree tree { version, move(spatial_nodes), move(frame_nodes), root_is_visual_viewport };
    if (root_isolation_frame.has_value())
        tree.set_root_isolation_frame(*root_isolation_frame);
    return tree;
}

}
