/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Camera {

enum class CaptureFailure : u8 {
    // The Browser refused it: the tab may not capture.
    Refused,
    DeviceUnavailable,
};

}
