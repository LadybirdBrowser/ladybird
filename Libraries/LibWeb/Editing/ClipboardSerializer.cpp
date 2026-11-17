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

    Utf16String serialize(Layout::BegunRead const& read)
    {
        collect(read, m_range->common_ancestor_container(), false);
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

    static DOM::Node const* child_on_path_to(DOM::Node const& ancestor, DOM::Node const& descendant)
    {
        for (auto const* node = &descendant; node; node = node->parent()) {
            if (node->parent() == &ancestor)
                return node;
        }
        return nullptr;
    }

    // NB: Only the nodes the range intersects are visited. Those that are not inclusive ancestors of a boundary node
    //     lie entirely inside the range, and so do all of their descendants. Testing each child for intersection
    //     instead would walk its preceding siblings every time, which is quadratic in a wide tree.
    template<typename Callback>
    void for_each_child_intersecting_range(DOM::Node const& node, bool node_is_fully_contained, Callback callback)
    {
        if (node_is_fully_contained) {
            node.for_each_child([&](DOM::Node const& child) {
                callback(child, true);
                return IterationDecision::Continue;
            });
            return;
        }

        auto const& start_node = *m_range->start_container();
        auto const& end_node = *m_range->end_container();
        auto const* child_on_path_to_start = child_on_path_to(node, start_node);
        auto const* child_on_path_to_end = child_on_path_to(node, end_node);

        DOM::Node const* first_child = node.first_child();
        if (&node == &start_node)
            first_child = node.child_at_index(m_range->start_offset());
        else if (child_on_path_to_start)
            first_child = child_on_path_to_start;

        DOM::Node const* child_to_stop_at = nullptr;
        if (&node == &end_node)
            child_to_stop_at = node.child_at_index(m_range->end_offset());
        else if (child_on_path_to_end)
            child_to_stop_at = child_on_path_to_end->next_sibling();

        for (auto const* child = first_child; child && child != child_to_stop_at; child = child->next_sibling())
            callback(*child, child != child_on_path_to_start && child != child_on_path_to_end);
    }

    void collect(Layout::BegunRead const& read, DOM::Node const& node, bool node_is_fully_contained)
    {
        auto const* layout_node = node.layout_node(read);
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
            if (!Layout::RustFFI::render_state_text_has_lines(layout_node->document_host(), Layout::Node::slot_id(layout_node)))
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
            if (node_is_fully_contained)
                request_explicit_line_break();
        } else if (auto const* image = as_if<HTML::HTMLImageElement>(node)) {
            if (node_is_fully_contained && layout_node && layout_node->user_select_used_value() != CSS::UserSelect::None)
                append(image->alt());
        } else {
            for_each_child_intersecting_range(node, node_is_fully_contained, [&](DOM::Node const& child, bool child_is_fully_contained) {
                collect(read, child, child_is_fully_contained);
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
    Layout::ForcedReadScope read { range.start_container()->document() };
    return ClipboardTextSerializer { range }.serialize(read);
}

Utf16String serialize_range_as_html_for_clipboard(DOM::Range& range)
{
    return serialize_styled_markup_for_clipboard(range);
}

}
