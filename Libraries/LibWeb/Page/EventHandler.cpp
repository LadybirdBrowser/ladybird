/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/GraphemeEdgeTracker.h>
#include <LibWeb/HTML/CloseWatcherManager.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/AutoScrollHandler.h>
#include <LibWeb/Page/DragAndDropEventHandler.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWeb/UIEvents/InputTypes.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/UIEvents/WheelEvent.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_joystick.h>

namespace Web {

#define FIRE(expression)                                                          \
    if (auto event_result = (expression); event_result == EventResult::Cancelled) \
        return event_result;

static GC::Ptr<DOM::Node> dom_node_for_event_dispatch(Painting::Paintable& paintable)
{
    if (auto node = paintable.dom_node())
        return node;
    auto* parent = paintable.parent();
    while (parent) {
        if (auto node = parent->dom_node())
            return node;
        parent = parent->parent();
    }
    return nullptr;
}

static bool parent_element_for_event_dispatch(Painting::Paintable& paintable, GC::Ptr<DOM::Node>& node, GC::Ptr<Layout::Node>& layout_node)
{
    layout_node = &paintable.layout_node();
    if (layout_node->is_generated_for_backdrop_pseudo_element()
        || layout_node->is_generated_for_after_pseudo_element()
        || layout_node->is_generated_for_before_pseudo_element()) {
        node = layout_node->pseudo_element_generator();
        layout_node = node->layout_node();
    }

    auto* current_ancestor_node = node.ptr();
    do {
        auto const* form_associated_element = as_if<HTML::FormAssociatedElement>(current_ancestor_node);
        if (form_associated_element && !form_associated_element->enabled()) {
            return false;
        }
    } while ((current_ancestor_node = current_ancestor_node->parent()));

    while (layout_node && node && !node->is_element() && layout_node->parent()) {
        layout_node = layout_node->parent();
        if (layout_node->is_anonymous())
            continue;
        node = layout_node->dom_node();
    }
    return node && layout_node;
}

static Gfx::Cursor css_to_gfx_cursor(CSS::CursorPredefined css_cursor)
{
    switch (css_cursor) {
    case CSS::CursorPredefined::Crosshair:
    case CSS::CursorPredefined::Cell:
        return Gfx::StandardCursor::Crosshair;
    case CSS::CursorPredefined::Grab:
        return Gfx::StandardCursor::OpenHand;
    case CSS::CursorPredefined::Grabbing:
        return Gfx::StandardCursor::Drag;
    case CSS::CursorPredefined::Pointer:
        return Gfx::StandardCursor::Hand;
    case CSS::CursorPredefined::Help:
        return Gfx::StandardCursor::Help;
    case CSS::CursorPredefined::None:
        return Gfx::StandardCursor::Hidden;
    case CSS::CursorPredefined::NotAllowed:
        return Gfx::StandardCursor::Disallowed;
    case CSS::CursorPredefined::Text:
    case CSS::CursorPredefined::VerticalText:
        return Gfx::StandardCursor::IBeam;
    case CSS::CursorPredefined::Move:
    case CSS::CursorPredefined::AllScroll:
        return Gfx::StandardCursor::Move;
    case CSS::CursorPredefined::Progress:
    case CSS::CursorPredefined::Wait:
        return Gfx::StandardCursor::Wait;
    case CSS::CursorPredefined::ColResize:
        return Gfx::StandardCursor::ResizeColumn;
    case CSS::CursorPredefined::EResize:
    case CSS::CursorPredefined::WResize:
    case CSS::CursorPredefined::EwResize:
        return Gfx::StandardCursor::ResizeHorizontal;
    case CSS::CursorPredefined::RowResize:
        return Gfx::StandardCursor::ResizeRow;
    case CSS::CursorPredefined::NResize:
    case CSS::CursorPredefined::SResize:
    case CSS::CursorPredefined::NsResize:
        return Gfx::StandardCursor::ResizeVertical;
    case CSS::CursorPredefined::NeResize:
    case CSS::CursorPredefined::SwResize:
    case CSS::CursorPredefined::NeswResize:
        return Gfx::StandardCursor::ResizeDiagonalBLTR;
    case CSS::CursorPredefined::NwResize:
    case CSS::CursorPredefined::SeResize:
    case CSS::CursorPredefined::NwseResize:
        return Gfx::StandardCursor::ResizeDiagonalTLBR;
    case CSS::CursorPredefined::ZoomIn:
    case CSS::CursorPredefined::ZoomOut:
        return Gfx::StandardCursor::Zoom;
    case CSS::CursorPredefined::Default:
        return Gfx::StandardCursor::Arrow;
    case CSS::CursorPredefined::ContextMenu:
    case CSS::CursorPredefined::Alias:
    case CSS::CursorPredefined::Copy:
    case CSS::CursorPredefined::NoDrop:
        // FIXME: No corresponding GFX Standard Cursor, fallthrough to None
    case CSS::CursorPredefined::Auto:
    default:
        return Gfx::StandardCursor::None;
    }
}

static Gfx::Cursor resolve_cursor(Layout::NodeWithStyle const& layout_node, Vector<CSS::CursorData> const& cursor_data, Gfx::StandardCursor auto_cursor)
{
    for (auto const& cursor : cursor_data) {
        auto result = cursor.visit(
            [auto_cursor](CSS::CursorPredefined css_cursor) -> Optional<Gfx::Cursor> {
                if (css_cursor == CSS::CursorPredefined::Auto)
                    return auto_cursor;
                return css_to_gfx_cursor(css_cursor);
            },
            [&layout_node](NonnullRefPtr<CSS::CursorStyleValue const> const& cursor_style_value) -> Optional<Gfx::Cursor> {
                if (auto image_cursor = cursor_style_value->make_image_cursor(layout_node); image_cursor.has_value())
                    return image_cursor.release_value();
                return {};
            });
        if (result.has_value())
            return result.release_value();
    }

    // We should never get here
    return Gfx::StandardCursor::None;
}

// https://drafts.csswg.org/cssom-view/#dom-mouseevent-offsetx
static CSSPixelPoint compute_mouse_event_offset(CSSPixelPoint position, Painting::Paintable const& paintable)
{
    // If the event’s dispatch flag is set,
    // FIXME: Is this guaranteed to be dispatched?

    // return the x-coordinate of the position where the event occurred,
    // ignoring the transforms that apply to the element and its ancestors,
    CSSPixelPoint offset_position = position;
    if (is<Painting::PaintableBox>(paintable)) {
        offset_position = static_cast<Painting::PaintableBox const&>(paintable).inverse_transform_point(position);
    } else if (auto* containing_block = paintable.containing_block()) {
        offset_position = containing_block->inverse_transform_point(position);
    }

    // relative to the origin of the padding edge of the target node
    auto const top_left_of_layout_node = paintable.box_type_agnostic_position();
    auto offset = offset_position - top_left_of_layout_node;

    // and terminate these steps.
    return offset;
}

static Optional<EventResult> dispatch_event_to_nested_navigable(Painting::Paintable& paintable, CSSPixelPoint viewport_position, Function<EventResult(EventHandler&, CSSPixelPoint)> dispatch)
{
    auto node = dom_node_for_event_dispatch(paintable);
    if (!node)
        return {};

    if (auto* navigable_paintable = as_if<Painting::NavigableContainerViewportPaintable>(paintable)) {
        auto position = navigable_paintable->transform_to_local_coordinates(viewport_position) - navigable_paintable->absolute_rect().location();
        if (auto content_navigable = as_if<HTML::NavigableContainer>(*node)->content_navigable()) {
            return dispatch(content_navigable->event_handler(), position);
        }
        return EventResult::Dropped;
    }

    return {};
}

// Find paragraph boundaries for triple-click selection. A paragraph is delimited by block nodes or <br> elements.
static GC::Ref<DOM::Range> find_paragraph_range(DOM::Text& text_node, WebIDL::UnsignedLong offset)
{
    GC::Ptr<DOM::Node> start_node = text_node;
    WebIDL::UnsignedLong start_offset = offset;
    GC::Ptr<DOM::Node> end_node = text_node;
    WebIDL::UnsignedLong end_offset = offset;

    // Walk backwards to find the paragraph start (a block boundary point).
    if (!Editing::is_block_start_point({ *start_node, start_offset })) {
        do {
            if (start_offset == 0) {
                start_offset = start_node->index();
                start_node = start_node->parent();
            } else {
                --start_offset;
            }
        } while (start_node && !Editing::is_block_boundary_point({ *start_node, start_offset }));
    }

    // Walk forwards to find the paragraph end (a block boundary point).
    if (!Editing::is_block_end_point({ *end_node, end_offset })) {
        do {
            if (end_offset == end_node->length()) {
                end_offset = end_node->index() + 1;
                end_node = end_node->parent();
            } else {
                ++end_offset;
            }
        } while (end_node && !Editing::is_block_boundary_point({ *end_node, end_offset }));
    }

    // Fallback if we couldn't find boundaries.
    if (!start_node) {
        start_node = text_node;
        start_offset = 0;
    }
    if (!end_node) {
        end_node = text_node;
        end_offset = text_node.length();
    }

    return DOM::Range::create(*start_node, start_offset, *end_node, end_offset);
}

// https://drafts.csswg.org/css-ui/#propdef-user-select
static void set_user_selection(GC::Ptr<DOM::Node> anchor_node, size_t anchor_offset, GC::Ptr<DOM::Node> focus_node, size_t focus_offset, Selection::Selection* selection, CSS::UserSelect user_select)
{
    // https://drafts.csswg.org/css-ui/#valdef-user-select-contain
    // NB: This is clamping the focus node to any node with user-select: contain that stands between it and the anchor node.
    if (focus_node != anchor_node) {
        // UAs must not allow a selection which is started in this element to be extended outside of this element.
        auto potential_contain_node = anchor_node;

        // NB: The way we do this is searching up the tree from the anchor, to find 'this element', i.e. its nearest
        //     contain ancestor. We stop the search early when we reach an element that contains both the anchor and the
        //     focus node, as this means they are inside the same contain element, or not in a contain element at all.
        //     This takes care of the "selection trying to escape from a contain" case.
        while (
            (!potential_contain_node->is_element() || potential_contain_node->layout_node()->user_select_used_value() != CSS::UserSelect::Contain) && potential_contain_node->parent() && !potential_contain_node->is_inclusive_ancestor_of(*focus_node)) {
            potential_contain_node = potential_contain_node->parent();
        }

        if (
            potential_contain_node->layout_node()->user_select_used_value() == CSS::UserSelect::Contain && !potential_contain_node->is_inclusive_ancestor_of(*focus_node)) {
            if (focus_node->is_before(*potential_contain_node)) {
                focus_offset = 0;
            } else {
                focus_offset = potential_contain_node->length();
            }
            focus_node = potential_contain_node;
            // NB: Prevents this from being handled again further down
            user_select = CSS::UserSelect::Contain;
        } else {
            // A selection started outside of this element must not end in this element. If the user attempts to create
            // such a selection, the UA must instead end the selection range at the element boundary.

            // NB: This branch takes care of the "selection trying to intrude into a contain" case. This is done by
            //     searching up the tree from the focus node, to see if there is a contain element between it and the
            //     common ancestor that also includes the anchor. We stop once reaching target_node, which is the common
            //     ancestor identified in step 1. If target_node wasn't a common ancestor, we would not be here.
            auto target_node = potential_contain_node;
            potential_contain_node = focus_node;
            while (
                (!potential_contain_node->is_element() || potential_contain_node->layout_node()->user_select_used_value() != CSS::UserSelect::Contain) && potential_contain_node->parent() && potential_contain_node != target_node) {
                potential_contain_node = potential_contain_node->parent();
            }
            if (
                potential_contain_node->layout_node()->user_select_used_value() == CSS::UserSelect::Contain && !potential_contain_node->is_inclusive_ancestor_of(*anchor_node)) {
                if (potential_contain_node->is_before(*anchor_node)) {
                    focus_node = potential_contain_node->next_in_pre_order();
                    while (potential_contain_node->is_inclusive_ancestor_of(*focus_node)) {
                        focus_node = focus_node->next_in_pre_order();
                    }
                    focus_offset = 0;
                } else {
                    focus_node = potential_contain_node->previous_in_pre_order();
                    while (potential_contain_node->is_inclusive_ancestor_of(*focus_node)) {
                        focus_node = focus_node->previous_in_pre_order();
                    }
                    focus_offset = focus_node->length();
                }
                // NB: Prevents this from being handled again further down
                user_select = CSS::UserSelect::Contain;
            }
        }
    }

    switch (user_select) {
    case CSS::UserSelect::None:
        // https://drafts.csswg.org/css-ui/#valdef-user-select-none

        // The UA must not allow selections to be started in this element.
        if (anchor_node == focus_node) {
            return;
        }

        // A selection started outside of this element must not end in this element. If the user attempts to create such
        // a selection, the UA must instead end the selection range at the element boundary.
        while (focus_node->parent() && focus_node->parent()->layout_node()->user_select_used_value() == CSS::UserSelect::None) {
            focus_node = focus_node->parent();
        }
        if (focus_node->is_before(*anchor_node)) {
            auto none_element = focus_node;
            do {
                focus_node = focus_node->next_in_pre_order();
            } while (none_element->is_inclusive_ancestor_of(*focus_node));
            focus_offset = 0;
        } else {
            focus_node = focus_node->previous_in_pre_order();
            focus_offset = focus_node->length();
        }
        break;
    case CSS::UserSelect::All:
        // https://drafts.csswg.org/css-ui/#valdef-user-select-all

        // The content of the element must be selected atomically: If a selection would contain part of the element,
        // then the selection must contain the entire element including all its descendants. If the element is selected
        // and the used value of 'user-select' on its parent is 'all', then the parent must be included in the selection,
        // recursively.
        while (focus_node->parent() && focus_node->parent()->layout_node()->user_select_used_value() == CSS::UserSelect::All) {
            if (anchor_node == focus_node) {
                anchor_node = focus_node->parent();
            }
            focus_node = focus_node->parent();
        }

        if (focus_node == anchor_node) {
            if (anchor_offset > focus_offset) {
                anchor_offset = focus_node->length();
                focus_offset = 0;
            } else {
                anchor_offset = 0;
                focus_offset = focus_node->length();
            }
        } else if (focus_node->is_before(*anchor_node)) {
            focus_offset = 0;
        } else {
            focus_offset = focus_node->length();
        }
        break;
    case CSS::UserSelect::Contain:
        // NB: This is handled at the start of this function
        break;
    case CSS::UserSelect::Text:
        // https://drafts.csswg.org/css-ui/#valdef-user-select-text
        // The element imposes no constraint on the selection.
        break;
    case CSS::UserSelect::Auto:
        VERIFY_NOT_REACHED();
        break;
    }

    (void)selection->set_base_and_extent(*anchor_node, anchor_offset, *focus_node, focus_offset);
}

void EventHandler::process_auto_scroll()
{
    if (m_auto_scroll_handler)
        m_auto_scroll_handler->perform_tick();
    if (m_middle_button_scroll_handler)
        m_middle_button_scroll_handler->perform_tick();
}

void EventHandler::update_mouse_selection(CSSPixelPoint visual_viewport_position)
{
    if (m_selection_mode == SelectionMode::None)
        return;

    auto clamped_position = m_auto_scroll_handler
        ? m_auto_scroll_handler->process(visual_viewport_position)
        : visual_viewport_position;
    apply_mouse_selection(clamped_position);
}

void EventHandler::apply_mouse_selection(CSSPixelPoint visual_viewport_position)
{
    auto hit = paint_root()->hit_test(visual_viewport_position, Painting::HitTestType::TextCursor);
    if (!hit.has_value() || !hit->paintable->dom_node())
        return;

    auto& document = *m_navigable->active_document();
    auto& hit_dom_node = *hit->paintable->dom_node();
    GC::Ref<DOM::Node> focus_node = hit_dom_node;
    size_t focus_index = hit->index_in_node;
    GC::Ptr<DOM::Node> anchor_node;
    Optional<size_t> anchor_offset;

    // In word selection mode, extend selection by whole words.
    if (m_selection_mode == SelectionMode::Word && m_selection_origin && is<DOM::Text>(*focus_node)) {
        auto& hit_text_node = as<DOM::Text>(*focus_node);
        auto& segmenter = hit_text_node.word_segmenter();
        auto word_start = segmenter.previous_boundary(focus_index, Unicode::Segmenter::Inclusive::Yes).value_or(0);
        auto word_end = segmenter.next_boundary(focus_index).value_or(focus_node->length());

        // Determine cursor position relative to anchor.
        auto position = m_selection_origin->compare_point(*focus_node, focus_index);
        if (!position.is_error()) {
            if (position.value() < 0) {
                // Cursor is before anchor: select from anchor end to current word start.
                anchor_node = m_selection_origin->end_container();
                anchor_offset = m_selection_origin->end_offset();
                focus_index = word_start;
            } else if (position.value() > 0) {
                // Cursor is after anchor: select from anchor start to current word end.
                anchor_node = m_selection_origin->start_container();
                anchor_offset = m_selection_origin->start_offset();
                focus_index = word_end;
            } else {
                // Cursor is within anchor: keep original word selected.
                anchor_node = m_selection_origin->start_container();
                anchor_offset = m_selection_origin->start_offset();
                focus_index = m_selection_origin->end_offset();
            }
        }
    }

    // In paragraph selection mode, extend selection by whole lines/paragraphs.
    if (m_selection_mode == SelectionMode::Paragraph && m_selection_origin && is<DOM::Text>(*focus_node)) {
        auto& focus_text_node = as<DOM::Text>(*focus_node);

        // For input/textarea, find line boundaries using newline characters.
        // For regular content, find paragraph boundaries using block elements.
        GC::Ref<DOM::Range> paragraph_range = m_mouse_selection_target
            ? DOM::Range::create(focus_text_node, find_line_start(focus_text_node.data().utf16_view(), focus_index),
                  focus_text_node, find_line_end(focus_text_node.data().utf16_view(), focus_index))
            : find_paragraph_range(focus_text_node, focus_index);

        // Determine cursor position relative to origin.
        auto position = m_selection_origin->compare_point(*focus_node, focus_index);
        if (!position.is_error()) {
            if (position.value() < 0) {
                // Cursor is before origin: select from origin end to current paragraph start.
                anchor_node = m_selection_origin->end_container();
                anchor_offset = m_selection_origin->end_offset();
                focus_node = *paragraph_range->start_container();
                focus_index = paragraph_range->start_offset();
            } else if (position.value() > 0) {
                // Cursor is after origin: select from origin start to current paragraph end.
                anchor_node = m_selection_origin->start_container();
                anchor_offset = m_selection_origin->start_offset();
                focus_node = *paragraph_range->end_container();
                focus_index = paragraph_range->end_offset();
            } else {
                // Cursor is within origin: keep original paragraph selected.
                anchor_node = m_selection_origin->start_container();
                anchor_offset = m_selection_origin->start_offset();
                focus_node = *m_selection_origin->end_container();
                focus_index = m_selection_origin->end_offset();
            }
        }
    }

    if (m_mouse_selection_target) {
        if (anchor_offset.has_value())
            m_mouse_selection_target->set_selection_anchor(anchor_node ? *anchor_node : *focus_node, anchor_offset.value());
        m_mouse_selection_target->set_selection_focus(*focus_node, focus_index);
    } else {
        if (auto selection = document.get_selection()) {
            auto selection_anchor_node = anchor_node ? anchor_node : selection->anchor_node();
            if (selection_anchor_node) {
                if (&selection_anchor_node->root() == &focus_node->root()) {
                    auto selection_anchor_offset = anchor_offset.has_value() ? anchor_offset.value() : selection->anchor_offset();
                    set_user_selection(*selection_anchor_node, selection_anchor_offset, *focus_node, focus_index, selection, hit->paintable->layout_node().user_select_used_value());
                }
            } else {
                set_user_selection(*focus_node, focus_index, *focus_node, focus_index, selection, hit->paintable->layout_node().user_select_used_value());
            }

            document.set_needs_repaint(Badge<EventHandler> {});
        }
    }
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#run-light-dismiss-activities
static void light_dismiss_activities(UIEvents::PointerEvent const& event, GC::Ref<DOM::Node> const target)
{
    // To run light dismiss activities, given a PointerEvent event:

    // 1. Run light dismiss open popovers with event.
    HTML::HTMLElement::light_dismiss_open_popovers(event, target);

    // 2. Run light dismiss open dialogs with event.
    HTML::HTMLDialogElement::light_dismiss_open_dialogs(event, target);
}

static void set_node_and_ancestors_being_activated(DOM::Node* node, bool activated)
{
    for (auto* ancestor = node; ancestor; ancestor = ancestor->parent()) {
        if (auto* element = as_if<DOM::Element>(*ancestor))
            element->set_being_activated(activated);
    }
}

EventHandler::EventHandler(Badge<HTML::Navigable>, HTML::Navigable& navigable)
    : m_navigable(navigable)
    , m_drag_and_drop_event_handler(make<DragAndDropEventHandler>())
{
}

EventHandler::~EventHandler() = default;

GC::Ptr<Painting::PaintableBox> EventHandler::paint_root()
{
    if (!m_navigable->active_document())
        return nullptr;
    return m_navigable->active_document()->paintable_box();
}

GC::Ptr<Painting::PaintableBox const> EventHandler::paint_root() const
{
    if (!m_navigable->active_document())
        return nullptr;
    return m_navigable->active_document()->paintable_box();
}

EventResult EventHandler::handle_mousewheel(CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers, int wheel_delta_x, int wheel_delta_y)
{
    if (should_ignore_device_input_event())
        return EventResult::Dropped;

    auto document = m_navigable->active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    auto viewport_position = document->visual_viewport()->map_to_layout_viewport(visual_viewport_position);

    document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseWheel);

    if (!paint_root())
        return EventResult::Dropped;

    if (modifiers & UIEvents::KeyModifier::Mod_Shift)
        swap(wheel_delta_x, wheel_delta_y);

    auto handled_event = EventResult::Dropped;

    GC::Ptr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(visual_viewport_position); result.has_value())
        paintable = result->paintable;

