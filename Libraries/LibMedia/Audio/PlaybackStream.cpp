/*
 * Copyright (c) 2023-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaybackStream.h"

namespace Audio {

#if !defined(AK_OS_WINDOWS)
ErrorOr<NonnullRefPtr<PlaybackStream>> __attribute__((weak)) PlaybackStream::create(OutputState, u32, SampleSpecificationCallback&&, AudioDataRequestCallback&&)
#else
ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStream::create(OutputState, u32, SampleSpecificationCallback&&, AudioDataRequestCallback&&)
#endif
{
    return Error::from_string_literal("Audio output is not available for this platform");
}

}
