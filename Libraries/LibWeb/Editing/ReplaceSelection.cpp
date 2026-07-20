/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibGC/Root.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/InlineInsertion.h>
#include <LibWeb/Editing/InsertedContent.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/ParagraphIntegration.h>
#include <LibWeb/Editing/ParagraphOperations.h>
#include <LibWeb/Editing/ParagraphTransfer.h>
#include <LibWeb/Editing/ReplaceSelection.h>
#include <LibWeb/Editing/ReplacementFragment.h>
#include <LibWeb/Editing/ReplacementRangeCleanup.h>
#include <LibWeb/Editing/ReplacementStyleCleanup.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Editing {

static bool begins_with_list_content(DOM::Node const& node)
{
    if (is<HTML::HTMLLIElement>(node) || is<HTML::HTMLUListElement>(node) || is<HTML::HTMLOListElement>(node))
        return true;
    auto* first_child = node.first_child();
    return first_child && (is<HTML::HTMLUListElement>(*first_child) || is<HTML::HTMLOListElement>(*first_child));
}

static bool can_merge_paragraph_contents(DOM::Node const& node)
{
    return is<HTML::HTMLElement>(node) && is_non_list_single_line_container(const_cast<DOM::Node&>(node));
}

static bool are_same_list_type(DOM::Node const& first, DOM::Node const& second)
{
    return (is<HTML::HTMLUListElement>(first) && is<HTML::HTMLUListElement>(second))
        || (is<HTML::HTMLOListElement>(first) && is<HTML::HTMLOListElement>(second));
}

static void integrate_first_pasted_list(InsertedContent& inserted_content, DOM::Node& destination_block)
{
    if (!is<HTML::HTMLLIElement>(destination_block) || !destination_block.parent())
        return;

    auto pasted_list = inserted_content.first_node();
    if (!are_same_list_type(*pasted_list, *destination_block.parent()) || !pasted_list->has_children())
        return;

    GC::Root<DOM::Node> first_item { *pasted_list->first_child() };
    GC::Root<DOM::Node> last_item { *pasted_list->last_child() };
    while (pasted_list->first_child())
        move_node_preserving_ranges(*pasted_list->first_child(), *pasted_list->parent(), pasted_list->index());
    remove_node(pasted_list);
    inserted_content.did_replace_node(pasted_list, *first_item, *last_item);
}

static bool is_at_end_of(DOM::BoundaryPoint boundary, DOM::Node const& ancestor)
{
    if (auto const* text = as_if<DOM::Text>(*boundary.node)) {
        if (boundary.offset != text->length_in_utf16_code_units())
            return false;
    } else {
        // INTEROP: A trailing br is a placeholder for the caret, not content after the rendered paragraph.
        for (size_t index = boundary.offset; index < boundary.node->child_count(); ++index) {
            if (!is<HTML::HTMLBRElement>(*boundary.node->child_at_index(index)))
                return false;
        }
    }

    for (auto* node = boundary.node.ptr(); node != &ancestor; node = node->parent()) {
        if (!node->parent())
            return false;
        for (auto* sibling = node->next_sibling(); sibling; sibling = sibling->next_sibling()) {
            if (!is<HTML::HTMLBRElement>(*sibling))
                return false;
        }
    }
    return true;
}

static bool has_mergeable_visible_children(DOM::Node& node)
{
    if (node.child_count() == 1 && is<HTML::HTMLBRElement>(*node.first_child()))
        return false;
    return has_visible_children(node);
}

static void restore_trailing_break(DOM::Node& block, HTML::HTMLBRElement& trailing_break)
{
    if (block.last_child() == &trailing_break)
        return;
    if (trailing_break.parent())
        move_node_preserving_ranges(trailing_break, block, block.child_count());
    else
        insert_node_before(trailing_break, block, nullptr);
}

static GC::Ptr<DOM::Text> first_text_descendant(DOM::Node& node)
{
    if (is<DOM::Text>(node))
        return static_cast<DOM::Text&>(node);
    for (auto* child = node.first_child(); child; child = child->next_sibling()) {
        if (auto text = first_text_descendant(*child))
            return text;
    }
    return {};
}

