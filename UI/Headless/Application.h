/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibWebView/Application.h>

namespace Ladybird {

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    ~Application();

    static Application& the()
    {
        return static_cast<Application&>(WebView::Application::the());
    }

    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::WebContentOptions&) override;

    ErrorOr<void> launch_test_fixtures();

    static constexpr u8 VERBOSITY_LEVEL_LOG_TEST_DURATION = 1;
    static constexpr u8 VERBOSITY_LEVEL_LOG_SLOWEST_TESTS = 2;
    static constexpr u8 VERBOSITY_LEVEL_LOG_SKIPPED_TESTS = 3;

    ByteString resources_folder;
    bool dump_failed_ref_tests { false };
    bool dump_gc_graph { false };
    size_t test_concurrency { 1 };
    ByteString python_executable_path;
    ByteString test_root_path;
    Vector<ByteString> test_globs;
    bool test_dry_run { false };
    bool rebaseline { false };
    u8 verbosity { 0 };
    int per_test_timeout_in_seconds { 30 };
};

}
