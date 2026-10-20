/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

// https://w3c.github.io/media-playback-quality/#videoplaybackquality-interface
class VideoPlaybackQuality final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(VideoPlaybackQuality, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(VideoPlaybackQuality);

public:
    [[nodiscard]] static GC::Ref<VideoPlaybackQuality> create(HighResolutionTime::DOMHighResTimeStamp creation_time, WebIDL::UnsignedLong dropped_video_frames, WebIDL::UnsignedLong total_video_frames);

    HighResolutionTime::DOMHighResTimeStamp creation_time() const { return m_creation_time; }
    WebIDL::UnsignedLong dropped_video_frames() const { return m_dropped_video_frames; }
    WebIDL::UnsignedLong total_video_frames() const { return m_total_video_frames; }
    WebIDL::UnsignedLong corrupted_video_frames() const { return 0; }

private:
    VideoPlaybackQuality(HighResolutionTime::DOMHighResTimeStamp creation_time, WebIDL::UnsignedLong dropped_video_frames, WebIDL::UnsignedLong total_video_frames);

    HighResolutionTime::DOMHighResTimeStamp m_creation_time { 0 };
    WebIDL::UnsignedLong m_dropped_video_frames { 0 };
    WebIDL::UnsignedLong m_total_video_frames { 0 };
};

}
