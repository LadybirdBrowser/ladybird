/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Debug.h>
#include <AK/Time.h>
#include <LibCore/Environment.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/Types.h>
#include <pthread.h>

namespace Web::WebAudio {

enum class WebAudioThreadRole : u8 {
    Unset,
    Control,
    Render,
    Offline,
};

WEB_API WebAudioThreadRole& current_thread_role();

ALWAYS_INLINE void register_control_thread_if_needed()
{
#ifndef NDEBUG
    static Threading::Mutex s_mutex;
    static Optional<pthread_t> s_control_thread_id;

    Threading::MutexLocker locker { s_mutex };
    if (!s_control_thread_id.has_value()) {
        s_control_thread_id = pthread_self();
    } else {
        ASSERT(pthread_equal(s_control_thread_id.value(), pthread_self()));
    }
#endif
}

ALWAYS_INLINE void mark_current_thread_as_control_thread()
{
    ASSERT(current_thread_role() != WebAudioThreadRole::Render);
    ASSERT(current_thread_role() != WebAudioThreadRole::Offline);
    register_control_thread_if_needed();
    current_thread_role() = WebAudioThreadRole::Control;
}

ALWAYS_INLINE void mark_current_thread_as_render_thread()
{
    ASSERT(current_thread_role() != WebAudioThreadRole::Control);
    ASSERT(current_thread_role() != WebAudioThreadRole::Offline);
    current_thread_role() = WebAudioThreadRole::Render;
}

ALWAYS_INLINE void mark_current_thread_as_offline_thread()
{
    ASSERT(current_thread_role() != WebAudioThreadRole::Render);
    current_thread_role() = WebAudioThreadRole::Offline;
}

ALWAYS_INLINE bool current_thread_is_control_thread()
{
    auto role = current_thread_role();
    return role == WebAudioThreadRole::Control || role == WebAudioThreadRole::Offline;
}

ALWAYS_INLINE bool current_thread_is_render_thread()
{
    auto role = current_thread_role();
    return role == WebAudioThreadRole::Render || role == WebAudioThreadRole::Offline;
}

ALWAYS_INLINE bool should_log_all()
{
    static bool enabled = Core::Environment::has("WEBAUDIO_LOG"sv);
    return enabled;
}

ALWAYS_INLINE bool should_log_info()
{
    if (should_log_all())
        return true;
    static bool enabled = Core::Environment::has("WEBAUDIO_INFO_LOG"sv);
    return enabled;
}

ALWAYS_INLINE bool should_log_media_element_bridge()
{
    if (should_log_all())
        return true;
    static bool enabled = Core::Environment::has("WEBAUDIO_MEDIA_LOG"sv);
    return enabled;
}

ALWAYS_INLINE bool should_log_script_processor_bridge()
{
    if (should_log_all())
        return true;
    static bool enabled = Core::Environment::has("WEBAUDIO_SCRIPT_PROCESSOR_LOG"sv);
    return enabled;
}

ALWAYS_INLINE bool should_log_output_driver()
{
    if (should_log_all())
        return true;
    static bool enabled = Core::Environment::has("WEBAUDIO_OUTPUT_LOG"sv);
    return enabled;
}

ALWAYS_INLINE bool should_log_nodes()
{
    if (should_log_all())
        return true;
    static bool enabled = Core::Environment::has("WEBAUDIO_NODE_LOG"sv);
    return enabled;
}

ALWAYS_INLINE bool should_log_zero_detector()
{
    if (should_log_all())
        return true;
    static bool enabled = Core::Environment::has("WEBAUDIO_ZERO_LOG"sv);
    return enabled;
}

ALWAYS_INLINE bool is_all_zeros(ReadonlySpan<f32> samples)
{
    for (auto const sample : samples) {
        if (sample != 0.0f)
            return false;
    }
    return true;
}

ALWAYS_INLINE bool is_all_zeros(Render::AudioBus const& bus)
{
    for (size_t ch = 0; ch < bus.channel_count(); ++ch) {
        if (!is_all_zeros(bus.channel(ch)))
            return false;
    }
    return true;
}

ALWAYS_INLINE bool log_if_all_zeros(StringView tag, ReadonlySpan<f32> samples, i64 min_interval_ms = 500)
{
    if (!should_log_zero_detector())
        return false;
    if (samples.is_empty())
        return false;
    if (!is_all_zeros(samples))
        return false;

    if (min_interval_ms <= 0) {
        dbgln("[WebAudio][ZERO] {} (n={})", tag, samples.size());
        return true;
    }

    static Atomic<i64> s_last_log_ms { 0 };
    i64 now_ms = AK::MonotonicTime::now().milliseconds();
    i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
    if ((now_ms - last_ms) < min_interval_ms)
        return true;
    if (!s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed))
        return true;

