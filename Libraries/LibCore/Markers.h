/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Optional.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/Export.h>
#include <LibCore/MarkerCategory.h>

namespace Core {

using MarkerString = Variant<StringView, String>;
using MarkerFieldValue = Variant<StringView, String, double, i64, size_t, bool>;

struct MarkerField {
    StringView key;
    MarkerFieldValue value;
};

enum class MarkerPhase : u8 {
    Instant = 0,
    Interval = 1,
    IntervalStart = 2,
    IntervalEnd = 3,
};

CORE_API extern Atomic<bool, AK::MemoryOrder::memory_order_relaxed> g_marker_collector_active;

ALWAYS_INLINE MonotonicTime marker_now() { return MonotonicTime::now(); }

CORE_API void marker_add(MarkerPhase phase, MarkerString name, StringView type, MarkerCategory category,
    Optional<MonotonicTime> start, Vector<MarkerField, 4> fields);

CORE_API void marker_scope_push_label(StringView name, MarkerCategory category);
CORE_API void marker_scope_pop_label();

class CORE_API MarkerScope {
public:
    MarkerScope(StringView name, StringView schema_type, MarkerCategory category)
    {
        if (!g_marker_collector_active) [[likely]]
            return;
        m_name = name;
        m_schema_type = schema_type;
        m_category = category;
        m_start = marker_now();
        m_active = true;
        marker_scope_push_label(name, category);
    }

    template<typename FieldsCallable>
    MarkerScope(StringView name, StringView schema_type, MarkerCategory category, FieldsCallable&& fields_callable)
    {
        if (!g_marker_collector_active) [[likely]]
            return;
        m_name = name;
        m_schema_type = schema_type;
        m_category = category;
        m_start = marker_now();
        m_fields = fields_callable();
        m_active = true;
        marker_scope_push_label(name, category);
    }

    ~MarkerScope();

    MarkerScope(MarkerScope const&) = delete;
    MarkerScope& operator=(MarkerScope const&) = delete;
    MarkerScope(MarkerScope&&) = delete;
    MarkerScope& operator=(MarkerScope&&) = delete;

private:
    StringView m_name;
    StringView m_schema_type;
    MarkerCategory m_category { MarkerCategory::Other };
    Optional<MonotonicTime> m_start;
    Vector<MarkerField, 4> m_fields;
    bool m_active { false };
};

}

#define MARKER_START_TIME(VAR) \
    auto VAR = ::Core::g_marker_collector_active ? ::AK::Optional<::AK::MonotonicTime> { ::Core::marker_now() } : ::AK::Optional<::AK::MonotonicTime> { }

#define MARKER_INSTANT(NAME, TYPE, CATEGORY, ...)                                                    \
    do {                                                                                             \
        if (::Core::g_marker_collector_active) [[unlikely]]                                          \
            ::Core::marker_add(::Core::MarkerPhase::Instant, NAME, TYPE, CATEGORY, {}, __VA_ARGS__); \
    } while (0)

#define MARKER_INTERVAL(NAME, TYPE, CATEGORY, START, ...)                                                   \
    do {                                                                                                    \
        if (::Core::g_marker_collector_active && (START).has_value()) [[unlikely]]                          \
            ::Core::marker_add(::Core::MarkerPhase::Interval, NAME, TYPE, CATEGORY, *(START), __VA_ARGS__); \
    } while (0)

#define MARKER_INTERVAL_START(NAME, TYPE, CATEGORY, ...)                                                   \
    do {                                                                                                   \
        if (::Core::g_marker_collector_active) [[unlikely]]                                                \
            ::Core::marker_add(::Core::MarkerPhase::IntervalStart, NAME, TYPE, CATEGORY, {}, __VA_ARGS__); \
    } while (0)

#define MARKER_INTERVAL_END(NAME, TYPE, CATEGORY, ...)                                                   \
    do {                                                                                                 \
        if (::Core::g_marker_collector_active) [[unlikely]]                                              \
            ::Core::marker_add(::Core::MarkerPhase::IntervalEnd, NAME, TYPE, CATEGORY, {}, __VA_ARGS__); \
    } while (0)

#define MARKER_SCOPE_IMPL2(NAME, TYPE, CATEGORY, COUNTER) \
    ::Core::MarkerScope _marker_scope_##COUNTER { NAME, TYPE, CATEGORY }
#define MARKER_SCOPE_IMPL1(NAME, TYPE, CATEGORY, COUNTER) MARKER_SCOPE_IMPL2(NAME, TYPE, CATEGORY, COUNTER)
#define MARKER_SCOPE(NAME, TYPE, CATEGORY) MARKER_SCOPE_IMPL1(NAME, TYPE, CATEGORY, __COUNTER__)

#define MARKER_SCOPE_FIELDS_IMPL2(NAME, TYPE, CATEGORY, COUNTER, ...) \
    ::Core::MarkerScope _marker_scope_##COUNTER                       \
    {                                                                 \
        NAME, TYPE, CATEGORY,                                         \
            [&]() -> ::AK::Vector<::Core::MarkerField, 4> {           \
                return __VA_ARGS__;                                   \
            }                                                         \
    }
#define MARKER_SCOPE_FIELDS_IMPL1(NAME, TYPE, CATEGORY, COUNTER, ...) MARKER_SCOPE_FIELDS_IMPL2(NAME, TYPE, CATEGORY, COUNTER, __VA_ARGS__)
#define MARKER_SCOPE_FIELDS(NAME, TYPE, CATEGORY, ...) MARKER_SCOPE_FIELDS_IMPL1(NAME, TYPE, CATEGORY, __COUNTER__, __VA_ARGS__)
