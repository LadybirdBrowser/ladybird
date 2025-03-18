/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/CloseWatcherManager.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/Layout/Label.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/DragAndDropEventHandler.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWeb/UIEvents/InputTypes.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/UIEvents/WheelEvent.h>

namespace Web {

#define FIRE(expression)                                                          \
    if (auto event_result = (expression); event_result == EventResult::Cancelled) \
        return event_result;

static GC::Ptr<DOM::Node> dom_node_for_event_dispatch(Painting::Paintable& paintable)
{
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

static DOM::Node* input_control_associated_with_ancestor_label_element(Painting::Paintable& paintable)
{
    if (is<Layout::Label>(paintable.layout_node())) {
        auto const& label = as<Layout::Label>(paintable.layout_node());
        return label.dom_node().control().ptr();
    }
    if (auto const* label = paintable.layout_node().first_ancestor_of_type<Layout::Label>())
        return label->dom_node().control().ptr();
    return nullptr;
}

static bool parent_element_for_event_dispatch(Painting::Paintable& paintable, GC::Ptr<DOM::Node>& node, Layout::Node*& layout_node)
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

static Gfx::Cursor resolve_cursor(Layout::NodeWithStyle const& layout_node, Vector<CSS::CursorData> const& cursor_data, Gfx::StandardCursor auto_cursor)
{
    for (auto const& cursor : cursor_data) {
        auto result = cursor.visit(
            [auto_cursor](CSS::Cursor css_cursor) -> Optional<Gfx::Cursor> {
                switch (css_cursor) {
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
                case CSS::Cursor::Auto:
                    return auto_cursor;
                case CSS::Cursor::ContextMenu:
                case CSS::Cursor::Alias:
                case CSS::Cursor::Copy:
                case CSS::Cursor::NoDrop:
                    // FIXME: No corresponding GFX Standard Cursor, fallthrough to None
                case CSS::Cursor::Default:
                default:
                    return Gfx::StandardCursor::None;
                }
            },
            [&layout_node](NonnullRefPtr<CSS::CursorStyleValue> const& cursor_style_value) -> Optional<Gfx::Cursor> {
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
    Gfx::Point<float> precision_offset = {
        position.x().to_double(),
        position.y().to_double()
    };

    // ignoring the transforms that apply to the element and its ancestors,
    if (paintable.layout_node().has_css_transform()) {
        auto const& paintable_box = static_cast<Painting::PaintableBox const&>(paintable);
        auto const affine_transform = Gfx::extract_2d_affine_transform(paintable_box.transform().inverse());

        auto const& origin = paintable_box.transform_origin();
        Gfx::Point<float> const precision_origin = {
            origin.x().to_double(),
            origin.y().to_double()
        };

        precision_offset.translate_by(-precision_origin);
        precision_offset.transform_by(affine_transform);
        precision_offset.translate_by(precision_origin);
    }

    // relative to the origin of the padding edge of the target node
    auto const top_left_of_layout_node = paintable.box_type_agnostic_position();
    CSSPixelPoint offset = {
        CSSPixels(precision_offset.x()),
        CSSPixels(precision_offset.y())
    };
    offset -= top_left_of_layout_node;

    // and terminate these steps.
    return offset;
}

// https://drafts.csswg.org/css-ui/#propdef-user-select
static void set_user_selection(GC::Ptr<DOM::Node> anchor_node, unsigned anchor_offset, GC::Ptr<DOM::Node> focus_node, unsigned focus_offset, Selection::Selection* selection, CSS::UserSelect user_select)
{
    // https://drafts.csswg.org/css-ui/#valdef-user-select-contain
    // NOTE: This is clamping the focus node to any node with user-select: contain that stands between it and the anchor node.
    if (focus_node != anchor_node) {
        // UAs must not allow a selection which is started in this element to be extended outside of this element.
        auto potential_contain_node = anchor_node;

        // NOTE: The way we do this is searching up the tree from the anchor, to find 'this element', i.e. its nearest contain ancestor.
        //       We stop the search early when we reach an element that contains both the anchor and the focus node, as this means they
        //       are inside the same contain element, or not in a contain element at all.
        //       This takes care of the "selection trying to escape from a contain" case.
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
            // NOTE: Prevents this from being handled again further down
            user_select = CSS::UserSelect::Contain;
        } else {
            // A selection started outside of this element must not end in this element. If the user attempts to create such a
            // selection, the UA must instead end the selection range at the element boundary.

            // NOTE: This branch takes care of the "selection trying to intrude into a contain" case.
            //       This is done by searching up the tree from the focus node, to see if there is a
            //       contain element between it and the common ancestor that also includes the anchor.
            //       We stop once reaching target_node, which is the common ancestor identified in step 1.
            //       If target_node wasn't a common ancestor, we would not be here.
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
                // NOTE: Prevents this from being handled again further down
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

        // A selection started outside of this element must not end in this element. If the user attempts to create such a
        // selection, the UA must instead end the selection range at the element boundary.
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
        // NOTE: This is handled at the start of this function
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

EventHandler::EventHandler(Badge<HTML::Navigable>, HTML::Navigable& navigable)
    : m_navigable(navigable)
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

    m_navigable->active_document()->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseWheel);

    if (!paint_root())
        return EventResult::Dropped;

    if (modifiers & UIEvents::KeyModifier::Mod_Shift)
        swap(wheel_delta_x, wheel_delta_y);

    auto handled_event = EventResult::Dropped;

    GC::Ptr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(viewport_position); result.has_value())
        paintable = result->paintable;

    if (paintable) {
        auto* containing_block = paintable->containing_block();
        while (containing_block) {
            auto handled_scroll_event = containing_block->handle_mousewheel({}, viewport_position, buttons, modifiers, wheel_delta_x, wheel_delta_y);
            if (handled_scroll_event)
                return EventResult::Handled;

            containing_block = containing_block->containing_block();
        }

        if (paintable->handle_mousewheel({}, viewport_position, buttons, modifiers, wheel_delta_x, wheel_delta_y))
            return EventResult::Handled;

        auto node = dom_node_for_event_dispatch(*paintable);

        if (node) {
            // FIXME: Support wheel events in nested browsing contexts.
            if (is<HTML::HTMLIFrameElement>(*node)) {
                auto& iframe = static_cast<HTML::HTMLIFrameElement&>(*node);
                auto position_in_iframe = viewport_position.translated(compute_mouse_event_offset({}, *paintable));
                iframe.content_navigable()->event_handler().handle_mousewheel(position_in_iframe, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y);
                return EventResult::Dropped;
            }

            // Search for the first parent of the hit target that's an element.
            Layout::Node* layout_node;
            if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
                return EventResult::Dropped;

            auto page_offset = compute_mouse_event_page_offset(viewport_position);
            auto offset = compute_mouse_event_offset(page_offset, *layout_node->first_paintable());
            if (node->dispatch_event(UIEvents::WheelEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::wheel, screen_position, page_offset, viewport_position, offset, wheel_delta_x, wheel_delta_y, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors())) {
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

    m_navigable->active_document()->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseUp);

    if (!paint_root())
        return EventResult::Dropped;

    GC::Ptr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(viewport_position); result.has_value())
        paintable = result->paintable;

    if (paintable && paintable->wants_mouse_events()) {
        if (paintable->handle_mouseup({}, viewport_position, button, modifiers) == Painting::Paintable::DispatchEventOfSameName::No)
            return EventResult::Cancelled;

        // Things may have changed as a consequence of Layout::Node::handle_mouseup(). Hit test again.
        if (!paint_root())
            return EventResult::Handled;

        if (auto result = paint_root()->hit_test(viewport_position, Painting::HitTestType::Exact); result.has_value())
            paintable = result->paintable;
    }

    auto handled_event = EventResult::Dropped;

    if (paintable) {
        auto node = dom_node_for_event_dispatch(*paintable);

        if (node) {
            if (is<HTML::HTMLIFrameElement>(*node)) {
                if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
                    return content_navigable->event_handler().handle_mouseup(viewport_position.translated(compute_mouse_event_offset({}, *paintable)), screen_position, button, buttons, modifiers);
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

            auto page_offset = compute_mouse_event_page_offset(viewport_position);
            auto offset = compute_mouse_event_offset(page_offset, *layout_node->first_paintable());
            node->dispatch_event(UIEvents::PointerEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::pointerup, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
            node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::mouseup, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
            handled_event = EventResult::Handled;

            bool run_activation_behavior = false;
            if (node.ptr() == m_mousedown_target) {
                if (button == UIEvents::MouseButton::Primary) {
                    run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::click, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
                } else if (button == UIEvents::MouseButton::Middle) {
                    run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::auxclick, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
                } else if (button == UIEvents::MouseButton::Secondary) {
                    // Allow the user to bypass custom context menus by holding shift, like Firefox.
                    if ((modifiers & UIEvents::Mod_Shift) == 0)
                        run_activation_behavior = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::contextmenu, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
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

                auto top_level_viewport_position = m_navigable->to_top_level_position(viewport_position);
                if (GC::Ptr<HTML::HTMLAnchorElement const> link = node->enclosing_link_element()) {
                    GC::Ref<DOM::Document> document = *m_navigable->active_document();
                    auto href = link->href();
                    auto url = document->encoding_parse_url(href);
                    if (url.has_value()) {
                        if (button == UIEvents::MouseButton::Primary && (modifiers & UIEvents::Mod_PlatformCtrl) != 0) {
                            m_navigable->page().client().page_did_click_link(*url, link->target().to_byte_string(), modifiers);
                        } else if (button == UIEvents::MouseButton::Middle) {
                            m_navigable->page().client().page_did_middle_click_link(*url, link->target().to_byte_string(), modifiers);
                        } else if (button == UIEvents::MouseButton::Secondary) {
                            m_navigable->page().client().page_did_request_link_context_menu(top_level_viewport_position, *url, link->target().to_byte_string(), modifiers);
                        }
                    }
                } else if (button == UIEvents::MouseButton::Secondary) {
                    if (is<HTML::HTMLImageElement>(*node)) {
                        auto& image_element = as<HTML::HTMLImageElement>(*node);
                        auto image_url = image_element.document().encoding_parse_url(image_element.src());
                        if (image_url.has_value()) {
                            Optional<Gfx::Bitmap const*> bitmap;
                            if (image_element.immutable_bitmap())
                                bitmap = image_element.immutable_bitmap()->bitmap();

                            m_navigable->page().client().page_did_request_image_context_menu(top_level_viewport_position, *image_url, "", modifiers, bitmap);
                        }
                    } else if (is<HTML::HTMLMediaElement>(*node)) {
                        auto& media_element = as<HTML::HTMLMediaElement>(*node);

                        Page::MediaContextMenu menu {
                            .media_url = *media_element.document().encoding_parse_url(media_element.current_src()),
                            .is_video = is<HTML::HTMLVideoElement>(*node),
                            .is_playing = media_element.potentially_playing(),
                            .is_muted = media_element.muted(),
                            .has_user_agent_controls = media_element.has_attribute(HTML::AttributeNames::controls),
                            .is_looping = media_element.has_attribute(HTML::AttributeNames::loop),
                        };

                        m_navigable->page().did_request_media_context_menu(media_element.unique_id(), top_level_viewport_position, "", modifiers, menu);
                    } else {
                        m_navigable->page().client().page_did_request_context_menu(top_level_viewport_position);
                    }
                }
            }

            if (auto* input_control = input_control_associated_with_ancestor_label_element(*paintable)) {
                if (button == UIEvents::MouseButton::Primary) {
                    input_control->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::click, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
                }
            }
        }
    }

after_node_use:
    if (button == UIEvents::MouseButton::Primary) {
        m_in_mouse_selection = false;
        m_mouse_selection_target = nullptr;
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

    m_navigable->active_document()->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseDown);

    if (!paint_root())
        return EventResult::Dropped;

    GC::Ref<DOM::Document> document = *m_navigable->active_document();
    GC::Ptr<DOM::Node> node;

    {
        GC::Ptr<Painting::Paintable> paintable;
        if (auto result = target_for_mouse_position(viewport_position); result.has_value())
            paintable = result->paintable;
        else
            return EventResult::Dropped;

        auto pointer_events = paintable->computed_values().pointer_events();
        // FIXME: Handle other values for pointer-events.
        VERIFY(pointer_events != CSS::PointerEvents::None);

        node = dom_node_for_event_dispatch(*paintable);
        document->set_hovered_node(node);

        if (paintable->wants_mouse_events()) {
            if (paintable->handle_mousedown({}, viewport_position, button, modifiers) == Painting::Paintable::DispatchEventOfSameName::No)
                return EventResult::Cancelled;
        }

        if (!node)
            return EventResult::Dropped;

        if (is<HTML::HTMLIFrameElement>(*node)) {
            if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
                return content_navigable->event_handler().handle_mousedown(viewport_position.translated(compute_mouse_event_offset({}, *paintable)), screen_position, button, buttons, modifiers);
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
        auto page_offset = compute_mouse_event_page_offset(viewport_position);
        auto offset = compute_mouse_event_offset(page_offset, *layout_node->first_paintable());
        node->dispatch_event(UIEvents::PointerEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::pointerdown, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
        node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::mousedown, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
    }

    // NOTE: Dispatching an event may have disturbed the world.
    if (!paint_root() || paint_root() != node->document().paintable_box())
        return EventResult::Accepted;

    if (button == UIEvents::MouseButton::Primary) {
        if (auto result = paint_root()->hit_test(viewport_position, Painting::HitTestType::TextCursor); result.has_value()) {
            auto paintable = result->paintable;
            auto dom_node = paintable->dom_node();
            if (dom_node) {
                // See if we want to focus something.
                GC::Ptr<DOM::Node> focus_candidate;
                if (auto* input_control = input_control_associated_with_ancestor_label_element(*paintable)) {
                    focus_candidate = input_control;
                } else {
                    for (auto candidate = node; candidate; candidate = candidate->parent_or_shadow_host()) {
                        if (candidate->is_focusable()) {
                            focus_candidate = candidate;
                            break;
                        }
                    }
                }

                // When a user activates a click focusable focusable area, the user agent must run the focusing steps on the focusable area with focus trigger set to "click".
                // Spec Note: Note that focusing is not an activation behavior, i.e. calling the click() method on an element or dispatching a synthetic click event on it won't cause the element to get focused.
                if (focus_candidate)
                    HTML::run_focusing_steps(focus_candidate, nullptr, "click"sv);
                else if (auto* focused_element = document->focused_element())
                    HTML::run_unfocusing_steps(focused_element);

                // https://drafts.csswg.org/css-ui/#valdef-user-select-none
                // Attempting to start a selection in an element where user-select is none, such as by clicking in it or starting
                // a drag in it, must not cause a pre-existing selection to become unselected or to be affected in any way.
                auto user_select = paintable->layout_node().user_select_used_value();
                if (user_select != CSS::UserSelect::None) {
                    auto target = document->active_input_events_target();
                    if (target) {
                        m_in_mouse_selection = true;
                        m_mouse_selection_target = target;
                        if (modifiers & UIEvents::KeyModifier::Mod_Shift) {
                            target->set_selection_focus(*dom_node, result->index_in_node);
                        } else {
                            target->set_selection_anchor(*dom_node, result->index_in_node);
                        }
                    } else if (!focus_candidate) {
                        m_in_mouse_selection = true;
                        if (auto selection = document->get_selection()) {
                            auto anchor_node = selection->anchor_node();
                            if (anchor_node && modifiers & UIEvents::KeyModifier::Mod_Shift) {
                                set_user_selection(*anchor_node, selection->anchor_offset(), *dom_node, result->index_in_node, selection, user_select);
                            } else {
                                set_user_selection(*dom_node, result->index_in_node, *dom_node, result->index_in_node, selection, user_select);
                            }
                        }
                    }
                }
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

    m_navigable->active_document()->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseMove);

    if (!paint_root())
        return EventResult::Dropped;

    auto& document = *m_navigable->active_document();

    bool hovered_node_changed = false;
    bool is_hovering_link = false;
    Gfx::Cursor hovered_node_cursor = Gfx::StandardCursor::None;

    GC::Ptr<Painting::Paintable> paintable;
    Optional<int> start_index;

    if (auto result = target_for_mouse_position(viewport_position); result.has_value()) {
        paintable = result->paintable;
        start_index = result->index_in_node;
    }

    const HTML::HTMLAnchorElement* hovered_link_element = nullptr;
    if (paintable) {
        if (paintable->wants_mouse_events()) {
            document.set_hovered_node(paintable->dom_node());
            if (paintable->handle_mousemove({}, viewport_position, buttons, modifiers) == Painting::Paintable::DispatchEventOfSameName::No)
                return EventResult::Cancelled;

            // FIXME: It feels a bit aggressive to always update the cursor like this.
            m_navigable->page().client().page_did_request_cursor_change(Gfx::StandardCursor::None);
        }

        auto node = dom_node_for_event_dispatch(*paintable);

        if (node && is<HTML::HTMLIFrameElement>(*node)) {
            if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
                return content_navigable->event_handler().handle_mousemove(viewport_position.translated(compute_mouse_event_offset({}, *paintable)), screen_position, buttons, modifiers);
            return EventResult::Dropped;
        }

        auto cursor_data = paintable->computed_values().cursor();
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
                hovered_node_cursor = resolve_cursor(*paintable->layout_node().parent(), cursor_data, Gfx::StandardCursor::IBeam);
            } else if (node->is_element()) {
                hovered_node_cursor = resolve_cursor(static_cast<Layout::NodeWithStyle&>(*layout_node), cursor_data, Gfx::StandardCursor::Arrow);
            }

            auto page_offset = compute_mouse_event_page_offset(viewport_position);
            auto offset = compute_mouse_event_offset(page_offset, *layout_node->first_paintable());
            auto movement = compute_mouse_event_movement(screen_position);

            m_mousemove_previous_screen_position = screen_position;

            bool continue_ = node->dispatch_event(UIEvents::PointerEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::pointermove, screen_position, page_offset, viewport_position, offset, movement, UIEvents::MouseButton::Primary, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
            if (!continue_)
                return EventResult::Cancelled;
            continue_ = node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::mousemove, screen_position, page_offset, viewport_position, offset, movement, UIEvents::MouseButton::Primary, buttons, modifiers).release_value_but_fixme_should_propagate_errors());
            if (!continue_)
                return EventResult::Cancelled;

            // NOTE: Dispatching an event may have disturbed the world.
            if (!paint_root() || paint_root() != node->document().paintable_box())
                return EventResult::Accepted;
        }

        if (m_in_mouse_selection) {
            auto hit = paint_root()->hit_test(viewport_position, Painting::HitTestType::TextCursor);
            if (m_mouse_selection_target) {
                if (hit.has_value() && hit->paintable->dom_node())
                    m_mouse_selection_target->set_selection_focus(*hit->paintable->dom_node(), hit->index_in_node);
            } else {
                if (start_index.has_value() && hit.has_value() && hit->dom_node()) {
                    if (auto selection = document.get_selection()) {
                        auto anchor_node = selection->anchor_node();
                        if (anchor_node) {
                            if (&anchor_node->root() == &hit->dom_node()->root())
                                set_user_selection(*anchor_node, selection->anchor_offset(), *hit->paintable->dom_node(), hit->index_in_node, selection, hit->paintable->layout_node().user_select_used_value());
                        } else {
                            set_user_selection(*hit->paintable->dom_node(), hit->index_in_node, *hit->paintable->dom_node(), hit->index_in_node, selection, hit->paintable->layout_node().user_select_used_value());
                        }
                    }

                    document.set_needs_display();
                }
            }
        }
    }

    auto& page = m_navigable->page();

    // FIXME: This check is only approximate. ImageCursors from the same CursorStyleValue share bitmaps, but may repaint them.
    //        So comparing them does not tell you if they are the same image. Also, the image may change even if the hovered
    //        node does not.
    if (page.current_cursor() != hovered_node_cursor || hovered_node_changed) {
        page.set_current_cursor(hovered_node_cursor);
        page.client().page_did_request_cursor_change(hovered_node_cursor);
    }

    if (hovered_node_changed) {
        GC::Ptr<HTML::HTMLElement const> hovered_html_element = document.hovered_node() ? document.hovered_node()->enclosing_html_element_with_attribute(HTML::AttributeNames::title) : nullptr;

        if (hovered_html_element && hovered_html_element->title().has_value()) {
            page.set_is_in_tooltip_area(true);
            page.client().page_did_enter_tooltip_area(hovered_html_element->title()->to_byte_string());
        } else if (page.is_in_tooltip_area()) {
            page.set_is_in_tooltip_area(false);
            page.client().page_did_leave_tooltip_area();
        }

        if (is_hovering_link) {
            page.set_is_hovering_link(true);
            page.client().page_did_hover_link(*document.encoding_parse_url(hovered_link_element->href()));
        } else if (page.is_hovering_link()) {
            page.set_is_hovering_link(false);
            page.client().page_did_unhover_link();
        }
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

    document.update_layout(DOM::UpdateLayoutReason::EventHandlerHandleDoubleClick);

    if (!paint_root())
        return EventResult::Dropped;

    GC::Ptr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(viewport_position); result.has_value())
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
            return content_navigable->event_handler().handle_doubleclick(viewport_position.translated(compute_mouse_event_offset({}, *paintable)), screen_position, button, buttons, modifiers);
        return EventResult::Dropped;
    }

    // Search for the first parent of the hit target that's an element.
    // "The topmost event target MUST be the element highest in the rendering order which is capable of being an event target." (https://www.w3.org/TR/uievents/#topmost-event-target)
    Layout::Node* layout_node;
    if (!parent_element_for_event_dispatch(*paintable, node, layout_node))
        return EventResult::Dropped;

    auto page_offset = compute_mouse_event_page_offset(viewport_position);
    auto offset = compute_mouse_event_offset(page_offset, *layout_node->first_paintable());
    node->dispatch_event(UIEvents::MouseEvent::create_from_platform_event(node->realm(), UIEvents::EventNames::dblclick, screen_position, page_offset, viewport_position, offset, {}, button, buttons, modifiers).release_value_but_fixme_should_propagate_errors());

    // NOTE: Dispatching an event may have disturbed the world.
    if (!paint_root() || paint_root() != node->document().paintable_box())
        return EventResult::Accepted;

    if (button == UIEvents::MouseButton::Primary) {
        if (auto result = paint_root()->hit_test(viewport_position, Painting::HitTestType::TextCursor); result.has_value()) {
            if (!result->paintable->dom_node())
                return EventResult::Accepted;
            if (!is<Painting::TextPaintable>(*result->paintable))
                return EventResult::Accepted;

            auto& hit_paintable = static_cast<Painting::TextPaintable const&>(*result->paintable);
            auto& hit_dom_node = const_cast<DOM::Text&>(as<DOM::Text>(*hit_paintable.dom_node()));
            auto previous_boundary = hit_dom_node.word_segmenter().previous_boundary(result->index_in_node, Unicode::Segmenter::Inclusive::Yes).value_or(0);
            auto next_boundary = hit_dom_node.word_segmenter().next_boundary(result->index_in_node).value_or(hit_dom_node.length());

            auto target = document.active_input_events_target();
            if (target) {
                target->set_selection_anchor(hit_dom_node, previous_boundary);
                target->set_selection_focus(hit_dom_node, next_boundary);
            } else if (auto selection = node->document().get_selection()) {
                set_user_selection(hit_dom_node, previous_boundary, hit_dom_node, next_boundary, selection, hit_paintable.layout_node().user_select_used_value());
            }
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
    document.update_layout(DOM::UpdateLayoutReason::EventHandlerHandleDragAndDrop);

    if (!paint_root())
        return EventResult::Dropped;

    GC::Ptr<Painting::Paintable> paintable;
    if (auto result = target_for_mouse_position(viewport_position); result.has_value())
        paintable = result->paintable;
    else
        return EventResult::Dropped;

    auto node = dom_node_for_event_dispatch(*paintable);
    if (!node)
        return EventResult::Dropped;

    if (is<HTML::HTMLIFrameElement>(*node)) {
        if (auto content_navigable = static_cast<HTML::HTMLIFrameElement&>(*node).content_navigable())
            return content_navigable->event_handler().handle_drag_and_drop_event(type, viewport_position.translated(compute_mouse_event_offset({}, *paintable)), screen_position, button, buttons, modifiers, move(files));
        return EventResult::Dropped;
    }

    auto page_offset = compute_mouse_event_page_offset(viewport_position);
    auto offset = compute_mouse_event_offset(page_offset, *paintable);

    switch (type) {
    case DragEvent::Type::DragStart:
        return m_drag_and_drop_event_handler->handle_drag_start(document.realm(), screen_position, page_offset, viewport_position, offset, button, buttons, modifiers, move(files));
    case DragEvent::Type::DragMove:
        return m_drag_and_drop_event_handler->handle_drag_move(document.realm(), document, *node, screen_position, page_offset, viewport_position, offset, button, buttons, modifiers);
    case DragEvent::Type::DragEnd:
        return m_drag_and_drop_event_handler->handle_drag_leave(document.realm(), screen_position, page_offset, viewport_position, offset, button, buttons, modifiers);
    case DragEvent::Type::Drop:
        return m_drag_and_drop_event_handler->handle_drop(document.realm(), screen_position, page_offset, viewport_position, offset, button, buttons, modifiers);
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
    GC::Ptr<DOM::Document> document = navigable.active_document();
    if (!document)
        return EventResult::Dropped;
    if (!document->is_fully_active())
        return EventResult::Dropped;

    if (GC::Ptr<DOM::Element> focused_element = document->focused_element()) {
        if (is<HTML::NavigableContainer>(*focused_element)) {
            auto& navigable_container = as<HTML::NavigableContainer>(*focused_element);
            if (navigable_container.content_navigable())
                return fire_keyboard_event(event_name, *navigable_container.content_navigable(), key, modifiers, code_point, repeat);
        }

        auto event = UIEvents::KeyboardEvent::create_from_platform_event(document->realm(), event_name, key, modifiers, code_point, repeat);
        return focused_element->dispatch_event(event) ? EventResult::Accepted : EventResult::Cancelled;
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
            auto& navigable_container = as<HTML::NavigableContainer>(*focused_element);
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
            if (modifiers & UIEvents::KeyModifier::Mod_Shift)
                return focus_previous_element() ? EventResult::Handled : EventResult::Dropped;
            return focus_next_element() ? EventResult::Handled : EventResult::Dropped;
        }
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

    if (auto* element = m_navigable->active_document()->focused_element(); is<HTML::HTMLMediaElement>(element)) {
        auto& media_element = static_cast<HTML::HTMLMediaElement&>(*element);
        if (media_element.handle_keydown({}, key, modifiers).release_value_but_fixme_should_propagate_errors())
            return EventResult::Handled;
    }

    auto* target = document->active_input_events_target();
    if (target) {
        if (key == UIEvents::KeyCode::Key_Backspace) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::deleteContentBackward, m_navigable, code_point));
            target->handle_delete(InputEventsTarget::DeleteDirection::Backward);
            FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::deleteContentBackward, m_navigable, code_point));
            return EventResult::Handled;
        }

        if (key == UIEvents::KeyCode::Key_Delete) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::deleteContentForward, m_navigable, code_point));
            target->handle_delete(InputEventsTarget::DeleteDirection::Forward);
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

        if (key == UIEvents::KeyCode::Key_Return) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::insertParagraph, m_navigable, code_point));
            target->handle_return_key();
            FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::insertParagraph, m_navigable, code_point));
        }

