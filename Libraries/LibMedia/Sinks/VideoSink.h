/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/RefPtr.h>
#include <LibMedia/Providers/VideoDataProvider.h>

namespace Media {

// A consumer to be attached to a VideoDataProvider in order to receive video frames from a decoding thread.
class VideoSink : public AtomicRefCounted<VideoSink> {
public:
    virtual ~VideoSink() = default;

    virtual void set_provider(Track const&, RefPtr<VideoDataProvider> const&) = 0;
    virtual RefPtr<VideoDataProvider> provider(Track const&) const = 0;
};

}
