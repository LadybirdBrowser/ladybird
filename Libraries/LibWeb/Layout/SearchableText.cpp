/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/HashMap.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/Layout/SearchableText.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>

namespace Web::Layout {

SearchableText SearchableText::collect(Viewport& viewport)
{
    Utf16StringBuilder builder;
    Vector<Segment> segments;
    Vector<RenderedTextOffset> offset_mapping;
    Vector<Block> blocks;

    DOM::Text* pending_space_dom_node = nullptr;
    DOM::Text const* current_dom_node = nullptr;
    size_t pending_space_dom_offset = 0;
    size_t pending_space_dom_end_offset = 0;
    Searchability pending_space_searchability = Searchability::Searchable;
    size_t expected_dom_offset = 0;
    size_t builder_length_in_code_units = 0;
    Boundary next_boundary = Boundary::Block;

    auto flush_block = [&] {
        if (!builder.is_empty())
            blocks.append({ builder.to_string(), move(segments), move(offset_mapping) });
        segments.clear_with_capacity();
        offset_mapping.clear_with_capacity();
        builder.clear();
        builder_length_in_code_units = 0;
        pending_space_dom_node = nullptr;
        current_dom_node = nullptr;
        next_boundary = Boundary::Block;
    };

    auto append_code_unit = [&](DOM::Text& dom_node, char16_t code_unit, size_t dom_start_offset, size_t dom_end_offset, Searchability searchability) {
        if (current_dom_node != &dom_node || dom_start_offset != expected_dom_offset) {
            segments.empend(dom_node, builder_length_in_code_units, searchability, next_boundary, dom_node.is_inert());
            next_boundary = Boundary::None;
        }
        builder.append_code_unit(code_unit);
        offset_mapping.empend(dom_start_offset, dom_end_offset);
        builder_length_in_code_units++;
        current_dom_node = &dom_node;
        expected_dom_offset = dom_end_offset;
    };

    // https://wicg.github.io/scroll-to-text-fragment/#find-a-range-from-a-node-list
    // ISSUE(WICG/scroll-to-text-fragment#98): CharacterData/data is not correct here since that's
    // the text data as it exists in the DOM. This algorithm means to run over the text as rendered
    // (and then convert back to Ranges in the DOM).
    //
    // Collect the rendered pieces first so that the specification-ordered DOM walk below can use
    // rendered text while retaining the corresponding DOM offsets.
    HashMap<DOM::Text const*, Vector<Layout::TextNode const*>> rendered_text_nodes;
    viewport.for_each_in_inclusive_subtree([&](auto const& layout_node) {
        auto const* text_node = as_if<Layout::TextNode>(layout_node);
        if (!text_node)
            return TraversalDecision::Continue;
        if (auto const* dom_text = text_node->dom_text())
            rendered_text_nodes.ensure(dom_text).append(text_node);
        return TraversalDecision::Continue;
    });

    auto has_block_level_display = [](DOM::Element const& element) {
        // https://wicg.github.io/scroll-to-text-fragment/#has-block-level-display
        // A node has block-level display if it is an element and the computed value of its 'display' property is any
        // of 'block', 'table', 'flow-root', 'grid', 'flex', 'list-item'.
        auto const* box_values = element.style_group<CSS::ComputedValues::BoxValues>();
        if (!box_values)
            return false;
        auto const display = CSS::display_from_ffi_display(box_values->display);
        return display == CSS::Display::from_short(CSS::Display::Short::Block)
            || display == CSS::Display::from_short(CSS::Display::Short::Table)
            || display == CSS::Display::from_short(CSS::Display::Short::FlowRoot)
            || display == CSS::Display::from_short(CSS::Display::Short::Grid)
            || display == CSS::Display::from_short(CSS::Display::Short::Flex)
            || display == CSS::Display::from_short(CSS::Display::Short::ListItem);
    };

    auto is_hidden_until_found = [](DOM::Element const& element) {
        auto const* html_element = as_if<HTML::HTMLElement>(element);
        if (!html_element)
            return false;
        auto const& hidden = html_element->get_attribute(HTML::AttributeNames::hidden);
        return hidden.has_value() && hidden->equals_ignoring_ascii_case("until-found"sv);
    };

    auto is_search_invisible = [&](DOM::Element const& element) {
        // https://wicg.github.io/scroll-to-text-fragment/#search-invisible
        // A node is search invisible if it is an element in the HTML namespace and meets any of the following
        // conditions:
        if (element.namespace_uri() != Namespace::HTML)
            return false;

        // 1. The computed value of its 'display' property is 'none'.
        if (auto const* box_values = element.style_group<CSS::ComputedValues::BoxValues>(); box_values && CSS::display_from_ffi_display(box_values->display).is_none() && !is_hidden_until_found(element))
            return true;

        // 2. If the node serializes as void.
        if (element.serializes_as_void())
            return true;

        // 3. Is any of the following types: HTMLIFrameElement, HTMLImageElement, HTMLMeterElement, HTMLObjectElement,
        //    HTMLProgressElement, HTMLStyleElement, HTMLScriptElement, HTMLVideoElement, HTMLAudioElement.
        if (element.local_name().is_one_of(
                HTML::TagNames::iframe,
                HTML::TagNames::img,
                HTML::TagNames::meter,
                HTML::TagNames::object,
                HTML::TagNames::progress,
                HTML::TagNames::style,
                HTML::TagNames::script,
                HTML::TagNames::video,
                HTML::TagNames::audio))
            return true;

        // 4. Is a select element whose multiple content attribute is absent.
        return element.local_name() == HTML::TagNames::select && !element.has_attribute(HTML::AttributeNames::multiple);
    };

    auto is_part_of_a_non_searchable_subtree = [&](DOM::Text const& text) -> Optional<Searchability> {
        // https://wicg.github.io/scroll-to-text-fragment/#non-searchable-subtree
        // A node is part of a non-searchable subtree if it is or has a shadow-including ancestor that is search
        // invisible.
        bool is_revealable = false;
        for (auto const* ancestor = text.parent_or_shadow_host_element(); ancestor; ancestor = ancestor->parent_or_shadow_host_element()) {
            if (is_hidden_until_found(*ancestor))
                is_revealable = true;
            if (auto const* details = as_if<HTML::HTMLDetailsElement>(*ancestor); details && !details->has_attribute(HTML::AttributeNames::open))
                is_revealable = true;
            if (is_search_invisible(*ancestor) && !is_hidden_until_found(*ancestor))
                return {};
        }
        return is_revealable ? Searchability::Revealable : Searchability::Searchable;
    };

    auto nearest_block_ancestor = [&](DOM::Text const& node) -> DOM::Element const* {
        // https://wicg.github.io/scroll-to-text-fragment/#nearest-block-ancestor
        // 1. Let curNode be node.
        DOM::Node const* cur_node = &node;

        // 2. While curNode is non-null
        while (cur_node) {
            // 1. If curNode is not a Text node and it has block-level display then return curNode.
            if (auto const* element = as_if<DOM::Element>(*cur_node); element && has_block_level_display(*element))
                return element;

            // 2. Otherwise, set curNode to curNode's parent.
            cur_node = cur_node->parent();
        }

        // 3. Return node's node document's document element.
        return node.document().document_element();
    };

    auto& document = const_cast<DOM::Document&>(viewport.dom_node());
    Vector<DOM::Node*> nodes_in_shadow_including_tree_order;
    document.for_each_shadow_including_descendant([&](DOM::Node& node) {
        nodes_in_shadow_including_tree_order.append(&node);
        return TraversalDecision::Continue;
    });

    auto is_a_visible_text_node = [&](DOM::Node& node) -> Optional<Searchability> {
        // https://wicg.github.io/scroll-to-text-fragment/#visible-text-node
        // A node is a visible text node if it is a Text node, the computed value of its parent element's 'visibility'
        // property is 'visible', and it is being rendered.
        auto* text = as_if<DOM::Text>(node);
        if (!text)
            return {};

        auto searchability = is_part_of_a_non_searchable_subtree(*text);
        if (!searchability.has_value())
            return {};

        auto* parent = text->parent_or_shadow_host_element();
        auto const* parent_inherited_box_values = parent ? parent->style_group<CSS::ComputedValues::InheritedBoxValues>() : nullptr;
        if (!parent_inherited_box_values || static_cast<CSS::Visibility>(parent_inherited_box_values->visibility) != CSS::Visibility::Visible)
            return {};

        // AD-HOC: Revealable hidden content does not have a layout node yet, so its hidden state
        //         supplies the being-rendered exception. It must still satisfy the visibility check.
        if (!rendered_text_nodes.contains(text) && *searchability != Searchability::Revealable)
            return {};

        return searchability;
    };

    auto append_visible_text_node = [&](DOM::Text& text, Searchability searchability) {
        auto* parent = text.parent_or_shadow_host_element();
        VERIFY(parent);
        auto const* parent_inherited_text_values = parent->style_group<CSS::ComputedValues::InheritedTextValues>();
        VERIFY(parent_inherited_text_values);

        auto const append_text = [&](Utf16View text_to_append, size_t dom_start_offset, TextNode const* layout_text_node = nullptr) {
            auto const white_space_collapse = parent_inherited_text_values->white_space_collapse_value();
            auto const should_collapse = first_is_one_of(
                white_space_collapse,
                CSS::WhiteSpaceCollapse::Collapse,
                CSS::WhiteSpaceCollapse::PreserveBreaks);

            auto append_rendered_code_unit = [&](char16_t code_unit, size_t current_dom_start_offset, size_t current_dom_end_offset) {
                auto const preserve_line_break = white_space_collapse == CSS::WhiteSpaceCollapse::PreserveBreaks && code_unit == '\n';
                if (should_collapse && is_ascii_space(code_unit) && !preserve_line_break) {
                    if (!pending_space_dom_node) {
                        pending_space_dom_node = &text;
                        pending_space_dom_offset = current_dom_start_offset;
                        pending_space_dom_end_offset = current_dom_end_offset;
                        pending_space_searchability = searchability;
                    }
                    return;
                }

                if (pending_space_dom_node) {
                    if (!segments.is_empty())
                        append_code_unit(*pending_space_dom_node, ' ', pending_space_dom_offset, pending_space_dom_end_offset, pending_space_searchability);
                    pending_space_dom_node = nullptr;
                }

                append_code_unit(text, code_unit, current_dom_start_offset, current_dom_end_offset, searchability);
            };

            if (layout_text_node) {
                size_t rendered_offset = 0;
                layout_text_node->for_each_rendered_text_code_unit_dom_range([&](size_t current_dom_start_offset, size_t current_dom_end_offset) {
                    VERIFY(rendered_offset < text_to_append.length_in_code_units());
                    append_rendered_code_unit(text_to_append.code_unit_at(rendered_offset), current_dom_start_offset, current_dom_end_offset);
                    ++rendered_offset;
                });
                VERIFY(rendered_offset == text_to_append.length_in_code_units());
                return;
            }

            for (size_t i = 0; i < text_to_append.length_in_code_units(); ++i)
                append_rendered_code_unit(text_to_append.code_unit_at(i), dom_start_offset + i, dom_start_offset + i + 1);
        };

        if (auto rendered_text = rendered_text_nodes.get(&text); rendered_text.has_value()) {
            for (auto const* text_node : *rendered_text) {
                append_text(
                    text_node->text_for_rendering().utf16_view(),
                    text_node->dom_start_offset(),
                    text_node);
            }
        } else {
            append_text(text.data().utf16_view(), 0);
        }
    };

    // https://wicg.github.io/scroll-to-text-fragment/#find-a-string-in-range
    // NB: This collection pass precomputes the following traversal and textNodeList construction
    //     steps for every searchable block. find_a_string_in_a_range() consumes the resulting
    //     blocks and performs the remaining steps of the algorithm.
    // 1. While searchRange is not collapsed:
    size_t cur_node_index = 0;
    while (cur_node_index < nodes_in_shadow_including_tree_order.size()) {
        // 1. Let curNode be searchRange's start node.
        auto& cur_node = *nodes_in_shadow_including_tree_order[cur_node_index];

        // 2. If curNode is part of a non-searchable subtree:
        if (auto* element = as_if<DOM::Element>(cur_node); element && is_search_invisible(*element)) {
            // 1. Set searchRange's start node to the next node, in shadow-including tree order, that isn't a
            //    shadow-including descendant of curNode.
            do {
                ++cur_node_index;
            } while (cur_node_index < nodes_in_shadow_including_tree_order.size()
                && nodes_in_shadow_including_tree_order[cur_node_index]->is_shadow_including_descendant_of(cur_node));

            // 2. Set searchRange's start offset to 0.
            // 3. Continue.
            continue;
        }

        // 3. If curNode is not a visible text node:
        auto searchability = is_a_visible_text_node(cur_node);
        if (!searchability.has_value()) {
            // 1. Set searchRange's start node to the next node, in shadow-including tree order, that is not a doctype.
            ++cur_node_index;

            // 2. Set searchRange's start offset to 0.
            // 3. Continue.
            continue;
        }

        auto& text = as<DOM::Text>(cur_node);

        // 4. Let blockAncestor be the nearest block ancestor of curNode.
        auto* block_ancestor = nearest_block_ancestor(text);
        VERIFY(block_ancestor);

        // 5. Let textNodeList be a list of Text nodes, initially empty.
        // AD-HOC: builder and segments represent textNodeList's rendered text and DOM offsets.

        // 6. While curNode is a shadow-including descendant of blockAncestor and the position of the boundary point
        //    (curNode, 0) is not after searchRange's end:
        while (cur_node_index < nodes_in_shadow_including_tree_order.size()
            && nodes_in_shadow_including_tree_order[cur_node_index]->is_shadow_including_descendant_of(*block_ancestor)) {
            auto& node = *nodes_in_shadow_including_tree_order[cur_node_index];

            // 1. If curNode has block-level display then break.
            if (auto* element = as_if<DOM::Element>(node); element && has_block_level_display(*element))
                break;

            // 2. If curNode is search invisible:
            if (auto* element = as_if<DOM::Element>(node); element && is_search_invisible(*element)) {
                // 1. Set curNode to the next node, in shadow-including tree order, that isn't a shadow-including
                //    descendant of curNode.
                do {
                    ++cur_node_index;
                } while (cur_node_index < nodes_in_shadow_including_tree_order.size()
                    && nodes_in_shadow_including_tree_order[cur_node_index]->is_shadow_including_descendant_of(node));

                // 2. Continue.
                continue;
            }

            // 3. If curNode is a visible text node then append it to textNodeList.
            if (auto node_searchability = is_a_visible_text_node(node); node_searchability.has_value())
                append_visible_text_node(as<DOM::Text>(node), *node_searchability);

            // 4. Set curNode to the next node in shadow-including tree order.
            ++cur_node_index;
        }

        // 7. Run the find a range from a node list steps given query, searchRange, textNodeList, wordStartBounded,
        //    wordEndBounded and matchMustBeAtBeginning as input. If the resulting range is not null, then return it.
        // AD-HOC: Store textNodeList in the shared index. find_a_string_in_a_range() performs this step when it
        //         consumes the index.
        flush_block();

        // AD-HOC: Steps 8 through 12 are performed by find_a_string_in_a_range(), after it searches the precomputed
        //         textNodeList.
    }

    Optional<Vector<Block>> find_in_page_blocks;
    auto contains_inert_text = false;
    for (auto const& block : blocks) {
        for (auto const& segment : block.segments) {
            if (segment.is_inert) {
                contains_inert_text = true;
                break;
            }
        }
        if (contains_inert_text)
            break;
    }

    if (contains_inert_text) {
        // https://html.spec.whatwg.org/multipage/interaction.html#inert-subtrees
        // When a node is inert:
        // - The user agent should ignore the node for the purposes of find-in-page.
        Utf16StringBuilder find_in_page_builder;
        Vector<Segment> find_in_page_segments;
        Vector<RenderedTextOffset> find_in_page_offset_mapping;
        Vector<Block> filtered_blocks;
        GC::Ptr<DOM::Text> find_in_page_pending_space_dom_node = nullptr;
        DOM::Text const* find_in_page_current_dom_node = nullptr;
        size_t find_in_page_pending_space_dom_offset = 0;
        size_t find_in_page_pending_space_dom_end_offset = 0;
        Searchability find_in_page_pending_space_searchability = Searchability::Searchable;
        size_t find_in_page_expected_dom_offset = 0;
        size_t find_in_page_builder_length_in_code_units = 0;
        Boundary find_in_page_next_boundary = Boundary::Block;

        auto flush_find_in_page_block = [&] {
            if (!find_in_page_builder.is_empty())
                filtered_blocks.append({ find_in_page_builder.to_string(), move(find_in_page_segments), move(find_in_page_offset_mapping) });
            find_in_page_segments.clear_with_capacity();
            find_in_page_offset_mapping.clear_with_capacity();
            find_in_page_builder.clear();
            find_in_page_builder_length_in_code_units = 0;
            find_in_page_pending_space_dom_node = nullptr;
            find_in_page_current_dom_node = nullptr;
            find_in_page_next_boundary = Boundary::Block;
        };

        auto append_find_in_page_code_unit = [&](DOM::Text& dom_node, char16_t code_unit, size_t dom_start_offset, size_t dom_end_offset, Searchability searchability) {
            if (find_in_page_current_dom_node != &dom_node || dom_start_offset != find_in_page_expected_dom_offset) {
                find_in_page_segments.empend(dom_node, find_in_page_builder_length_in_code_units, searchability, find_in_page_next_boundary, false);
                find_in_page_next_boundary = Boundary::None;
            }
            find_in_page_builder.append_code_unit(code_unit);
            find_in_page_offset_mapping.empend(dom_start_offset, dom_end_offset);
            ++find_in_page_builder_length_in_code_units;
            find_in_page_current_dom_node = &dom_node;
            find_in_page_expected_dom_offset = dom_end_offset;
        };

        for (auto const& block : blocks) {
            auto const block_text = block.text.utf16_view();
            for (size_t segment_index = 0; segment_index < block.segments.size(); ++segment_index) {
                auto const& segment = block.segments[segment_index];
                if (segment.is_inert)
                    continue;

                auto dom_node = segment.dom_node.ptr();
                if (!dom_node)
                    continue;
                auto* parent = dom_node->parent_or_shadow_host_element();
                auto const* parent_inherited_text_values = parent ? parent->style_group<CSS::ComputedValues::InheritedTextValues>() : nullptr;
                if (!parent_inherited_text_values)
                    continue;

                auto const segment_end = segment_index + 1 < block.segments.size()
                    ? block.segments[segment_index + 1].start_offset
                    : block_text.length_in_code_units();
                auto const white_space_collapse = parent_inherited_text_values->white_space_collapse_value();
                auto const should_collapse = first_is_one_of(
                    white_space_collapse,
                    CSS::WhiteSpaceCollapse::Collapse,
                    CSS::WhiteSpaceCollapse::PreserveBreaks);

                for (size_t offset = segment.start_offset; offset < segment_end; ++offset) {
                    auto const code_unit = block_text.code_unit_at(offset);
                    auto const& rendered_offset = block.offset_mapping[offset];
                    auto const preserve_line_break = white_space_collapse == CSS::WhiteSpaceCollapse::PreserveBreaks && code_unit == '\n';
                    if (should_collapse && is_ascii_space(code_unit) && !preserve_line_break) {
                        if (!find_in_page_pending_space_dom_node) {
                            find_in_page_pending_space_dom_node = dom_node;
                            find_in_page_pending_space_dom_offset = rendered_offset.dom_start_offset;
                            find_in_page_pending_space_dom_end_offset = rendered_offset.dom_end_offset;
                            find_in_page_pending_space_searchability = segment.searchability;
                        }
                        continue;
                    }

                    if (find_in_page_pending_space_dom_node) {
                        if (!find_in_page_segments.is_empty())
                            append_find_in_page_code_unit(*find_in_page_pending_space_dom_node, ' ', find_in_page_pending_space_dom_offset, find_in_page_pending_space_dom_end_offset, find_in_page_pending_space_searchability);
                        find_in_page_pending_space_dom_node = nullptr;
                    }

                    append_find_in_page_code_unit(*dom_node, code_unit, rendered_offset.dom_start_offset, rendered_offset.dom_end_offset, segment.searchability);
                }
            }
            flush_find_in_page_block();
        }

        find_in_page_blocks = move(filtered_blocks);
    }

    return SearchableText { move(blocks), move(find_in_page_blocks) };
}

Optional<GC::Ref<DOM::Range>> SearchableText::range_from_offsets(Block const& block, size_t start_offset, size_t end_offset) const
{
    if (block.segments.is_empty() || block.offset_mapping.is_empty() || start_offset > end_offset || end_offset > block.offset_mapping.size())
        return {};

    auto segment_for_code_unit = [&](size_t code_unit_offset) -> Segment const& {
        auto const* segment = &block.segments.first();
        for (size_t i = 1; i < block.segments.size() && block.segments[i].start_offset <= code_unit_offset; ++i)
            segment = &block.segments[i];
        return *segment;
    };

    auto const start_mapping_index = start_offset < block.offset_mapping.size() ? start_offset : block.offset_mapping.size() - 1;
    auto const end_mapping_index = end_offset > 0 ? end_offset - 1 : 0;
    auto const& start_mapping = block.offset_mapping[start_mapping_index];
    auto const& end_mapping = block.offset_mapping[end_mapping_index];
    auto start_node = segment_for_code_unit(start_mapping_index).dom_node.ptr();
    auto end_node = segment_for_code_unit(end_mapping_index).dom_node.ptr();
    if (!start_node || !end_node)
        return {};

    auto const start_dom_offset = start_offset < block.offset_mapping.size()
        ? start_mapping.dom_start_offset
        : start_mapping.dom_end_offset;
    auto const end_dom_offset = end_offset > 0
        ? end_mapping.dom_end_offset
        : end_mapping.dom_start_offset;

    if (&start_node->root() != &end_node->root()
        || !start_node->is_connected()
        || !end_node->is_connected()
        || start_dom_offset > start_node->length()
        || end_dom_offset > end_node->length())
        return {};

    return DOM::Range::create(*start_node, start_dom_offset, *end_node, end_dom_offset);
}

SearchableText const& SearchableTextCache::get()
{
    if (!m_searchable_text.has_value())
        m_searchable_text = SearchableText::collect(m_viewport);
    return *m_searchable_text;
}

}
