/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>

namespace Web::HTML {

void CustomElementDefinition::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_constructor);
    visitor.visit(m_lifecycle_callbacks);
    for (auto& entry : m_construction_stack) {
        entry.visit(
            [&](GC::Ref<DOM::Element>& element) { visitor.visit(element); },
            [&](AlreadyConstructedCustomElementMarker&) {});
    }
}

}
