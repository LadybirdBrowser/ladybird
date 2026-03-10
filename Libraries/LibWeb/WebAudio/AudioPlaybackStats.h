/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWeb/Bindings/AudioPlaybackStatsPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::WebAudio {

class AudioContext;

class AudioPlaybackStats final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioPlaybackStats, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioPlaybackStats);

public:
    static GC::Ref<AudioPlaybackStats> create(JS::Realm&, AudioContext&);

    virtual ~AudioPlaybackStats() override = default;

    double underrun_duration() const;
    u32 underrun_events() const;
    double total_duration() const;
    double average_latency() const;
    double minimum_latency() const;
    double maximum_latency() const;

    void reset_latency();

private:
    AudioPlaybackStats(JS::Realm&, AudioContext&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void ensure_updated() const;
    void update_now();
    bool should_update_stats() const;

    static Optional<HTML::TaskID> current_task_id();
    double current_playback_latency() const;
    void update_latency_stats(double current_latency);

    GC::Ref<AudioContext> m_audio_context;

    mutable double m_underrun_duration { 0 };
    mutable u32 m_underrun_events { 0 };
    mutable double m_total_duration { 0 };
    mutable double m_average_latency { 0 };
    mutable double m_minimum_latency { 0 };
    mutable double m_maximum_latency { 0 };
    mutable double m_latency_reset_time { 0 };

    mutable double m_latency_sum { 0 };
    mutable u64 m_latency_sample_count { 0 };

    mutable u64 m_last_underrun_frames_total { 0 };

    mutable Optional<MonotonicTime> m_last_update_time;
    mutable Optional<HTML::TaskID> m_last_update_task_id;
};

}
