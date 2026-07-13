/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

class BeforeUnloadEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(BeforeUnloadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(BeforeUnloadEvent);

public:
    [[nodiscard]] static GC::Ref<BeforeUnloadEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::EventInit const& = {});

    BeforeUnloadEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::EventInit const&);

    virtual ~BeforeUnloadEvent() override;

    Utf16String const& return_value() const { return m_return_value; }
    void set_return_value(Utf16String return_value) { m_return_value = move(return_value); }

private:
    virtual void initialize(JS::Realm&) override;

    Utf16String m_return_value;
};

}