enum class BlockInsertionMode {
    SplitDestination,
    InsertAfterContainingBlock,
    InsertAtInternalParagraphBoundary,
    InsertInsideListItem,
    InsertInsideEmptyBlock,
};

// Capture the immutable facts needed to choose the initial insertion topology. Paragraph merging is intentionally not
// part of this plan: it is decided from the rendered, mutation-tracked inserted range after insertion and cleanup.
struct BlockReplacementPlan {
    bool selection_start_was_start_of_paragraph { false };
    bool selection_end_was_end_of_paragraph { false };
    bool destination_is_list_item { false };
    bool fragment_begins_with_list { false };
    BlockInsertionMode insertion_mode { BlockInsertionMode::SplitDestination };
};

static BlockReplacementPlan make_block_replacement_plan(DOM::BoundaryPoint insertion_boundary, DOM::Node& destination_block, InsertedContent const& inserted_content)
{
    BlockReplacementPlan plan;
    auto visible_insertion_position = VisiblePosition::create(destination_block.document(), insertion_boundary);
    plan.selection_start_was_start_of_paragraph = visible_insertion_position.is_start_of_paragraph();
    plan.selection_end_was_end_of_paragraph = visible_insertion_position.is_end_of_paragraph();
    auto selection_start_was_start_of_block = visible_insertion_position.is_start_of_containing_block();
    auto selection_end_was_end_of_block = visible_insertion_position.is_end_of_containing_block();
    plan.destination_is_list_item = is<HTML::HTMLLIElement>(destination_block);
    plan.fragment_begins_with_list = begins_with_list_content(*inserted_content.first_node());

    if (plan.destination_is_list_item && !plan.fragment_begins_with_list) {
        plan.insertion_mode = BlockInsertionMode::InsertInsideListItem;
    } else if (!plan.destination_is_list_item && !has_mergeable_visible_children(destination_block)) {
        plan.insertion_mode = BlockInsertionMode::InsertInsideEmptyBlock;
    } else if (plan.selection_end_was_end_of_paragraph && selection_end_was_end_of_block) {
        plan.insertion_mode = BlockInsertionMode::InsertAfterContainingBlock;
    } else if ((plan.selection_start_was_start_of_paragraph && !selection_start_was_start_of_block)
        || (plan.selection_end_was_end_of_paragraph && !selection_end_was_end_of_block)) {
        // INTEROP: Blink and WebKit keep a flow container intact when inserting at a br-delimited paragraph boundary.
        //          The fragment is inserted between its paragraphs; only an insertion inside paragraph contents
        //          requires splitting the containing block.
        plan.insertion_mode = BlockInsertionMode::InsertAtInternalParagraphBoundary;
    }
    return plan;
}

