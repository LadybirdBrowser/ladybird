/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/EventTiming/EventCounts.h>

namespace Web::Bindings {

// Maplike glue for EventCounts (see Meta/Generators/libweb_bindings/glue_headers.py).
inline GC::Ref<JS::Map> map_entries(JS::Realm& realm, EventTiming::EventCounts& event_counts)
{
    auto map_entries = JS::Map::create(realm);
    for (auto const& entry : event_counts.entries()) {
        auto key = JS::PrimitiveString::create(realm.vm(), entry.key);
        map_entries->map_set(JS::Value { key.ptr() }, JS::Value { static_cast<double>(entry.value) });
    }
    return map_entries;
}

inline Optional<JS::Value> map_get(JS::Realm&, EventTiming::EventCounts& event_counts, Utf16View key)
{
    auto it = event_counts.entries().find(key);
    if (it == event_counts.entries().end())
        return {};
    return JS::Value { static_cast<double>(it->value) };
}

inline bool map_has(EventTiming::EventCounts& event_counts, Utf16View key)
{
    return event_counts.entries().contains(key);
}

}
