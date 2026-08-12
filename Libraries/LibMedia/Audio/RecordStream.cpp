/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/RecordStream.h>

namespace Audio {

// Backends that support capture define RecordStream::create() in their own translation unit.
#if !defined(LADYBIRD_AUDIO_BACKEND_PULSE) && !defined(LADYBIRD_AUDIO_BACKEND_AUDIO_UNIT)

ErrorOr<NonnullRefPtr<RecordStream>> RecordStream::create(SampleSpecification const&, u32, StringView, RecordCallback)
{
    return Error::from_string_literal("Audio capture is not supported on this platform");
}

#endif

}
