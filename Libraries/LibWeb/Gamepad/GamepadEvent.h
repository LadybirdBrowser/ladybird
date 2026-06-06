/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/GamepadEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::Gamepad {

class GamepadEvent final : public DOM::Event {
    WEB_WRAPPABLE(GamepadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(GamepadEvent);

public:
    [[nodiscard]] static GC::Ref<GamepadEvent> create(FlyString const& event_name, Bindings::GamepadEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<GamepadEvent>> construct_impl(HTML::Window&, FlyString const& event_name, Bindings::GamepadEventInit const& = {});

    virtual ~GamepadEvent() override;

    GC::Ptr<Gamepad> gamepad() const { return m_gamepad; }

private:
    GamepadEvent(FlyString const& event_name, Bindings::GamepadEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<Gamepad> m_gamepad;
};

}
