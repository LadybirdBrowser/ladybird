/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/RefPtr.h>
#include <LibMedia/Forward.h>
#include <LibThreading/Mutex.h>

namespace Web::Painting {

class VideoFrameSource final : public AtomicRefCounted<VideoFrameSource> {
public:
    static NonnullRefPtr<VideoFrameSource> create();
    ~VideoFrameSource();

    void update(RefPtr<Media::VideoFrame>);
    void clear();
    RefPtr<Media::VideoFrame> current_frame() const;

private:
    VideoFrameSource() = default;

    mutable Threading::Mutex m_mutex;
    RefPtr<Media::VideoFrame> m_frame;
};

}
