/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::WebRTC {

// https://w3c.github.io/webrtc-pc/#rtcstatsreport-object
//
// Read-only maplike<DOMString, object>. Native entries live here; the bindings
// glue materializes a JS::Map on demand. The values are plain JS objects built
// by the stats gathering code (see RTCPeerConnection::get_stats()).
class RTCStatsReport final : public Bindings::Wrappable {
    WEB_WRAPPABLE(RTCStatsReport, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(RTCStatsReport);

public:
    [[nodiscard]] static GC::Ref<RTCStatsReport> create();

    virtual ~RTCStatsReport() override;

    OrderedHashMap<Utf16String, GC::Ref<JS::Object>> const& entries() const { return m_entries; }
    void set_entry(Utf16String key, GC::Ref<JS::Object> value) { m_entries.set(move(key), value); }

private:
    RTCStatsReport();

    virtual void visit_edges(Cell::Visitor&) override;

    OrderedHashMap<Utf16String, GC::Ref<JS::Object>> m_entries;
};

}
