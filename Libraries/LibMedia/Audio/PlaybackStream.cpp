/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaybackStream.h"

namespace Audio {

#if !defined(AK_OS_WINDOWS)
ErrorOr<NonnullRefPtr<PlaybackStream>> __attribute__((weak)) PlaybackStream::create(OutputState, u32, u8, u32, AudioDataRequestCallback&&)
#else
ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStream::create(OutputState, u32, u8, u32, AudioDataRequestCallback&&)
#endif
{
    return Error::from_string_literal("Audio output is not available for this platform");
}

}
