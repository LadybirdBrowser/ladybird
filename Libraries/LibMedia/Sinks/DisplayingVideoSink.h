/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/Track.h>

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

    virtual void set_producer(Track const&, RefPtr<DecodedVideoProducer> const&) override;
    RefPtr<DecodedVideoProducer> producer(Track const&) const override;

    [[nodiscard]] DisplayingVideoSinkUpdateResult update();
    void prepare_current_frame_for_next_update();
    RefPtr<VideoFrame> current_frame();

    void pause_updates();
    void resume_updates();

private:
    void verify_track(Track const&) const;
    void dispatch_state_if_changed(PipelineStatus);

    NonnullRefPtr<MediaTimeProvider> m_time_provider;
    RefPtr<DecodedVideoProducer> m_producer;
    Optional<Track> m_track;

    RefPtr<VideoFrame> m_next_frame;
    RefPtr<VideoFrame> m_current_frame;
    bool m_pause_updates { false };
    bool m_has_new_current_frame { false };

    PipelineStateChangeHandler m_on_state_changed;
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
};

}
