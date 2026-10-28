/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/LexicalPath.h>
#include <AK/String.h>
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

    virtual bool should_coordinate_browser_process() const override { return false; }
    virtual bool should_capture_web_content_output() const override { return true; }
    // A test's position is emulated, so the OS is never asked for one, nor for leave to get one: the machine's own
    // Location Services setting would otherwise read as a denial for every site a test allowed.
    virtual bool should_use_os_geolocation() const override { return false; }
    // Tests cover elastic overscroll on every platform, whichever of them the browser enables it on.
    virtual bool platform_scrolls_elastically() const override { return true; }

    virtual ErrorOr<LexicalPath> default_path_for_downloaded_file(ByteString const&) const override;

    ErrorOr<void> launch_test_fixtures();

    static constexpr u8 VERBOSITY_LEVEL_LOG_TEST_OUTPUT = 1;
    static constexpr u8 VERBOSITY_LEVEL_LOG_TEST_DURATION = 2;
    static constexpr u8 VERBOSITY_LEVEL_LOG_SLOWEST_TESTS = 3;
    static constexpr u8 VERBOSITY_LEVEL_LOG_SKIPPED_TESTS = 4;

    ByteString test_root_path;
    ByteString results_directory { "test-dumps/results"sv };
    Optional<ByteString> downloads_directory;
    size_t test_concurrency { 1 };
    Vector<ByteString> test_globs;

    ByteString python_executable_path;
    String invocation_command_line;

    bool dump_gc_graph { false };
    bool fail_fast { false };
    size_t repeat_count { 1 };
    bool test_dry_run { false };
    bool rebaseline { false };
    bool shuffle { false };
    bool run_ui_process_session_history_tests { false };
    bool verify_style { false };
    int per_test_timeout_in_seconds { 30 };

    u8 verbosity { 0 };
    bool quiet { false };
};

}
