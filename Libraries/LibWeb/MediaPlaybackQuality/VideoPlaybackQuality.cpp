/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/VideoPlaybackQualityPrototype.h>
#include <LibWeb/MediaPlaybackQuality/VideoPlaybackQuality.h>

namespace Web::MediaPlaybackQuality {

GC_DEFINE_ALLOCATOR(VideoPlaybackQuality);

VideoPlaybackQuality::VideoPlaybackQuality(JS::Realm& realm, HighResolutionTime::DOMHighResTimeStamp creation_time, u32 dropped_video_frames, u32 total_video_frames)
    : Bindings::PlatformObject(realm)
    , m_creation_time(creation_time)
    , m_dropped_video_frames(dropped_video_frames)
    , m_total_video_frames(total_video_frames)
{
}

VideoPlaybackQuality::~VideoPlaybackQuality() = default;

void VideoPlaybackQuality::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VideoPlaybackQuality);
    Base::initialize(realm);
}

}
