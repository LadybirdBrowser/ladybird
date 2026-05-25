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

    void reset_viewport_metric_dependency_tracking() const
    {
        m_did_resolve_viewport_relative_length = false;
        length_resolution_context.set_did_resolve_viewport_relative_length(m_did_resolve_viewport_relative_length);
    }

    bool depends_on_viewport_metrics() const
    {
        return m_did_resolve_viewport_relative_length;
    }

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        if (abstract_element.has_value())
            abstract_element->visit(visitor);
    }

    mutable bool m_did_resolve_viewport_relative_length { false };
};

}
