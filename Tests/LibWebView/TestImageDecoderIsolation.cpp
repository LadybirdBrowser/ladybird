/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <signal.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    }
};

HashTable<pid_t> processes_of_type(WebView::ProcessType type)
{
    HashTable<pid_t> pids;
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == type)
            pids.set(process.pid());
    });
    return pids;
}

}

static constexpr auto page_with_image = "<!DOCTYPE html><img src=\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7\">"sv;

// An ImageDecoder parses untrusted images, so it is launched only for a process that has some to decode. Each such process
// has one of its own, and never sees another's images. A decoder that crashes is replaced for its renderer alone, once the
// renderer next has an image to decode.

TEST_CASE(image_decoder_isolation)
{
    Test::HeadlessTestEnvironment environment { "TestImageDecoderIsolation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto first_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second_view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    size_t first_view_loads = 0;
    size_t second_view_loads = 0;
    first_view->on_load_finish = [&](auto const&) { ++first_view_loads; };
    second_view->on_load_finish = [&](auto const&) { ++second_view_loads; };
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 1 && second_view_loads >= 1; });

    // Nothing has had an image to decode yet.
    VERIFY(processes_of_type(WebView::ProcessType::ImageDecoder).is_empty());

    // The load event waits for the image, so its decoder has been launched by the time the load finishes.
    first_view->load_html(page_with_image);
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 2; });
    VERIFY(processes_of_type(WebView::ProcessType::ImageDecoder).size() == 1);

    second_view->load_html(page_with_image);
    Core::EventLoop::current().spin_until([&] { return second_view_loads >= 2; });
    auto decoders = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(decoders.size() == 2);

    auto crashed_decoder = *decoders.begin();
    TRY_OR_FAIL(Core::System::kill(crashed_decoder, SIGKILL));
    Core::EventLoop::current().spin_until([&] {
        return !processes_of_type(WebView::ProcessType::ImageDecoder).contains(crashed_decoder);
    });

    // Both renderers decode again, and only the one that lost its decoder gets a new one.
    first_view->load_html(page_with_image);
    second_view->load_html(page_with_image);
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 3 && second_view_loads >= 3; });

    auto decoders_after_crash = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(decoders_after_crash.size() == 2);
    VERIFY(!decoders_after_crash.contains(crashed_decoder));
    for (auto decoder : decoders) {
        if (decoder != crashed_decoder)
            VERIFY(decoders_after_crash.contains(decoder));
    }

    outln("PASS: each renderer gets an image decoder of its own when it needs one, which is replaced alone when it crashes");
}
