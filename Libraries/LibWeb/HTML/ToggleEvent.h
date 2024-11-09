/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

struct ToggleEventInit : public DOM::EventInit {
    String old_state;
    String new_state;
};

class ToggleEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(ToggleEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(ToggleEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ToggleEvent> create(JS::Realm&, FlyString const& event_name, ToggleEventInit = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<ToggleEvent>> construct_impl(JS::Realm&, FlyString const& event_name, ToggleEventInit);

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-oldstate
    String const& old_state() const { return m_old_state; }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-newstate
    String const& new_state() const { return m_new_state; }

private:
    ToggleEvent(JS::Realm&, FlyString const& event_name, ToggleEventInit event_init);

    virtual void initialize(JS::Realm&) override;

    String m_old_state;
    String m_new_state;
};

}
