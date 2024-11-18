/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/DOM/Utils.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-event-listener-inner-invoke
bool EventDispatcher::inner_invoke(Event& event, Vector<GC::Root<DOM::DOMEventListener>>& listeners, Event::Phase phase, bool invocation_target_in_shadow_tree)
{
    // 1. Let found be false.
    bool found = false;

    // 2. For each listener in listeners, whose removed is false:
    for (auto& listener : listeners) {
        if (listener->removed)
            continue;

        // 1. If event’s type attribute value is not listener’s type, then continue.
        if (event.type() != listener->type)
            continue;

        // 2. Set found to true.
        found = true;

        // 3. If phase is "capturing" and listener’s capture is false, then continue.
        if (phase == Event::Phase::CapturingPhase && !listener->capture)
            continue;

        // 4. If phase is "bubbling" and listener’s capture is true, then continue.
        if (phase == Event::Phase::BubblingPhase && listener->capture)
            continue;

        // 5. If listener’s once is true, then remove an event listener given event’s currentTarget attribute value and listener.
        if (listener->once)
            event.current_target()->remove_an_event_listener(*listener);

        // 6. Let global be listener callback’s associated Realm’s global object.
        auto& callback = listener->callback->callback();
        auto& realm = callback.callback->shape().realm();
        auto& global = realm.global_object();

        // 7. Let currentEvent be undefined.
        Event* current_event = nullptr;

        // 8. If global is a Window object, then:
        if (is<HTML::Window>(global)) {
            auto& window = verify_cast<HTML::Window>(global);

            // 1. Set currentEvent to global’s current event.
            current_event = window.current_event();

            // 2. If invocationTargetInShadowTree is false, then set global’s current event to event.
            if (!invocation_target_in_shadow_tree)
                window.set_current_event(&event);
        }

        // 9. If listener’s passive is true, then set event’s in passive listener flag.
        if (listener->passive)
            event.set_in_passive_listener(true);

        // 10. Call a user object’s operation with listener’s callback, "handleEvent", « event », and event’s currentTarget attribute value. If this throws an exception, then:
        // FIXME: These should be wrapped for us in call_user_object_operation, but it currently doesn't do that.
        auto* this_value = event.current_target().ptr();
        auto* wrapped_event = &event;
        auto result = WebIDL::call_user_object_operation(callback, "handleEvent"_string, this_value, wrapped_event);

        // If this throws an exception, then:
        if (result.is_error()) {
            // 1. Report exception for listener’s callback’s corresponding JavaScript object’s associated realm’s global object.
            auto* window_or_worker = dynamic_cast<HTML::WindowOrWorkerGlobalScopeMixin*>(&global);
            VERIFY(window_or_worker);
            window_or_worker->report_an_exception(*result.release_error().value());

            // FIXME: 2. Set legacyOutputDidListenersThrowFlag if given. (Only used by IndexedDB currently)
        }

        // 11. Unset event’s in passive listener flag.
        event.set_in_passive_listener(false);

        // 12. If global is a Window object, then set global’s current event to currentEvent.
        if (is<HTML::Window>(global)) {
            auto& window = verify_cast<HTML::Window>(global);
            window.set_current_event(current_event);
        }

        // 13. If event’s stop immediate propagation flag is set, then return found.
        if (event.should_stop_immediate_propagation())
            return found;
    }

    // 3. Return found.
    return found;
}

