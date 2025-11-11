/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

StyleProperty::~StyleProperty() = default;

bool StyleProperty::operator==(StyleProperty const& other) const
{
    if (important != other.important || property_id != other.property_id)
        return false;
    return value->equals(*other.value);
}

}
