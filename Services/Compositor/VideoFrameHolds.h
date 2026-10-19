/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCompositing/Video/VideoFramePresentationPage.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaCodecs/Forward.h>

namespace Compositor {

class VideoFrameHolds {
public:
    void add_sink(Media::VideoSinkHandle, NonnullRefPtr<Compositing::VideoFramePresentationPage>);
    void remove_sink(Media::VideoSinkHandle);

    void present_frame(Media::VideoSinkHandle, RefPtr<Media::VideoFrame>);

    ErrorOr<void> release_hold(Media::VideoSinkHandle, u64 generation);
    RefPtr<Media::VideoFrame> held_frame(Media::VideoSinkHandle, u64 generation) const;

private:
    struct Sink {
        NonnullRefPtr<Compositing::VideoFramePresentationPage> page;
        u64 generation { 0 };
        RefPtr<Media::VideoFrame> frame;
    };
    struct RetainedFrame {
        RefPtr<Media::VideoFrame> frame;
        u64 hold_count { 0 };
    };

    void retire_current_generation(Media::VideoSinkHandle, Sink&, u64 next_generation);

    HashMap<Media::VideoSinkHandle, Sink> m_sinks;
    HashMap<Media::VideoSinkHandle, HashMap<u64, RetainedFrame>> m_retained_frames;
};

}
