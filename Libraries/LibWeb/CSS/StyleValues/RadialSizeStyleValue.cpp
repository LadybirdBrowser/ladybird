/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RadialSizeStyleValue.h"
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> RadialSizeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    bool any_component_required_absolutization = false;
    Vector<Component> absolutized_components;

    for (auto const& component : components()) {
        if (component.has<RadialExtent>()) {
            absolutized_components.append(component);
        } else {
            auto const& absolutized_length_percentage = component.get<NonnullRefPtr<StyleValue const>>()->absolutized(computation_context);

            if (!absolutized_length_percentage->equals(component.get<NonnullRefPtr<StyleValue const>>()))
                any_component_required_absolutization = true;

            absolutized_components.append(absolutized_length_percentage);
        }
    }

    if (!any_component_required_absolutization)
        return *this;

    return RadialSizeStyleValue::create(move(absolutized_components));
}

}
