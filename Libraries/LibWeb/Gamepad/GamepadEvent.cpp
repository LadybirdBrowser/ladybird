/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/GamepadEvent.h>

namespace Web::Gamepad {

GC_DEFINE_ALLOCATOR(GamepadEvent);

GC::Ref<GamepadEvent> GamepadEvent::create(FlyString const& event_name, GamepadEventInit const& gamepad_event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<GamepadEvent>(event_name, gamepad_event_init, time_stamp);
}

GamepadEvent::GamepadEvent(FlyString const& event_name, GamepadEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_gamepad(event_init.gamepad)
{
}

GamepadEvent::~GamepadEvent() = default;

void GamepadEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_gamepad);
}

}
