/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/AudioPlaybackStatsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/AudioPlaybackStats.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioPlaybackStats);

GC::Ref<AudioPlaybackStats> AudioPlaybackStats::create(JS::Realm& realm, AudioContext& context)
{
    return realm.create<AudioPlaybackStats>(realm, context);
}

AudioPlaybackStats::AudioPlaybackStats(JS::Realm& realm, AudioContext& context)
    : PlatformObject(realm)
    , m_audio_context(context)
{
}

void AudioPlaybackStats::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioPlaybackStats);
    Base::initialize(realm);
}

void AudioPlaybackStats::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_audio_context);
}

double AudioPlaybackStats::underrun_duration() const
{
    ensure_updated();
    return m_underrun_duration;
}

u32 AudioPlaybackStats::underrun_events() const
{
    ensure_updated();
    return m_underrun_events;
}

double AudioPlaybackStats::total_duration() const
{
    ensure_updated();
    return m_total_duration;
}

double AudioPlaybackStats::average_latency() const
{
    ensure_updated();
    return m_average_latency;
}

double AudioPlaybackStats::minimum_latency() const
{
    ensure_updated();
    return m_minimum_latency;
}

double AudioPlaybackStats::maximum_latency() const
{
    ensure_updated();
    return m_maximum_latency;
}

void AudioPlaybackStats::reset_latency()
{
    // 1. Set [[latency reset time]] to currentTime.
    m_latency_reset_time = m_audio_context->current_time();

    // 2. Let currentLatency be the playback latency of the last frame played by [[audio context]], or 0 if no frames have been played out yet.
    double current_latency = current_playback_latency();

    // 3. Set [[average latency]] to currentLatency.
    m_average_latency = current_latency;

    // 4. Set [[minimum latency]] to currentLatency.
    m_minimum_latency = current_latency;

    // 5. Set [[maximum latency]] to currentLatency.
    m_maximum_latency = current_latency;

    m_latency_sum = current_latency;
    m_latency_sample_count = 1;
}

Optional<HTML::TaskID> AudioPlaybackStats::current_task_id()
{
    HTML::Task const* task = HTML::main_thread_event_loop().currently_running_task();
    if (!task)
        return {};
    return task->id();
}

bool AudioPlaybackStats::should_update_stats() const
{
    // 1. If [[audio context]] is not running, abort these steps.
    if (!m_audio_context->is_running())
        return false;

    // 2. Let canUpdate be false.
    bool can_update = false;

    // 3. Let document be the current this's relevant global object's associated Document. If document is fully active and document's visibility state is visible, set canUpdate to true.
    auto& global_object = HTML::relevant_global_object(*m_audio_context);
    if (auto* window = as_if<HTML::Window>(global_object)) {
        auto& document = window->associated_document();
        if (document.is_fully_active() && document.visibility_state() == "visible"sv)
            can_update = true;
    }

    // 4. Let permission be the permission state for the permission associated with microphone access. If permission is granted, set canUpdate to true.
    // AD-HOC: Permissions are not yet implemented.

    // 5. If canUpdate is false, abort these steps.
    return can_update;
}

void AudioPlaybackStats::ensure_updated() const
{
    if (!should_update_stats())
        return;

    auto now = MonotonicTime::now_coarse();
    if (!m_last_update_time.has_value() || (now - m_last_update_time.value()) >= AK::Duration::from_seconds(1)) {
        auto task_id = current_task_id();
        if (m_last_update_time.has_value()) {
            if (!task_id.has_value())
                return;
            if (m_last_update_task_id.has_value() && task_id.value() == m_last_update_task_id.value())
                return;
        }

        if (!task_id.has_value())
            return;

        const_cast<AudioPlaybackStats*>(this)->update_now();
        m_last_update_time = now;
        m_last_update_task_id = task_id;
    }
}

void AudioPlaybackStats::update_now()
{
    m_audio_context->refresh_timing_page_for_stats();
    // 6. Set [[underrun duration]] to the total duration of all underrun events (in seconds) that have occurred in [[audio context]] playback since its construction.
    // 7. Set [[underrun events]] to the total number of underrun events that have occurred in [[audio context]] playback since its construction.
    u64 underrun_frames_total = m_audio_context->underrun_frames_total();
    float sample_rate = m_audio_context->sample_rate();
    double underrun_duration = 0.0;
    if (sample_rate > 0.0f)
        underrun_duration = static_cast<double>(underrun_frames_total) / static_cast<double>(sample_rate);

    if (underrun_frames_total > m_last_underrun_frames_total)
        ++m_underrun_events;

    m_last_underrun_frames_total = underrun_frames_total;
    m_underrun_duration = underrun_duration;

    // 8. Set [[total duration]] to [[underrun duration]] + [[audio context]].currentTime.
    m_total_duration = m_underrun_duration + m_audio_context->current_time();

    // 9. Set [[average latency]] to the average playback latency (in seconds) of [[audio context]] playback over the currently tracked interval.
    // 10. Set [[minimum latency]] to the minimum playback latency (in seconds) of [[audio context]] playback over the currently tracked interval.
    // 11. Set [[maximum latency]] to the maximum playback latency (in seconds) of [[audio context]] playback over the currently tracked interval.
    double current_latency = current_playback_latency();
    update_latency_stats(current_latency);
}

double AudioPlaybackStats::current_playback_latency() const
{
    if (m_audio_context->current_frame() == 0)
        return 0.0;
    return m_audio_context->output_latency();
}

void AudioPlaybackStats::update_latency_stats(double current_latency)
{
    if (m_latency_sample_count == 0) {
        m_minimum_latency = current_latency;
        m_maximum_latency = current_latency;
        m_average_latency = current_latency;
        m_latency_sum = current_latency;
        m_latency_sample_count = 1;
        return;
    }

    m_minimum_latency = min(m_minimum_latency, current_latency);
    m_maximum_latency = max(m_maximum_latency, current_latency);
    m_latency_sum += current_latency;
    ++m_latency_sample_count;
    m_average_latency = m_latency_sum / static_cast<double>(m_latency_sample_count);
}

}
