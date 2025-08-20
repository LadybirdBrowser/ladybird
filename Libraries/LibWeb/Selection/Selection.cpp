/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SelectionPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/GraphemeEdgeTracker.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Selection {

GC_DEFINE_ALLOCATOR(Selection);

GC::Ref<Selection> Selection::create(GC::Ref<JS::Realm> realm, GC::Ref<DOM::Document> document)
{
    return realm->create<Selection>(realm, document);
}

Selection::Selection(GC::Ref<JS::Realm> realm, GC::Ref<DOM::Document> document)
    : PlatformObject(realm)
    , m_document(document)
{
}

Selection::~Selection() = default;

void Selection::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Selection);
    Base::initialize(realm);
}

// https://w3c.github.io/selection-api/#dfn-empty
bool Selection::is_empty() const
{
    // Each selection can be associated with a single range.
    // When there is no range associated with the selection, the selection is empty.
    // The selection must be initially empty.

    // NOTE: This function should not be confused with Selection.empty() which empties the selection.
    return !m_range;
}

void Selection::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_range);
    visitor.visit(m_document);
}

// https://w3c.github.io/selection-api/#dfn-anchor
GC::Ptr<DOM::Node> Selection::anchor_node()
{
    if (!m_range)
        return nullptr;
    if (m_direction == Direction::Forwards)
        return m_range->start_container();
    return m_range->end_container();
}

// https://w3c.github.io/selection-api/#dfn-anchor
unsigned Selection::anchor_offset()
{
    if (!m_range)
        return 0;
    if (m_direction == Direction::Forwards)
        return m_range->start_offset();
    return m_range->end_offset();
}

// https://w3c.github.io/selection-api/#dfn-focus
GC::Ptr<DOM::Node> Selection::focus_node()
{
    if (!m_range)
        return nullptr;
    if (m_direction == Direction::Forwards)
        return m_range->end_container();
    return m_range->start_container();
}

// https://w3c.github.io/selection-api/#dfn-focus
unsigned Selection::focus_offset() const
{
    if (!m_range)
        return 0;
    if (m_direction == Direction::Forwards)
        return m_range->end_offset();
    return m_range->start_offset();
}

// https://w3c.github.io/selection-api/#dom-selection-iscollapsed
bool Selection::is_collapsed() const
{
    // The attribute must return true if and only if the anchor and focus are the same
    // (including if both are null). Otherwise it must return false.
    if (!m_range)
        return true;
    return const_cast<Selection*>(this)->anchor_node() == const_cast<Selection*>(this)->focus_node()
        && m_range->start_offset() == m_range->end_offset();
}

// https://w3c.github.io/selection-api/#dom-selection-rangecount
unsigned Selection::range_count() const
{
    if (m_range)
        return 1;
    return 0;
}

String Selection::type() const
{
    if (!m_range)
        return "None"_string;
    if (m_range->collapsed())
        return "Caret"_string;
    return "Range"_string;
}

String Selection::direction() const
{
    if (!m_range || m_direction == Direction::Directionless)
        return "none"_string;
    if (m_direction == Direction::Forwards)
        return "forward"_string;
    return "backward"_string;
}

// https://w3c.github.io/selection-api/#dom-selection-getrangeat
WebIDL::ExceptionOr<GC::Ptr<DOM::Range>> Selection::get_range_at(unsigned index)
{
    GC::Ptr<DOM::Node> focus = focus_node();
    GC::Ptr<DOM::Node> anchor = anchor_node();

    // The method must throw an IndexSizeError exception if index is not 0, or if this is empty or either focus or anchor is not in the document tree.
    auto is_focus_in_document_tree = focus && &focus->document() == document();
    auto is_anchor_in_document_tree = anchor && &anchor->document() == document();

    if (index != 0 || is_empty() || !is_focus_in_document_tree || !is_anchor_in_document_tree)
        return WebIDL::IndexSizeError::create(realm(), "Selection.getRangeAt() on empty Selection or with invalid argument"_utf16);

    // Otherwise, it must return a reference to (not a copy of) this's range.
    return m_range;
}

