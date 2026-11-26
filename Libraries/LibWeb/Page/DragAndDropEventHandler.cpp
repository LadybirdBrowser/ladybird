/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/DragEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/DragAndDropEventHandler.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebCommon/MimeSniff/Resource.h>
#include <LibWebCommon/UIEvents/KeyCode.h>

namespace Web {

void DragAndDropEventHandler::visit_edges(JS::Cell::Visitor& visitor) const
{
    visitor.visit(m_source_node);
    visitor.visit(m_immediate_user_selection);
    visitor.visit(m_current_target_element);
}

// https://html.spec.whatwg.org/multipage/dnd.html#drag-and-drop-processing-model
GC::Ptr<HTML::HTMLElement> DragAndDropEventHandler::first_draggable_inclusive_ancestor(DOM::Node& node)
{
    GC::Ptr<HTML::HTMLElement> draggable_element;
    node.for_each_inclusive_ancestor_of_type<HTML::HTMLElement>([&](HTML::HTMLElement& element) {
        if (!element.draggable())
            return IterationDecision::Continue;

        draggable_element = element;
        return IterationDecision::Break;
    });
    return draggable_element;
}

EventResult DragAndDropEventHandler::handle_drag_start(
    JS::Object const& relevant_global_object,
    GC::Ptr<DOM::Node> drag_target,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers,
    Vector<HTML::SelectedFile> files)
{
    auto fire_a_drag_and_drop_event = [&](GC::Ptr<DOM::EventTarget> target, Utf16FlyString const& name, GC::Ptr<DOM::EventTarget> related_target = nullptr) {
        return this->fire_a_drag_and_drop_event(relevant_global_object, target, name, screen_position, page_offset, client_offset, offset, button, buttons, modifiers, related_target);
    };

    // 1. Determine what is being dragged, as follows:
    GC::Ptr<DOM::Node> dragged_node;

    //    FIXME: If the drag operation was invoked on a selection, then it is the selection that is being dragged.

    //    Otherwise, if the drag operation was invoked on a Document, it is the first element, going up the ancestor chain,
    //    starting at the node that the user tried to drag, that has the IDL attribute draggable set to true. If there is
    //    no such element, then nothing is being dragged; return, the drag-and-drop operation is never started.
    if (drag_target) {
        dragged_node = first_draggable_inclusive_ancestor(*drag_target);

        if (!dragged_node)
            return EventResult::Cancelled;
    }

    //    Otherwise, the drag operation was invoked outside the user agent's purview. What is being dragged is defined by
    //    the document or application where the drag was started.

    // 2. Create a drag data store. All the DND events fired subsequently by the steps in this section must use this drag
    //    data store.
    m_drag_data_store = HTML::DragDataStore::create();

    // 3. Establish which DOM node is the source node, as follows:
    //
    //    FIXME: If it is a selection that is being dragged, then the source node is the Text node that the user started the
    //           drag on (typically the Text node that the user originally clicked). If the user did not specify a particular
    //           node, for example if the user just told the user agent to begin a drag of "the selection", then the source
    //           node is the first Text node containing a part of the selection.

    //    Otherwise, if it is an element that is being dragged, then the source node is the element that is being dragged.
    if (dragged_node) {
        m_source_node = dragged_node;
    }
    //    Otherwise, the source node is part of another document or application. When this specification requires that
    //    an event be dispatched at the source node in this case, the user agent must instead follow the platform-specific
    //    conventions relevant to that situation.
    else {
        m_source_node = nullptr;
    }

    // FIXME: 4. Determine the list of dragged nodes, as follows:
    //
    //    If it is a selection that is being dragged, then the list of dragged nodes contains, in tree order, every node
    //    that is partially or completely included in the selection (including all their ancestors).
    //
    //    Otherwise, the list of dragged nodes contains only the source node, if any.

    // 5. If it is a selection that is being dragged, then add an item to the drag data store item list, with its
    //    properties set as follows:
    //
    //    The drag data item type string
    //        "text/plain"
    //    The drag data item kind
    //        Text
    //    The actual data
    //        The text of the selection
    //
    //    Otherwise, if any files are being dragged, then add one item per file to the drag data store item list, with
    //    their properties set as follows:
    //
    //    The drag data item type string
    //        The MIME type of the file, if known, or "application/octet-stream" otherwise.
    //    The drag data item kind
    //        File
    //    The actual data
    //        The file's contents and name.
    for (auto& file : files) {
        auto contents = file.take_contents();
        auto mime_type = MimeSniff::Resource::sniff(contents);

        m_drag_data_store->add_file(Utf16FlyString::from_utf8(mime_type.essence()), move(contents), file.name());
    }

    // FIXME: 6. If the list of dragged nodes is not empty, then extract the microdata from those nodes into a JSON form, and
    //           add one item to the drag data store item list, with its properties set as follows:
    //
    //    The drag data item type string
    //        application/microdata+json
    //    The drag data item kind
    //        Text
    //    The actual data
    //        The resulting JSON string.

    // 7. Run the following substeps:
    [&]() {
        // 1. Let urls be « ».
        Vector<Utf16String> urls;

        // 2. For each node in the list of dragged nodes:
        if (auto* element = as_if<DOM::Element>(m_source_node.ptr())) {
            // If the node is an a element with an href attribute
            if (is<HTML::HTMLAnchorElement>(element) && element->has_attribute(HTML::AttributeNames::href)) {
                // Add to urls the result of encoding-parsing-and-serializing a URL given the element's href content
                // attribute's value, relative to the element's node document.
                if (auto url = element->document().encoding_parse_and_serialize_url(element->get_attribute_value(HTML::AttributeNames::href)); url.has_value())
                    urls.append(url.release_value());
            }
            // If the node is an img element with a src attribute
            if (is<HTML::HTMLImageElement>(element) && element->has_attribute(HTML::AttributeNames::src)) {
                // Add to urls the result of encoding-parsing-and-serializing a URL given the element's src content
                // attribute's value, relative to the element's node document.
                if (auto url = element->document().encoding_parse_and_serialize_url(element->get_attribute_value(HTML::AttributeNames::src)); url.has_value())
                    urls.append(url.release_value());
            }
        }

        // 3. If urls is still empty, then return.
        if (urls.is_empty())
            return;

        // 4. Let url string be the result of concatenating the strings in urls, in the order they were added, separated
        //    by a U+000D CARRIAGE RETURN U+000A LINE FEED character pair (CRLF).
        Utf16StringBuilder url_builder;
        for (auto const& url : urls) {
            if (!url_builder.is_empty())
                url_builder.append_ascii("\r\n"sv);
            url_builder.append(url);
        }
        auto url = url_builder.to_string();

        // 5. Add one item to the drag data store item list, with its properties set as follows:
        //
        //    The drag data item type string
        //        text/uri-list
        //    The drag data item kind
        //        Text
        //    The actual data
        //        url string
        m_drag_data_store->add_text("text/uri-list"_utf16_fly_string, url);
    }();

    // FIXME: 8. Update the drag data store default feedback as appropriate for the user agent (if the user is dragging the
    //           selection, then the selection would likely be the basis for this feedback; if the user is dragging an element,
    //           then that element's rendering would be used; if the drag began outside the user agent, then the platform
    //           conventions for determining the drag feedback should be used).

    // 9. Fire a DND event named dragstart at the source node.
    auto drag_event = fire_a_drag_and_drop_event(m_source_node, HTML::EventNames::dragstart);

    // If the event is canceled, then the drag-and-drop operation should not occur; return.
    if (drag_event->cancelled()) {
        reset();
        return EventResult::Cancelled;
    }

    // FIXME: 10. Fire a pointer event at the source node named pointercancel, and fire any other follow-up events as
    //            required by Pointer Events.

    // 11. Initiate the drag-and-drop operation in a manner consistent with platform conventions, and as described below.
    //
    //     The drag-and-drop feedback must be generated from the first of the following sources that is available:
    //
    //         1. The drag data store bitmap, if any. In this case, the drag data store hot spot coordinate should be
    //            used as hints for where to put the cursor relative to the resulting image. The values are expressed
    //            as distances in CSS pixels from the left side and from the top side of the image respectively.
    //         2. The drag data store default feedback.

    return EventResult::Handled;
}

// https://html.spec.whatwg.org/multipage/dnd.html#drag-and-drop-processing-model:queue-a-task
EventResult DragAndDropEventHandler::handle_drag_move(
    JS::Object const& relevant_global_object,
    GC::Ref<DOM::Node> node,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers)
{
    if (!has_ongoing_drag_and_drop_operation())
        return EventResult::Cancelled;

    auto fire_a_drag_and_drop_event = [&](GC::Ptr<DOM::EventTarget> target, Utf16FlyString const& name, GC::Ptr<DOM::EventTarget> related_target = nullptr) {
        return this->fire_a_drag_and_drop_event(relevant_global_object, target, name, screen_position, page_offset, client_offset, offset, button, buttons, modifiers, related_target);
    };

    // FIXME: 1. If the user agent is still performing the previous iteration of the sequence (if any) when the next iteration
    //           becomes due, return for this iteration (effectively "skipping missed frames" of the drag-and-drop operation).

    // 2. Fire a DND event named drag at the source node. If this event is canceled, the user agent must set the current
    //    drag operation to "none" (no drag operation).
    auto drag_event = fire_a_drag_and_drop_event(m_source_node, HTML::EventNames::drag);
    if (drag_event->cancelled())
        m_current_drag_operation = "none"_utf16_fly_string;

    // 3. If the drag event was not canceled and the user has not ended the drag-and-drop operation, check the state of
    //    the drag-and-drop operation, as follows:
    if (!drag_event->cancelled()) {
        GC::Ptr<DOM::Node> previous_target_element = m_current_target_element;

        // 1. If the user is indicating a different immediate user selection than during the last iteration (or if this
        //    is the first iteration), and if this immediate user selection is not the same as the current target element,
        //    then update the current target element as follows:
        if (m_immediate_user_selection != node && node != m_current_target_element) {
            m_immediate_user_selection = node;

            // -> If the new immediate user selection is null
            if (!m_immediate_user_selection) {
                // Set the current target element to null also.
                m_current_target_element = nullptr;
            }
            // FIXME: -> If the new immediate user selection is in a non-DOM document or application
            else if (false) {
                // Set the current target element to the immediate user selection.
                m_current_target_element = m_immediate_user_selection;
            }
            // -> Otherwise
            else {
                // Fire a DND event named dragenter at the immediate user selection.
                fire_a_drag_and_drop_event(m_immediate_user_selection, HTML::EventNames::dragenter);

                // If the event is canceled, then set the current target element to the immediate user selection.
                // Otherwise, run the appropriate step from the following list:
                // -> If the immediate user selection is a text control (e.g., textarea, or an input element whose
                //    type attribute is in the Text state) or an editing host or editable element, and the drag data
                //    store item list has an item with the drag data item type string "text/plain" and the drag data
                //    item kind text
                //        Set the current target element to the immediate user selection anyway.
                // -> If the immediate user selection is the body element
                //        Leave the current target element unchanged.
                // -> Otherwise
                //        Fire a DND event named dragenter at the body element, if there is one, or at the Document
                //        object, if not. Then, set the current target element to the body element, regardless of
                //        whether that event was canceled or not.

                // FIXME: Spec isue: Contrary to the spec, all browsers do not require the user script to cancel the
                //        dragenter event. See: https://github.com/whatwg/html/issues/11608
                m_current_target_element = is<DOM::Element>(*m_immediate_user_selection)
                    ? m_immediate_user_selection
                    : m_immediate_user_selection->first_ancestor_of_type<DOM::Element>();
            }
        }

        // 2. If the previous step caused the current target element to change, and if the previous target element
        //    was not null or a part of a non-DOM document, then fire a DND event named dragleave at the previous
        //    target element, with the new current target element as the specific related target.
        if (previous_target_element && previous_target_element != m_current_target_element)
            fire_a_drag_and_drop_event(previous_target_element, HTML::EventNames::dragleave, m_current_target_element);

        // 3. If the current target element is a DOM element, then fire a DND event named dragover at this current
        //    target element.
        if (m_current_target_element && is<DOM::Element>(*m_current_target_element)) {
            auto drag_event = fire_a_drag_and_drop_event(m_current_target_element, HTML::EventNames::dragover);

            // If the dragover event is not canceled, run the appropriate step from the following list:
            if (!drag_event->cancelled()) {
                // -> If the current target element is a text control (e.g., textarea, or an input element whose type
                //    attribute is in the Text state) or an editing host or editable element, and the drag data store
                //    item list has an item with the drag data item type string "text/plain" and the drag data item kind
                //    text.
                if (allow_text_drop(*m_current_target_element)) {
                    // Set the current drag operation to either "copy" or "move", as appropriate given the platform
                    // conventions.
                    m_current_drag_operation = "copy"_utf16_fly_string;
                }
                // -> Otherwise
                else {
                    // Reset the current drag operation to "none".
                    m_current_drag_operation = "none"_utf16_fly_string;
                }
            }
            // Otherwise (if the dragover event is canceled), set the current drag operation based on the values of the
            // effectAllowed and dropEffect attributes of the DragEvent object's dataTransfer object as they stood after
            // the event dispatch finished.
            else {
                m_current_drag_operation = HTML::drag_event_operation(*drag_event);
            }
        }
    }

    // Set 4 continues in handle_drag_end.
    if (drag_event->cancelled())
        return handle_drag_end(relevant_global_object, Cancelled::Yes, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);

    return EventResult::Handled;
}

EventResult DragAndDropEventHandler::handle_drag_leave(
    JS::Object const& relevant_global_object,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers)
{
    return handle_drag_end(relevant_global_object, Cancelled::Yes, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
}

EventResult DragAndDropEventHandler::handle_drag_cancel(
    JS::Object const& relevant_global_object,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers)
{
    return handle_drag_end(relevant_global_object, Cancelled::Yes, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
}

EventResult DragAndDropEventHandler::handle_drop(
    JS::Object const& relevant_global_object,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers)
{
    return handle_drag_end(relevant_global_object, Cancelled::No, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
}

// https://html.spec.whatwg.org/multipage/dnd.html#drag-and-drop-processing-model:event-dnd-drag-3
EventResult DragAndDropEventHandler::handle_drag_end(
    JS::Object const& relevant_global_object,
    Cancelled cancelled,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers)
{
    if (!has_ongoing_drag_and_drop_operation())
        return EventResult::Cancelled;

    auto fire_a_drag_and_drop_event = [&](GC::Ptr<DOM::EventTarget> target, Utf16FlyString const& name, GC::Ptr<DOM::EventTarget> related_target = nullptr) {
        return this->fire_a_drag_and_drop_event(relevant_global_object, target, name, screen_position, page_offset, client_offset, offset, button, buttons, modifiers, related_target);
    };

    ScopeGuard guard { [&]() { reset(); } };

    // 4. Otherwise, if the user ended the drag-and-drop operation (e.g. by releasing the mouse button in a mouse-driven
    //    drag-and-drop interface), or if the drag event was canceled, then this will be the last iteration. Run the
    //    following steps, then stop the drag-and-drop operation:
    {
        bool dropped = false;

        // 1. If the current drag operation is "none" (no drag operation), or if the user ended the drag-and-drop
        //    operation by canceling it (e.g. by hitting the Escape key), or if the current target element is null, then
        //    the drag operation failed. Run these substeps:
        if (m_current_drag_operation == "none"_utf16_fly_string || cancelled == Cancelled::Yes || !m_current_target_element) {
            // 1. Let dropped be false.
            dropped = false;

            // 2. If the current target element is a DOM element, fire a DND event named dragleave at it; otherwise, if
            //    it is not null, use platform-specific conventions for drag cancelation.
            if (m_current_target_element && is<DOM::Element>(*m_current_target_element)) {
                fire_a_drag_and_drop_event(m_current_target_element, HTML::EventNames::dragleave);
            } else if (m_current_target_element) {
                // FIXME: "use platform-specific conventions for drag cancelation"
            }

            // 3. Set the current drag operation to "none".
            m_current_drag_operation = "none"_utf16_fly_string;
        }
        // Otherwise, the drag operation might be a success; run these substeps:
        else {
            GC::Ptr<DOM::Event> drag_event;

            // 1. Let dropped be true.
            dropped = true;

            // 2. If the current target element is a DOM element, fire a DND event named drop at it; otherwise, use
            //    platform-specific conventions for indicating a drop.
            if (is<DOM::Element>(*m_current_target_element)) {
                drag_event = fire_a_drag_and_drop_event(m_current_target_element, HTML::EventNames::drop);
            } else {
                // FIXME: "use platform-specific conventions for indicating a drop"
            }

            // 3. If the event is canceled, set the current drag operation to the value of the dropEffect attribute of
            //    the DragEvent object's dataTransfer object as it stood after the event dispatch finished.
            if (drag_event && drag_event->cancelled()) {
                m_current_drag_operation = HTML::drag_event_drop_effect(*drag_event);
            }

            // Otherwise, the event is not canceled; perform the event's default action, which depends on the exact
            // target as follows:
            else {
                // -> If the current target element is a text control (e.g., textarea, or an input element whose type
                //    attribute is in the Text state) or an editing host or editable element, and the drag data store
                //    item list has an item with the drag data item type string "text/plain" and the drag data item
                //    kind text
                if (allow_text_drop(*m_current_target_element)) {
                    // FIXME: Insert the actual data of the first item in the drag data store item list to have a drag data item
                    //        type string of "text/plain" and a drag data item kind that is text into the text control or editing
                    //        host or editable element in a manner consistent with platform-specific conventions (e.g. inserting
                    //        it at the current mouse cursor position, or inserting it at the end of the field).
                }
                // -> Otherwise
                else {
                    // Reset the current drag operation to "none".
                    m_current_drag_operation = "none"_utf16_fly_string;
                }
            }
        }

        // 2. Fire a DND event named dragend at the source node.
        fire_a_drag_and_drop_event(m_source_node, HTML::EventNames::dragend);

        // 3. Run the appropriate steps from the following list as the default action of the dragend event:

        // -> If dropped is true, the current target element is a text control (see below), the current drag operation
        //    is "move", and the source of the drag-and-drop operation is a selection in the DOM that is entirely
        //    contained within an editing host
        if (false) {
            // FIXME: Delete the selection.
        }
        // -> If dropped is true, the current target element is a text control (see below), the current drag operation
        //    is "move", and the source of the drag-and-drop operation is a selection in a text control
        else if (false) {
            // FIXME: The user agent should delete the dragged selection from the relevant text control.
        }
        // -> If dropped is false or if the current drag operation is "none"
        else if (!dropped || m_current_drag_operation == "none"_utf16_fly_string) {
            // The drag was canceled. If the platform conventions dictate that this be represented to the user (e.g. by
            // animating the dragged selection going back to the source of the drag-and-drop operation), then do so.
            return EventResult::Cancelled;
        }
        // -> Otherwise
        else {
            // The event has no default action.
        }
    }

    return EventResult::Handled;
}

// https://html.spec.whatwg.org/multipage/dnd.html#fire-a-dnd-event
// drag_event.rs handles the steps that initialize and detach the drag data store.
GC::Ref<DOM::Event> DragAndDropEventHandler::fire_a_drag_and_drop_event(
    JS::Object const& relevant_global_object,
    GC::Ptr<DOM::EventTarget> target,
    Utf16FlyString const& name,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers,
    GC::Ptr<DOM::EventTarget> related_target)
{
    // NOTE: When the source node is determined above, the spec indicates we must follow platform-specific conventions
    //       for dispatching events at the source node if the source node is an out-of-document object. We currently
    //       handle this by allowing callers to pass a null `target` node. This allows us to execute all state-change
    //       operations in the fire-a-DND-event AO, and simply skip event dispatching for now if the target is null.

    // 2. If no specific related target was provided, set related target to null.

    // 3. Let window be the relevant global object of the Document object of the specified target element.
    // NOTE: We defer this until it's needed later, to more easily handle when the target is not an element.

    // 9. Let event be the result of creating an event using DragEvent.
    // FIXME: Implement https://dom.spec.whatwg.org/#concept-event-create
    UIEvents::MouseEventOptions event_init {};

    // 10. Initialize event's type attribute to e, its bubbles attribute to true, its view attribute to window, its
    //     relatedTarget attribute to related target, and its dataTransfer attribute to dataTransfer.
    event_init.bubbles = true;
    event_init.related_target = related_target;

    if (target) {
        auto* target_node = as_if<DOM::Node>(target.ptr());
        VERIFY(target_node);
        auto& window = HTML::relevant_window(*target_node);
        event_init.view = window.window();
    }

    //     If e is not dragleave or dragend, then initialize event's cancelable attribute to true.
    if (!name.is_one_of(HTML::EventNames::dragleave, HTML::EventNames::dragend))
        event_init.cancelable = true;

    // 11. Initialize event's mouse and key attributes according to the state of the input devices as they would be for
    //     user interaction events.
    event_init.ctrl_key = (modifiers & UIEvents::Mod_Ctrl) != 0;
    event_init.shift_key = (modifiers & UIEvents::Mod_Shift) != 0;
    event_init.alt_key = (modifiers & UIEvents::Mod_Alt) != 0;
    event_init.meta_key = (modifiers & UIEvents::Mod_Super) != 0;
    event_init.screen_x = screen_position.x().to_double();
    event_init.screen_y = screen_position.y().to_double();
    event_init.client_x = client_offset.x().to_double();
    event_init.client_y = client_offset.y().to_double();
    event_init.button = button;
    event_init.buttons = buttons;

    auto event = HTML::create_drag_event(name, event_init, page_offset.x().to_double(), page_offset.y().to_double(), offset.x().to_double(), offset.y().to_double(), HighResolutionTime::current_high_resolution_time(relevant_global_object), *m_drag_data_store, m_current_drag_operation);

    // The "create an event" AO in step 9 should set these.
    event->set_initialized(true);
    event->set_composed(true);

    // 12. Dispatch event at the specified target element.
    if (target)
        target->dispatch_event(event);

    HTML::finish_drag_event(*event);

    return event;
}

bool DragAndDropEventHandler::allow_text_drop(GC::Ref<DOM::Node> node) const
{
    if (!m_drag_data_store->has_text_item())
        return false;

    if (node->is_editable_or_editing_host())
        return true;

    if (is<HTML::HTMLTextAreaElement>(*node))
        return true;

    if (is<HTML::HTMLInputElement>(*node)) {
        auto const& input = static_cast<HTML::HTMLInputElement const&>(*node);
        return input.type_state() == HTML::HTMLInputElement::TypeAttributeState::Text;
    }

    return false;
}

void DragAndDropEventHandler::reset()
{
    // When the drag-and-drop operation has completed, we no longer need the drag data store and its related fields.
    // Clear them, as we currently use the existence of the drag data store to ignore other input events.
    m_drag_data_store.clear();
    m_source_node = nullptr;
    m_immediate_user_selection = nullptr;
    m_current_target_element = nullptr;
    m_current_drag_operation = "none"_utf16_fly_string;
}

}
