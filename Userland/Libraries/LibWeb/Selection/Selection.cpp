/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SelectionPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Selection {

JS_DEFINE_ALLOCATOR(Selection);

JS::NonnullGCPtr<Selection> Selection::create(JS::NonnullGCPtr<JS::Realm> realm, JS::NonnullGCPtr<DOM::Document> document)
{
    return realm->heap().allocate<Selection>(realm, realm, document);
}

Selection::Selection(JS::NonnullGCPtr<JS::Realm> realm, JS::NonnullGCPtr<DOM::Document> document)
    : PlatformObject(realm)
    , m_document(document)
{
}

Selection::~Selection() = default;

void Selection::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Selection);
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
JS::GCPtr<DOM::Node> Selection::anchor_node()
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
JS::GCPtr<DOM::Node> Selection::focus_node()
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
    return const_cast<Selection*>(this)->anchor_node() == const_cast<Selection*>(this)->focus_node();
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

// https://w3c.github.io/selection-api/#dom-selection-getrangeat
WebIDL::ExceptionOr<JS::GCPtr<DOM::Range>> Selection::get_range_at(unsigned index)
{
    // The method must throw an IndexSizeError exception if index is not 0, or if this is empty.
    if (index != 0 || is_empty())
        return WebIDL::IndexSizeError::create(realm(), "Selection.getRangeAt() on empty Selection or with invalid argument"_fly_string);

    // Otherwise, it must return a reference to (not a copy of) this's range.
    return m_range;
}

// https://w3c.github.io/selection-api/#dom-selection-addrange
void Selection::add_range(JS::NonnullGCPtr<DOM::Range> range)
{
    // 1. If the root of the range's boundary points are not the document associated with this, abort these steps.
    if (&range->start_container()->root() != m_document.ptr())
        return;

    // 2. If rangeCount is not 0, abort these steps.
    if (range_count() != 0)
        return;

    // 3. Set this's range to range by a strong reference (not by making a copy).
    set_range(range);
}

