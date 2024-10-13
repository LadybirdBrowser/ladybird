/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Console.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NavigateEventPrototype.h>
#include <LibWeb/DOM/AbortController.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/NavigateEvent.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationDestination.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(NavigateEvent);

JS::NonnullGCPtr<NavigateEvent> NavigateEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, NavigateEventInit const& event_init)
{
    return realm.heap().allocate<NavigateEvent>(realm, realm, event_name, event_init);
}

NavigateEvent::NavigateEvent(JS::Realm& realm, FlyString const& event_name, NavigateEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_navigation_type(event_init.navigation_type)
    , m_destination(*event_init.destination)
    , m_can_intercept(event_init.can_intercept)
    , m_user_initiated(event_init.user_initiated)
    , m_hash_change(event_init.hash_change)
    , m_signal(*event_init.signal)
    , m_form_data(event_init.form_data)
    , m_download_request(event_init.download_request)
    , m_info(event_init.info.value_or(JS::js_undefined()))
    , m_has_ua_visual_transition(event_init.has_ua_visual_transition)
{
}

NavigateEvent::~NavigateEvent() = default;

void NavigateEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NavigateEvent);
}

void NavigateEvent::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_navigation_handler_list);
    visitor.visit(m_abort_controller);
    visitor.visit(m_destination);
    visitor.visit(m_signal);
    visitor.visit(m_form_data);
    visitor.visit(m_info);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-intercept
