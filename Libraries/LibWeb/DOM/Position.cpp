/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(Position);

Position::Position(GC::Ref<Node> node, unsigned offset, TextAffinity affinity)
    : m_node(node)
    , m_offset(offset)
    , m_affinity(affinity)
{
}

void Position::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node);
}

ErrorOr<Utf16String> Position::to_string() const
{
    return Utf16String::formatted("DOM::Position({} ({})), {})", node()->node_name(), node().ptr(), offset());
}

}
