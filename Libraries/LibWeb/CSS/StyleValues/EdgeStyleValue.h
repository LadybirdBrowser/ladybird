/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class EdgeStyleValue final : public StyleValueWithDefaultOperators<EdgeStyleValue> {
public:
    virtual ~EdgeStyleValue() override = default;

    // This is nonnull as it is only called after resolving keywords
    NonnullRefPtr<StyleValue const> offset() const { return wrap_rust_child(m_value->edge.offset); }

private:
    friend class StyleValue;

    explicit EdgeStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Edge, data)
    {
    }
};

}