WebIDL::ExceptionOr<void> NavigateEvent::intercept(NavigationInterceptOptions const& options)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // The intercept(options) method steps are:

    // 1. Perform shared checks given this.
    TRY(perform_shared_checks());

    // 2. If this's canIntercept attribute was initialized to false, then throw a "SecurityError" DOMException.
    if (!m_can_intercept)
        return WebIDL::SecurityError::create(realm, "NavigateEvent cannot be intercepted"_string);

    // 3. If this's dispatch flag is unset, then throw an "InvalidStateError" DOMException.
    if (!this->dispatched())
        return WebIDL::InvalidStateError::create(realm, "NavigationEvent is not dispatched yet"_string);

    // 4. Assert: this's interception state is either "none" or "intercepted".
    VERIFY(m_interception_state == InterceptionState::None || m_interception_state == InterceptionState::Intercepted);

    // 5. Set this's interception state to "intercepted".
    m_interception_state = InterceptionState::Intercepted;

    // 6. If options["handler"] exists, then append it to this's navigation handler list.
    if (options.handler != nullptr)
        TRY_OR_THROW_OOM(vm, m_navigation_handler_list.try_append(*options.handler));

    // 7. If options["focusReset"] exists, then:
    if (options.focus_reset.has_value()) {
        // 1.  If this's focus reset behavior is not null, and it is not equal to options["focusReset"],
        //     then the user agent may report a warning to the console indicating that the focusReset option
        //     for a previous call to intercept() was overridden by this new value, and the previous value
        //     will be ignored.
        if (m_focus_reset_behavior.has_value() && *m_focus_reset_behavior != *options.focus_reset) {
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(vm, String::formatted("focusReset behavior on NavigationEvent overriden (was: {}, now: {})", *m_focus_reset_behavior, *options.focus_reset)));
        }

        // 2. Set this's focus reset behavior to options["focusReset"].
        m_focus_reset_behavior = options.focus_reset;
    }

    // 8. If options["scroll"] exists, then:
    if (options.scroll.has_value()) {
        // 1. If this's scroll behavior is not null, and it is not equal to options["scroll"], then the user
        //    agent may report a warning to the console indicating that the scroll option for a previous call
        //    to intercept() was overridden by this new value, and the previous value will be ignored.
        if (m_scroll_behavior.has_value() && *m_scroll_behavior != *options.scroll) {
            auto& console = realm.intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Warn,
                TRY_OR_THROW_OOM(vm, String::formatted("scroll option on NavigationEvent overriden (was: {}, now: {})", *m_scroll_behavior, *options.scroll)));
        }

        // 2. Set this's scroll behavior to options["scroll"].
        m_scroll_behavior = options.scroll;
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-scroll
WebIDL::ExceptionOr<void> NavigateEvent::scroll()
{
    // The scroll() method steps are:
    // 1. Perform shared checks given this.
    TRY(perform_shared_checks());

    // 2. If this's interception state is not "committed", then throw an "InvalidStateError" DOMException.
    if (m_interception_state != InterceptionState::Committed)
        return WebIDL::InvalidStateError::create(realm(), "Cannot scroll NavigationEvent that is not committed"_string);

    // 3. Process scroll behavior given this.
    process_scroll_behavior();

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigateevent-perform-shared-checks
WebIDL::ExceptionOr<void> NavigateEvent::perform_shared_checks()
{
    // To perform shared checks for a NavigateEvent event:

    // 1. If event's relevant global object's associated Document is not fully active,
    //    then throw an "InvalidStateError" DOMException.
    auto& associated_document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create(realm(), "Document is not fully active"_string);

    // 2. If event's isTrusted attribute was initialized to false, then throw a "SecurityError" DOMException.
    if (!this->is_trusted())
        return WebIDL::SecurityError::create(realm(), "NavigateEvent is not trusted"_string);

    // 3. If event's canceled flag is set, then throw an "InvalidStateError" DOMException.
    if (this->cancelled())
        return WebIDL::InvalidStateError::create(realm(), "NavigateEvent already cancelled"_string);

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#process-scroll-behavior
void NavigateEvent::process_scroll_behavior()
{
    // To process scroll behavior given a NavigateEvent event:

    // 1. Assert: event's interception state is "committed".
    VERIFY(m_interception_state == InterceptionState::Committed);

    // 2. Set event's interception state to "scrolled".
    m_interception_state = InterceptionState::Scrolled;

    // FIXME: 3. If event's navigationType was initialized to "traverse" or "reload", then restore scroll position data
    //           given event's relevant global object's navigable's active session history entry.
    if (m_navigation_type == Bindings::NavigationType::Traverse || m_navigation_type == Bindings::NavigationType::Reload) {
        dbgln("FIXME: restore scroll position data after traversal or reload navigation");
    }

    // 4. Otherwise:
    else {
        // 1. Let document be event's relevant global object's associated Document.
        auto& document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();

        // 2. If document's indicated part is null, then scroll to the beginning of the document given document. [CSSOMVIEW]
        auto indicated_part = document.determine_the_indicated_part();
        if (indicated_part.has<DOM::Element*>() && indicated_part.get<DOM::Element*>() == nullptr) {
            document.scroll_to_the_beginning_of_the_document();
        }

        // 3. Otherwise, scroll to the fragment given document.
        else {
            // FIXME: This will re-determine the indicated part. Can we avoid this extra work?
            document.scroll_to_the_fragment();
        }
    }
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#potentially-process-scroll-behavior
void NavigateEvent::potentially_process_scroll_behavior()
{
    // 1. Assert: event's interception state is "committed" or "scrolled".
    VERIFY(m_interception_state != InterceptionState::Committed && m_interception_state != InterceptionState::Scrolled);

    // 2. If event's interception state is "scrolled", then return.
    if (m_interception_state == InterceptionState::Scrolled)
        return;

    // 3. If event's scroll behavior is "manual", then return.
    // NOTE: If it was left as null, then we treat that as "after-transition", and continue onward.
    if (m_scroll_behavior == Bindings::NavigationScrollBehavior::Manual)
        return;

    // 4. Process scroll behavior given event.
    process_scroll_behavior();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#potentially-reset-the-focus
void NavigateEvent::potentially_reset_the_focus()
{
    // 1. Assert: event's interception state is "committed" or "scrolled".
    VERIFY(m_interception_state == InterceptionState::Committed || m_interception_state == InterceptionState::Scrolled);

    // 2. Let navigation be event's relevant global object's navigation API.
    auto& relevant_global_object = verify_cast<Window>(HTML::relevant_global_object(*this));
    auto navigation = relevant_global_object.navigation();

    // 3. Let focusChanged be navigation's focus changed during ongoing navigation.
    auto focus_changed = navigation->focus_changed_during_ongoing_navigation();

    // 4. Set navigation's focus changed during ongoing navigation to false.
    navigation->set_focus_changed_during_ongoing_navigation(false);

    // 5. If focusChanged is true, then return.
    if (focus_changed)
        return;

    // 6. If event's focus reset behavior is "manual", then return.
    // NOTE: If it was left as null, then we treat that as "after-transition", and continue onward.
    if (m_focus_reset_behavior == Bindings::NavigationFocusReset::Manual)
        return;

    // 7. Let document be event's relevant global object's associated Document.
    auto& document = relevant_global_object.associated_document();

    // 8. FIXME: Let focusTarget be the autofocus delegate for document.
    JS::GCPtr<DOM::Node> focus_target = nullptr;

    // 9. If focusTarget is null, then set focusTarget to document's body element.
    if (focus_target == nullptr)
        focus_target = document.body();

    // 10. If focusTarget is null, then set focusTarget to document's document element.
    if (focus_target == nullptr)
        focus_target = document.document_element();

    // FIXME: 11. Run the focusing steps for focusTarget, with document's viewport as the fallback target.
    run_focusing_steps(focus_target, nullptr);

    // FIXME: 12. Move the sequential focus navigation starting point to focusTarget.
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigateevent-finish
void NavigateEvent::finish(bool did_fulfill)
{
    // 1. Assert: event's interception state is not "intercepted" or "finished".
    VERIFY(m_interception_state != InterceptionState::Intercepted && m_interception_state != InterceptionState::Finished);

    // 2. If event's interception state is "none", then return.
    if (m_interception_state == InterceptionState::None)
        return;

    // 3. Potentially reset the focus given event.
    potentially_reset_the_focus();

    // 4. If didFulfill is true, then potentially process scroll behavior given event.
    if (did_fulfill)
        potentially_process_scroll_behavior();

    // 5. Set event's interception state to "finished".
    m_interception_state = InterceptionState::Finished;
}

}
