/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/VideoSink.h>

namespace Media {

enum class DisplayingVideoSinkUpdateResult : u8 {
    NewFrameAvailable,
    NoChange,
};

class MEDIA_API DisplayingVideoSink final : public VideoSink {
public:
    static ErrorOr<NonnullRefPtr<DisplayingVideoSink>> try_create(NonnullRefPtr<MediaTimeProvider> const&, PipelineStateChangeHandler on_state_changed);

    DisplayingVideoSink(NonnullRefPtr<MediaTimeProvider> const&, PipelineStateChangeHandler);
    virtual ~DisplayingVideoSink() override;

    void set_time_provider(NonnullRefPtr<MediaTimeProvider> const&);

    virtual ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<VideoProducer> const&) override;

    virtual void seek(AK::Duration timestamp) override;

    [[nodiscard]] DisplayingVideoSinkUpdateResult update();
    RefPtr<VideoFrame> current_frame();

private:
    void dispatch_state_if_changed(PipelineStatus);

    NonnullRefPtr<MediaTimeProvider> m_time_provider;
    RefPtr<VideoProducer> m_input;

    RefPtr<VideoFrame> m_next_frame;
    RefPtr<VideoFrame> m_current_frame;
    bool m_seek_pending_display_update { false };

    PipelineStateChangeHandler m_on_state_changed;
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
};

}
