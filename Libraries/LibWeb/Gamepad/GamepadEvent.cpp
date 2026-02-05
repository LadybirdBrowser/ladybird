/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GamepadEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/GamepadEvent.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(GamepadEvent);

WebIDL::ExceptionOr<GC::Ref<GamepadEvent>> GamepadEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, GamepadEventInit const& gamepad_event_init)
{
    return realm.create<GamepadEvent>(realm, event_name, gamepad_event_init);
}

GamepadEvent::GamepadEvent(JS::Realm& realm, FlyString const& event_name, GamepadEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_gamepad(event_init.gamepad.has_value() ? event_init.gamepad->ptr() : nullptr)
{
}

GamepadEvent::~GamepadEvent() = default;

void GamepadEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GamepadEvent);
    Base::initialize(realm);
}

void GamepadEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_gamepad);
}

}
