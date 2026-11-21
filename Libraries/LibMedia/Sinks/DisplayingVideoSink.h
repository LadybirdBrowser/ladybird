/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/ThreadSafeWeakable.h>
#include <AK/Time.h>
#include <LibGfx/Size.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/Sinks/VideoSinkTicking.h>

namespace Media {

struct DisplayingVideoSinkUpdateResult {
    bool new_frame_available { false };
    bool may_require_updates { false };
};

struct DisplayingVideoSinkFrameCounts {
    u64 presented { 0 };
    u64 dropped { 0 };

    bool is_zero() const { return presented == 0 && dropped == 0; }
};

class MEDIA_API DisplayingVideoSink final : public VideoSink
    , public ThreadSafeWeakable<DisplayingVideoSink> {
public:
    static ErrorOr<NonnullRefPtr<DisplayingVideoSink>> try_create(MediaTimeReader);

    explicit DisplayingVideoSink(MediaTimeReader);
    virtual ~DisplayingVideoSink() override;

    virtual void set_time_reader(MediaTimeReader) override;
    virtual void set_state_change_handler(PipelineStateChangeHandler) override;
    virtual void set_resize_handler(PipelineResizeHandler) override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<VideoProducer> const&) override;

    virtual void seek(AK::Duration timestamp, InvalidateHeldData = InvalidateHeldData::No) override;
    virtual void notify_clock_resumed() override;
    virtual void set_ticking(bool) override;
    bool is_ticking() const { return m_ticking == VideoSinkTicking::Yes; }
    bool ticking_is_decided() const { return m_ticking != VideoSinkTicking::Undecided; }
    // Called when the sink starts or stops ticking, catching up, or having an up-to-date frame.
    void set_on_freshness_changed(Function<void()> handler) { m_on_freshness_changed = move(handler); }
    // Seeks the input to the clock's time if the current frame doesn't show it, whether or not the sink is ticking.
    void catch_up_to_clock();
    // A catch-up that waits for data keeps the last frame shown until the data arrives, so it is not waited for.
    bool is_catching_up() const { return m_seek_status == SeekStatus::CatchingUp && m_latest_status != PipelineStatus::Blocked; }
    // Whether the frame may change if it is ticked.
    bool frame_is_up_to_date() const { return is_ticking() && m_has_updated_since_ticking_began && !is_catching_up(); }

    DisplayingVideoSinkUpdateResult update(MonotonicTime now);
    RefPtr<VideoFrame> current_frame() const;
    DisplayingVideoSinkFrameCounts take_frame_counts() { return exchange(m_frame_counts, {}); }

    void set_on_present_needed(Function<void()> handler) { m_on_present_needed = move(handler); }

private:
    void dispatch_latest_state_if_changed();
    void defer_latest_status_dispatch();
    void report_freshness_if_changed();
    void start_input();

    MediaTimeReader m_time_reader;
    RefPtr<VideoProducer> m_input;

    VideoSinkTicking m_ticking { VideoSinkTicking::Undecided };
    bool m_has_updated_since_ticking_began { false };
    RefPtr<VideoFrame> m_next_frame;
    RefPtr<VideoFrame> m_current_frame;
    bool m_cached_frames_are_discontinuous { false };
    bool m_seek_resolution_is_pending_frame_at_target { false };
    DisplayingVideoSinkFrameCounts m_frame_counts;

    enum class SeekStatus : u8 {
        None,
        // A seek that the sink made to catch its frames up to the clock. A seek from upstream replaces it.
        CatchingUp,
        InProgress,
    };
    SeekStatus m_seek_status { SeekStatus::None };
    void begin_seek(AK::Duration timestamp, InvalidateHeldData, SeekStatus);

    PipelineStateChangeHandler m_on_state_changed;
    PipelineStatus m_latest_status { PipelineStatus::Pending };
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
    PipelineCheckpoint m_input_checkpoint { 0 };
    PipelineCheckpoint m_last_dispatched_checkpoint { 0 };
    u32 m_seek_id { 0 };

    PipelineResizeHandler m_on_resize;
    Gfx::Size<u32> m_last_dispatched_size;

    Function<void()> m_on_present_needed;
    Function<void()> m_on_freshness_changed;
    struct Freshness {
        bool is_ticking { false };
        bool is_catching_up { false };
        bool frame_is_up_to_date { false };

        bool operator==(Freshness const&) const = default;
    };
    Freshness freshness() const;
    Freshness m_reported_freshness;
    bool m_last_update_may_require_updates { false };
};

}
