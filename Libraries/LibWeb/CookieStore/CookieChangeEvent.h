/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CookieStore/CookieStore.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebIDL/CachedAttribute.h>

namespace Web::CookieStore {

// https://cookiestore.spec.whatwg.org/#dictdef-cookiechangeeventinit
struct CookieChangeEventInit final : public DOM::EventInit {
    Optional<Vector<CookieListItem>> changed;
    Optional<Vector<CookieListItem>> deleted;
};

// https://cookiestore.spec.whatwg.org/#cookiechangeevent
class CookieChangeEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(CookieChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(CookieChangeEvent);

public:
    [[nodiscard]] static GC::Ref<CookieChangeEvent> create(JS::Realm&, FlyString const& event_name, CookieChangeEventInit const& event_init);
    [[nodiscard]] static GC::Ref<CookieChangeEvent> construct_impl(JS::Realm&, FlyString const& event_name, CookieChangeEventInit const& event_init);

    virtual ~CookieChangeEvent() override;

    Vector<CookieListItem> changed() const { return m_changed; }
    Vector<CookieListItem> deleted() const { return m_deleted; }

    DEFINE_CACHED_ATTRIBUTE(changed);
    DEFINE_CACHED_ATTRIBUTE(deleted);

private:
    CookieChangeEvent(JS::Realm&, FlyString const& event_name, CookieChangeEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<CookieListItem> m_changed;
    Vector<CookieListItem> m_deleted;
};

}
