/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibMedia/Audio/ChannelMap.h>

struct AudioChannelLayout;

namespace Audio {

ErrorOr<ChannelMap> core_audio_channel_layout_to_channel_map(AudioChannelLayout const& channel_layout, u32 channel_layout_size);

}
