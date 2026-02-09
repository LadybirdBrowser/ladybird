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

bool ClipData::contains(CSSPixelPoint point) const
{
    return corner_radii.contains(point, rect);
}

Optional<CSSPixelPoint> AccumulatedVisualContext::transform_point_for_hit_test(CSSPixelPoint screen_point, ScrollStateSnapshot const& scroll_state) const
{
    Vector<AccumulatedVisualContext const*> chain;
    for (auto const* node = this; node; node = node->parent().ptr())
        chain.append(node);

    auto point = screen_point;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const* node = chain[i - 1];

        auto result = node->data().visit(
            [&](PerspectiveData const& perspective) -> Optional<CSSPixelPoint> {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};
                point = inverse->map(point.to_type<float>()).to_type<CSSPixels>();
                return point;
            },
            [&](ScrollData const& scroll) -> Optional<CSSPixelPoint> {
                auto offset = scroll_state.own_offset_for_frame_with_id(scroll.scroll_frame_id);
                point.translate_by(-offset);
                return point;
            },
            [&](TransformData const& transform) -> Optional<CSSPixelPoint> {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};

                auto offset_point = point - transform.origin;
                auto transformed = inverse->map(offset_point.to_type<float>()).to_type<CSSPixels>();
                point = transformed + transform.origin;
                return point;
            },
            [&](ClipData const& clip) -> Optional<CSSPixelPoint> {
                // NOTE: The clip rect is stored in absolute (layout) coordinates. After inverse-transforming, `point`
                //       is also in layout coordinates, so we compare them directly without mapping back to screen space.
                if (!clip.contains(point))
                    return {};
                return point;
            },
            [&](ClipPathData const& clip_path) -> Optional<CSSPixelPoint> {
                // NOTE: The clip path is stored in absolute (layout) coordinates. After inverse-transforming, `point`
                //       is also in layout coordinates, so we compare them directly without mapping back to screen space.
                if (!clip_path.bounding_rect.contains(point))
                    return {};
                if (!clip_path.path.contains(point.to_type<float>(), clip_path.fill_rule))
                    return {};
                return point;
            },
            [&](EffectsData const&) -> Optional<CSSPixelPoint> {
                // Effects don't affect coordinate transforms
                return point;
            });

        if (!result.has_value())
            return {};
    }

    return point;
}

CSSPixelPoint AccumulatedVisualContext::inverse_transform_point(CSSPixelPoint screen_point) const
{
    Vector<AccumulatedVisualContext const*> chain;
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
                    point = inverse->map(point.to_type<float>()).to_type<CSSPixels>();
            },
            [&](TransformData const& transform) {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto inverse = affine.inverse();
                if (inverse.has_value()) {
                    auto offset_point = point - transform.origin;
                    auto transformed = inverse->map(offset_point.to_type<float>()).to_type<CSSPixels>();
                    point = transformed + transform.origin;
                }
            },
            [&](auto const&) {});
    }

    return point;
}

CSSPixelRect AccumulatedVisualContext::transform_rect_to_viewport(CSSPixelRect const& source_rect, ScrollStateSnapshot const& scroll_state) const
{
    Vector<AccumulatedVisualContext const*> chain;
    for (auto const* node = this; node; node = node->parent().ptr())
        chain.append(node);

    auto rect = source_rect.to_type<float>();
    for (auto const* node : chain) {
        node->data().visit(
            [&](TransformData const& transform) {
                auto affine = Gfx::extract_2d_affine_transform(transform.matrix);
                auto origin = transform.origin.to_type<float>();
                rect.translate_by(-origin);
                rect = affine.map(rect);
                rect.translate_by(origin);
            },
            [&](PerspectiveData const& perspective) {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                rect = affine.map(rect);
            },
            [&](ScrollData const& scroll) {
                auto offset = scroll_state.own_offset_for_frame_with_id(scroll.scroll_frame_id);
                rect.translate_by(offset.to_type<float>());
            },
            [&](ClipData const&) { /* clips don't affect rect coordinates */ },
            [&](ClipPathData const&) { /* clip paths don't affect rect coordinates */ },
            [&](EffectsData const&) { /* effects don't affect rect coordinates */ });
    }

    return rect.to_type<CSSPixels>();
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
            builder.appendff("transform=[{},{},{},{},{},{}] origin=({},{})", matrix[0][0], matrix[0][1], matrix[1][0], matrix[1][1], matrix[0][3], matrix[1][3], origin.x().to_float(), origin.y().to_float());
        },
        [&](ClipData const& clip) {
            auto const& rect = clip.rect;
            builder.appendff("clip=[{},{} {}x{}]", rect.x().to_float(), rect.y().to_float(), rect.width().to_float(), rect.height().to_float());

            if (clip.corner_radii.has_any_radius()) {
                auto const& corner_radii = clip.corner_radii;
                builder.appendff(" radii=({},{},{},{})", corner_radii.top_left.horizontal_radius, corner_radii.top_right.horizontal_radius, corner_radii.bottom_right.horizontal_radius, corner_radii.bottom_left.horizontal_radius);
            }
        },
        [&](ClipPathData const& clip_path) {
            auto const& rect = clip_path.bounding_rect;
            builder.appendff("clip_path=[bounds: {},{} {}x{}, path: {}]", rect.x().to_float(), rect.y().to_float(), rect.width().to_float(), rect.height().to_float(), clip_path.path.to_svg_string());
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
            if (effects.filter.has_filters()) {
                if (has_content)
                    builder.append(' ');
                effects.filter.dump(builder);
                has_content = true;
            }
            builder.append("]"sv);
        });
}

}
