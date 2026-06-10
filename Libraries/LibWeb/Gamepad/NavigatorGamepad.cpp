/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Gamepad/EventNames.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/GamepadEvent.h>
#include <LibWeb/Gamepad/NavigatorGamepad.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

#include <SDL3/SDL_gamepad.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#dom-navigator-getgamepads
WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> NavigatorGamepadPartial::get_gamepads()
{
    // 1. Let doc be the current global object's associated Document.
    auto& window = HTML::current_window();
    auto& document = window.associated_document();

    // 2. If doc is null or doc is not fully active, then return an empty list.
    GC::RootVector<GC::Ptr<Gamepad>> gamepads;
    if (!document.is_fully_active())
        return gamepads;

    // 3. If doc is not allowed to use the "gamepad" permission, then throw a "SecurityError" DOMException and abort these steps.
    if (!document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Gamepad))
        return WebIDL::SecurityError::create("Not allowed to use gamepads"_utf16);

    // 4. If this.[[hasGamepadGesture]] is false, then return an empty list.
    if (!m_has_gamepad_gesture)
        return gamepads;

    // 5. Let now be the current high resolution time given the current global object.
    auto now = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window));

    // 6. Let gamepads be an empty list.
    // NOTE: Already done.

    // 7. For each gamepad of this.[[gamepads]]:
    for (auto gamepad : m_gamepads) {
        // 1. If gamepad is not null and gamepad.[[exposed]] is false:
        if (gamepad && !gamepad->exposed()) {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad->set_exposed({}, true);

            // 2. Set gamepad.[[timestamp]] to now.
            gamepad->set_timestamp({}, now);
        }

        // 2. Append gamepad to gamepads.
        gamepads.append(gamepad);
    }

    // 8. Return gamepads.
    return gamepads;
}

void NavigatorGamepadPartial::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_gamepads);
}

// https://w3c.github.io/gamepad/#dfn-selecting-an-unused-gamepad-index
size_t NavigatorGamepadPartial::select_an_unused_gamepad_index(Badge<Gamepad>)
{
    // 2. Let maxGamepadIndex be the size of navigator.[[gamepads]] − 1.
    // 3. For each gamepadIndex of the range from 0 to maxGamepadIndex:
    for (size_t gamepad_index = 0; gamepad_index < m_gamepads.size(); ++gamepad_index) {
        // 1. If navigator.[[gamepads]][gamepadIndex] is null, then return gamepadIndex.
        if (!m_gamepads[gamepad_index])
            return gamepad_index;
    }

    // 4. Append null to navigator.[[gamepads]].
    m_gamepads.append(nullptr);

    // 5. Return the size of navigator.[[gamepads]] − 1.
    return m_gamepads.size() - 1;
}

// https://w3c.github.io/gamepad/#event-gamepadconnected
void NavigatorGamepadPartial::handle_gamepad_connected(SDL_JoystickID sdl_joystick_id)
{
    // When a gamepad becomes available on the system, run the following steps:
    if (m_available_gamepads.contains_slow(sdl_joystick_id))
        return;

    // 1. Let document be the current global object's associated Document; otherwise null.
    // FIXME: We can't use the current global object here, since it's not executing in a scripting context.
    // NOTE: NavigatorGamepad is only available on Window.
    // NOTE: document is never null.
    auto& navigator = as<HTML::Navigator>(*this);
    auto& window = navigator.window();
    auto& realm = window.realm();
    auto& global = realm.global_object();
    auto& document = window.associated_document();

    // 2. If document is not null and is not allowed to use the "gamepad" permission, then abort these steps.
    if (!document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Gamepad))
        return;

    // AD-HOC: In test mode, ignore any non-virtual gamepads.
    //         All fake gamepads added by Internals are always virtual, and no other ones are.
    if (HTML::Window::in_test_mode() && !SDL_IsJoystickVirtual(sdl_joystick_id))
        return;

    m_available_gamepads.append(sdl_joystick_id);

    // 3. Queue a global task on the gamepad task source with the current global object to perform the following steps:
    HTML::queue_global_task(HTML::Task::Source::Gamepad, global, GC::create_function(GC::Heap::the(), [&window, &document, sdl_joystick_id] mutable {
        // 1. Let gamepad be a new Gamepad representing the gamepad.
        auto gamepad = Gamepad::create(window, sdl_joystick_id);

        // 2. Let navigator be gamepad's relevant global object's Navigator object.
        auto navigator = window.navigator();

        // 3. Set navigator.[[gamepads]][gamepad.index] to gamepad.
        navigator->m_gamepads[gamepad->index()] = gamepad;

        // 4. If navigator.[[hasGamepadGesture]] is true:
        if (navigator->m_has_gamepad_gesture) {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad->set_exposed({}, true);

            // 2. If document is not null and is fully active, then fire an event named gamepadconnected at gamepad's
            //    relevant global object using GamepadEvent with its gamepad attribute initialized to gamepad.
            if (document.is_fully_active()) {
                GamepadEventInit gamepad_connected_event_init { {}, gamepad };
                auto gamepad_connected_event = GamepadEvent::create(EventNames::gamepadconnected, gamepad_connected_event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
                window.dispatch_event(gamepad_connected_event);
            }
        }
    }));
}

