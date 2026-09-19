/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>

namespace {

// Neither WebDriver nor the test harness drives this application, so its renderers get no HTTP-like cookie access.
class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

// A renderer must not read or write cookies the way an HTTP response or WebDriver does: that would expose and let it
// replace the HttpOnly cookies of every site. RequestServer handles the cookies of HTTP traffic itself, so the UI process
// treats such a request from a renderer as misbehavior and terminates it, and the cookie jar stays untouched.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestRendererCookieAccess-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto victim_url = URL::Parser::basic_parse("https://victim.example/"sv).release_value();

    // The messages below go straight to the UI process's handlers, which only need the view's page to be assigned. A
    // view may be given a spare process that finished its initial load before the view existed, so there is no load
    // to wait for.
    auto create_view = [&] {
        return WebView::HeadlessWebView::create(theme, { 800, 600 });
    };

    auto victim_cookie_value = [&](WebView::CookieJar& cookie_jar) -> Optional<String> {
        for (auto const& cookie : cookie_jar.get_all_cookies_webdriver(victim_url)) {
            if (cookie.name == "session"sv)
                return cookie.value;
        }
        return {};
    };

    auto view = create_view();
    auto& cookie_jar = *view->client().session().cookie_jar;
    cookie_jar.set_cookie(victim_url, HTTP::Cookie::ParsedCookie { .name = "session"_string, .value = "secret"_string, .http_only_attribute_present = true }, HTTP::Cookie::Source::Http);
    VERIFY(victim_cookie_value(cookie_jar) == "secret"sv);

    // Script access is fine, and does not see the HttpOnly cookie.
    auto& stub = static_cast<WebContentClientStub&>(view->client());
    VERIFY(stub.did_request_cookie(view->page_id(), victim_url, HTTP::Cookie::Source::NonHttp).cookie().cookie.is_empty());

    auto expect_rejected = [&](StringView what, Function<void(WebContentClientStub&, Web::PageId)> send) {
        auto view = create_view();
        Optional<WebView::ViewImplementation::WebContentCrashReason> crash_reason;
        view->on_web_content_crashed = [&](auto reason) { crash_reason = reason; };

        send(static_cast<WebContentClientStub&>(view->client()), view->page_id());
        Core::EventLoop::current().spin_until([&]() { return crash_reason.has_value(); });

        if (crash_reason != WebView::ViewImplementation::WebContentCrashReason::RejectedIPC) {
            warnln("FAIL: {} was not rejected", what);
            VERIFY_NOT_REACHED();
        }
        VERIFY(victim_cookie_value(cookie_jar) == "secret"sv);
    };

    expect_rejected("reading cookies with the HTTP source"sv, [&](auto& stub, auto page_id) {
        VERIFY(stub.did_request_cookie(page_id, victim_url, HTTP::Cookie::Source::Http).cookie().cookie.is_empty());
    });
    expect_rejected("reading all cookies for WebDriver"sv, [&](auto& stub, auto) {
        VERIFY(stub.did_request_all_cookies_webdriver(victim_url).cookies().is_empty());
    });
    expect_rejected("storing a cookie with the HTTP source"sv, [&](auto& stub, auto) {
        stub.did_set_cookie(victim_url, HTTP::Cookie::ParsedCookie { .name = "session"_string, .value = "attacker"_string, .http_only_attribute_present = true }, HTTP::Cookie::Source::Http);
    });
    expect_rejected("updating a cookie by its identity"sv, [&](auto& stub, auto) {
        auto cookie = cookie_jar.get_all_cookies_webdriver(victim_url).first();
        cookie.value = "attacker"_string;
        stub.did_update_cookie(move(cookie));
    });

    outln("PASS: renderers cannot read, store or update cookies the way HTTP responses and WebDriver do");
    return 0;
}
