/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/ResolvedTransform.h>

namespace Web::CSS {

Gfx::FloatMatrix4x4 ResolvedTransform::to_matrix(CSSPixels reference_width, CSSPixels reference_height) const
{
    return m_value.visit(
        [](Gfx::FloatMatrix4x4 const& matrix) { return matrix; },
        [&](Translate const& translate) {
            auto resolve_axis = [](TranslateAxis const& axis, CSSPixels reference_length) {
                if (axis.percentage_value)
                    return Length::from_style_value(*axis.percentage_value, Length::make_px(reference_length)).absolute_length_to_px().to_float();
                return axis.px;
            };
            return Gfx::translation_matrix(Vector3 {
                resolve_axis(translate.x, reference_width),
                resolve_axis(translate.y, reference_height),
                translate.z });
        });
}

bool ResolvedTransform::operator==(ResolvedTransform const& other) const
{
    if (auto const* matrix = m_value.get_pointer<Gfx::FloatMatrix4x4>()) {
        auto const* other_matrix = other.m_value.get_pointer<Gfx::FloatMatrix4x4>();
        return other_matrix && __builtin_memcmp(matrix->elements(), other_matrix->elements(), sizeof(float) * 16) == 0;
    }
    auto const* other_translate = other.m_value.get_pointer<Translate>();
    return other_translate && m_value.get<Translate>() == *other_translate;
}

}
