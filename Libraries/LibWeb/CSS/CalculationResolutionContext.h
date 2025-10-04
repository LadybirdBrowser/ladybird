/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

struct CalculationResolutionContext {
    using PercentageBasis = Variant<Empty, Angle, Frequency, Length, Time>;

    PercentageBasis percentage_basis {};
    Optional<Length::ResolutionContext> length_resolution_context;

    static CalculationResolutionContext from_computation_context(ComputationContext const& computation_context, PercentageBasis percentage_basis = {})
    {
        return {
            .percentage_basis = percentage_basis,
            .length_resolution_context = computation_context.length_resolution_context,
        };
    }
};

}
