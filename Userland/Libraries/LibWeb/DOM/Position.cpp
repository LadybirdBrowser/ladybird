/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibUnicode/CharacterTypes.h>
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

    auto const& node = verify_cast<DOM::Text>(*m_node);
    if (auto const length = node.length_in_utf16_code_units(); m_offset < length)
        ++m_offset;

    return true;
}

bool Position::decrement_offset()
{
    if (!is<DOM::Text>(*m_node))
        return false;

    auto const& node = verify_cast<DOM::Text>(*m_node);
    if (auto const length = node.length_in_utf16_code_units(); length > 0 && m_offset > 0)
        --m_offset;

    return true;
}

static bool should_continue_beyond_word(Utf8View const& word)
{
    for (auto code_point : word) {
        if (!Unicode::code_point_has_punctuation_general_category(code_point) && !Unicode::code_point_has_separator_general_category(code_point))
            return false;
    }

    return true;
}

bool Position::increment_offset_to_next_word()
{
    if (!is<DOM::Text>(*m_node) || offset_is_at_end_of_node())
        return false;

    auto const& node = static_cast<DOM::Text&>(*m_node);

    while (true) {
        if (auto offset = node.word_segmenter().next_boundary(m_offset); offset.has_value()) {
            auto word = MUST(node.data().substring_from_code_unit_offset(m_offset, *offset - m_offset));
            m_offset = *offset;

            if (should_continue_beyond_word(word.code_points()))
                continue;
        }

        break;
    }

    return true;
}

bool Position::decrement_offset_to_previous_word()
{
    if (!is<DOM::Text>(*m_node) || m_offset == 0)
        return false;

    auto const& node = static_cast<DOM::Text&>(*m_node);

    while (true) {
        if (auto offset = node.word_segmenter().previous_boundary(m_offset); offset.has_value()) {
            auto word = MUST(node.data().substring_from_code_unit_offset(*offset, m_offset - *offset));
            m_offset = *offset;

            if (should_continue_beyond_word(word.code_points()))
                continue;
        }

        break;
    }

    return true;
}

bool Position::offset_is_at_end_of_node() const
{
    if (!is<DOM::Text>(*m_node))
        return false;

    auto const& node = verify_cast<DOM::Text>(*m_node);
    return m_offset == node.length_in_utf16_code_units();
}

}
