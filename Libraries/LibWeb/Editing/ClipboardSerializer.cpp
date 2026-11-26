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
#include <LibWeb/DOM/UserSelect.h>
#include <LibWeb/Editing/ClipboardSerializer.h>
#include <LibWeb/Editing/InterchangeWhitespace.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLFontElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/SVGTextElement.h>

#include <LibWeb/Editing/StyledMarkupSerializer.h>

namespace Web::Editing {

enum class TextDestination {
    Clipboard,
    Selection,
};

// INTEROP: The Clipboard API does not define how a DOM range becomes text/plain. Blink and WebKit use a rendered-text
//          iterator which emits paragraph boundaries, replaced-element text, and explicit line breaks without exposing
//          the DOM's formatting whitespace. Keep that policy separate from DOM serialization so both clipboard
//          representations can evolve without coupling their traversal rules.
class RangeTextSerializer {
public:
    RangeTextSerializer(DOM::Range const& range, TextDestination destination)
        : m_range(GC::Root<DOM::Range const>::create(&range))
        , m_destination(destination)
    {
    }

    Utf16String serialize(Layout::BegunRead const& read)
    {
        SelectedTableParts table;
        collect(read, m_range->common_ancestor_container(), false, table);
        if (m_pending_break_is_explicit)
            flush_pending_breaks();
        return m_builder.to_string();
    }

private:
    struct SelectedTableParts {
        bool visited_row { false };
        bool visited_cell { false };
        size_t trailing_hidden_cells { 0 };
    };

    void request_line_breaks(size_t count)
    {
        m_pending_space = false;
        m_pending_line_breaks = max(m_pending_line_breaks, count);
    }

