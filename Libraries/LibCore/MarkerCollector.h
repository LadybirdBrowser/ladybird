/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Implementation header for the marker collector. Only include this if
// you need to read or manage the collector (e.g., Internals, exporter).
// The emit-side API lives in <LibCore/Markers.h>.

#include <AK/Vector.h>
#include <LibCore/Export.h>
#include <LibCore/Markers.h>
#include <LibThreading/Mutex.h>

namespace Core {

struct Marker {
    MarkerString name;
    StringView type;
    MonotonicTime start;
    MonotonicTime end;
    MarkerPhase phase;
    MarkerCategory category;
    u64 tid { 0 };
    Vector<MarkerField, 4> fields;
};

class CORE_API MarkerCollector {
public:
    MarkerCollector();
    ~MarkerCollector();

    void add_marker(Marker);

    Vector<Marker> take_markers_snapshot() const;

    MonotonicTime creation_time() const { return m_creation_time; }

    void clear();

private:
    mutable Threading::Mutex m_markers_mutex;
    Vector<Marker> m_markers;
    MonotonicTime m_creation_time { MonotonicTime::now() };
};

extern CORE_API MarkerCollector* g_marker_collector;

}