    dbgln("[WebAudio][ZERO] {} (n={})", tag, samples.size());
    return true;
}

ALWAYS_INLINE bool log_if_all_zeros(StringView tag, ReadonlySpan<f32> samples, Atomic<i64>& last_log_ms, i64 min_interval_ms = 500)
{
    if (!should_log_zero_detector())
        return false;
    if (samples.is_empty())
        return false;
    if (!is_all_zeros(samples))
        return false;

    if (min_interval_ms <= 0) {
        dbgln("[WebAudio][ZERO] {} (n={})", tag, samples.size());
        return true;
    }

    i64 now_ms = AK::MonotonicTime::now().milliseconds();
    i64 last_ms = last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
    if ((now_ms - last_ms) < min_interval_ms)
        return true;
    if (!last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed))
        return true;

    dbgln("[WebAudio][ZERO] {} (n={})", tag, samples.size());
    return true;
}

ALWAYS_INLINE bool log_if_all_zeros(StringView tag, Render::AudioBus const& bus, i64 min_interval_ms = 500)
{
    if (!should_log_zero_detector())
        return false;
    if (!is_all_zeros(bus))
        return false;

    // Log through the span overload so throttling is shared.
    return log_if_all_zeros(tag, bus.channel(0), min_interval_ms);
}

ALWAYS_INLINE bool log_if_all_zeros(StringView tag, Render::AudioBus const& bus, Atomic<i64>& last_log_ms, i64 min_interval_ms = 500)
{
    if (!should_log_zero_detector())
        return false;
    if (!is_all_zeros(bus))
        return false;

    // Log through the span overload.
    return log_if_all_zeros(tag, bus.channel(0), last_log_ms, min_interval_ms);
}

}

#define ASSERT_CONTROL_THREAD() ASSERT(::Web::WebAudio::current_thread_is_control_thread())
#define ASSERT_RENDER_THREAD() ASSERT(::Web::WebAudio::current_thread_is_render_thread())
#define ASSERT_WEBAUDIO_THREAD() ASSERT(::Web::WebAudio::current_thread_is_control_thread() || ::Web::WebAudio::current_thread_is_render_thread())

#define WA_DBGLN(...)                           \
    do {                                        \
        if (::Web::WebAudio::should_log_info()) \
            dbgln(__VA_ARGS__);                 \
    } while (0)

#define WA_MEDIA_DBGLN(...)                                     \
    do {                                                        \
        if (::Web::WebAudio::should_log_media_element_bridge()) \
            dbgln(__VA_ARGS__);                                 \
    } while (0)

#define WA_SP_DBGLN(...)                                           \
    do {                                                           \
        if (::Web::WebAudio::should_log_script_processor_bridge()) \
            dbgln(__VA_ARGS__);                                    \
    } while (0)

#define WA_OUT_DBGLN(...)                                \
    do {                                                 \
        if (::Web::WebAudio::should_log_output_driver()) \
            dbgln(__VA_ARGS__);                          \
    } while (0)

#define WA_NODE_DBGLN(...)                       \
    do {                                         \
        if (::Web::WebAudio::should_log_nodes()) \
            dbgln(__VA_ARGS__);                  \
    } while (0)
