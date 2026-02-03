/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/Bindings/HTMLDialogElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/CloseWatcher.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/ToggleEvent.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLDialogElement);

HTMLDialogElement::HTMLDialogElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLDialogElement::~HTMLDialogElement() = default;

void HTMLDialogElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLDialogElement);
    Base::initialize(realm);
}

void HTMLDialogElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_close_watcher);
    visitor.visit(m_request_close_source_element);
    visitor.visit(m_previously_focused_element);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#the-dialog-element:html-element-removing-steps
void HTMLDialogElement::removed_from(Node* old_parent, Node& old_root)
{
    HTMLElement::removed_from(old_parent, old_root);

    // 1. If removedNode has an open attribute, then run the dialog cleanup steps given removedNode.
    if (has_attribute(AttributeNames::open))
        run_dialog_cleanup_steps();

    // 2. If removedNode's node document's top layer contains removedNode, then remove an element from the top layer
    //    immediately given removedNode.
    if (document().top_layer_elements().contains(*this))
        document().remove_an_element_from_the_top_layer_immediately(*this);

    // 3. Set is modal of removedNode to false.
    set_is_modal(false);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#queue-a-dialog-toggle-event-task
void HTMLDialogElement::queue_a_dialog_toggle_event_task(AK::String old_state, AK::String new_state, GC::Ptr<DOM::Element> source)
{
    // 1. If element's dialog toggle task tracker is not null, then:
    if (m_dialog_toggle_task_tracker.has_value()) {
        // 1. Set oldState to element's dialog toggle task tracker's old state.
        old_state = m_dialog_toggle_task_tracker->old_state;

        // 2. Remove element's dialog toggle task tracker's task from its task queue.
        HTML::main_thread_event_loop().task_queue().remove_tasks_matching([&](auto const& task) {
            return task.id() == m_dialog_toggle_task_tracker->task_id;
        });

        // 3. Set element's dialog toggle task tracker to null.
        m_dialog_toggle_task_tracker = {};
    }

    // 2. Queue an element task given the DOM manipulation task source and element to run the following steps:
    auto task_id = queue_an_element_task(Task::Source::DOMManipulation, [this, old_state, new_state = move(new_state), source]() {
        // 1. Fire an event named toggle at element, using ToggleEvent, with the oldState attribute initialized to
        //    oldState, the newState attribute initialized to newState, and the source attribute initialized to source.
        ToggleEventInit event_init {};
        event_init.old_state = move(old_state);
        event_init.new_state = move(new_state);
        event_init.source = source;

        dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::toggle, move(event_init)));

        // 2. Set element's dialog toggle task tracker to null.
        m_dialog_toggle_task_tracker = {};
    });

    // 3. Set element's dialog toggle task tracker to a struct with task set to the just-queued task and old state set to oldState.
    m_dialog_toggle_task_tracker = ToggleTaskTracker {
        .task_id = task_id,
        .old_state = move(old_state),
    };
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-show
WebIDL::ExceptionOr<void> HTMLDialogElement::show()
{
    // 1. If this has an open attribute and the is modal flag of this is false, then return.
    if (has_attribute(AttributeNames::open) && !m_is_modal)
        return {};

    // 2. If this has an open attribute, then throw an "InvalidStateError" DOMException.
    if (has_attribute(AttributeNames::open))
        return WebIDL::InvalidStateError::create(realm(), "Dialog already open"_utf16);

    // 3. If the result of firing an event named beforetoggle, using ToggleEvent,
    //    with the cancelable attribute initialized to true, the oldState attribute initialized to "closed",
    //    and the newState attribute initialized to "open" at this is false, then return.
    ToggleEventInit event_init {};
    event_init.cancelable = true;
    event_init.old_state = "closed"_string;
    event_init.new_state = "open"_string;

    auto beforetoggle_result = dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::beforetoggle, move(event_init)));
    if (!beforetoggle_result)
        return {};

    // 4. If this has an open attribute, then return.
    if (has_attribute(AttributeNames::open))
        return {};

    // 5. Queue a dialog toggle event task given this, "closed", "open", and null.
    queue_a_dialog_toggle_event_task("closed"_string, "open"_string, nullptr);

    // 6. Add an open attribute to this, whose value is the empty string.
    set_attribute_value(AttributeNames::open, String {});

    // 7. Set this's previously focused element to the focused element.
    m_previously_focused_element = document().focused_area();

    // 8. Let document be this's node document.
    auto document = m_document;

    // 9. Let hideUntil be the result of running topmost popover ancestor given this, document's showing hint popover
    //    list, null, and false.
    Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> hide_until = topmost_popover_ancestor(this, document->showing_hint_popover_list(), nullptr, IsPopover::No);

    // 10. If hideUntil is null, then set hideUntil to the result of running topmost popover ancestor given this,
    //     document's showing auto popover list, null, and false.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = topmost_popover_ancestor(this, document->showing_auto_popover_list(), nullptr, IsPopover::No);

    // 11. If hideUntil is null, then set hideUntil to document.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = document;

    // 12. Run hide all popovers until given hideUntil, false, and true.
    hide_all_popovers_until(hide_until, FocusPreviousElement::No, FireEvents::Yes);

    // 13. Run the dialog focusing steps given this.
    run_dialog_focusing_steps();

    return {};
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-showmodal
WebIDL::ExceptionOr<void> HTMLDialogElement::show_modal()
{
    // The showModal() method steps are to show a modal dialog given this and null.
    return show_a_modal_dialog(*this, nullptr);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#show-a-modal-dialog
WebIDL::ExceptionOr<void> HTMLDialogElement::show_a_modal_dialog(HTMLDialogElement& subject, GC::Ptr<DOM::Element> source)
{
    auto& realm = subject.realm();

    // 1. If subject has an open attribute and is modal of subject is true, then return.
    if (subject.has_attribute(AttributeNames::open) && subject.m_is_modal)
        return {};

    // 2. If subject has an open attribute, then throw an "InvalidStateError" DOMException.
    if (subject.has_attribute(AttributeNames::open))
        return WebIDL::InvalidStateError::create(realm, "Dialog already open"_utf16);

    // 3. If subject's node document is not fully active, then throw an "InvalidStateError" DOMException.
    if (!subject.document().is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16);

    // 4. If subject is not connected, then throw an "InvalidStateError" DOMException.
    if (!subject.is_connected())
        return WebIDL::InvalidStateError::create(realm, "Dialog not connected"_utf16);

    // 5. If subject is in the popover showing state, then throw an "InvalidStateError" DOMException.
    if (subject.popover_visibility_state() == PopoverVisibilityState::Showing)
        return WebIDL::InvalidStateError::create(realm, "Dialog already open as popover"_utf16);

    // 6. If the result of firing an event named beforetoggle, using ToggleEvent,
    //    with the cancelable attribute initialized to true, the oldState attribute initialized to "closed",
    //    the newState attribute initialized to "open", and the source attribute initialized to source at subject is
    //    false, then return.
    ToggleEventInit event_init {};
    event_init.cancelable = true;
    event_init.old_state = "closed"_string;
    event_init.new_state = "open"_string;
    event_init.source = source;

    auto beforetoggle_result = subject.dispatch_event(ToggleEvent::create(realm, EventNames::beforetoggle, move(event_init)));
    if (!beforetoggle_result)
        return {};

    // 7. If subject has an open attribute, then return.
    if (subject.has_attribute(AttributeNames::open))
        return {};

    // 8. If subject is not connected, then return.
    if (!subject.is_connected())
        return {};

    // 9. If subject is in the popover showing state, then return.
    if (subject.popover_visibility_state() == PopoverVisibilityState::Showing)
        return {};

    // 10. Queue a dialog toggle event task given subject, "closed", "open", and source.
    subject.queue_a_dialog_toggle_event_task("closed"_string, "open"_string, source);

    // 11. Add an open attribute to subject, whose value is the empty string.
    subject.set_attribute_value(AttributeNames::open, String {});

    // 12. Assert: subject's close watcher is not null.
    VERIFY(subject.m_close_watcher);

    // 13. Set is modal of subject to true.
    subject.set_is_modal(true);

    // FIXME: 14. Set subject's node document to be blocked by the modal dialog subject.

    // 15. If subject's node document's top layer does not already contain subject, then add an element to the top
    //     layer given subject.
    if (!subject.document().top_layer_elements().contains(subject))
        subject.document().add_an_element_to_the_top_layer(subject);

    // FIXME: 16. Set subject's previously focused element to the focused element.

    // 17. Let document be subject's node document.
    auto& document = subject.document();

    // 18. Let hideUntil be the result of running topmost popover ancestor given subject, document's showing hint
    //     popover list, null, and false.
    Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> hide_until = topmost_popover_ancestor(subject, document.showing_hint_popover_list(), nullptr, IsPopover::No);

    // 19. If hideUntil is null, then set hideUntil to the result of running topmost popover ancestor given subject,
    //     document's showing auto popover list, null, and false.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = topmost_popover_ancestor(subject, document.showing_auto_popover_list(), nullptr, IsPopover::No);

    // 20. If hideUntil is null, then set hideUntil to document.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = GC::Ptr(document);

    // 21. Run hide all popovers until given hideUntil, false, and true.
    hide_all_popovers_until(hide_until, FocusPreviousElement::No, FireEvents::Yes);

    // 22. Run the dialog focusing steps given subject.
    subject.run_dialog_focusing_steps();

    return {};
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-close
void HTMLDialogElement::close(Optional<String> return_value)
{
    // 1. If returnValue is not given, then set it to null.
    // 2. Close the dialog this with returnValue and null.
    close_the_dialog(move(return_value), nullptr);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-requestclose
void HTMLDialogElement::request_close(Optional<String> return_value)
{
    // 1. If returnValue is not given, then set it to null.
    // 2. Request to close the dialog this with returnValue and null.
    request_close_the_dialog(move(return_value), nullptr);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dialog-request-close
void HTMLDialogElement::request_close_the_dialog(Optional<String> return_value, GC::Ptr<DOM::Element> source)
{
    // 1. If this does not have an open attribute, then return.
    if (!has_attribute(AttributeNames::open))
        return;

    // 2. If subject is not connected or subject's node document is not fully active, then return.
    if (!is_connected() || !document().is_fully_active())
        return;

    // 3. Assert: subject's close watcher is not null.
    VERIFY(m_close_watcher);

    // 4. Set subject's enable close watcher for request close to true.
    m_enable_close_watcher_for_request_close = true;

    // 5. Set subject's request close return value to returnValue.
    m_request_close_return_value = return_value;

    // 6. Set subject's request close source element to source.
    m_request_close_source_element = source;

    // 7. Request to close dialog's close watcher with false.
    m_close_watcher->request_close(false);

    // 8. Set subject's enable close watcher for request close to false.
    m_enable_close_watcher_for_request_close = false;
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-returnvalue
String HTMLDialogElement::return_value() const
{
    return m_return_value;
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-returnvalue
void HTMLDialogElement::set_return_value(String return_value)
{
    m_return_value = move(return_value);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#close-the-dialog
void HTMLDialogElement::close_the_dialog(Optional<String> result, GC::Ptr<DOM::Element> source)
{
    // 1. If subject does not have an open attribute, then return.
    if (!has_attribute(AttributeNames::open))
        return;

    // 2. Fire an event named beforetoggle, using ToggleEvent, with the oldState attribute initialized to "open", the
    //    newState attribute initialized to "closed", and the source attribute initialized to source at subject.
    ToggleEventInit event_init {};
    event_init.old_state = "open"_string;
    event_init.new_state = "closed"_string;
    event_init.source = source;

    dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::beforetoggle, move(event_init)));

    // 3. If subject does not have an open attribute, then return.
    if (!has_attribute(AttributeNames::open))
        return;

    // 4. Queue a dialog toggle event task given subject, "open", "closed", and source.
    queue_a_dialog_toggle_event_task("open"_string, "closed"_string, source);

    // 5. Remove subject's open attribute.
    remove_attribute(AttributeNames::open);

    // 6. If is modal of subject is true, then request an element to be removed from the top layer given subject.
    if (m_is_modal)
        document().request_an_element_to_be_remove_from_the_top_layer(*this);

    // 7. Let wasModal be the value of subject's is modal flag.
    auto was_modal = m_is_modal;

    // 8. Set is modal of subject to false.
    set_is_modal(false);

    // 9. If result is not null, then set subject's returnValue attribute to result.
    if (result.has_value())
        set_return_value(result.release_value());

    // 10. Set subject's request close return value to null.
    m_request_close_return_value = {};

    // 11. Set subject's request close source element to null.
    m_request_close_source_element = nullptr;

    // 12. If subject's previously focused element is not null, then:
    if (m_previously_focused_element) {
        // 1. Let element be subject's previously focused element.
        auto element = m_previously_focused_element;

        // 2. Set subject's previously focused element to null.
        m_previously_focused_element = nullptr;

        // 3. If subject's node document's focused area of the document's DOM anchor is a shadow-including inclusive
        //   descendant of subject, or wasModal is true, then run the focusing steps for element; the viewport should
        //   not be scrolled by doing this step.
        auto focused_element = document().focused_area();
        auto is_focus_within_dialog = focused_element && focused_element->is_shadow_including_inclusive_descendant_of(*this);
        if (is_focus_within_dialog || was_modal)
            run_focusing_steps(element);
    }

    // 13. Queue an element task on the user interaction task source given the subject element to fire an event named
    //     close at subject.
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
        auto close_event = DOM::Event::create(realm(), HTML::EventNames::close);
        dispatch_event(close_event);
    });
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#set-the-dialog-close-watcher
void HTMLDialogElement::set_close_watcher()
{
    // 1. Assert: dialog's close watcher is null.
    VERIFY(m_close_watcher == nullptr);

    // 2. Assert: dialog has an open attribute and dialog's node document is fully active.
    VERIFY(has_attribute(AttributeNames::open) && document().is_fully_active());

    // 3. Set dialog's close watcher to the result of establishing a close watcher given dialog's relevant global
    //    object, with:
    //    - cancelAction given canPreventClose being to return the result of firing an event named cancel at dialog,
    //      with the cancelable attribute initialized to canPreventClose.
    auto cancel_callback_function = JS::NativeFunction::create(
        realm(), [this](JS::VM& vm) {
            auto& event = as<DOM::Event>(vm.argument(0).as_object());
            bool can_prevent_close = event.cancelable();
            auto should_continue = dispatch_event(DOM::Event::create(realm(), HTML::EventNames::cancel, { .cancelable = can_prevent_close }));
            if (!should_continue)
                event.prevent_default();
            return JS::js_undefined();
        },
        0, Utf16FlyString {}, &realm());
    auto cancel_callback = realm().heap().allocate<WebIDL::CallbackType>(*cancel_callback_function, realm());
    //    - closeAction being to close the dialog given dialog, dialog's request close return value, and dialog's
    //      request close source element.
    auto close_callback_function = JS::NativeFunction::create(
        realm(), [this](JS::VM&) {
            close_the_dialog(m_request_close_return_value, m_request_close_source_element);

            return JS::js_undefined();
        },
        0, Utf16FlyString {}, &realm());
    auto close_callback = realm().heap().allocate<WebIDL::CallbackType>(*close_callback_function, realm());
    //    - getEnabledState being to return true if dialog's enable close watcher for request close is true or dialog's
    //      computed closed-by state is not None; otherwise false.
    auto get_enabled_state = GC::create_function(heap(), [dialog = this] {
        if (dialog->m_enable_close_watcher_for_request_close)
            return true;
        // FIXME: dialog's computed closed-by state is not None
        return false;
    });

    m_close_watcher = CloseWatcher::establish(*document().window(), move(get_enabled_state));
    m_close_watcher->add_event_listener_without_options(HTML::EventNames::cancel, DOM::IDLEventListener::create(realm(), cancel_callback));
    m_close_watcher->add_event_listener_without_options(HTML::EventNames::close, DOM::IDLEventListener::create(realm(), close_callback));
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dialog-setup-steps
void HTMLDialogElement::run_dialog_setup_steps()
{
    // 1. Assert: subject has an open attribute.
    VERIFY(has_attribute(AttributeNames::open));

    // 2. Assert: subject is connected.
    VERIFY(is_connected());

    // 3. Assert: subject's node document's open dialogs list does not contain subject.
    VERIFY(!m_document->open_dialogs_list().contains_slow(GC::Ref(*this)));

    // 4. Add subject to subject's node document's open dialogs list.
    m_document->open_dialogs_list().append(*this);

    // 5. Set the dialog close watcher with subject.
    set_close_watcher();
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dialog-cleanup-steps
void HTMLDialogElement::run_dialog_cleanup_steps()
{
    // 1. Remove subject from subject's node document's open dialogs list.
    document().open_dialogs_list().remove_first_matching([this](auto other) { return other == this; });

    // 2. If subject's close watcher is not null, then:
    if (m_close_watcher) {
        // 1. Destroy subject's close watcher.
        m_close_watcher->destroy();

        // 2. Set subject's close watcher to null.
        m_close_watcher = nullptr;
    }
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dialog-focusing-steps
void HTMLDialogElement::run_dialog_focusing_steps()
{
    // 1. If the allow focus steps given subject's node document return false, then return.
    if (!document().allow_focus())
        return;

    // 2. Let control be null
    GC::Ptr<Element> control = nullptr;

    // FIXME: 3. If subject has the autofocus attribute, then set control to subject.
    // FIXME: 4. If control is null, then set control to the focus delegate of subject.

    // 5. If control is null, then set control to subject.
    if (!control)
        control = this;

    // 6. Run the focusing steps for control.
    run_focusing_steps(control);

    // 7. Let topDocument be control's node navigable's top-level traversable's active document.
    auto top_document = control->navigable()->top_level_traversable()->active_document();

    // 8. If control's node document's origin is not the same as the origin of topDocument, then return.
    if (!control->document().origin().is_same_origin(top_document->origin()))
        return;

    // FIXME: 9. Empty topDocument's autofocus candidates.
    // FIXME: 10. Set topDocument's autofocus processed flag to true.
}

void HTMLDialogElement::set_is_modal(bool is_modal)
{
    if (m_is_modal == is_modal)
        return;
    m_is_modal = is_modal;
    invalidate_style(DOM::StyleInvalidationReason::HTMLDialogElementSetIsModal);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#the-dialog-element:is-valid-command-steps
bool HTMLDialogElement::is_valid_command(String& command)
{
    // 1. If command is in the Close state, the Request Close state, or the Show Modal state, then return true.
    if (command == "close" || command == "request-close" || command == "show-modal")
        return true;

    // 2. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#the-dialog-element:command-steps
void HTMLDialogElement::command_steps(DOM::Element& source, String& command)
{
    // 1. If element is in the popover showing state, then return.
    if (popover_visibility_state() == PopoverVisibilityState::Showing) {
        return;
    }

    // 2. If command is in the Close state and element has an open attribute,
    //    then close the dialog given element with source's optional value and source.
    if (command == "close" && has_attribute(AttributeNames::open)) {
        auto const optional_value = as<FormAssociatedElement>(source).optional_value();
        close_the_dialog(optional_value, source);
    }

    // 3. If command is in the Request Close state and element has an open attribute,
    //    then request to close the dialog element with source's optional value and source.
    if (command == "request-close" && has_attribute(AttributeNames::open)) {
        auto const optional_value = as<FormAssociatedElement>(source).optional_value();
        request_close_the_dialog(optional_value, source);
    }

    // 4. If command is the Show Modal state and element does not have an open attribute,
    //    then show a modal dialog given element and source.
    if (command == "show-modal" && !has_attribute(AttributeNames::open)) {
        MUST(show_a_modal_dialog(*this, source));
    }
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#nearest-clicked-dialog
GC::Ptr<HTMLDialogElement> HTMLDialogElement::nearest_clicked_dialog(UIEvents::PointerEvent const& event, GC::Ptr<DOM::Node> const target)
{
    // To find the nearest clicked dialog, given a PointerEvent event:

    // 1. Let target be event's target.

    // 2. If target is a dialog element, target has an open attribute, target's is modal is true, and event's clientX
    //    and clientY are outside the bounds of target, then return null.
    if (auto const* target_dialog = as_if<HTMLDialogElement>(*target); target_dialog
        && target_dialog->has_attribute(AttributeNames::open)
        && target_dialog->is_modal()
        && !target_dialog->get_bounding_client_rect().to_type<double>().contains(event.client_x(), event.client_y()))
        return {};

    // 3. Let currentNode be target.
    auto current_node = target;

    // 4. While currentNode is not null:
    while (current_node) {
        // 1. If currentNode is a dialog element and currentNode has an open attribute, then return currentNode.
        if (auto* current_dialog = as_if<HTMLDialogElement>(*current_node); current_dialog
            && current_dialog->has_attribute(AttributeNames::open))
            return current_dialog;

        // 2. Set currentNode to currentNode's parent in the flat tree.
        current_node = current_node->first_flat_tree_ancestor_of_type<HTMLElement>();
    }

    // 5. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#light-dismiss-open-dialogs
void HTMLDialogElement::light_dismiss_open_dialogs(UIEvents::PointerEvent const& event, GC::Ptr<DOM::Node> const target)
{
    // To light dismiss open dialogs, given a PointerEvent event:

    // 1. Assert: event's isTrusted attribute is true.
    VERIFY(event.is_trusted());

    // 2. Let document be event's target's node document.
    // FIXME: The event's target hasn't been initialized yet, so it's passed as an argument
    auto& document = target->document();

    // 3. If document's open dialogs list is empty, then return.
    if (document.open_dialogs_list().is_empty())
        return;

    // 4. Let ancestor be the result of running nearest clicked dialog given event.
    auto const ancestor = nearest_clicked_dialog(event, target);

    // 5. If event's type is "pointerdown", then set document's dialog pointerdown target to ancestor.
    if (event.type() == UIEvents::EventNames::pointerdown)
        document.set_dialog_pointerdown_target(ancestor);

    // 6. If event's type is "pointerup", then:
    if (event.type() == UIEvents::EventNames::pointerup) {
        // 1. Let sameTarget be true if ancestor is document's dialog pointerdown target.
        bool const same_target = ancestor == document.dialog_pointerdown_target();

        // 2. Set document's dialog pointerdown target to null.
        document.set_dialog_pointerdown_target({});

        // 3. If sameTarget is false, then return.
        if (!same_target)
            return;

        // 4. Let topmostDialog be the last element of document's open dialogs list.
        auto const topmost_dialog = document.open_dialogs_list().last();

        // 5. If ancestor is topmostDialog, then return.
        if (ancestor == topmost_dialog)
            return;

        // 6. If topmostDialog's computed closed-by state is not Any, then return.
        // FIXME: This should use the "computed closed-by state" algorithm.
        auto closedby = topmost_dialog->attribute(AttributeNames::closedby);
        if (!closedby.has_value() || !closedby.value().equals_ignoring_ascii_case("any"sv))
            return;

        // 7. Assert: topmostDialog's close watcher is not null.
        VERIFY(topmost_dialog->m_close_watcher);

        // 8. Request to close topmostDialog's close watcher with false.
        topmost_dialog->request_close({});
    }
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#the-dialog-element:html-element-insertion-steps
void HTMLDialogElement::inserted()
{
    Base::inserted();

    // 1. If insertedNode's node document is not fully active, then return.
    if (!document().is_fully_active())
        return;

    // 2. If insertedNode has an open attribute and is connected, then run the dialog setup steps given insertedNode.
    if (has_attribute(AttributeNames::open) && is_connected())
        run_dialog_setup_steps();
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#the-dialog-element:concept-element-attributes-change-ext
void HTMLDialogElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);

    // 1. If namespace is not null, then return.
    if (namespace_.has_value())
        return;

    // 2. If localName is not open, then return.
    if (local_name != "open"_fly_string)
        return;

    // The :open pseudo-class can affect sibling selectors (e.g., dialog:open + sibling),
    // so we need full subtree + sibling invalidation, not just targeted invalidation.
    invalidate_style(DOM::StyleInvalidationReason::HTMLDetailsOrDialogOpenAttributeChange);

    // 3. If value is null and oldValue is not null, then run the dialog cleanup steps given element.
    if (!value.has_value() && old_value.has_value())
        run_dialog_cleanup_steps();

    // 4. If element's node document is not fully active, then return.
    if (!document().is_fully_active())
        return;

    // 5. If element is not connected, then return.
    if (!is_connected())
        return;

    // 6. If value is not null and oldValue is null, then run the dialog setup steps given element.
    if (value.has_value() && !old_value.has_value())
        run_dialog_setup_steps();
}

}