    if (paintable) {
        Painting::Paintable* containing_block = paintable;
        while (containing_block) {
            auto handled_scroll_event = containing_block->handle_mousewheel({}, visual_viewport_position, buttons, modifiers, wheel_delta_x, wheel_delta_y);
            if (handled_scroll_event)
                return EventResult::Handled;

            containing_block = containing_block->containing_block();
        }

        if (paintable->handle_mousewheel({}, visual_viewport_position, buttons, modifiers, wheel_delta_x, wheel_delta_y))
            return EventResult::Handled;

        auto node = dom_node_for_event_dispatch(*paintable);

        if (node) {
            if (auto result = dispatch_event_to_nested_navigable(*paintable, visual_viewport_position, [screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y](EventHandler& event_handler, CSSPixelPoint position) -> EventResult {
                    return event_handler.handle_mousewheel(position, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y);
                });
                result.has_value())
                return result.value();

            // NB: Search for the first parent of the hit target that's an element.
            GC::Ptr<Layout::Node> layout_node;
            if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
                return EventResult::Dropped;

            auto page_offset = compute_mouse_event_page_offset(viewport_position);
            auto const& offset_paintable = layout_node->first_paintable() ? layout_node->first_paintable() : paintable.ptr();
            auto scroll_offset = document->navigable()->viewport_scroll_offset();
            auto offset = compute_mouse_event_offset(visual_viewport_position.translated(scroll_offset), *offset_paintable);
            if (node->dispatch_event(UIEvents::WheelEvent::create_from_platform_event(node->realm(), m_navigable->active_window_proxy(), UIEvents::EventNames::wheel, screen_position, page_offset, viewport_position, offset, wheel_delta_x, wheel_delta_y, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors())) {
                m_navigable->scroll_viewport_by_delta({ wheel_delta_x, wheel_delta_y });
            }

            handled_event = EventResult::Handled;
        }
    }

    return handled_event;
}

void EventHandler::update_hovered_chrome_widget(GC::Ptr<Painting::ChromeWidget> widget)
{
    if (m_hovered_chrome_widget == widget)
        return;
    if (m_hovered_chrome_widget)
        m_hovered_chrome_widget->mouse_leave();
    m_hovered_chrome_widget = widget;
    if (m_hovered_chrome_widget)
        m_hovered_chrome_widget->mouse_enter();
}

EventHandler::MouseEventCoordinates EventHandler::compute_mouse_event_coordinates(CSSPixelPoint visual_viewport_position, CSSPixelPoint viewport_position, Painting::Paintable const& paintable, Layout::Node const& layout_node) const
{
    auto page_offset = compute_mouse_event_page_offset(viewport_position);
    auto const& offset_paintable = layout_node.first_paintable() ? layout_node.first_paintable() : &paintable;
    auto scroll_offset = m_navigable->active_document()->navigable()->viewport_scroll_offset();
    auto offset = compute_mouse_event_offset(visual_viewport_position.translated(scroll_offset), *offset_paintable);
    return { page_offset, visual_viewport_position, viewport_position, offset };
}

bool EventHandler::dispatch_chrome_widget_pointer_event(GC::Ptr<Painting::ChromeWidget> target, FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position)
{
    bool allow_default_behavior = true;

    if (m_captured_chrome_widget) {
        auto action = m_captured_chrome_widget->handle_pointer_event(type, button, visual_viewport_position);
        if (has_flag(action, Painting::MouseAction::SwallowEvent))
            allow_default_behavior = false;
        if (has_flag(action, Painting::MouseAction::CaptureInput))
            return allow_default_behavior;
        m_captured_chrome_widget = nullptr;
    }

    if (target) {
        auto action = target->handle_pointer_event(type, button, visual_viewport_position);
        if (has_flag(action, Painting::MouseAction::SwallowEvent))
            allow_default_behavior = false;
        if (has_flag(action, Painting::MouseAction::CaptureInput))
            m_captured_chrome_widget = target;
    }

    return allow_default_behavior;
}

// https://w3c.github.io/pointerevents/#mapping-for-devices-that-support-hover
bool EventHandler::dispatch_a_pointer_event_for_a_device_that_supports_hover(PointerEventType type, GC::Ptr<DOM::Node> node, GC::Ptr<Painting::ChromeWidget> chrome_widget, MouseEventCoordinates const& coordinates, CSSPixelPoint screen_position, CSSPixelPoint movement, unsigned button, unsigned buttons, unsigned modifiers, int click_count)
{
    auto& document = *m_navigable->active_document();
    auto& realm = document.realm();

    auto pointer_event_name = [&] {
        switch (type) {
        case PointerEventType::PointerDown:
            return UIEvents::EventNames::pointerdown;
        case PointerEventType::PointerUp:
            return UIEvents::EventNames::pointerup;
        case PointerEventType::PointerMove:
            return UIEvents::EventNames::pointermove;
        case PointerEventType::PointerCancel:
            return UIEvents::EventNames::pointercancel;
        }
        VERIFY_NOT_REACHED();
    }();
    auto pointer_event = MUST(UIEvents::PointerEvent::create_from_platform_event(realm, m_navigable->active_window_proxy(), pointer_event_name, screen_position, coordinates.page_offset, coordinates.viewport_position, coordinates.offset, movement, button, buttons, modifiers));

    // FIXME: 1. If the isPrimary property for the pointer event to be dispatched is false then dispatch the
    //           pointer event and terminate these steps.

    // 2. If the pointer event to be dispatched is a pointerdown, pointerup or pointermove event, or a
    //    pointerleave event at the window, dispatch compatibility mouse transition events as described in
    //    "Tracking the effective position of the legacy mouse pointer".
    if (type == PointerEventType::PointerDown || type == PointerEventType::PointerUp || type == PointerEventType::PointerMove)
        track_the_effective_position_of_the_legacy_mouse_pointer(node);

    // AD-HOC: Update the activated state for the pointed-to element.
    if (button == UIEvents::MouseButton::Primary && type != PointerEventType::PointerMove) {
        if (type == PointerEventType::PointerDown)
            set_node_and_ancestors_being_activated(node, true);
        else if (m_mousedown_target)
            set_node_and_ancestors_being_activated(m_mousedown_target, false);
    }

    update_hovered_chrome_widget(chrome_widget);

    // The events above may have changed layout, and chrome widgets are very likely to touch paintables.
    if (chrome_widget || m_captured_chrome_widget)
        document.update_layout(DOM::UpdateLayoutReason::EventHandlerDispatchChromeWidgetEvent);
    if (!dispatch_chrome_widget_pointer_event(chrome_widget, pointer_event_name, button, coordinates.visual_viewport_position))
        return false;

    // FIXME: This will be moved to the click event and need to track the targets of the pointerdown and pointerup events.
    //        https://github.com/w3c/pointerevents/pull/460
    light_dismiss_activities(pointer_event, *node);

    // 3. Dispatch the pointer event.
    bool pointer_event_cancelled = !node->dispatch_event(pointer_event);

    // 4. If the pointer event dispatched was pointerdown and event's canceled flag is set, then set the PREVENT MOUSE
    //    EVENT flag for this pointerType.
    if (type == PointerEventType::PointerDown && pointer_event_cancelled)
        m_prevent_mouse_event = true;

    // 5. If the PREVENT MOUSE EVENT flag is not set for this pointerType and the pointer event dispatched was:
    bool run_default_activation_behavior = false;
    if (!m_prevent_mouse_event) {
        GC::Ptr<DOM::EventTarget> mouse_event_target = node;
        auto mouse_event_name = [&] {
            switch (type) {
            // - pointerdown, then fire a mousedown event.
            case PointerEventType::PointerDown:
                return UIEvents::EventNames::mousedown;
            // - pointermove, then fire a mousemove event.
            case PointerEventType::PointerMove:
                return UIEvents::EventNames::mousemove;
            // - pointerup, then fire a mouseup event.
            case PointerEventType::PointerUp:
                return UIEvents::EventNames::mouseup;
            // - pointercancel, then fire a mouseup event at the window.
            case PointerEventType::PointerCancel:
                mouse_event_target = node->document().window();
                return UIEvents::EventNames::mouseup;
            }
            VERIFY_NOT_REACHED();
        }();
        run_default_activation_behavior = mouse_event_target->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), m_navigable->active_window_proxy(), mouse_event_name, screen_position, coordinates.page_offset, coordinates.viewport_position, coordinates.offset, movement, button, buttons, modifiers, click_count).release_value_but_fixme_should_propagate_errors());
    }

    // 6. If the pointer event dispatched was pointerup or pointercancel, clear the PREVENT MOUSE EVENT flag for this
    //    pointerType.
    if (type == PointerEventType::PointerUp || type == PointerEventType::PointerCancel)
        m_prevent_mouse_event = false;

    return run_default_activation_behavior;
}

