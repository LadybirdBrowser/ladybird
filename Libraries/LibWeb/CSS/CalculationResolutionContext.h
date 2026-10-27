/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct CalculationResolutionContext {
    Optional<double> percentage_basis_in_px {};
    Optional<LengthResolutionContext> length_resolution_context {};
    Optional<DOM::AbstractElement> abstract_element {};

    static CalculationResolutionContext from_computation_context(ComputationContext const& computation_context)
    {
        return {
            .length_resolution_context = computation_context.length_resolution_context,
            .abstract_element = computation_context.abstract_element
        };
    }
};

}
