/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CloseWatcherManager.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/DragAndDropEventHandler.h>
#include <LibWeb/Page/EditEventHandler.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWeb/UIEvents/InputTypes.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/WheelEvent.h>

namespace Web {

#define FIRE(expression)                                                          \
    if (auto event_result = (expression); event_result == EventResult::Cancelled) \
        return event_result;

static JS::GCPtr<DOM::Node> dom_node_for_event_dispatch(Painting::Paintable& paintable)
{
    if (auto node = paintable.mouse_event_target())
        return node;
    if (auto node = paintable.dom_node())
        return node;
    auto* layout_parent = paintable.layout_node().parent();
    while (layout_parent) {
        if (auto* node = layout_parent->dom_node())
            return node;
        layout_parent = layout_parent->parent();
    }
    return nullptr;
}

static bool parent_element_for_event_dispatch(Painting::Paintable& paintable, JS::GCPtr<DOM::Node>& node, Layout::Node*& layout_node)
{
    auto* current_ancestor_node = node.ptr();
    do {
        if (is<HTML::FormAssociatedElement>(current_ancestor_node) && !dynamic_cast<HTML::FormAssociatedElement*>(current_ancestor_node)->enabled()) {
            return false;
        }
    } while ((current_ancestor_node = current_ancestor_node->parent()));

    layout_node = &paintable.layout_node();
    while (layout_node && node && !node->is_element() && layout_node->parent()) {
        layout_node = layout_node->parent();
        if (layout_node->is_anonymous())
            continue;
        node = layout_node->dom_node();
    }
    return node && layout_node;
}

static Gfx::StandardCursor cursor_css_to_gfx(Optional<CSS::Cursor> cursor)
{
    if (!cursor.has_value()) {
        return Gfx::StandardCursor::None;
    }
    switch (cursor.value()) {
    case CSS::Cursor::Crosshair:
    case CSS::Cursor::Cell:
        return Gfx::StandardCursor::Crosshair;
    case CSS::Cursor::Grab:
    case CSS::Cursor::Grabbing:
        return Gfx::StandardCursor::Drag;
    case CSS::Cursor::Pointer:
        return Gfx::StandardCursor::Hand;
    case CSS::Cursor::Help:
        return Gfx::StandardCursor::Help;
    case CSS::Cursor::None:
        return Gfx::StandardCursor::Hidden;
    case CSS::Cursor::NotAllowed:
        return Gfx::StandardCursor::Disallowed;
    case CSS::Cursor::Text:
    case CSS::Cursor::VerticalText:
        return Gfx::StandardCursor::IBeam;
    case CSS::Cursor::Move:
    case CSS::Cursor::AllScroll:
        return Gfx::StandardCursor::Move;
    case CSS::Cursor::Progress:
    case CSS::Cursor::Wait:
        return Gfx::StandardCursor::Wait;
    case CSS::Cursor::ColResize:
        return Gfx::StandardCursor::ResizeColumn;
    case CSS::Cursor::EResize:
    case CSS::Cursor::WResize:
    case CSS::Cursor::EwResize:
        return Gfx::StandardCursor::ResizeHorizontal;
    case CSS::Cursor::RowResize:
        return Gfx::StandardCursor::ResizeRow;
    case CSS::Cursor::NResize:
    case CSS::Cursor::SResize:
    case CSS::Cursor::NsResize:
        return Gfx::StandardCursor::ResizeVertical;
    case CSS::Cursor::NeResize:
    case CSS::Cursor::SwResize:
    case CSS::Cursor::NeswResize:
        return Gfx::StandardCursor::ResizeDiagonalBLTR;
    case CSS::Cursor::NwResize:
    case CSS::Cursor::SeResize:
    case CSS::Cursor::NwseResize:
        return Gfx::StandardCursor::ResizeDiagonalTLBR;
    case CSS::Cursor::ZoomIn:
    case CSS::Cursor::ZoomOut:
        return Gfx::StandardCursor::Zoom;
    case CSS::Cursor::ContextMenu:
    case CSS::Cursor::Alias:
    case CSS::Cursor::Copy:
    case CSS::Cursor::NoDrop:
        // FIXME: No corresponding GFX Standard Cursor, fallthrough to None
    case CSS::Cursor::Auto:
    case CSS::Cursor::Default:
    default:
        return Gfx::StandardCursor::None;
    }
}

static CSSPixelPoint compute_mouse_event_offset(CSSPixelPoint position, Layout::Node const& layout_node)
{
    auto top_left_of_layout_node = layout_node.first_paintable()->box_type_agnostic_position();
    return {
        position.x() - top_left_of_layout_node.x(),
        position.y() - top_left_of_layout_node.y()
    };
}

EventHandler::EventHandler(Badge<HTML::Navigable>, HTML::Navigable& navigable)
    : m_navigable(navigable)
    , m_edit_event_handler(make<EditEventHandler>())
    , m_drag_and_drop_event_handler(make<DragAndDropEventHandler>())
{
}

EventHandler::~EventHandler() = default;

Painting::PaintableBox* EventHandler::paint_root()
{
    if (!m_navigable->active_document())
        return nullptr;
    return m_navigable->active_document()->paintable_box();
}

Painting::PaintableBox const* EventHandler::paint_root() const
{
    if (!m_navigable->active_document())
        return nullptr;
    return m_navigable->active_document()->paintable_box();
}

EventResult EventHandler::handle_mousewheel(CSSPixelPoint viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers, int wheel_delta_x, int wheel_delta_y)
{
    if (should_ignore_device_input_event())
        return EventResult::Dropped;

    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto position = viewport_position;

    m_navigable->active_document()->update_layout();

    if (!paint_root())
        return EventResult::Dropped;

    if (modifiers & UIEvents::KeyModifier::Mod_Shift)
        swap(wheel_delta_x, wheel_delta_y);

    auto handled_event = EventResult::Dropped;

    JS::GCPtr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(position); result.has_value())
        paintable = result->paintable;