void EventHandler::track_the_effective_position_of_the_legacy_mouse_pointer(GC::Ptr<DOM::Node> target)
{
    auto& page = m_navigable->page();
    auto& document = *m_navigable->active_document();

    // 1. Let T be the target of the pointerdown, pointerup or pointermove event being dispatched. For the pointerleave
    //    event, unset T.

    // 2. If T and current effective legacy mouse pointer position are both unset or they are equal, terminate these
    //    steps.
    if (m_effective_legacy_mouse_pointer_position == target)
        return;

    // 3. Dispatch mouseover, mouseout, mouseenter and mouseleave events as per [UIEVENTS] for a mouse moving from the
    //    current effective legacy mouse pointer position to T. Consider an unset value of either current effective
    //    legacy mouse pointer position or T as an out-of-window mouse position.
    // FIXME: This is supposed to only fire legacy events. The pointer events should be fired elsewhere.
    document.set_hovered_node(target);

    // 4. Set effective legacy mouse pointer position to T.
    m_effective_legacy_mouse_pointer_position = target;

    // AD-HOC: Notify the WebView client about hovered text/links.
    HTML::HTMLElement const* hovered_title_element = nullptr;
    if (target)
        hovered_title_element = target->enclosing_html_element_with_attribute(HTML::AttributeNames::title);
    if (hovered_title_element && hovered_title_element->title().has_value()) {
        page.client().page_did_enter_tooltip_area(hovered_title_element->title()->to_byte_string());
        page.set_is_in_tooltip_area(true);
    } else if (page.is_in_tooltip_area()) {
        page.client().page_did_leave_tooltip_area();
        page.set_is_in_tooltip_area(false);
    }

    HTML::HTMLAnchorElement const* hovered_link_element = nullptr;
    if (target)
        hovered_link_element = target->enclosing_link_element();
    if (hovered_link_element) {
        if (auto link_url = document.encoding_parse_url(hovered_link_element->href()); link_url.has_value()) {
            page.client().page_did_hover_link(*link_url);
            page.set_is_hovering_link(true);
        }
    } else if (page.is_hovering_link()) {
        page.client().page_did_unhover_link();
        page.set_is_hovering_link(false);
    }
}

