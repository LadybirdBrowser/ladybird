/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibGfx/Matrix4x4.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

bool ClipData::contains(DevicePixelPoint point) const
{
    return corner_radii.contains(point.to_type<int>(), rect.to_type<int>());
}

NonnullRefPtr<AccumulatedVisualContextTree> AccumulatedVisualContextTree::create()
{
    auto visual_context_tree = adopt_ref(*new AccumulatedVisualContextTree());
    // Sentinel at index 0 (null context). Data type doesn't matter; it's never accessed.
    visual_context_tree->m_nodes.append({ ScrollData { {}, false }, {}, 0, false });
    return visual_context_tree;
}

VisualContextIndex AccumulatedVisualContextTree::append(VisualContextData data, VisualContextIndex parent_index)
{
    size_t depth = parent_index.value() ? m_nodes[parent_index.value()].depth + 1 : 1;

    bool empty_clip = false;
    if (parent_index.value() && m_nodes[parent_index.value()].has_empty_effective_clip) {
        empty_clip = true;
    } else if (data.has<ClipData>()) {
        empty_clip = data.get<ClipData>().rect.is_empty();
    } else if (data.has<ClipPathData>()) {
        empty_clip = data.get<ClipPathData>().path.bounding_box().is_empty();
    }

    auto index = VisualContextIndex(m_nodes.size());
    m_nodes.append({ move(data), parent_index, depth, empty_clip });
    return index;
}

VisualContextIndex AccumulatedVisualContextTree::find_common_ancestor(VisualContextIndex a, VisualContextIndex b) const
{
    if (!a.value() || !b.value())
        return {};
    size_t a_index = a.value();
    size_t b_index = b.value();
    while (m_nodes[a_index].depth > m_nodes[b_index].depth)
        a_index = m_nodes[a_index].parent_index.value();
    while (m_nodes[b_index].depth > m_nodes[a_index].depth)
        b_index = m_nodes[b_index].parent_index.value();
    while (a_index != b_index) {
        a_index = m_nodes[a_index].parent_index.value();
        b_index = m_nodes[b_index].parent_index.value();
    }
    return a_index;
}

Vector<size_t, 8> AccumulatedVisualContextTree::build_ancestor_chain(VisualContextIndex index) const
{
    auto const& node = m_nodes[index.value()];
    Vector<size_t, 8> chain;
    chain.ensure_capacity(node.depth);
    for (size_t i = index.value(); i; i = m_nodes[i].parent_index.value())
        chain.append(i);
    return chain;
}

Optional<Gfx::FloatPoint> AccumulatedVisualContextTree::transform_point_for_hit_test(VisualContextIndex index, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state) const
{
    if (!index.value())
        return screen_point;

    auto chain = build_ancestor_chain(index);

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const& node = m_nodes[chain[i - 1]];

        auto result = node.data.visit(
            [&](PerspectiveData const& perspective) -> Optional<Gfx::FloatPoint> {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};
                point = inverse->map(point);
                return point;
            },
            [&](ScrollData const& scroll) -> Optional<Gfx::FloatPoint> {
                point.translate_by(-scroll_state.device_offset_for_index(scroll.scroll_frame_index));
                return point;
            },
            [&](TransformData const& transform) -> Optional<Gfx::FloatPoint> {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};

                auto offset_point = point - transform.origin;
                auto transformed = inverse->map(offset_point);
                point = transformed + transform.origin;
                return point;
            },
            [&](ClipData const& clip) -> Optional<Gfx::FloatPoint> {
                // NOTE: The clip rect is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                if (!clip.contains(point.to_type<int>().to_type<DevicePixels>()))
                    return {};
                return point;
            },
            [&](ClipPathData const& clip_path) -> Optional<Gfx::FloatPoint> {
                // NOTE: The clip path is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                if (!clip_path.bounding_rect.contains(point.to_type<int>().to_type<DevicePixels>()))
                    return {};
                if (!clip_path.path.contains(point, clip_path.fill_rule))
                    return {};
                return point;
            },
            [&](EffectsData const&) -> Optional<Gfx::FloatPoint> {
                // Effects don't affect coordinate transforms
                return point;
            });

        if (!result.has_value())
            return {};
    }

    return point;
}

Gfx::FloatPoint AccumulatedVisualContextTree::inverse_transform_point(VisualContextIndex index, Gfx::FloatPoint screen_point) const
{
    if (!index.value())
        return screen_point;

    auto chain = build_ancestor_chain(index);

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const& node = m_nodes[chain[i - 1]];

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

Gfx::FloatRect AccumulatedVisualContextTree::transform_rect_to_viewport(VisualContextIndex index, Gfx::FloatRect const& source_rect, ScrollStateSnapshot const& scroll_state) const
{
    if (!index.value())
        return source_rect;

    auto rect = source_rect;
    for (size_t i = index.value(); i; i = m_nodes[i].parent_index.value()) {
        auto const& node = m_nodes[i];
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
            [&](ScrollData const& scroll) {
                rect.translate_by(scroll_state.device_offset_for_index(scroll.scroll_frame_index));
            },
            [&](ClipData const&) { /* clips don't affect rect coordinates */ },
            [&](ClipPathData const&) { /* clip paths don't affect rect coordinates */ },
            [&](EffectsData const&) { /* effects don't affect rect coordinates */ });
    }

    return rect;
}

void AccumulatedVisualContextTree::dump(VisualContextIndex index, StringBuilder& builder) const
{
    if (!index.value())
        return;

    auto const& node = m_nodes[index.value()];
    node.data.visit(
        [&](PerspectiveData const&) {
            builder.append("perspective"sv);
        },
        [&](ScrollData const& scroll) {
            builder.appendff("scroll_frame_id={}", scroll.scroll_frame_index);
            if (scroll.is_sticky)
                builder.append(" (sticky)"sv);
        },
        [&](TransformData const& transform) {
            auto const& matrix = transform.matrix.elements();
            auto const& origin = transform.origin;
            builder.appendff("transform=[{},{},{},{},{},{}] origin=({},{})", matrix[0][0], matrix[0][1], matrix[1][0], matrix[1][1], matrix[0][3], matrix[1][3], origin.x(), origin.y());
        },
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
        });
}

}