    if (paintable) {
        auto* containing_block = paintable->containing_block();
        while (containing_block) {
            auto handled_scroll_event = containing_block->handle_mousewheel({}, position, buttons, modifiers, wheel_delta_x, wheel_delta_y);
            if (handled_scroll_event)
                return EventResult::Handled;

            containing_block = containing_block->containing_block();
        }

        if (paintable->handle_mousewheel({}, position, buttons, modifiers, wheel_delta_x, wheel_delta_y))
            return EventResult::Handled;

        auto node = dom_node_for_event_dispatch(*paintable);

        if (node) {
            // FIXME: Support wheel events in nested browsing contexts.
            if (is<HTML::HTMLIFrameElement>(*node)) {
                auto& iframe = static_cast<HTML::HTMLIFrameElement&>(*node);
                auto position_in_iframe = position.translated(compute_mouse_event_offset({}, paintable->layout_node()));
                iframe.content_navigable()->event_handler().handle_mousewheel(position_in_iframe, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y);
                return EventResult::Dropped;
            }

            // Search for the first parent of the hit target that's an element.
            Layout::Node* layout_node;
            if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
                return EventResult::Dropped;

            auto offset = compute_mouse_event_offset(position, *layout_node);
            auto client_offset = compute_mouse_event_client_offset(position);
            auto page_offset = compute_mouse_event_page_offset(client_offset);
            if (node->dispatch_event(UIEvents::WheelEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::wheel, screen_position, page_offset, client_offset, offset, wheel_delta_x, wheel_delta_y, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors())) {
                m_navigable->active_window()->scroll_by(wheel_delta_x, wheel_delta_y);
            }

            handled_event = EventResult::Handled;
        }
    }

    return handled_event;
}

EventResult EventHandler::handle_mouseup(CSSPixelPoint viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers)
{
    if (should_ignore_device_input_event())
        return EventResult::Dropped;

    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto position = viewport_position;

    m_navigable->active_document()->update_layout();

    if (!paint_root())
        return EventResult::Dropped;

    JS::GCPtr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(position); result.has_value())
        paintable = result->paintable;

    if (paintable && paintable->wants_mouse_events()) {
        if (paintable->handle_mouseup({}, position, button, modifiers) == Painting::Paintable::DispatchEventOfSameName::No)
            return EventResult::Cancelled;

        // Things may have changed as a consequence of Layout::Node::handle_mouseup(). Hit test again.
        if (!paint_root())
            return EventResult::Handled;

        if (auto result = paint_root()->hit_test(position, Painting::HitTestType::Exact); result.has_value())
            paintable = result->paintable;
    }

    auto handled_event = EventResult::Dropped;

    if (paintable) {
        auto node = dom_node_for_event_dispatch(*paintable);

        if (node) {
            if (is<HTML::HTMLIFrameElement>(*node)) {
                if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
                    return content_navigable->event_handler().handle_mouseup(position.translated(compute_mouse_event_offset({}, paintable->layout_node())), screen_position, button, buttons, modifiers);
                return EventResult::Dropped;
            }

            // Search for the first parent of the hit target that's an element.
            // "The click event type MUST be dispatched on the topmost event target indicated by the pointer." (https://www.w3.org/TR/uievents/#event-type-click)
            // "The topmost event target MUST be the element highest in the rendering order which is capable of being an event target." (https://www.w3.org/TR/uievents/#topmost-event-target)
            Layout::Node* layout_node;
            if (!parent_element_for_event_dispatch(*paintable, node, layout_node)) {
                // FIXME: This is pretty ugly but we need to bail out here.
                goto after_node_use;
            }

            auto offset = compute_mouse_event_offset(position, *layout_node);
            auto client_offset = compute_mouse_event_client_offset(position);
            auto page_offset = compute_mouse_event_page_offset(client_offset);
            node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::mouseup, screen_position, page_offset, client_offset, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
            handled_event = EventResult::Handled;

            bool run_activation_behavior = false;
            if (node.ptr() == m_mousedown_target) {
                if (button == UIEvents::MouseButton::Primary) {
                    run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::click, screen_position, page_offset, client_offset, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
                } else if (button == UIEvents::MouseButton::Middle) {
                    run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::auxclick, screen_position, page_offset, client_offset, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
                } else if (button == UIEvents::MouseButton::Secondary) {
                    // Allow the user to bypass custom context menus by holding shift, like Firefox.
                    if ((modifiers & UIEvents::Mod_Shift) == 0)
                        run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::contextmenu, screen_position, page_offset, client_offset, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
                    else
                        run_activation_behavior = true;
                }
            }

            if (run_activation_behavior) {
                // FIXME: Currently cannot spawn a new top-level
                //        browsing context for new tab operations, because the new
                //        top-level browsing context would be in another process. To
                //        fix this, there needs to be some way to be able to
                //        communicate with browsing contexts in remote WebContent
                //        processes, and then step 8 of this algorithm needs to be
                //        implemented in Navigable::choose_a_navigable:
                //
                //        https://html.spec.whatwg.org/multipage/document-sequences.html#the-rules-for-choosing-a-navigable

                if (JS::GCPtr<HTML::HTMLAnchorElement const> link = node->enclosing_link_element()) {
                    JS::NonnullGCPtr<DOM::Document> document = *m_navigable->active_document();
                    auto href = link->href();
                    auto url = document->parse_url(href);

                    if (button == UIEvents::MouseButton::Primary && (modifiers & UIEvents::Mod_PlatformCtrl) != 0) {
                        m_navigable->page().client().page_did_click_link(url, link->target().to_byte_string(), modifiers);
                    } else if (button == UIEvents::MouseButton::Middle) {
                        m_navigable->page().client().page_did_middle_click_link(url, link->target().to_byte_string(), modifiers);
                    } else if (button == UIEvents::MouseButton::Secondary) {
                        m_navigable->page().client().page_did_request_link_context_menu(viewport_position, url, link->target().to_byte_string(), modifiers);
                    }
                } else if (button == UIEvents::MouseButton::Secondary) {
                    if (is<HTML::HTMLImageElement>(*node)) {
                        auto& image_element = verify_cast<HTML::HTMLImageElement>(*node);
                        auto image_url = image_element.document().parse_url(image_element.src());
                        m_navigable->page().client().page_did_request_image_context_menu(viewport_position, image_url, "", modifiers, image_element.bitmap());
                    } else if (is<HTML::HTMLMediaElement>(*node)) {
                        auto& media_element = verify_cast<HTML::HTMLMediaElement>(*node);

                        Page::MediaContextMenu menu {
                            .media_url = media_element.document().parse_url(media_element.current_src()),
                            .is_video = is<HTML::HTMLVideoElement>(*node),
                            .is_playing = media_element.potentially_playing(),
                            .is_muted = media_element.muted(),
                            .has_user_agent_controls = media_element.has_attribute(HTML::AttributeNames::controls),
                            .is_looping = media_element.has_attribute(HTML::AttributeNames::loop),
                        };

                        m_navigable->page().did_request_media_context_menu(media_element.unique_id(), viewport_position, "", modifiers, move(menu));
                    } else {
                        m_navigable->page().client().page_did_request_context_menu(viewport_position);
                    }
                }
            }
        }
    }

