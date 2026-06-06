/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GamepadHapticActuator.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::Gamepad {

class GamepadHapticActuator final : public Bindings::Wrappable {
    WEB_WRAPPABLE(GamepadHapticActuator, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(GamepadHapticActuator);

public:
    static GC::Ref<GamepadHapticActuator> create(HTML::Window&, GC::Ref<Gamepad>);

    virtual ~GamepadHapticActuator() override;

    Vector<Bindings::GamepadHapticEffectType> const& effects() const { return m_effects; }

    GC::Ref<WebIDL::Promise> play_effect(JS::Realm&, Bindings::GamepadHapticEffectType, Bindings::GamepadEffectParameters const&);
    GC::Ref<WebIDL::Promise> reset(JS::Realm&);

private:
    GamepadHapticActuator(HTML::Window&, GC::Ref<Gamepad>, GC::Ref<DOM::DocumentObserver>);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    void document_became_hidden();
    void issue_haptic_effect(Bindings::GamepadHapticEffectType, Bindings::GamepadEffectParameters const&, GC::Ref<GC::Function<void()>> on_complete);
    bool stop_haptic_effects();
    void clear_playing_effect_timers();

    GC::Ref<Gamepad> m_gamepad;
    GC::Ref<HTML::Window> m_window;
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
