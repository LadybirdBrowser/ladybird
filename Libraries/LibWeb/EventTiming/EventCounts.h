/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::EventTiming {

// https://w3c.github.io/event-timing/#sec-event-counts
class WEB_API EventCounts final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(EventCounts, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(EventCounts);

public:
    static GC::Ref<EventCounts> create();

    virtual ~EventCounts() override;

    OrderedHashMap<Utf16FlyString, u64> const& entries() const { return m_entries; }

    void increment(Utf16FlyString const& type);

private:
    EventCounts();

    OrderedHashMap<Utf16FlyString, u64> m_entries;
};

}
