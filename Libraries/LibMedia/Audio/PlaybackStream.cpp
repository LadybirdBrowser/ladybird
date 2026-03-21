/*
 * Copyright (c) 2023-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaybackStream.h"

namespace Audio {

#if !defined(AK_OS_WINDOWS)
NonnullRefPtr<PlaybackStream::CreatePromise> __attribute__((weak)) PlaybackStream::create(OutputState, u32, AudioDataRequestCallback&&)
#else
NonnullRefPtr<PlaybackStream::CreatePromise> PlaybackStream::create(OutputState, u32, AudioDataRequestCallback&&)
#endif
{
    auto promise = CreatePromise::construct();
    promise->reject(Error::from_string_literal("Audio output is not available for this platform"));
    return promise;
}

}
