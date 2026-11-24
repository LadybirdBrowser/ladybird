/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>

// When a RequestServer that died cannot be replaced, its instance is retired, and the next one asked for is launched.

TEST_CASE(a_request_server_that_cannot_be_replaced_is_retired)
{
    Test::HeadlessTestEnvironment environment { "TestRequestServerRelaunchFailure"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto& session = WebView::Application::default_session();
    auto instance = TRY_OR_FAIL(WebView::RequestServerManager::the().browser_instance(session));

    // With every file descriptor taken, the replacement's sockets cannot be made.
    rlimit original_limit {};
    VERIFY(getrlimit(RLIMIT_NOFILE, &original_limit) == 0);
    auto lowest_free_descriptor = TRY_OR_FAIL(Core::System::open("/dev/null"sv, O_RDONLY));
    TRY_OR_FAIL(Core::System::close(lowest_free_descriptor));
    auto exhausted_limit = original_limit;
    exhausted_limit.rlim_cur = static_cast<rlim_t>(lowest_free_descriptor);
    VERIFY(setrlimit(RLIMIT_NOFILE, &exhausted_limit) == 0);

    WebView::Application::process_manager().for_each_process([](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::RequestServer)
            MUST(Core::System::kill(process.pid(), SIGKILL));
    });
    Core::EventLoop::current().spin_until([&] { return instance->is_retired(); });

    VERIFY(setrlimit(RLIMIT_NOFILE, &original_limit) == 0);
    auto replacement = TRY_OR_FAIL(WebView::RequestServerManager::the().browser_instance(session));
    VERIFY(replacement.ptr() != instance.ptr());

    outln("PASS: a RequestServer that cannot be replaced is retired, and the next one asked for is launched");
}
