/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/Painting/VideoFrameSource.h>

namespace Web::Painting {

static Atomic<u64> s_next_id { 1 };

NonnullRefPtr<VideoFrameSource> VideoFrameSource::create()
{
    return adopt_ref(*new VideoFrameSource());
}

VideoFrameSource::VideoFrameSource()
    : m_id(s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

VideoFrameSource::~VideoFrameSource() = default;

void VideoFrameSource::update(RefPtr<Media::VideoFrame> frame)
{
    RefPtr<Media::VideoFrame> old;
    {
        Threading::MutexLocker const locker { m_mutex };
        old = move(m_frame);
        m_frame = move(frame);
    }
}

void VideoFrameSource::clear()
{
    RefPtr<Media::VideoFrame> old;
    {
        Threading::MutexLocker const locker { m_mutex };
        old = move(m_frame);
    }
}

RefPtr<Media::VideoFrame> VideoFrameSource::current_frame() const
{
    Threading::MutexLocker const locker { m_mutex };
    return m_frame;
}

}
