/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibUnicode/Segmenter.h>
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

bool Position::increment_offset()
{
    if (!is<DOM::Text>(*m_node))
        return false;

    auto& node = verify_cast<DOM::Text>(*m_node);

    if (auto offset = node.segmenter().next_boundary(m_offset); offset.has_value()) {
        m_offset = *offset;
        return true;
    }

    // NOTE: Already at end of current node.
    return false;
}

bool Position::decrement_offset()
{
    if (!is<DOM::Text>(*m_node))
        return false;

    auto& node = verify_cast<DOM::Text>(*m_node);

    if (auto offset = node.segmenter().previous_boundary(m_offset); offset.has_value()) {
        m_offset = *offset;
        return true;
    }

    // NOTE: Already at beginning of current node.
    return false;
}

bool Position::offset_is_at_end_of_node() const
{
    if (!is<DOM::Text>(*m_node))
        return false;

    auto& node = verify_cast<DOM::Text>(*m_node);
    auto text = node.data();
    return m_offset == text.bytes_as_string_view().length();
}

}
