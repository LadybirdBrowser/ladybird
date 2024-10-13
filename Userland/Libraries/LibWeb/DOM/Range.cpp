/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/RangePrototype.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(Range);

HashTable<Range*>& Range::live_ranges()
{
    static HashTable<Range*> ranges;
    return ranges;
}

JS::NonnullGCPtr<Range> Range::create(HTML::Window& window)
{
    return Range::create(window.associated_document());
}

JS::NonnullGCPtr<Range> Range::create(Document& document)
{
    auto& realm = document.realm();
    return realm.heap().allocate<Range>(realm, document);
}

JS::NonnullGCPtr<Range> Range::create(Node& start_container, WebIDL::UnsignedLong start_offset, Node& end_container, WebIDL::UnsignedLong end_offset)
{
    auto& realm = start_container.realm();
    return realm.heap().allocate<Range>(realm, start_container, start_offset, end_container, end_offset);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<Range>> Range::construct_impl(JS::Realm& realm)
{
    auto& window = verify_cast<HTML::Window>(realm.global_object());
    return Range::create(window);
}

Range::Range(Document& document)
    : Range(document, 0, document, 0)
{
}

Range::Range(Node& start_container, WebIDL::UnsignedLong start_offset, Node& end_container, WebIDL::UnsignedLong end_offset)
    : AbstractRange(start_container, start_offset, end_container, end_offset)
{
    live_ranges().set(this);
}

Range::~Range()
{
    live_ranges().remove(this);
}

void Range::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Range);
}

void Range::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_selection);
}

void Range::set_associated_selection(Badge<Selection::Selection>, JS::GCPtr<Selection::Selection> selection)
{
    m_associated_selection = selection;
    update_associated_selection();
}

void Range::update_associated_selection()
{
    auto& document = m_start_container->document();
    if (auto* viewport = document.paintable()) {
        viewport->recompute_selection_states(*this);
        viewport->update_selection();
        viewport->set_needs_display();
    }

    // https://w3c.github.io/selection-api/#selectionchange-event
    // When the selection is dissociated with its range, associated with a new range, or the
    // associated range's boundary point is mutated either by the user or the content script, the
    // user agent must schedule a selectionchange event on document.
    schedule_a_selectionchange_event(document);

    // When an input or textarea element provide a text selection and its selection changes (in
    // either extent or direction), the user agent must schedule a selectionchange event on the
    // element.
    if (m_start_container == m_end_container) {
        if (is<HTML::HTMLInputElement>(*m_start_container))
            schedule_a_selectionchange_event(verify_cast<HTML::HTMLInputElement>(*m_start_container));
        if (is<HTML::HTMLTextAreaElement>(*m_start_container))
            schedule_a_selectionchange_event(verify_cast<HTML::HTMLTextAreaElement>(*m_start_container));
    }
}

// https://w3c.github.io/selection-api/#scheduling-selectionhange-event
template<SelectionChangeTarget T>
void Range::schedule_a_selectionchange_event(T& target)
{
    // 1. If target's has scheduled selectionchange event is true, abort these steps.
    if (target.has_scheduled_selectionchange_event())
        return;

    // AD-HOC (https://github.com/w3c/selection-api/issues/338):
    // Set target's has scheduled selectionchange event to true
    target.set_scheduled_selectionchange_event(true);

    // 2. Queue a task on the user interaction task source to fire a selectionchange event on
    //    target.
    JS::NonnullGCPtr<Document> document = m_start_container->document();
    queue_global_task(HTML::Task::Source::UserInteraction, relevant_global_object(*document), JS::create_heap_function(document->heap(), [&] {
        fire_a_selectionchange_event(target);
    }));
}

// https://w3c.github.io/selection-api/#firing-selectionhange-event
template<SelectionChangeTarget T>
void Range::fire_a_selectionchange_event(T& target)
{
    // 1. Set target's has scheduled selectionchange event to false.
    target.set_scheduled_selectionchange_event(false);

    // 2. If target is an element, fire an event named selectionchange, which bubbles and not
    //    cancelable, at target.
    // 3. Otherwise, if target is a document, fire an event named selectionchange, which does not
    //    bubble and not cancelable, at target.
    EventInit event_init;
    event_init.bubbles = SameAs<T, Element>;
    event_init.cancelable = false;

    auto& realm = m_start_container->document().realm();
    auto event = DOM::Event::create(realm, HTML::EventNames::selectionchange, event_init);
    target.dispatch_event(event);
}

// https://dom.spec.whatwg.org/#concept-range-root
Node& Range::root()
{
    // The root of a live range is the root of its start node.
    return m_start_container->root();
}

Node const& Range::root() const
{
    return m_start_container->root();
}

// https://dom.spec.whatwg.org/#concept-range-bp-position
RelativeBoundaryPointPosition position_of_boundary_point_relative_to_other_boundary_point(Node const& node_a, u32 offset_a, Node const& node_b, u32 offset_b)
{
    // 1. Assert: nodeA and nodeB have the same root.
    VERIFY(&node_a.root() == &node_b.root());

    // 2. If nodeA is nodeB, then return equal if offsetA is offsetB, before if offsetA is less than offsetB, and after if offsetA is greater than offsetB.
    if (&node_a == &node_b) {
        if (offset_a == offset_b)
            return RelativeBoundaryPointPosition::Equal;

        if (offset_a < offset_b)
            return RelativeBoundaryPointPosition::Before;

        return RelativeBoundaryPointPosition::After;
    }

    // 3. If nodeA is following nodeB, then if the position of (nodeB, offsetB) relative to (nodeA, offsetA) is before, return after, and if it is after, return before.
    if (node_a.is_following(node_b)) {
        auto relative_position = position_of_boundary_point_relative_to_other_boundary_point(node_b, offset_b, node_a, offset_a);

        if (relative_position == RelativeBoundaryPointPosition::Before)
            return RelativeBoundaryPointPosition::After;

        if (relative_position == RelativeBoundaryPointPosition::After)
            return RelativeBoundaryPointPosition::Before;
    }

    // 4. If nodeA is an ancestor of nodeB:
    if (node_a.is_ancestor_of(node_b)) {
        // 1. Let child be nodeB.
        JS::NonnullGCPtr<Node const> child = node_b;

        // 2. While child is not a child of nodeA, set child to its parent.
        while (!node_a.is_parent_of(child)) {
            auto* parent = child->parent();
            VERIFY(parent);
            child = *parent;
        }

        // 3. If child’s index is less than offsetA, then return after.
        if (child->index() < offset_a)
            return RelativeBoundaryPointPosition::After;
    }

    // 5. Return before.
    return RelativeBoundaryPointPosition::Before;
}

