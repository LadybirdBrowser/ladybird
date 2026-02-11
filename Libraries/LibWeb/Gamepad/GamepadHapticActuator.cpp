/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/GamepadHapticActuator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <SDL3/SDL_gamepad.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(GamepadHapticActuator);

// FIXME: What is a valid duration and startDelay? The spec doesn't define that.
//        Safari: clamps any duration above 5000ms to 5000ms and doesn't seem to clamp or reject any startDelay.
//        Chrome: rejects if duration + startDelay > 5000ms.
//        Firefox doesn't support vibration at the time of writing.
static constexpr u64 MAX_VIBRATION_DURATION = 5000;

// https://w3c.github.io/gamepad/#dfn-constructing-a-gamepadhapticactuator
GC::Ref<GamepadHapticActuator> GamepadHapticActuator::create(JS::Realm& realm, GC::Ref<Gamepad> gamepad)
{
    auto& window = as<HTML::Window>(realm.global_object());
    auto document_became_hidden_observer = realm.create<DOM::DocumentObserver>(realm, window.associated_document());

    // 1. Let gamepadHapticActuator be a newly created GamepadHapticActuator instance.
    auto gamepad_haptic_actuator = realm.create<GamepadHapticActuator>(realm, gamepad, document_became_hidden_observer);

    // 2. Let supportedEffectsList be an empty list.
    Vector<Bindings::GamepadHapticEffectType> supported_effects_list;

    // 3. For each enum value type of GamepadHapticEffectType, if the user agent can send a command to initiate effects
    //    of that type on that actuator, append type to supportedEffectsList.
    SDL_PropertiesID sdl_gamepad_properties = SDL_GetGamepadProperties(gamepad->sdl_gamepad());

    if (SDL_GetBooleanProperty(sdl_gamepad_properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, /* default_value= */ false))
        supported_effects_list.append(Bindings::GamepadHapticEffectType::DualRumble);

    if (SDL_GetBooleanProperty(sdl_gamepad_properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, /* default_value= */ false))
        supported_effects_list.append(Bindings::GamepadHapticEffectType::TriggerRumble);

    // 4. Set gamepadHapticActuator.[[effects]] to supportedEffectsList.
    gamepad_haptic_actuator->m_effects = move(supported_effects_list);

    return gamepad_haptic_actuator;
}

GamepadHapticActuator::GamepadHapticActuator(JS::Realm& realm, GC::Ref<Gamepad> gamepad, GC::Ref<DOM::DocumentObserver> document_became_hidden_observer)
    : Bindings::PlatformObject(realm)
    , m_gamepad(gamepad)
    , m_document_became_hidden_observer(document_became_hidden_observer)
{
    m_document_became_hidden_observer->set_document_visibility_state_observer([this](HTML::VisibilityState visibility_state) {
        if (visibility_state == HTML::VisibilityState::Hidden)
            document_became_hidden();
    });
}

GamepadHapticActuator::~GamepadHapticActuator() = default;

void GamepadHapticActuator::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GamepadHapticActuator);
    Base::initialize(realm);
}

void GamepadHapticActuator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_gamepad);
    visitor.visit(m_document_became_hidden_observer);
    visitor.visit(m_playing_effect_promise);
    visitor.visit(m_playing_effect_timer);
}

