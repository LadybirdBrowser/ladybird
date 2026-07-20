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
#include <LibWeb/Editing/LegacyFontStyle.h>
#include <LibWeb/Editing/StyledMarkupAccumulator.h>
#include <LibWeb/Editing/StyledMarkupSelection.h>
#include <LibWeb/Editing/VisiblePosition.h>
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

static bool is_inter_block_formatting_whitespace(DOM::Node const& node)
{
    if (!is_ascii_whitespace_text(node))
        return false;

    // Formatting whitespace between block containers participates in the DOM range, but not in either rendered
    // paragraph. Using the surrounding structure also identifies it before layout has been updated for the copy.
    return node.previous_sibling() && node.next_sibling()
        && is_prohibited_paragraph_child(const_cast<DOM::Node&>(*node.previous_sibling()))
        && is_prohibited_paragraph_child(const_cast<DOM::Node&>(*node.next_sibling()));
}

static bool is_empty_paragraph(DOM::Node const& node)
{
    return is_prohibited_paragraph_child(const_cast<DOM::Node&>(node)) && node.child_count() == 1
        && is<HTML::HTMLBRElement>(*node.first_child());
}

static bool is_empty_or_clipped_paragraph(DOM::Node const& node)
{
    if (!is_prohibited_paragraph_child(const_cast<DOM::Node&>(node)))
        return false;

    // INTEROP: Range cloning can leave empty text nodes or inline ancestors in a clipped list item. Blink treats the
    //          item as an interchange paragraph boundary unless a descendant can produce rendered content.
    bool has_renderable_content = false;
    const_cast<DOM::Node&>(node).for_each_in_subtree([&](GC::Ref<DOM::Node> descendant) {
        if (auto* text = as_if<DOM::Text>(*descendant); text && !text->data().is_empty()) {
            has_renderable_content = true;
            return TraversalDecision::Break;
        }
        if (auto* element = as_if<DOM::Element>(*descendant); element && element->is_void_element() && !is<HTML::HTMLBRElement>(*element)) {
            has_renderable_content = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return !has_renderable_content;
}

static GC::Root<DOM::Element> create_interchange_newline(DOM::Document& document)
{
    GC::Root<DOM::Element> line_break { MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML)) };
    line_break->set_attribute_value(HTML::AttributeNames::class_, "Apple-interchange-newline"_utf16);
    return line_break;
}

static void replace_with_interchange_newline(DOM::Document& document, DOM::Node& node)
{
    auto line_break = create_interchange_newline(document);
    node.parent()->insert_before(*line_break, node);
    node.remove();
}

static void normalize_content_after_interchange_newline(DOM::Range const& range, DOM::DocumentFragment& fragment)
{
    GC::RootVector<GC::Ref<HTML::HTMLFontElement>> fonts;
    fragment.for_each_in_subtree([&](GC::Ref<DOM::Node> node) {
        if (is<HTML::HTMLFontElement>(*node))
            fonts.append(static_cast<HTML::HTMLFontElement&>(*node));
        return TraversalDecision::Continue;
    });
    GC::Ptr<HTML::HTMLFontElement> selected_font;
    for (auto& font : fonts) {
        if (!font->descendant_text_content().is_empty())
            selected_font = font;
        else
            font->remove();
    }
    if (!selected_font)
        return;

    auto* source_node = range.end_container().ptr();
    if (range.end_offset() == 0) {
        while (source_node != range.common_ancestor_container().ptr() && !source_node->previous_sibling())
            source_node = source_node->parent();
        if (auto previous_sibling = source_node->previous_sibling()) {
            source_node = previous_sibling;
            while (source_node->last_child())
                source_node = source_node->last_child();
        }
    }
    auto color = resolved_value(*source_node, CSS::PropertyID::Color);
    if (!color)
        return;

    auto& font = *selected_font;
    auto span = MUST(DOM::create_element(range.start_container()->document(), HTML::TagNames::span, Namespace::HTML));
    auto style = Utf16String::formatted("color: {};", color->to_string(CSS::SerializationMode::Normal));
    span->set_attribute_value(HTML::AttributeNames::style, style);
    auto parent = GC::Ref { *font.parent() };
    parent->insert_before(span, font);
    while (font.first_child())
        MUST(span->append_child(*font.first_child()));
    font.remove();

    auto* trailing_text = as_if<DOM::Text>(span->last_child());
    if (!trailing_text || trailing_text->data().is_empty()
        || !is_ascii_space(trailing_text->data().code_unit_at(trailing_text->length_in_utf16_code_units() - 1)))
        return;

    MUST(trailing_text->delete_data(trailing_text->length_in_utf16_code_units() - 1, 1));
    auto whitespace_span = MUST(DOM::create_element(range.start_container()->document(), HTML::TagNames::span, Namespace::HTML));
    whitespace_span->set_attribute_value(HTML::AttributeNames::style, style);
    MUST(whitespace_span->append_child(range.start_container()->document().create_text_node(" "_utf16)));
    parent->insert_before(whitespace_span, span->next_sibling());
}

struct SkippedUnrenderedText {
    bool leading { false };
    bool trailing { false };
};

static void annotate_interchange_newlines(StyledMarkupSelection const& selection, DOM::DocumentFragment& fragment, SkippedUnrenderedText const& skipped_unrendered_text)
{
    auto& range = selection.serialization_range();
    // INTEROP: Blink and WebKit encode a selected rendered paragraph boundary as this private break marker. Their
    //          replacement commands consume it as structure instead of inserting a literal br. Formatting whitespace
    //          in source markup is otherwise omitted from the clipboard representation.
    // INTEROP: A selected empty paragraph before block content is represented at the start of the first content block.
    //          Blink and WebKit descend through its leading formatting wrappers, or into the first item of a list, so
    //          the receiving replacement command preserves the boundary while still recognizing the block fragment.
    auto* leading_paragraph = fragment.first_child();
    bool placed_leading_marker = false;
    bool skipped_fragment_edge_whitespace = skipped_unrendered_text.leading;
    while (leading_paragraph && is_ascii_whitespace_text(*leading_paragraph)) {
        skipped_fragment_edge_whitespace = true;
        leading_paragraph = leading_paragraph->next_sibling();
    }
    if (leading_paragraph && is_empty_or_clipped_paragraph(*leading_paragraph)) {
        auto* content = leading_paragraph->next_sibling();
        while (content && is_ascii_whitespace_text(*content))
            content = content->next_sibling();
        DOM::Node* marker_parent = content;
        auto* following_content = content ? content->next_sibling() : nullptr;
        while (following_content && is_ascii_whitespace_text(*following_content))
            following_content = following_content->next_sibling();
        if (skipped_fragment_edge_whitespace && !following_content) {
            // INTEROP: When a range starts in source formatting whitespace and ends inside the first content block,
            //          Blink emits a top-level boundary followed by inline content. Leave that case to the range-edge
            //          handling below instead of moving the boundary into a partially selected block wrapper.
            marker_parent = nullptr;
        } else if (content && (is<HTML::HTMLUListElement>(*content) || is<HTML::HTMLOListElement>(*content))) {
            marker_parent = as_if<HTML::HTMLLIElement>(content->first_child());
        } else if (!content || !is_prohibited_paragraph_child(*content)) {
            marker_parent = nullptr;
        }

        if (marker_parent) {
            while (marker_parent->first_child() && is_inline_node(*marker_parent->first_child())
                && is<DOM::Element>(*marker_parent->first_child()) && marker_parent->first_child()->has_children()) {
                marker_parent = marker_parent->first_child();
            }
            auto marker = create_interchange_newline(range.start_container()->document());
            marker_parent->insert_before(*marker, marker_parent->first_child());
            placed_leading_marker = true;
            while (fragment.first_child() != content)
                fragment.first_child()->remove();
        }
    }

    auto* previous_sibling = range.start_container()->previous_sibling();
    bool starts_after_empty_paragraph = range.start_offset() != 0 && previous_sibling && is_empty_paragraph(*previous_sibling);
    if (!placed_leading_marker && !starts_after_empty_paragraph
        && selection.has_leading_interchange_newline() && fragment.first_child()) {
        bool inserted_marker = false;
        if (is<DOM::Text>(*fragment.first_child())) {
            replace_with_interchange_newline(range.start_container()->document(), *fragment.first_child());
            inserted_marker = true;
        } else if (skipped_unrendered_text.leading) {
            auto marker = create_interchange_newline(range.start_container()->document());
            fragment.insert_before(*marker, fragment.first_child());
            inserted_marker = true;
        }
        if (inserted_marker) {
            if (fragment.first_child()->next_sibling() && is_empty_paragraph(*fragment.first_child()->next_sibling()))
                fragment.first_child()->next_sibling()->remove();
            normalize_content_after_interchange_newline(range, fragment);
        }
    }

    if (selection.has_trailing_interchange_newline() && fragment.last_child()) {
        if (is<DOM::Text>(*fragment.last_child())) {
            replace_with_interchange_newline(range.start_container()->document(), *fragment.last_child());
        } else if (skipped_unrendered_text.trailing) {
            // INTEROP: Blink and WebKit append the trailing marker from the rendered selection after traversal. It
            //          must not depend on skipped source indentation surviving as a text node at the fragment edge.
            auto marker = create_interchange_newline(range.start_container()->document());
            MUST(fragment.append_child(*marker));
        }
    } else if (!is<DOM::Text>(*range.end_container()) && range.end_offset() > 0) {
        auto* source_before_end = range.end_container()->child_at_index(range.end_offset() - 1);
        if (source_before_end && is_inter_block_formatting_whitespace(*source_before_end)
            && fragment.last_child() && is<DOM::Text>(*fragment.last_child())) {
            replace_with_interchange_newline(range.start_container()->document(), *fragment.last_child());
        } else if (source_before_end && is_empty_paragraph(*source_before_end)) {
            auto marker = create_interchange_newline(range.start_container()->document());
            MUST(fragment.append_child(*marker));
        }
    }
}

static void restore_leading_interchange_newline_ancestry(DOM::DocumentFragment& fragment)
{
    auto* leading_node = fragment.first_child();
    while (leading_node && !is<HTML::HTMLBRElement>(*leading_node))
        leading_node = leading_node->first_child();
    auto* marker = as_if<HTML::HTMLBRElement>(leading_node);
    if (!marker || !marker->has_class(u"Apple-interchange-newline"sv))
        return;

    // INTEROP: Blink's traversal serializer wraps a leading paragraph boundary in the inline ancestors of the first
    //          selected content. Range cloning leaves the synthesized marker beside those wrappers. Reconstruct that
    //          ancestry without depending on the particular formatting element used by the source document. Detached
    //          fragments have no computed display, so classify wrappers by whether they may contain paragraph text.
    while (auto* following_element = as_if<DOM::Element>(marker->next_sibling())) {
        if (is_prohibited_paragraph_child(*following_element) || !following_element->has_children())
            break;
        marker->remove();
        following_element->insert_before(*marker, following_element->first_child());
    }
}

static void wrap_cloned_list_items(DOM::Range const& range, DOM::DocumentFragment& fragment)
{
    if (!fragment.has_children())
        return;
    bool contains_only_list_items = true;
    fragment.for_each_child([&](GC::Ref<DOM::Node> child) {
        if (!is<HTML::HTMLLIElement>(*child)) {
            contains_only_list_items = false;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!contains_only_list_items)
        return;

    auto* source_list = range.common_ancestor_container().ptr();
    while (source_list && !is<HTML::HTMLUListElement>(*source_list) && !is<HTML::HTMLOListElement>(*source_list))
        source_list = source_list->parent();
    if (!source_list)
        return;

    // INTEROP: Range cloning omits the common ancestor itself. Blink and WebKit restore an enclosing list around
    //          cloned list-item siblings so the clipboard fragment retains their shared list structure.
    GC::Root<DOM::Node> list_wrapper { MUST(source_list->clone_node()) };
    while (fragment.first_child())
        MUST(list_wrapper->append_child(*fragment.first_child()));
    MUST(fragment.append_child(*list_wrapper));
}

static void encode_empty_list_item_boundaries(DOM::Range const& range, DOM::DocumentFragment& fragment)
{
    GC::RootVector<GC::Ref<DOM::Node>> lists;
    fragment.for_each_child([&](GC::Ref<DOM::Node> child) {
        if (is<HTML::HTMLUListElement>(*child) || is<HTML::HTMLOListElement>(*child))
            lists.append(child);
        return IterationDecision::Continue;
    });

    for (auto& list : lists) {
        auto* first_item = as_if<HTML::HTMLLIElement>(list->first_child());
        auto* following_item = first_item ? as_if<HTML::HTMLLIElement>(first_item->next_sibling()) : nullptr;
        if (first_item && following_item && is_empty_or_clipped_paragraph(*first_item)) {
            // INTEROP: A selected leading list-item boundary is carried inside the following item. Keeping it within
            //          the list lets replacement distinguish a new rendered paragraph from an empty list item.
            auto marker = create_interchange_newline(range.start_container()->document());
            following_item->insert_before(*marker, following_item->first_child());
            first_item->remove();
        }

        auto* last_item = as_if<HTML::HTMLLIElement>(list->last_child());
        if (!last_item || !is_empty_or_clipped_paragraph(*last_item))
            continue;

        // INTEROP: A selected trailing list-item boundary is serialized after the list. The empty item itself is
        //          transport structure, including when it is the list's only selected child.
        auto marker = create_interchange_newline(range.start_container()->document());
        fragment.insert_before(*marker, list->next_sibling());
        last_item->remove();
    }
}

static void classify_cloned_text_for_interchange(StyledMarkupAccumulator const& accumulator,
    GC::RootVector<GC::Ref<DOM::Text>>& text_with_preserved_breaks,
    GC::RootVector<GC::Ref<DOM::Text>>& unrendered_text)
{
    for (auto const& text : accumulator.serialized_text()) {
        auto white_space_collapse = resolved_keyword(*text.source, CSS::PropertyID::WhiteSpaceCollapse);
        if (white_space_collapse.has_value() && white_space_collapse != CSS::Keyword::Collapse) {
            text_with_preserved_breaks.append(*text.serialized);
        } else if (collect_visual_lines(*text.source).is_empty()) {
            unrendered_text.append(*text.serialized);
        }
    }
}

static void remove_unrendered_text(GC::RootVector<GC::Ref<DOM::Text>> const& unrendered_text)
{
    for (auto& text : unrendered_text) {
        if (text->parent())
            text->remove();
    }
}

static void protect_collapsible_whitespace_for_interchange(DOM::DocumentFragment& fragment,
    GC::RootVector<GC::Ref<DOM::Text>> const& text_with_preserved_breaks)
{
    GC::RootVector<GC::Ref<DOM::Text>> text_nodes;
    fragment.for_each_in_subtree([&](GC::Ref<DOM::Node> node) {
        if (is<DOM::Text>(*node))
            text_nodes.append(static_cast<DOM::Text&>(*node));
        return TraversalDecision::Continue;
    });

    auto preserves_breaks = [&](DOM::Text const& text) {
        return any_of(text_with_preserved_breaks, [&](auto const& preserved_text) { return preserved_text.ptr() == &text; });
    };
    for (auto& text : text_nodes) {
        if (!text->parent())
            continue;
        if (preserves_breaks(text) || is_inter_block_formatting_whitespace(text))
            continue;
        auto data = text->data();
        if (data.is_empty())
            continue;

        auto parent = GC::Root { *text->parent() };
        for (auto const& token : rebalance_whitespace_for_interchange(data.utf16_view())) {
            if (token.type == InterchangeWhitespaceTokenType::Text) {
                parent->insert_before(fragment.document().create_text_node(token.text), text);
            } else {
                auto span = MUST(DOM::create_element(fragment.document(), HTML::TagNames::span, Namespace::HTML));
                MUST(span->append_child(fragment.document().create_text_node("\u00a0"_utf16)));
                parent->insert_before(span, text);
            }
        }
        text->remove();
    }
}

Utf16String serialize_styled_markup_for_clipboard(DOM::Range& range)
{
    if (range.collapsed())
        return {};

    range.start_container()->document().update_layout_if_needed_for_node(range.common_ancestor_container(), DOM::UpdateLayoutReason::NavigableSelectedText);
    StyledMarkupSelection selection { range };
    // INTEROP: Blink and WebKit represent a selection containing exactly one rendered paragraph boundary with a
    //          standalone interchange marker. Let replacement interpret that marker instead of cloning either block.
    if (selection.contains_only_interchange_newline())
        return "<br class=\"Apple-interchange-newline\">"_utf16;
    StyledMarkupAccumulator accumulator { selection };
    auto fragment = accumulator.fragment();
    GC::RootVector<GC::Ref<DOM::Text>> text_with_preserved_breaks;
    GC::RootVector<GC::Ref<DOM::Text>> unrendered_text;
    classify_cloned_text_for_interchange(accumulator, text_with_preserved_breaks, unrendered_text);
    // INTEROP: A range ending at offset zero has not entered its final text run. Blink keeps a selected leading
    //          paragraph boundary at the fragment edge in that case, while ranges containing final-run text move the
    //          boundary into the selected structure. Preserve that distinction after removing unrendered edge text.
    SkippedUnrenderedText skipped_unrendered_text {
        .leading = any_of(unrendered_text, [&](auto const& text) {
            return text.ptr() == fragment->first_child() && !text->data().is_empty()
                && is<DOM::CharacterData>(*range.end_container()) && range.end_offset() == 0;
        }),
        .trailing = any_of(unrendered_text, [&](auto const& text) { return text.ptr() == fragment->last_child(); }),
    };
    // INTEROP: Blink and WebKit skip text without rendered content during traversal. Remove it before structural
    //          wrapping and interchange-marker placement so source indentation cannot become fragment topology.
    remove_unrendered_text(unrendered_text);
    wrap_cloned_list_items(range, *fragment);
    encode_empty_list_item_boundaries(range, *fragment);
    annotate_interchange_newlines(selection, fragment, skipped_unrendered_text);
    restore_leading_interchange_newline_ancestry(*fragment);
    accumulator.remove_partial_block_transport_wrapper();

    // INTEROP: Chromium represents a partial text-node selection with the computed inline styles that distinguish it
    //          from its editing host. The DOM Range cloning algorithm cannot preserve these styles because the common
    //          ancestor is the text node itself, so provide the missing formatting context explicitly.
    if (range.start_container() == range.end_container() && is<DOM::Text>(*range.start_container())
        && (range.start_offset() != 0 || range.end_offset() != range.start_container()->length())) {
        auto editing_host = range.start_container()->editing_host();
        if (editing_host) {
            Utf16StringBuilder style;
            for (auto property_id : { CSS::PropertyID::Color, CSS::PropertyID::BackgroundColor, CSS::PropertyID::FontFamily,
                     CSS::PropertyID::FontSize, CSS::PropertyID::FontStyle, CSS::PropertyID::FontWeight,
                     CSS::PropertyID::TextDecorationLine }) {
                auto selected_value = resolved_value(range.start_container(), property_id);
                auto host_value = resolved_value(*editing_host, property_id);
                if (!selected_value || !host_value)
                    continue;

                auto selected_value_string = selected_value->to_string(CSS::SerializationMode::Normal);
                if (selected_value_string == host_value->to_string(CSS::SerializationMode::Normal))
                    continue;
                style.appendff("{}: {};", CSS::string_from_property_id(property_id), selected_value_string);
            }

            if (!style.is_empty()) {
                auto span = MUST(DOM::create_element(range.start_container()->document(), HTML::TagNames::span, Namespace::HTML));
                span->set_attribute_value(HTML::AttributeNames::style, style.to_string());
                while (fragment->has_children())
                    MUST(span->append_child(*fragment->first_child()));
                MUST(fragment->append_child(span));
            }
        }
    }

    // INTEROP: Blink and WebKit serialize collapsible whitespace through span-wrapped non-breaking spaces. The wrapper
    //          preserves both the rendered space and its text-node seam until rich replacement performs its ordered
    //          whitespace cleanup. Text whose CSS preserves line breaks is serialized verbatim.
    protect_collapsible_whitespace_for_interchange(*fragment, text_with_preserved_breaks);
    if (!accumulator.serialized_text().is_empty())
        materialize_single_run_legacy_font_style(*fragment, *accumulator.serialized_text().first().source);

    return MUST(fragment->serialize_fragment(HTML::RequireWellFormed::No));
}

}