static DOM::BoundaryPoint canonicalize_inserted_content_end(DOM::BoundaryPoint boundary, ReplacementFragment const& fragment, BlockReplacementPlan const& plan)
{
    // INTEROP: When paste splits a paragraph, Blink's end interchange newline advances the visible end through an
    //          empty transport paragraph into the destination suffix. At the original paragraph end, the empty
    //          paragraph is genuine inserted content and continues to own the caret.
    if (!fragment.has_interchange_newline_at_end())
        return boundary;

    auto visible_end = VisiblePosition::create(boundary.node->document(), boundary);
    auto next = visible_end.next();
    auto end_block = block_node_of_node(*visible_end.deep_equivalent().node);
    if (end_block && is<HTML::HTMLLIElement>(*end_block) && next.has_value()) {
        auto next_block = block_node_of_node(*next->deep_equivalent().node);
        if (next_block && is<HTML::HTMLLIElement>(*next_block) && next_block != end_block
            && next_block->parent() == end_block->parent())
            return next->deep_equivalent();
    }
    if ((plan.selection_end_was_end_of_paragraph || !visible_end.is_end_of_paragraph() || !next.has_value())
        && !visible_end.is_start_of_paragraph()) {
        if (end_block && is<HTML::HTMLLIElement>(*end_block) && end_block->parent()) {
            // INTEROP: Blink materializes a terminal interchange newline after list content as a new empty item when
            //          there is no following paragraph to receive it. Clipboard serialization removes the empty
            //          transport item, so this is the phase which restores the rendered paragraph it represented.
            auto new_list_item = MUST(DOM::create_element(boundary.node->document(), HTML::TagNames::li, Namespace::HTML));
            insert_node_before(new_list_item, *end_block->parent(), end_block->next_sibling());
            return { new_list_item, 0 };
        }
    }

    auto* traversal_start = boundary.node.ptr();
    auto is_empty_placeholder = boundary.offset == 0 && is<HTML::HTMLElement>(*boundary.node)
        && !is<HTML::HTMLLIElement>(*boundary.node) && is<HTML::HTMLBRElement>(boundary.node->first_child());
    if (is_empty_placeholder && plan.selection_end_was_end_of_paragraph)
        return boundary;
    if (!is_empty_placeholder) {
        auto block = block_node_of_node(*boundary.node);
        if (!block || !is_at_end_of(boundary, *block))
            return boundary;
        traversal_start = block;
    }

    for (auto* ancestor = traversal_start; ancestor && ancestor->parent(); ancestor = ancestor->parent()) {
        if (ancestor->is_editing_host())
            break;
        for (auto* sibling = ancestor->next_sibling(); sibling; sibling = sibling->next_sibling()) {
            if (auto text = first_text_descendant(*sibling))
                return { *text, 0 };
        }
    }
    return boundary;
}

