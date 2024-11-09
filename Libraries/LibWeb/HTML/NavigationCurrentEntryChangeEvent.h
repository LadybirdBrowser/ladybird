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
    JS::GCPtr<NavigationHistoryEntry> from;
};

class NavigationCurrentEntryChangeEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(NavigationCurrentEntryChangeEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(NavigationCurrentEntryChangeEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<NavigationCurrentEntryChangeEvent> construct_impl(JS::Realm&, FlyString const& event_name, NavigationCurrentEntryChangeEventInit const&);

    virtual ~NavigationCurrentEntryChangeEvent() override;

    Optional<Bindings::NavigationType> const& navigation_type() const { return m_navigation_type; }
    JS::NonnullGCPtr<NavigationHistoryEntry> from() const { return m_from; }

private:
    NavigationCurrentEntryChangeEvent(JS::Realm&, FlyString const& event_name, NavigationCurrentEntryChangeEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Optional<Bindings::NavigationType> m_navigation_type;
    JS::NonnullGCPtr<NavigationHistoryEntry> m_from;
};

}