// https://w3c.github.io/selection-api/#dom-selection-removerange
WebIDL::ExceptionOr<void> Selection::remove_range(JS::NonnullGCPtr<DOM::Range> range)
{
    // The method must make this empty by disassociating its range if this's range is range.
    if (m_range == range) {
        set_range(nullptr);
        return {};
    }

    // Otherwise, it must throw a NotFoundError.
    return WebIDL::NotFoundError::create(realm(), "Selection.removeRange() with invalid argument"_fly_string);
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
WebIDL::ExceptionOr<void> Selection::collapse(JS::GCPtr<DOM::Node> node, unsigned offset)
{
    // 1. If node is null, this method must behave identically as removeAllRanges() and abort these steps.
    if (!node) {
        remove_all_ranges();
        return {};
    }

    // 2. The method must throw an IndexSizeError exception if offset is longer than node's length and abort these steps.
    if (offset > node->length()) {
        return WebIDL::IndexSizeError::create(realm(), "Selection.collapse() with offset longer than node's length"_fly_string);
    }

    // 3. If node's root is not the document associated with this, abort these steps.
    if (&node->root() != m_document.ptr())
        return {};

    // 4. Otherwise, let newRange be a new range.
    auto new_range = DOM::Range::create(*m_document);

    // 5. Set the start the start and the end of newRange to (node, offset).
    TRY(new_range->set_start(*node, offset));

    // 6. Set this's range to newRange.
    set_range(new_range);

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-setposition
WebIDL::ExceptionOr<void> Selection::set_position(JS::GCPtr<DOM::Node> node, unsigned offset)
{
    // The method must be an alias, and behave identically, to collapse().
    return collapse(node, offset);
}

// https://w3c.github.io/selection-api/#dom-selection-collapsetostart
WebIDL::ExceptionOr<void> Selection::collapse_to_start()
{
    // 1. The method must throw InvalidStateError exception if the this is empty.
    if (!m_range) {
        return WebIDL::InvalidStateError::create(realm(), "Selection.collapse_to_start() on empty range"_fly_string);
    }

    // 2. Otherwise, it must create a new range
    auto new_range = DOM::Range::create(*m_document);

    // 3. Set the start both its start and end to the start of this's range
    TRY(new_range->set_start(*anchor_node(), m_range->start_offset()));
    TRY(new_range->set_end(*anchor_node(), m_range->start_offset()));

    // 4. Then set this's range to the newly-created range.
    set_range(new_range);
    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-collapsetoend
WebIDL::ExceptionOr<void> Selection::collapse_to_end()
{
    // 1. The method must throw InvalidStateError exception if the this is empty.
    if (!m_range) {
        return WebIDL::InvalidStateError::create(realm(), "Selection.collapse_to_end() on empty range"_fly_string);
    }

    // 2. Otherwise, it must create a new range
    auto new_range = DOM::Range::create(*m_document);

    // 3. Set the start both its start and end to the start of this's range
    TRY(new_range->set_start(*anchor_node(), m_range->end_offset()));
    TRY(new_range->set_end(*anchor_node(), m_range->end_offset()));

    // 4. Then set this's range to the newly-created range.
    set_range(new_range);

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-extend
WebIDL::ExceptionOr<void> Selection::extend(JS::NonnullGCPtr<DOM::Node> node, unsigned offset)
{
    // 1. If node's root is not the document associated with this, abort these steps.
    if (&node->root() != m_document.ptr())
        return {};

    // 2. If this is empty, throw an InvalidStateError exception and abort these steps.
    if (!m_range) {
        return WebIDL::InvalidStateError::create(realm(), "Selection.extend() on empty range"_fly_string);
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
    }
    // 6. Otherwise, if oldAnchor is before or equal to newFocus, set the start newRange's start to oldAnchor, then set its end to newFocus.
    else if (old_anchor_node.is_before(new_focus_node) || &old_anchor_node == new_focus_node.ptr()) {
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
    if (new_focus_node->is_before(old_anchor_node)) {
        m_direction = Direction::Backwards;
    } else {
        m_direction = Direction::Forwards;
    }

    return {};
}

// https://w3c.github.io/selection-api/#dom-selection-setbaseandextent
WebIDL::ExceptionOr<void> Selection::set_base_and_extent(JS::NonnullGCPtr<DOM::Node> anchor_node, unsigned anchor_offset, JS::NonnullGCPtr<DOM::Node> focus_node, unsigned focus_offset)
{
    // 1. If anchorOffset is longer than anchorNode's length or if focusOffset is longer than focusNode's length, throw an IndexSizeError exception and abort these steps.
    if (anchor_offset > anchor_node->length())
        return WebIDL::IndexSizeError::create(realm(), "Anchor offset points outside of the anchor node"_fly_string);

    if (focus_offset > focus_node->length())
        return WebIDL::IndexSizeError::create(realm(), "Focus offset points outside of the focus node"_fly_string);

    // 2. If document associated with this is not a shadow-including inclusive ancestor of anchorNode or focusNode, abort these steps.
    if (!(m_document->is_shadow_including_inclusive_ancestor_of(anchor_node) || m_document->is_shadow_including_inclusive_ancestor_of(focus_node)))
        return {};

    // 3. Let anchor be the boundary point (anchorNode, anchorOffset) and let focus be the boundary point (focusNode, focusOffset).

    // 4. Let newRange be a new range.
    auto new_range = DOM::Range::create(*m_document);

    // 5. If anchor is before focus, set the start the newRange's start to anchor and its end to focus. Otherwise, set the start them to focus and anchor respectively.
    auto position_of_anchor_relative_to_focus = DOM::position_of_boundary_point_relative_to_other_boundary_point(anchor_node, anchor_offset, focus_node, focus_offset);
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
WebIDL::ExceptionOr<void> Selection::select_all_children(JS::NonnullGCPtr<DOM::Node> node)
{
    // 1. If node's root is not the document associated with this, abort these steps.
    if (&node->root() != m_document.ptr())
        return {};

    // 2. Let newRange be a new range and childCount be the number of children of node.
    auto new_range = DOM::Range::create(*m_document);
    auto child_count = node->child_count();

    // 3. Set newRange's start to (node, 0).
    TRY(new_range->set_start(node, 0));

    // 4. Set newRange's end to (node, childCount).
    TRY(new_range->set_end(node, child_count));

    // 5. Set this's range to newRange.
    set_range(new_range);

    // 6. Set this's direction to forwards.
    m_direction = Direction::Forwards;

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
bool Selection::contains_node(JS::NonnullGCPtr<DOM::Node> node, bool allow_partial_containment) const
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
        auto start_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(
            *m_range->start_container(),
            m_range->start_offset(),
            node,
            0);
        auto end_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(
            *m_range->end_container(),
            m_range->end_offset(),
            node,
            node->length());

        return (start_relative_position == DOM::RelativeBoundaryPointPosition::Before || start_relative_position == DOM::RelativeBoundaryPointPosition::Equal)
            && (end_relative_position == DOM::RelativeBoundaryPointPosition::Equal || end_relative_position == DOM::RelativeBoundaryPointPosition::After);
    }

    // If allowPartialContainment is true, the method must return true if and only if
    // start of its range is before or visually equivalent to the last boundary point in the node
    // and end of its range is after or visually equivalent to the first boundary point in the node.

    auto start_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(
        *m_range->start_container(),
        m_range->start_offset(),
        node,
        node->length());
    auto end_relative_position = DOM::position_of_boundary_point_relative_to_other_boundary_point(
        *m_range->end_container(),
        m_range->end_offset(),
        node,
        0);

    return (start_relative_position == DOM::RelativeBoundaryPointPosition::Before || start_relative_position == DOM::RelativeBoundaryPointPosition::Equal)
        && (end_relative_position == DOM::RelativeBoundaryPointPosition::Equal || end_relative_position == DOM::RelativeBoundaryPointPosition::After);
}

String Selection::to_string() const
{
    // FIXME: This needs more work to be compatible with other engines.
    //        See https://www.w3.org/Bugs/Public/show_bug.cgi?id=10583
    if (!m_range)
        return String {};
    return m_range->to_string();
}

JS::NonnullGCPtr<DOM::Document> Selection::document() const
{
    return m_document;
}

JS::GCPtr<DOM::Range> Selection::range() const
{
    return m_range;
}

void Selection::set_range(JS::GCPtr<DOM::Range> range)
{
    if (m_range == range)
        return;

    if (m_range)
        m_range->set_associated_selection({}, nullptr);

    m_range = range;

    if (m_range)
        m_range->set_associated_selection({}, this);
}

}
