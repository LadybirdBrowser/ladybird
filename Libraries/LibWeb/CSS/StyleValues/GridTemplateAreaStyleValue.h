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

class GridTemplateAreaStyleValue final : public StyleValueWithDefaultOperators<GridTemplateAreaStyleValue> {
public:
    virtual ~GridTemplateAreaStyleValue() override = default;

private:
    friend class StyleValue;

    explicit GridTemplateAreaStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::GridTemplateArea, data)
    {
    }
};

}
