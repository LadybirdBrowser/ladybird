/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

RefPtr<VideoFrameHold> VideoFrameHold::hold_current_frame(NonnullRefPtr<VideoFramePresentationPage> page, Media::VideoSinkHandle sink_handle, ReleaseThroughCompositor release_through_compositor)
{
    auto generation = page->hold_current_generation();
    if (!generation.has_value())
        return nullptr;
    return adopt_ref(*new VideoFrameHold(move(page), sink_handle, generation.value(), move(release_through_compositor)));
}

RefPtr<VideoFrameHold> VideoFrameHold::hold_frame_of_generation(NonnullRefPtr<VideoFramePresentationPage> page, Media::VideoSinkHandle sink_handle, u64 generation, ReleaseThroughCompositor release_through_compositor)
{
    if (!page->hold_generation(generation))
        return nullptr;
    return adopt_ref(*new VideoFrameHold(move(page), sink_handle, generation, move(release_through_compositor)));
}

VideoFrameHold::VideoFrameHold(NonnullRefPtr<VideoFramePresentationPage> page, Media::VideoSinkHandle sink_handle, u64 generation, ReleaseThroughCompositor release_through_compositor)
    : m_page(move(page))
    , m_sink_handle(sink_handle)
    , m_generation(generation)
    , m_release_through_compositor(move(release_through_compositor))
{
}

VideoFrameHold::~VideoFrameHold()
{
    if (!m_was_sent_to_compositor && m_page->release_hold_if_current(m_generation) == VideoFramePresentationPage::ReleaseResult::Released)
        return;
    if (m_release_through_compositor)
        m_release_through_compositor(m_sink_handle, m_generation);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::CapturedVideoFrame const& frame)
{
    TRY(encoder.encode(frame.sink_handle));
    TRY(encoder.encode(frame.generation));
    return {};
}

template<>
ErrorOr<Compositing::CapturedVideoFrame> decode(Decoder& decoder)
{
    return Compositing::CapturedVideoFrame {
        .sink_handle = TRY(decoder.decode<Media::VideoSinkHandle>()),
        .generation = TRY(decoder.decode<u64>()),
        .hold = nullptr,
    };
}

}
