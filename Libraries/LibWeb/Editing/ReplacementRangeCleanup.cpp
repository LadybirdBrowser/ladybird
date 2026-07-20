/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/InsertedContent.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/MutationTrackedRange.h>
#include <LibWeb/Editing/ReplacementRangeCleanup.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>

namespace Web::Editing {

enum class TraversalDirection {
    Backward,
    Forward,
};

// Blink passes the replacement endpoints through every text-node join, while Gecko relies on tracked DOM points.
// Keep the equivalent invariant in one place: every join relocates the final caret before removing either node. The
// caller remains responsible only for deciding which semantic seam should be coalesced.
class TextNodeCoalescer {
public:
    explicit TextNodeCoalescer(ReplacementEndpoints& endpoints)
        : m_endpoints(endpoints)
    {
    }

    void merge_left_into_right(DOM::Text& left, DOM::Text& right)
    {
        auto left_length = left.length_in_utf16_code_units();
        MUST(insert_data(right, 0, left.data()));
        m_endpoints.prepare_for_merging_left_text_into_right(left, right, left_length);
        remove_node(left);
    }

    void merge_right_into_left(DOM::Text& left, DOM::Text& right)
    {
        auto left_length = left.length_in_utf16_code_units();
        MUST(insert_data(left, left_length, right.data()));
        m_endpoints.prepare_for_merging_right_text_into_left(left, right, left_length);
        remove_node(right);
    }

private:
    ReplacementEndpoints& m_endpoints;
};

static Optional<u16> adjacent_text_character(DOM::Text const& text, size_t offset, TraversalDirection direction)
{
    auto search_forward = direction == TraversalDirection::Forward;
    if (search_forward && offset < text.length_in_utf16_code_units())
        return text.data().code_unit_at(offset);
    if (!search_forward && offset > 0)
        return text.data().code_unit_at(offset - 1);

    auto block = block_node_of_node(const_cast<DOM::Text&>(text));
    for (auto* node = search_forward ? text.next_in_pre_order(block) : text.previous_in_pre_order();
        node && node != block; node = search_forward ? node->next_in_pre_order(block) : node->previous_in_pre_order()) {
        if (is<HTML::HTMLBRElement>(*node) || is<HTML::HTMLImageElement>(*node)
            || (is_prohibited_paragraph_child(const_cast<DOM::Node&>(*node)) && node != block))
            return {};
        auto* adjacent_text = as_if<DOM::Text>(*node);
        if (!adjacent_text || adjacent_text->data().is_empty())
            continue;
        return search_forward
            ? adjacent_text->data().code_unit_at(0)
            : adjacent_text->data().code_unit_at(adjacent_text->length_in_utf16_code_units() - 1);
    }
    return {};
}

static GC::RootVector<GC::Ref<DOM::Text>> normalize_inserted_nbsp(InsertedContent const& inserted_content)
{
    // INTEROP: Chromium normalizes protected clipboard spaces only when ordinary text surrounds them on both sides.
    GC::RootVector<GC::Ref<DOM::Text>> normalized_whitespace;
    auto text_nodes = inserted_content.text_nodes();
    for (auto& text : text_nodes) {
        auto data = text->data();
        if (!data.contains(0xa0))
            continue;

        Utf16StringBuilder replacement;
        bool changed = false;
        for (size_t offset = 0; offset < data.length_in_code_units(); ++offset) {
            auto code_unit = data.code_unit_at(offset);
            if (code_unit == 0xa0) {
                auto previous = adjacent_text_character(*text, offset, TraversalDirection::Backward);
                auto next = adjacent_text_character(*text, offset + 1, TraversalDirection::Forward);
                if (previous.has_value() && next.has_value()
                    && !is_ascii_space(*previous) && *previous != 0xa0
                    && !is_ascii_space(*next) && *next != 0xa0) {
                    code_unit = ' ';
                    changed = true;
                }
            }
            replacement.append_code_point(code_unit);
        }
        if (changed) {
            MUST(replace_data(*text, 0, text->length_in_utf16_code_units(), replacement.to_string()));
            if (all_of(text->data().utf16_view(), is_ascii_space))
                normalized_whitespace.append(text);
        }
    }
    return normalized_whitespace;
}

static void coalesce_normalized_whitespace_in_integrated_end_paragraph(InsertedContent& inserted_content, GC::RootVector<GC::Ref<DOM::Text>> const& normalized_whitespace)
{
    if (!inserted_content.end_paragraph_was_integrated())
        return;
    auto integrated_end_block = block_node_of_node(*inserted_content.end_boundary().node);

    // INTEROP: Integrating the final pasted paragraph removes a redundant whitespace wrapper in Blink and WebKit.
    //          Coalesce only that newly exposed seam; unrelated clipboard whitespace remains split.
    TextNodeCoalescer coalescer { inserted_content.endpoints() };
    for (auto& text : normalized_whitespace) {
        if (!text->parent() || block_node_of_node(*text) != integrated_end_block)
            continue;
        // Chromium keeps normalized clipboard whitespace as a separate replacement endpoint after a noninitial text
        // run in a complex paragraph. Its node identity is observable through Selection even though coalescing would
        // serialize to the same HTML. A simple leading text run absorbs its trailing transport whitespace instead.
        auto* previous_text = as_if<DOM::Text>(text->previous_sibling());
        auto endpoint_follows_noninitial_text_run = inserted_content.endpoints().end().node.ptr() == text.ptr()
            && previous_text && previous_text->previous_sibling();
        if (endpoint_follows_noninitial_text_run)
            continue;
        if (previous_text)
            coalescer.merge_right_into_left(*previous_text, text);
        else if (auto* next_text = as_if<DOM::Text>(text->next_sibling()))
            coalescer.merge_left_into_right(text, *next_text);
    }
}

static void merge_text_nodes_at_start_of_inserted_content(InsertedContent& inserted_content)
{
    // INTEROP: Blink completes a replacement by choosing the text node at, immediately before, or immediately after
    //          the inserted range's start, then merging its adjacent text siblings into it. Both replacement endpoints
    //          are relocated by each join so the final selection continues to delimit the inserted content.
    auto start = inserted_content.endpoints().start();
    GC::Ptr<DOM::Text> start_text = as_if<DOM::Text>(*start.node);
    if (!start_text && start.offset > 0)
        start_text = as_if<DOM::Text>(start.node->child_at_index(start.offset - 1));
    if (!start_text)
        start_text = as_if<DOM::Text>(start.node->child_at_index(start.offset));
    if (!start_text)
        return;

    // A protected space can represent the same rendered caret as the start of the following text. Blink resolves
    // that interior position forward before completing the replacement, so retain the following text node and fold
    // the protected-space node into it. At a paragraph boundary the positions are distinct and the space stays put.
    auto start_is_interior_protected_whitespace = inserted_content.contains(*start_text)
        && !VisiblePosition::create(start.node->document(), start).is_start_of_paragraph()
        && all_of(start_text->data().utf16_view(), [](u16 code_unit) { return is_ascii_space(code_unit) || code_unit == 0xa0; });
    if (!is<DOM::Text>(start_text->previous_sibling()) && start_is_interior_protected_whitespace) {
        if (auto* next_text = as_if<DOM::Text>(start_text->next_sibling()); next_text && inserted_content.contains(*next_text)
            && any_of(next_text->data().utf16_view(), [](u16 code_unit) { return !is_ascii_space(code_unit) && code_unit != 0xa0; }))
            start_text = next_text;
    }

    TextNodeCoalescer coalescer { inserted_content.endpoints() };
    if (auto* previous_text = as_if<DOM::Text>(start_text->previous_sibling()))
        coalescer.merge_left_into_right(*previous_text, *start_text);

    if (auto* next_text = as_if<DOM::Text>(start_text->next_sibling())) {
        auto* following_atomic_content = next_text->next_sibling();
        bool atomic_run_has_following_inserted_text = false;
        if (following_atomic_content && editing_ignores_content(*following_atomic_content)) {
            for (auto& text : inserted_content.text_nodes()) {
                if (following_atomic_content->is_before(text)
                    && any_of(text->data().utf16_view(), [](u16 code_unit) { return !is_ascii_space(code_unit) && code_unit != 0xa0; })) {
                    atomic_run_has_following_inserted_text = true;
                    break;
                }
            }
        }
        auto next_text_is_protected_inserted_whitespace = inserted_content.contains(*next_text)
            && inserted_content.replacement_topology() == InsertedContent::ReplacementTopology::Inline
            && all_of(next_text->data().utf16_view(), [](u16 code_unit) { return is_ascii_space(code_unit) || code_unit == 0xa0; })
            && following_atomic_content && editing_ignores_content(*following_atomic_content)
            && !atomic_run_has_following_inserted_text && inserted_content.insertion_was_inside_paragraph()
            && !inserted_content.insertion_was_after_whitespace();
        if (!next_text_is_protected_inserted_whitespace)
            coalescer.merge_right_into_left(*start_text, *next_text);
    }
}

static void merge_text_nodes_at_end_of_inserted_content(InsertedContent& inserted_content)
{
    auto text_nodes = inserted_content.text_nodes();
    if (text_nodes.is_empty())
        return;
    auto last_text = text_nodes.last();
    auto* next_text = as_if<DOM::Text>(last_text->next_sibling());
    if (!next_text)
        return;

    // INTEROP: Blink completes an integrated end paragraph by retaining the endpoint text node and folding both of
    //          its adjacent text siblings into it. The replacement endpoint continues to delimit the inserted content
    //          rather than moving past a prepended protected-space node.
    TextNodeCoalescer coalescer { inserted_content.endpoints() };
    if (auto* previous_text = as_if<DOM::Text>(last_text->previous_sibling())) {
        auto end_was_in_last_text = inserted_content.endpoints().end().node.ptr() == last_text.ptr();
        auto end_offset = inserted_content.endpoints().end().offset;
        coalescer.merge_left_into_right(*previous_text, last_text);
        if (end_was_in_last_text)
            inserted_content.endpoints().set_end({ last_text, end_offset });
    }
    coalescer.merge_right_into_left(last_text, *next_text);
}

static void remove_empty_modifiable_containers(InsertedContent const& inserted_content)
{
    GC::RootVector<GC::Ref<DOM::Element>> empty_containers;
    for (auto& inserted_node : inserted_content.nodes()) {
        inserted_node->for_each_in_inclusive_subtree([&](GC::Ref<DOM::Node> node) {
            auto* element = as_if<DOM::Element>(*node);
            if (element && !element->has_children() && is_modifiable_element(*element))
                empty_containers.append(*element);
            return TraversalDecision::Continue;
        });
    }
    if (empty_containers.is_empty())
        return;

    // INTEROP: Paragraph integration can move every selected child out of a partial formatting ancestor. Blink and
    //          WebKit do not retain those now-unrendered wrappers after replacement completion; block containers and
    //          atomic inline content remain meaningful even without ordinary children.
    for (auto& container : empty_containers) {
        if (container->parent())
            remove_node(container);
    }
}

static void move_end_past_empty_terminal_container(InsertedContent& inserted_content)
{
    auto caret = inserted_content.endpoints().end();
    auto end = inserted_content.end_boundary();
    if (caret.node != end.node || caret.offset != end.offset || caret.offset != 0
        || !is<DOM::Element>(*caret.node) || is<HTML::HTMLLIElement>(*caret.node)
        || is_non_list_single_line_container(*caret.node) || has_visible_children(*caret.node))
        return;

    // INTEROP: Blink exposes the next visible position when replacement ends in an empty transport container. Empty
    //          paragraph containers and list items remain genuine editable paragraphs and continue to own the caret.
    auto position = VisiblePosition::create(caret.node->document(), caret).next();
    if (position.has_value())
        inserted_content.endpoints().set_end(position->deep_equivalent());
}

DOM::BoundaryPoint finalize_inserted_content(InsertedContent& inserted_content, DOM::BoundaryPoint end)
{
    inserted_content.begin_completion(end);
    // INTEROP: Blink completes structural replacement before normalizing protected clipboard spaces. In particular,
    //          text seams are coalesced while the protected spaces still retain their own observable node identities.
    merge_text_nodes_at_start_of_inserted_content(inserted_content);
    if (inserted_content.should_normalize_end_text_seam())
        merge_text_nodes_at_end_of_inserted_content(inserted_content);
    auto end_before_whitespace_normalization = inserted_content.endpoints().end();
    auto normalized_whitespace = normalize_inserted_nbsp(inserted_content);
    // INTEROP: Blink stores replacement endpoints as Positions rather than live ranges. Replacing an NBSP with an
    //          ordinary space must therefore leave the already completed endpoint unchanged.
    inserted_content.endpoints().set_end(end_before_whitespace_normalization);
    coalesce_normalized_whitespace_in_integrated_end_paragraph(inserted_content, normalized_whitespace);
    remove_empty_modifiable_containers(inserted_content);
    move_end_past_empty_terminal_container(inserted_content);

    // INTEROP: An atomic inline owns the caret boundary after it. Recompute that boundary after text seam cleanup,
    //          since removing an earlier sibling changes the parent offset while the atomic node itself stays stable.
    if (auto atomic_boundary = inserted_content.end_boundary_after_atomic_content(); atomic_boundary.has_value())
        inserted_content.endpoints().set_end(*atomic_boundary);
    return inserted_content.endpoints().end();
}

}
