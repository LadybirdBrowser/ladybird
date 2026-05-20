/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CookieChangeEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebIDL/CachedAttribute.h>

namespace Web::CookieStore {

// https://cookiestore.spec.whatwg.org/#cookiechangeevent
class CookieChangeEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(CookieChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(CookieChangeEvent);

public:
    [[nodiscard]] static GC::Ref<CookieChangeEvent> create(JS::Realm&, FlyString const& event_name, Bindings::CookieChangeEventInit const& event_init);
    [[nodiscard]] static GC::Ref<CookieChangeEvent> construct_impl(JS::Realm&, FlyString const& event_name, Bindings::CookieChangeEventInit const& event_init);

    virtual ~CookieChangeEvent() override;

    Vector<Bindings::CookieListItem> changed() const { return m_changed; }
    Vector<Bindings::CookieListItem> deleted() const { return m_deleted; }

    DEFINE_CACHED_ATTRIBUTE(changed);
    DEFINE_CACHED_ATTRIBUTE(deleted);

private:
    CookieChangeEvent(JS::Realm&, FlyString const& event_name, Bindings::CookieChangeEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<Bindings::CookieListItem> m_changed;
    Vector<Bindings::CookieListItem> m_deleted;
};

}
