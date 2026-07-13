/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Selection.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/VisualLines.h>

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

Utf16String Selection::type() const
{
    if (!m_range)
        return "None"_utf16;
    if (m_range->collapsed())
        return "Caret"_utf16;
    return "Range"_utf16;
}

Utf16String Selection::direction() const
{
    if (!m_range || m_direction == Direction::Directionless)
        return "none"_utf16;
    if (m_direction == Direction::Forwards)
        return "forward"_utf16;
    return "backward"_utf16;
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

    auto old_anchor_and_new_focus_have_the_same_shadow_including_root = &old_anchor_node.shadow_including_root() == &new_focus_node->shadow_including_root();

    // 5. If node's root is not the same as the this's range's root, set the start newRange's start and end to newFocus.
    if (&node->root() != &m_range->start_container()->root() || !old_anchor_and_new_focus_have_the_same_shadow_including_root) {
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
    if (old_anchor_and_new_focus_have_the_same_shadow_including_root && DOM::position_of_boundary_point_relative_to_other_boundary_point({ new_focus_node, new_focus_offset }, { old_anchor_node, old_anchor_offset }) == DOM::RelativeBoundaryPointPosition::Before) {
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
WebIDL::ExceptionOr<void> Selection::modify(Optional<Utf16String> alter, Optional<Utf16String> direction, Optional<Utf16String> granularity)
{
    // 1. If alter is not ASCII case-insensitive match with "extend" or "move", abort these steps.
    if (!alter.has_value() || !alter->utf16_view().is_one_of_ignoring_ascii_case(u"extend"sv, u"move"sv))
        return {};

    // 2. If direction is not ASCII case-insensitive match with "forward", "backward", "left", or "right", abort these steps.
    if (!direction.has_value() || !direction->utf16_view().is_one_of_ignoring_ascii_case(u"forward"sv, u"backward"sv, u"left"sv, u"right"sv))
        return {};

    // 3. If granularity is not ASCII case-insensitive match with "character", "word", "sentence", "line", "paragraph",
    //    "lineboundary", "sentenceboundary", "paragraphboundary", "documentboundary", abort these steps.
    if (!granularity.has_value() || !granularity->utf16_view().is_one_of_ignoring_ascii_case(u"character"sv, u"word"sv, u"sentence"sv, u"line"sv, u"paragraph"sv, u"lineboundary"sv, u"sentenceboundary"sv, u"paragraphboundary"sv, u"documentboundary"sv))
        return {};

    // 4. If this selection is empty, abort these steps.
    if (is_empty())
        return {};

    auto focus = focus_node();
    if (!focus)
        return {};

    // 5. Let effectiveDirection be backwards.
    auto effective_direction = Direction::Backwards;

    // 6. If direction is ASCII case-insensitive match with "forward", set effectiveDirection to forwards.
    if (direction->equals_ignoring_ascii_case(u"forward"sv))
        effective_direction = Direction::Forwards;

    // Inline base direction of this selection's focus.
    auto focus_directionality = DOM::Element::Directionality::Ltr;
    if (auto* text = as_if<DOM::Text>(*focus)) {
        if (auto text_directionality = text->directionality(); text_directionality.has_value())
            focus_directionality = *text_directionality;
    } else if (auto* element = as_if<DOM::Element>(*focus)) {
        focus_directionality = element->directionality();
    }

    // 7. If direction is ASCII case-insensitive match with "right" and inline base direction of this selection's focus is ltr, set effectiveDirection to forwards.
    if (direction->equals_ignoring_ascii_case(u"right"sv) && focus_directionality == DOM::Element::Directionality::Ltr)
        effective_direction = Direction::Forwards;

    // 8. If direction is ASCII case-insensitive match with "left" and inline base direction of this selection's focus is rtl, set effectiveDirection to forwards.
    if (direction->equals_ignoring_ascii_case(u"left"sv) && focus_directionality == DOM::Element::Directionality::Rtl)
        effective_direction = Direction::Forwards;

    // 9. Set this selection's direction to effectiveDirection.
    // NOTE: This is handled by calls to move_offset_to_* later on

    // 10. If alter is ASCII case-insensitive match with "extend", set this selection's focus to the location as if the user had requested to extend selection by granularity.
    // 11. Otherwise, set this selection's focus and anchor to the location as if the user had requested to move selection by granularity.
    auto collapse_selection = alter->equals_ignoring_ascii_case(u"move"sv);

    auto move_focus_to_visual_line_boundary = [&](bool forwards) {
        auto* text = as_if<DOM::Text>(focus_node().ptr());
        if (!text)
            return;
        auto position = forwards
            ? find_visual_line_end(*text, focus_offset(), m_focus_affinity)
            : CursorLinePosition { find_visual_line_start(*text, focus_offset(), m_focus_affinity), TextAffinity::Downstream };
        if (collapse_selection)
            MUST(collapse(text, position.offset));
        else
            MUST(set_base_and_extent(*anchor_node(), anchor_offset(), *text, position.offset));
        m_focus_affinity = position.affinity;
        m_document->reset_cursor_blink_cycle();
        m_document->set_cursor_position_needs_repaint();
    };

    // TODO: Implement the sentence, paragraph, and document granularity options.
    if (effective_direction == Direction::Forwards) {
        if (granularity->equals_ignoring_ascii_case(u"character"sv))
            move_offset_to_next_character(collapse_selection);
        else if (granularity->equals_ignoring_ascii_case(u"word"sv))
            move_offset_to_next_word(collapse_selection);
        else if (granularity->equals_ignoring_ascii_case(u"line"sv))
            move_offset_to_next_line(collapse_selection);
        else if (granularity->equals_ignoring_ascii_case(u"lineboundary"sv))
            move_focus_to_visual_line_boundary(true);
    } else {
        if (granularity->equals_ignoring_ascii_case(u"character"sv))
            move_offset_to_previous_character(collapse_selection);
        else if (granularity->equals_ignoring_ascii_case(u"word"sv))
            move_offset_to_previous_word(collapse_selection);
        else if (granularity->equals_ignoring_ascii_case(u"line"sv))
            move_offset_to_previous_line(collapse_selection);
        else if (granularity->equals_ignoring_ascii_case(u"lineboundary"sv))
            move_focus_to_visual_line_boundary(false);
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
    // The range's boundary points can be in a tree that's not connected to the document (for example, inside the shadow
    // tree of a removed host). Such a range isn't comparable with a node in the document.
    if (&m_range->start().node->shadow_including_root() != m_document.ptr())
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

Optional<Utf16String> Selection::try_form_control_selected_text_for_stringifier() const
{
    // FIXME: According to https://bugzilla.mozilla.org/show_bug.cgi?id=85686#c69,
    // sometimes you want the selection from a previously focused form text, probably
    // when a button or context menu has temporarily stolen focus but page scripts
    // still expect window.getSelection() to have the goodies.

    auto const* form_element = as_if<HTML::FormAssociatedTextControlElement>(m_document->active_element());
    if (!form_element)
        return {};
    return form_element->selected_text_for_stringifier();
}

Utf16String Selection::to_string() const
{
    // https://w3c.github.io/selection-api/#dom-selection-stringifier
    // If the selection is within a textarea or input element, it must return the
    // selected substring in its value.
    if (auto form_text = try_form_control_selected_text_for_stringifier(); form_text.has_value())
        return form_text.release_value();

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
    m_focus_affinity = TextAffinity::Downstream;

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

    return DOM::Position::create(m_document->realm(), *m_range->start_container(), m_range->start_offset(), m_focus_affinity);
}

// Cross-node caret navigation: when character or line movement reaches the edge of the current node, the caret moves
// to the closest node in the editing host that can house it: either a text node with rendered text, or a block-level
// element rendering an empty line (such as the `<p><br></p>` paragraphs produced by pressing Enter on an empty line).
// FIXME: Word movement is still limited to a single DOM node.

enum class CaretNavigationDirection : u8 {
    Forward,
    Backward,
};

enum class CaretEntryMode : u8 {
    LineEdge,
    ClosestToInlineCoordinate,
};

static bool text_node_has_rendered_text(DOM::Text const& text)
{
    for (auto const& line : collect_visual_lines(text)) {
        if (!line.fragments.is_empty())
            return true;
    }
    return false;
}

// A <br> that renders an empty line hosts a caret position on its parent, at its child index. This covers a leading
// <br> in a paragraph with text after it, and the lines between consecutive <br>s.
static bool is_empty_line_break(DOM::Node& node)
{
    auto* br = as_if<HTML::HTMLBRElement>(node);
    return br && br->is_editable() && br->represents_empty_line();
}

// A block-level element that renders no text but hosts an empty line where the caret can sit, such as `<p><br></p>`.
static bool is_empty_line_host(DOM::Node& node)
{
    auto* element = as_if<DOM::Element>(node);
    if (!element || !element->is_editable())
        return false;
    auto element_paintable = element->unsafe_paintable();
    auto* paintable = as_if<Painting::PaintableWithLines>(element_paintable.ptr());
    if (!paintable)
        return false;
    if (paintable->layout_node().display().is_inline_outside())
        return false;

    bool has_rendered_text = false;
    element->for_each_in_subtree_of_type<DOM::Text>([&](auto& text) {
        if (!text_node_has_rendered_text(text))
            return TraversalDecision::Continue;
        has_rendered_text = true;
        return TraversalDecision::Break;
    });
    return !has_rendered_text;
}

static GC::Ptr<DOM::Node> adjacent_caret_host_in_editing_host(DOM::Node& from, DOM::Node& editing_host, CaretNavigationDirection direction)
{
    auto* node = &from;
    while (node) {
        node = direction == CaretNavigationDirection::Forward
            ? node->next_in_pre_order(&editing_host)
            : node->previous_in_pre_order();
        if (!node || node == &editing_host || !editing_host.is_inclusive_ancestor_of(*node))
            return nullptr;
        // Walking backwards visits ancestors of the origin; the caret is already inside those.
        if (node->is_inclusive_ancestor_of(from))
            continue;
        if (auto* text = as_if<DOM::Text>(*node); text && text->is_editable() && text_node_has_rendered_text(*text))
            return node;
        if (is_empty_line_host(*node) || is_empty_line_break(*node))
            return node;
    }
    return nullptr;
}

// The node caret navigation should walk from: for a caret anchored on an element, that is the child the offset
// points at (e.g. the <br> hosting the empty line the caret sits on), not the element itself.
static DOM::Node& caret_navigation_origin(DOM::Node& node, size_t offset)
{
    if (auto* child = node.child_at_index(offset))
        return *child;
    return node;
}

// Moves the caret out of `from` into the closest caret host before or after it in the editing host. Enters text
// nodes at the rendered edge facing `from` (LineEdge, for horizontal movement) or at the position on the edge line
// closest to the given inline coordinate (for vertical movement). Empty line hosts house the caret at offset 0.
static bool move_cursor_to_adjacent_caret_host(Selection& selection, DOM::Node& from, CaretNavigationDirection direction, CaretEntryMode entry_mode, Optional<CSSPixels> inline_coordinate, bool collapse_selection)
{
    auto editing_host = from.editing_host();
    if (!editing_host)
        return false;

    selection.document()->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);

    auto target = adjacent_caret_host_in_editing_host(from, *editing_host, direction);
    if (!target)
        return false;

    size_t new_offset = 0;
    auto new_affinity = TextAffinity::Downstream;

    // A <br> hosting an empty line houses the caret on its parent, at the child index of the <br>.
    if (is<HTML::HTMLBRElement>(*target) && target->parent()) {
        new_offset = target->index();
        target = target->parent();
    }

    if (auto* text = as_if<DOM::Text>(*target)) {
        Optional<CursorLinePosition> position;
        if (entry_mode == CaretEntryMode::LineEdge) {
            position = direction == CaretNavigationDirection::Forward
                ? cursor_position_at_visual_start(*text)
                : cursor_position_at_visual_end(*text);
        } else {
            position = direction == CaretNavigationDirection::Forward
                ? cursor_position_on_first_line_closest_to(*text, inline_coordinate)
                : cursor_position_on_last_line_closest_to(*text, inline_coordinate);
        }
        if (!position.has_value())
            return false;
        new_offset = position->offset;
        new_affinity = position->affinity;
    }

    if (collapse_selection)
        MUST(selection.collapse(target, new_offset));
    else
        MUST(selection.set_base_and_extent(*selection.anchor_node(), selection.anchor_offset(), *target, new_offset));
    selection.set_focus_affinity(new_affinity);
    selection.document()->reset_cursor_blink_cycle();
    selection.document()->set_cursor_position_needs_repaint();
    return true;
}

void Selection::move_offset_to_next_character(bool collapse_selection)
{
    // If there is a selection range, collapse to the end of that range without moving forward
    if (collapse_selection && !is_collapsed()) {
        MUST(collapse(m_range->end_container(), m_range->end_offset()));
        m_document->reset_cursor_blink_cycle();
        scroll_focus_into_view();
        return;
    }

    auto node = focus_node();
    if (!node)
        return;

    if (auto* text_node = as_if<DOM::Text>(*node)) {
        // Move forward within the text node if possible
        if (auto new_position = compute_cursor_position_on_next_character(*text_node, focus_offset(), m_focus_affinity); new_position.has_value()) {
            if (collapse_selection) {
                MUST(collapse(text_node, new_position->offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(set_base_and_extent(*anchor_node(), anchor_offset(), *text_node, new_position->offset));
            }
            m_focus_affinity = new_position->affinity;
            m_document->set_cursor_position_needs_repaint();
        }
        // At the very end of this text node, move into the closest caret host after it.
        else {
            move_cursor_to_adjacent_caret_host(*this, *text_node, CaretNavigationDirection::Forward, CaretEntryMode::LineEdge, {}, collapse_selection);
        }
    }
    // The focus is parked on an element, e.g. an empty line; step into the closest caret host after it.
    else {
        move_cursor_to_adjacent_caret_host(*this, caret_navigation_origin(*node, focus_offset()), CaretNavigationDirection::Forward, CaretEntryMode::LineEdge, {}, collapse_selection);
    }
    scroll_focus_into_view();
}

void Selection::move_offset_to_previous_character(bool collapse_selection)
{
    // If there is a selection range, collapse to the start of that range without moving backward
    if (collapse_selection && !is_collapsed()) {
        MUST(collapse(m_range->start_container(), m_range->start_offset()));
        m_document->reset_cursor_blink_cycle();
        scroll_focus_into_view();
        return;
    }

    auto node = focus_node();
    if (!node)
        return;

    if (auto* text_node = as_if<DOM::Text>(*node)) {
        // Move backward within the text node if possible
        if (auto new_position = compute_cursor_position_on_previous_character(*text_node, focus_offset(), m_focus_affinity); new_position.has_value()) {
            if (collapse_selection) {
                MUST(collapse(text_node, new_position->offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(set_base_and_extent(*anchor_node(), anchor_offset(), *text_node, new_position->offset));
            }
            m_focus_affinity = new_position->affinity;
            m_document->set_cursor_position_needs_repaint();
        }
        // At the very start of this text node, move into the closest caret host before it.
        else {
            move_cursor_to_adjacent_caret_host(*this, *text_node, CaretNavigationDirection::Backward, CaretEntryMode::LineEdge, {}, collapse_selection);
        }
    }
    // The focus is parked on an element, e.g. an empty line; step into the closest caret host before it.
    else {
        move_cursor_to_adjacent_caret_host(*this, caret_navigation_origin(*node, focus_offset()), CaretNavigationDirection::Backward, CaretEntryMode::LineEdge, {}, collapse_selection);
    }
    scroll_focus_into_view();
}

void Selection::move_offset_to_next_word(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(focus_node().ptr());
    if (!text_node) {
        // The focus is parked on an element, e.g. an empty line; step into the closest caret host after it.
        if (auto node = focus_node())
            move_cursor_to_adjacent_caret_host(*this, caret_navigation_origin(*node, focus_offset()), CaretNavigationDirection::Forward, CaretEntryMode::LineEdge, {}, collapse_selection);
        scroll_focus_into_view();
        return;
    }

    while (true) {
        auto focus_offset = this->focus_offset();
        if (focus_offset == text_node->data().length_in_code_units())
            break;

        if (auto offset = text_node->word_segmenter().next_boundary(focus_offset); offset.has_value()) {
            if (collapse_selection) {
                MUST(collapse(text_node, *offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(set_base_and_extent(*anchor_node(), anchor_offset(), *text_node, *offset));
            }
            auto word = text_node->data().substring_view(focus_offset, *offset - focus_offset);
            if (Unicode::Segmenter::should_continue_beyond_word(word))
                continue;
        }
        break;
    }
    scroll_focus_into_view();
}

void Selection::move_offset_to_previous_word(bool collapse_selection)
{
    auto* text_node = as_if<DOM::Text>(focus_node().ptr());
    if (!text_node) {
        // The focus is parked on an element, e.g. an empty line; step into the closest caret host before it.
        if (auto node = focus_node())
            move_cursor_to_adjacent_caret_host(*this, caret_navigation_origin(*node, focus_offset()), CaretNavigationDirection::Backward, CaretEntryMode::LineEdge, {}, collapse_selection);
        scroll_focus_into_view();
        return;
    }

    while (true) {
        auto focus_offset = this->focus_offset();
        if (auto offset = text_node->word_segmenter().previous_boundary(focus_offset); offset.has_value()) {
            if (collapse_selection) {
                MUST(collapse(text_node, *offset));
                m_document->reset_cursor_blink_cycle();
            } else {
                MUST(set_base_and_extent(*anchor_node(), anchor_offset(), *text_node, *offset));
            }
            auto word = text_node->data().substring_view(*offset, focus_offset - *offset);
            if (Unicode::Segmenter::should_continue_beyond_word(word))
                continue;
        }
        break;
    }
    scroll_focus_into_view();
}

void Selection::move_offset_to_next_line(bool collapse_selection)
{
    auto node = focus_node();
    if (!node)
        return;

    auto* text_node = as_if<DOM::Text>(*node);
    if (!text_node) {
        // The focus is parked on an element, e.g. an empty line; move to the closest caret host below.
        move_cursor_to_adjacent_caret_host(*this, caret_navigation_origin(*node, focus_offset()), CaretNavigationDirection::Forward, CaretEntryMode::ClosestToInlineCoordinate, {}, collapse_selection);
        scroll_focus_into_view();
        return;
    }

    // On the last visual line of this text node, move to the closest caret host below instead.
    if (offset_is_on_last_visual_line(*text_node, focus_offset(), m_focus_affinity)) {
        auto inline_coordinate = cursor_inline_coordinate(*text_node, focus_offset(), m_focus_affinity);
        if (move_cursor_to_adjacent_caret_host(*this, *text_node, CaretNavigationDirection::Forward, CaretEntryMode::ClosestToInlineCoordinate, inline_coordinate, collapse_selection)) {
            scroll_focus_into_view();
            return;
        }
    }

    auto new_position = compute_cursor_position_on_next_line(*text_node, focus_offset(), m_focus_affinity);
    if (!new_position.has_value())
        return;

    if (collapse_selection) {
        MUST(collapse(text_node, new_position->offset));
        m_document->reset_cursor_blink_cycle();
    } else {
        MUST(set_base_and_extent(*anchor_node(), anchor_offset(), *text_node, new_position->offset));
    }
    m_focus_affinity = new_position->affinity;
    scroll_focus_into_view();
}

void Selection::move_offset_to_previous_line(bool collapse_selection)
{
    auto node = focus_node();
    if (!node)
        return;

    auto* text_node = as_if<DOM::Text>(*node);
    if (!text_node) {
        // The focus is parked on an element, e.g. an empty line; move to the closest caret host above.
        move_cursor_to_adjacent_caret_host(*this, caret_navigation_origin(*node, focus_offset()), CaretNavigationDirection::Backward, CaretEntryMode::ClosestToInlineCoordinate, {}, collapse_selection);
        scroll_focus_into_view();
        return;
    }

    // On the first visual line of this text node, move to the closest caret host above instead.
    if (offset_is_on_first_visual_line(*text_node, focus_offset(), m_focus_affinity)) {
        auto inline_coordinate = cursor_inline_coordinate(*text_node, focus_offset(), m_focus_affinity);
        if (move_cursor_to_adjacent_caret_host(*this, *text_node, CaretNavigationDirection::Backward, CaretEntryMode::ClosestToInlineCoordinate, inline_coordinate, collapse_selection)) {
            scroll_focus_into_view();
            return;
        }
    }

    auto new_position = compute_cursor_position_on_previous_line(*text_node, focus_offset(), m_focus_affinity);
    if (!new_position.has_value())
        return;

    if (collapse_selection) {
        MUST(collapse(text_node, new_position->offset));
        m_document->reset_cursor_blink_cycle();
    } else {
        MUST(set_base_and_extent(*anchor_node(), anchor_offset(), *text_node, new_position->offset));
    }
    m_focus_affinity = new_position->affinity;
    scroll_focus_into_view();
}

void Selection::scroll_focus_into_view()
{
    auto focus = focus_node();
    if (!focus)
        return;

    m_document->update_layout(DOM::UpdateLayoutReason::ScrollCursorIntoView);

    if (auto* text = as_if<DOM::Text>(*focus)) {
        Painting::Paintable::scroll_text_offset_into_view(*text, focus_offset(), m_focus_affinity);
        return;
    }

    auto paintable = focus->paintable();
    if (!paintable)
        return;

    paintable->scroll_ancestor_to_offset_into_view(focus_offset());
}

}
