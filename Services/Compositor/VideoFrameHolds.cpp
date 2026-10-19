/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <Compositor/VideoFrameHolds.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositor {

using Compositing::VideoFramePresentationPage;

void VideoFrameHolds::add_sink(Media::VideoSinkHandle handle, NonnullRefPtr<VideoFramePresentationPage> page)
{
    // WebContent may already hold generation 0.
    if (auto sink = m_sinks.get(handle); sink.has_value())
        retire_current_generation(handle, *sink, VideoFramePresentationPage::retired_generation);
    m_sinks.set(handle, Sink { .page = move(page), .generation = 0, .frame = nullptr });
}

void VideoFrameHolds::remove_sink(Media::VideoSinkHandle handle)
{
    auto sink = m_sinks.get(handle);
    if (!sink.has_value())
        return;
    retire_current_generation(handle, *sink, VideoFramePresentationPage::retired_generation);
    m_sinks.remove(handle);
}

void VideoFrameHolds::present_frame(Media::VideoSinkHandle handle, RefPtr<Media::VideoFrame> frame)
{
    auto sink = m_sinks.get(handle);
    if (!sink.has_value() || sink->frame == frame)
        return;
    auto next_generation = sink->generation + 1;
    retire_current_generation(handle, *sink, next_generation);
    sink->generation = next_generation;
    sink->frame = move(frame);

    Compositing::VideoFramePresentationPage::PresentedFrame presented_frame { .generation = next_generation, .presentation_time = MonotonicTime::now() };
    if (sink->frame) {
        sink->presented_frame_count++;
        presented_frame.media_time = sink->frame->timestamp();
        presented_frame.width = sink->frame->width();
        presented_frame.height = sink->frame->height();
    }
    presented_frame.presented_frame_count = sink->presented_frame_count;
    sink->page->publish_presented_frame(presented_frame);
}

void VideoFrameHolds::retire_current_generation(Media::VideoSinkHandle handle, Sink& sink, u64 next_generation)
{
    auto replaced = sink.page->replace_generation(next_generation);
    if (replaced.generation != sink.generation) {
        dbgln("Compositor: WebContent overwrote the generation of video sink {}", handle.value());
        return;
    }
    if (replaced.hold_count == 0)
        return;
    auto& retained_frames = m_retained_frames.ensure(handle);
    retained_frames.set(sink.generation, RetainedFrame { .frame = sink.frame, .hold_count = replaced.hold_count });
}

ErrorOr<void> VideoFrameHolds::release_hold(Media::VideoSinkHandle handle, u64 generation)
{
    if (auto sink = m_sinks.get(handle); sink.has_value() && sink->generation == generation) {
        if (sink->page->release_hold_if_current(generation) != VideoFramePresentationPage::ReleaseResult::Released)
            return Error::from_string_literal("The current video frame was not held");
        return {};
    }

    auto retained_frames = m_retained_frames.get(handle);
    if (!retained_frames.has_value())
        return Error::from_string_literal("The video frame was not retained");
    auto retained_frame = retained_frames->get(generation);
    if (!retained_frame.has_value())
        return Error::from_string_literal("The video frame was not retained");
    if (--retained_frame->hold_count > 0)
        return {};
    retained_frames->remove(generation);
    if (retained_frames->is_empty())
        m_retained_frames.remove(handle);
    return {};
}

RefPtr<Media::VideoFrame> VideoFrameHolds::held_frame(Media::VideoSinkHandle handle, u64 generation) const
{
    if (auto sink = m_sinks.get(handle); sink.has_value() && sink->generation == generation)
        return sink->frame;
    auto retained_frames = m_retained_frames.get(handle);
    if (!retained_frames.has_value())
        return nullptr;
    auto retained_frame = retained_frames->get(generation);
    if (!retained_frame.has_value())
        return nullptr;
    return retained_frame->frame;
}

}