after_node_use:
    if (button == UIEvents::MouseButton::Primary) {
        m_in_mouse_selection = false;
        update_selection_range_for_input_or_textarea();
    }
    return handled_event;
}

EventResult EventHandler::handle_mousedown(CSSPixelPoint viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers)
{
    if (should_ignore_device_input_event())
        return EventResult::Dropped;

    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto position = viewport_position;

    m_navigable->active_document()->update_layout();

    if (!paint_root())
        return EventResult::Dropped;

    JS::NonnullGCPtr<DOM::Document> document = *m_navigable->active_document();
    JS::GCPtr<DOM::Node> node;

    {
        JS::GCPtr<Painting::Paintable> paintable;
        if (auto result = target_for_mouse_position(position); result.has_value())
            paintable = result->paintable;
        else
            return EventResult::Dropped;

        auto pointer_events = paintable->computed_values().pointer_events();
        // FIXME: Handle other values for pointer-events.
        VERIFY(pointer_events != CSS::PointerEvents::None);

        node = dom_node_for_event_dispatch(*paintable);
        document->set_hovered_node(node);

        if (paintable->wants_mouse_events()) {
            if (paintable->handle_mousedown({}, position, button, modifiers) == Painting::Paintable::DispatchEventOfSameName::No)
                return EventResult::Cancelled;
        }

        if (!node)
            return EventResult::Dropped;

        if (is<HTML::HTMLIFrameElement>(*node)) {
            if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
                return content_navigable->event_handler().handle_mousedown(position.translated(compute_mouse_event_offset({}, paintable->layout_node())), screen_position, button, buttons, modifiers);
            return EventResult::Dropped;
        }

        m_navigable->page().set_focused_navigable({}, m_navigable);

        // Search for the first parent of the hit target that's an element.
        // "The click event type MUST be dispatched on the topmost event target indicated by the pointer." (https://www.w3.org/TR/uievents/#event-type-click)
        // "The topmost event target MUST be the element highest in the rendering order which is capable of being an event target." (https://www.w3.org/TR/uievents/#topmost-event-target)
        Layout::Node* layout_node;
        if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
            return EventResult::Dropped;

        m_mousedown_target = node.ptr();
        auto offset = compute_mouse_event_offset(position, *layout_node);
        auto client_offset = compute_mouse_event_client_offset(position);
        auto page_offset = compute_mouse_event_page_offset(client_offset);
        node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::mousedown, screen_position, page_offset, client_offset, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
    }

    // NOTE: Dispatching an event may have disturbed the world.
    if (!paint_root() || paint_root() != node->document().paintable_box())
        return EventResult::Accepted;

    if (button == UIEvents::MouseButton::Primary) {
        if (auto result = paint_root()->hit_test(position, Painting::HitTestType::TextCursor); result.has_value()) {
            auto paintable = result->paintable;
            auto dom_node = paintable->dom_node();
            if (dom_node) {
                // See if we want to focus something.
                JS::GCPtr<DOM::Node> focus_candidate;
                for (auto candidate = node; candidate; candidate = candidate->parent_or_shadow_host()) {
                    if (candidate->is_focusable()) {
                        focus_candidate = candidate;
                        break;
                    }
                }

                // If we didn't focus anything, place the document text cursor at the mouse position.
                // FIXME: This is all rather strange. Find a better solution.
                if (!focus_candidate || dom_node->is_editable()) {
                    auto& realm = document->realm();
                    document->set_cursor_position(DOM::Position::create(realm, *dom_node, result->index_in_node));
                    if (auto selection = document->get_selection()) {
                        auto anchor_node = selection->anchor_node();
                        if (anchor_node && modifiers & UIEvents::KeyModifier::Mod_Shift) {
                            (void)selection->set_base_and_extent(*anchor_node, selection->anchor_offset(), *dom_node, result->index_in_node);
                        } else {
                            (void)selection->set_base_and_extent(*dom_node, result->index_in_node, *dom_node, result->index_in_node);
                        }
                    }
                    update_selection_range_for_input_or_textarea();
                    m_in_mouse_selection = true;
                }

                // When a user activates a click focusable focusable area, the user agent must run the focusing steps on the focusable area with focus trigger set to "click".
                // Spec Note: Note that focusing is not an activation behavior, i.e. calling the click() method on an element or dispatching a synthetic click event on it won't cause the element to get focused.
                if (focus_candidate)
                    HTML::run_focusing_steps(focus_candidate, nullptr, "click"sv);
                else if (auto* focused_element = document->focused_element())
                    HTML::run_unfocusing_steps(focused_element);
            }
        }
    }

    return EventResult::Handled;
}

