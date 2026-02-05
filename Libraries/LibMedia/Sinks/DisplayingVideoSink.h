/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/TimedImage.h>
#include <LibMedia/Track.h>

namespace Media {

enum class DisplayingVideoSinkUpdateResult : u8 {
    NewFrameAvailable,
    NoChange,
};

class MEDIA_API DisplayingVideoSink final : public VideoSink {
public:
    static ErrorOr<NonnullRefPtr<DisplayingVideoSink>> try_create(NonnullRefPtr<MediaTimeProvider> const&);

    DisplayingVideoSink(NonnullRefPtr<MediaTimeProvider> const&);
    virtual ~DisplayingVideoSink() override;

    virtual void set_provider(Track const&, RefPtr<VideoDataProvider> const&) override;
    RefPtr<VideoDataProvider> provider(Track const&) const override;

    // Updates the frame returned by current_frame() based on the time provider's current timestamp.
    //
    // Note that push_frame may block until update() is called, so do not call them from the same thread.
    DisplayingVideoSinkUpdateResult update();
    void prepare_current_frame_for_next_update();
    RefPtr<Gfx::ImmutableBitmap> current_frame();

    void pause_updates();
    void resume_updates();

    Function<void()> m_on_start_buffering;

private:
    static constexpr size_t DEFAULT_QUEUE_SIZE = 8;

    void verify_track(Track const&) const;

    NonnullRefPtr<MediaTimeProvider> m_time_provider;
    RefPtr<VideoDataProvider> m_provider;
    Optional<Track> m_track;

    TimedImage m_next_frame;
    RefPtr<Gfx::ImmutableBitmap> m_current_frame;
    bool m_pause_updates { false };
    bool m_has_new_current_frame { false };
};

}
