/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GamepadEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::Gamepad {

class GamepadEvent final : public DOM::Event {
    WEB_WRAPPABLE(GamepadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(GamepadEvent);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<GamepadEvent>> construct_impl(JS::Realm&, FlyString const& event_name, Bindings::GamepadEventInit const& = {});

    virtual ~GamepadEvent() override;

    GC::Ptr<Gamepad> gamepad() const { return m_gamepad; }

private:
    GamepadEvent(JS::Realm&, FlyString const& event_name, Bindings::GamepadEventInit const& event_init);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<Gamepad> m_gamepad;
};

}
