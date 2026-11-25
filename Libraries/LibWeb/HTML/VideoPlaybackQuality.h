/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/VideoPlaybackQuality.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

// https://w3c.github.io/media-playback-quality/#videoplaybackquality-interface
class VideoPlaybackQuality final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(VideoPlaybackQuality, video_playback_quality)

public:
    [[nodiscard]] static GC::Ref<VideoPlaybackQuality> create(HighResolutionTime::DOMHighResTimeStamp creation_time, WebIDL::UnsignedLong dropped_video_frames, WebIDL::UnsignedLong total_video_frames);
};

}
