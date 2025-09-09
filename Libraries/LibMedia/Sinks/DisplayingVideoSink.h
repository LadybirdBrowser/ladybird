/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/TimedImage.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

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
    RefPtr<Gfx::Bitmap> current_frame();

private:
    static constexpr size_t DEFAULT_QUEUE_SIZE = 8;

    void verify_track(Track const&) const;

    NonnullRefPtr<MediaTimeProvider> m_time_provider;
    RefPtr<VideoDataProvider> m_provider;
    Optional<Track> m_track;

    Threading::Mutex m_mutex;
    TimedImage m_next_frame;
    RefPtr<Gfx::Bitmap> m_current_frame;
};

}