    void request_explicit_line_break()
    {
        m_pending_space = false;
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

    // Appends inline content after text, keeping the space a soft wrap trimmed from the end of that text.
    void append_after_text(Utf16View text)
    {
        if (text.is_empty())
            return;
        if (m_pending_space && !m_builder.is_empty() && !m_pending_line_breaks && !m_builder.view().ends_with(u' ') && !text.starts_with(u' '))
            m_builder.append_ascii(' ');
        m_pending_space = false;
        append(text);
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

    bool has_visible_table_content(Layout::BegunRead const& read, DOM::Node const& node, bool node_is_fully_contained)
    {
        if (auto const* box = as_if<Layout::NodeWithStyle>(node.layout_node(read)); box && box->visibility() == CSS::Visibility::Visible && box->user_select_used_value() != CSS::UserSelect::None)
            return true;
        bool visible = false;
        for_each_child_intersecting_range(node, node_is_fully_contained, [&](auto const& child, bool fully_contained) {
            visible = visible || has_visible_table_content(read, child, fully_contained);
        });
        return visible;
    }

    void append_selected_text(Layout::TextNode const& layout_text, DOM::Text const& text)
    {
        auto const* parent = layout_text.parent();
        if (!parent || parent->visibility() != CSS::Visibility::Visible)
            return;
        auto collapse = CSS::white_space_collapse_from_inherited_bits(parent->inherited_set_values().bits);
        bool collapses_spaces = first_is_one_of(collapse, CSS::WhiteSpaceCollapse::Collapse, CSS::WhiteSpaceCollapse::PreserveBreaks);
        size_t range_start = &text == m_range->start_container().ptr() ? m_range->start_offset() : 0;
        size_t range_end = &text == m_range->end_container().ptr() ? m_range->end_offset() : text.length_in_utf16_code_units();
        if (range_start >= range_end)
            return;
        auto source = text.data().utf16_view();
        auto is_collapsible_space = [&](size_t offset) {
            auto unit = source.code_unit_at(offset);
            return collapses_spaces && (first_is_one_of(unit, ' ', '\t', '\r') || (unit == '\n' && collapse == CSS::WhiteSpaceCollapse::Collapse));
        };
        auto slot = Layout::Node::slot_id(&layout_text);
        Layout::RustFFI::FfiVisualLine bounds {};
        bool preserves_breaks = first_is_one_of(collapse, CSS::WhiteSpaceCollapse::Preserve, CSS::WhiteSpaceCollapse::PreserveBreaks, CSS::WhiteSpaceCollapse::BreakSpaces);
        size_t first_offset = range_start;
        size_t last_offset = range_end;
        if (Layout::RustFFI::render_state_text_line_bounds(layout_text.document_host(), slot, &bounds)) {
            first_offset = bounds.start_offset;
            last_offset = bounds.end_offset;
        } else if (first_is_one_of(parent->kind(), Layout::RustFFI::NodeKind::SVGTextBox, Layout::RustFFI::NodeKind::SVGTextPathBox)) {
            // SVG text content has no inline line boxes. Its layout text still carries rendered text and DOM offsets.
            first_offset = 0;
            last_offset = source.length_in_code_units();
            while (first_offset < last_offset && is_collapsible_space(first_offset))
                ++first_offset;
            while (last_offset > first_offset && is_collapsible_space(last_offset - 1))
                --last_offset;
        } else {
            auto selected = source.substring_view(range_start, range_end - range_start);
            if (!preserves_breaks || !all_of(selected, [](u32 code_point) { return code_point == '\n'; })) {
                if (selected.is_ascii_whitespace() && is_collapsible_space(range_start))
                    m_pending_space = true;
                return;
            }
        }
        // A caret boundary can skip a preserved line feed and the collapsible spaces beside it. Include each
        // selected line feed and the whitespace between it and the rendered text.
        if (preserves_breaks) {
            for (auto offset = first_offset; offset > range_start; --offset) {
                if (source.code_unit_at(offset - 1) == '\n')
                    first_offset = offset - 1;
                else if (!is_collapsible_space(offset - 1))
                    break;
            }
            for (auto offset = last_offset; offset < range_end; ++offset) {
                if (source.code_unit_at(offset) == '\n')
                    last_offset = offset + 1;
                else if (!is_collapsible_space(offset))
                    break;
            }
        }
        auto start = max(first_offset, range_start);
        auto end = min(last_offset, range_end);
        if (range_start < first_offset && is_collapsible_space(range_start))
            m_pending_space = true;
        bool ends_in_trimmed_space = last_offset < range_end && is_collapsible_space(range_end - 1);
        if (start >= end) {
            m_pending_space |= ends_in_trimmed_space;
            return;
        }

        Utf16String rendered;
        if (start == 0 && end == text.length_in_utf16_code_units()) {
            rendered = layout_text.rendered_text_for_dom();
        } else {
            auto mapped = layout_text.rendered_text_for_dom_with_offset_mapping();
            size_t from = 0;
            while (from < mapped.offset_mapping.size() && mapped.offset_mapping[from].dom_end_offset <= start)
                ++from;
            size_t to = from;
            while (to < mapped.offset_mapping.size() && mapped.offset_mapping[to].dom_start_offset < end)
                ++to;
            rendered = Utf16String::from_utf16(mapped.text.utf16_view().substring_view(from, to - from));
        }

        auto units = rendered.utf16_view();
        if (units.is_empty())
            return;
        // A soft wrap can trim a space from both painted lines. Keep one separator when the selection includes text
        // on both sides, and drop it at a paragraph or explicit line break.
        if (m_pending_space && !m_builder.is_empty() && !m_pending_line_breaks && !m_builder.view().ends_with(u' ') && !units.starts_with(u' '))
            m_builder.append_ascii(' ');
        m_pending_space = ends_in_trimmed_space;
        flush_pending_breaks();
        if (!collapses_spaces) {
            m_builder.append(units);
            return;
        }

        size_t run_start = 0;
        for (size_t i = 1; i < units.length_in_code_units(); ++i) {
            if (units.code_unit_at(i) != ' ' || units.code_unit_at(i - 1) != ' ')
                continue;
            m_builder.append(units.substring_view(run_start, i - run_start));
            while (i + 1 < units.length_in_code_units() && units.code_unit_at(i + 1) == ' ')
                ++i;
            run_start = i + 1;
        }
        m_builder.append(units.substring_view(run_start));
    }

    void collect(Layout::BegunRead const& read, DOM::Node const& node, bool node_is_fully_contained, SelectedTableParts& table, DOM::UsedUserSelect inherited = {})
    {
        auto const* layout_node = node.layout_node(read);
        SelectedTableParts nested_table;
        bool starts_table = false;
        size_t block_line_breaks = 0;
        if (layout_node && is<Layout::NodeWithStyle>(*layout_node)) {
            auto const& styled_node = as<Layout::NodeWithStyle>(*layout_node);
            auto display = styled_node.display();
            if (display.is_block_outside() || display.is_table_caption()
                || (m_destination == TextDestination::Selection && is<SVG::SVGTextElement>(node))) {
                auto const* element = as_if<HTML::HTMLElement>(node);
                block_line_breaks = element && element->local_name() == HTML::TagNames::p ? 2 : 1;
            }
            starts_table = display.is_table_inside();
            if (m_destination == TextDestination::Selection && (display.is_table_row() || display.is_table_cell())) {
                if (layout_node->user_select_used_value() != CSS::UserSelect::None) {
                    if (styled_node.visibility() != CSS::Visibility::Visible && !has_visible_table_content(read, node, node_is_fully_contained)) {
                        if (display.is_table_cell() && table.visited_cell)
                            ++table.trailing_hidden_cells;
                        return;
                    }
                    // Hidden cells still occupy columns between visible parts of the selection.
                    if (table.trailing_hidden_cells && !m_pending_break_is_explicit) {
                        flush_pending_breaks();
                        m_builder.append_repeated_ascii('\t', table.trailing_hidden_cells);
                    }
                    table.trailing_hidden_cells = 0;
                    if (display.is_table_row()) {
                        if (table.visited_row)
                            request_explicit_line_break();
                        table.visited_row = true;
                        table.visited_cell = false;
                    } else {
                        if (table.visited_cell && !m_pending_break_is_explicit)
                            append("\t"_utf16);
                        table.visited_cell = true;
                        m_pending_space = false;
                    }
                }
            }
        }

        if (block_line_breaks)
            request_line_breaks(block_line_breaks);

        if (auto const* text = as_if<DOM::Text>(node)) {
            if (!layout_node)
                return;
            bool can_use_parent_value = inherited.element && text->flat_tree_parent_element() == inherited.element.ptr()
                && layout_node->has_style_or_parent_with_style() && !layout_node->is_generated_for_pseudo_element() && layout_node->dom_node() == text;
            auto used_value = can_use_parent_value ? inherited.value : layout_node->user_select_used_value();
            if (used_value == CSS::UserSelect::None)
                return;
            // Copied text is the text the page shows, as the selection's stringification is: Whitespace collapsed as
            // it's rendered, and nothing from text that isn't visible.
            append_selected_text(as<Layout::TextNode>(*layout_node), *text);
        } else if (is<HTML::HTMLBRElement>(node)) {
            auto const* box = as_if<Layout::NodeWithStyle>(layout_node);
            bool selectable = box && box->visibility() == CSS::Visibility::Visible && box->user_select_used_value() != CSS::UserSelect::None;
            // A line break that's laid out breaks the copied text even where it's hidden, but one with no box (as for
            // display: none) breaks nothing.
            bool breaks_line = m_destination == TextDestination::Clipboard ? box != nullptr : selectable;
            if (node_is_fully_contained && breaks_line)
                request_explicit_line_break();
        } else if (auto const* image = as_if<HTML::HTMLImageElement>(node)) {
            if (m_destination == TextDestination::Clipboard && node_is_fully_contained && layout_node && layout_node->user_select_used_value() != CSS::UserSelect::None)
                append_after_text(image->alt());
        } else {
            if (m_destination == TextDestination::Selection && node.has_children()) {
                if (auto const* element = as_if<DOM::Element>(node))
                    inherited = { element, DOM::used_user_select_for_element(*element, inherited) };
            }
            for_each_child_intersecting_range(node, node_is_fully_contained, [&](DOM::Node const& child, bool child_is_fully_contained) {
                collect(read, child, child_is_fully_contained, starts_table ? nested_table : table, inherited);
            });
        }

        if (m_destination == TextDestination::Clipboard && layout_node && is<Layout::NodeWithStyle>(*layout_node)) {
            auto display = as<Layout::NodeWithStyle>(*layout_node).display();
            // A space trimmed from the end of a cell's text belongs to that cell, not to the next one.
            if (display.is_table_cell() && node.next_sibling()) {
                m_pending_space = false;
                append("\t"_utf16);
            }
            if (display.is_table_row() && node.next_sibling())
                request_explicit_line_break();
        }

        if (block_line_breaks)
            request_line_breaks(block_line_breaks);
    }

    GC::Root<DOM::Range const> m_range;
    Utf16StringBuilder m_builder;
    TextDestination m_destination;
    size_t m_pending_line_breaks { 0 };
    bool m_pending_break_is_explicit { false };
    bool m_pending_space { false };
};

Utf16String serialize_range_as_plain_text_for_clipboard(DOM::Range const& range)
{
    Layout::ForcedReadScope read { range.start_container()->document() };
    return RangeTextSerializer { range, TextDestination::Clipboard }.serialize(read);
}

Utf16String serialize_selection_as_plain_text(DOM::Range const& range)
{
    auto& document = range.start_container()->document();
    document.update_layout(DOM::UpdateLayoutReason::NavigableSelectedText);
    Layout::ForcedReadScope read { document };
    return RangeTextSerializer { range, TextDestination::Selection }.serialize(read);
}

Utf16String serialize_range_as_html_for_clipboard(DOM::Range& range)
{
    return serialize_styled_markup_for_clipboard(range);
}

}