EventResult EventHandler::handle_mousemove(CSSPixelPoint viewport_position, CSSPixelPoint screen_position, u32 buttons, u32 modifiers)
{
    if (should_ignore_device_input_event())
        return EventResult::Dropped;

    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto position = viewport_position;

    m_navigable->active_document()->update_layout();

    if (!paint_root())
        return EventResult::Dropped;

    auto& document = *m_navigable->active_document();
    auto& realm = document.realm();

    bool hovered_node_changed = false;
    bool is_hovering_link = false;
    Gfx::StandardCursor hovered_node_cursor = Gfx::StandardCursor::None;

    JS::GCPtr<Painting::Paintable> paintable;
    Optional<int> start_index;

    if (auto result = target_for_mouse_position(position); result.has_value()) {
        paintable = result->paintable;
        start_index = result->index_in_node;
    }

    const HTML::HTMLAnchorElement* hovered_link_element = nullptr;
    if (paintable) {
        if (paintable->wants_mouse_events()) {
            document.set_hovered_node(paintable->dom_node());
            if (paintable->handle_mousemove({}, position, buttons, modifiers) == Painting::Paintable::DispatchEventOfSameName::No)
                return EventResult::Cancelled;

            // FIXME: It feels a bit aggressive to always update the cursor like this.
            m_navigable->page().client().page_did_request_cursor_change(Gfx::StandardCursor::None);
        }

        auto node = dom_node_for_event_dispatch(*paintable);

        if (node && is<HTML::HTMLIFrameElement>(*node)) {
            if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
                return content_navigable->event_handler().handle_mousemove(position.translated(compute_mouse_event_offset({}, paintable->layout_node())), screen_position, buttons, modifiers);
            return EventResult::Dropped;
        }

        auto const cursor = paintable->computed_values().cursor();
        auto pointer_events = paintable->computed_values().pointer_events();
        // FIXME: Handle other values for pointer-events.
        VERIFY(pointer_events != CSS::PointerEvents::None);

        // Search for the first parent of the hit target that's an element.
        // "The click event type MUST be dispatched on the topmost event target indicated by the pointer." (https://www.w3.org/TR/uievents/#event-type-click)
        // "The topmost event target MUST be the element highest in the rendering order which is capable of being an event target." (https://www.w3.org/TR/uievents/#topmost-event-target)
        Layout::Node* layout_node;
        bool found_parent_element = parent_element_for_event_dispatch(*paintable, node, layout_node);
        hovered_node_changed = node.ptr() != document.hovered_node();
        document.set_hovered_node(node);
        if (found_parent_element) {
            hovered_link_element = node->enclosing_link_element();
            if (hovered_link_element)
                is_hovering_link = true;

            if (paintable->layout_node().is_text_node()) {
                if (cursor == CSS::Cursor::Auto)
                    hovered_node_cursor = Gfx::StandardCursor::IBeam;
                else
                    hovered_node_cursor = cursor_css_to_gfx(cursor);
            } else if (node->is_element()) {
                if (cursor == CSS::Cursor::Auto)
                    hovered_node_cursor = Gfx::StandardCursor::Arrow;
                else
                    hovered_node_cursor = cursor_css_to_gfx(cursor);
            }

            auto offset = compute_mouse_event_offset(position, *layout_node);
            auto client_offset = compute_mouse_event_client_offset(position);
            auto page_offset = compute_mouse_event_page_offset(client_offset);
            auto movement = compute_mouse_event_movement(screen_position);

            m_mousemove_previous_screen_position = screen_position;

            bool continue_ = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::mousemove, screen_position, page_offset, client_offset, offset, movement, UIEvents::MouseButton::Primary, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
            if (!continue_)
                return EventResult::Cancelled;

            // NOTE: Dispatching an event may have disturbed the world.
            if (!paint_root() || paint_root() != node->document().paintable_box())
                return EventResult::Accepted;
        }

        if (m_in_mouse_selection) {
            auto hit = paint_root()->hit_test(position, Painting::HitTestType::TextCursor);
            auto should_set_cursor_position = true;
            if (start_index.has_value() && hit.has_value() && hit->dom_node()) {
                if (auto selection = document.get_selection()) {
                    auto anchor_node = selection->anchor_node();
                    if (anchor_node) {
                        if (&anchor_node->root() == &hit->dom_node()->root())
                            (void)selection->set_base_and_extent(*anchor_node, selection->anchor_offset(), *hit->paintable->dom_node(), hit->index_in_node);
                        else
                            should_set_cursor_position = false;
                    } else {
                        (void)selection->set_base_and_extent(*hit->paintable->dom_node(), hit->index_in_node, *hit->paintable->dom_node(), hit->index_in_node);
                    }
                }
                if (should_set_cursor_position)
                    document.set_cursor_position(DOM::Position::create(realm, *hit->dom_node(), *start_index));

                document.set_needs_display();
            }
        }
    }

    auto& page = m_navigable->page();

    page.client().page_did_request_cursor_change(hovered_node_cursor);

    if (hovered_node_changed) {
        JS::GCPtr<HTML::HTMLElement const> hovered_html_element = document.hovered_node() ? document.hovered_node()->enclosing_html_element_with_attribute(HTML::AttributeNames::title) : nullptr;
        if (hovered_html_element && hovered_html_element->title().has_value()) {
            page.client().page_did_enter_tooltip_area(hovered_html_element->title()->to_byte_string());
        } else {
            page.client().page_did_leave_tooltip_area();
        }
        if (is_hovering_link)
            page.client().page_did_hover_link(document.parse_url(hovered_link_element->href()));
        else
            page.client().page_did_unhover_link();
    }

    return EventResult::Handled;
}