// https://dom.spec.whatwg.org/#concept-event-listener-invoke
void EventDispatcher::invoke(Event::PathEntry& struct_, Event& event, Event::Phase phase)
{
    auto last_valid_shadow_adjusted_target = event.path().last_matching([&struct_](auto& entry) {
        return entry.index <= struct_.index && entry.shadow_adjusted_target;
    });

    VERIFY(last_valid_shadow_adjusted_target.has_value());

    // 1. Set event’s target to the shadow-adjusted target of the last struct in event’s path,
    // that is either struct or preceding struct, whose shadow-adjusted target is non-null.
    event.set_target(last_valid_shadow_adjusted_target.value().shadow_adjusted_target.ptr());

    // 2. Set event’s relatedTarget to struct’s relatedTarget.
    event.set_related_target(struct_.related_target.ptr());

    // 3. Set event’s touch target list to struct’s touch target list.
    event.set_touch_target_list(struct_.touch_target_list);

    // 4. If event’s stop propagation flag is set, then return.
    if (event.should_stop_propagation())
        return;

    // 5. Initialize event’s currentTarget attribute to struct’s invocation target.
    event.set_current_target(struct_.invocation_target.ptr());

    // 6. Let listeners be a clone of event’s currentTarget attribute value’s event listener list.
    // NOTE: This avoids event listeners added after this point from being run. Note that removal still has an effect due to the removed field.
    auto listeners = event.current_target()->event_listener_list();

    // 7. Let invocationTargetInShadowTree be struct’s invocation-target-in-shadow-tree.
    bool invocation_target_in_shadow_tree = struct_.invocation_target_in_shadow_tree;

    // 8. Let found be the result of running inner invoke with event, listeners, phase, invocationTargetInShadowTree, and legacyOutputDidListenersThrowFlag if given.
    bool found = inner_invoke(event, listeners, phase, invocation_target_in_shadow_tree);

    // 9. If found is false and event’s isTrusted attribute is true, then:
    if (!found && event.is_trusted()) {
        // 1. Let originalEventType be event’s type attribute value.
        auto original_event_type = event.type();

        // 2. If event’s type attribute value is a match for any of the strings in the first column in the following table,
        //    set event’s type attribute value to the string in the second column on the same row as the matching string, and return otherwise.
        if (event.type() == HTML::EventNames::animationend)
            event.set_type(HTML::EventNames::webkitAnimationEnd);
        else if (event.type() == HTML::EventNames::animationiteration)
            event.set_type(HTML::EventNames::webkitAnimationIteration);
        else if (event.type() == HTML::EventNames::animationstart)
            event.set_type(HTML::EventNames::webkitAnimationStart);
        else if (event.type() == HTML::EventNames::transitionend)
            event.set_type(HTML::EventNames::webkitTransitionEnd);
        else
            return;

        // 3. Inner invoke with event, listeners, phase, invocationTargetInShadowTree, and legacyOutputDidListenersThrowFlag if given.
        inner_invoke(event, listeners, phase, invocation_target_in_shadow_tree);

        // 4. Set event’s type attribute value to originalEventType.
        event.set_type(original_event_type);
    }
}