// https://w3c.github.io/gamepad/#dfn-receives-new-button-or-axis-input-values
void NavigatorGamepadPartial::handle_gamepad_updated(Badge<EventHandler>, SDL_JoystickID sdl_joystick_id)
{
    // When the system receives new button or axis input values, run the following steps:
    // 1. Let gamepad be the Gamepad object representing the device that received new button or axis input values.
    auto gamepad = m_gamepads.find_if([&sdl_joystick_id](GC::Ptr<Gamepad> gamepad) {
        return gamepad && gamepad->sdl_joystick_id() == sdl_joystick_id;
    });

    if (gamepad.is_end())
        return;

    // 2. Queue a global task on the gamepad task source with gamepad's relevant global object to update gamepad state
    //    for gamepad.
    auto& global = (*gamepad)->window().realm().global_object();
    HTML::queue_global_task(HTML::Task::Source::Gamepad, global, GC::create_function(GC::Heap::the(), [gamepad = GC::Ref { **gamepad }] {
        gamepad->update_gamepad_state({});
    }));
}

void NavigatorGamepadPartial::handle_gamepad_disconnected(Badge<EventHandler>, SDL_JoystickID sdl_joystick_id)
{
    // When a gamepad becomes unavailable on the system, run the following steps:
    m_available_gamepads.remove_first_matching([&sdl_joystick_id](SDL_JoystickID available_gamepad) {
        return sdl_joystick_id == available_gamepad;
    });

    // 1. Let gamepad be the Gamepad representing the unavailable device.
    auto gamepad = m_gamepads.find_if([&sdl_joystick_id](GC::Ptr<Gamepad> gamepad) {
        return gamepad && gamepad->sdl_joystick_id() == sdl_joystick_id;
    });

    if (gamepad.is_end())
        return;

    // 2. Queue a global task on the gamepad task source with gamepad's relevant global object to perform the
    //    following steps:
    auto& window = (*gamepad)->window();
    auto& global = window.realm().global_object();
    HTML::queue_global_task(HTML::Task::Source::Gamepad, global, GC::create_function(GC::Heap::the(), [gamepad = GC::Ref { **gamepad }, &window] {
        // 1. Set gamepad.[[connected]] to false.
        gamepad->set_connected({}, false);

        // 2. Let document be gamepad's relevant global object's associated Document; otherwise null.
        auto& document = window.associated_document();

        // 3. If gamepad.[[exposed]] is true and document is not null and is fully active, then fire an event named
        //    gamepaddisconnected at gamepad's relevant global object using GamepadEvent with its gamepad attribute
        //    initialized to gamepad.
        if (gamepad->exposed() && document.is_fully_active()) {
            GamepadEventInit gamepad_disconnected_event_init { {}, gamepad };
            auto gamepad_disconnected_event = GamepadEvent::create(EventNames::gamepaddisconnected, gamepad_disconnected_event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
            window.dispatch_event(gamepad_disconnected_event);
        }

        // 4. Let navigator be gamepad's relevant global object's Navigator object.
        auto navigator = window.navigator();

        // 5. Set navigator.[[gamepads]][gamepad.index] to null.
        navigator->m_gamepads[gamepad->index()] = nullptr;

        // 6. While navigator.[[gamepads]] is not empty and the last item of navigator.[[gamepads]] is null, remove the
        //    last item of navigator.[[gamepads]].
        while (!navigator->m_gamepads.is_empty() && navigator->m_gamepads.last() == nullptr) {
            (void)navigator->m_gamepads.take_last();
        }
    }));
}

void NavigatorGamepadPartial::check_for_connected_gamepads()
{
    // "(SDL_JoystickID *) Returns a 0 terminated array of joystick instance IDs or NULL on failure; call
    // SDL_GetError() for more information. This should be freed with SDL_free() when it is no longer needed."
    int gamepad_count = 0;
    SDL_JoystickID* connected_gamepads = SDL_GetGamepads(&gamepad_count);
    if (!connected_gamepads)
        return;

    for (int gamepad_index = 0; gamepad_index < gamepad_count; ++gamepad_index) {
        handle_gamepad_connected(connected_gamepads[gamepad_index]);
    }

    SDL_free(connected_gamepads);
}

void NavigatorGamepadPartial::set_has_gamepad_gesture(Badge<Gamepad>, bool value)
{
    m_has_gamepad_gesture = value;
}

GC::RootVector<GC::Ptr<Gamepad>> NavigatorGamepadPartial::gamepads(Badge<Gamepad>) const
{
    return GC::RootVector<GC::Ptr<Gamepad>> { as<HTML::Navigator>(*this).m_gamepads };
}

}
