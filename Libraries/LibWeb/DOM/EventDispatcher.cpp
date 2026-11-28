/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/Optional.h>
#include <AK/TypeCasts.h>
#include <LibWeb/DOM/BindingsGlue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/EventTiming/EventTiming.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/UIEvents/TouchEvent.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-event-listener-invoke
static Optional<Utf16FlyString> legacy_event_type_for_event_type(Utf16FlyString const& event_type)
{
    if (event_type == HTML::EventNames::animationend)
        return HTML::EventNames::webkitAnimationEnd;
    if (event_type == HTML::EventNames::animationiteration)
        return HTML::EventNames::webkitAnimationIteration;
    if (event_type == HTML::EventNames::animationstart)
        return HTML::EventNames::webkitAnimationStart;
    if (event_type == HTML::EventNames::transitionend)
        return HTML::EventNames::webkitTransitionEnd;
    // INTEROP: Other browsers use mousewheel as the legacy event type for trusted wheel events.
    if (event_type == UIEvents::EventNames::wheel) {
        static NeverDestroyed<Utf16FlyString> mousewheel_event_type { "mousewheel"_utf16_fly_string };
        return *mousewheel_event_type;
    }
    return {};
}

// Steps 6 to 15 of https://dom.spec.whatwg.org/#concept-event-listener-inner-invoke, which inner invoke in libweb_rust
// runs for each listener it gets to, after steps 1 to 5.
static void run_listener(Event& event, DOMEventListener& listener, bool invocation_target_in_shadow_tree, bool& legacy_output_did_listeners_throw)
{
    auto& callback = listener.callback->callback();

    // 6. Let targetGlobal be event's currentTarget's node document's relevant global object if event's currentTarget
    //    is a node; otherwise event's currentTarget's relevant global object.
    auto* current_target_node = as_if<Node>(event.current_target().ptr());
    auto* target_global = current_target_node
        ? current_target_node->document().relevant_global_event_target().ptr()
        : Bindings::current_target_global(callback, event);

    // 7. Let listenerGlobal be listener callback's associated realm's global object.
    auto* listener_window = Bindings::window_from_callback(callback);

    // 8. If targetGlobal or listenerGlobal is a Window object whose associated Document is not fully active, then
    //    continue.
    if (listener_window && !listener_window->associated_document().is_fully_active())
        return;
    // NB: Mostly, targetGlobal is listenerGlobal, which was just checked.
    if (target_global != listener_window) {
        if (auto* target_window = as_if<HTML::Window>(target_global); target_window && !target_window->associated_document().is_fully_active())
            return;
    }

    // 9. Let currentEvent be undefined.
    Event* current_event = nullptr;

    // 10. If listenerGlobal is a Window object:
    if (listener_window) {
        // 1. Set currentEvent to listenerGlobal's current event.
        current_event = listener_window->current_event();

        // 2. If invocationTargetInShadowTree is false, then set listenerGlobal's current event to event.
        if (!invocation_target_in_shadow_tree)
            listener_window->set_current_event(&event);
    }

    // 11. If listener’s passive is true, then set event’s in passive listener flag.
    if (listener.passive() == true)
        event.set_in_passive_listener(true);

    // FIXME: 12. If listenerGlobal is a Window object, then record timing info for event listener given event and listener.

    // 13. Call a user object’s operation with listener’s callback, "handleEvent", « event », and event’s currentTarget attribute value.
    auto result = Bindings::invoke_event_listener(callback, event);

    // If this throws an exception, then:
    if (result.is_error()) {
        // 1. Report exception for listener’s callback’s corresponding JavaScript object’s associated realm’s global object.
        Bindings::report_exception_for_callback(callback, result.release_error().value());

        // 2. Set legacyOutputDidListenersThrowFlag if given. (Only used by IndexedDB currently)
        legacy_output_did_listeners_throw = true;
    }

    // 14. Unset event’s in passive listener flag.
    event.set_in_passive_listener(false);

    // 15. If listenerGlobal is a Window object, then set listenerGlobal's current event to currentEvent.
    if (listener_window)
        listener_window->set_current_event(current_event);
}

static bool button_submits_or_resets_its_form(HTML::HTMLButtonElement const& button)
{
    return button.form() && (button.is_submit_button() || button.type_state() == HTML::HTMLButtonElement::TypeAttributeState::Reset);
}

static GC::Ptr<EventTarget> link_activation_target_above_button(Event const& event, HTML::HTMLButtonElement const& button)
{
    bool passed_button = false;
    GC::Ptr<EventTarget> intervening_activation_target;
    for (auto const& entry : event.path()) {
        auto& invocation_target = *entry.invocation_target;
        if (&invocation_target == &button) {
            passed_button = true;
            continue;
        }
        if (!passed_button || !invocation_target.has_activation_behavior())
            continue;

        // INTEROP: An intervening form-associated submit or reset button consumes activation
        //          before it can reach the link, even when its form action is canceled.
        if (auto* ancestor_button = as_if<HTML::HTMLButtonElement>(invocation_target); ancestor_button
            && ancestor_button->enabled() && button_submits_or_resets_its_form(*ancestor_button)) {
            if (!intervening_activation_target)
                intervening_activation_target = ancestor_button;
            continue;
        }

        if (is<HTML::HTMLAnchorElement>(invocation_target))
            return intervening_activation_target ? intervening_activation_target : GC::Ptr<EventTarget> { invocation_target };

        // NB: Keep other intervening activation behaviors, such as labels, as boundaries.
        //     The link fallback must not bypass them just because the clicked button is plain.
        return {};
    }
    return {};
}