WebIDL::ExceptionOr<void> Range::set_start_or_end(Node& node, u32 offset, StartOrEnd start_or_end)
{
    // To set the start or end of a range to a boundary point (node, offset), run these steps:

    // 1. If node is a doctype, then throw an "InvalidNodeTypeError" DOMException.
    if (is<DocumentType>(node))
        return WebIDL::InvalidNodeTypeError::create(realm(), "Node cannot be a DocumentType."_string);

    // 2. If offset is greater than node’s length, then throw an "IndexSizeError" DOMException.
    if (offset > node.length())
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Node does not contain a child at offset {}", offset)));

    // 3. Let bp be the boundary point (node, offset).

    if (start_or_end == StartOrEnd::Start) {
        // -> If these steps were invoked as "set the start"

        // 1. If range’s root is not equal to node’s root, or if bp is after the range’s end, set range’s end to bp.
        if (&root() != &node.root() || position_of_boundary_point_relative_to_other_boundary_point(node, offset, m_end_container, m_end_offset) == RelativeBoundaryPointPosition::After) {
            m_end_container = node;
            m_end_offset = offset;
        }

        // 2. Set range’s start to bp.
        m_start_container = node;
        m_start_offset = offset;
    } else {
        // -> If these steps were invoked as "set the end"
        VERIFY(start_or_end == StartOrEnd::End);

        // 1. If range’s root is not equal to node’s root, or if bp is before the range’s start, set range’s start to bp.
        if (&root() != &node.root() || position_of_boundary_point_relative_to_other_boundary_point(node, offset, m_start_container, m_start_offset) == RelativeBoundaryPointPosition::Before) {
            m_start_container = node;
            m_start_offset = offset;
        }

        // 2. Set range’s end to bp.
        m_end_container = node;
        m_end_offset = offset;
    }

    update_associated_selection();
    return {};
}

// https://dom.spec.whatwg.org/#concept-range-bp-set
WebIDL::ExceptionOr<void> Range::set_start(Node& node, WebIDL::UnsignedLong offset)
{
    // The setStart(node, offset) method steps are to set the start of this to boundary point (node, offset).
    return set_start_or_end(node, offset, StartOrEnd::Start);
}

WebIDL::ExceptionOr<void> Range::set_end(Node& node, WebIDL::UnsignedLong offset)
{
    // The setEnd(node, offset) method steps are to set the end of this to boundary point (node, offset).
    return set_start_or_end(node, offset, StartOrEnd::End);
}

// https://dom.spec.whatwg.org/#dom-range-setstartbefore
WebIDL::ExceptionOr<void> Range::set_start_before(Node& node)
{
    // 1. Let parent be node’s parent.
    auto* parent = node.parent();

    // 2. If parent is null, then throw an "InvalidNodeTypeError" DOMException.
    if (!parent)
        return WebIDL::InvalidNodeTypeError::create(realm(), "Given node has no parent."_string);

    // 3. Set the start of this to boundary point (parent, node’s index).
    return set_start_or_end(*parent, node.index(), StartOrEnd::Start);
}

// https://dom.spec.whatwg.org/#dom-range-setstartafter
WebIDL::ExceptionOr<void> Range::set_start_after(Node& node)
{
    // 1. Let parent be node’s parent.
    auto* parent = node.parent();

    // 2. If parent is null, then throw an "InvalidNodeTypeError" DOMException.
    if (!parent)
        return WebIDL::InvalidNodeTypeError::create(realm(), "Given node has no parent."_string);

    // 3. Set the start of this to boundary point (parent, node’s index plus 1).
    return set_start_or_end(*parent, node.index() + 1, StartOrEnd::Start);
}

// https://dom.spec.whatwg.org/#dom-range-setendbefore
WebIDL::ExceptionOr<void> Range::set_end_before(Node& node)
{
    // 1. Let parent be node’s parent.
    auto* parent = node.parent();

    // 2. If parent is null, then throw an "InvalidNodeTypeError" DOMException.
    if (!parent)
        return WebIDL::InvalidNodeTypeError::create(realm(), "Given node has no parent."_string);

    // 3. Set the end of this to boundary point (parent, node’s index).
    return set_start_or_end(*parent, node.index(), StartOrEnd::End);
}

// https://dom.spec.whatwg.org/#dom-range-setendafter
WebIDL::ExceptionOr<void> Range::set_end_after(Node& node)
{
    // 1. Let parent be node’s parent.
    auto* parent = node.parent();

    // 2. If parent is null, then throw an "InvalidNodeTypeError" DOMException.
    if (!parent)
        return WebIDL::InvalidNodeTypeError::create(realm(), "Given node has no parent."_string);

    // 3. Set the end of this to boundary point (parent, node’s index plus 1).
    return set_start_or_end(*parent, node.index() + 1, StartOrEnd::End);
}