EventResult EventHandler::handle_doubleclick(CSSPixelPoint viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers)
{
    if (should_ignore_device_input_event())
        return EventResult::Dropped;

    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto& document = *m_navigable->active_document();

    auto scroll_offset = document.navigable()->viewport_scroll_offset();
    auto position = viewport_position.translated(scroll_offset);

    document.update_layout();

    if (!paint_root())
        return EventResult::Dropped;

    JS::GCPtr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(position); result.has_value())
        paintable = result->paintable;
    else
        return EventResult::Dropped;

    auto pointer_events = paintable->computed_values().pointer_events();
    // FIXME: Handle other values for pointer-events.
    if (pointer_events == CSS::PointerEvents::None)
        return EventResult::Cancelled;

    auto node = dom_node_for_event_dispatch(*paintable);

    if (paintable->wants_mouse_events()) {
        // FIXME: Handle double clicks.
    }

    if (!node)
        return EventResult::Dropped;

    if (is<HTML::HTMLIFrameElement>(*node)) {
        if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
            return content_navigable->event_handler().handle_doubleclick(position.translated(compute_mouse_event_offset({}, paintable->layout_node())), screen_position, button, buttons, modifiers);
        return EventResult::Dropped;
    }

    // Search for the first parent of the hit target that's an element.
    // "The topmost event target MUST be the element highest in the rendering order which is capable of being an event target." (https://www.w3.org/TR/uievents/#topmost-event-target)
    Layout::Node* layout_node;
    if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
        return EventResult::Dropped;

    auto offset = compute_mouse_event_offset(position, *layout_node);
    auto client_offset = compute_mouse_event_client_offset(position);
    auto page_offset = compute_mouse_event_page_offset(client_offset);
    node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::dblclick, screen_position, page_offset, client_offset, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());

    // NOTE: Dispatching an event may have disturbed the world.
    if (!paint_root() || paint_root() != node->document().paintable_box())
        return EventResult::Accepted;

    if (button == UIEvents::MouseButton::Primary) {
        if (auto result = paint_root()->hit_test(position, Painting::HitTestType::TextCursor); result.has_value()) {
            if (!result->paintable->dom_node())
                return EventResult::Accepted;
            if (!is<Painting::TextPaintable>(*result->paintable))
                return EventResult::Accepted;

            auto& hit_paintable = static_cast<Painting::TextPaintable const&>(*result->paintable);

            auto& hit_dom_node = const_cast<DOM::Text&>(verify_cast<DOM::Text>(*hit_paintable.dom_node()));
            auto const& text_for_rendering = hit_paintable.text_for_rendering();

            auto& segmenter = word_segmenter();
            segmenter.set_segmented_text(text_for_rendering);

            auto previous_boundary = segmenter.previous_boundary(result->index_in_node, Unicode::Segmenter::Inclusive::Yes).value_or(0);
            auto next_boundary = segmenter.next_boundary(result->index_in_node).value_or(text_for_rendering.byte_count());

            auto& realm = node->document().realm();
            document.set_cursor_position(DOM::Position::create(realm, hit_dom_node, next_boundary));
            if (auto selection = node->document().get_selection()) {
                (void)selection->set_base_and_extent(hit_dom_node, previous_boundary, hit_dom_node, next_boundary);
            }
            update_selection_range_for_input_or_textarea();
        }
    }

    return EventResult::Handled;
}