// https://w3c.github.io/selection-api/#dom-selection-addrange
void Selection::add_range(GC::Ref<DOM::Range> range)
{
    // 1. If the root of the range's boundary points are not the document associated with this, abort these steps.
    if (&range->start_container()->root() != m_document.ptr())
        return;

    // 2. If rangeCount is not 0, abort these steps.
    if (range_count() != 0)
        return;

    // 3. Set this's range to range by a strong reference (not by making a copy).
    set_range(range);

    // AD-HOC: WPT selection/removeAllRanges.html and selection/addRange.htm expect this
    m_direction = Direction::Forwards;
}

// https://w3c.github.io/selection-api/#dom-selection-removerange
WebIDL::ExceptionOr<void> Selection::remove_range(GC::Ref<DOM::Range> range)
{
    // The method must make this empty by disassociating its range if this's range is range.
    if (m_range == range) {
        set_range(nullptr);
        return {};
    }

    // Otherwise, it must throw a NotFoundError.
    return WebIDL::NotFoundError::create(realm(), "Selection.removeRange() with invalid argument"_utf16);
}

// https://w3c.github.io/selection-api/#dom-selection-removeallranges
void Selection::remove_all_ranges()
{
    // The method must make this empty by disassociating its range if this has an associated range.
    set_range(nullptr);
}

// https://w3c.github.io/selection-api/#dom-selection-empty
void Selection::empty()
{
    // The method must be an alias, and behave identically, to removeAllRanges().
    remove_all_ranges();
}