// The callbacks of the dispatch in libweb_rust, see Rust/src/dom/dispatch.rs.
struct RustEventHooks {
    static void run_listener(Event* event, DOMEventListener* listener, bool invocation_target_in_shadow_tree, bool* legacy_output_did_listeners_throw)
    {
        DOM::run_listener(*event, *listener, invocation_target_in_shadow_tree, *legacy_output_did_listeners_throw);
    }

    static void run_event_listener_removal_steps(EventTarget* target, DOMEventListener* listener)
    {
        target->run_event_listener_removal_steps(*listener);
    }

    static void set_event_type(Event* event, size_t type)
    {
        event->set_type(Utf16FlyString::from_raw(type));
    }

    static EventTarget* get_the_parent(EventTarget* target, Event* event)
    {
        return target->get_parent(*event);
    }

    static bool has_activation_behavior(EventTarget* target)
    {
        return target->has_activation_behavior();
    }

    static bool is_disabled_form_control(EventTarget* target)
    {
        return as<Element>(*target).is_disabled_form_control();
    }

    static void set_touch_target_list(Event* event, ReadonlySpan<GC::Member<EventTarget>> touch_targets)
    {
        event->set_touch_target_list(touch_targets);
        if (auto* touch_event = as_if<UIEvents::TouchEvent>(*event))
            touch_event->retarget_touch_lists();
    }

    static void set_target_of_coalesced_events(Event* event)
    {
        as<UIEvents::PointerEvent>(*event).set_target_of_coalesced_events(event->target());
    }

    static bool run_legacy_pre_activation_behavior(EventTarget* activation_target, EventTarget* activation_event_target)
    {
        // NB: Keep the original interactive ancestry if listeners remove or reparent the clicked node.
        bool is_interactive_label_descendant = false;
        if (auto* label = as_if<HTML::HTMLLabelElement>(*activation_target))
            is_interactive_label_descendant = label->is_interactive_descendant_event_target(activation_event_target);

        activation_target->legacy_pre_activation_behavior();
        return is_interactive_label_descendant;
    }

    static EventTarget* activation_target_after_listeners(Event* event, EventTarget* activation_target)
    {
        if (auto* button = as_if<HTML::HTMLButtonElement>(*activation_target); button && event->bubbles()) {
            if (auto link_activation_target = link_activation_target_above_button(*event, *button); link_activation_target
                && button->enabled() && !button_submits_or_resets_its_form(*button)
                && !button->get_the_attribute_associated_element(HTML::AttributeNames::commandfor, button->command_for_element())
                && !HTML::PopoverTargetAttributes::get_the_popover_target_element(*button)) {
                return link_activation_target.ptr();
            }
        }
        return activation_target;
    }

    static void run_activation_behavior(Event* event, EventTarget* activation_target, bool is_interactive_label_descendant)
    {
        if (auto* label = as_if<HTML::HTMLLabelElement>(*activation_target))
            label->activation_behavior(*event, is_interactive_label_descendant);
        else
            activation_target->activation_behavior(*event);
        activation_target->legacy_cancelled_activation_behavior_was_not_called();
    }

    static void run_legacy_canceled_activation_behavior(EventTarget* activation_target)
    {
        activation_target->legacy_cancelled_activation_behavior();
    }
};

static constexpr RustFFI::EventHooks s_rust_event_hooks {
    .run_listener = RustEventHooks::run_listener,
    .run_event_listener_removal_steps = RustEventHooks::run_event_listener_removal_steps,
    .set_event_type = RustEventHooks::set_event_type,
    .get_the_parent = RustEventHooks::get_the_parent,
    .has_activation_behavior = RustEventHooks::has_activation_behavior,
    .is_disabled_form_control = RustEventHooks::is_disabled_form_control,
    .set_touch_target_list = RustEventHooks::set_touch_target_list,
    .set_target_of_coalesced_events = RustEventHooks::set_target_of_coalesced_events,
    .run_legacy_pre_activation_behavior = RustEventHooks::run_legacy_pre_activation_behavior,
    .activation_target_after_listeners = RustEventHooks::activation_target_after_listeners,
    .run_activation_behavior = RustEventHooks::run_activation_behavior,
    .run_legacy_canceled_activation_behavior = RustEventHooks::run_legacy_canceled_activation_behavior,
};

static struct RustEventHooksRegistration {
    RustEventHooksRegistration() { RustFFI::rust_dom_register_event_hooks(&s_rust_event_hooks); }
} s_rust_event_hooks_registration;