static void set_page_cursor(Page& page, Gfx::Cursor cursor)
{
    // FIXME: This check is only approximate. ImageCursors from the same CursorStyleValue share bitmaps, but may
    //        repaint them. So comparing them does not tell you if they are the same image. Also, the image may
    //        change even if the hovered node does not.
    if (page.current_cursor() != cursor) {
        page.client().page_did_request_cursor_change(cursor);
        page.set_current_cursor(cursor);
    }
}

void EventHandler::update_cursor(GC::Ptr<Painting::Paintable> paintable, GC::Ptr<DOM::Node> host_element, GC::Ptr<Painting::ChromeWidget> chrome_widget)
{
    // AD-HOC: Update the cursor image based on the CSS rules before the steps terminate if the target hasn't changed.
    auto cursor = [&] -> Gfx::Cursor {
        if (chrome_widget) {
            if (auto cursor_override = chrome_widget->cursor(); cursor_override.has_value())
                return css_to_gfx_cursor(cursor_override.value());
        }

        if (paintable) {
            auto cursor_data = paintable->computed_values().cursor();
            if (paintable->layout_node().is_text_node() || host_element->is_editable_or_editing_host())
                return resolve_cursor(*paintable->layout_node().parent(), cursor_data, Gfx::StandardCursor::IBeam);
            if (host_element && host_element->is_element())
                return resolve_cursor(static_cast<Layout::NodeWithStyle&>(*host_element->layout_node()), cursor_data, Gfx::StandardCursor::Arrow);
        }

        return Gfx::StandardCursor::Arrow;
    }();

    set_page_cursor(m_navigable->page(), cursor);
}

bool EventHandler::fire_click_events(GC::Ref<DOM::Node> node, MouseEventCoordinates const& coordinates, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int click_count)
{
    bool run_activation_behavior = false;

    if (button == UIEvents::MouseButton::Primary) {
        // https://w3c.github.io/pointerevents/#click
        run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), m_navigable->active_window_proxy(), UIEvents::EventNames::click, screen_position, coordinates.page_offset, coordinates.viewport_position, coordinates.offset, {}, button, buttons, modifiers, click_count).release_value_but_fixme_should_propagate_errors());

        // https://w3c.github.io/uievents/#event-type-dblclick
        // This event type MUST be dispatched after the event type click if a click and
        // double click occur simultaneously, and after the event type mouseup otherwise.
        if (click_count == 2)
            node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), m_navigable->active_window_proxy(), UIEvents::EventNames::dblclick, screen_position, coordinates.page_offset, coordinates.viewport_position, coordinates.offset, {}, button, buttons, modifiers, click_count).release_value_but_fixme_should_propagate_errors());
    } else {
        // https://w3c.github.io/pointerevents/#auxclick
        run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), m_navigable->active_window_proxy(), UIEvents::EventNames::auxclick, screen_position, coordinates.page_offset, coordinates.viewport_position, coordinates.offset, {}, button, buttons, modifiers, click_count).release_value_but_fixme_should_propagate_errors());
    }

    return run_activation_behavior;
}

void EventHandler::run_activation_behavior(GC::Ref<DOM::Node> node, unsigned button, unsigned modifiers)
{
    if (GC::Ptr<HTML::HTMLAnchorElement const> link = node->enclosing_link_element()) {
        GC::Ref<DOM::Document> document = *m_navigable->active_document();
        auto href = link->href();
        auto url = document->encoding_parse_url(href);
        if (url.has_value()) {
            if (button == UIEvents::MouseButton::Primary && (modifiers & UIEvents::Mod_PlatformCtrl) != 0) {
                m_navigable->page().client().page_did_click_link(*url, link->target().to_byte_string(), modifiers);
            } else if (button == UIEvents::MouseButton::Middle) {
                m_navigable->page().client().page_did_middle_click_link(*url, link->target().to_byte_string(), modifiers);
            }
        }
    }
}

// https://w3c.github.io/uievents/#maybe-show-context-menu
void EventHandler::maybe_show_context_menu(GC::Ref<DOM::Node> node, MouseEventCoordinates const& coordinates, CSSPixelPoint screen_position, CSSPixelPoint viewport_position, unsigned buttons, unsigned modifiers)
{
    // AD-HOC: Allow the user to bypass custom context menus by holding shift, like Firefox.
    if ((modifiers & UIEvents::Mod_Shift) == 0) {
        // 1. Let menuevent = create a PointerEvent with "contextmenu", target
        // 2. If native is valid, then set MouseEvent attributes from native.
        auto menuevent = UIEvents::MouseEvent::create_from_platform_event(node->realm(), m_navigable->active_window_proxy(), UIEvents::EventNames::contextmenu, screen_position, coordinates.page_offset, coordinates.viewport_position, coordinates.offset, {}, UIEvents::MouseButton::Secondary, buttons, modifiers).release_value_but_fixme_should_propagate_errors();

        // 3. Let result = dispatch menuevent at target.
        bool result = node->dispatch_event(menuevent);

        // 4. If result is true, then show the UA context menu.
        if (!result)
            return;
    }

    // AD-HOC: The context menu will consume our mouseup on macOS, and it seems that Chromium allows the mouseup to be
    //         eaten by the context menu. Doing this prevents us from ignoring the cursor leaving the window when the
    //         context menu is open.
    clear_mousedown_tracking();

    // NB: Event dispatches above may have run JS that invalidated layout.
    m_navigable->active_document()->update_layout(DOM::UpdateLayoutReason::EventHandlerShowContextMenu);

    auto top_level_viewport_position = m_navigable->to_top_level_position(viewport_position);
    if (GC::Ptr<HTML::HTMLAnchorElement const> link = node->enclosing_link_element()) {
        GC::Ref<DOM::Document> document = *m_navigable->active_document();
        auto href = link->href();
        auto url = document->encoding_parse_url(href);
        if (url.has_value())
            m_navigable->page().client().page_did_request_link_context_menu(top_level_viewport_position, *url, link->target().to_byte_string(), modifiers);
    } else {
        // AD-HOC: Skip up the tree to the first ancestor that is not a UA shadow DOM node, and use its context menu.
        //         Media elements' controls' shadow DOM nodes should not have their own context menu, but rather
        //         activate their parent media element's menu.
        auto context_menu_node = node;
        while (auto shadow_root = context_menu_node->containing_shadow_root()) {
            if (!shadow_root->is_user_agent_internal())
                break;
            VERIFY(shadow_root->host() != nullptr);
            context_menu_node = *shadow_root->host();
        }

        if (is<HTML::HTMLImageElement>(*context_menu_node)) {
            auto& image_element = as<HTML::HTMLImageElement>(*context_menu_node);
            auto image_url = image_element.document().encoding_parse_url(image_element.current_src());
            if (image_url.has_value()) {
                Optional<Gfx::Bitmap const*> bitmap;
                if (image_element.immutable_bitmap())
                    bitmap = image_element.immutable_bitmap()->bitmap();

                m_navigable->page().client().page_did_request_image_context_menu(top_level_viewport_position, *image_url, "", modifiers, bitmap);
            }
        } else if (is<HTML::HTMLMediaElement>(*context_menu_node)) {
            auto& media_element = as<HTML::HTMLMediaElement>(*context_menu_node);
            auto is_video = is<HTML::HTMLVideoElement>(*context_menu_node);

            Page::MediaContextMenu menu {
                .media_url = *media_element.document().encoding_parse_url(media_element.current_src()),
                .is_video = is_video,
                .is_playing = media_element.potentially_playing(),
                .is_muted = media_element.muted(),
                .has_user_agent_controls = media_element.has_attribute(HTML::AttributeNames::controls),
                .is_looping = media_element.has_attribute(HTML::AttributeNames::loop),
                .is_fullscreen = is_video && media_element.is_fullscreen_element(),
            };

            m_navigable->page().did_request_media_context_menu(media_element.unique_id(), top_level_viewport_position, "", modifiers, menu);
        } else {
            m_navigable->page().client().page_did_request_context_menu(top_level_viewport_position);
        }
    }
}

void EventHandler::clear_mousedown_tracking()
{
    m_mousedown_button = {};
    m_mousedown_modifiers = UIEvents::KeyModifier::Mod_None;
    m_mousedown_target = nullptr;
    m_mousedown_visual_viewport_position = {};
    m_mousedown_click_count = 0;
}

void EventHandler::stop_updating_selection()
{
    m_selection_mode = SelectionMode::None;
    m_selection_origin = {};
    m_mouse_selection_target = nullptr;

    m_auto_scroll_handler = nullptr;
}

EventResult EventHandler::handle_mouseup(CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers)
{
    ScopeGuard clear_mousedown_tracking_and_stop_selection([&] {
        clear_mousedown_tracking();
        stop_updating_selection();
        m_middle_button_scroll_handler = nullptr;
    });

    if (should_ignore_device_input_event()) {
        if (is_dragging_element()) {
            auto result = handle_drag_and_drop_event(DragEvent::Type::Drop, visual_viewport_position, screen_position, button, buttons, modifiers, {});
            set_page_cursor(m_navigable->page(), Gfx::StandardCursor::Arrow);
            return result;
        }

        return EventResult::Dropped;
    }

    auto document = m_navigable->active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    auto viewport_position = document->visual_viewport()->map_to_layout_viewport(visual_viewport_position);
    document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseUp);

    if (!paint_root())
        return EventResult::Dropped;

    GC::Ptr<Painting::Paintable> paintable;
    GC::Ptr<Painting::ChromeWidget> chrome_widget;
    if (auto result = target_for_mouse_position(visual_viewport_position); result.has_value()) {
        paintable = result->paintable;
        chrome_widget = result->chrome_widget;
    }

    auto click_count = m_mousedown_click_count;
    if (click_count <= 0)
        return EventResult::Dropped;

    if (!paintable) {
        VERIFY(!chrome_widget);

        // NB: Always fire a mouseup event if we've fired a mousedown event. Otherwise, web pages will not have a
        //     chance to end a drag that went outside the window.
        if (m_mousedown_target) {
            auto coordinates = compute_mouse_event_coordinates(visual_viewport_position, viewport_position, *document->paintable(), *document->layout_node());
            dispatch_a_pointer_event_for_a_device_that_supports_hover(PointerEventType::PointerUp, document->html_element(), nullptr, coordinates, screen_position, {}, button, buttons, modifiers, click_count);
            return EventResult::Handled;
        }

        return EventResult::Dropped;
    }

    auto pointer_events = paintable->computed_values().pointer_events();
    if (pointer_events == CSS::PointerEvents::None)
        return EventResult::Cancelled;

    auto node = dom_node_for_event_dispatch(*paintable);
    if (!node)
        return EventResult::Dropped;

    if (auto result = dispatch_event_to_nested_navigable(*paintable, visual_viewport_position, [=](EventHandler& event_handler, CSSPixelPoint position) -> EventResult {
            return event_handler.handle_mouseup(position, screen_position, button, buttons, modifiers);
        });
        result.has_value())
        return result.value();

    // NB: Search for the first parent of the hit target that's an element.
    //
    // https://www.w3.org/TR/uievents/#event-type-click
    // The click event type MUST be dispatched on the topmost event target indicated by the pointer.
    //
    // https://www.w3.org/TR/uievents/#topmost-event-target
    // The topmost event target MUST be the element highest in the rendering order which is capable of being an
    // event target.
    GC::Ptr<Layout::Node> layout_node;
    if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
        return EventResult::Dropped;

    auto coordinates = compute_mouse_event_coordinates(visual_viewport_position, viewport_position, *paintable, *layout_node);
    dispatch_a_pointer_event_for_a_device_that_supports_hover(PointerEventType::PointerUp, *node, chrome_widget, coordinates, screen_position, {}, button, buttons, modifiers, click_count);

    // FIXME: Per spec, the click target should be the nearest common inclusive ancestor of the pointerdown
    //        and pointerup targets. Currently we require an exact match.
    // NB: If the middle button was used to scroll, suppress click and activation behavior.
    auto middle_button_scrolled = m_middle_button_scroll_handler && m_middle_button_scroll_handler->mouse_has_moved();
    if (node.ptr() == m_mousedown_target && !middle_button_scrolled) {
        if (fire_click_events(*node, coordinates, screen_position, button, buttons, modifiers, click_count)
            && !chrome_widget) {
            // NB: Event dispatches above may have run JS that invalidated layout.
            m_navigable->active_document()->update_layout(DOM::UpdateLayoutReason::EventHandlerRunActivationBehavior);

            // FIXME: Currently cannot spawn a new top-level browsing context for new tab operations, because the
            //        new top-level browsing context would be in another process. To fix this, there needs to be
            //        some way to be able to communicate with browsing contexts in remote WebContent processes, and
            //        then step 8 of this algorithm needs to be implemented in Navigable::choose_a_navigable:
            //        https://html.spec.whatwg.org/multipage/document-sequences.html#the-rules-for-choosing-a-navigable
            run_activation_behavior(*node, button, modifiers);
        }
    }

    return EventResult::Handled;
}

