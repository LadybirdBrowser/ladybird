/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/CloseEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

class CloseEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(CloseEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(CloseEvent);

public:
    [[nodiscard]] static GC::Ref<CloseEvent> create(JS::Realm&, FlyString const& event_name, Bindings::CloseEventInit const& event_init = {});
    static WebIDL::ExceptionOr<GC::Ref<CloseEvent>> construct_impl(JS::Realm&, FlyString const& event_name, Bindings::CloseEventInit const& event_init);

    virtual ~CloseEvent() override;

    bool was_clean() const { return m_was_clean; }
    u16 code() const { return m_code; }
    String reason() const { return m_reason; }

private:
    CloseEvent(JS::Realm&, FlyString const& event_name, Bindings::CloseEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

    bool m_was_clean { false };
    u16 m_code { 0 };
    String m_reason;
};

}
