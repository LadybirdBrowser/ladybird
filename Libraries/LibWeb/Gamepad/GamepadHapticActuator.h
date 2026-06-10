/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibWeb/Bindings/GamepadHapticActuator.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::Gamepad {

using GamepadHapticEffectType = Bindings::GamepadHapticEffectType;
using GamepadHapticsResult = Bindings::GamepadHapticsResult;
using GamepadHapticsCompletionSteps = GC::Function<void(GamepadHapticsResult)>;
using GamepadEffectParameters = Bindings::GamepadEffectParameters;

class GamepadHapticActuator final : public Bindings::Wrappable {
    WEB_WRAPPABLE(GamepadHapticActuator, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(GamepadHapticActuator);

public:
    static GC::Ref<GamepadHapticActuator> create(HTML::Window&, GC::Ref<Gamepad>);

    virtual ~GamepadHapticActuator() override;

    Vector<GamepadHapticEffectType> effects() const;

    bool is_valid_effect(GamepadHapticEffectType, GamepadEffectParameters const&) const;
    bool can_play_effect_with_type(GamepadHapticEffectType) const;

    void play_effect(JS::Realm&, GamepadHapticEffectType, GamepadEffectParameters const&, GC::Ref<WebIDL::Promise>);
    void reset(JS::Realm&, GC::Ref<WebIDL::Promise>);

    void play_effect(GamepadHapticEffectType, GamepadEffectParameters const&, GC::Ref<GamepadHapticsCompletionSteps>);
    void reset(GC::Ref<GamepadHapticsCompletionSteps>);

private:
    GamepadHapticActuator(HTML::Window&, GC::Ref<Gamepad>, GC::Ref<DOM::DocumentObserver>);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    void document_became_hidden();
    void issue_haptic_effect(GamepadHapticEffectType, GamepadEffectParameters const&, GC::Ref<GC::Function<void()>> on_complete);
    bool stop_haptic_effects();
    void clear_playing_effect_timers();

    GC::Ref<Gamepad> m_gamepad;
    GC::Ref<HTML::Window> m_window;
    GC::Ref<DOM::DocumentObserver> m_document_became_hidden_observer;

    // https://w3c.github.io/gamepad/#dfn-effects
    // Represents the effects supported by the actuator.
    Vector<GamepadHapticEffectType> m_effects;

    // https://w3c.github.io/gamepad/#dfn-playingeffectpromise
    // The completion steps for playing some effect, or null if no effect is playing.
    GC::Ptr<GamepadHapticsCompletionSteps> m_playing_effect_completion_steps;
    GC::Ptr<Platform::Timer> m_playing_effect_timer;
};

}
