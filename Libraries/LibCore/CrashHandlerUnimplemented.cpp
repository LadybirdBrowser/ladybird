/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/CrashHandler.h>

namespace Core::CrashHandler {

ErrorOr<void> initialize(int)
{
    return Error::from_string_literal("Native crash capture is not implemented on this platform");
}

}