bool EventHandler::initiate_character_selection(DOM::Document& document, Painting::HitTestResult const& hit, CSS::UserSelect user_select, bool shift_held)
{
    auto& hit_node = *hit.paintable->dom_node();

    size_t index = hit.index_in_node;
    if (InputEventsTarget* active_target = document.active_input_events_target(&hit_node)) {
        m_mouse_selection_target = active_target;

        if (shift_held)
            active_target->set_selection_focus(hit_node, index);
        else
            active_target->set_selection_anchor(hit_node, index);
    } else {
        m_mouse_selection_target = nullptr;

        if (auto selection = document.get_selection()) {
            if (auto anchor_node = selection->anchor_node(); anchor_node && shift_held)
                set_user_selection(*anchor_node, selection->anchor_offset(), hit_node, index, selection, user_select);
            else
                set_user_selection(hit_node, index, hit_node, index, selection, user_select);
        }
    }

    m_selection_mode = SelectionMode::Character;
    return true;
}

bool EventHandler::initiate_word_selection(DOM::Document& document, Painting::HitTestResult const& hit, CSS::UserSelect user_select)
{
    if (!is<Painting::TextPaintable>(*hit.paintable))
        return false;

    auto& hit_paintable = static_cast<Painting::TextPaintable&>(*hit.paintable);
    auto& hit_node = as<DOM::Text>(*hit_paintable.dom_node());

    size_t previous_boundary = 0;
    size_t next_boundary = 0;

    if (hit_node.is_password_input()) {
        next_boundary = hit_node.length_in_utf16_code_units();
    } else {
        auto& segmenter = word_segmenter();
        segmenter.set_segmented_text(hit_paintable.layout_node().text_for_rendering());

        previous_boundary = segmenter.previous_boundary(hit.index_in_node, Unicode::Segmenter::Inclusive::Yes).value_or(0);
        next_boundary = segmenter.next_boundary(hit.index_in_node).value_or(hit_node.length());
    }

    m_selection_mode = SelectionMode::Word;
    m_selection_origin = DOM::Range::create(hit_node, previous_boundary, hit_node, next_boundary);

    if (auto* target = document.active_input_events_target(&hit_node)) {
        m_mouse_selection_target = target;
        target->set_selection_anchor(hit_node, previous_boundary);
        target->set_selection_focus(hit_node, next_boundary);
    } else if (auto selection = document.get_selection()) {
        m_mouse_selection_target = nullptr;
        set_user_selection(hit_node, previous_boundary, hit_node, next_boundary, selection, user_select);
    }

    m_selection_mode = SelectionMode::Word;
    return true;
}

bool EventHandler::initiate_paragraph_selection(DOM::Document& document, Painting::HitTestResult const& hit, CSS::UserSelect user_select)
{
    if (!is<DOM::Text>(*hit.paintable->dom_node()))
        return false;

    auto& hit_node = as<DOM::Text>(*hit.paintable->dom_node());
    size_t hit_index = hit.index_in_node;

    // For input/textarea elements, select the current line (delimited by newlines).
    if (auto* target = document.active_input_events_target(&hit_node)) {
        auto text = hit_node.data().utf16_view();
        auto line_start = find_line_start(text, hit_index);
        auto line_end = find_line_end(text, hit_index);

        m_selection_mode = SelectionMode::Paragraph;
        m_selection_origin = DOM::Range::create(hit_node, line_start, hit_node, line_end);
        m_mouse_selection_target = target;
        target->set_selection_anchor(hit_node, line_start);
        target->set_selection_focus(hit_node, line_end);
    } else if (auto selection = document.get_selection()) {
        // For regular content, find paragraph boundaries within the containing block.
        m_selection_origin = find_paragraph_range(hit_node, hit_index);

        m_selection_mode = SelectionMode::Paragraph;
        m_mouse_selection_target = nullptr;

        set_user_selection(m_selection_origin->start_container(), m_selection_origin->start_offset(), *m_selection_origin->end_container(), m_selection_origin->end_offset(), selection, user_select);
    }

    m_selection_mode = SelectionMode::Paragraph;
    return true;
}

void EventHandler::run_mousedown_default_actions(DOM::Document& document, CSSPixelPoint visual_viewport_position, CSSPixelPoint viewport_position, unsigned button, unsigned modifiers, int click_count)
{
    if (button == UIEvents::MouseButton::Middle) {
        if (!m_navigable->page().enable_autoscroll())
            return;

        auto hit = paint_root()->hit_test(visual_viewport_position, Painting::HitTestType::Exact);
        if (!hit.has_value())
            return;

        for (GC::Ptr<DOM::Node const> node = hit->paintable->dom_node(); node; node = node->parent_or_shadow_host_node()) {
            if (is<HTML::FormAssociatedTextControlElement>(*node))
                return;
            if (auto const* anchor = as_if<HTML::HTMLAnchorElement>(*node); anchor && anchor->has_attribute(HTML::AttributeNames::href))
                return;
        }

        if (auto container = MiddleButtonScrollHandler::find_scrollable_ancestor(document, *hit->paintable))
            m_middle_button_scroll_handler = make<MiddleButtonScrollHandler>(*container, visual_viewport_position);

        return;
    }

    if (button != UIEvents::MouseButton::Primary)
        return;
#if defined(AK_OS_MACOS)
    if (modifiers & UIEvents::KeyModifier::Mod_Ctrl)
        return;
#endif

    // https://html.spec.whatwg.org/multipage/interaction.html#data-model:click-focusable-5
    // When a user activates a click focusable focusable area, the user agent must run the focusing steps on that
    // focusable area with focus trigger set to "click".
    // NOTE: Note that focusing is not an activation behavior, i.e. calling the click() method on an element or
    //       dispatching a synthetic click event on it won't cause the element to get focused.
    if (auto focus_candidate = focus_candidate_for_position(viewport_position))
        HTML::run_focusing_steps(focus_candidate, nullptr, HTML::FocusTrigger::Click);
    else if (auto focused_area = document.focused_area())
        HTML::run_unfocusing_steps(focused_area);

    // NB: Focusing may have invalidated layout.
    document.update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseDown);
    if (!paint_root())
        return;

    // NB: Now we can do selection with a cursor hit test.
    auto cursor_hit = paint_root()->hit_test(visual_viewport_position, Painting::HitTestType::TextCursor);
    if (!cursor_hit.has_value())
        return;

    auto dom_node = cursor_hit->paintable->dom_node();
    if (!dom_node)
        return;

    // https://drafts.csswg.org/css-ui/#valdef-user-select-none
    // Attempting to start a selection in an element where user-select is none, such as by clicking in it or starting a
    // drag in it, must not cause a pre-existing selection to become unselected or to be affected in any way.
    auto user_select = cursor_hit->paintable->layout_node().user_select_used_value();
    if (user_select == CSS::UserSelect::None)
        return;

    auto selection_started = [&] {
        if (click_count == 3)
            return initiate_paragraph_selection(document, *cursor_hit, user_select);
        if (click_count == 2)
            return initiate_word_selection(document, *cursor_hit, user_select);

        return initiate_character_selection(document, *cursor_hit, user_select, modifiers & UIEvents::KeyModifier::Mod_Shift);
    }();
    if (!selection_started)
        return;
    VERIFY(m_selection_mode != SelectionMode::None);

    if (auto container = AutoScrollHandler::find_scrollable_ancestor(*cursor_hit->paintable))
        m_auto_scroll_handler = make<AutoScrollHandler>(m_navigable, *container);
}

EventResult EventHandler::handle_mousedown(CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers, int click_count)
{
    if (should_ignore_device_input_event())
        return EventResult::Dropped;

    auto document = m_navigable->active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    auto viewport_position = document->visual_viewport()->map_to_layout_viewport(visual_viewport_position);

    document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseDown);

    if (!paint_root())
        return EventResult::Dropped;

    GC::Ptr<DOM::Node> node;

    GC::Ptr<Painting::Paintable> paintable;
    GC::Ptr<Painting::ChromeWidget> chrome_widget;
    if (auto result = target_for_mouse_position(visual_viewport_position); result.has_value()) {
        paintable = result->paintable;
        chrome_widget = result->chrome_widget;
    } else {
        return EventResult::Dropped;
    }

    auto pointer_events = paintable->computed_values().pointer_events();
    // FIXME: Handle other values for pointer-events.
    VERIFY(pointer_events != CSS::PointerEvents::None);

    node = dom_node_for_event_dispatch(*paintable);

    if (!node)
        return EventResult::Dropped;

    m_mousedown_click_count = click_count;

    if (auto result = dispatch_event_to_nested_navigable(*paintable, visual_viewport_position, [=](EventHandler& event_handler, CSSPixelPoint position) -> EventResult {
            return event_handler.handle_mousedown(position, screen_position, button, buttons, modifiers, click_count);
        });
        result.has_value())
        return result.value();

    m_navigable->page().set_focused_navigable({}, m_navigable);

    // NB: Search for the first parent of the hit target that's an element.
    //
    // https://www.w3.org/TR/uievents/#event-type-click
    // The click event type MUST be dispatched on the topmost event target indicated by the pointer.
    //
    // https://www.w3.org/TR/uievents/#topmost-event-target
    // The topmost event target MUST be the element highest in the rendering order which is capable of being an
    // event target.
    GC::Ptr<Layout::Node> layout_node;
    if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
        return EventResult::Dropped;

    m_mousedown_button = button;
    m_mousedown_modifiers = modifiers;
    m_mousedown_target = node;
    m_mousedown_visual_viewport_position = visual_viewport_position;

    auto coordinates = compute_mouse_event_coordinates(visual_viewport_position, viewport_position, *paintable, *layout_node);
    if (!dispatch_a_pointer_event_for_a_device_that_supports_hover(PointerEventType::PointerDown, *node, chrome_widget, coordinates, screen_position, {}, button, buttons, modifiers, click_count))
        return EventResult::Cancelled;

    // FIXME: The spec allows this to be fired following mousedown or mouseup. The native behavior on Windows is to
    //        do so in mouseup, so we should make this configurable by the UI.
    if (button == UIEvents::MouseButton::Secondary)
        maybe_show_context_menu(*node, coordinates, screen_position, viewport_position, buttons, modifiers);
