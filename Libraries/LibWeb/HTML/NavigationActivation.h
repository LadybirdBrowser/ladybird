/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/NavigationActivation.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

class NavigationActivation final : public Bindings::Wrappable {
    WEB_WRAPPABLE(NavigationActivation, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(NavigationActivation);

public:
    [[nodiscard]] static GC::Ref<NavigationActivation> create(GC::Ptr<NavigationHistoryEntry> from, GC::Ref<NavigationHistoryEntry> entry, Bindings::NavigationType);

    virtual ~NavigationActivation() override;

    GC::Ptr<NavigationHistoryEntry> from() const { return m_from; }
    GC::Ref<NavigationHistoryEntry> entry() const { return m_entry; }
    Bindings::NavigationType navigation_type() const { return m_navigation_type; }

private:
    NavigationActivation(GC::Ptr<NavigationHistoryEntry> from, GC::Ref<NavigationHistoryEntry> entry, Bindings::NavigationType);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<NavigationHistoryEntry> m_from;
    GC::Ref<NavigationHistoryEntry> m_entry;
    Bindings::NavigationType m_navigation_type;
};

}
