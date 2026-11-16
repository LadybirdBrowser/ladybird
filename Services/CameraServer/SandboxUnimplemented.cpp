/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CameraServer/Sandbox.h>

namespace CameraServer {

// FIXME: Sandbox the CameraServer on macOS once it has a backend there, with the camera as its one system service.
ErrorOr<void> apply_sandbox(StringView)
{
    return {};
}

}