static void replace_selection_with_block_fragment(DOM::Document& document, GC::Ref<DOM::Range> range, GC::Ref<DOM::Node> destination_block, ReplacementFragment& replacement_fragment, InsertedContent& inserted_content)
{
    auto& selection = *document.get_selection();
    inserted_content.track_replacement_boundary(range->start(), InsertedContent::ReplacementTopology::Block);
    auto plan = make_block_replacement_plan(range->start(), *destination_block, inserted_content);
    GC::Root<HTML::HTMLBRElement> destination_trailing_break;
    if (auto* trailing_break = as_if<HTML::HTMLBRElement>(destination_block->last_child()))
        destination_trailing_break = *trailing_break;
    GC::Root<DOM::Node> right_block;
    if (plan.insertion_mode == BlockInsertionMode::InsertInsideListItem) {
        // INTEROP: Blink reserves its list-item insertion path for fragments which begin with list content. Ordinary
        //          block fragments remain inside the destination list item and participate in paragraph merging there.
        auto insertion_boundary = split_inline_ancestors_at_boundary(range->start(), *destination_block);
        inserted_content.insert_before(*destination_block, destination_block->child_at_index(insertion_boundary.offset));
    } else if (plan.insertion_mode == BlockInsertionMode::InsertInsideEmptyBlock) {
        // INTEROP: Blink replaces an empty paragraph's placeholder with block content inside that paragraph instead
        //          of splitting the paragraph and placing the inserted blocks beside it.
        if (destination_block->child_count() == 1 && is<HTML::HTMLBRElement>(*destination_block->first_child()))
            remove_node(*destination_block->first_child());
        else
            remove_extraneous_line_breaks_at_the_end_of_node(*destination_block);
        inserted_content.insert_before(*destination_block, nullptr);
    } else if (plan.insertion_mode == BlockInsertionMode::InsertAtInternalParagraphBoundary) {
        auto insertion_boundary = split_inline_ancestors_at_boundary(range->start(), *destination_block);
        inserted_content.insert_before(*destination_block, destination_block->child_at_index(insertion_boundary.offset));
    } else if (plan.insertion_mode == BlockInsertionMode::InsertAfterContainingBlock) {
        inserted_content.insert_before(*destination_block->parent(), destination_block->next_sibling());
    } else {
        right_block = split_containing_block_at_boundary(range->start(), *destination_block);
        inserted_content.insert_before(*destination_block->parent(), right_block.ptr());
    }
    remove_redundant_styles_from_inserted_content(inserted_content);

    // INTEROP: Blink integrates a pasted list with a destination list of the same type. Leaving the list wrapper in
    //          place would create a nested list between the two halves of the destination list item.
    integrate_first_pasted_list(inserted_content, *destination_block);
    inserted_content.begin_tracking_content_boundaries();
    auto paragraph_integration = ParagraphIntegration(document, inserted_content, {
                                                                                      .selection_start_was_start_of_paragraph = plan.selection_start_was_start_of_paragraph,
                                                                                      .selection_end_was_end_of_paragraph = plan.selection_end_was_end_of_paragraph,
                                                                                  })
                                     .decide();
    // INTEROP: A trailing br remains a list-item terminator when insertion splits an existing paragraph. At the
    //          paragraph's visible end it is only a caret placeholder, and the inserted block content supersedes it.
    if (plan.insertion_mode == BlockInsertionMode::InsertInsideListItem
        && plan.selection_end_was_end_of_paragraph && !replacement_fragment.has_interchange_newline_at_end()
        && destination_trailing_break)
        remove_node(*destination_trailing_break);
    auto first_inserted = inserted_content.first_node();
    if (!paragraph_integration.merge_start) {
        // INTEROP: Splitting a block before a fragment whose first paragraph cannot merge can leave collapsible
        //          whitespace at the end of the destination prefix. Blink protects that rendered space.
        canonicalize_whitespace_after_node_insertion(inserted_content.insertion_boundary());
    }

    if (paragraph_integration.merge_start) {
        if (plan.insertion_mode == BlockInsertionMode::InsertAfterContainingBlock && destination_trailing_break)
            remove_node(*destination_trailing_break);
        auto merge_offset = static_cast<WebIDL::UnsignedLong>(destination_block->child_count());
        if (has_visible_children(*destination_block))
            remove_extraneous_line_breaks_at_the_end_of_node(first_inserted);
        auto destination = plan.insertion_mode == BlockInsertionMode::InsertInsideListItem
                || plan.insertion_mode == BlockInsertionMode::InsertAtInternalParagraphBoundary
            ? DOM::BoundaryPoint { *destination_block, static_cast<WebIDL::UnsignedLong>(first_inserted->index()) }
            : DOM::BoundaryPoint { *destination_block, merge_offset };
        auto transfer_options = ParagraphTransferOptions {
            .source_style = is<HTML::HTMLLIElement>(*destination_block)
                ? ParagraphTransferStyle::MatchDestination
                : ParagraphTransferStyle::PreserveVisualAppearance,
        };
        auto transferred_paragraph = transfer_paragraph_contents(first_inserted, destination, transfer_options);
        canonicalize_whitespace_after_node_insertion({ *destination_block, merge_offset });
        inserted_content.did_replace_node(first_inserted, *destination_block);
        inserted_content.did_move_start_paragraph(transferred_paragraph.start);
        if (plan.insertion_mode == BlockInsertionMode::InsertAfterContainingBlock && destination_trailing_break)
            restore_trailing_break(*destination_block, *destination_trailing_break);
    } else if (!has_visible_children(*destination_block)) {
        remove_node(*destination_block);
    }

    auto last_inserted = inserted_content.last_node();
    auto can_merge_end_with_destination_suffix = plan.insertion_mode == BlockInsertionMode::SplitDestination
        || plan.insertion_mode == BlockInsertionMode::InsertAtInternalParagraphBoundary;
    if (plan.insertion_mode == BlockInsertionMode::InsertInsideListItem && paragraph_integration.merge_end
        && last_inserted->parent() && can_merge_paragraph_contents(*last_inserted) && has_mergeable_visible_children(*last_inserted)) {
        if (has_visible_children(*destination_block))
            remove_extraneous_line_breaks_at_the_end_of_node(last_inserted);
        auto caret = inserted_content.end_boundary();
        inserted_content.will_integrate_end_paragraph(*last_inserted);
        auto transferred_paragraph = transfer_paragraph_contents(last_inserted,
            { *destination_block, static_cast<WebIDL::UnsignedLong>(last_inserted->index()) });
        inserted_content.did_replace_node(last_inserted, *destination_block);
        caret = finalize_inserted_content(inserted_content, transferred_paragraph.end);
        if (destination_trailing_break)
            restore_trailing_break(*destination_block, *destination_trailing_break);
        MUST(selection.collapse(caret.node, caret.offset));
    } else if (can_merge_end_with_destination_suffix && paragraph_integration.merge_end
        && last_inserted->parent() && can_merge_paragraph_contents(*last_inserted) && has_mergeable_visible_children(*last_inserted)) {
        // INTEROP: Blink also coalesces the end seam when list-leading content ends in a plain single-text paragraph.
        //          More complex final paragraphs retain their source and destination text-node boundary.
        auto merge_end_text_seam = plan.fragment_begins_with_list
            && last_inserted->child_count() == 1 && is<DOM::Text>(*last_inserted->first_child());
        if (merge_end_text_seam)
            inserted_content.require_end_text_seam_normalization();
        auto caret = inserted_content.end_boundary();
        auto merge_offset = static_cast<WebIDL::UnsignedLong>(last_inserted->child_count());
        Optional<DOM::BoundaryPoint> whitespace_boundary;
        inserted_content.will_integrate_end_paragraph(*last_inserted);
        if (plan.insertion_mode == BlockInsertionMode::InsertAtInternalParagraphBoundary) {
            auto insertion_offset = last_inserted->index();
            auto transfer_options = ParagraphTransferOptions {
                .source_style = ParagraphTransferStyle::PreserveVisualAppearance,
            };
            auto transferred_paragraph = transfer_paragraph_contents(last_inserted,
                { *destination_block, static_cast<WebIDL::UnsignedLong>(insertion_offset) }, transfer_options);
            inserted_content.did_replace_node(last_inserted, *transferred_paragraph.first_node, *transferred_paragraph.last_node);
            caret = transferred_paragraph.end;
            whitespace_boundary = DOM::BoundaryPoint { *destination_block, static_cast<WebIDL::UnsignedLong>(insertion_offset + merge_offset) };
        } else {
            auto transfer_seam = last_inserted->child_count() == 1 && is<DOM::Text>(*last_inserted->first_child())
                ? ParagraphTransferSeam::CoalesceDestinationText
                : ParagraphTransferSeam::Preserve;
            auto transfer_options = ParagraphTransferOptions {
                .destination_style = ParagraphTransferStyle::PreserveVisualAppearance,
                .seam = transfer_seam,
            };
            auto transferred_paragraph = transfer_paragraph_contents(*right_block, { last_inserted, merge_offset }, transfer_options);
            if (transferred_paragraph.destination_text_seam.has_value()) {
                caret = *transferred_paragraph.destination_text_seam;
                whitespace_boundary = transferred_paragraph.destination_text_seam;
            }
        }
        if (whitespace_boundary.has_value())
            canonicalize_whitespace_after_node_insertion(*whitespace_boundary);
        else
            canonicalize_whitespace_after_node_insertion({ last_inserted, merge_offset });
        caret = finalize_inserted_content(inserted_content, caret);
        caret = canonicalize_inserted_content_end(caret, replacement_fragment, plan);
        MUST(selection.collapse(caret.node, caret.offset));
    } else {
        auto caret = inserted_content.end_boundary();
        if (plan.insertion_mode != BlockInsertionMode::SplitDestination) {
            // The insertion remained inside its destination block, so there is no separate right block to merge.
        } else if (!has_visible_children(*right_block)) {
            remove_node(*right_block);
        } else if (auto leading_text = first_text_descendant(*right_block); leading_text && !leading_text->data().is_empty()
            && is_ascii_space(leading_text->data().code_unit_at(0))) {
            // INTEROP: Chromium protects an ordinary leading space in place. If the split leaves a whitespace-only
            //          text node before inline content, it instead adds a protected space and retains that node.
            auto contains_only_whitespace = all_of(leading_text->data().utf16_view(), is_ascii_space);
            if (contains_only_whitespace)
                MUST(insert_data(*leading_text, 0, "\u00a0"_utf16));
            else
                MUST(replace_data(*leading_text, 0, 1, "\u00a0"_utf16));
        } else {
            canonicalize_whitespace_after_node_insertion({ *right_block, 0 });
        }
        caret = finalize_inserted_content(inserted_content, caret);
        caret = canonicalize_inserted_content_end(caret, replacement_fragment, plan);
        MUST(selection.collapse(caret.node, caret.offset));
    }
}

