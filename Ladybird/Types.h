/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Ladybird {

enum class EnableCallgrindProfiling {
    No,
    Yes
};

enum class EnableSkiaPainting {
    No,
    Yes
};

enum class IsLayoutTestMode {
    No,
    Yes
};

enum class UseLagomNetworking {
    No,
    Yes
};

enum class WaitForDebugger {
    No,
    Yes
};

enum class LogAllJSExceptions {
    No,
    Yes
};

enum class EnableIDLTracing {
    No,
    Yes
};

enum class EnableHTTPCache {
    No,
    Yes
};

enum class ExposeInternalsObject {
    No,
    Yes
};

struct WebContentOptions {
    String command_line;
    String executable_path;
    EnableCallgrindProfiling enable_callgrind_profiling { EnableCallgrindProfiling::No };
    EnableSkiaPainting enable_skia_painting { EnableSkiaPainting::No };
    IsLayoutTestMode is_layout_test_mode { IsLayoutTestMode::No };
    UseLagomNetworking use_lagom_networking { UseLagomNetworking::Yes };
    WaitForDebugger wait_for_debugger { WaitForDebugger::No };
    LogAllJSExceptions log_all_js_exceptions { LogAllJSExceptions::No };
    EnableIDLTracing enable_idl_tracing { EnableIDLTracing::No };
    EnableHTTPCache enable_http_cache { EnableHTTPCache::No };
    ExposeInternalsObject expose_internals_object { ExposeInternalsObject::No };
};

}