#if defined(AK_OS_MACOS)
    if (button == UIEvents::MouseButton::Primary && (modifiers & UIEvents::KeyModifier::Mod_Ctrl) != 0)
        maybe_show_context_menu(*node, coordinates, screen_position, viewport_position, buttons, modifiers);
#endif

    // NB: Dispatching an event may have disturbed the world.
    if (m_navigable->active_document() != document)
        return EventResult::Accepted;
    document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseDown);
    if (!paint_root())
        return EventResult::Accepted;

    if (!chrome_widget)
        run_mousedown_default_actions(*document, visual_viewport_position, viewport_position, button, modifiers, click_count);

    return EventResult::Handled;
}

EventResult EventHandler::handle_mousemove(CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, u32 buttons, u32 modifiers)
{
    if (should_ignore_device_input_event()) {
        if (is_dragging_element())
            return handle_drag_and_drop_event(DragEvent::Type::DragMove, visual_viewport_position, screen_position, UIEvents::MouseButton::Primary, buttons, modifiers, {});
        return EventResult::Dropped;
    }

    auto document = m_navigable->active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    auto viewport_position = document->visual_viewport()->map_to_layout_viewport(visual_viewport_position);
    document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseMove);

    if (!paint_root())
        return EventResult::Dropped;

    if (is_dragging_element()) {
        static constexpr CSSPixels DRAG_THRESHOLD = 5;
        auto delta = visual_viewport_position - *m_mousedown_visual_viewport_position;

        if (delta.x().abs() >= DRAG_THRESHOLD || delta.y().abs() >= DRAG_THRESHOLD) {
            auto result = handle_drag_and_drop_event(DragEvent::Type::DragStart, visual_viewport_position, screen_position, UIEvents::MouseButton::Primary, buttons, modifiers, {});

            if (result == EventResult::Handled) {
                set_page_cursor(m_navigable->page(), Gfx::StandardCursor::Drag);
                stop_updating_selection();

                return EventResult::Handled;
            }

            // NB: Dispatching an event may have disturbed the world.
            if (m_navigable->active_document() != document)
                return EventResult::Accepted;
            document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseMove);
            if (!paint_root())
                return EventResult::Accepted;
        }
    }

    GC::Ptr<Painting::Paintable> paintable;
    GC::Ptr<Painting::ChromeWidget> chrome_widget;
    Optional<int> start_index;

    if (auto result = target_for_mouse_position(visual_viewport_position); result.has_value()) {
        paintable = result->paintable;
        chrome_widget = result->chrome_widget;
        start_index = result->index_in_node;
    }

    ArmedScopeGuard clear_cursor = [&] {
        update_cursor(nullptr, nullptr, nullptr);
    };

    if (paintable) {
        auto node = dom_node_for_event_dispatch(*paintable);

        if (!node)
            return EventResult::Dropped;

        if (auto result = dispatch_event_to_nested_navigable(*paintable, visual_viewport_position, [screen_position, buttons, modifiers](EventHandler& event_handler, CSSPixelPoint position) -> EventResult {
                return event_handler.handle_mousemove(position, screen_position, buttons, modifiers);
            });
            result.has_value()) {
            clear_cursor.disarm();
            return result.value();
        }

        auto pointer_events = paintable->computed_values().pointer_events();
        // FIXME: Handle other values for pointer-events.
        VERIFY(pointer_events != CSS::PointerEvents::None);

        // NB: Search for the first parent of the hit target that's an element.
        //
        // https://www.w3.org/TR/uievents/#event-type-click
        // The click event type MUST be dispatched on the topmost event target indicated by the pointer.
        //
        // https://www.w3.org/TR/uievents/#topmost-event-target
        // The topmost event target MUST be the element highest in the rendering order which is capable of being an
        // event target.
        GC::Ptr<Layout::Node> layout_node;
        bool found_parent_element = parent_element_for_event_dispatch(*paintable, node, layout_node);

        if (found_parent_element) {
            update_cursor(*paintable, *node, chrome_widget);
            clear_cursor.disarm();

            auto coordinates = compute_mouse_event_coordinates(visual_viewport_position, viewport_position, *paintable, *layout_node);
            auto movement = compute_mouse_event_movement(screen_position);
            m_mousemove_previous_screen_position = screen_position;

            if (!dispatch_a_pointer_event_for_a_device_that_supports_hover(PointerEventType::PointerMove, *node, chrome_widget, coordinates, screen_position, movement, UIEvents::MouseButton::Primary, buttons, modifiers))
                return EventResult::Cancelled;
        }
    } else if (m_mousedown_target) {
        // NOTE: In some implementation environments, such as a browser, mousemove events can continue to fire if the
        //       user began a drag operation (e.g., a mouse button is pressed) and the pointing device has left the
        //       boundary of the user agent.
        auto coordinates = compute_mouse_event_coordinates(visual_viewport_position, viewport_position, *document->paintable(), *document->layout_node());
        auto movement = compute_mouse_event_movement(screen_position);
        m_mousemove_previous_screen_position = screen_position;

        if (!dispatch_a_pointer_event_for_a_device_that_supports_hover(PointerEventType::PointerMove, document->html_element(), nullptr, coordinates, screen_position, movement, UIEvents::MouseButton::Primary, buttons, modifiers))
            return EventResult::Cancelled;
    }

    // NB: Dispatching an event may have disturbed the world.
    if (m_navigable->active_document() != document)
        return EventResult::Accepted;
    document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseMove);
    if (!paint_root())
        return EventResult::Accepted;

    if (m_middle_button_scroll_handler)
        m_middle_button_scroll_handler->update_mouse_position(visual_viewport_position);

    update_mouse_selection(visual_viewport_position);
    return EventResult::Handled;
}

EventResult EventHandler::handle_mouseleave()
{
    if (should_ignore_device_input_event()) {
        if (is_dragging_element()) {
            auto result = handle_drag_and_drop_event(DragEvent::Type::DragEnd, {}, {}, UIEvents::MouseButton::Primary, UIEvents::MouseButton::Primary, 0, {});
            set_page_cursor(m_navigable->page(), Gfx::StandardCursor::Arrow);
            return result;
        }

        return EventResult::Dropped;
    }

    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    m_navigable->active_document()->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseMove);

    if (!paint_root())
        return EventResult::Dropped;

    update_hovered_chrome_widget(nullptr);
    update_cursor(nullptr, nullptr, nullptr);

    if (!m_mousedown_target)
        track_the_effective_position_of_the_legacy_mouse_pointer(nullptr);
    return EventResult::Handled;
}

EventResult EventHandler::handle_drag_and_drop_event(DragEvent::Type type, CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers, Vector<HTML::SelectedFile> files)
{
    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto& document = *m_navigable->active_document();
    auto viewport_position = document.visual_viewport()->map_to_layout_viewport(visual_viewport_position);

    document.update_layout(DOM::UpdateLayoutReason::EventHandlerHandleDragAndDrop);

    if (!paint_root())
        return EventResult::Dropped;

    GC::Ptr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(visual_viewport_position); result.has_value())
        paintable = result->paintable;
    else
        return EventResult::Dropped;

    auto node = dom_node_for_event_dispatch(*paintable);
    if (!node)
        return EventResult::Dropped;

    if (auto result = dispatch_event_to_nested_navigable(*paintable, visual_viewport_position, [type, screen_position, button, buttons, modifiers, &files](EventHandler& event_handler, CSSPixelPoint position) -> EventResult {
            return event_handler.handle_drag_and_drop_event(type, position, screen_position, button, buttons, modifiers, move(files));
        });
        result.has_value())
        return result.value();

    auto page_offset = compute_mouse_event_page_offset(viewport_position);
    auto scroll_offset = document.navigable()->viewport_scroll_offset();
    auto offset = compute_mouse_event_offset(visual_viewport_position.translated(scroll_offset), *paintable);

    switch (type) {
    case DragEvent::Type::DragStart:
        return m_drag_and_drop_event_handler->handle_drag_start(document.realm(), m_mousedown_target.ptr(), screen_position, page_offset, viewport_position, offset, button, buttons, modifiers, move(files));
    case DragEvent::Type::DragMove:
        return m_drag_and_drop_event_handler->handle_drag_move(document.realm(), *node, screen_position, page_offset, viewport_position, offset, button, buttons, modifiers);
    case DragEvent::Type::DragEnd:
        return m_drag_and_drop_event_handler->handle_drag_leave(document.realm(), screen_position, page_offset, viewport_position, offset, button, buttons, modifiers);
    case DragEvent::Type::Drop:
        return m_drag_and_drop_event_handler->handle_drop(document.realm(), screen_position, page_offset, viewport_position, offset, button, buttons, modifiers);
    }

    VERIFY_NOT_REACHED();
}

EventResult EventHandler::handle_pinch_event(CSSPixelPoint point, double scale_delta)
{
    auto document = m_navigable->active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    auto visual_viewport = document->visual_viewport();
    visual_viewport->zoom(point, scale_delta);
    return EventResult::Handled;
}

EventResult EventHandler::focus_next_element()
{
    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto set_focus_to_first_focusable_element = [&] {
        auto* element = m_navigable->active_document()->first_child_of_type<DOM::Element>();

        for (; element; element = element->next_element_in_pre_order()) {
            if (element->is_focusable()) {
                HTML::run_focusing_steps(element, nullptr, HTML::FocusTrigger::Key);
                return EventResult::Handled;
            }
        }

        return EventResult::Dropped;
    };

    auto node = m_navigable->active_document()->focused_area();
    if (!node)
        return set_focus_to_first_focusable_element();

    for (node = node->next_in_pre_order(); node && !node->is_focusable(); node = node->next_in_pre_order())
        ;

    if (!node)
        return set_focus_to_first_focusable_element();

    HTML::run_focusing_steps(node, nullptr, HTML::FocusTrigger::Key);
    return EventResult::Handled;
}

