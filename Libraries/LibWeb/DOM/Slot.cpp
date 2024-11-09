/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Slot.h>
#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

Slot::~Slot() = default;

void Slot::visit_edges(JS::Cell::Visitor& visitor)
{
    for (auto const& node : m_assigned_nodes)
        node.visit([&](auto const& slottable) { visitor.visit(slottable); });
}

}