// https://dom.spec.whatwg.org/#dom-range-compareboundarypoints
WebIDL::ExceptionOr<WebIDL::Short> Range::compare_boundary_points(WebIDL::UnsignedShort how, Range const& source_range) const
{
    // 1. If how is not one of
    //      - START_TO_START,
    //      - START_TO_END,
    //      - END_TO_END, and
    //      - END_TO_START,
    //    then throw a "NotSupportedError" DOMException.
    if (how != HowToCompareBoundaryPoints::START_TO_START && how != HowToCompareBoundaryPoints::START_TO_END && how != HowToCompareBoundaryPoints::END_TO_END && how != HowToCompareBoundaryPoints::END_TO_START)
        return WebIDL::NotSupportedError::create(realm(), MUST(String::formatted("Expected 'how' to be one of START_TO_START (0), START_TO_END (1), END_TO_END (2) or END_TO_START (3), got {}", how)));

    // 2. If this’s root is not the same as sourceRange’s root, then throw a "WrongDocumentError" DOMException.
    if (&root() != &source_range.root())
        return WebIDL::WrongDocumentError::create(realm(), "This range is not in the same tree as the source range."_string);

    JS::GCPtr<Node> this_point_node;
    u32 this_point_offset = 0;

    JS::GCPtr<Node> other_point_node;
    u32 other_point_offset = 0;

    // 3. If how is:
    switch (how) {
    case HowToCompareBoundaryPoints::START_TO_START:
        // -> START_TO_START:
        //    Let this point be this’s start. Let other point be sourceRange’s start.
        this_point_node = m_start_container;
        this_point_offset = m_start_offset;

        other_point_node = source_range.m_start_container;
        other_point_offset = source_range.m_start_offset;
        break;
    case HowToCompareBoundaryPoints::START_TO_END:
        // -> START_TO_END:
        //    Let this point be this’s end. Let other point be sourceRange’s start.
        this_point_node = m_end_container;
        this_point_offset = m_end_offset;

        other_point_node = source_range.m_start_container;
        other_point_offset = source_range.m_start_offset;
        break;
    case HowToCompareBoundaryPoints::END_TO_END:
        // -> END_TO_END:
        //    Let this point be this’s end. Let other point be sourceRange’s end.
        this_point_node = m_end_container;
        this_point_offset = m_end_offset;

        other_point_node = source_range.m_end_container;
        other_point_offset = source_range.m_end_offset;
        break;
    case HowToCompareBoundaryPoints::END_TO_START:
        // -> END_TO_START:
        //    Let this point be this’s start. Let other point be sourceRange’s end.
        this_point_node = m_start_container;
        this_point_offset = m_start_offset;

        other_point_node = source_range.m_end_container;
        other_point_offset = source_range.m_end_offset;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    VERIFY(this_point_node);
    VERIFY(other_point_node);

    // 4. If the position of this point relative to other point is
    auto relative_position = position_of_boundary_point_relative_to_other_boundary_point(*this_point_node, this_point_offset, *other_point_node, other_point_offset);
    switch (relative_position) {
    case RelativeBoundaryPointPosition::Before:
        // -> before
        //    Return −1.
        return -1;
    case RelativeBoundaryPointPosition::Equal:
        // -> equal
        //    Return 0.
        return 0;
    case RelativeBoundaryPointPosition::After:
        // -> after
        //    Return 1.
        return 1;
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://dom.spec.whatwg.org/#concept-range-select
WebIDL::ExceptionOr<void> Range::select(Node& node)
{
    // 1. Let parent be node’s parent.
    auto* parent = node.parent();

    // 2. If parent is null, then throw an "InvalidNodeTypeError" DOMException.
    if (!parent)
        return WebIDL::InvalidNodeTypeError::create(realm(), "Given node has no parent."_string);

    // 3. Let index be node’s index.
    auto index = node.index();

    // 4. Set range’s start to boundary point (parent, index).
    m_start_container = *parent;
    m_start_offset = index;

    // 5. Set range’s end to boundary point (parent, index plus 1).
    m_end_container = *parent;
    m_end_offset = index + 1;

    update_associated_selection();
    return {};
}

// https://dom.spec.whatwg.org/#dom-range-selectnode
WebIDL::ExceptionOr<void> Range::select_node(Node& node)
{
    // The selectNode(node) method steps are to select node within this.
    return select(node);
}

// https://dom.spec.whatwg.org/#dom-range-collapse
void Range::collapse(bool to_start)
{
    // The collapse(toStart) method steps are to, if toStart is true, set end to start; otherwise set start to end.
    if (to_start) {
        m_end_container = m_start_container;
        m_end_offset = m_start_offset;
    } else {
        m_start_container = m_end_container;
        m_start_offset = m_end_offset;
    }
    update_associated_selection();
}

// https://dom.spec.whatwg.org/#dom-range-selectnodecontents
WebIDL::ExceptionOr<void> Range::select_node_contents(Node& node)
{
    // 1. If node is a doctype, throw an "InvalidNodeTypeError" DOMException.
    if (is<DocumentType>(node))
        return WebIDL::InvalidNodeTypeError::create(realm(), "Node cannot be a DocumentType."_string);

    // 2. Let length be the length of node.
    auto length = node.length();

    // 3. Set start to the boundary point (node, 0).
    m_start_container = node;
    m_start_offset = 0;

    // 4. Set end to the boundary point (node, length).
    m_end_container = node;
    m_end_offset = length;

    update_associated_selection();
    return {};
}

JS::NonnullGCPtr<Range> Range::clone_range() const
{
    return heap().allocate<Range>(shape().realm(), const_cast<Node&>(*m_start_container), m_start_offset, const_cast<Node&>(*m_end_container), m_end_offset);
}

JS::NonnullGCPtr<Range> Range::inverted() const
{
    return heap().allocate<Range>(shape().realm(), const_cast<Node&>(*m_end_container), m_end_offset, const_cast<Node&>(*m_start_container), m_start_offset);
}

JS::NonnullGCPtr<Range> Range::normalized() const
{
    if (m_start_container.ptr() == m_end_container.ptr()) {
        if (m_start_offset <= m_end_offset)
            return clone_range();

        return inverted();
    }

    if (m_start_container->is_before(m_end_container))
        return clone_range();

    return inverted();
}

// https://dom.spec.whatwg.org/#dom-range-commonancestorcontainer
JS::NonnullGCPtr<Node> Range::common_ancestor_container() const
{
    // 1. Let container be start node.
    auto container = m_start_container;

    // 2. While container is not an inclusive ancestor of end node, let container be container’s parent.
    while (!container->is_inclusive_ancestor_of(m_end_container)) {
        VERIFY(container->parent());
        container = *container->parent();
    }

    // 3. Return container.
    return container;
}

// https://dom.spec.whatwg.org/#dom-range-intersectsnode
bool Range::intersects_node(Node const& node) const
{
    // 1. If node’s root is different from this’s root, return false.
    if (&node.root() != &root())
        return false;

    // 2. Let parent be node’s parent.
    auto* parent = node.parent();

    // 3. If parent is null, return true.
    if (!parent)
        return true;

    // 4. Let offset be node’s index.
    auto offset = node.index();

    // 5. If (parent, offset) is before end and (parent, offset plus 1) is after start, return true
    auto relative_position_to_end = position_of_boundary_point_relative_to_other_boundary_point(*parent, offset, m_end_container, m_end_offset);
    auto relative_position_to_start = position_of_boundary_point_relative_to_other_boundary_point(*parent, offset + 1, m_start_container, m_start_offset);
    if (relative_position_to_end == RelativeBoundaryPointPosition::Before && relative_position_to_start == RelativeBoundaryPointPosition::After)
        return true;

    // 6. Return false.
    return false;
}

// https://dom.spec.whatwg.org/#dom-range-ispointinrange
WebIDL::ExceptionOr<bool> Range::is_point_in_range(Node const& node, WebIDL::UnsignedLong offset) const
{
    // 1. If node’s root is different from this’s root, return false.
    if (&node.root() != &root())
        return false;

    // 2. If node is a doctype, then throw an "InvalidNodeTypeError" DOMException.
    if (is<DocumentType>(node))
        return WebIDL::InvalidNodeTypeError::create(realm(), "Node cannot be a DocumentType."_string);

    // 3. If offset is greater than node’s length, then throw an "IndexSizeError" DOMException.
    if (offset > node.length())
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Node does not contain a child at offset {}", offset)));

    // 4. If (node, offset) is before start or after end, return false.
    auto relative_position_to_start = position_of_boundary_point_relative_to_other_boundary_point(node, offset, m_start_container, m_start_offset);
    auto relative_position_to_end = position_of_boundary_point_relative_to_other_boundary_point(node, offset, m_end_container, m_end_offset);
    if (relative_position_to_start == RelativeBoundaryPointPosition::Before || relative_position_to_end == RelativeBoundaryPointPosition::After)
        return false;

    // 5. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#dom-range-comparepoint
WebIDL::ExceptionOr<WebIDL::Short> Range::compare_point(Node const& node, WebIDL::UnsignedLong offset) const
{
    // 1. If node’s root is different from this’s root, then throw a "WrongDocumentError" DOMException.
    if (&node.root() != &root())
        return WebIDL::WrongDocumentError::create(realm(), "Given node is not in the same document as the range."_string);

    // 2. If node is a doctype, then throw an "InvalidNodeTypeError" DOMException.
    if (is<DocumentType>(node))
        return WebIDL::InvalidNodeTypeError::create(realm(), "Node cannot be a DocumentType."_string);

    // 3. If offset is greater than node’s length, then throw an "IndexSizeError" DOMException.
    if (offset > node.length())
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Node does not contain a child at offset {}", offset)));

    // 4. If (node, offset) is before start, return −1.
    auto relative_position_to_start = position_of_boundary_point_relative_to_other_boundary_point(node, offset, m_start_container, m_start_offset);
    if (relative_position_to_start == RelativeBoundaryPointPosition::Before)
        return -1;

    // 5. If (node, offset) is after end, return 1.
    auto relative_position_to_end = position_of_boundary_point_relative_to_other_boundary_point(node, offset, m_end_container, m_end_offset);
    if (relative_position_to_end == RelativeBoundaryPointPosition::After)
        return 1;

    // 6. Return 0.
    return 0;
}

// https://dom.spec.whatwg.org/#dom-range-stringifier
String Range::to_string() const
{
    // 1. Let s be the empty string.
    StringBuilder builder;

    // 2. If this’s start node is this’s end node and it is a Text node,
    //    then return the substring of that Text node’s data beginning at this’s start offset and ending at this’s end offset.
    if (start_container() == end_container() && is<Text>(*start_container())) {
        auto const& text = static_cast<Text const&>(*start_container());
        return MUST(text.substring_data(start_offset(), end_offset() - start_offset()));
    }

    // 3. If this’s start node is a Text node, then append the substring of that node’s data from this’s start offset until the end to s.
    if (is<Text>(*start_container())) {
        auto const& text = static_cast<Text const&>(*start_container());
        builder.append(MUST(text.substring_data(start_offset(), text.length_in_utf16_code_units() - start_offset())));
    }

    // 4. Append the concatenation of the data of all Text nodes that are contained in this, in tree order, to s.
    for (Node const* node = start_container(); node != end_container()->next_sibling(); node = node->next_in_pre_order()) {
        if (is<Text>(*node) && contains_node(*node))
            builder.append(static_cast<Text const&>(*node).data());
    }

    // 5. If this’s end node is a Text node, then append the substring of that node’s data from its start until this’s end offset to s.
    if (is<Text>(*end_container())) {
        auto const& text = static_cast<Text const&>(*end_container());
        builder.append(MUST(text.substring_data(0, end_offset())));
    }

    // 6. Return s.
    return MUST(builder.to_string());
}

// https://dom.spec.whatwg.org/#dom-range-extractcontents
WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentFragment>> Range::extract_contents()
{
    return extract();
}

// https://dom.spec.whatwg.org/#concept-range-extract
WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentFragment>> Range::extract()
{
    // 1. Let fragment be a new DocumentFragment node whose node document is range’s start node’s node document.
    auto fragment = heap().allocate<DOM::DocumentFragment>(realm(), const_cast<Document&>(start_container()->document()));

    // 2. If range is collapsed, then return fragment.
    if (collapsed())
        return fragment;

    // 3. Let original start node, original start offset, original end node, and original end offset
    //    be range’s start node, start offset, end node, and end offset, respectively.
    JS::NonnullGCPtr<Node> original_start_node = m_start_container;
    auto original_start_offset = m_start_offset;
    JS::NonnullGCPtr<Node> original_end_node = m_end_container;
    auto original_end_offset = m_end_offset;

    // 4. If original start node is original end node and it is a CharacterData node, then:
    if (original_start_node.ptr() == original_end_node.ptr() && is<CharacterData>(*original_start_node)) {
        // 1. Let clone be a clone of original start node.
        auto clone = TRY(original_start_node->clone_node());

        // 2. Set the data of clone to the result of substringing data with node original start node,
        //    offset original start offset, and count original end offset minus original start offset.
        auto result = TRY(static_cast<CharacterData const&>(*original_start_node).substring_data(original_start_offset, original_end_offset - original_start_offset));
        verify_cast<CharacterData>(*clone).set_data(move(result));

        // 3. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 4. Replace data with node original start node, offset original start offset, count original end offset minus original start offset, and data the empty string.
        TRY(static_cast<CharacterData&>(*original_start_node).replace_data(original_start_offset, original_end_offset - original_start_offset, String {}));

        // 5. Return fragment.
        return fragment;
    }

    // 5. Let common ancestor be original start node.
    JS::NonnullGCPtr<Node> common_ancestor = original_start_node;

    // 6. While common ancestor is not an inclusive ancestor of original end node, set common ancestor to its own parent.
    while (!common_ancestor->is_inclusive_ancestor_of(original_end_node))
        common_ancestor = *common_ancestor->parent_node();

    // 7. Let first partially contained child be null.
    JS::GCPtr<Node> first_partially_contained_child;

    // 8. If original start node is not an inclusive ancestor of original end node,
    //    set first partially contained child to the first child of common ancestor that is partially contained in range.
    if (!original_start_node->is_inclusive_ancestor_of(original_end_node)) {
        for (auto* child = common_ancestor->first_child(); child; child = child->next_sibling()) {
            if (partially_contains_node(*child)) {
                first_partially_contained_child = child;
                break;
            }
        }
    }

    // 9. Let last partially contained child be null.
    JS::GCPtr<Node> last_partially_contained_child;

    // 10. If original end node is not an inclusive ancestor of original start node,
    //     set last partially contained child to the last child of common ancestor that is partially contained in range.
    if (!original_end_node->is_inclusive_ancestor_of(original_start_node)) {
        for (auto* child = common_ancestor->last_child(); child; child = child->previous_sibling()) {
            if (partially_contains_node(*child)) {
                last_partially_contained_child = child;
                break;
            }
        }
    }

    // 11. Let contained children be a list of all children of common ancestor that are contained in range, in tree order.
    Vector<JS::NonnullGCPtr<Node>> contained_children;
    for (Node* node = common_ancestor->first_child(); node; node = node->next_sibling()) {
        if (contains_node(*node))
            contained_children.append(*node);
    }

    // 12. If any member of contained children is a doctype, then throw a "HierarchyRequestError" DOMException.
    for (auto const& child : contained_children) {
        if (is<DocumentType>(*child))
            return WebIDL::HierarchyRequestError::create(realm(), "Contained child is a DocumentType"_string);
    }

    JS::GCPtr<Node> new_node;
    size_t new_offset = 0;

    // 13. If original start node is an inclusive ancestor of original end node, set new node to original start node and new offset to original start offset.
    if (original_start_node->is_inclusive_ancestor_of(original_end_node)) {
        new_node = original_start_node;
        new_offset = original_start_offset;
    }
    // 14. Otherwise:
    else {
        // 1. Let reference node equal original start node.
        JS::GCPtr<Node> reference_node = original_start_node;

        // 2. While reference node’s parent is not null and is not an inclusive ancestor of original end node, set reference node to its parent.
        while (reference_node->parent_node() && !reference_node->parent_node()->is_inclusive_ancestor_of(original_end_node))
            reference_node = reference_node->parent_node();

        // 3. Set new node to the parent of reference node, and new offset to one plus reference node’s index.
        new_node = reference_node->parent_node();
        new_offset = 1 + reference_node->index();
    }

    // 15. If first partially contained child is a CharacterData node, then:
    if (first_partially_contained_child && is<CharacterData>(*first_partially_contained_child)) {
        // 1. Let clone be a clone of original start node.
        auto clone = TRY(original_start_node->clone_node());

        // 2. Set the data of clone to the result of substringing data with node original start node, offset original start offset,
        //    and count original start node’s length minus original start offset.
        auto result = TRY(static_cast<CharacterData const&>(*original_start_node).substring_data(original_start_offset, original_start_node->length() - original_start_offset));
        verify_cast<CharacterData>(*clone).set_data(move(result));

        // 3. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 4. Replace data with node original start node, offset original start offset, count original start node’s length minus original start offset, and data the empty string.
        TRY(static_cast<CharacterData&>(*original_start_node).replace_data(original_start_offset, original_start_node->length() - original_start_offset, String {}));
    }
    // 16. Otherwise, if first partially contained child is not null:
    else if (first_partially_contained_child) {
        // 1. Let clone be a clone of first partially contained child.
        auto clone = TRY(first_partially_contained_child->clone_node());

        // 2. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 3. Let subrange be a new live range whose start is (original start node, original start offset) and whose end is (first partially contained child, first partially contained child’s length).
        auto subrange = Range::create(original_start_node, original_start_offset, *first_partially_contained_child, first_partially_contained_child->length());

        // 4. Let subfragment be the result of extracting subrange.
        auto subfragment = TRY(subrange->extract());

        // 5. Append subfragment to clone.
        TRY(clone->append_child(subfragment));
    }

    // 17. For each contained child in contained children, append contained child to fragment.
    for (auto& contained_child : contained_children) {
        TRY(fragment->append_child(contained_child));
    }

    // 18. If last partially contained child is a CharacterData node, then:
    if (last_partially_contained_child && is<CharacterData>(*last_partially_contained_child)) {
        // 1. Let clone be a clone of original end node.
        auto clone = TRY(original_end_node->clone_node());

        // 2. Set the data of clone to the result of substringing data with node original end node, offset 0, and count original end offset.
        auto result = TRY(static_cast<CharacterData const&>(*original_end_node).substring_data(0, original_end_offset));
        verify_cast<CharacterData>(*clone).set_data(move(result));

        // 3. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 4. Replace data with node original end node, offset 0, count original end offset, and data the empty string.
        TRY(verify_cast<CharacterData>(*original_end_node).replace_data(0, original_end_offset, String {}));
    }
    // 19. Otherwise, if last partially contained child is not null:
    else if (last_partially_contained_child) {
        // 1. Let clone be a clone of last partially contained child.
        auto clone = TRY(last_partially_contained_child->clone_node());

        // 2. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 3. Let subrange be a new live range whose start is (last partially contained child, 0) and whose end is (original end node, original end offset).
        auto subrange = Range::create(*last_partially_contained_child, 0, original_end_node, original_end_offset);

        // 4. Let subfragment be the result of extracting subrange.
        auto subfragment = TRY(subrange->extract());

        // 5. Append subfragment to clone.
        TRY(clone->append_child(subfragment));
    }

    // 20. Set range’s start and end to (new node, new offset).
    TRY(set_start(*new_node, new_offset));
    TRY(set_end(*new_node, new_offset));

    // 21. Return fragment.
    return fragment;
}

// https://dom.spec.whatwg.org/#contained
bool Range::contains_node(Node const& node) const
{
    // A node node is contained in a live range range if node’s root is range’s root,
    if (&node.root() != &root())
        return false;

    // and (node, 0) is after range’s start,
    if (position_of_boundary_point_relative_to_other_boundary_point(node, 0, m_start_container, m_start_offset) != RelativeBoundaryPointPosition::After)
        return false;

    // and (node, node’s length) is before range’s end.
    if (position_of_boundary_point_relative_to_other_boundary_point(node, node.length(), m_end_container, m_end_offset) != RelativeBoundaryPointPosition::Before)
        return false;

    return true;
}

// https://dom.spec.whatwg.org/#partially-contained
bool Range::partially_contains_node(Node const& node) const
{
    // A node is partially contained in a live range if it’s an inclusive ancestor of the live range’s start node but not its end node, or vice versa.
    if (node.is_inclusive_ancestor_of(m_start_container) && &node != m_end_container.ptr())
        return true;
    if (node.is_inclusive_ancestor_of(m_end_container) && &node != m_start_container.ptr())
        return true;
    return false;
}

// https://dom.spec.whatwg.org/#dom-range-insertnode
WebIDL::ExceptionOr<void> Range::insert_node(JS::NonnullGCPtr<Node> node)
{
    return insert(node);
}

// https://dom.spec.whatwg.org/#concept-range-insert
WebIDL::ExceptionOr<void> Range::insert(JS::NonnullGCPtr<Node> node)
{
    // 1. If range’s start node is a ProcessingInstruction or Comment node, is a Text node whose parent is null, or is node, then throw a "HierarchyRequestError" DOMException.
    if ((is<ProcessingInstruction>(*m_start_container) || is<Comment>(*m_start_container))
        || (is<Text>(*m_start_container) && !m_start_container->parent_node())
        || m_start_container.ptr() == node.ptr()) {
        return WebIDL::HierarchyRequestError::create(realm(), "Range has inappropriate start node for insertion"_string);
    }

    // 2. Let referenceNode be null.
    JS::GCPtr<Node> reference_node;

    // 3. If range’s start node is a Text node, set referenceNode to that Text node.
    if (is<Text>(*m_start_container)) {
        reference_node = m_start_container;
    }
    // 4. Otherwise, set referenceNode to the child of start node whose index is start offset, and null if there is no such child.
    else {
        reference_node = m_start_container->child_at_index(m_start_offset);
    }

    // 5. Let parent be range’s start node if referenceNode is null, and referenceNode’s parent otherwise.
    JS::GCPtr<Node> parent;
    if (!reference_node)
        parent = m_start_container;
    else
        parent = reference_node->parent();

    // 6. Ensure pre-insertion validity of node into parent before referenceNode.
    TRY(parent->ensure_pre_insertion_validity(node, reference_node));

    // 7. If range’s start node is a Text node, set referenceNode to the result of splitting it with offset range’s start offset.
    if (is<Text>(*m_start_container))
        reference_node = TRY(static_cast<Text&>(*m_start_container).split_text(m_start_offset));

    // 8. If node is referenceNode, set referenceNode to its next sibling.
    if (node == reference_node)
        reference_node = reference_node->next_sibling();

    // 9. If node’s parent is non-null, then remove node.
    if (node->parent())
        node->remove();

    // 10. Let newOffset be parent’s length if referenceNode is null, and referenceNode’s index otherwise.
    size_t new_offset = 0;
    if (!reference_node)
        new_offset = parent->length();
    else
        new_offset = reference_node->index();

    // 11. Increase newOffset by node’s length if node is a DocumentFragment node, and one otherwise.
    if (is<DocumentFragment>(*node))
        new_offset += node->length();
    else
        new_offset += 1;

    // 12. Pre-insert node into parent before referenceNode.
    (void)TRY(parent->pre_insert(node, reference_node));

    // 13. If range is collapsed, then set range’s end to (parent, newOffset).
    if (collapsed())
        TRY(set_end(*parent, new_offset));

    return {};
}

// https://dom.spec.whatwg.org/#dom-range-surroundcontents
WebIDL::ExceptionOr<void> Range::surround_contents(JS::NonnullGCPtr<Node> new_parent)
{
    // 1. If a non-Text node is partially contained in this, then throw an "InvalidStateError" DOMException.
    Node* start_non_text_node = start_container();
    if (is<Text>(*start_non_text_node))
        start_non_text_node = start_non_text_node->parent_node();
    Node* end_non_text_node = end_container();
    if (is<Text>(*end_non_text_node))
        end_non_text_node = end_non_text_node->parent_node();
    if (start_non_text_node != end_non_text_node)
        return WebIDL::InvalidStateError::create(realm(), "Non-Text node is partially contained in range."_string);

    // 2. If newParent is a Document, DocumentType, or DocumentFragment node, then throw an "InvalidNodeTypeError" DOMException.
    if (is<Document>(*new_parent) || is<DocumentType>(*new_parent) || is<DocumentFragment>(*new_parent))
        return WebIDL::InvalidNodeTypeError::create(realm(), "Invalid parent node type"_string);

    // 3. Let fragment be the result of extracting this.
    auto fragment = TRY(extract());

    // 4. If newParent has children, then replace all with null within newParent.
    if (new_parent->has_children())
        new_parent->replace_all(nullptr);

    // 5. Insert newParent into this.
    TRY(insert(new_parent));

    // 6. Append fragment to newParent.
    (void)TRY(new_parent->append_child(fragment));

    // 7. Select newParent within this.
    return select(*new_parent);
}

// https://dom.spec.whatwg.org/#dom-range-clonecontents
WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentFragment>> Range::clone_contents()
{
    return clone_the_contents();
}

// https://dom.spec.whatwg.org/#concept-range-clone
WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentFragment>> Range::clone_the_contents()
{
    // 1. Let fragment be a new DocumentFragment node whose node document is range’s start node’s node document.
    auto fragment = heap().allocate<DOM::DocumentFragment>(realm(), const_cast<Document&>(start_container()->document()));

    // 2. If range is collapsed, then return fragment.
    if (collapsed())
        return fragment;

    // 3. Let original start node, original start offset, original end node, and original end offset
    //    be range’s start node, start offset, end node, and end offset, respectively.
    JS::NonnullGCPtr<Node> original_start_node = m_start_container;
    auto original_start_offset = m_start_offset;
    JS::NonnullGCPtr<Node> original_end_node = m_end_container;
    auto original_end_offset = m_end_offset;

    // 4. If original start node is original end node and it is a CharacterData node, then:
    if (original_start_node.ptr() == original_end_node.ptr() && is<CharacterData>(*original_start_node)) {
        // 1. Let clone be a clone of original start node.
        auto clone = TRY(original_start_node->clone_node());

        // 2. Set the data of clone to the result of substringing data with node original start node,
        //    offset original start offset, and count original end offset minus original start offset.
        auto result = TRY(static_cast<CharacterData const&>(*original_start_node).substring_data(original_start_offset, original_end_offset - original_start_offset));
        verify_cast<CharacterData>(*clone).set_data(move(result));

        // 3. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 4. Return fragment.
        return fragment;
    }

    // 5. Let common ancestor be original start node.
    JS::NonnullGCPtr<Node> common_ancestor = original_start_node;

    // 6. While common ancestor is not an inclusive ancestor of original end node, set common ancestor to its own parent.
    while (!common_ancestor->is_inclusive_ancestor_of(original_end_node))
        common_ancestor = *common_ancestor->parent_node();

    // 7. Let first partially contained child be null.
    JS::GCPtr<Node> first_partially_contained_child;

    // 8. If original start node is not an inclusive ancestor of original end node,
    //    set first partially contained child to the first child of common ancestor that is partially contained in range.
    if (!original_start_node->is_inclusive_ancestor_of(original_end_node)) {
        for (auto* child = common_ancestor->first_child(); child; child = child->next_sibling()) {
            if (partially_contains_node(*child)) {
                first_partially_contained_child = child;
                break;
            }
        }
    }

    // 9. Let last partially contained child be null.
    JS::GCPtr<Node> last_partially_contained_child;

    // 10. If original end node is not an inclusive ancestor of original start node,
    //     set last partially contained child to the last child of common ancestor that is partially contained in range.
    if (!original_end_node->is_inclusive_ancestor_of(original_start_node)) {
        for (auto* child = common_ancestor->last_child(); child; child = child->previous_sibling()) {
            if (partially_contains_node(*child)) {
                last_partially_contained_child = child;
                break;
            }
        }
    }

    // 11. Let contained children be a list of all children of common ancestor that are contained in range, in tree order.
    Vector<JS::NonnullGCPtr<Node>> contained_children;
    for (Node* node = common_ancestor->first_child(); node; node = node->next_sibling()) {
        if (contains_node(*node))
            contained_children.append(*node);
    }

    // 12. If any member of contained children is a doctype, then throw a "HierarchyRequestError" DOMException.
    for (auto const& child : contained_children) {
        if (is<DocumentType>(*child))
            return WebIDL::HierarchyRequestError::create(realm(), "Contained child is a DocumentType"_string);
    }

    // 13. If first partially contained child is a CharacterData node, then:
    if (first_partially_contained_child && is<CharacterData>(*first_partially_contained_child)) {
        // 1. Let clone be a clone of original start node.
        auto clone = TRY(original_start_node->clone_node());

        // 2. Set the data of clone to the result of substringing data with node original start node, offset original start offset,
        //    and count original start node’s length minus original start offset.
        auto result = TRY(static_cast<CharacterData const&>(*original_start_node).substring_data(original_start_offset, original_start_node->length() - original_start_offset));
        verify_cast<CharacterData>(*clone).set_data(move(result));

        // 3. Append clone to fragment.
        TRY(fragment->append_child(clone));
    }
    // 14. Otherwise, if first partially contained child is not null:
    else if (first_partially_contained_child) {
        // 1. Let clone be a clone of first partially contained child.
        auto clone = TRY(first_partially_contained_child->clone_node());

        // 2. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 3. Let subrange be a new live range whose start is (original start node, original start offset) and whose end is (first partially contained child, first partially contained child’s length).
        auto subrange = Range::create(original_start_node, original_start_offset, *first_partially_contained_child, first_partially_contained_child->length());

        // 4. Let subfragment be the result of cloning the contents of subrange.
        auto subfragment = TRY(subrange->clone_the_contents());

        // 5. Append subfragment to clone.
        TRY(clone->append_child(subfragment));
    }

    // 15. For each contained child in contained children.
    for (auto& contained_child : contained_children) {
        // 1. Let clone be a clone of contained child with the clone children flag set.
        auto clone = TRY(contained_child->clone_node(nullptr, true));

        // 2. Append clone to fragment.
        TRY(fragment->append_child(move(clone)));
    }

    // 16. If last partially contained child is a CharacterData node, then:
    if (last_partially_contained_child && is<CharacterData>(*last_partially_contained_child)) {
        // 1. Let clone be a clone of original end node.
        auto clone = TRY(original_end_node->clone_node());

        // 2. Set the data of clone to the result of substringing data with node original end node, offset 0, and count original end offset.
        auto result = TRY(static_cast<CharacterData const&>(*original_end_node).substring_data(0, original_end_offset));
        verify_cast<CharacterData>(*clone).set_data(move(result));

        // 3. Append clone to fragment.
        TRY(fragment->append_child(clone));
    }
    // 17. Otherwise, if last partially contained child is not null:
    else if (last_partially_contained_child) {
        // 1. Let clone be a clone of last partially contained child.
        auto clone = TRY(last_partially_contained_child->clone_node());

        // 2. Append clone to fragment.
        TRY(fragment->append_child(clone));

        // 3. Let subrange be a new live range whose start is (last partially contained child, 0) and whose end is (original end node, original end offset).
        auto subrange = Range::create(*last_partially_contained_child, 0, original_end_node, original_end_offset);

        // 4. Let subfragment be the result of cloning the contents of subrange.
        auto subfragment = TRY(subrange->clone_the_contents());

        // 5. Append subfragment to clone.
        TRY(clone->append_child(subfragment));
    }

    // 18. Return fragment.
    return fragment;
}

// https://dom.spec.whatwg.org/#dom-range-deletecontents
WebIDL::ExceptionOr<void> Range::delete_contents()
{
    // 1. If this is collapsed, then return.
    if (collapsed())
        return {};

    // 2. Let original start node, original start offset, original end node, and original end offset be this’s start node, start offset, end node, and end offset, respectively.
    JS::NonnullGCPtr<Node> original_start_node = m_start_container;
    auto original_start_offset = m_start_offset;
    JS::NonnullGCPtr<Node> original_end_node = m_end_container;
    auto original_end_offset = m_end_offset;

    // 3. If original start node is original end node and it is a CharacterData node, then replace data with node original start node, offset original start offset,
    //    count original end offset minus original start offset, and data the empty string, and then return.
    if (original_start_node.ptr() == original_end_node.ptr() && is<CharacterData>(*original_start_node)) {
        TRY(static_cast<CharacterData&>(*original_start_node).replace_data(original_start_offset, original_end_offset - original_start_offset, String {}));
        return {};
    }

    // 4. Let nodes to remove be a list of all the nodes that are contained in this, in tree order, omitting any node whose parent is also contained in this.
    JS::MarkedVector<Node*> nodes_to_remove(heap());
    for (Node const* node = start_container(); node != end_container()->next_sibling(); node = node->next_in_pre_order()) {
        if (contains_node(*node) && (!node->parent_node() || !contains_node(*node->parent_node())))
            nodes_to_remove.append(const_cast<Node*>(node));
    }

    JS::GCPtr<Node> new_node;
    size_t new_offset = 0;

    // 5. If original start node is an inclusive ancestor of original end node, set new node to original start node and new offset to original start offset.
    if (original_start_node->is_inclusive_ancestor_of(original_end_node)) {
        new_node = original_start_node;
        new_offset = original_start_offset;
    }
    // 6. Otherwise
    else {
        // 1. Let reference node equal original start node.
        auto reference_node = original_start_node;

        // 2. While reference node’s parent is not null and is not an inclusive ancestor of original end node, set reference node to its parent.
        while (reference_node->parent_node() && !reference_node->parent_node()->is_inclusive_ancestor_of(original_end_node))
            reference_node = *reference_node->parent_node();

        // 3. Set new node to the parent of reference node, and new offset to one plus the index of reference node.
        new_node = reference_node->parent_node();
        new_offset = 1 + reference_node->index();
    }

    // 7. If original start node is a CharacterData node, then replace data with node original start node, offset original start offset, count original start node’s length minus original start offset, data the empty string.
    if (is<CharacterData>(*original_start_node))
        TRY(static_cast<CharacterData&>(*original_start_node).replace_data(original_start_offset, original_start_node->length() - original_start_offset, String {}));

    // 8. For each node in nodes to remove, in tree order, remove node.
    for (auto& node : nodes_to_remove)
        node->remove();

    // 9. If original end node is a CharacterData node, then replace data with node original end node, offset 0, count original end offset and data the empty string.
    if (is<CharacterData>(*original_end_node))
        TRY(static_cast<CharacterData&>(*original_end_node).replace_data(0, original_end_offset, String {}));

    // 10. Set start and end to (new node, new offset).
    TRY(set_start(*new_node, new_offset));
    TRY(set_end(*new_node, new_offset));
    return {};
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
// https://drafts.csswg.org/cssom-view/#extensions-to-the-range-interface
JS::NonnullGCPtr<Geometry::DOMRectList> Range::get_client_rects()
{
    // 1. return an empty DOMRectList object if the range is not in the document
    if (!start_container()->document().navigable())
        return Geometry::DOMRectList::create(realm(), {});

    start_container()->document().update_layout();
    update_associated_selection();
    Vector<JS::Handle<Geometry::DOMRect>> rects;
    // FIXME: take Range collapsed into consideration
    // 2. Iterate the node included in Range
    auto start_node = start_container();
    auto end_node = end_container();
    if (!is<DOM::Text>(start_node)) {
        start_node = start_node->child_at_index(m_start_offset);
    }
    if (!is<DOM::Text>(end_node)) {
        // end offset shouldn't be 0
        if (m_end_offset == 0)
            return Geometry::DOMRectList::create(realm(), {});
        end_node = end_node->child_at_index(m_end_offset - 1);
    }
    for (Node const* node = start_node; node && node != end_node->next_in_pre_order(); node = node->next_in_pre_order()) {
        auto node_type = static_cast<NodeType>(node->node_type());
        if (node_type == NodeType::ELEMENT_NODE) {
            // 1. For each element selected by the range, whose parent is not selected by the range, include the border
            // areas returned by invoking getClientRects() on the element.
            if (contains_node(*node) && !contains_node(*node->parent())) {
                auto const& element = static_cast<DOM::Element const&>(*node);
                JS::NonnullGCPtr<Geometry::DOMRectList> const element_rects = element.get_client_rects();
                for (u32 i = 0; i < element_rects->length(); i++) {
                    auto rect = element_rects->item(i);
                    rects.append(Geometry::DOMRect::create(realm(),
                        Gfx::FloatRect(rect->x(), rect->y(), rect->width(), rect->height())));
                }
            }
        } else if (node_type == NodeType::TEXT_NODE) {
            // 2. For each Text node selected or partially selected by the range (including when the boundary-points
            // are identical), include scaled DOMRect object (for the part that is selected, not the whole line box).
            auto const& text = static_cast<DOM::Text const&>(*node);
            auto const* paintable = text.paintable();
            if (paintable) {
                auto const* containing_block = paintable->containing_block();
                if (is<Painting::PaintableWithLines>(*containing_block)) {
                    auto const& paintable_lines = static_cast<Painting::PaintableWithLines const&>(*containing_block);
                    auto fragments = paintable_lines.fragments();
                    auto const& font = paintable->layout_node().first_available_font();
                    for (auto frag = fragments.begin(); frag != fragments.end(); frag++) {
                        auto rect = frag->range_rect(font, *this);
                        if (rect.is_empty())
                            continue;
                        rects.append(Geometry::DOMRect::create(realm(),
                            Gfx::FloatRect(rect)));
                    }
                } else {
                    dbgln("FIXME: Failed to get client rects for node {}", node->debug_description());
                }
            }
        }
    }
    return Geometry::DOMRectList::create(realm(), move(rects));
}

// https://w3c.github.io/csswg-drafts/cssom-view/#dom-range-getboundingclientrect
JS::NonnullGCPtr<Geometry::DOMRect> Range::get_bounding_client_rect()
{
    // 1. Let list be the result of invoking getClientRects() on element.
    auto list = get_client_rects();

    // 2. If the list is empty return a DOMRect object whose x, y, width and height members are zero.
    if (list->length() == 0)
        return Geometry::DOMRect::construct_impl(realm(), 0, 0, 0, 0).release_value_but_fixme_should_propagate_errors();

    // 3. If all rectangles in list have zero width or height, return the first rectangle in list.
    auto all_rectangle_has_zero_width_or_height = true;
    for (auto i = 0u; i < list->length(); ++i) {
        auto const& rect = list->item(i);
        if (rect->width() != 0 && rect->height() != 0) {
            all_rectangle_has_zero_width_or_height = false;
            break;
        }
    }
    if (all_rectangle_has_zero_width_or_height)
        return JS::NonnullGCPtr { *const_cast<Geometry::DOMRect*>(list->item(0)) };

    // 4. Otherwise, return a DOMRect object describing the smallest rectangle that includes all of the rectangles in
    //    list of which the height or width is not zero.
    auto const* first_rect = list->item(0);
    auto bounding_rect = Gfx::Rect { first_rect->x(), first_rect->y(), first_rect->width(), first_rect->height() };
    for (auto i = 1u; i < list->length(); ++i) {
        auto const& rect = list->item(i);
        if (rect->width() == 0 || rect->height() == 0)
            continue;
        bounding_rect = bounding_rect.united({ rect->x(), rect->y(), rect->width(), rect->height() });
    }
    return Geometry::DOMRect::create(realm(), bounding_rect.to_type<float>());
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-range-createcontextualfragment
WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentFragment>> Range::create_contextual_fragment(String const& string)
{
    // FIXME: Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, string, and "Range createContextualFragment".

    // 2. Let node be this's start node.
    JS::NonnullGCPtr<Node> node = *start_container();

    // 3. Let element be null.
    JS::GCPtr<Element> element = nullptr;

    auto node_type = static_cast<NodeType>(node->node_type());
    // 4. If node implements Element, set element to node.
    if (node_type == NodeType::ELEMENT_NODE)
        element = static_cast<DOM::Element&>(*node);
    // 5. Otherwise, if node implements Text or Comment node, set element to node's parent element.
    else if (first_is_one_of(node_type, NodeType::TEXT_NODE, NodeType::COMMENT_NODE))
        element = node->parent_element();

    // 6. If either element is null or all of the following are true:
    //    - element's node document is an HTML document,
    //    - element's local name is "html"; and
    //    - element's namespace is the HTML namespace;
    if (!element || is<HTML::HTMLHtmlElement>(*element)) {
        // then set element to the result of creating an element given this's node document,
        // body, and the HTML namespace.
        element = TRY(DOM::create_element(node->document(), HTML::TagNames::body, Namespace::HTML));
    }

    // 7. Let fragment node be the result of invoking the fragment parsing algorithm steps with element and compliantString. FIXME: Use compliantString.
    auto fragment_node = TRY(element->parse_fragment(string));

    // 8. For each script of fragment node's script element descendants:
    fragment_node->for_each_in_subtree_of_type<HTML::HTMLScriptElement>([&](HTML::HTMLScriptElement& script_element) {
        // 8.1 Set scripts already started to false.
        script_element.unmark_as_already_started({});
        // 8.2 Set scripts parser document to null.
        script_element.unmark_as_parser_inserted({});
        return TraversalDecision::Continue;
    });

    // 5. Return fragment node.
    return fragment_node;
}

void Range::increase_start_offset(Badge<Node>, WebIDL::UnsignedLong count)
{
    m_start_offset += count;
}

void Range::increase_end_offset(Badge<Node>, WebIDL::UnsignedLong count)
{
    m_end_offset += count;
}

void Range::decrease_start_offset(Badge<Node>, WebIDL::UnsignedLong count)
{
    m_start_offset -= count;
}

void Range::decrease_end_offset(Badge<Node>, WebIDL::UnsignedLong count)
{
    m_end_offset -= count;
}

}