// https://w3c.github.io/gamepad/#dfn-valid-effect
static bool is_valid_effect(Bindings::GamepadHapticEffectType type, GamepadEffectParameters const& params)
{
    // 1. Given the value of GamepadHapticEffectType type, switch on:
    //    "dual-rumble"
    //          If params does not describe a valid dual-rumble effect, return false.
    //    "trigger-rumble"
    //          If params does not describe a valid trigger-rumble effect, return false.
    // 2. Return true
    switch (type) {
    case Bindings::GamepadHapticEffectType::DualRumble:
        // https://w3c.github.io/gamepad/#dfn-valid-dual-rumble-effect
        // Given GamepadEffectParameters params, a valid dual-rumble effect must have a valid duration, a valid
        // startDelay, and both the strongMagnitude and the weakMagnitude must be in the range [0 .. 1].
        if (Checked<u64>::addition_would_overflow(params.duration, params.start_delay))
            return false;

        if (params.duration + params.start_delay > MAX_VIBRATION_DURATION)
            return false;

        if (params.strong_magnitude < 0.0 || params.strong_magnitude > 1.0)
            return false;

        if (params.weak_magnitude < 0.0 || params.weak_magnitude > 1.0)
            return false;

        return true;
    case Bindings::GamepadHapticEffectType::TriggerRumble:
        // https://w3c.github.io/gamepad/#dfn-valid-trigger-rumble-effect
        // Given GamepadEffectParameters params, a valid trigger-rumble effect must have a valid duration, a valid
        // startDelay, and the strongMagnitude, weakMagnitude, leftTrigger, and rightTrigger must be in the range
        // [0 .. 1].
        if (Checked<u64>::addition_would_overflow(params.duration, params.start_delay))
            return false;

        if (params.duration + params.start_delay > MAX_VIBRATION_DURATION)
            return false;

        if (params.strong_magnitude < 0.0 || params.strong_magnitude > 1.0)
            return false;

        if (params.weak_magnitude < 0.0 || params.weak_magnitude > 1.0)
            return false;

        if (params.left_trigger < 0.0 || params.left_trigger > 1.0)
            return false;

        if (params.right_trigger < 0.0 || params.right_trigger > 1.0)
            return false;

        return true;
    }

    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/gamepad/#dom-gamepadhapticactuator-playeffect
GC::Ref<WebIDL::Promise> GamepadHapticActuator::play_effect(Bindings::GamepadHapticEffectType type, GamepadEffectParameters const& params)
{
    auto& realm = this->realm();

    // 1. If params does not describe a valid effect of type type, return a promise rejected with a TypeError.
    if (!is_valid_effect(type, params))
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid effect"_string });

    // 2. Let document be the current settings object's relevant global object's associated Document.
    auto& window = as<HTML::Window>(HTML::current_principal_settings_object().global_object());
    auto& document = window.associated_document();

    // 3. If document is null or document is not fully active or document's visibility state is "hidden", return a
    //    promise rejected with an "InvalidStateError" DOMException.
    if (!document.is_fully_active() || document.visibility_state_value() == HTML::VisibilityState::Hidden)
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::InvalidStateError::create(realm, "Haptics are not allowed in a hidden document"_utf16));

    // 4. If this.[[playingEffectPromise]] is not null:
    if (m_playing_effect_promise) {
        // 1. Let effectPromise be this.[[playingEffectPromise]].
        auto effect_promise = GC::Ref { *m_playing_effect_promise };

        // 2. Set this.[[playingEffectPromise]] to null.
        m_playing_effect_promise = nullptr;
        clear_playing_effect_timers();

        // 3. Queue a global task on the gamepad task source with the relevant global object of this to resolve
        //    effectPromise with "preempted".
        HTML::queue_global_task(HTML::Task::Source::Gamepad, HTML::relevant_global_object(*this), GC::create_function(realm.heap(), [effect_promise, &realm] {
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            auto preempted_string = JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(Bindings::GamepadHapticsResult::Preempted));
            WebIDL::resolve_promise(realm, effect_promise, preempted_string);
        }));
    }

    // 5. If this GamepadHapticActuator cannot play effects with type type, return a promise rejected with reason
    //    NotSupportedError.
    // https://w3c.github.io/gamepad/#ref-for-dfn-play-effects-with-type-1
    // A GamepadHapticActuator can play effects with type type if type can be found in the [[effects]] list.
    if (!m_effects.contains_slow(type))
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::NotSupportedError::create(realm, "Gamepad does not support this effect"_utf16));

    // 6. Let [[playingEffectPromise]] be a new promise.
    m_playing_effect_promise = WebIDL::create_promise(realm);

    // 7. Let playEffectTimestamp be the current high resolution time given the document's relevant global object.
    // NOTE: Unused.

    // 8. Do the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, type, params] {
        // 1. Issue a haptic effect to the actuator with type, params, and the playEffectTimestamp.
        issue_haptic_effect(type, params, GC::create_function(realm.heap(), [this, &realm] {
            // 2. When the effect completes, if this.[[playingEffectPromise]] is not null, queue a global task on the
            //    gamepad task source with the relevant global object of this to run the following steps:
            if (m_playing_effect_promise) {
                HTML::queue_global_task(HTML::Task::Source::Gamepad, HTML::relevant_global_object(*this), GC::create_function(realm.heap(), [this, &realm] {
                    // 1. If this.[[playingEffectPromise]] is null, abort these steps.
                    if (!m_playing_effect_promise)
                        return;

                    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    // 2. Resolve this.[[playingEffectPromise]] with "complete".
                    auto complete_string = JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(Bindings::GamepadHapticsResult::Complete));
                    WebIDL::resolve_promise(realm, *m_playing_effect_promise, complete_string);

                    // 3. Set this.[[playingEffectPromise]] to null.
                    m_playing_effect_promise = nullptr;
                    clear_playing_effect_timers();
                }));
            }
        }));
    }));

    // 9. Return [[playingEffectPromise]].
    VERIFY(m_playing_effect_promise);
    return *m_playing_effect_promise;
}

