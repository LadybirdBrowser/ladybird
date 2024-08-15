/*
 * Copyright (c) 2018-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibAudio/Sample.h>
#include <LibCore/SharedCircularQueue.h>

namespace Audio {

inline constexpr size_t AUDIO_BUFFERS_COUNT = 128;
// The audio buffer size is specifically chosen to be about 1/1000th of a second (1ms).
// This has the biggest impact on latency and performance.
// The currently chosen value was not put here with much thought and a better choice is surely possible.
inline constexpr size_t AUDIO_BUFFER_SIZE = 50;
using AudioQueue = Core::SharedSingleProducerCircularQueue<Array<Sample, AUDIO_BUFFER_SIZE>, AUDIO_BUFFERS_COUNT>;

}
