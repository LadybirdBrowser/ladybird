/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/ClipboardSerializer.h>
#include <LibWeb/Editing/InterchangeWhitespace.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLFontElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/VisualLines.h>

#include <LibWeb/Editing/StyledMarkupSerializer.h>

namespace Web::Editing {

// INTEROP: The Clipboard API does not define how a DOM range becomes text/plain. Blink and WebKit use a rendered-text
//          iterator which emits paragraph boundaries, replaced-element text, and explicit line breaks without exposing
//          the DOM's formatting whitespace. Keep that policy separate from DOM serialization so both clipboard
//          representations can evolve without coupling their traversal rules.
class ClipboardTextSerializer {
public:
    explicit ClipboardTextSerializer(DOM::Range const& range)
        : m_range(GC::Root<DOM::Range const>::create(&range))
    {
    }

    Utf16String serialize()
    {
        collect(m_range->common_ancestor_container());
        if (m_pending_break_is_explicit)
            flush_pending_breaks();
        return m_builder.to_string();
    }

private:
    void request_line_breaks(size_t count)
    {
        m_pending_line_breaks = max(m_pending_line_breaks, count);
    }

    void request_explicit_line_break()
    {
        flush_pending_breaks();
        m_pending_line_breaks = 1;
        m_pending_break_is_explicit = true;
    }

    void flush_pending_breaks()
    {
        if (!m_builder.is_empty())
            m_builder.append_repeated_ascii('\n', m_pending_line_breaks);
        else if (m_pending_break_is_explicit)
            m_builder.append_ascii('\n');
        m_pending_line_breaks = 0;
        m_pending_break_is_explicit = false;
    }

    void append(Utf16View text)
    {
        if (text.is_empty())
            return;
        flush_pending_breaks();
        m_builder.append(text);
    }

    bool range_fully_contains_node(DOM::Node const& node) const
    {
        auto parent = node.parent();
        if (!parent)
            return false;
        auto index = static_cast<WebIDL::UnsignedLong>(node.index());
        auto before_node = DOM::BoundaryPoint { const_cast<DOM::Node&>(*parent), index };
        auto after_node = DOM::BoundaryPoint { const_cast<DOM::Node&>(*parent), index + 1 };
        return DOM::position_of_boundary_point_relative_to_other_boundary_point(before_node, m_range->start()) != DOM::RelativeBoundaryPointPosition::Before
            && DOM::position_of_boundary_point_relative_to_other_boundary_point(after_node, m_range->end()) != DOM::RelativeBoundaryPointPosition::After;
    }

    void collect(DOM::Node const& node)
    {
        if (!m_range->intersects_node(const_cast<DOM::Node&>(node)))
            return;

        auto const* layout_node = node.layout_node();
        bool is_rendered_block = false;
        if (layout_node && is<Layout::NodeWithStyle>(*layout_node)) {
            auto display = as<Layout::NodeWithStyle>(*layout_node).display();
            is_rendered_block = display.is_block_outside() || display.is_table_caption();
        }

        if (is_rendered_block)
            request_line_breaks(is<HTML::HTMLParagraphElement>(node) ? 2 : 1);

        if (auto const* text = as_if<DOM::Text>(node)) {
            if (!layout_node || layout_node->user_select_used_value() == CSS::UserSelect::None)
                return;
            // Source formatting whitespace between blocks has a layout node but produces no painted text. TextIterator
            // based engines omit it from the plain-text clipboard representation.
            if (collect_visual_lines(*text).is_empty())
                return;

            Utf16String data;
            if (&node == m_range->start_container().ptr() && &node == m_range->end_container().ptr())
                data = MUST(text->substring_data(m_range->start_offset(), m_range->end_offset() - m_range->start_offset()));
            else if (&node == m_range->start_container().ptr())
                data = MUST(text->substring_data(m_range->start_offset(), text->length_in_utf16_code_units() - m_range->start_offset()));
            else if (&node == m_range->end_container().ptr())
                data = MUST(text->substring_data(0, m_range->end_offset()));
            else
                data = text->data();
            append(data);
        } else if (is<HTML::HTMLBRElement>(node)) {
            if (range_fully_contains_node(node))
                request_explicit_line_break();
        } else if (auto const* image = as_if<HTML::HTMLImageElement>(node)) {
            if (range_fully_contains_node(node) && layout_node && layout_node->user_select_used_value() != CSS::UserSelect::None)
                append(image->alt());
        } else {
            node.for_each_child([&](DOM::Node const& child) {
                collect(child);
                return IterationDecision::Continue;
            });
        }

        if (layout_node && is<Layout::NodeWithStyle>(*layout_node)) {
            auto display = as<Layout::NodeWithStyle>(*layout_node).display();
            if (display.is_table_cell() && node.next_sibling())
                append("\t"_utf16);
            if (display.is_table_row() && node.next_sibling())
                request_explicit_line_break();
        }

        if (is_rendered_block)
            request_line_breaks(is<HTML::HTMLParagraphElement>(node) ? 2 : 1);
    }

    GC::Root<DOM::Range const> m_range;
    Utf16StringBuilder m_builder;
    size_t m_pending_line_breaks { 0 };
    bool m_pending_break_is_explicit { false };
};

Utf16String serialize_range_as_plain_text_for_clipboard(DOM::Range const& range)
{
    return ClipboardTextSerializer { range }.serialize();
}

Utf16String serialize_range_as_html_for_clipboard(DOM::Range& range)
{
    return serialize_styled_markup_for_clipboard(range);
}

}