static void replace_selection_with_inline_fragment(DOM::Document& document, GC::Ref<DOM::Range> range, ReplacementFragment& replacement_fragment, InsertedContent& inserted_content)
{
    auto& selection = *document.get_selection();
    inserted_content.track_replacement_boundary(range->start(), InsertedContent::ReplacementTopology::Inline);

    GC::RootVector<GC::Ref<DOM::Node>> descendants;
    replacement_fragment.fragment().for_each_in_subtree([&](GC::Ref<DOM::Node> descendant) {
        descendants.append(descendant);
        return TraversalDecision::Continue;
    });

    if (is_block_node(range->start_container())) {
        GC::RootVector<GC::Ref<DOM::Node>> collapsed_block_props;
        range->start_container()->for_each_child([&](GC::Ref<DOM::Node> child) {
            if (child->is_editable() && is_collapsed_block_prop(child) && child->index() >= range->start_offset())
                collapsed_block_props.append(child);
            return IterationDecision::Continue;
        });
        for (auto& node : collapsed_block_props)
            remove_node(node);
    }

    auto containing_block = block_node_of_node(*range->start_container());
    VERIFY(containing_block);
    auto insertion = prepare_inline_insertion_boundary(range->start(), *containing_block);
    MUST(range->set_start(insertion.boundary.node, insertion.boundary.offset));
    range->collapse(true);

    inserted_content.insert_into_range(*range, replacement_fragment.fragment());
    remove_redundant_styles_from_inserted_content(inserted_content);
    inserted_content.begin_tracking_content_boundaries();

    auto first_inserted = inserted_content.first_node();
    if (insertion.split_destination_style)
        canonicalize_whitespace_after_node_insertion({ first_inserted, 0 });

    // INTEROP: Clipboard serialization has already protected whitespace adjacent to atomic inline content. Chromium
    //          only rebalances the outer replacement boundary here; re-canonicalizing each inserted atomic node can
    //          turn the protected space after it back into collapsible whitespace.
    if (!is<DOM::Text>(*first_inserted))
        canonicalize_whitespace_after_node_insertion({ first_inserted, 0 });
    canonicalize_whitespace_after_node_insertion(inserted_content.end_boundary());

    auto caret = inserted_content.end_boundary();
    if (auto* last_text = as_if<DOM::Text>(*inserted_content.last_node()); last_text
        && !replacement_fragment.has_interchange_newline_at_start()) {
        auto ends_in_protected_whitespace = all_of(last_text->data().utf16_view(), [](u16 code_unit) {
            return is_ascii_space(code_unit) || code_unit == 0xa0;
        });
        if (!ends_in_protected_whitespace || replacement_fragment.contains_only_text())
            inserted_content.require_end_text_seam_normalization();
    }
    caret = finalize_inserted_content(inserted_content, caret);

    auto current_range = active_range(document);
    if (is_block_node(current_range->start_container()) && !has_visible_children(current_range->start_container())) {
        auto line_break = MUST(DOM::create_element(document, HTML::TagNames::br, Namespace::HTML));
        MUST(append_node(line_break, *current_range->start_container()));
    }

    MUST(selection.collapse(caret.node, caret.offset));
    for (auto& descendant : descendants)
        fix_disallowed_ancestors_of_node(descendant);
}

void replace_selection_with_fragment(DOM::Document& document, TrustedTypes::TrustedHTMLOrString const& value)
{
    auto range = active_range(document);
    VERIFY(range && range->collapsed());

    ReplacementFragment replacement_fragment { *range, value };
    InsertedContent inserted_content { replacement_fragment.children() };
    if (inserted_content.is_empty())
        return;

    auto destination_block = block_node_of_node(*range->start_container());
    if (replacement_fragment.contains_block_content() && destination_block
        && destination_block != range->start_container()->editing_host()) {
        replace_selection_with_block_fragment(document, *range, *destination_block, replacement_fragment, inserted_content);
        return;
    }

    replace_selection_with_inline_fragment(document, *range, replacement_fragment, inserted_content);
}

}
