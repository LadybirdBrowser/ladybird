/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWebView/Options.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

enum class ProcessScope : u8 {
    // These scopes describe how the browser routes helper-process requests.
    // The HTML spec models execution in terms of agents (an idealized thread
    // of script execution) and agent clusters (an idealized process boundary).
    // Ladybird's process model does not map 1:1 to the spec, but we try to keep
    // the policy language compatible with those concepts.
    Singleton,

    // Cache one helper instance per top-level page (page_id). This is the
    // closest approximation to "per agent cluster" for helpers that should not
    // accumulate across navigations/tests.
    PerView,

    // Do not cache; spawn/connect a fresh helper instance per request.
    PerRequest,
};

class ProcessPolicyRouter {
public:
    static ProcessScope default_scope_for(ProcessType);

    // Encodes current behavior: keep one spare WebContent process unless it interferes
    // with debugging/profiling.
    static bool should_maintain_spare_web_content_process(BrowserOptions const&);

    // Encodes current behavior: these are launched as singleton services at startup.
    static Vector<ProcessType> singleton_services_to_launch();

    static bool web_content_has_live_audio_connection(pid_t pid);
    static void set_web_content_has_live_audio_connection(pid_t pid, bool has_live_connection);
    static void forget_web_content(pid_t pid);
    static void clear_all_web_content_audio_connections();

private:
    static HashMap<pid_t, bool> s_web_content_audio_connections;
};

}
