/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/NavigationType.h>

namespace Web::HTML {

struct NavigationCurrentEntryChangeEventInit : public DOM::EventInit {
    Optional<Bindings::NavigationType> navigation_type = {};
    GC::Ptr<NavigationHistoryEntry> from;
};

class NavigationCurrentEntryChangeEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(NavigationCurrentEntryChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(NavigationCurrentEntryChangeEvent);

public:
    [[nodiscard]] static GC::Ref<NavigationCurrentEntryChangeEvent> construct_impl(JS::Realm&, FlyString const& event_name, NavigationCurrentEntryChangeEventInit const&);

    virtual ~NavigationCurrentEntryChangeEvent() override;

    Optional<Bindings::NavigationType> const& navigation_type() const { return m_navigation_type; }
    GC::Ref<NavigationHistoryEntry> from() const { return m_from; }

private:
    NavigationCurrentEntryChangeEvent(JS::Realm&, FlyString const& event_name, NavigationCurrentEntryChangeEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Optional<Bindings::NavigationType> m_navigation_type;
    GC::Ref<NavigationHistoryEntry> m_from;
};

}
