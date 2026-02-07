/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/DOM/AbstractElement.h>

namespace Web::CSS {

struct ComputationContext {
    Length::ResolutionContext length_resolution_context;
    Optional<DOM::AbstractElement> abstract_element {};
    Optional<PreferredColorScheme> color_scheme {};

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        if (abstract_element.has_value())
            abstract_element->visit(visitor);
    }
};

}
