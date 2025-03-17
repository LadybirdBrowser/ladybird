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

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLDialogElement);

HTMLDialogElement::HTMLDialogElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLDialogElement::~HTMLDialogElement() = default;

void HTMLDialogElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLDialogElement);
}

void HTMLDialogElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_close_watcher);
}

void HTMLDialogElement::removed_from(Node* old_parent, Node& old_root)
{
    HTMLElement::removed_from(old_parent, old_root);

    // 1. If removedNode's close watcher is not null, then:
    if (m_close_watcher) {
        // 1.1. Destroy removedNode's close watcher.
        m_close_watcher->destroy();
        // 1.2. Set removedNode's close watcher to null.
        m_close_watcher = nullptr;
    }

    // 2. If removedNode's node document's top layer contains removedNode, then remove an element from the top layer
    //    immediately given removedNode.
    if (document().top_layer_elements().contains(*this))
        document().remove_an_element_from_the_top_layer_immediately(*this);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#queue-a-dialog-toggle-event-task
void HTMLDialogElement::queue_a_dialog_toggle_event_task(AK::String old_state, AK::String new_state)
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
    auto task_id = queue_an_element_task(Task::Source::DOMManipulation, [this, old_state, new_state = move(new_state)]() {
        // 1. Fire an event named toggle at element, using ToggleEvent, with the oldState attribute initialized to
        //    oldState and the newState attribute initialized to newState.
        ToggleEventInit event_init {};
        event_init.old_state = move(old_state);
        event_init.new_state = move(new_state);

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
        return WebIDL::InvalidStateError::create(realm(), "Dialog already open"_string);

    // 3. If the result of firing an event named beforetoggle, using ToggleEvent,
    //  with the cancelable attribute initialized to true, the oldState attribute initialized to "closed",
    //  and the newState attribute initialized to "open" at this is false, then return.
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

    // 5. Queue a dialog toggle event task given this, "closed", and "open".
    queue_a_dialog_toggle_event_task("closed"_string, "open"_string);

    // 6. Add an open attribute to this, whose value is the empty string.
    TRY(set_attribute(AttributeNames::open, {}));

    // FIXME: 7. Assert: this's node document's open dialogs list does not contain this.
    // FIXME: 8. Add this to this's node document's open dialogs list.
    // 9. Set the dialog close watcher with this.
    set_close_watcher();
    // FIXME: 10. Set this's previously focused element to the focused element.

    // 11. Let document be this's node document.
    auto document = m_document;

    // 12. Let hideUntil be the result of running topmost popover ancestor given this, document's showing hint popover list, null, and false.
    Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> hide_until = topmost_popover_ancestor(this, document->showing_hint_popover_list(), nullptr, IsPopover::No);

    // 13. If hideUntil is null, then set hideUntil to the result of running topmost popover ancestor given this, document's showing auto popover list, null, and false.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = topmost_popover_ancestor(this, document->showing_auto_popover_list(), nullptr, IsPopover::No);

    // 14. If hideUntil is null, then set hideUntil to document.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = document;

    // 15. Run hide all popovers until given hideUntil, false, and true.
    hide_all_popovers_until(hide_until, FocusPreviousElement::No, FireEvents::Yes);

    // 16. Run the dialog focusing steps given this.
    run_dialog_focusing_steps();

    return {};
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-showmodal
WebIDL::ExceptionOr<void> HTMLDialogElement::show_modal()
{
    // The showModal() method steps are to show a modal dialog given this.
    return show_a_modal_dialog(*this);
}

WebIDL::ExceptionOr<void> HTMLDialogElement::show_a_modal_dialog(HTMLDialogElement& subject)
{
    // To show a modal dialog given a dialog element subject:
    auto& realm = subject.realm();

    // 1. If subject has an open attribute and is modal of subject is true, then return.
    if (subject.has_attribute(AttributeNames::open) && subject.m_is_modal)
        return {};

    // 2. If subject has an open attribute, then throw an "InvalidStateError" DOMException.
    if (subject.has_attribute(AttributeNames::open))
        return WebIDL::InvalidStateError::create(realm, "Dialog already open"_string);

    // 3. If subject's node document is not fully active, then throw an "InvalidStateError" DOMException.
    if (!subject.document().is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string);

    // 4. If subject is not connected, then throw an "InvalidStateError" DOMException.
    if (!subject.is_connected())
        return WebIDL::InvalidStateError::create(realm, "Dialog not connected"_string);

    // 5. If subject is in the popover showing state, then throw an "InvalidStateError" DOMException.
    if (subject.popover_visibility_state() == PopoverVisibilityState::Showing)
        return WebIDL::InvalidStateError::create(realm, "Dialog already open as popover"_string);

    // 6. If the result of firing an event named beforetoggle, using ToggleEvent,
    //  with the cancelable attribute initialized to true, the oldState attribute initialized to "closed",
    //  and the newState attribute initialized to "open" at subject is false, then return.
    ToggleEventInit event_init {};
    event_init.cancelable = true;
    event_init.old_state = "closed"_string;
    event_init.new_state = "open"_string;

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

    // 10. Queue a dialog toggle event task given subject, "closed", and "open".
    subject.queue_a_dialog_toggle_event_task("closed"_string, "open"_string);

    // 11. Add an open attribute to subject, whose value is the empty string.
    TRY(subject.set_attribute(AttributeNames::open, {}));

    // 12. Set is modal of subject to true.
    subject.m_is_modal = true;

    // FIXME: 13. Assert: subject's node document's open dialogs list does not contain subject.
    // FIXME: 14. Add subject to subject's node document's open dialogs list.
    // FIXME: 15. Let subject's node document be blocked by the modal dialog subject.

    // 16. If subject's node document's top layer does not already contain subject, then add an element to the top layer given subject.
    if (!subject.document().top_layer_elements().contains(subject))
        subject.document().add_an_element_to_the_top_layer(subject);

    // 17. Set the dialog close watcher with subject.
    subject.set_close_watcher();

    // FIXME: 18. Set subject's previously focused element to the focused element.

    // 19. Let document be subject's node document.
    auto& document = subject.document();

    // 20. Let hideUntil be the result of running topmost popover ancestor given subject, document's showing hint popover list, null, and false.
    Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> hide_until = topmost_popover_ancestor(subject, document.showing_hint_popover_list(), nullptr, IsPopover::No);

    // 21. If hideUntil is null, then set hideUntil to the result of running topmost popover ancestor given subject, document's showing auto popover list, null, and false.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = topmost_popover_ancestor(subject, document.showing_auto_popover_list(), nullptr, IsPopover::No);

    // 22. If hideUntil is null, then set hideUntil to document.
    if (!hide_until.get<GC::Ptr<HTMLElement>>())
        hide_until = GC::Ptr(document);

    // 23. Run hide all popovers until given hideUntil, false, and true.
    hide_all_popovers_until(hide_until, FocusPreviousElement::No, FireEvents::Yes);

    // 24. Run the dialog focusing steps given subject.
    subject.run_dialog_focusing_steps();

    return {};
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-close
void HTMLDialogElement::close(Optional<String> return_value)
{
    // 1. If returnValue is not given, then set it to null.
    // 2. Close the dialog this with returnValue.
    close_the_dialog(move(return_value));
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dom-dialog-requestclose
void HTMLDialogElement::request_close(Optional<String> return_value)
{
    // 1. If this does not have an open attribute, then return.
    if (!has_attribute(AttributeNames::open))
        return;
    // ADHOC: 2. If this's close watcher is null, then close the dialog this with returnValue, and return. See https://github.com/whatwg/html/pull/10983
    if (!m_close_watcher) {
        close_the_dialog(move(return_value));
        return;
    }
    // 3. Set dialog's enable close watcher for requestClose() to true.
    // ADHOC: Implemented slightly differently to the spec, as the spec is unnecessarily complex.
    m_close_watcher->set_enabled(true);
    // 4. If returnValue is not given, then set it to null.
    // 5. Set this's request close return value to returnValue.
    m_request_close_return_value = return_value;
    // 6. Request to close dialog's close watcher with false.
    m_close_watcher->request_close(false);
    // 7. Set dialog's enable close watcher for requestClose() to false.
    // ADHOC: Implemented slightly differently to the spec, as the spec is unnecessarily complex.
    // FIXME: This should be set based on dialog closedby state, when implemented.
    if (m_close_watcher)
        m_close_watcher->set_enabled(m_is_modal);
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
void HTMLDialogElement::close_the_dialog(Optional<String> result)
{
    // 1. If subject does not have an open attribute, then return.
    if (!has_attribute(AttributeNames::open))
        return;

    // 2. Fire an event named beforetoggle, using ToggleEvent, with the oldState attribute initialized to "open" and the newState attribute initialized to "closed" at subject.
    ToggleEventInit event_init {};
    event_init.old_state = "open"_string;
    event_init.new_state = "closed"_string;

    dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::beforetoggle, move(event_init)));

    // 3. If subject does not have an open attribute, then return.
    if (!has_attribute(AttributeNames::open))
        return;

    // 4. Queue a dialog toggle event task given subject, "open", and "closed".
    queue_a_dialog_toggle_event_task("open"_string, "closed"_string);

    // 5. Remove subject's open attribute.
    remove_attribute(AttributeNames::open);

    // 6. If the is modal flag of subject is true, then request an element to be removed from the top layer given subject.
    if (m_is_modal)
        document().request_an_element_to_be_remove_from_the_top_layer(*this);

    // FIXME: 7. Let wasModal be the value of subject's is modal flag.

    // 8. Set the is modal flag of subject to false.
    m_is_modal = false;

    // FIXME: 9. Remove subject from subject's node document's open dialogs list.

    // 10. If result is not null, then set the returnValue attribute to result.
    if (result.has_value())
        set_return_value(result.release_value());

    // 11. Set the request close return value to null.
    m_request_close_return_value = {};

    // FIXME: 12. If subject's previously focused element is not null, then:
    //           1. Let element be subject's previously focused element.
    //           2. Set subject's previously focused element to null.
    //           3. If subject's node document's focused area of the document's DOM anchor is a shadow-including inclusive descendant of subject,
    //              or wasModal is true, then run the focusing steps for element; the viewport should not be scrolled by doing this step.

    // 13. Queue an element task on the user interaction task source given the subject element to fire an event named close at subject.
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
        auto close_event = DOM::Event::create(realm(), HTML::EventNames::close);
        dispatch_event(close_event);
    });

    // 14. If subject's close watcher is not null, then:
    if (m_close_watcher) {
        // 14.1 Destroy subject's close watcher.
        m_close_watcher->destroy();
        // 14.2 Set subject's close watcher to null.
        m_close_watcher = nullptr;
    }
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#set-the-dialog-close-watcher
void HTMLDialogElement::set_close_watcher()
{
    // 1. Set dialog's close watcher to the result of establishing a close watcher given dialog's relevant global object, with:
    m_close_watcher = CloseWatcher::establish(*document().window());
    // - cancelAction given canPreventClose being to return the result of firing an event named cancel at dialog, with the cancelable attribute initialized to canPreventClose.
    auto cancel_callback_function = JS::NativeFunction::create(
        realm(), [this](JS::VM& vm) {
            auto& event = as<DOM::Event>(vm.argument(0).as_object());
            bool can_prevent_close = event.cancelable();
            auto should_continue = dispatch_event(DOM::Event::create(realm(), HTML::EventNames::cancel, { .cancelable = can_prevent_close }));
            if (!should_continue)
                event.prevent_default();
            return JS::js_undefined();
        },
        0, ""_fly_string, &realm());
    auto cancel_callback = realm().heap().allocate<WebIDL::CallbackType>(*cancel_callback_function, realm());
    m_close_watcher->add_event_listener_without_options(HTML::EventNames::cancel, DOM::IDLEventListener::create(realm(), cancel_callback));
    // - closeAction being to close the dialog given dialog and dialog's request close return value.
    auto close_callback_function = JS::NativeFunction::create(
        realm(), [this](JS::VM&) {
            close_the_dialog(m_request_close_return_value);

            return JS::js_undefined();
        },
        0, ""_fly_string, &realm());
    auto close_callback = realm().heap().allocate<WebIDL::CallbackType>(*close_callback_function, realm());
    m_close_watcher->add_event_listener_without_options(HTML::EventNames::close, DOM::IDLEventListener::create(realm(), close_callback));
    // - getEnabledState being to return true if dialog's enable close watcher for requestClose() is true or dialog's computed closed-by state is not None; otherwise false.
    // ADHOC: Implemented slightly differently to the spec, as the spec is unnecessarily complex.
    // FIXME: This should be set based on dialog closedby state, when implemented.
    m_close_watcher->set_enabled(m_is_modal);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#dialog-focusing-steps
void HTMLDialogElement::run_dialog_focusing_steps()
{
    // 1. If the allow focus steps given subject's node document return false, then return.
    if (!document().allow_focus())
        return;

    // 2. Let control be null
    GC::Ptr<Element> control = nullptr;

    // FIXME 3. If subject has the autofocus attribute, then set control to subject.
    // FIXME 4. If control is null, then set control to the focus delegate of subject.

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

}
