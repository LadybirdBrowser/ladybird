/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(Position);

Position::Position(JS::GCPtr<Node> node, unsigned offset)
    : m_node(node)
    , m_offset(offset)
{
}

void Position::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node);
}

ErrorOr<String> Position::to_string() const
{
    if (!node())
        return String::formatted("DOM::Position(nullptr, {})", offset());
    return String::formatted("DOM::Position({} ({})), {})", node()->node_name(), node().ptr(), offset());
}

}
