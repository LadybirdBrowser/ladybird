/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Platform.h>
#include <LibCore/Export.h>

#if !defined(AK_OS_WINDOWS)
#    include <signal.h>
#endif

namespace Core::CrashHandler {

// Install before sandboxing. The caller owns the descriptor and must keep it
// open for the process lifetime. Only bounded native diagnostics and assertion text are written.
CORE_API ErrorOr<void> initialize(int fd);

#if !defined(AK_OS_WINDOWS)
[[noreturn]] CORE_API void handle_fatal_signal(int signal, siginfo_t*, void* context);
#endif

}
