/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CookieChangeEvent.h>
#include <LibWeb/Bindings/CookieStore.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::CookieStore {

using CookieListItem = Bindings::CookieListItem;
using CookieChangeEventInit = Bindings::CookieChangeEventInit;

// https://cookiestore.spec.whatwg.org/#cookiechangeevent
class CookieChangeEvent final : public DOM::Event {
    WEB_WRAPPABLE(CookieChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(CookieChangeEvent);

public:
    [[nodiscard]] static GC::Ref<CookieChangeEvent> create(FlyString const& event_name, CookieChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~CookieChangeEvent() override;

    Vector<CookieListItem> changed() const { return m_changed; }
    Vector<CookieListItem> deleted() const { return m_deleted; }

private:
    CookieChangeEvent(FlyString const& event_name, CookieChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<CookieListItem> m_changed;
    Vector<CookieListItem> m_deleted;
};

}
