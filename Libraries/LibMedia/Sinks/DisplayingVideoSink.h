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
#include <AK/Time.h>
#include <LibGfx/Size.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoPresentation/PresentedFramePage.h>
#include <LibSync/Weakable.h>

namespace Media {

struct DisplayingVideoSinkUpdateResult {
    bool new_frame_available { false };
    bool may_require_updates { false };
};

class MEDIA_API DisplayingVideoSink final : public VideoSink
    , public Sync::Weakable<DisplayingVideoSink> {
public:
    static ErrorOr<NonnullRefPtr<DisplayingVideoSink>> try_create(MediaTimeReader);

    explicit DisplayingVideoSink(MediaTimeReader);
    virtual ~DisplayingVideoSink() override;

    virtual void set_time_reader(MediaTimeReader) override;
    virtual void set_state_change_handler(PipelineStateChangeHandler) override;
    virtual void set_resize_handler(PipelineResizeHandler) override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<VideoProducer> const&) override;

    virtual void seek(AK::Duration timestamp) override;

    DisplayingVideoSinkUpdateResult update(MonotonicTime now);
    virtual RefPtr<VideoFrame> current_frame() const override;

    void set_on_present_needed(Function<void()> handler) { m_on_present_needed = move(handler); }
    void set_presented_frame_page(PresentedFramePage page) { m_presented_frame_page = move(page); }

private:
    void dispatch_state_if_changed(PipelineStatus);

    MediaTimeReader m_time_reader;
    RefPtr<VideoProducer> m_input;

    RefPtr<VideoFrame> m_next_frame;
    RefPtr<VideoFrame> m_current_frame;
    bool m_cached_frames_are_discontinuous { false };

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
    Optional<PresentedFramePage> m_presented_frame_page;
};

}
