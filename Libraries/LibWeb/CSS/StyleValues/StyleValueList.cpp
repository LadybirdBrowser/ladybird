/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StyleValueList.h"
#include <LibGC/RootVector.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>

namespace Web::CSS {

void StyleValueList::set_style_sheet(StyleSheetState* style_sheet)
{
    for (auto& value : values())
        const_cast<StyleValue&>(*value).set_style_sheet(style_sheet);
}

// https://drafts.css-houdini.org/css-typed-om-1/#subdivide-into-iterations
StyleValueVector StyleValueList::subdivide_into_iterations(PropertyNameAndID const& property) const
{
    // To subdivide into iterations a CSS value whole value for a property property, execute the following steps:
    // 1. If property is a single-valued property, return a list containing whole value.
    if (property.is_custom_property() || !property_is_list_valued(property.id()))
        return StyleValueVector { *this };

    // 2. Otherwise, divide whole value into individual iterations, as appropriate for property, and return a list
    //    containing the iterations in order.
    return StyleValueVector { values() };
}

}
