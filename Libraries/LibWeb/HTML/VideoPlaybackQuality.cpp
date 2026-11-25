/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/VideoPlaybackQuality.h>

namespace Web::HTML {

// Defined in Bindings/Rust/src/html/video_playback_quality.rs.
extern "C" void* web_rust_video_playback_quality_create(JSVM*, double creation_time, u32 dropped_video_frames, u32 total_video_frames);

GC::Ref<VideoPlaybackQuality> VideoPlaybackQuality::create(HighResolutionTime::DOMHighResTimeStamp creation_time, WebIDL::UnsignedLong dropped_video_frames, WebIDL::UnsignedLong total_video_frames)
{
    return GC::Ref { *static_cast<VideoPlaybackQuality*>(web_rust_video_playback_quality_create(JS::HostABI::vm_to_abi(JS::VM::the()), creation_time, dropped_video_frames, total_video_frames)) };
}

}
