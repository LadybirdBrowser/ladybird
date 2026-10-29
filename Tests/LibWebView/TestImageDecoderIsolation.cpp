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

// An ImageDecoder parses untrusted images, so each renderer has one of its own, and never sees another's images. A
// decoder that crashes is replaced for its renderer alone.

TEST_CASE(image_decoder_isolation)
{
    Test::HeadlessTestEnvironment environment { "TestImageDecoderIsolation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto first_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second_view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    auto renderers = processes_of_type(WebView::ProcessType::WebContent);
    auto decoders = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(renderers.size() >= 2);
    VERIFY(decoders.size() == renderers.size());

    auto crashed_decoder = *decoders.begin();
    TRY_OR_FAIL(Core::System::kill(crashed_decoder, SIGKILL));

    // The replacement is launched as the Browser learns that the decoder died, so this cannot finish without it.
    Core::EventLoop::current().spin_until([&] {
        return !processes_of_type(WebView::ProcessType::ImageDecoder).contains(crashed_decoder);
    });

    // A spare renderer may have been launched meanwhile, with a decoder of its own.
    auto decoders_after_crash = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(decoders_after_crash.size() == processes_of_type(WebView::ProcessType::WebContent).size());
    for (auto decoder : decoders) {
        if (decoder != crashed_decoder)
            VERIFY(decoders_after_crash.contains(decoder));
    }

    outln("PASS: each renderer has an image decoder of its own, which is replaced alone when it crashes");
}
