/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/VideoPlaybackQuality.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(VideoPlaybackQuality);

GC::Ref<VideoPlaybackQuality> VideoPlaybackQuality::create(HighResolutionTime::DOMHighResTimeStamp creation_time, WebIDL::UnsignedLong dropped_video_frames, WebIDL::UnsignedLong total_video_frames)
{
    return GC::Heap::the().allocate<VideoPlaybackQuality>(creation_time, dropped_video_frames, total_video_frames);
}

VideoPlaybackQuality::VideoPlaybackQuality(HighResolutionTime::DOMHighResTimeStamp creation_time, WebIDL::UnsignedLong dropped_video_frames, WebIDL::UnsignedLong total_video_frames)
    : m_creation_time(creation_time)
    , m_dropped_video_frames(dropped_video_frames)
    , m_total_video_frames(total_video_frames)
{
}

}
