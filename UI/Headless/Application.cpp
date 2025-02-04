/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/System.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Utilities.h>
#include <UI/Headless/Application.h>
#include <UI/Headless/Fixture.h>
#include <UI/Headless/HeadlessWebView.h>

namespace Ladybird {

Application::Application(Badge<WebView::Application>, Main::Arguments&)
    : resources_folder(WebView::s_ladybird_resource_root)
    , test_concurrency(Core::System::hardware_concurrency())
    , python_executable_path("python3")

{
}

Application::~Application()
{
    for (auto& fixture : Fixture::all())
        fixture->teardown();
}

void Application::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(screenshot_timeout, "Take a screenshot after [n] seconds (default: 1)", "screenshot", 's', "n");
    args_parser.add_option(dump_layout_tree, "Dump layout tree and exit", "dump-layout-tree", 'd');
    args_parser.add_option(dump_text, "Dump text and exit", "dump-text", 'T');
    args_parser.add_option(test_concurrency, "Maximum number of tests to run at once", "test-concurrency", 'j', "jobs");
    args_parser.add_option(python_executable_path, "Path to python3", "python-executable", 'P', "path");
    args_parser.add_option(test_root_path, "Run tests in path", "run-tests", 'R', "test-root-path");
    args_parser.add_option(test_globs, "Only run tests matching the given glob", "filter", 'f', "glob");
    args_parser.add_option(test_dry_run, "List the tests that would be run, without running them", "dry-run");
    args_parser.add_option(dump_failed_ref_tests, "Dump screenshots of failing ref tests", "dump-failed-ref-tests", 'D');
    args_parser.add_option(dump_gc_graph, "Dump GC graph", "dump-gc-graph", 'G');
    args_parser.add_option(resources_folder, "Path of the base resources folder (defaults to /res)", "resources", 'r', "resources-root-path");
    args_parser.add_option(is_layout_test_mode, "Enable layout test mode", "layout-test-mode");
    args_parser.add_option(rebaseline, "Rebaseline any executed layout or text tests", "rebaseline");
    args_parser.add_option(per_test_timeout_in_seconds, "Per-test timeout (default: 30)", "per-test-timeout", 't', "seconds");
    args_parser.add_option(width, "Set viewport width in pixels (default: 800)", "width", 'W', "pixels");
    args_parser.add_option(height, "Set viewport height in pixels (default: 600)", "height", 'H', "pixels");

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

void Application::create_platform_options(WebView::ChromeOptions& chrome_options, WebView::WebContentOptions& web_content_options)
{
    if (!test_root_path.is_empty()) {
        // --run-tests implies --layout-test-mode.
        is_layout_test_mode = true;
    }

    if (is_layout_test_mode) {
        // Allow window.open() to succeed for tests.
        chrome_options.allow_popups = WebView::AllowPopups::Yes;

        // Ensure consistent font rendering between operating systems.
        web_content_options.force_fontconfig = WebView::ForceFontconfig::Yes;
    }

    if (dump_gc_graph) {
        // Force all tests to run in serial if we are interested in the GC graph.
        test_concurrency = 1;
    }

    web_content_options.is_layout_test_mode = is_layout_test_mode ? WebView::IsLayoutTestMode::Yes : WebView::IsLayoutTestMode::No;
    web_content_options.is_headless = WebView::IsHeadless::Yes;
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

HeadlessWebView& Application::create_web_view(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
{
    auto web_view = HeadlessWebView::create(move(theme), window_size);
    m_web_views.append(move(web_view));

    return *m_web_views.last();
}

HeadlessWebView& Application::create_child_web_view(HeadlessWebView const& parent, u64 page_index)
{
    auto web_view = HeadlessWebView::create_child(parent, page_index);
    m_web_views.append(move(web_view));

    return *m_web_views.last();
}

void Application::destroy_web_views()
{
    m_web_views.clear();
}

}
