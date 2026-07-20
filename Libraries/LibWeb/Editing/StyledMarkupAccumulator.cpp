/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/TypeCasts.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/StyledMarkupAccumulator.h>
#include <LibWeb/Editing/StyledMarkupSelection.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/TagNames.h>

namespace Web::Editing {

static bool should_serialize_node(DOM::Node const& node)
{
    if (node.layout_node())
        return true;

    auto const* element = as_if<DOM::Element>(node);
    return element && element->computed_values() && element->computed_values()->display().is_contents();
}

StyledMarkupAccumulator::StyledMarkupAccumulator(StyledMarkupSelection const& selection)
    : m_range(selection.serialization_range())
    , m_fragment(m_range->start_container()->document().create_document_fragment())
{
    auto common_ancestor = m_range->common_ancestor_container();
    if (is<DOM::CharacterData>(*common_ancestor)) {
        append_selected_node(common_ancestor, *m_fragment, SelectionCoverage::Partial);
        return;
    }

    for (auto* child = common_ancestor->first_child(); child; child = child->next_sibling()) {
        if (!participates_in_selection(*child))
            continue;
        auto coverage = m_range->contains_node(*child) ? SelectionCoverage::Full : SelectionCoverage::Partial;
        append_selected_node(*child, *m_fragment, coverage);
    }
}

bool StyledMarkupAccumulator::participates_in_selection(DOM::Node& node) const
{
    return m_range->contains_node(node)
        || node.is_inclusive_ancestor_of(m_range->start_container())
        || node.is_inclusive_ancestor_of(m_range->end_container());
}

bool StyledMarkupAccumulator::append_selected_node(DOM::Node& source, DOM::Node& destination_parent, SelectionCoverage coverage)
{
    // INTEROP: Blink and WebKit traverse the rendered tree when producing clipboard markup. Skip an unrendered node
    //          and its entire subtree so display:none content cannot enter the clipboard. A display:contents element
    //          is the exception: it has no layout node of its own, but both engines retain it around rendered children.
    if (!should_serialize_node(source))
        return false;

    // INTEROP: Blink and WebKit treat a trailing br as a block's caret placeholder when the selection ends
    //          immediately after its containing branch. It does not enter the serialized markup, while the same
    //          break in an earlier fully selected block remains part of that block's paragraph structure.
    if (is<HTML::HTMLBRElement>(source) && source.previous_sibling() && source.parent() && &source == source.parent()->last_child()) {
        auto* selected_branch = &source;
        while (selected_branch->parent() && selected_branch->parent() != m_range->end_container().ptr()) {
            if (selected_branch != selected_branch->parent()->last_child())
                break;
            selected_branch = selected_branch->parent();
        }
        if (selected_branch->parent() == m_range->end_container().ptr()
            && m_range->end_offset() == selected_branch->index() + 1)
            return false;
    }

    auto fully_selected = coverage == SelectionCoverage::Full;
    auto clone = MUST(source.clone_node());
    MUST(destination_parent.append_child(clone));

    if (auto* source_character_data = as_if<DOM::CharacterData>(source)) {
        size_t start_offset = &source == m_range->start_container().ptr() ? m_range->start_offset() : 0;
        size_t end_offset = &source == m_range->end_container().ptr() ? m_range->end_offset() : source.length();
        if (fully_selected) {
            start_offset = 0;
            end_offset = source.length();
        }
        auto data = MUST(source_character_data->substring_data(start_offset, end_offset - start_offset));
        auto emitted_content = !data.is_empty();
        as<DOM::CharacterData>(*clone).set_data(move(data));
        if (is<DOM::Text>(source))
            m_serialized_text.append({ static_cast<DOM::Text&>(source), static_cast<DOM::Text&>(*clone) });
        return emitted_content;
    }

    bool emitted_content = false;
    for (auto* child = source.first_child(); child; child = child->next_sibling()) {
        if (!fully_selected && !participates_in_selection(*child))
            continue;
        auto child_coverage = fully_selected || m_range->contains_node(*child) ? SelectionCoverage::Full : SelectionCoverage::Partial;
        emitted_content |= append_selected_node(*child, clone, child_coverage);
    }

    auto* element = as_if<DOM::Element>(source);
    auto is_atomic_content = element && element->is_void_element();
    // INTEROP: Blink and WebKit open ancestors while traversing selected nodes, so an inline ancestor reached only
    //          through a zero-length range edge never enters the output. Empty blocks and atomic elements still carry
    //          paragraph structure or content and must remain serializable.
    // INTEROP: A range ending at offset zero in a text node reaches its div only as traversal ancestry. Blink and
    //          WebKit do not emit that empty transport wrapper. A rendered empty paragraph has its placeholder br
    //          inside StyledMarkupSelection's range and therefore has emitted content by the time we get here.
    auto is_unselected_trailing_wrapper = source.is_inclusive_ancestor_of(m_range->end_container())
        && !source.is_inclusive_ancestor_of(m_range->start_container())
        && is<DOM::CharacterData>(*m_range->end_container())
        && element && element->local_name() == HTML::TagNames::div;
    if (coverage == SelectionCoverage::Partial && element && !is_atomic_content && !emitted_content
        && (is_unselected_trailing_wrapper || !is_prohibited_paragraph_child(source))) {
        clone->remove();
        return false;
    }
    return emitted_content || is_atomic_content || is_prohibited_paragraph_child(source);
}

static bool is_interchange_newline(DOM::Node const& node)
{
    auto const* line_break = as_if<HTML::HTMLBRElement>(node);
    return line_break && line_break->has_class(u"Apple-interchange-newline"sv);
}

static bool is_list_transport_sibling(DOM::Node const& node)
{
    if (auto const* text = as_if<DOM::Text>(node))
        return all_of(text->data().utf16_view(), is_ascii_space);
    if (is_interchange_newline(node))
        return true;
    return is<DOM::Element>(node) && node.child_count() == 1
        && is<HTML::HTMLBRElement>(*node.first_child());
}

static GC::Ptr<DOM::Element> single_list_transport_wrapper(DOM::DocumentFragment& fragment)
{
    GC::Ptr<DOM::Element> list;
    fragment.for_each_child([&](GC::Ref<DOM::Node> child) {
        auto* element = as_if<DOM::Element>(*child);
        if (element && element->local_name().is_one_of(HTML::TagNames::ul, HTML::TagNames::ol)) {
            if (list) {
                list = nullptr;
                return IterationDecision::Break;
            }
            list = *element;
        } else if (!is_list_transport_sibling(child)) {
            list = nullptr;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return list;
}

void StyledMarkupAccumulator::remove_partial_block_transport_wrapper()
{
    auto* first_child = m_fragment->first_child();
    auto* block = as_if<DOM::Element>(first_child);
    if (first_child && is_interchange_newline(*first_child))
        block = as_if<DOM::Element>(first_child->next_sibling());
    auto visible_start = VisiblePosition::create(m_range->start_container()->document(), m_range->start());
    auto visible_end = VisiblePosition::create(m_range->start_container()->document(), m_range->end());
    if (block && block->local_name() == HTML::TagNames::div) {
        // INTEROP: A leading interchange newline is selected boundary content, so the following div is not a lone
        //          transport wrapper. This differs from list serialization, where boundary markers may surround the
        //          single structural list wrapper without becoming part of it.
        if (block != m_fragment->first_child() || block != m_fragment->last_child())
            return;
        if (visible_start.is_start_of_containing_block() && visible_end.is_end_of_containing_block())
            return;

        auto* source_block = m_range->end_container().ptr();
        while (source_block && (!is<DOM::Element>(*source_block) || static_cast<DOM::Element&>(*source_block).local_name() != HTML::TagNames::div))
            source_block = source_block->parent();
        auto* start_text = as_if<DOM::Text>(*m_range->start_container());
        if (source_block && start_text && start_text->next_sibling() == source_block
            && all_of(start_text->data().utf16_view(), is_ascii_space)) {
            // INTEROP: Formatting whitespace before a partially selected block is not serialized, but the range still
            //          crosses into that block. Keep its transport wrapper so replacement retains the paragraph seam.
            return;
        }

        // INTEROP: Blink and WebKit traverse selected nodes and wrap only ancestors which carry required structure or
        //          formatting. A lone partial div is transport structure rather than selected content, so flatten it.
        while (block->first_child())
            m_fragment->insert_before(*block->first_child(), block);
        block->remove();
        return;
    }

    auto list = single_list_transport_wrapper(*m_fragment);
    if (!list || list->child_count() != 1 || !is<HTML::HTMLLIElement>(*list->first_child()))
        return;

    auto* source_list_item = m_range->end_container().ptr();
    while (source_list_item && !is<HTML::HTMLLIElement>(*source_list_item))
        source_list_item = source_list_item->parent();
    if (!source_list_item || source_list_item->is_inclusive_ancestor_of(m_range->start_container())
        || visible_end.is_end_of_containing_block())
        return;

    // INTEROP: A range entering a list and ending inside its first selected item contains only an inline prefix of
    //          that paragraph. Blink and WebKit do not retain the list ancestors reached through the range edge.
    auto list_item = GC::Root { *list->first_child() };
    while (list_item->first_child())
        m_fragment->insert_before(*list_item->first_child(), list);
    list->remove();
}

}
