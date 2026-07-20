/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/TypeCasts.h>
#include <LibGC/RootVector.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/Editing/InterchangeWhitespace.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/ReplacementFragment.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Namespace.h>

namespace Web::Editing {

static void remove_formatting_whitespace_between_blocks(DOM::DocumentFragment& fragment)
{
    GC::RootVector<GC::Ref<DOM::Node>> whitespace_nodes;
    fragment.for_each_child([&](GC::Ref<DOM::Node> child) {
        if (is_ascii_whitespace_text(child))
            whitespace_nodes.append(child);
        return IterationDecision::Continue;
    });
    for (auto& whitespace : whitespace_nodes)
        whitespace->remove();
}

static void make_whitespace_round_trippable(DOM::DocumentFragment& fragment)
{
    GC::RootVector<GC::Ref<DOM::Text>> text_nodes;
    fragment.for_each_in_subtree([&](GC::Ref<DOM::Node> node) {
        if (is<DOM::Text>(*node))
            text_nodes.append(static_cast<DOM::Text&>(*node));
        return TraversalDecision::Continue;
    });

    auto& document = fragment.document();
    for (auto& text : text_nodes) {
        if (!text->parent())
            continue;
        auto data = text->data();
        if (data.is_empty() || (!is_ascii_space(data.code_unit_at(0)) && !is_ascii_space(data.code_unit_at(data.length_in_code_units() - 1))))
            continue;

        auto parent = GC::Root { *text->parent() };
        for (auto const& token : rebalance_whitespace_for_interchange(data.utf16_view())) {
            auto token_text = token.type == InterchangeWhitespaceTokenType::Text ? token.text : "\u00a0"_utf16;
            parent->insert_before(document.create_text_node(token_text), text);
        }
        text->remove();
    }
}

static void unwrap_interchange_whitespace_spans(DOM::DocumentFragment& fragment)
{
    GC::RootVector<GC::Ref<DOM::Element>> spans;
    fragment.for_each_in_subtree([&](GC::Ref<DOM::Node> node) {
        auto* element = as_if<DOM::Element>(*node);
        if (!element || element->local_name() != HTML::TagNames::span || element->has_attributes()
            || element->child_count() != 1)
            return TraversalDecision::Continue;
        auto* text = as_if<DOM::Text>(element->first_child());
        if (text && text->data() == "\u00a0"_utf16)
            spans.append(*element);
        return TraversalDecision::Continue;
    });
    for (auto& span : spans) {
        if (!span->parent())
            continue;
        auto parent = GC::Root { *span->parent() };
        while (span->first_child())
            parent->insert_before(*span->first_child(), span);
        span->remove();
    }
}

static bool has_renderable_descendant(DOM::Element& element)
{
    bool has_renderable_descendant = false;
    element.for_each_in_subtree([&](GC::Ref<DOM::Node> descendant) {
        if (auto const* text = as_if<DOM::Text>(*descendant); text && !text->data().is_empty()) {
            has_renderable_descendant = true;
            return TraversalDecision::Break;
        }
        auto const* descendant_element = as_if<DOM::Element>(*descendant);
        if (descendant_element && descendant_element->is_void_element()) {
            has_renderable_descendant = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return has_renderable_descendant;
}

static void remove_empty_inline_staging_wrappers(DOM::DocumentFragment& fragment)
{
    GC::RootVector<GC::Ref<DOM::Element>> empty_wrappers;
    fragment.for_each_in_subtree([&](GC::Ref<DOM::Node> node) {
        auto* element = as_if<DOM::Element>(*node);
        if (element && !element->is_void_element() && !is_prohibited_paragraph_child(*element) && !has_renderable_descendant(*element))
            empty_wrappers.append(*element);
        return TraversalDecision::Continue;
    });

    // INTEROP: Blink and WebKit render parsed replacement fragments in a staging container and remove nodes which do
    //          not produce content. Range serialization can create empty partial formatting ancestors at either edge;
    //          pruning them here keeps those transport wrappers out of paragraph and style integration.
    for (auto& wrapper : empty_wrappers.in_reverse()) {
        if (wrapper->parent())
            wrapper->remove();
    }
}

ReplacementFragment::ReplacementFragment(DOM::Range& range, TrustedTypes::TrustedHTMLOrString const& value)
{
    EditingHistory::ProxyMutationScope mutation_scope { *range.start_container() };
    m_fragment = MUST(range.create_contextual_fragment(value));
    remove_formatting_whitespace_between_blocks(*m_fragment);

    auto* first_leaf = m_fragment->first_child();
    while (first_leaf && first_leaf->first_child())
        first_leaf = first_leaf->first_child();
    if (auto* line_break = as_if<HTML::HTMLBRElement>(first_leaf); line_break
        && line_break->has_class(u"Apple-interchange-newline"sv))
        m_has_interchange_newline_at_start = true;

    // INTEROP: Chromium preserves a leading interchange newline as inserted content. A trailing interchange newline
    //          remains transport metadata and is consumed below. Keeping those representations distinct avoids
    //          applying the leading paragraph boundary twice, once as a br and once as replacement state.
    if (auto* line_break = as_if<HTML::HTMLBRElement>(m_fragment->last_child()); line_break
        && line_break->has_class(u"Apple-interchange-newline"sv)) {
        m_has_interchange_newline_at_end = true;
        line_break->remove();
    }

    // INTEROP: Blink and WebKit wrap serialized protected spaces in otherwise empty spans. Their replacement commands
    //          discard those implementation wrappers while retaining the text-node seam for whitespace cleanup.
    unwrap_interchange_whitespace_spans(*m_fragment);
    remove_empty_inline_staging_wrappers(*m_fragment);

    // INTEROP: Blink and WebKit make collapsible whitespace at serialized text-run boundaries round-trippable with
    //          non-breaking spaces. Preserve their node seams here so the replacement cleanup can normalize only the
    //          spaces which are surrounded by ordinary text. The conversion also serves as a fallback for HTML from
    //          external producers which did not protect its edge whitespace while serializing.
    make_whitespace_round_trippable(*m_fragment);

    m_contains_only_text = m_fragment->has_children();
    m_fragment->for_each_child([&](GC::Ref<DOM::Node> child) {
        if (!is<DOM::Text>(*child))
            m_contains_only_text = false;
        // INTEROP: Detached fragments have no computed display. Blink classifies block paste content using the HTML
        //          elements which cannot be paragraph children instead of consulting layout.
        if (is_prohibited_paragraph_child(child)) {
            m_contains_block_content = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
}

GC::RootVector<GC::Ref<DOM::Node>> ReplacementFragment::children() const
{
    GC::RootVector<GC::Ref<DOM::Node>> children;
    m_fragment->for_each_child([&](GC::Ref<DOM::Node> child) {
        children.append(child);
        return IterationDecision::Continue;
    });
    return children;
}

}
