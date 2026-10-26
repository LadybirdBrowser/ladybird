/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/WebRTC/BindingsGlue.h>
#include <LibWeb/WebRTC/RTCStatsReport.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCStatsReport);

GC::Ref<RTCStatsReport> RTCStatsReport::create()
{
    return GC::Heap::the().allocate<RTCStatsReport>();
}

RTCStatsReport::RTCStatsReport() = default;

RTCStatsReport::~RTCStatsReport() = default;

void RTCStatsReport::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& entry : m_entries)
        visitor.visit(entry.value);
}

}

namespace Web::Bindings {

GC::Ref<JS::Map> map_entries(JS::Realm& realm, WebRTC::RTCStatsReport& report)
{
    auto map_entries = JS::Map::create(realm);
    for (auto const& entry : report.entries()) {
        auto key = JS::PrimitiveString::create(realm.vm(), entry.key);
        map_entries->map_set(JS::Value { key.ptr() }, JS::Value { entry.value.ptr() });
    }
    return map_entries;
}

Optional<JS::Value> map_get(JS::Realm&, WebRTC::RTCStatsReport& report, Utf16View key)
{
    auto it = report.entries().find(key);
    if (it == report.entries().end())
        return {};
    return JS::Value { it->value.ptr() };
}

bool map_has(WebRTC::RTCStatsReport& report, Utf16View key)
{
    return report.entries().contains(key);
}

}