EventResult EventHandler::focus_previous_element()
{
    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto set_focus_to_last_focusable_element = [&] {
        // FIXME: This often returns the HTML element itself, which has no previous sibling.
        auto* element = m_navigable->active_document()->last_child_of_type<DOM::Element>();

        for (; element; element = element->previous_element_in_pre_order()) {
            if (element->is_focusable()) {
                HTML::run_focusing_steps(element, nullptr, HTML::FocusTrigger::Key);
                return EventResult::Handled;
            }
        }

        return EventResult::Dropped;
    };

    auto node = m_navigable->active_document()->focused_area();
    if (!node)
        return set_focus_to_last_focusable_element();

    for (node = node->previous_in_pre_order(); node && !node->is_focusable(); node = node->previous_in_pre_order())
        ;

    if (!node)
        return set_focus_to_last_focusable_element();

    HTML::run_focusing_steps(node, nullptr, HTML::FocusTrigger::Key);
    return EventResult::Handled;
}

GC::Ptr<DOM::Node> EventHandler::focus_candidate_for_position(CSSPixelPoint visual_viewport_position) const
{
    auto exact_hit = paint_root()->hit_test(visual_viewport_position, Painting::HitTestType::Exact);
    if (!exact_hit.has_value())
        return {};

    auto focus_dom_node = exact_hit->paintable ? exact_hit->paintable->dom_node() : nullptr;

    while (focus_dom_node && !focus_dom_node->is_focusable())
        focus_dom_node = focus_dom_node->parent_or_shadow_host();

    return focus_dom_node;
}

static constexpr bool should_ignore_keydown_event(u32 code_point, u32 modifiers)
{
    if (modifiers & (UIEvents::KeyModifier::Mod_Ctrl | UIEvents::KeyModifier::Mod_Alt | UIEvents::KeyModifier::Mod_Super))
        return true;

    // FIXME: There are probably also keys with non-zero code points that should be filtered out.
    return code_point == 0 || code_point == 27;
}

EventResult EventHandler::fire_keyboard_event(FlyString const& event_name, HTML::Navigable& navigable, UIEvents::KeyCode key, u32 modifiers, u32 code_point, bool repeat)
{
    GC::Ptr<DOM::Document> document = navigable.active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    if (GC::Ptr focused_area = document->focused_area()) {
        if (is<HTML::NavigableContainer>(*focused_area)) {
            auto& navigable_container = as<HTML::NavigableContainer>(*focused_area);
            if (navigable_container.content_navigable())
                return fire_keyboard_event(event_name, *navigable_container.content_navigable(), key, modifiers, code_point, repeat);
        }

        auto event = UIEvents::KeyboardEvent::create_from_platform_event(document->realm(), event_name, key, modifiers, code_point, repeat);
        return focused_area->dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;
    }

    // FIXME: De-duplicate this. This is just to prevent wasting a KeyboardEvent allocation when recursing into an (i)frame.
    auto event = UIEvents::KeyboardEvent::create_from_platform_event(document->realm(), event_name, key, modifiers, code_point, repeat);

    GC::Ptr target = document->body() ?: &document->root();
    return target->dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;
}

// https://w3c.github.io/uievents/#unicode-character-categories
static bool produces_character_value(u32 code_point)
{
    // A subset of the General Category values that are defined for each Unicode code point. This subset contains all
    // the Letter (Ll, Lm, Lo, Lt, Lu), Number (Nd, Nl, No), Punctuation (Pc, Pd, Pe, Pf, Pi, Po, Ps) and Symbol (Sc,
    // Sk, Sm, So) category values.
    return Unicode::code_point_has_letter_general_category(code_point)
        || Unicode::code_point_has_number_general_category(code_point)
        || Unicode::code_point_has_punctuation_general_category(code_point)
        || Unicode::code_point_has_symbol_general_category(code_point);
}

// https://github.com/w3c/uievents/issues/183#issuecomment-448091687
static bool is_enter_key_or_interoperable_enter_key_combo(UIEvents::KeyCode key, u32 modifiers)
{
    if (key != UIEvents::KeyCode::Key_Return)
        return false;
    if (!modifiers)
        return true;
    if (modifiers & (UIEvents::KeyModifier::Mod_Shift | UIEvents::KeyModifier::Mod_Ctrl))
        return true;
    return false;
}

static GC::RootVector<GC::Ref<DOM::StaticRange>> target_ranges_for_input_event(DOM::Document const& document)
{
    GC::RootVector<GC::Ref<DOM::StaticRange>> target_ranges { document.heap() };
    if (auto selection = document.get_selection(); selection && !selection->is_collapsed()) {
        if (auto range = selection->range()) {
            auto static_range = document.realm().create<DOM::StaticRange>(range->start_container(), range->start_offset(), range->end_container(), range->end_offset());
            target_ranges.append(static_range);
        }
    }
    return target_ranges;
}

EventResult EventHandler::input_event(FlyString const& event_name, FlyString const& input_type, HTML::Navigable& navigable, Variant<u32, Utf16String> code_point_or_string)
{
    auto document = navigable.active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    UIEvents::InputEventInit input_event_init;

    code_point_or_string.visit(
        [&](u32 code_point) {
            if (!is_unicode_control(code_point))
                input_event_init.data = Utf16String::from_code_point(code_point);
        },
        [&](Utf16String const& string) {
            input_event_init.data = string;
        });

    input_event_init.input_type = input_type;

    if (auto focused_area = document->focused_area()) {
        if (is<HTML::NavigableContainer>(*focused_area)) {
            auto& navigable_container = as<HTML::NavigableContainer>(*focused_area);
            if (navigable_container.content_navigable())
                return input_event(event_name, input_type, *navigable_container.content_navigable(), move(code_point_or_string));
        }

        auto event = UIEvents::InputEvent::create_from_platform_event(document->realm(), event_name, input_event_init, target_ranges_for_input_event(*document));
        return focused_area->dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;
    }

    auto event = UIEvents::InputEvent::create_from_platform_event(document->realm(), event_name, input_event_init, target_ranges_for_input_event(*document));

    if (auto* body = document->body())
        return body->dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;

    return document->root().dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;
}

EventResult EventHandler::handle_keydown(UIEvents::KeyCode key, u32 modifiers, u32 code_point, bool repeat)
{
    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto dispatch_result = fire_keyboard_event(UIEvents::EventNames::keydown, m_navigable, key, modifiers, code_point, repeat);
    if (dispatch_result != EventResult::Accepted)
        return dispatch_result;

    // https://w3c.github.io/uievents/#event-type-keypress
    // If supported by a user agent, this event MUST be dispatched when a key is pressed down, if and only if that key
    // normally produces a character value.
    // AD-HOC: For web compat and for interop with other engines, we make an exception here for the Enter key. See:
    //         https://github.com/w3c/uievents/issues/183#issuecomment-448091687 and
    //         https://github.com/w3c/uievents/issues/266#issuecomment-1887917756
    if (produces_character_value(code_point) || is_enter_key_or_interoperable_enter_key_combo(key, modifiers)) {
        dispatch_result = fire_keyboard_event(UIEvents::EventNames::keypress, m_navigable, key, modifiers, code_point, repeat);
        if (dispatch_result != EventResult::Accepted)
            return dispatch_result;
    }

    GC::Ref<DOM::Document> document = *m_navigable->active_document();

    if (!(modifiers & UIEvents::KeyModifier::Mod_Ctrl)) {
        if (key == UIEvents::KeyCode::Key_Tab) {
            return modifiers & UIEvents::KeyModifier::Mod_Shift ? focus_previous_element() : focus_next_element();
        }
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#close-requests
    // FIXME: Close requests should queue a global task on the user interaction task source, given `document`'s relevant global object.
    if (key == UIEvents::KeyCode::Key_Escape) {
        // 1. If document's fullscreen element is not null, then:
        if (document->fullscreen()) {
            // 1. Fully exit fullscreen given document's node navigable's top-level traversable's active document.
            m_navigable->top_level_traversable()->active_document()->fully_exit_fullscreen();
            // 2. Return.
            return EventResult::Handled;
        }

        // 7. Let closedSomething be the result of processing close watchers on document's relevant global object.
        auto closed_something = document->window()->close_watcher_manager()->process_close_watchers();

        // 8. If closedSomething is true, then return.
        if (closed_something)
            return EventResult::Handled;

        // 9. Alternative processing: Otherwise, there was nothing watching for a close request. The user agent may
        //    instead interpret this interaction as some other action, instead of interpreting it as a close request.
    }

    auto* target = document->active_input_events_target();
    if (target) {
        if (key == UIEvents::KeyCode::Key_Backspace) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::deleteContentBackward, m_navigable, code_point));
            target->handle_delete(UIEvents::InputTypes::deleteContentBackward);
            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_Delete) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::deleteContentForward, m_navigable, code_point));
            target->handle_delete(UIEvents::InputTypes::deleteContentForward);
            return EventResult::Handled;
        }

#if defined(AK_OS_MACOS)
        if ((modifiers & UIEvents::Mod_Super) != 0) {
            if (key == UIEvents::KeyCode::Key_Left) {
                key = UIEvents::KeyCode::Key_Home;
                modifiers &= ~UIEvents::Mod_Super;
            }
            if (key == UIEvents::KeyCode::Key_Right) {
                key = UIEvents::KeyCode::Key_End;
                modifiers &= ~UIEvents::Mod_Super;
            }
        }