// https://w3c.github.io/selection-api/#dom-selection-collapse
WebIDL::ExceptionOr<void> Selection::collapse(GC::Ptr<DOM::Node> node, unsigned offset)
{
    // 1. If node is null, this method must behave identically as removeAllRanges() and abort these steps.
    if (!node) {
        remove_all_ranges();
        return {};
    }

    // 2. If node is a DocumentType, throw an InvalidNodeTypeError exception and abort these steps.
    if (node->is_document_type())
        return WebIDL::InvalidNodeTypeError::create(realm(), "Selection.collapse() with DocumentType node"_utf16);

    // 3. The method must throw an IndexSizeError exception if offset is longer than node's length and abort these steps.
    if (offset > node->length())
        return WebIDL::IndexSizeError::create(realm(), "Selection.collapse() with offset longer than node's length"_utf16);

    // 4. If document associated with this is not a shadow-including inclusive ancestor of node, abort these steps.
    if (!m_document->is_shadow_including_inclusive_ancestor_of(*node))
        return {};

    // 5. Otherwise, let newRange be a new range.
    auto new_range = DOM::Range::create(*m_document);

    // 6. Set the start the start and the end of newRange to (node, offset).
    TRY(new_range->set_start(*node, offset));

    // 7. Set this's range to newRange.
    set_range(new_range);

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-setposition
WebIDL::ExceptionOr<void> Selection::set_position(GC::Ptr<DOM::Node> node, unsigned offset)
{
    // The method must be an alias, and behave identically, to collapse().
    return collapse(node, offset);
}

// https://w3c.github.io/selection-api/#dom-selection-collapsetostart
WebIDL::ExceptionOr<void> Selection::collapse_to_start()
{
    // 1. The method must throw InvalidStateError exception if the this is empty.
    if (!m_range) {
        return WebIDL::InvalidStateError::create(realm(), "Selection.collapse_to_start() on empty range"_utf16);
    }

    // 2. Otherwise, it must create a new range
    auto new_range = DOM::Range::create(*m_document);

    // 3. Set the start both its start and end to the start of this's range
    TRY(new_range->set_start(*m_range->start_container(), m_range->start_offset()));
    TRY(new_range->set_end(*m_range->start_container(), m_range->start_offset()));

    // 4. Then set this's range to the newly-created range.
    set_range(new_range);
    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-collapsetoend
WebIDL::ExceptionOr<void> Selection::collapse_to_end()
{
    // 1. The method must throw InvalidStateError exception if the this is empty.
    if (!m_range) {
        return WebIDL::InvalidStateError::create(realm(), "Selection.collapse_to_end() on empty range"_utf16);
    }

    // 2. Otherwise, it must create a new range
    auto new_range = DOM::Range::create(*m_document);

    // 3. Set the start both its start and end to the start of this's range
    TRY(new_range->set_start(*m_range->end_container(), m_range->end_offset()));
    TRY(new_range->set_end(*m_range->end_container(), m_range->end_offset()));

    // 4. Then set this's range to the newly-created range.
    set_range(new_range);

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-extend
WebIDL::ExceptionOr<void> Selection::extend(GC::Ref<DOM::Node> node, unsigned offset)
{
    // 1. If the document associated with this is not a shadow-including inclusive ancestor of node, abort these steps.
    if (!m_document->is_shadow_including_inclusive_ancestor_of(node))
        return {};

    // 2. If this is empty, throw an InvalidStateError exception and abort these steps.
    if (!m_range) {
        return WebIDL::InvalidStateError::create(realm(), "Selection.extend() on empty range"_utf16);
    }

    // 3. Let oldAnchor and oldFocus be the this's anchor and focus, and let newFocus be the boundary point (node, offset).
    auto& old_anchor_node = *anchor_node();
    auto old_anchor_offset = anchor_offset();

    auto& new_focus_node = node;
    auto new_focus_offset = offset;

    // 4. Let newRange be a new range.
    auto new_range = DOM::Range::create(*m_document);

    // 5. If node's root is not the same as the this's range's root, set the start newRange's start and end to newFocus.
    if (&node->root() != &m_range->start_container()->root()) {
        TRY(new_range->set_start(new_focus_node, new_focus_offset));
        TRY(new_range->set_end(new_focus_node, new_focus_offset));
    }
    // 6. Otherwise, if oldAnchor is before or equal to newFocus, set the start newRange's start to oldAnchor, then set its end to newFocus.
    else if (DOM::position_of_boundary_point_relative_to_other_boundary_point({ old_anchor_node, old_anchor_offset }, { new_focus_node, new_focus_offset }) != DOM::RelativeBoundaryPointPosition::After) {
        TRY(new_range->set_start(old_anchor_node, old_anchor_offset));
        TRY(new_range->set_end(new_focus_node, new_focus_offset));
    }
    // 7. Otherwise, set the start newRange's start to newFocus, then set its end to oldAnchor.
    else {
        TRY(new_range->set_start(new_focus_node, new_focus_offset));
        TRY(new_range->set_end(old_anchor_node, old_anchor_offset));
    }

    // 8. Set this's range to newRange.
    set_range(new_range);

    // 9. If newFocus is before oldAnchor, set this's direction to backwards. Otherwise, set it to forwards.
    if (DOM::position_of_boundary_point_relative_to_other_boundary_point({ new_focus_node, new_focus_offset }, { old_anchor_node, old_anchor_offset }) == DOM::RelativeBoundaryPointPosition::Before) {
        m_direction = Direction::Backwards;
    } else {
        m_direction = Direction::Forwards;
    }

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-setbaseandextent
WebIDL::ExceptionOr<void> Selection::set_base_and_extent(GC::Ref<DOM::Node> anchor_node, unsigned anchor_offset, GC::Ref<DOM::Node> focus_node, unsigned focus_offset)
{
    // 1. If anchorOffset is longer than anchorNode's length or if focusOffset is longer than focusNode's length, throw an IndexSizeError exception and abort these steps.
    if (anchor_offset > anchor_node->length())
        return WebIDL::IndexSizeError::create(realm(), "Anchor offset points outside of the anchor node"_utf16);

    if (focus_offset > focus_node->length())
        return WebIDL::IndexSizeError::create(realm(), "Focus offset points outside of the focus node"_utf16);

    // 2. If document associated with this is not a shadow-including inclusive ancestor of anchorNode or focusNode, abort these steps.
    if (!m_document->is_shadow_including_inclusive_ancestor_of(anchor_node) || !m_document->is_shadow_including_inclusive_ancestor_of(focus_node))
        return {};

    // 3. Let anchor be the boundary point (anchorNode, anchorOffset) and let focus be the boundary point (focusNode, focusOffset).

    // 4. Let newRange be a new range.
    auto new_range = DOM::Range::create(*m_document);

    // 5. If anchor is before focus, set the start the newRange's start to anchor and its end to focus. Otherwise, set the start them to focus and anchor respectively.
    auto position_of_anchor_relative_to_focus = DOM::position_of_boundary_point_relative_to_other_boundary_point({ anchor_node, anchor_offset }, { focus_node, focus_offset });
    if (position_of_anchor_relative_to_focus == DOM::RelativeBoundaryPointPosition::Before) {
        TRY(new_range->set_start(anchor_node, anchor_offset));
        TRY(new_range->set_end(focus_node, focus_offset));
    } else {
        TRY(new_range->set_start(focus_node, focus_offset));
        TRY(new_range->set_end(anchor_node, anchor_offset));
    }

    // 6. Set this's range to newRange.
    set_range(new_range);

    // 7. If focus is before anchor, set this's direction to backwards. Otherwise, set it to forwards
    // NOTE: "Otherwise" can be seen as "focus is equal to or after anchor".
    if (position_of_anchor_relative_to_focus == DOM::RelativeBoundaryPointPosition::After)
        m_direction = Direction::Backwards;
    else
        m_direction = Direction::Forwards;

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-selectallchildren
WebIDL::ExceptionOr<void> Selection::select_all_children(GC::Ref<DOM::Node> node)
{
    // 1. If node is a DocumentType, throw an InvalidNodeTypeError exception and abort these steps.
    if (node->is_document_type())
        return WebIDL::InvalidNodeTypeError::create(realm(), "Selection.selectAllChildren() with DocumentType node"_utf16);

    // 2. If node's root is not the document associated with this, abort these steps.
    if (&node->root() != m_document.ptr())
        return {};

    // 3. Let newRange be a new range and childCount be the number of children of node.
    auto new_range = DOM::Range::create(*m_document);
    auto child_count = node->child_count();

    // 4. Set newRange's start to (node, 0).
    TRY(new_range->set_start(node, 0));

    // 5. Set newRange's end to (node, childCount).
    TRY(new_range->set_end(node, child_count));

    // 6. Set this's range to newRange.
    set_range(new_range);

    // 7. Set this's direction to forwards.
    m_direction = Direction::Forwards;

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-modify
WebIDL::ExceptionOr<void> Selection::modify(Optional<String> alter, Optional<String> direction, Optional<String> granularity)
{
    auto anchor_node = this->anchor_node();
    if (!anchor_node || !is<DOM::Text>(*anchor_node))
        return {};

    auto& text_node = static_cast<DOM::Text&>(*anchor_node);

    // 1. If alter is not ASCII case-insensitive match with "extend" or "move", abort these steps.
    if (!alter.has_value() || !alter.value().bytes_as_string_view().is_one_of_ignoring_ascii_case("extend"sv, "move"sv))
        return {};

    // 2. If direction is not ASCII case-insensitive match with "forward", "backward", "left", or "right", abort these steps.
    if (!direction.has_value() || !direction.value().bytes_as_string_view().is_one_of_ignoring_ascii_case("forward"sv, "backward"sv, "left"sv, "right"sv))
        return {};

    // 3. If granularity is not ASCII case-insensitive match with "character", "word", "sentence", "line", "paragraph",
    //    "lineboundary", "sentenceboundary", "paragraphboundary", "documentboundary", abort these steps.
    if (!granularity.has_value() || !granularity.value().bytes_as_string_view().is_one_of_ignoring_ascii_case("character"sv, "word"sv, "sentence"sv, "line"sv, "paragraph"sv, "lineboundary"sv, "sentenceboundary"sv, "paragraphboundary"sv, "documentboundary"sv))
        return {};

    // 4. If this selection is empty, abort these steps.
    if (is_empty())
        return {};

    // 5. Let effectiveDirection be backwards.
    auto effective_direction = Direction::Backwards;

    // 6. If direction is ASCII case-insensitive match with "forward", set effectiveDirection to forwards.
    if (direction.value().equals_ignoring_ascii_case("forward"sv))
        effective_direction = Direction::Forwards;

    // 7. If direction is ASCII case-insensitive match with "right" and inline base direction of this selection's focus is ltr, set effectiveDirection to forwards.
    if (direction.value().equals_ignoring_ascii_case("right"sv) && text_node.directionality() == DOM::Element::Directionality::Ltr)
        effective_direction = Direction::Forwards;

    // 8. If direction is ASCII case-insensitive match with "left" and inline base direction of this selection's focus is rtl, set effectiveDirection to forwards.
    if (direction.value().equals_ignoring_ascii_case("left"sv) && text_node.directionality() == DOM::Element::Directionality::Rtl)
        effective_direction = Direction::Forwards;

    // 9. Set this selection's direction to effectiveDirection.
    // NOTE: This is handled by calls to move_offset_to_* later on

    // 10. If alter is ASCII case-insensitive match with "extend", set this selection's focus to the location as if the user had requested to extend selection by granularity.
    // 11. Otherwise, set this selection's focus and anchor to the location as if the user had requested to move selection by granularity.
    auto collapse_selection = alter.value().equals_ignoring_ascii_case("move"sv);

    // TODO: Implement the other granularity options.
    if (effective_direction == Direction::Forwards) {
        if (granularity.value().equals_ignoring_ascii_case("character"sv))
            move_offset_to_next_character(collapse_selection);
        if (granularity.value().equals_ignoring_ascii_case("word"sv))
            move_offset_to_next_word(collapse_selection);
    } else {
        if (granularity.value().equals_ignoring_ascii_case("character"sv))
            move_offset_to_previous_character(collapse_selection);
        if (granularity.value().equals_ignoring_ascii_case("word"sv))
            move_offset_to_previous_word(collapse_selection);
    }

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-deletefromdocument
WebIDL::ExceptionOr<void> Selection::delete_from_document()
{
    // The method must invoke deleteContents() on this's range if this is not empty.
    // Otherwise the method must do nothing.
    if (!is_empty())
        return m_range->delete_contents();
    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-containsnode
bool Selection::contains_node(GC::Ref<DOM::Node> node, bool allow_partial_containment) const
{
    // The method must return false if this is empty or if node's root is not the document associated with this.
    if (!m_range)
        return false;
    if (&node->root() != m_document.ptr())
        return false;

    // Otherwise, if allowPartialContainment is false, the method must return true if and only if
    // start of its range is before or visually equivalent to the first boundary point in the node
    // and end of its range is after or visually equivalent to the last boundary point in the node.
    if (!allow_partial_containment) {
        auto start_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(m_range->start(), { node, 0 });
        auto end_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(m_range->end(), { node, static_cast<WebIDL::UnsignedLong>(node->length()) });

        return (start_relative_position == DOM::RelativeBoundaryPointPosition::Before || start_relative_position == DOM::RelativeBoundaryPointPosition::Equal)
            && (end_relative_position == DOM::RelativeBoundaryPointPosition::Equal || end_relative_position == DOM::RelativeBoundaryPointPosition::After);
    }

    // If allowPartialContainment is true, the method must return true if and only if
    // start of its range is before or visually equivalent to the last boundary point in the node
    // and end of its range is after or visually equivalent to the first boundary point in the node.

    auto start_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(m_range->start(), { node, static_cast<WebIDL::UnsignedLong>(node->length()) });
    auto end_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(m_range->end(), { node, 0 });

    return (start_relative_position == DOM::RelativeBoundaryPointPosition::Before || start_relative_position == DOM::RelativeBoundaryPointPosition::Equal)
        && (end_relative_position == DOM::RelativeBoundaryPointPosition::Equal || end_relative_position == DOM::RelativeBoundaryPointPosition::After);
}

Utf16String Selection::to_string() const
{
    // FIXME: This needs more work to be compatible with other engines.
    //        See https://www.w3.org/Bugs/Public/show_bug.cgi?id=10583
    if (!m_range)
        return {};
    return m_range->to_string();
}

GC::Ref<DOM::Document> Selection::document() const
{
    return m_document;
}

GC::Ptr<DOM::Range> Selection::range() const
{
    return m_range;
}

void Selection::set_range(GC::Ptr<DOM::Range> range)
{
    auto old_range = m_range;
    if (old_range == range)
        return;

    if (old_range)
        old_range->set_associated_selection({}, nullptr);

    m_range = range;

    if (range)
        range->set_associated_selection({}, this);

    // https://w3c.github.io/editing/docs/execCommand/#state-override
    // Whenever the number of ranges in the selection changes to something different, and whenever a boundary point of
    // the range at a given index in the selection changes to something different, the state override and value override
    // must be unset for every command.
    if (((old_range == nullptr) != (range == nullptr)) || (old_range && *old_range != *range)) {
        m_document->reset_command_state_overrides();
        m_document->reset_command_value_overrides();
    }

    // https://developer.mozilla.org/en-US/docs/Web/API/Selection#behavior_of_selection_api_in_terms_of_editing_host_focus_changes
    // AD-HOC: Focus editing host if the previous selection was outside of it. There seems to be no spec for this.
    if (range && range->start_container()->is_editable_or_editing_host()) {
        GC::Ref new_editing_host = *range->start_container()->editing_host();
        if (document()->focused_area() != new_editing_host) {
            // FIXME: Determine and propagate the right focus trigger.
            HTML::run_focusing_steps(new_editing_host, nullptr, HTML::FocusTrigger::Other);
        }
    }
}

GC::Ptr<DOM::Position> Selection::cursor_position() const
{
    if (!m_range || !is_collapsed())
        return nullptr;

    return DOM::Position::create(m_document->realm(), *m_range->start_container(), m_range->start_offset());
}

// FIXME: The offset adjustment algorithms below do not handle moving over multiple DOM nodes. For example, if we have:
//        `<div contenteditable><p>Well hello</p><p>friends</p></div>`, we should be able to move the cursor between the
//        two <p> elements. But the algorithms below limit us to a single DOM node.

void Selection::move_offset_to_next_character(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(anchor_node().ptr());
    if (!text_node)
        return;

    if (auto offset = text_node->grapheme_segmenter().next_boundary(focus_offset()); offset.has_value()) {
        if (collapse_selection) {
            MUST(collapse(text_node, *offset));
            m_document->reset_cursor_blink_cycle();
        } else {
            MUST(set_base_and_extent(*text_node, anchor_offset(), *text_node, *offset));
        }
    }
}

void Selection::move_offset_to_previous_character(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(anchor_node().ptr());
    if (!text_node)
        return;

    if (auto offset = text_node->grapheme_segmenter().previous_boundary(focus_offset()); offset.has_value()) {
        if (collapse_selection) {
            MUST(collapse(text_node, *offset));
            m_document->reset_cursor_blink_cycle();
        } else {
            MUST(set_base_and_extent(*text_node, anchor_offset(), *text_node, *offset));
        }
    }
}

void Selection::move_offset_to_next_word(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(anchor_node().ptr());
    if (!text_node)
        return;

    while (true) {
        auto focus_offset = this->focus_offset();
        if (focus_offset == text_node->data().length_in_code_units())
            return;

        if (auto offset = text_node->word_segmenter().next_boundary(focus_offset); offset.has_value()) {
            auto word = text_node->data().substring_view(focus_offset, *offset - focus_offset);
            if (collapse_selection) {
                MUST(collapse(text_node, *offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(set_base_and_extent(*text_node, anchor_offset(), *text_node, *offset));
            }
            if (Unicode::Segmenter::should_continue_beyond_word(word))
                continue;
        }
        break;
    }
}

void Selection::move_offset_to_previous_word(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(anchor_node().ptr());
    if (!text_node)
        return;

    while (true) {
        auto focus_offset = this->focus_offset();
        if (auto offset = text_node->word_segmenter().previous_boundary(focus_offset); offset.has_value()) {
            auto word = text_node->data().substring_view(*offset, focus_offset - *offset);
            if (collapse_selection) {
                MUST(collapse(text_node, *offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(set_base_and_extent(*text_node, anchor_offset(), *text_node, *offset));
            }
            if (Unicode::Segmenter::should_continue_beyond_word(word))
                continue;
        }
        break;
    }
}

void Selection::move_offset_to_next_line(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(anchor_node().ptr());
    if (!text_node)
        return;

    auto new_offset = compute_cursor_position_on_next_line(*text_node, focus_offset());
    if (!new_offset.has_value())
        return;

    if (collapse_selection) {
        MUST(collapse(text_node, *new_offset));
        m_document->reset_cursor_blink_cycle();
    } else {
        MUST(set_base_and_extent(*text_node, anchor_offset(), *text_node, *new_offset));
    }
}

void Selection::move_offset_to_previous_line(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(anchor_node().ptr());
    if (!text_node)
        return;

    auto new_offset = compute_cursor_position_on_previous_line(*text_node, focus_offset());
    if (!new_offset.has_value())
        return;

    if (collapse_selection) {
        MUST(collapse(text_node, *new_offset));
        m_document->reset_cursor_blink_cycle();
    } else {
        MUST(set_base_and_extent(*text_node, anchor_offset(), *text_node, *new_offset));
    }
}

}
