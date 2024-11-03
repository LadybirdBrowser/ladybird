/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/HTML/DragEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/MimeSniff/Resource.h>
#include <LibWeb/Page/DragAndDropEventHandler.h>

namespace Web {

void DragAndDropEventHandler::visit_edges(JS::Cell::Visitor& visitor) const
{
    visitor.visit(m_source_node);
    visitor.visit(m_immediate_user_selection);
    visitor.visit(m_current_target_element);
}

// https://html.spec.whatwg.org/multipage/dnd.html#drag-and-drop-processing-model
EventResult DragAndDropEventHandler::handle_drag_start(
    JS::Realm& realm,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers,
    Vector<HTML::SelectedFile> files)
{
    auto fire_a_drag_and_drop_event = [&](JS::GCPtr<DOM::EventTarget> target, FlyString const& name, JS::GCPtr<DOM::EventTarget> related_target = nullptr) {
        return this->fire_a_drag_and_drop_event(realm, target, name, screen_position, page_offset, client_offset, offset, button, buttons, modifiers, related_target);
    };

    // 1. Determine what is being dragged, as follows:
    //
    //    FIXME: If the drag operation was invoked on a selection, then it is the selection that is being dragged.
    //
    //    FIXME: Otherwise, if the drag operation was invoked on a Document, it is the first element, going up the ancestor chain,
    //           starting at the node that the user tried to drag, that has the IDL attribute draggable set to true. If there is
    //           no such element, then nothing is being dragged; return, the drag-and-drop operation is never started.
    //
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
    //
    //    FIXME: Otherwise, if it is an element that is being dragged, then the source node is the element that is being dragged.
    //
    //    Otherwise, the source node is part of another document or application. When this specification requires that
    //    an event be dispatched at the source node in this case, the user agent must instead follow the platform-specific
    //    conventions relevant to that situation.
    m_source_node = nullptr;

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

        m_drag_data_store->add_item({
            .kind = HTML::DragDataStoreItem::Kind::File,
            .type_string = mime_type.essence(),
            .data = move(contents),
            .file_name = file.name(),
        });
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

    // FIXME: 7. Run the following substeps:
    [&]() {
        // 1. Let urls be « ».

        // 2. For each node in the list of dragged nodes:
        //
        //    If the node is an a element with an href attribute
        //        Add to urls the result of encoding-parsing-and-serializing a URL given the element's href content
        //        attribute's value, relative to the element's node document.
        //    If the node is an img element with a src attribute
        //        Add to urls the result of encoding-parsing-and-serializing a URL given the element's src content
        //        attribute's value, relative to the element's node document.

        // 3. If urls is still empty, then return.

        // 4. Let url string be the result of concatenating the strings in urls, in the order they were added, separated
        //    by a U+000D CARRIAGE RETURN U+000A LINE FEED character pair (CRLF).

        // 5. Add one item to the drag data store item list, with its properties set as follows:
        //
        //    The drag data item type string
        //        text/uri-list
        //    The drag data item kind
        //        Text
        //    The actual data
        //        url string
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
    JS::Realm& realm,
    JS::NonnullGCPtr<DOM::Document> document,
    JS::NonnullGCPtr<DOM::Node> node,
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

    auto fire_a_drag_and_drop_event = [&](JS::GCPtr<DOM::EventTarget> target, FlyString const& name, JS::GCPtr<DOM::EventTarget> related_target = nullptr) {
        return this->fire_a_drag_and_drop_event(realm, target, name, screen_position, page_offset, client_offset, offset, button, buttons, modifiers, related_target);
    };

    // FIXME: 1. If the user agent is still performing the previous iteration of the sequence (if any) when the next iteration
    //           becomes due, return for this iteration (effectively "skipping missed frames" of the drag-and-drop operation).

    // 2. Fire a DND event named drag at the source node. If this event is canceled, the user agent must set the current
    //    drag operation to "none" (no drag operation).
    auto drag_event = fire_a_drag_and_drop_event(m_source_node, HTML::EventNames::drag);
    if (drag_event->cancelled())
        m_current_drag_operation = HTML::DataTransferEffect::none;

    // 3. If the drag event was not canceled and the user has not ended the drag-and-drop operation, check the state of
    //    the drag-and-drop operation, as follows:
    if (!drag_event->cancelled()) {
        JS::GCPtr<DOM::Node> previous_target_element = m_current_target_element;

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
                auto drag_event = fire_a_drag_and_drop_event(m_immediate_user_selection, HTML::EventNames::dragenter);

                // If the event is canceled, then set the current target element to the immediate user selection.
                if (drag_event->cancelled()) {
                    m_current_target_element = m_immediate_user_selection;
                }
                // Otherwise, run the appropriate step from the following list:
                else {
                    // -> If the immediate user selection is a text control (e.g., textarea, or an input element whose
                    //    type attribute is in the Text state) or an editing host or editable element, and the drag data
                    //    store item list has an item with the drag data item type string "text/plain" and the drag data
                    //    item kind text
                    if (allow_text_drop(*m_immediate_user_selection)) {
                        // Set the current target element to the immediate user selection anyway.
                        m_current_target_element = m_immediate_user_selection;
                    }
                    // -> If the immediate user selection is the body element
                    else if (m_immediate_user_selection == document->body()) {
                        // Leave the current target element unchanged.
                    }
                    // -> Otherwise
                    else {
                        // Fire a DND event named dragenter at the body element, if there is one, or at the Document
                        // object, if not. Then, set the current target element to the body element, regardless of
                        // whether that event was canceled or not.
                        DOM::EventTarget* target = document->body();
                        if (!target)
                            target = document;

                        fire_a_drag_and_drop_event(target, HTML::EventNames::dragenter);
                        m_current_target_element = document->body();
                    }
                }
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
                    m_current_drag_operation = HTML::DataTransferEffect::copy;
                }
                // -> Otherwise
                else {
                    // Reset the current drag operation to "none".
                    m_current_drag_operation = HTML::DataTransferEffect::none;
                }
            }
            // Otherwise (if the dragover event is canceled), set the current drag operation based on the values of the
            // effectAllowed and dropEffect attributes of the DragEvent object's dataTransfer object as they stood after
            // the event dispatch finished, as per the following table:
            else {
                auto const& effect_allowed = drag_event->data_transfer()->effect_allowed();
                auto const& drop_effect = drag_event->data_transfer()->drop_effect();

                // effectAllowed                                             | dropEffect | Drag operation
                // ---------------------------------------------------------------------------------------
                // "uninitialized", "copy", "copyLink", "copyMove", or "all" | "copy"     | "copy"
                // "uninitialized", "link", "copyLink", "linkMove", or "all" | "link"     | "link"
                // "uninitialized", "move", "copyMove", "linkMove", or "all" | "move"     | "move"
                // Any other case                                            |            | "none"
                using namespace HTML::DataTransferEffect;

                if (effect_allowed.is_one_of(uninitialized, copy, copyLink, copyMove, all) && drop_effect == copy)
                    m_current_drag_operation = copy;
                else if (effect_allowed.is_one_of(uninitialized, link, copyLink, linkMove, all) && drop_effect == link)
                    m_current_drag_operation = link;
                else if (effect_allowed.is_one_of(uninitialized, move, copyMove, linkMove, all) && drop_effect == move)
                    m_current_drag_operation = move;
                else
                    m_current_drag_operation = none;
            }
        }
    }

    // Set 4 continues in handle_drag_end.
    if (drag_event->cancelled())
        return handle_drag_end(realm, Cancelled::Yes, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);

    return EventResult::Handled;
}

