/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Audio {

// Why the AudioServer could not open a capture stream.
enum class RecordStreamFailure : u8 {
    // The Browser refused it: the tab may no longer capture, or the system keeps the microphone from the browser.
    Refused,
    DeviceUnavailable,
};

}
