/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

class BeforeUnloadEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(BeforeUnloadEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(BeforeUnloadEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<BeforeUnloadEvent> create(JS::Realm&, FlyString const& event_name, DOM::EventInit const& = {});

    BeforeUnloadEvent(JS::Realm&, FlyString const& event_name, DOM::EventInit const&);

    virtual ~BeforeUnloadEvent() override;

    String const& return_value() const { return m_return_value; }
    void set_return_value(String const& return_value) { m_return_value = return_value; }

private:
    virtual void initialize(JS::Realm&) override;

    String m_return_value;
};

}