// https://w3c.github.io/gamepad/#dom-gamepadhapticactuator-reset
GC::Ref<WebIDL::Promise> GamepadHapticActuator::reset()
{
    auto& realm = this->realm();

    // 1. Let document be the current settings object's relevant global object's associated Document.
    auto& window = as<HTML::Window>(HTML::current_principal_settings_object().global_object());
    auto& document = window.associated_document();

    // 2. If document is null or document is not fully active or document's visibility state is "hidden", return a
    //    promise rejected with an "InvalidStateError" DOMException.
    if (!document.is_fully_active() || document.visibility_state_value() == HTML::VisibilityState::Hidden)
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::InvalidStateError::create(realm, "Haptics are not allowed in a hidden document"_utf16));

    // 3. Let resetResultPromise be a new promise.
    auto reset_result_promise = WebIDL::create_promise(realm);

    // 4. If this.[[playingEffectPromise]] is not null, do the following steps in parallel:
    if (m_playing_effect_promise) {
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, reset_result_promise] {
            // 1. Let effectPromise be this.[[playingEffectPromise]].
            auto effect_promise = m_playing_effect_promise;

            // 2. Stop haptic effects on this's gamepad's actuator.
            bool stopped_all = stop_haptic_effects();

            // 3. If the effect has been successfully stopped, do:
            if (stopped_all) {
                // 1. If effectPromise and this.[[playingEffectPromise]] are still the same,
                //    set this.[[playingEffectPromise]] to null.
                if (effect_promise == m_playing_effect_promise)
                    m_playing_effect_promise = nullptr;

                // 2. Queue a global task on the gamepad task source with the relevant global object of this to resolve
                //    effectPromise with "preempted".
                // AD-HOC: With doing this in parallel, there is a chance effect_promise is null. Don't try to resolve it
                //         if so.
                if (effect_promise) {
                    HTML::queue_global_task(HTML::Task::Source::Gamepad, HTML::relevant_global_object(*this), GC::create_function(realm.heap(), [&realm, effect_promise = GC::Ref { *effect_promise }] {
                        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                        auto preempted_string = JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(Bindings::GamepadHapticsResult::Preempted));
                        WebIDL::resolve_promise(realm, effect_promise, preempted_string);
                    }));
                }
            }

            // 4. Resolve resetResultPromise with "complete"
            auto complete_string = JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(Bindings::GamepadHapticsResult::Complete));
            WebIDL::resolve_promise(realm, reset_result_promise, complete_string);
        }));
    }

    // 5. Return resetResultPromise.
    return reset_result_promise;
}

// https://w3c.github.io/gamepad/#handling-visibility-change
void GamepadHapticActuator::document_became_hidden()
{
    auto& realm = this->realm();

    // When the document's visibility state becomes "hidden", run these steps for each GamepadHapticActuator actuator:
    // 1. If actuator.[[playingEffectPromise]] is null, abort these steps.
    if (!m_playing_effect_promise)
        return;

    // 2. Queue a global task on the gamepad task source with the relevant global object of actuator to run the
    //    following steps:
    auto& global = HTML::relevant_global_object(*this);
    HTML::queue_global_task(HTML::Task::Source::Gamepad, global, GC::create_function(global.heap(), [this, &realm] {
        // 1. If actuator.[[playingEffectPromise]] is null, abort these steps.
        if (!m_playing_effect_promise)
            return;

        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 2. Resolve actuator.[[playingEffectPromise]] with "preempted".
        auto preempted_string = JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(Bindings::GamepadHapticsResult::Preempted));
        WebIDL::resolve_promise(realm, *m_playing_effect_promise, preempted_string);

        // 3. Set actuator.[[playingEffectPromise]] to null.
        m_playing_effect_promise = nullptr;
        clear_playing_effect_timers();
    }));

    // 3. Stop haptic effects on actuator.
    stop_haptic_effects();
}

