/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibAudio/ChannelMap.h>
#include <LibAudio/Export.h>

namespace Audio {

// Reads the speaker positions of a WAVEFORMATEXTENSIBLE channel mask, in the order its channels are interleaved.
AUDIO_API ErrorOr<ChannelMap> windows_channel_mask_to_channel_map(u32 channel_mask);

}