// https://dom.spec.whatwg.org/#concept-event-dispatch
bool EventDispatcher::dispatch(GC::Ref<EventTarget> target, Event& event, bool legacy_target_override)
{
    bool legacy_output_did_listeners_throw = false;
    return dispatch(target, event, legacy_target_override, legacy_output_did_listeners_throw);
}

// https://dom.spec.whatwg.org/#concept-event-dispatch
bool EventDispatcher::dispatch(GC::Ref<EventTarget> target, Event& event, bool legacy_target_override, bool& legacy_output_did_listeners_throw)
{
    // https://w3c.github.io/largest-contentful-paint/#sec-modifications-DOM
    // If target's relevant global object is a Window object, event's type is scroll and its isTrusted is true, set
    // target's relevant global object's has dispatched scroll event to true.
    if (event.is_trusted() && event.type() == HTML::EventNames::scroll) {
        if (auto* window = EventTiming::relevant_window_of_event_target(*target))
            window->set_has_dispatched_scroll_event();
    }

    // https://w3c.github.io/event-timing/#sec-modifications-DOM
    // OPTIMIZATION: Only trusted events of some types are timed, so nothing else needs to look up the window.
    GC::Ptr<EventTiming::PerformanceEventTiming> timing_entry;
    GC::Ptr<HTML::Window> timing_window;
    if (EventTiming::is_considered_for_event_timing(event)) {
        timing_window = EventTiming::relevant_window_of_event_target(*target);
        if (timing_window) {
            // 1. Let interactionId be the result of computing interactionId given event.
            auto interaction_id = EventTiming::compute_interaction_id(event, *timing_window);

            // 2. Let timingEntry be the result of initializing event timing given event, the current high resolution
            //    time, and interactionId.
            timing_entry = EventTiming::initialize_event_timing(event, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*timing_window)), interaction_id);
        }
    }

    // 2. Let targetOverride be target, if legacy target override flag is not given, and target's associated Document
    //    otherwise. [HTML]
    // NOTE: legacy target override flag is only used by HTML and only when target is a Window object.
    GC::Ref<EventTarget> target_override = legacy_target_override ? GC::Ref<EventTarget> { as<HTML::Window>(*target).associated_document() } : target;

    // NB: The dispatch in libweb_rust tells which targets may have listeners for the event by the bit of its type in
    //     their listener type summaries, and by that of its legacy type, as a trusted event's listeners of that type
    //     run where none of its own type do. While those run, it names the event by its legacy type, and by the raw
    //     identity of its type again after, which stays valid as every type with a legacy type is one of the
    //     EventNames, which live as long as the process.
    u16 type_bits = event_type_summary_bit(event.type());
    Optional<Utf16FlyString> legacy_event_type;
    if (event.is_trusted()) {
        legacy_event_type = legacy_event_type_for_event_type(event.type());
        if (legacy_event_type.has_value())
            type_bits |= event_type_summary_bit(*legacy_event_type);
    }

    u8 flags = 0;
    // 4. Let isActivationEvent be true, if event is a MouseEvent object and event's type attribute is "click";
    //    otherwise false.
    bool is_mouse_event = is<UIEvents::MouseEvent>(event);
    if (is_mouse_event && event.type() == HTML::EventNames::click)
        flags |= RustFFI::DISPATCH_IS_ACTIVATION_EVENT;
    if (event.type() == HTML::EventNames::load)
        flags |= RustFFI::DISPATCH_TYPE_IS_LOAD;
    if (event.is_trusted() && is_mouse_event
        && (event.type() == UIEvents::EventNames::mousedown || event.type() == UIEvents::EventNames::mouseup
            || event.type() == UIEvents::EventNames::click || event.type() == UIEvents::EventNames::dblclick))
        flags |= RustFFI::DISPATCH_PATH_ENDS_BELOW_DISABLED_FORM_CONTROL;
    if (event.is_trusted() && is<UIEvents::PointerEvent>(event))
        flags |= RustFFI::DISPATCH_IS_TRUSTED_POINTER_EVENT;

    auto const* last_target = RustFFI::rust_dom_dispatch(&event, target.ptr(), target_override.ptr(), event.touch_target_list().span(), type_bits, event.type().raw_identity(), legacy_event_type.has_value() ? legacy_event_type->raw_identity() : 0, flags, &legacy_output_did_listeners_throw);

    // https://w3c.github.io/event-timing/#sec-modifications-DOM
    // 1. Finalize event timing passing timingEntry, event, target, and the current high resolution time as inputs.
    if (timing_entry)
        EventTiming::finalize_event_timing(timing_entry, event, const_cast<EventTarget&>(*last_target), HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*timing_window)));

    // 13. Return false if event's canceled flag is set; otherwise true.
    return !event.cancelled();
}

// The "get the parent" of target given event, which is not being dispatched, as it would be if origin were the
// invocation target of the first item of event's path.
// https://dom.spec.whatwg.org/#get-the-parent
EventTarget* get_the_parent(EventTarget& target, Event const& event, EventTarget& origin)
{
    return const_cast<EventTarget*>(RustFFI::rust_dom_get_the_parent(&event, &target, &origin, event.type() == HTML::EventNames::load));
}

}