// https://w3c.github.io/gamepad/#dfn-issue-a-haptic-effect
void GamepadHapticActuator::issue_haptic_effect(Bindings::GamepadHapticEffectType type, GamepadEffectParameters const& params, GC::Ref<GC::Function<void()>> on_complete)
{
    auto& heap = this->heap();

    // To issue a haptic effect on an actuator, the user agent MUST send a command to the device to render an effect
    // of type and try to make it use the provided params. The user agent SHOULD use the provided playEffectTimestamp
    // for more precise playback timing when params.startDelay is not 0.0. The user agent MAY modify the effect to
    // increase compatibility. For example, an effect intended for a rumble motor may be transformed into a
    // waveform-based effect for a device that supports waveform haptics but lacks rumble motors.
    m_playing_effect_timer = Platform::Timer::create_single_shot(heap, static_cast<int>(params.start_delay), GC::create_function(heap, [this, type, params, on_complete, &heap] {
        // NOTE: We pass duration=0 (infinite) to SDL and handle the duration ourselves. This avoids a race condition
        //       where SDL's expiration check (in SDL_UpdateJoysticks) and our Platform::Timer resolve at slightly
        //       different times, potentially causing the stop signal to be missed before the promise resolves.
        switch (type) {
        case Bindings::GamepadHapticEffectType::DualRumble:
            SDL_RumbleGamepad(m_gamepad->sdl_gamepad(), params.strong_magnitude * NumericLimits<u16>::max(), params.weak_magnitude * NumericLimits<u16>::max(), 0);
            break;
        case Bindings::GamepadHapticEffectType::TriggerRumble:
            SDL_RumbleGamepadTriggers(m_gamepad->sdl_gamepad(), params.left_trigger * NumericLimits<u16>::max(), params.right_trigger * NumericLimits<u16>::max(), 0);
            break;
        }

        m_playing_effect_timer = Platform::Timer::create_single_shot(heap, params.duration, GC::create_function(heap, [this, type, on_complete] {
            // Explicitly stop the rumble before completing, ensuring the stop signal is sent synchronously.
            switch (type) {
            case Bindings::GamepadHapticEffectType::DualRumble:
                SDL_RumbleGamepad(m_gamepad->sdl_gamepad(), 0, 0, 0);
                break;
            case Bindings::GamepadHapticEffectType::TriggerRumble:
                SDL_RumbleGamepadTriggers(m_gamepad->sdl_gamepad(), 0, 0, 0);
                break;
            }
            on_complete->function()();
        }));

        m_playing_effect_timer->start();
    }));

    m_playing_effect_timer->start();
}

// https://w3c.github.io/gamepad/#dfn-stop-haptic-effects
bool GamepadHapticActuator::stop_haptic_effects()
{
    // To stop haptic effects on an actuator, the user agent MUST send a command to the device to abort any effects
    // currently being played. If a haptic effect was interrupted, the actuator SHOULD return to a motionless state
    // as quickly as possible.
    bool stopped_all = true;

    // https://wiki.libsdl.org/SDL3/SDL_RumbleGamepad
    // "Each call to this function cancels any previous rumble effect, and calling it with 0 intensity stops any
    // rumbling."
    if (m_effects.contains_slow(Bindings::GamepadHapticEffectType::DualRumble)) {
        bool success = SDL_RumbleGamepad(m_gamepad->sdl_gamepad(), 0, 0, 0);
        if (!success)
            stopped_all = false;
    }

    // https://wiki.libsdl.org/SDL3/SDL_RumbleGamepadTriggers
    // "Each call to this function cancels any previous trigger rumble effect, and calling it with 0 intensity stops
    // any rumbling."
    if (m_effects.contains_slow(Bindings::GamepadHapticEffectType::TriggerRumble)) {
        bool success = SDL_RumbleGamepadTriggers(m_gamepad->sdl_gamepad(), 0, 0, 0);
        if (!success)
            stopped_all = false;
    }

    return stopped_all;
}

void GamepadHapticActuator::clear_playing_effect_timers()
{
    if (m_playing_effect_timer) {
        m_playing_effect_timer->stop();
        m_playing_effect_timer = nullptr;
    }
}

}
