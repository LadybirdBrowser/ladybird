/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/FixedArray.h>
#include <AK/Types.h>
#include <LibMedia/Audio/AudioFrameRingBase.h>
#include <LibMedia/Export.h>

namespace Audio {

// A lock-free, allocation-free single-producer/single-consumer ring of interleaved float32 frames that owns its
// storage; atomic reference counting permits producer and consumer ownership on different threads.
class MEDIA_API SpscAudioFrameRing final : public AudioFrameRingBase
    , public AtomicRefCounted<SpscAudioFrameRing> {
public:
    // The frame capacity is rounded up to the next power of two.
    SpscAudioFrameRing(size_t frame_capacity, u32 channel_count);

private:
    AudioFrameRingHeader m_owned_header;
    FixedArray<float> m_owned_samples;
};

}
