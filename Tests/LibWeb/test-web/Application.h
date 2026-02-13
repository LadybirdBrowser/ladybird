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

namespace TestWeb {

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    explicit Application(Optional<ByteString> ladybird_binary_path);
    ~Application();

    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual bool should_capture_web_content_output() const override { return true; }

    ErrorOr<void> launch_test_fixtures();

    static constexpr u8 VERBOSITY_LEVEL_LOG_TEST_OUTPUT = 1;
    static constexpr u8 VERBOSITY_LEVEL_LOG_TEST_DURATION = 2;
    static constexpr u8 VERBOSITY_LEVEL_LOG_SLOWEST_TESTS = 3;
    static constexpr u8 VERBOSITY_LEVEL_LOG_SKIPPED_TESTS = 4;

    ByteString test_root_path;
    ByteString results_directory { "test-dumps/results"sv };
    size_t test_concurrency { 1 };
    Vector<ByteString> test_globs;

    ByteString python_executable_path;

    bool dump_gc_graph { false };
    bool debug_timeouts { false };
    bool fail_fast { false };
    size_t repeat_count { 1 };
    bool test_dry_run { false };
    bool rebaseline { false };
    bool shuffle { false };

    int per_test_timeout_in_seconds { 30 };

    u8 verbosity { 0 };
};

}
