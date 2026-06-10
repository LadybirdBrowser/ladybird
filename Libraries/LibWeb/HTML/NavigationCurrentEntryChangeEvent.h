/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Bindings {

struct NavigationCurrentEntryChangeEventInit;

}

namespace Web::HTML {

class NavigationHistoryEntry;

struct NavigationCurrentEntryChangeEventInit : public DOM::EventInit {
    GC::Ref<NavigationHistoryEntry> from;
    Optional<NavigationType> navigation_type;
};

class NavigationCurrentEntryChangeEvent final : public DOM::Event {
    WEB_WRAPPABLE(NavigationCurrentEntryChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(NavigationCurrentEntryChangeEvent);

public:
    [[nodiscard]] static GC::Ref<NavigationCurrentEntryChangeEvent> create(JS::Realm&, FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const&);
    [[nodiscard]] static GC::Ref<NavigationCurrentEntryChangeEvent> create(FlyString const& event_name, NavigationCurrentEntryChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~NavigationCurrentEntryChangeEvent() override;

    Optional<NavigationType> const& navigation_type() const { return m_navigation_type; }
    GC::Ref<NavigationHistoryEntry> from() const { return m_from; }

private:
    NavigationCurrentEntryChangeEvent(FlyString const& event_name, NavigationCurrentEntryChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Optional<NavigationType> m_navigation_type;
    GC::Ref<NavigationHistoryEntry> m_from;
};

}
