/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/Time.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

class AnchorResolver {
public:
    virtual ~AnchorResolver() = default;
    virtual Optional<CSSPixels> resolve(AnchorStyleValue const&) const = 0;
};

struct CalculationResolutionContext {
    using PercentageBasis = Variant<Empty, Angle, Frequency, Length, Time>;

    PercentageBasis percentage_basis {};
    Optional<Length::ResolutionContext> length_resolution_context {};
    Optional<DOM::AbstractElement> abstract_element {};

    AnchorResolver const* anchor_resolver { nullptr };

    static CalculationResolutionContext from_computation_context(ComputationContext const& computation_context, PercentageBasis percentage_basis = {})
    {
        return {
            .percentage_basis = percentage_basis,
            .length_resolution_context = computation_context.length_resolution_context,
            .abstract_element = computation_context.abstract_element
        };
    }
};

}
