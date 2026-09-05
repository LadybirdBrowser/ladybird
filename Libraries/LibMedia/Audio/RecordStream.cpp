/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/RecordStream.h>

namespace Audio {

// Backends that support capture define RecordStream::create() in their own translation unit.
#if !defined(LIBMEDIA_AUDIO_CAPTURE_BACKEND)

NonnullRefPtr<RecordStream::CreatePromise> RecordStream::create(SampleSpecification const&, u32, StringView, RecordCallback)
{
    return CreatePromise::rejected(Error::from_string_literal("Audio capture is not supported on this platform"));
}

#endif

}
