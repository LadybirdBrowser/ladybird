/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CloseEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

using CloseEventInit = Bindings::CloseEventInit;

class CloseEvent : public DOM::Event {
    WEB_WRAPPABLE(CloseEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(CloseEvent);

public:
    [[nodiscard]] static GC::Ref<CloseEvent> create(FlyString const& event_name, CloseEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~CloseEvent() override;

    bool was_clean() const { return m_was_clean; }
    u16 code() const { return m_code; }
    String reason() const { return m_reason; }

private:
    CloseEvent(FlyString const& event_name, CloseEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    bool m_was_clean { false };
    u16 m_code { 0 };
    String m_reason;
};

}
