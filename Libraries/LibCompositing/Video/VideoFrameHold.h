/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Video/VideoFramePresentationPage.h>
#include <LibIPC/Forward.h>
#include <LibMedia/VideoSinkHandle.h>

namespace Compositing {

class COMPOSITING_API VideoFrameHold final
    : public RefCounted<VideoFrameHold>
    , public Weakable<VideoFrameHold> {
public:
    using ReleaseThroughCompositor = Function<void(Media::VideoSinkHandle, u64 generation)>;

    static RefPtr<VideoFrameHold> hold_current_frame(NonnullRefPtr<VideoFramePresentationPage>, Media::VideoSinkHandle, ReleaseThroughCompositor);
    static RefPtr<VideoFrameHold> hold_frame_of_generation(NonnullRefPtr<VideoFramePresentationPage>, Media::VideoSinkHandle, u64 generation, ReleaseThroughCompositor);
    ~VideoFrameHold();

    Media::VideoSinkHandle sink_handle() const { return m_sink_handle; }
    u64 generation() const { return m_generation; }

    void note_sent_to_compositor() { m_was_sent_to_compositor = true; }

private:
    VideoFrameHold(NonnullRefPtr<VideoFramePresentationPage>, Media::VideoSinkHandle, u64 generation, ReleaseThroughCompositor);

    NonnullRefPtr<VideoFramePresentationPage> m_page;
    Media::VideoSinkHandle m_sink_handle;
    u64 m_generation { 0 };
    ReleaseThroughCompositor m_release_through_compositor;
    bool m_was_sent_to_compositor { false };
};

struct COMPOSITING_API CapturedVideoFrame {
    Media::VideoSinkHandle sink_handle;
    u64 generation { 0 };
    RefPtr<VideoFrameHold> hold;
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::CapturedVideoFrame const&);

template<>
COMPOSITING_API ErrorOr<Compositing::CapturedVideoFrame> decode(Decoder&);

}
