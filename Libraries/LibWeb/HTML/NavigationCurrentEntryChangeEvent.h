/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/NavigationCurrentEntryChangeEvent.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

class NavigationCurrentEntryChangeEvent final : public DOM::Event {
    WEB_WRAPPABLE(NavigationCurrentEntryChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(NavigationCurrentEntryChangeEvent);

public:
    [[nodiscard]] static GC::Ref<NavigationCurrentEntryChangeEvent> create(FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<NavigationCurrentEntryChangeEvent> construct_impl(Window&, FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const&);

    virtual ~NavigationCurrentEntryChangeEvent() override;

    Optional<Bindings::NavigationType> const& navigation_type() const { return m_navigation_type; }
    GC::Ref<NavigationHistoryEntry> from() const { return m_from; }

private:
    NavigationCurrentEntryChangeEvent(FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Optional<Bindings::NavigationType> m_navigation_type;
    GC::Ref<NavigationHistoryEntry> m_from;
};

}
