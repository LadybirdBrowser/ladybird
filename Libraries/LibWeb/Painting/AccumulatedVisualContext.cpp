/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibGfx/Matrix4x4.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>

namespace Web::Painting {

NonnullRefPtr<AccumulatedVisualContext> AccumulatedVisualContext::create(size_t id, VisualContextData data, RefPtr<AccumulatedVisualContext const> parent)
{
    return adopt_ref(*new AccumulatedVisualContext(id, move(data), move(parent)));
}

bool ClipData::contains(DevicePixelPoint point) const
{
    return corner_radii.contains(point.to_type<int>(), rect.to_type<int>());
}

Optional<Gfx::FloatPoint> AccumulatedVisualContext::transform_point_for_hit_test(Gfx::FloatPoint screen_point, ReadonlySpan<Gfx::FloatPoint> scroll_offsets) const
{
    Vector<AccumulatedVisualContext const*, 8> chain;
    chain.ensure_capacity(m_depth);
    for (auto const* node = this; node; node = node->parent().ptr())
        chain.append(node);

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const* node = chain[i - 1];

        auto result = node->data().visit(
            [&](PerspectiveData const& perspective) -> Optional<Gfx::FloatPoint> {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};
                point = inverse->map(point);
                return point;
            },
            [&](ScrollData const& scroll) -> Optional<Gfx::FloatPoint> {
                if (scroll.scroll_frame_id < scroll_offsets.size())
                    point.translate_by(-scroll_offsets[scroll.scroll_frame_id]);
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

Gfx::FloatPoint AccumulatedVisualContext::inverse_transform_point(Gfx::FloatPoint screen_point) const
{
    Vector<AccumulatedVisualContext const*, 8> chain;
    chain.ensure_capacity(m_depth);
    for (auto const* node = this; node; node = node->parent().ptr())
        chain.append(node);

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const* node = chain[i - 1];

        node->data().visit(
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

Gfx::FloatRect AccumulatedVisualContext::transform_rect_to_viewport(Gfx::FloatRect const& source_rect, ReadonlySpan<Gfx::FloatPoint> scroll_offsets) const
{
    auto rect = source_rect;
    for (auto const* node = this; node; node = node->parent().ptr()) {
        node->data().visit(
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
                if (scroll.scroll_frame_id < scroll_offsets.size())
                    rect.translate_by(scroll_offsets[scroll.scroll_frame_id]);
            },
            [&](ClipData const&) { /* clips don't affect rect coordinates */ },
            [&](ClipPathData const&) { /* clip paths don't affect rect coordinates */ },
            [&](EffectsData const&) { /* effects don't affect rect coordinates */ });
    }

    return rect;
}

void AccumulatedVisualContext::dump(StringBuilder& builder) const
{
    m_data.visit(
        [&](PerspectiveData const&) {
            builder.append("perspective"sv);
        },
        [&](ScrollData const& scroll) {
            builder.appendff("scroll_frame_id={}", scroll.scroll_frame_id);
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
