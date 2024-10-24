/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ladybird/Headless/Application.h>
#include <Ladybird/Headless/HeadlessWebView.h>
#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/System.h>

namespace Ladybird {

Application::Application(Badge<WebView::Application>, Main::Arguments&)
    : resources_folder(s_ladybird_resource_root)
    , test_concurrency(Core::System::hardware_concurrency())
{
}

void Application::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(screenshot_timeout, "Take a screenshot after [n] seconds (default: 1)", "screenshot", 's', "n");
    args_parser.add_option(dump_layout_tree, "Dump layout tree and exit", "dump-layout-tree", 'd');
    args_parser.add_option(dump_text, "Dump text and exit", "dump-text", 'T');
    args_parser.add_option(test_concurrency, "Maximum number of tests to run at once", "test-concurrency", 'j', "jobs");
    args_parser.add_option(test_root_path, "Run tests in path", "run-tests", 'R', "test-root-path");
    args_parser.add_option(test_glob, "Only run tests matching the given glob", "filter", 'f', "glob");
    args_parser.add_option(test_dry_run, "List the tests that would be run, without running them", "dry-run");
    args_parser.add_option(dump_failed_ref_tests, "Dump screenshots of failing ref tests", "dump-failed-ref-tests", 'D');
    args_parser.add_option(dump_gc_graph, "Dump GC graph", "dump-gc-graph", 'G');
    args_parser.add_option(resources_folder, "Path of the base resources folder (defaults to /res)", "resources", 'r', "resources-root-path");
    args_parser.add_option(is_layout_test_mode, "Enable layout test mode", "layout-test-mode");
    args_parser.add_option(rebaseline, "Rebaseline any executed layout or text tests", "rebaseline");
    args_parser.add_option(log_slowest_tests, "Log the tests with the slowest run times", "log-slowest-tests");
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
}

ErrorOr<void> Application::launch_services()
{
    auto request_server_paths = TRY(get_paths_for_helper_process("RequestServer"sv));
    m_request_client = TRY(launch_request_server_process(request_server_paths, resources_folder));

    auto image_decoder_paths = TRY(get_paths_for_helper_process("ImageDecoder"sv));
    m_image_decoder_client = TRY(launch_image_decoder_process(image_decoder_paths));

    return {};
}

HeadlessWebView& Application::create_web_view(Core::AnonymousBuffer theme, Gfx::IntSize window_size)
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
