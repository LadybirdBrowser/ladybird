/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

enum class ShadowPlacement {
    Outer,
    Inner,
};

class ShadowStyleValue final : public StyleValueWithDefaultOperators<ShadowStyleValue> {
public:
    enum class ShadowType : u8 {
        // none | <shadow>#
        Normal,
        // none | [ <color>? && <length>{2,3} ]#
        Text
    };

    virtual ~ShadowStyleValue() override = default;

    ValueComparingRefPtr<StyleValue const> color_or_null() const { return wrap_rust_child_or_null(m_value->shadow._0->color); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return wrap_rust_child(m_value->shadow._0->offset_x); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return wrap_rust_child(m_value->shadow._0->offset_y); }
    ValueComparingRefPtr<StyleValue const> blur_radius_or_null() const { return wrap_rust_child_or_null(m_value->shadow._0->blur_radius); }
    ValueComparingRefPtr<StyleValue const> spread_distance_or_null() const { return wrap_rust_child_or_null(m_value->shadow._0->spread_distance); }

private:
    explicit ShadowStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Shadow, data)
    {
    }

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
};

}
