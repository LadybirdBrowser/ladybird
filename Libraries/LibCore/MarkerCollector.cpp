/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibCore/MarkerCollector.h>

#if defined(AK_OS_LINUX)
#    include <sys/syscall.h>
#    include <unistd.h>
#elif defined(AK_OS_MACOS)
#    include <pthread.h>
#elif defined(AK_OS_WINDOWS)
#    include <windows.h>
#endif

namespace Core {

Atomic<bool, AK::MemoryOrder::memory_order_relaxed> g_marker_collector_active { false };
MarkerCollector* g_marker_collector { nullptr };

MarkerCollector::MarkerCollector()
{
    VERIFY(g_marker_collector == nullptr);
    g_marker_collector = this;
    g_marker_collector_active = true;

    m_markers.ensure_capacity(4096);
}

MarkerCollector::~MarkerCollector()
{
    g_marker_collector_active = false;
    g_marker_collector = nullptr;
}

void MarkerCollector::add_marker(Marker marker)
{
    if (marker.tid == 0) {
#if defined(AK_OS_LINUX)
        marker.tid = static_cast<u64>(::syscall(SYS_gettid));
#elif defined(AK_OS_MACOS)
        uint64_t tid = 0;
        pthread_threadid_np(nullptr, &tid);
        marker.tid = tid;
#elif defined(AK_OS_WINDOWS)
        marker.tid = static_cast<u64>(::GetCurrentThreadId());
#endif
    }

    Threading::MutexLocker locker(m_markers_mutex);
    m_markers.append(move(marker));
}

Vector<Marker> MarkerCollector::take_markers_snapshot() const
{
    Threading::MutexLocker locker(m_markers_mutex);
    return m_markers;
}

void MarkerCollector::clear()
{
    Threading::MutexLocker locker(m_markers_mutex);
    m_markers.clear_with_capacity();
}

void marker_add(MarkerPhase phase, MarkerString name, StringView type, MarkerCategory category,
    Optional<MonotonicTime> start, Vector<MarkerField, 4> fields)
{
    auto* c = g_marker_collector;
    if (!c)
        return;
    auto now = MonotonicTime::now();
    auto marker_start = start.value_or(now);
    auto marker_end = now;
    c->add_marker({ move(name), type, marker_start, marker_end, phase, category, 0, move(fields) });
}

void marker_scope_push_label(StringView, MarkerCategory) { }
void marker_scope_pop_label() { }

MarkerScope::~MarkerScope()
{
    if (!m_active)
        return;
    marker_scope_pop_label();
    if (g_marker_collector_active && m_start.has_value()) [[unlikely]]
        marker_add(MarkerPhase::Interval, m_name, m_schema_type, m_category, m_start, move(m_fields));
}

}