EventResult DragAndDropEventHandler::handle_drag_leave(
    JS::Realm& realm,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers)
{
    return handle_drag_end(realm, Cancelled::Yes, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
}

EventResult DragAndDropEventHandler::handle_drop(
    JS::Realm& realm,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers)
{
    return handle_drag_end(realm, Cancelled::No, screen_position, page_offset, client_offset, offset, button, buttons, modifiers);
}

// https://html.spec.whatwg.org/multipage/dnd.html#drag-and-drop-processing-model:event-dnd-drag-3
EventResult DragAndDropEventHandler::handle_drag_end(
    JS::Realm& realm,
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

    auto fire_a_drag_and_drop_event = [&](JS::GCPtr<DOM::EventTarget> target, FlyString const& name, JS::GCPtr<DOM::EventTarget> related_target = nullptr) {
        return this->fire_a_drag_and_drop_event(realm, target, name, screen_position, page_offset, client_offset, offset, button, buttons, modifiers, related_target);
    };

    ScopeGuard guard { [&]() { reset(); } };

    // 4. Otherwise, if the user ended the drag-and-drop operation (e.g. by releasing the mouse button in a mouse-driven
    //    drag-and-drop interface), or if the drag event was canceled, then this will be the last iteration. Run the
    //    following steps, then stop the drag-and-drop operation:
    {
        bool dropped = false;

        // 1. If the current drag operation is "none" (no drag operation), or, if the user ended the drag-and-drop
        //    operation by canceling it (e.g. by hitting the Escape key), or if the current target element is null, then
        //    the drag operation failed. Run these substeps:
        if (m_current_drag_operation == HTML::DataTransferEffect::none || cancelled == Cancelled::Yes || !m_current_target_element) {
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
            m_current_drag_operation = HTML::DataTransferEffect::none;
        }
        // Otherwise, the drag operation might be a success; run these substeps:
        else {
            JS::GCPtr<HTML::DragEvent> drag_event;

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
                m_current_drag_operation = drag_event->data_transfer()->drop_effect();
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
                    m_current_drag_operation = HTML::DataTransferEffect::none;
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
        else if (!dropped || m_current_drag_operation == HTML::DataTransferEffect::none) {
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
JS::NonnullGCPtr<HTML::DragEvent> DragAndDropEventHandler::fire_a_drag_and_drop_event(
    JS::Realm& realm,
    JS::GCPtr<DOM::EventTarget> target,
    FlyString const& name,
    CSSPixelPoint screen_position,
    CSSPixelPoint page_offset,
    CSSPixelPoint client_offset,
    CSSPixelPoint offset,
    unsigned button,
    unsigned buttons,
    unsigned modifiers,
    JS::GCPtr<DOM::EventTarget> related_target)
{
    // NOTE: When the source node is determined above, the spec indicates we must follow platform-specific conventions
    //       for dispatching events at the source node if the source node is an out-of-document object. We currently
    //       handle this by allowing callers to pass a null `target` node. This allows us to execute all state-change
    //       operations in the fire-a-DND-event AO, and simply skip event dispatching for now if the target is null.

    // 1. Let dataDragStoreWasChanged be false.
    bool drag_data_store_was_changed = false;

    // 2. If no specific related target was provided, set related target to null.

    // 3. Let window be the relevant global object of the Document object of the specified target element.
    // NOTE: We defer this until it's needed later, to more easily handle when the target is not an element.

    // 4. If e is dragstart, then set the drag data store mode to the read/write mode and set dataDragStoreWasChanged to true.
    if (name == HTML::EventNames::dragstart) {
        m_drag_data_store->set_mode(HTML::DragDataStore::Mode::ReadWrite);
        drag_data_store_was_changed = true;
    }

    // 5. If e is drop, set the drag data store mode to the read-only mode.
    else if (name == HTML::EventNames::drop) {
        m_drag_data_store->set_mode(HTML::DragDataStore::Mode::ReadOnly);
    }

    // 6. Let dataTransfer be a newly created DataTransfer object associated with the given drag data store.
    auto data_transfer = HTML::DataTransfer::create(realm, *m_drag_data_store);

    // 7. Set the effectAllowed attribute to the drag data store's drag data store allowed effects state.
    data_transfer->set_effect_allowed_internal(m_drag_data_store->allowed_effects_state());

    // 8. Set the dropEffect attribute to "none" if e is dragstart, drag, or dragleave; to the value corresponding to the
    //    current drag operation if e is drop or dragend; and to a value based on the effectAllowed attribute's value and
    //    the drag-and-drop source, as given by the following table, otherwise (i.e. if e is dragenter or dragover):
    if (name.is_one_of(HTML::EventNames::dragstart, HTML::EventNames::drag, HTML::EventNames::dragleave)) {
        data_transfer->set_drop_effect(HTML::DataTransferEffect::none);
    } else if (name.is_one_of(HTML::EventNames::drop, HTML::EventNames::dragend)) {
        data_transfer->set_drop_effect(m_current_drag_operation);
    } else {
        // effectAllowed                                                                     | dropEffect
        // ---------------------------------------------------------------------------------------------------------------------------------------
        // "none"                                                                            | "none"
        // "copy"                                                                            | "copy"
        // "copyLink"                                                                        | "copy", or, if appropriate, "link"
        // "copyMove"                                                                        | "copy", or, if appropriate, "move"
        // "all"                                                                             | "copy", or, if appropriate, either "link" or "move"
        // "link"                                                                            | "link"
        // "linkMove"                                                                        | "link", or, if appropriate, "move"
        // "move"                                                                            | "move"
        // "uninitialized" when what is being dragged is a selection from a text control     | "move", or, if appropriate, either "copy" or "link"
        // "uninitialized" when what is being dragged is a selection                         | "copy", or, if appropriate, either "link" or "move"
        // "uninitialized" when what is being dragged is an a element with an href attribute | "link", or, if appropriate, either "copy" or "move"
        // Any other case                                                                    | "copy", or, if appropriate, either "link" or "move"
        using namespace HTML::DataTransferEffect;

        // clang-format off
        if (data_transfer->effect_allowed() == none)          data_transfer->set_drop_effect(none);
        else if (data_transfer->effect_allowed() == copy)     data_transfer->set_drop_effect(copy);
        else if (data_transfer->effect_allowed() == copyLink) data_transfer->set_drop_effect(copy);
        else if (data_transfer->effect_allowed() == copyMove) data_transfer->set_drop_effect(copy);
        else if (data_transfer->effect_allowed() == all)      data_transfer->set_drop_effect(copy);
        else if (data_transfer->effect_allowed() == link)     data_transfer->set_drop_effect(link);
        else if (data_transfer->effect_allowed() == linkMove) data_transfer->set_drop_effect(link);
        else if (data_transfer->effect_allowed() == move)     data_transfer->set_drop_effect(move);
        // FIXME: Handle "uninitialized" when element drag operations are supported.
        else data_transfer->set_drop_effect(copy);
        // clang-format on
    }

    // 9. Let event be the result of creating an event using DragEvent.
    // FIXME: Implement https://dom.spec.whatwg.org/#concept-event-create
    HTML::DragEventInit event_init {};

    // 10. Initialize event's type attribute to e, its bubbles attribute to true, its view attribute to window, its
    //     relatedTarget attribute to related target, and its dataTransfer attribute to dataTransfer.
    event_init.bubbles = true;
    event_init.related_target = related_target;
    event_init.data_transfer = data_transfer;

    if (target) {
        auto& window = static_cast<HTML::Window&>(HTML::relevant_global_object(*target));
        event_init.view = window;
    }

    //     If e is not dragleave or dragend, then initialize event's cancelable attribute to true.
    if (!name.is_one_of(HTML::EventNames::dragleave, HTML::EventNames::dragend))
        event_init.cancelable = true;

    // 11. Initialize event's mouse and key attributes initialized according to the state of the input devices as they
    //     would be for user interaction events.
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

    auto event = HTML::DragEvent::create(realm, name, event_init, page_offset.x().to_double(), page_offset.y().to_double(), offset.x().to_double(), offset.y().to_double());

    // The "create an event" AO in step 9 should set these.
    event->set_is_trusted(true);
    event->set_initialized(true);
    event->set_composed(true);

    // 12. Dispatch event at the specified target element.
    if (target)
        target->dispatch_event(event);

    // 13. Set the drag data store allowed effects state to the current value of dataTransfer's effectAllowed attribute.
    //     (It can only have changed value if e is dragstart.)
    m_drag_data_store->set_allowed_effects_state(data_transfer->effect_allowed());

    // 14. If dataDragStoreWasChanged is true, then set the drag data store mode back to the protected mode.
    if (drag_data_store_was_changed)
        m_drag_data_store->set_mode(HTML::DragDataStore::Mode::Protected);

    // 15. Break the association between dataTransfer and the drag data store.
    data_transfer->disassociate_with_drag_data_store();

    return event;
}

bool DragAndDropEventHandler::allow_text_drop(JS::NonnullGCPtr<DOM::Node> node) const
{
    if (!m_drag_data_store->has_text_item())
        return false;

    if (node->is_editable())
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
    m_current_drag_operation = HTML::DataTransferEffect::none;
}

}