// https://dom.spec.whatwg.org/#concept-event-dispatch
bool EventDispatcher::dispatch(GC::Ref<EventTarget> target, Event& event, bool legacy_target_override)
{
    // 1. Set event’s dispatch flag.
    event.set_dispatched(true);

    // 2. Let targetOverride be target, if legacy target override flag is not given, and target’s associated Document otherwise. [HTML]
    // NOTE: legacy target override flag is only used by HTML and only when target is a Window object.
    GC::Ptr<EventTarget> target_override;
    if (!legacy_target_override) {
        target_override = target;
    } else {
        target_override = &verify_cast<HTML::Window>(*target).associated_document();
    }

    // 3. Let activationTarget be null.
    GC::Ptr<EventTarget> activation_target;

    // 4. Let relatedTarget be the result of retargeting event’s relatedTarget against target.
    GC::Ptr<EventTarget> related_target = retarget(event.related_target(), target);

    bool clear_targets = false;
    // 5. If target is not relatedTarget or target is event’s relatedTarget, then:
    if (related_target != target || event.related_target() == target) {
        // 1. Let touchTargets be a new list.
        Event::TouchTargetList touch_targets;

        // 2. For each touchTarget of event’s touch target list, append the result of retargeting touchTarget against target to touchTargets.
        for (auto& touch_target : event.touch_target_list()) {
            touch_targets.append(retarget(touch_target, target));
        }

        // 3. Append to an event path with event, target, targetOverride, relatedTarget, touchTargets, and false.
        event.append_to_path(*target, target_override, related_target, touch_targets, false);

        // 4. Let isActivationEvent be true, if event is a MouseEvent object and event’s type attribute is "click"; otherwise false.
        bool is_activation_event = is<UIEvents::MouseEvent>(event) && event.type() == HTML::EventNames::click;

        // 5. If isActivationEvent is true and target has activation behavior, then set activationTarget to target.
        if (is_activation_event && target->has_activation_behavior())
            activation_target = target;

        // 6. Let slottable be target, if target is a slottable and is assigned, and null otherwise.
        GC::Ptr<EventTarget> slottable;

        if (is<Node>(*target) && is_an_assigned_slottable(static_cast<Node&>(*target)))
            slottable = target;

        // 7. Let slot-in-closed-tree be false
        bool slot_in_closed_tree = false;

        // 8. Let parent be the result of invoking target’s get the parent with event.
        auto* parent = target->get_parent(event);

        // 9. While parent is non-null:
        while (parent) {
            // 1. If slottable is non-null:
            if (slottable != nullptr) {
                // 1. Assert: parent is a slot.
                VERIFY(is<HTML::HTMLSlotElement>(parent));

                // 2. Set slottable to null.
                slottable = nullptr;

                // 3. If parent’s root is a shadow root whose mode is "closed", then set slot-in-closed-tree to true.
                auto& parent_root = static_cast<Node&>(*parent).root();

                if (parent_root.is_shadow_root() && static_cast<ShadowRoot&>(parent_root).mode() == Bindings::ShadowRootMode::Closed)
                    slot_in_closed_tree = true;
            }

            // 2. If parent is a slottable and is assigned, then set slottable to parent.
            if (is<Node>(*parent) && is_an_assigned_slottable(static_cast<Node&>(*parent)))
                slottable = parent;

            // 3. Let relatedTarget be the result of retargeting event’s relatedTarget against parent.
            related_target = retarget(event.related_target(), parent);

            // 4. Let touchTargets be a new list.
            touch_targets.clear();

            // 5. For each touchTarget of event’s touch target list, append the result of retargeting touchTarget against parent to touchTargets.
            for (auto& touch_target : event.touch_target_list()) {
                touch_targets.append(retarget(touch_target, parent));
            }

            // 6. If parent is a Window object, or parent is a node and target’s root is a shadow-including inclusive ancestor of parent, then:
            if (is<HTML::Window>(parent)
                || (is<Node>(parent) && verify_cast<Node>(*target).root().is_shadow_including_inclusive_ancestor_of(verify_cast<Node>(*parent)))) {
                // 1. If isActivationEvent is true, event’s bubbles attribute is true, activationTarget is null, and parent has activation behavior, then set activationTarget to parent.
                if (is_activation_event && event.bubbles() && !activation_target && parent->has_activation_behavior())
                    activation_target = parent;

                // 2. Append to an event path with event, parent, null, relatedTarget, touchTargets, and slot-in-closed-tree.
                event.append_to_path(*parent, nullptr, related_target, touch_targets, slot_in_closed_tree);

            }
            // 7. Otherwise, if parent is relatedTarget, then set parent to null.
            else if (related_target.ptr() == parent) {
                parent = nullptr;
            }
            // 8. Otherwise, set target to parent and then:
            else {
                target = *parent;

                // 1. If isActivationEvent is true, activationTarget is null, and target has activation behavior, then set activationTarget to target.
                if (is_activation_event && !activation_target && target->has_activation_behavior())
                    activation_target = target;

                // 2. Append to an event path with event, parent, target, relatedTarget, touchTargets, and slot-in-closed-tree.
                event.append_to_path(*parent, target, related_target, touch_targets, slot_in_closed_tree);
            }

            // 9. If parent is non-null, then set parent to the result of invoking parent’s get the parent with event.
            if (parent) {
                parent = parent->get_parent(event);
            }

            // 10. Set slot-in-closed-tree to false.
            slot_in_closed_tree = false;
        }

        // 10. Let clearTargetsStruct be the last struct in event’s path whose shadow-adjusted target is non-null.
        auto clear_targets_struct = event.path().last_matching([](auto& entry) {
            return entry.shadow_adjusted_target;
        });

        VERIFY(clear_targets_struct.has_value());

        // 11. Let clearTargets be true if clearTargetsStruct’s shadow-adjusted target, clearTargetsStruct’s relatedTarget,
        //     or an EventTarget object in clearTargetsStruct’s touch target list is a node and its root is a shadow root; otherwise false.
        if (is<Node>(clear_targets_struct.value().shadow_adjusted_target.ptr())) {
            auto& shadow_adjusted_target_node = verify_cast<Node>(*clear_targets_struct.value().shadow_adjusted_target);
            if (is<ShadowRoot>(shadow_adjusted_target_node.root()))
                clear_targets = true;
        }

        if (!clear_targets && is<Node>(clear_targets_struct.value().related_target.ptr())) {
            auto& related_target_node = verify_cast<Node>(*clear_targets_struct.value().related_target);
            if (is<ShadowRoot>(related_target_node.root()))
                clear_targets = true;
        }

        if (!clear_targets) {
            for (auto touch_target : clear_targets_struct.value().touch_target_list) {
                if (is<Node>(*touch_target.ptr())) {
                    auto& touch_target_node = verify_cast<Node>(*touch_target.ptr());
                    if (is<ShadowRoot>(touch_target_node.root())) {
                        clear_targets = true;
                        break;
                    }
                }
            }
        }

        // 12. If activationTarget is non-null and activationTarget has legacy-pre-activation behavior, then run activationTarget’s legacy-pre-activation behavior.
        if (activation_target)
            activation_target->legacy_pre_activation_behavior();

        // 13. For each struct in event’s path, in reverse order:
        for (auto& entry : event.path().in_reverse()) {
            // 1. If struct’s shadow-adjusted target is non-null, then set event’s eventPhase attribute to AT_TARGET.
            if (entry.shadow_adjusted_target)
                event.set_phase(Event::Phase::AtTarget);
            // 2. Otherwise, set event’s eventPhase attribute to CAPTURING_PHASE.
            else
                event.set_phase(Event::Phase::CapturingPhase);

            // 3. Invoke with struct, event, "capturing", and legacyOutputDidListenersThrowFlag if given.
            invoke(entry, event, Event::Phase::CapturingPhase);
        }

        // 14. For each struct in event’s path:
        for (auto& entry : event.path()) {
            // 1. If struct’s shadow-adjusted target is non-null, then set event’s eventPhase attribute to AT_TARGET.
            if (entry.shadow_adjusted_target) {
                event.set_phase(Event::Phase::AtTarget);
            }
            // 2. Otherwise:
            else {
                // 1. If event’s bubbles attribute is false, then continue.
                if (!event.bubbles())
                    continue;

                // 2. Set event’s eventPhase attribute to BUBBLING_PHASE.
                event.set_phase(Event::Phase::BubblingPhase);
            }

            // 3. Invoke with struct, event, "bubbling", and legacyOutputDidListenersThrowFlag if given.
            invoke(entry, event, Event::Phase::BubblingPhase);
        }
    }

    // 6. Set event’s eventPhase attribute to NONE.
    event.set_phase(Event::Phase::None);

    // 7. Set event’s currentTarget attribute to null.
    event.set_current_target(nullptr);

    // 8. Set event’s path to the empty list.
    event.clear_path();

    // 9. Unset event’s dispatch flag, stop propagation flag, and stop immediate propagation flag.
    event.set_dispatched(false);
    event.set_stop_propagation(false);
    event.set_stop_immediate_propagation(false);

    // 10. If clearTargets, then:
    if (clear_targets) {
        // 1. Set event’s target to null.
        event.set_target(nullptr);

        // 2. Set event’s relatedTarget to null.
        event.set_related_target(nullptr);

        // 3. Set event’s touch target list to the empty list.
        event.clear_touch_target_list();
    }

    // 11. If activationTarget is non-null, then:
    if (activation_target) {
        // 1. If event’s canceled flag is unset, then run activationTarget’s activation behavior with event.
        if (!event.cancelled()) {
            activation_target->activation_behavior(event);
            activation_target->legacy_cancelled_activation_behavior_was_not_called();
        }
        // 2. Otherwise, if activationTarget has legacy-canceled-activation behavior, then run activationTarget’s legacy-canceled-activation behavior.
        else {
            activation_target->legacy_cancelled_activation_behavior();
        }
    }

    // 12. Return false if event’s canceled flag is set; otherwise true.
    return !event.cancelled();
}

}
