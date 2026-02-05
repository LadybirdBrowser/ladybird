/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GamepadHapticActuatorPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#dom-gamepadeffectparameters
struct GamepadEffectParameters {
    // https://w3c.github.io/gamepad/#dom-gamepadeffectparameters-duration
    // duration sets the duration of the vibration effect in milliseconds.
    u64 duration { 0 };

    // https://w3c.github.io/gamepad/#dom-gamepadeffectparameters-startdelay
    // startDelay sets the duration of the delay after playEffect() is called until vibration is started, in
    // milliseconds. During the delay interval, the actuator SHOULD NOT vibrate.
    u64 start_delay { 0 };

    // https://w3c.github.io/gamepad/#dom-gamepadeffectparameters-strongmagnitude
    // The vibration magnitude for the low frequency rumble in a "dual-rumble" or "trigger-rumble" effect.
    double strong_magnitude { 0.0 };

    // https://w3c.github.io/gamepad/#dom-gamepadeffectparameters-weakmagnitude
    // The vibration magnitude for the high frequency rumble in a "dual-rumble" or "trigger-rumble" effect.
    double weak_magnitude { 0.0 };

    // https://w3c.github.io/gamepad/#dom-gamepadeffectparameters-lefttrigger
    // The vibration magnitude for the bottom left front button (canonical index 6) rumble in a "trigger-rumble"
    // effect.
    double left_trigger { 0.0 };

    // https://w3c.github.io/gamepad/#dom-gamepadeffectparameters-righttrigger
    // The vibration magnitude for the bottom right front button (canonical index 7) rumble in a "trigger-rumble"
    // effect.
    double right_trigger { 0.0 };
};

class GamepadHapticActuator final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GamepadHapticActuator, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GamepadHapticActuator);

public:
    static GC::Ref<GamepadHapticActuator> create(JS::Realm&, GC::Ref<Gamepad>);

    virtual ~GamepadHapticActuator() override;

    Vector<Bindings::GamepadHapticEffectType> const& effects() const { return m_effects; }

    GC::Ref<WebIDL::Promise> play_effect(Bindings::GamepadHapticEffectType type, GamepadEffectParameters const& params);
    GC::Ref<WebIDL::Promise> reset();

private:
    GamepadHapticActuator(JS::Realm&, GC::Ref<Gamepad>, GC::Ref<DOM::DocumentObserver>);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void document_became_hidden();
    void issue_haptic_effect(Bindings::GamepadHapticEffectType type, GamepadEffectParameters const& params, GC::Ref<GC::Function<void()>> on_complete);
    bool stop_haptic_effects();
    void clear_playing_effect_timers();

    GC::Ref<Gamepad> m_gamepad;
    GC::Ref<DOM::DocumentObserver> m_document_became_hidden_observer;

    // https://w3c.github.io/gamepad/#dfn-effects
    // Represents the effects supported by the actuator.
    Vector<Bindings::GamepadHapticEffectType> m_effects;

    // https://w3c.github.io/gamepad/#dfn-playingeffectpromise
    // The Promise to play some effect, or null if no effect is playing.
    GC::Ptr<WebIDL::Promise> m_playing_effect_promise;
    GC::Ptr<Platform::Timer> m_playing_effect_timer;
};

}
