/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Application.h"
#include "Fixture.h"

#include <LibCore/ArgsParser.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibWebView/Utilities.h>

namespace TestWeb {

Application::Application(Badge<WebView::Application>, Main::Arguments&)
    : test_concurrency(Core::System::hardware_concurrency())
    , python_executable_path("python3")
{
    if (auto ladybird_source_dir = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv); ladybird_source_dir.has_value())
        test_root_path = LexicalPath::join(*ladybird_source_dir, "Tests"sv, "LibWeb"sv).string();
}

Application::~Application()
{
    for (auto& fixture : Fixture::all())
        fixture->teardown();
}

void Application::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(test_root_path, "Path containing the tests to run", "test-path", 0, "path");
    args_parser.add_option(test_concurrency, "Maximum number of tests to run at once", "test-concurrency", 'j', "jobs");
    args_parser.add_option(test_globs, "Only run tests matching the given glob", "filter", 'f', "glob");
    args_parser.add_option(python_executable_path, "Path to python3", "python-executable", 'P', "path");
    args_parser.add_option(dump_failed_ref_tests, "Dump screenshots of failing ref tests", "dump-failed-ref-tests", 'D');
    args_parser.add_option(dump_gc_graph, "Dump GC graph", "dump-gc-graph", 'G');
    args_parser.add_option(test_dry_run, "List the tests that would be run, without running them", "dry-run");
    args_parser.add_option(rebaseline, "Rebaseline any executed layout or text tests", "rebaseline");
    args_parser.add_option(per_test_timeout_in_seconds, "Per-test timeout (default: 30)", "per-test-timeout", 't', "seconds");

    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Optional,
        .help_string = "Log extra information about test results (use multiple times for more information)",
        .long_name = "verbose",
        .short_name = 'v',
        .accept_value = [&](StringView value) -> ErrorOr<bool> {
            if (value.is_empty() && verbosity < NumericLimits<u8>::max()) {
                ++verbosity;
                return true;
            }

            return false;
        },
    });
}

void Application::create_platform_options(WebView::BrowserOptions& browser_options, WebView::WebContentOptions& web_content_options)
{
    browser_options.headless_mode = WebView::HeadlessMode::Test;
    web_content_options.is_layout_test_mode = WebView::IsLayoutTestMode::Yes;

    // Allow window.open() to succeed for tests.
    browser_options.allow_popups = WebView::AllowPopups::Yes;

    // Ensure consistent font rendering between operating systems.
    web_content_options.force_fontconfig = WebView::ForceFontconfig::Yes;

    // Ensure tests are resilient to minor changes to the viewport scrollbar.
    web_content_options.paint_viewport_scrollbars = WebView::PaintViewportScrollbars::No;

    if (dump_gc_graph) {
        // Force all tests to run in serial if we are interested in the GC graph.
        test_concurrency = 1;
    }
}

ErrorOr<void> Application::launch_test_fixtures()
{
    Fixture::initialize_fixtures();

    // FIXME: Add option to only run specific fixtures from command line by name
    //        And an option to not run any fixtures at all
    for (auto const& fixture : Fixture::all()) {
        if (auto result = fixture->setup(web_content_options()); result.is_error())
            return result;
    }

    return {};
}

}
