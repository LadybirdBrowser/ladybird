/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::MediaPlaybackQuality {

class VideoPlaybackQuality : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(VideoPlaybackQuality, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(VideoPlaybackQuality);

public:
    virtual ~VideoPlaybackQuality() override;

    HighResolutionTime::DOMHighResTimeStamp creation_time() const { return m_creation_time; }
    u32 dropped_video_frames() const { return m_dropped_video_frames; }
    u32 total_video_frames() const { return m_total_video_frames; }

private:
    VideoPlaybackQuality(JS::Realm&, HighResolutionTime::DOMHighResTimeStamp creation_time, u32 dropped_video_frames, u32 total_video_frames);

    virtual void initialize(JS::Realm&) override;

    HighResolutionTime::DOMHighResTimeStamp m_creation_time { 0.0 };
    u32 m_dropped_video_frames { 0 };
    u32 m_total_video_frames { 0 };
};

}
