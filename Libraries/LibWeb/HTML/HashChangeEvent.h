/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/HashChangeEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

class HashChangeEvent final : public DOM::Event {
    WEB_WRAPPABLE(HashChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(HashChangeEvent);

public:
    [[nodiscard]] static GC::Ref<HashChangeEvent> create(FlyString const& event_name, Bindings::HashChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<HashChangeEvent> construct_impl(Window&, FlyString const& event_name, Bindings::HashChangeEventInit const&);

    String old_url() const { return m_old_url; }
    String new_url() const { return m_new_url; }

private:
    HashChangeEvent(FlyString const& event_name, Bindings::HashChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

    String m_old_url;
    String m_new_url;
};

}
