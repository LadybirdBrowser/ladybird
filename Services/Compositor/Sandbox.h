/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Platform.h>
#include <AK/StringView.h>

namespace Compositor {

#if defined(AK_OS_LINUX)
// Landlock confines only the calling thread and the threads that it starts later, so this has to run before the GPU
// driver starts its threads. apply_sandbox() then installs the rest of the sandbox.
[[nodiscard]] ErrorOr<void> restrict_filesystem();
#endif

[[nodiscard]] ErrorOr<void> apply_sandbox(StringView mach_server_name, StringView cache_path);

}