#endif

        if (key == UIEvents::KeyCode::Key_Left || key == UIEvents::KeyCode::Key_Right) {
            auto collapse = modifiers & UIEvents::Mod_Shift ? InputEventsTarget::CollapseSelection::No : InputEventsTarget::CollapseSelection::Yes;
            if ((modifiers & UIEvents::Mod_PlatformWordJump) == 0) {
                if (key == UIEvents::KeyCode::Key_Left) {
                    target->decrement_cursor_position_offset(collapse);
                } else {
                    target->increment_cursor_position_offset(collapse);
                }
            } else {
                if (key == UIEvents::KeyCode::Key_Left) {
                    target->decrement_cursor_position_to_previous_word(collapse);
                } else {
                    target->increment_cursor_position_to_next_word(collapse);
                }
            }
            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_Up || key == UIEvents::KeyCode::Key_Down) {
            auto collapse = modifiers & UIEvents::Mod_Shift ? InputEventsTarget::CollapseSelection::No : InputEventsTarget::CollapseSelection::Yes;
            if (key == UIEvents::KeyCode::Key_Up) {
                target->decrement_cursor_position_to_previous_line(collapse);
            } else {
                target->increment_cursor_position_to_next_line(collapse);
            }
            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_Home) {
            auto collapse = modifiers & UIEvents::Mod_Shift ? InputEventsTarget::CollapseSelection::No : InputEventsTarget::CollapseSelection::Yes;
            target->move_cursor_to_start(collapse);
            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_End) {
            auto collapse = modifiers & UIEvents::Mod_Shift ? InputEventsTarget::CollapseSelection::No : InputEventsTarget::CollapseSelection::Yes;
            target->move_cursor_to_end(collapse);
            return EventResult::Handled;
        }

        // Ignore Mod_Keypad when determining behavior - it only indicates key location (numpad vs standard).
        auto const significant_modifiers = modifiers & ~UIEvents::Mod_Keypad;
        if (key == UIEvents::KeyCode::Key_Return && (significant_modifiers == UIEvents::Mod_None || significant_modifiers == UIEvents::Mod_Shift)) {
            auto input_type = significant_modifiers == UIEvents::Mod_Shift ? UIEvents::InputTypes::insertLineBreak : UIEvents::InputTypes::insertParagraph;

            // Form controls always use insertLineBreak rather than insertParagraph.
            if (is<HTML::FormAssociatedTextControlElement>(target)) {
                input_type = UIEvents::InputTypes::insertLineBreak;
            }
            // If the editing host is contenteditable="plaintext-only", we force a line break.
            // NB: We check the selection's editing host rather than focused_area because with nested
            //     contenteditable elements, the focused element may differ from where the selection is.
            else if (auto selection = document->get_selection(); selection && selection->range()) {
                if (auto editing_host = selection->range()->start_container()->editing_host(); editing_host
                    && as<HTML::HTMLElement>(*editing_host).content_editable_state() == HTML::ContentEditableState::PlaintextOnly) {
                    input_type = UIEvents::InputTypes::insertLineBreak;
                }
            }

            FIRE(input_event(UIEvents::EventNames::beforeinput, input_type, m_navigable, code_point));
            if (target->handle_return_key(input_type) != EventResult::Handled)
                target->handle_insert(input_type, Utf16String::from_code_point(code_point));

            return EventResult::Handled;
        }

        // FIXME: Text editing shortcut keys (copy/paste etc.) should be handled here.
        if (!should_ignore_keydown_event(code_point, modifiers)) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::insertText, m_navigable, code_point));
            target->handle_insert(UIEvents::InputTypes::insertText, Utf16String::from_code_point(code_point));
            return EventResult::Handled;
        }
    } else if (auto selection = document->get_selection(); selection && !selection->is_collapsed()) {
        if (modifiers & UIEvents::Mod_Shift) {
            if (key == UIEvents::KeyCode::Key_Right) {
                if (modifiers & UIEvents::Mod_PlatformWordJump)
                    selection->move_offset_to_next_word(false);
                else
                    selection->move_offset_to_next_character(false);
                return EventResult::Handled;
            }
            if (key == UIEvents::KeyCode::Key_Left) {
                if (modifiers & UIEvents::Mod_PlatformWordJump)
                    selection->move_offset_to_previous_word(false);
                else
                    selection->move_offset_to_previous_character(false);
                return EventResult::Handled;
            }
        }
    }

    // FIXME: Implement scroll by line and by page instead of approximating the behavior of other browsers.
    auto arrow_key_scroll_distance = 100;
    auto page_scroll_distance = document->window()->inner_height() - (document->window()->outer_height() - document->window()->inner_height());

    switch (key) {
    case UIEvents::KeyCode::Key_Up:
    case UIEvents::KeyCode::Key_Down:
        if (modifiers && modifiers != UIEvents::KeyModifier::Mod_PlatformCtrl)
            break;
        if (modifiers) {
            if (key == UIEvents::KeyCode::Key_Up)
                document->scroll_to_the_beginning_of_the_document();
            else
                document->window()->scroll_by(0, INT64_MAX);
        } else {
            document->window()->scroll_by(0, key == UIEvents::KeyCode::Key_Up ? -arrow_key_scroll_distance : arrow_key_scroll_distance);
        }
        return EventResult::Handled;
    case UIEvents::KeyCode::Key_Left:
    case UIEvents::KeyCode::Key_Right:
#if defined(AK_OS_MACOS)
        if (modifiers && modifiers != UIEvents::KeyModifier::Mod_Super)
#else
        if (modifiers && modifiers != UIEvents::KeyModifier::Mod_Alt)
#endif
            break;
        if (modifiers)
            document->page().traverse_the_history_by_delta(key == UIEvents::KeyCode::Key_Left ? -1 : 1);
        else
            document->window()->scroll_by(key == UIEvents::KeyCode::Key_Left ? -arrow_key_scroll_distance : arrow_key_scroll_distance, 0);
        return EventResult::Handled;
    case UIEvents::KeyCode::Key_PageUp:
    case UIEvents::KeyCode::Key_PageDown:
        if (modifiers != UIEvents::KeyModifier::Mod_None)
            break;
        document->window()->scroll_by(0, key == UIEvents::KeyCode::Key_PageUp ? -page_scroll_distance : page_scroll_distance);
        return EventResult::Handled;
    case UIEvents::KeyCode::Key_Home:
        document->scroll_to_the_beginning_of_the_document();
        return EventResult::Handled;
    case UIEvents::KeyCode::Key_End:
        document->window()->scroll_by(0, INT64_MAX);
        return EventResult::Handled;
    default:
        break;
    }

    return EventResult::Accepted;
}

EventResult EventHandler::handle_keyup(UIEvents::KeyCode key, u32 modifiers, u32 code_point, bool repeat)
{
    // Keyup events as a result of auto-repeat are not fired.
    // See: https://w3c.github.io/uievents/#events-keyboard-event-order
    if (repeat)
        return EventResult::Dropped;

    return fire_keyboard_event(UIEvents::EventNames::keyup, m_navigable, key, modifiers, code_point, false);
}

EventResult EventHandler::handle_paste(Utf16String const& text)
{
    auto active_document = m_navigable->active_document();
    if (!active_document)
        return EventResult::Dropped;
    if (!active_document->is_fully_active())
        return EventResult::Dropped;

    auto* target = active_document->active_input_events_target();
    if (!target)
        return EventResult::Dropped;

    FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::insertFromPaste, m_navigable, text));
    target->handle_insert(UIEvents::InputTypes::insertFromPaste, text);

    return EventResult::Handled;
}

void EventHandler::handle_gamepad_connected(SDL_JoystickID sdl_joystick_id)
{
    auto active_document = m_navigable->active_document();
    if (active_document)
        active_document->window()->navigator()->handle_gamepad_connected(sdl_joystick_id);

    for (auto const& child_navigable : m_navigable->child_navigables())
        child_navigable->event_handler().handle_gamepad_connected(sdl_joystick_id);
}

void EventHandler::handle_gamepad_updated(SDL_JoystickID sdl_joystick_id)
{
    auto active_document = m_navigable->active_document();
    if (active_document)
        active_document->window()->navigator()->handle_gamepad_updated({}, sdl_joystick_id);

    for (auto const& child_navigable : m_navigable->child_navigables())
        child_navigable->event_handler().handle_gamepad_updated(sdl_joystick_id);
}

void EventHandler::handle_gamepad_disconnected(SDL_JoystickID sdl_joystick_id)
{
    auto active_document = m_navigable->active_document();
    if (active_document)
        active_document->window()->navigator()->handle_gamepad_disconnected({}, sdl_joystick_id);

    for (auto const& child_navigable : m_navigable->child_navigables())
        child_navigable->event_handler().handle_gamepad_disconnected(sdl_joystick_id);
}

void EventHandler::handle_sdl_input_events()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            handle_gamepad_connected(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
            handle_gamepad_updated(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            handle_gamepad_disconnected(event.gdevice.which);
            break;
        default:
            break;
        }
    }
}

CSSPixelPoint EventHandler::compute_mouse_event_page_offset(CSSPixelPoint event_client_offset) const
{
    // https://w3c.github.io/csswg-drafts/cssom-view/#dom-mouseevent-pagex
    // FIXME: 1. If the event’s dispatch flag is set, return the horizontal coordinate of the position where the event occurred
    //           relative to the origin of the initial containing block and terminate these steps.

    // 2. Let offset be the value of the scrollX attribute of the event’s associated Window object, if there is one, or zero otherwise.
    auto scroll_offset = m_navigable->active_document()->navigable()->viewport_scroll_offset();

    // 3. Return the sum of offset and the value of the event’s clientX attribute.
    return event_client_offset.translated(scroll_offset);
}

CSSPixelPoint EventHandler::compute_mouse_event_movement(CSSPixelPoint screen_position) const
{
    // https://w3c.github.io/pointerlock/#dom-mouseevent-movementx
    // The attributes movementX movementY must provide the change in position of the pointer, as if the values of
    // screenX, screenY, were stored between two subsequent mousemove events eNow and ePrevious and the difference taken
    // movementX = eNow.screenX-ePrevious.screenX.

    if (!m_mousemove_previous_screen_position.has_value())
        // When unlocked, the system cursor can exit and re-enter the user agent window. If it does so and the user agent
        // was not the target of operating system mouse move events then the most recent pointer position will be unknown
        // to the user agent and movementX/movementY can not be computed and must be set to zero.
        // FIXME: For this to actually work, m_mousemove_previous_client_offset needs to be cleared when the mouse leaves
        //        the window.
        return { 0, 0 };

    return { screen_position.x() - m_mousemove_previous_screen_position.value().x(), screen_position.y() - m_mousemove_previous_screen_position.value().y() };
}

Optional<EventHandler::Target> EventHandler::target_for_mouse_position(CSSPixelPoint position)
{
    if (auto result = paint_root()->hit_test(position, Painting::HitTestType::Exact); result.has_value())
        return Target { .paintable = result->paintable.ptr(), .chrome_widget = result->chrome_widget, .index_in_node = result->index_in_node };

    return {};
}

bool EventHandler::should_ignore_device_input_event() const
{
    // From the moment that the user agent is to initiate the drag-and-drop operation, until the end of the drag-and-drop
    // operation, device input events (e.g. mouse and keyboard events) must be suppressed.
    return m_drag_and_drop_event_handler->has_ongoing_drag_and_drop_operation();
}

bool EventHandler::is_dragging_element() const
{
    return m_mousedown_target
        && m_mousedown_visual_viewport_position.has_value()
        && m_mousedown_button == UIEvents::MouseButton::Primary
        && (m_mousedown_modifiers & UIEvents::KeyModifier::Mod_Ctrl) == 0;
}

void EventHandler::visit_edges(JS::Cell::Visitor& visitor) const
{
    m_drag_and_drop_event_handler->visit_edges(visitor);
    visitor.visit(m_hovered_chrome_widget);
    visitor.visit(m_captured_chrome_widget);
    if (m_mouse_selection_target)
        visitor.visit(m_mouse_selection_target->as_cell());
    visitor.visit(m_selection_origin);
    visitor.visit(m_navigable);
    if (m_auto_scroll_handler)
        m_auto_scroll_handler->visit_edges(visitor);
    if (m_middle_button_scroll_handler)
        m_middle_button_scroll_handler->visit_edges(visitor);
}

Unicode::Segmenter& EventHandler::word_segmenter()
{
    if (!m_word_segmenter)
        m_word_segmenter = m_navigable->active_document()->word_segmenter().clone();
    return *m_word_segmenter;
}

}