        // FIXME: Text editing shortcut keys (copy/paste etc.) should be handled here.
        if (!should_ignore_keydown_event(code_point, modifiers)) {
            FIRE(input_event(UIEvents::EventNames::beforeinput, UIEvents::InputTypes::insertText, m_navigable, code_point));
            target->handle_insert(String::from_code_point(code_point));
            FIRE(input_event(UIEvents::EventNames::input, UIEvents::InputTypes::insertText, m_navigable, code_point));
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
            } else if (key == UIEvents::KeyCode::Key_Left) {
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
        if (modifiers)
            key == UIEvents::KeyCode::Key_Up ? document->scroll_to_the_beginning_of_the_document() : document->window()->scroll_by(0, INT64_MAX);
        else
            document->window()->scroll_by(0, key == UIEvents::KeyCode::Key_Up ? -arrow_key_scroll_distance : arrow_key_scroll_distance);
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

void EventHandler::handle_paste(String const& text)
{
    auto active_document = m_navigable->active_document();
    if (!active_document)
        return;
    if (!active_document->is_fully_active())
        return;

    auto* target = active_document->active_input_events_target();
    if (!target)
        return;
    target->handle_insert(text);
}

void EventHandler::set_mouse_event_tracking_paintable(Painting::Paintable* paintable)
{
    m_mouse_event_tracking_paintable = paintable;
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

    if (m_mouse_selection_target)
        visitor.visit(m_mouse_selection_target->as_cell());
}

Unicode::Segmenter& EventHandler::word_segmenter()
{
    if (!m_word_segmenter)
        m_word_segmenter = m_navigable->active_document()->word_segmenter().clone();
    return *m_word_segmenter;
}

}
