/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>

namespace Web::Gamepad {

struct GamepadEventInit : public DOM::EventInit {
    Optional<GC::Root<Gamepad>> gamepad;
};

class GamepadEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(GamepadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(GamepadEvent);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<GamepadEvent>> construct_impl(JS::Realm&, FlyString const& event_name, GamepadEventInit const& = {});

    virtual ~GamepadEvent() override;

    GC::Ptr<Gamepad> gamepad() const { return m_gamepad; }

private:
    GamepadEvent(JS::Realm&, FlyString const& event_name, GamepadEventInit const& event_init);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<Gamepad> m_gamepad;
};

}