EventResult EventHandler::handle_drag_and_drop_event(DragEvent::Type type, CSSPixelPoint viewport_position, CSSPixelPoint screen_position, u32 button, u32 buttons, u32 modifiers, Vector<HTML::SelectedFile> files)
{
    if (!m_navigable->active_document())
        return EventResult::Dropped;
    if (!m_navigable->active_document()->is_fully_active())
        return EventResult::Dropped;

    auto& document = *m_navigable->active_document();
    document.update_layout();

    if (!paint_root())
        return EventResult::Dropped;

    JS::GCPtr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(viewport_position); result.has_value())
        paintable = result->paintable;
    else
        return EventResult::Dropped;

    auto node = dom_node_for_event_dispatch(*paintable);
    if (!node)
        return EventResult::Dropped;

    if (is<HTML::HTMLIFrameElement>(*node)) {
        if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
            return content_navigable->event_handler().handle_drag_and_drop_event(type, viewport_position.translated(compute_mouse_event_offset({}, paintable->layout_node())), screen_position, button, buttons, modifiers, move(files));
        return EventResult::Dropped;
    }

    auto offset = compute_mouse_event_offset(viewport_position, paintable->layout_node());
    auto client_offset = compute_mouse_event_client_offset(viewport_position);
    auto page_offset = compute_mouse_event_page_offset(client_offset);

    switch (type) {
    case DragEvent::Type::DragStart:
        return m_drag_and_drop_event_handler->handle_drag_start(document.realm(), screen_position, page_offset, client_offset, offset, button, buttons, modifiers, move(files));
    case DragEvent::Type::DragMove:
        return m_drag_and_drop_event_handler->handle_drag_move(document.realm(), document, *node, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
    case DragEvent::Type::DragEnd:
        return m_drag_and_drop_event_handler->handle_drag_leave(document.realm(), screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
    case DragEvent::Type::Drop:
        return m_drag_and_drop_event_handler->handle_drop(document.realm(), screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
    }

    VERIFY_NOT_REACHED();
}

bool EventHandler::focus_next_element()
{
    if (!m_navigable->active_document())
        return false;
    if (!m_navigable->active_document()->is_fully_active())
        return false;

    auto set_focus_to_first_focusable_element = [&]() {
        auto* element = m_navigable->active_document()->first_child_of_type<DOM::Element>();

        for (; element; element = element->next_element_in_pre_order()) {
            if (element->is_focusable()) {
                m_navigable->active_document()->set_focused_element(element);
                return true;
            }
        }

        return false;
    };

    auto* element = m_navigable->active_document()->focused_element();
    if (!element)
        return set_focus_to_first_focusable_element();

    for (element = element->next_element_in_pre_order(); element && !element->is_focusable(); element = element->next_element_in_pre_order())
        ;

    if (!element)
        return set_focus_to_first_focusable_element();

    m_navigable->active_document()->set_focused_element(element);
    return true;
}

bool EventHandler::focus_previous_element()
{
    if (!m_navigable->active_document())
        return false;
    if (!m_navigable->active_document()->is_fully_active())
        return false;

    auto set_focus_to_last_focusable_element = [&]() {
        // FIXME: This often returns the HTML element itself, which has no previous sibling.
        auto* element = m_navigable->active_document()->last_child_of_type<DOM::Element>();

        for (; element; element = element->previous_element_in_pre_order()) {
            if (element->is_focusable()) {
                m_navigable->active_document()->set_focused_element(element);
                return true;
            }
        }

        return false;
    };

    auto* element = m_navigable->active_document()->focused_element();
    if (!element)
        return set_focus_to_last_focusable_element();

    for (element = element->previous_element_in_pre_order(); element && !element->is_focusable(); element = element->previous_element_in_pre_order())
        ;

    if (!element)
        return set_focus_to_last_focusable_element();

    m_navigable->active_document()->set_focused_element(element);
    return true;
}

constexpr bool should_ignore_keydown_event(u32 code_point, u32 modifiers)
{
    if (modifiers & (UIEvents::KeyModifier::Mod_Ctrl | UIEvents::KeyModifier::Mod_Alt | UIEvents::KeyModifier::Mod_Super))
        return true;

    // FIXME: There are probably also keys with non-zero code points that should be filtered out.
    return code_point == 0 || code_point == 27;
}

EventResult EventHandler::fire_keyboard_event(FlyString const& event_name, HTML::Navigable& navigable, UIEvents::KeyCode key, u32 modifiers, u32 code_point, bool repeat)
{
    JS::GCPtr<DOM::Document> document = navigable.active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    if (JS::GCPtr<DOM::Element> focused_element = document->focused_element()) {
        if (is<HTML::NavigableContainer>(*focused_element)) {
            auto& navigable_container = verify_cast<HTML::NavigableContainer>(*focused_element);
            if (navigable_container.content_navigable())
                return fire_keyboard_event(event_name, *navigable_container.content_navigable(), key, modifiers, code_point, repeat);
        }

        auto event = UIEvents::KeyboardEvent::create_from_platform_event(document->realm(), event_name, key, modifiers, code_point, repeat);
        return focused_element->dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;
    }

    // FIXME: De-duplicate this. This is just to prevent wasting a KeyboardEvent allocation when recursing into an (i)frame.
    auto event = UIEvents::KeyboardEvent::create_from_platform_event(document->realm(), event_name, key, modifiers, code_point, repeat);

    JS::GCPtr target = document->body() ?: &document->root();
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

EventResult EventHandler::input_event(FlyString const& event_name, FlyString const& input_type, HTML::Navigable& navigable, u32 code_point)
{
    auto document = navigable.active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    UIEvents::InputEventInit input_event_init;
    if (!is_unicode_control(code_point)) {
        input_event_init.data = String::from_code_point(code_point);
    }
    input_event_init.input_type = input_type;

    if (auto* focused_element = document->focused_element()) {
        if (is<HTML::NavigableContainer>(*focused_element)) {
            auto& navigable_container = verify_cast<HTML::NavigableContainer>(*focused_element);
            if (navigable_container.content_navigable())
                return input_event(event_name, input_type, *navigable_container.content_navigable(), code_point);
        }

        auto event = UIEvents::InputEvent::create_from_platform_event(document->realm(), event_name, input_event_init);
        return focused_element->dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;
    }

    auto event = UIEvents::InputEvent::create_from_platform_event(document->realm(), event_name, input_event_init);

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
    if (produces_character_value(code_point)) {
        dispatch_result = fire_keyboard_event(UIEvents::EventNames::keypress, m_navigable, key, modifiers, code_point, repeat);
        if (dispatch_result != EventResult::Accepted)
            return dispatch_result;
    }

    JS::NonnullGCPtr<DOM::Document> document = *m_navigable->active_document();

    if (key == UIEvents::KeyCode::Key_Tab) {
        if (modifiers & UIEvents::KeyModifier::Mod_Shift)
            return focus_previous_element() ? EventResult::Handled : EventResult::Dropped;
        return focus_next_element() ? EventResult::Handled : EventResult::Dropped;
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#close-requests
    if (key == UIEvents::KeyCode::Key_Escape) {
        // 7. Let closedSomething be the result of processing close watchers on document's relevant global object.
        auto closed_something = document->window()->close_watcher_manager()->process_close_watchers();

        // 8. If closedSomething is true, then return.
        if (closed_something)
            return EventResult::Handled;

        // 9. Alternative processing: Otherwise, there was nothing watching for a close request. The user agent may
        //    instead interpret this interaction as some other action, instead of interpreting it as a close request.
    }

    auto& realm = document->realm();

    auto selection = document->get_selection();
    auto range = [&]() -> JS::GCPtr<DOM::Range> {
        if (selection) {
            if (auto range = selection->range(); range && !range->collapsed())
                return range;
        }
        return nullptr;
    }();

    if (selection && range && range->start_container()->is_editable()) {
        auto clear_selection = [&]() {
            selection->remove_all_ranges();

            // FIXME: This doesn't work for some reason?
            document->set_cursor_position(DOM::Position::create(realm, *range->start_container(), range->start_offset()));
        };

        if (key == UIEvents::KeyCode::Key_Backspace || key == UIEvents::KeyCode::Key_Delete) {
            clear_selection();
            m_edit_event_handler->handle_delete(document, *range);
            return EventResult::Handled;
        }

        // FIXME: Text editing shortcut keys (copy/paste etc.) should be handled here.
        if (!should_ignore_keydown_event(code_point, modifiers)) {
            clear_selection();
            m_edit_event_handler->handle_delete(document, *range);
            m_edit_event_handler->handle_insert(document, JS::NonnullGCPtr { *document->cursor_position() }, code_point);
            document->increment_cursor_position_offset();
            return EventResult::Handled;
        }
    }

    if (auto* element = m_navigable->active_document()->focused_element(); is<HTML::HTMLMediaElement>(element)) {
        auto& media_element = static_cast<HTML::HTMLMediaElement&>(*element);
        if (media_element.handle_keydown({}, key, modifiers).release_value_but_fixme_should_propagate_errors())
            return EventResult::Handled;
    }

    if (document->cursor_position()) {
        auto& node = *document->cursor_position()->node();

        if (key == UIEvents::KeyCode::Key_Backspace && node.is_editable()) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::deleteContentBackward, m_navigable, code_point));

            if (!document->decrement_cursor_position_offset()) {
                // FIXME: Move to the previous node and delete the last character there.
                return EventResult::Handled;
            }

            m_edit_event_handler->handle_delete_character_after(document, *document->cursor_position());
            FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::deleteContentBackward, m_navigable, code_point));
            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_Delete && node.is_editable()) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::deleteContentForward, m_navigable, code_point));

            if (document->cursor_position()->offset_is_at_end_of_node()) {
                // FIXME: Move to the next node and delete the first character there.
                return EventResult::Handled;
            }

            m_edit_event_handler->handle_delete_character_after(document, *document->cursor_position());
            FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::deleteContentForward, m_navigable, code_point));
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
            auto increment_or_decrement_cursor = [&]() {
                if ((modifiers & UIEvents::Mod_PlatformWordJump) == 0) {
                    if (key == UIEvents::KeyCode::Key_Left)
                        return document->decrement_cursor_position_offset();
                    return document->increment_cursor_position_offset();
                }

                if (key == UIEvents::KeyCode::Key_Left)
                    return document->decrement_cursor_position_to_previous_word();
                return document->increment_cursor_position_to_next_word();
            };

            if ((modifiers & UIEvents::Mod_Shift) != 0) {
                auto previous_position = document->cursor_position()->offset();
                auto should_udpdate_selection = increment_or_decrement_cursor();

                if (should_udpdate_selection && selection) {
                    auto selection_start = range ? selection->anchor_offset() : previous_position;
                    auto selection_end = document->cursor_position()->offset();

                    (void)selection->set_base_and_extent(node, selection_start, node, selection_end);
                }
            } else if (node.is_editable()) {
                if (selection && range) {
                    auto cursor_edge = key == UIEvents::KeyCode::Key_Left ? range->start_offset() : range->end_offset();

                    document->set_cursor_position(DOM::Position::create(document->realm(), node, cursor_edge));
                    selection->remove_all_ranges();
                } else {
                    increment_or_decrement_cursor();
                }
            }

            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_Home || key == UIEvents::KeyCode::Key_End) {
            auto cursor_edge = key == UIEvents::KeyCode::Key_Home ? 0uz : node.length();

            if ((modifiers & UIEvents::Mod_Shift) != 0) {
                auto previous_position = document->cursor_position()->offset();
                auto should_udpdate_selection = previous_position != cursor_edge;

                if (should_udpdate_selection && selection) {
                    auto selection_start = range ? selection->anchor_offset() : previous_position;
                    (void)selection->set_base_and_extent(node, selection_start, node, cursor_edge);
                }
            } else if (node.is_editable()) {
                if (selection && range)
                    selection->remove_all_ranges();
            }

            document->set_cursor_position(DOM::Position::create(realm, node, cursor_edge));
            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_Return && node.is_editable()) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::insertParagraph, m_navigable, code_point));
            HTML::HTMLInputElement* input_element = nullptr;
            if (auto node = document->cursor_position()->node()) {
                if (node->is_text()) {
                    auto& text_node = static_cast<DOM::Text&>(*node);
                    if (is<HTML::HTMLInputElement>(text_node.editable_text_node_owner()))
                        input_element = static_cast<HTML::HTMLInputElement*>(text_node.editable_text_node_owner());
                } else if (node->is_html_input_element()) {
                    input_element = static_cast<HTML::HTMLInputElement*>(node.ptr());
                }
            }
            if (input_element) {
                if (auto* form = input_element->form()) {
                    form->implicitly_submit_form().release_value_but_fixme_should_propagate_errors();
                    return EventResult::Handled;
                }

                input_element->commit_pending_changes();
                FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::insertParagraph, m_navigable, code_point));
                return EventResult::Handled;
            }
            FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::insertParagraph, m_navigable, code_point));
        }

        // FIXME: Text editing shortcut keys (copy/paste etc.) should be handled here.
        if (!should_ignore_keydown_event(code_point, modifiers) && node.is_editable()) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::insertText, m_navigable, code_point));
            m_edit_event_handler->handle_insert(document, JS::NonnullGCPtr { *document->cursor_position() }, code_point);
            document->increment_cursor_position_offset();
            FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::insertText, m_navigable, code_point));
            return EventResult::Handled;
        }
    }

    update_selection_range_for_input_or_textarea();

    // FIXME: Implement scroll by line and by page instead of approximating the behavior of other browsers.
    auto arrow_key_scroll_distance = 100;
    auto page_scroll_distance = document->window()->inner_height() - (document->window()->outer_height() - document->window()->inner_height());

    switch (key) {
    case UIEvents::KeyCode::Key_Up:
    case UIEvents::KeyCode::Key_Down:
        if (modifiers && modifiers != UIEvents::KeyModifier::Mod_Ctrl)
            break;
        if (modifiers)
            key == UIEvents::KeyCode::Key_Up ? document->scroll_to_the_beginning_of_the_document() : document->window()->scroll_by(0, INT64_MAX);
        else
            document->window()->scroll_by(0, key == UIEvents::KeyCode::Key_Up ? -arrow_key_scroll_distance : arrow_key_scroll_distance);
        return EventResult::Handled;
    case UIEvents::KeyCode::Key_Left:
    case UIEvents::KeyCode::Key_Right:
        if (modifiers && modifiers != UIEvents::KeyModifier::Mod_Alt)
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

void EventHandler::handle_paste(String const& text)
{
    auto active_document = m_navigable->active_document();
    if (!active_document)
        return;
    if (!active_document->is_fully_active())
        return;

    if (auto cursor_position = active_document->cursor_position()) {
        if (!cursor_position->node()->is_editable())
            return;
        active_document->update_layout();
        m_edit_event_handler->handle_insert(*active_document, *cursor_position, text);
        cursor_position->set_offset(cursor_position->offset() + text.code_points().length());
    }
}

void EventHandler::set_mouse_event_tracking_paintable(Painting::Paintable* paintable)
{
    m_mouse_event_tracking_paintable = paintable;
}

CSSPixelPoint EventHandler::compute_mouse_event_client_offset(CSSPixelPoint event_page_position) const
{
    // https://w3c.github.io/csswg-drafts/cssom-view/#dom-mouseevent-clientx
    // The clientX attribute must return the x-coordinate of the position where the event occurred relative to the origin of the viewport.

    auto scroll_offset = m_navigable->active_document()->navigable()->viewport_scroll_offset();
    return event_page_position.translated(-scroll_offset);
}

CSSPixelPoint EventHandler::compute_mouse_event_page_offset(CSSPixelPoint event_client_offset) const
{
    // https://w3c.github.io/csswg-drafts/cssom-view/#dom-mouseevent-pagex
    // FIXME: 1. If the event’s dispatch flag is set, return the horizontal coordinate of the position where the event occurred relative to the origin of the initial containing block and terminate these steps.

    // 2. Let offset be the value of the scrollX attribute of the event’s associated Window object, if there is one, or zero otherwise.
    auto scroll_offset = m_navigable->active_document()->navigable()->viewport_scroll_offset();

    // 3. Return the sum of offset and the value of the event’s clientX attribute.
    return event_client_offset.translated(scroll_offset);
}

CSSPixelPoint EventHandler::compute_mouse_event_movement(CSSPixelPoint screen_position) const
{
    // https://w3c.github.io/pointerlock/#dom-mouseevent-movementx
    // The attributes movementX movementY must provide the change in position of the pointer,
    // as if the values of screenX, screenY, were stored between two subsequent mousemove events eNow and ePrevious and the difference taken movementX = eNow.screenX-ePrevious.screenX.

    if (!m_mousemove_previous_screen_position.has_value())
        // When unlocked, the system cursor can exit and re-enter the user agent window.
        // If it does so and the user agent was not the target of operating system mouse move events
        // then the most recent pointer position will be unknown to the user agent and movementX/movementY can not be computed and must be set to zero.
        // FIXME: For this to actually work, m_mousemove_previous_client_offset needs to be cleared when the mouse leaves the window
        return { 0, 0 };

    return { screen_position.x() - m_mousemove_previous_screen_position.value().x(), screen_position.y() - m_mousemove_previous_screen_position.value().y() };
}

Optional<EventHandler::Target> EventHandler::target_for_mouse_position(CSSPixelPoint position)
{
    if (m_mouse_event_tracking_paintable) {
        if (m_mouse_event_tracking_paintable->wants_mouse_events())
            return Target { m_mouse_event_tracking_paintable, {} };

        m_mouse_event_tracking_paintable = nullptr;
    }

    if (auto result = paint_root()->hit_test(position, Painting::HitTestType::Exact); result.has_value())
        return Target { result->paintable.ptr(), result->index_in_node };

    return {};
}

bool EventHandler::should_ignore_device_input_event() const
{
    // From the moment that the user agent is to initiate the drag-and-drop operation, until the end of the drag-and-drop
    // operation, device input events (e.g. mouse and keyboard events) must be suppressed.
    return m_drag_and_drop_event_handler->has_ongoing_drag_and_drop_operation();
}

void EventHandler::visit_edges(JS::Cell::Visitor& visitor) const
{
    m_drag_and_drop_event_handler->visit_edges(visitor);
    visitor.visit(m_mouse_event_tracking_paintable);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#textFieldSelection:set-the-selection-range
void EventHandler::update_selection_range_for_input_or_textarea()
{
    // Where possible, user interface features for changing the text selection in input and
    // textarea elements must be implemented using the set the selection range algorithm so that,
    // e.g., all the same events fire.

    // NOTE: It seems like only new selections are registered with the respective elements. I.e.
    //       existing selections in other elements are not cleared, so we only need to set the
    //       selection range for the element with the current selection.

    // Get the active selection
    auto active_document = m_navigable->active_document();
    if (!active_document)
        return;
    auto selection = active_document->get_selection();
    if (!selection)
        return;

    // Do we have a range within the same node?
    auto range = selection->range();
    if (!range || range->start_container() != range->end_container())
        return;

    // We are only interested in text nodes with a shadow root
    auto& node = *range->start_container();
    if (!node.is_text())
        return;
    auto& root = node.root();
    if (!root.is_shadow_root())
        return;
    auto* shadow_host = root.parent_or_shadow_host();
    if (!shadow_host)
        return;

    // Invoke "set the selection range" on the form associated element
    auto selection_start = range->start_offset();
    auto selection_end = range->end_offset();
    // FIXME: support selection directions other than ::Forward
    auto direction = HTML::SelectionDirection::Forward;

    Optional<HTML::FormAssociatedTextControlElement&> target {};
    if (is<HTML::HTMLInputElement>(*shadow_host))
        target = static_cast<HTML::HTMLInputElement&>(*shadow_host);
    else if (is<HTML::HTMLTextAreaElement>(*shadow_host))
        target = static_cast<HTML::HTMLTextAreaElement&>(*shadow_host);

    if (target.has_value())
        target.value().set_the_selection_range(selection_start, selection_end, direction, HTML::SelectionSource::UI);
}

Unicode::Segmenter& EventHandler::word_segmenter()
{
    if (!m_word_segmenter)
        m_word_segmenter = m_navigable->active_document()->word_segmenter().clone();
    return *m_word_segmenter;
}

}
