/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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

Optional<CSSPixelPoint> AccumulatedVisualContext::transform_point_for_hit_test(CSSPixelPoint screen_point, ScrollStateSnapshot const& scroll_state) const
{
    Vector<AccumulatedVisualContext const*> chain;
    for (auto const* node = this; node; node = node->parent().ptr())
        chain.append(node);

    auto point = screen_point;
    Gfx::AffineTransform current_to_document;
    for (size_t i = chain.size(); i > 0; --i) {
        auto const* node = chain[i - 1];

        auto result = node->data().visit(
            [&](PerspectiveData const& perspective) -> Optional<CSSPixelPoint> {
                auto affine = Gfx::extract_2d_affine_transform(perspective.matrix);
                auto inverse = affine.inverse();
                if (!inverse.has_value())
                    return {};
                point = inverse->map(point.to_type<float>()).to_type<CSSPixels>();
                current_to_document = affine.multiply(current_to_document);
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

                auto origin_f = transform.origin.to_type<float>();
                auto transform_around_origin = Gfx::AffineTransform {}.translate(origin_f).multiply(affine).translate(-origin_f);
                current_to_document = transform_around_origin.multiply(current_to_document);
                return point;
            },
            [&](ClipData const& clip) -> Optional<CSSPixelPoint> {
                auto point_in_document = current_to_document.map(point.to_type<float>()).to_type<CSSPixels>();
                if (!clip.rect.contains(point_in_document))
                    return {};
                return point;
            },
            [&](ClipPathData const& clip_path) -> Optional<CSSPixelPoint> {
                auto point_in_document = current_to_document.map(point.to_type<float>()).to_type<CSSPixels>();
                if (!clip_path.bounding_rect.contains(point_in_document))
                    return {};
                if (!clip_path.path.contains(point_in_document.to_type<float>(), clip_path.fill_rule))
                    return {};
                return point;
            });

        if (!result.has_value())
            return {};
    }

    return point;
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
        });
}

}
