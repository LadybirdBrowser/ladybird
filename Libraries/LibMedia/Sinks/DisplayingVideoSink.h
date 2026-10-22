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
    void set_ticking(bool);
    bool is_ticking() const { return m_is_ticking; }

    DisplayingVideoSinkUpdateResult update(MonotonicTime now);
    RefPtr<VideoFrame> current_frame() const;
    DisplayingVideoSinkFrameCounts take_frame_counts() { return exchange(m_frame_counts, {}); }

    void set_on_present_needed(Function<void()> handler) { m_on_present_needed = move(handler); }

private:
    void dispatch_state_if_changed(PipelineStatus);
    void seek_back_to_current_time();

    MediaTimeReader m_time_reader;
    RefPtr<VideoProducer> m_input;

    bool m_is_ticking { true };
    RefPtr<VideoFrame> m_next_frame;
    RefPtr<VideoFrame> m_current_frame;
    bool m_cached_frames_are_discontinuous { false };
    bool m_seek_resolution_is_pending_frame_at_target { false };
    DisplayingVideoSinkFrameCounts m_frame_counts;

    enum class SeekStatus : u8 {
        None,
        InProgress,
    };
    SeekStatus m_seek_status { SeekStatus::None };

    PipelineStateChangeHandler m_on_state_changed;
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
    u32 m_seek_id { 0 };

    PipelineResizeHandler m_on_resize;
    Gfx::Size<u32> m_last_dispatched_size;

    Function<void()> m_on_present_needed;
    bool m_last_update_may_require_updates { false };
};

}
