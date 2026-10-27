/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibWebView/Application.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Settings.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
    }

    virtual Core::EventLoop& create_platform_event_loop() override { return Core::EventLoop::current(); }
    virtual bool should_coordinate_browser_process() const override { return false; }
};

class TestEnvironment {
public:
    TestEnvironment()
        : m_test_directory(ByteString::formatted("{}/Ladybird-TestApplicationDestruction-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid()))
    {
    }

    ~TestEnvironment()
    {
        Core::ThreadEventQueue::destroy_current();
        if (FileSystem::exists(m_test_directory))
            (void)FileSystem::remove(m_test_directory, FileSystem::RecursionMode::Allowed);
    }

    ErrorOr<NonnullOwnPtr<TestApplication>> create_application(bool test_mode = true)
    {
        auto profile_path = LexicalPath::join(m_test_directory, "profile"sv).string();
        Vector<StringView> argument_strings { "ladybird"sv };

        if (test_mode) {
            argument_strings.append("--test-mode"sv);
        } else {
            argument_strings.append("--profile-path"sv);
            argument_strings.append(profile_path);
        }

        Main::Arguments arguments {
            .argc = 0,
            .argv = nullptr,
            .strings = argument_strings,
        };

#if defined(LADYBIRD_BINARY_PATH)
        return TestApplication::create(arguments, LADYBIRD_BINARY_PATH);
#else
        return TestApplication::create(arguments, OptionalNone {});
#endif
    }

private:
    Core::EventLoop m_event_loop;
    ByteString m_test_directory;
};

static ErrorOr<Core::AnonymousBuffer> load_test_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    return Gfx::load_system_theme(theme_path.string());
}

TEST_CASE(hsts_store_synchronized_on_destruction)
{
    TestEnvironment environment;
    ByteString database_path;
    {
        auto app = TRY_OR_FAIL(environment.create_application(false));
        auto view = WebView::HeadlessWebView::create(TRY_OR_FAIL(load_test_theme()), { 1000, 700 });
        database_path = app->profile().paths().data;

        // Push a HSTS policy into transient storage and immediately destroy the application to check persistence.
        view->session().hsts_store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });
    }

    auto database = TRY_OR_FAIL(Database::Database::create(database_path, "Ladybird"sv));
    auto hsts_store = TRY_OR_FAIL(WebView::HSTSStore::create(database));
    VERIFY(hsts_store->is_known_hsts_host("example.test"_string));
}

TEST_CASE(closing_a_page_drops_its_web_ui)
{
    TestEnvironment environment;
    auto app = TRY_OR_FAIL(environment.create_application());
    auto theme = TRY_OR_FAIL(load_test_theme());
    auto view = WebView::HeadlessWebView::create(theme, { 1000, 700 });
    auto& page = view->page();
    auto web_ui = TRY_OR_FAIL(WebView::WebUI::create(page.client(), page.id(), "settings"_string));
    VERIFY(web_ui);
    page.set_web_ui(move(web_ui));
    EXPECT(page.web_ui());

    page.close();
    EXPECT(!page.web_ui());
}

TEST_CASE(application_destruction_with_pending_web_ui_callbacks)
{
    TestEnvironment environment;
    struct WebUILifetime {
        WeakPtr<WebView::WebContentClient> client;
        WeakPtr<WebView::WebUI> web_ui;
        bool retained_by_callback { false };
    };
    Vector<WebUILifetime> web_ui_lifetimes;

    {
        auto app = TRY_OR_FAIL(environment.create_application());
        auto theme = TRY_OR_FAIL(load_test_theme());
        for (auto host : { "settings"sv, "bookmarks"sv, "downloads"sv }) {
            for (bool retained_by_callback : { false, true }) {
                auto view = WebView::HeadlessWebView::create(theme, { 1000, 700 });
                auto& page = view->page();
                auto web_ui = TRY_OR_FAIL(WebView::WebUI::create(page.client(), page.id(), MUST(String::from_utf8(host))));
                VERIFY(web_ui);
                web_ui_lifetimes.append({ page.client(), web_ui, retained_by_callback });
                page.set_web_ui(move(web_ui));

                if (retained_by_callback) {
                    // EventReceiver::deferred_invoke() retains its receiver until the callback is run or discarded.
                    page.client().deferred_invoke([] { });
                    page.web_ui()->deferred_invoke([] { });
                }
            }
        }
    }

    VERIFY(!WebView::Application::is_initialized());
    for (auto const& lifetime : web_ui_lifetimes) {
        EXPECT_EQ(static_cast<bool>(lifetime.client), lifetime.retained_by_callback);
        EXPECT_EQ(static_cast<bool>(lifetime.web_ui), lifetime.retained_by_callback);
    }

    // The Qt event loop discards queued callbacks after the application has been destroyed.
    Core::ThreadEventQueue::destroy_current();
    for (auto const& lifetime : web_ui_lifetimes) {
        EXPECT(!lifetime.client);
        EXPECT(!lifetime.web_ui);
    }
}

TEST_CASE(test_mode_uses_a_fresh_profile_and_removes_it_on_destruction)
{
    TestEnvironment environment;
    ByteString user_config_path;
    {
        auto app = TRY_OR_FAIL(environment.create_application(false));
        EXPECT(!app->profile().is_temporary());
        user_config_path = app->profile().paths().config;
        app->settings().set_languages({ "fr"_string });
        app->settings().set_content_blocker_enabled(false);
        app->settings().set_custom_content_blocker_filters("example.com##.advertisement"_string);
    }

    ByteString first_test_root;
    {
        auto app = TRY_OR_FAIL(environment.create_application());
        EXPECT(app->profile().is_temporary());
        EXPECT_NE(app->profile().paths().config, user_config_path);
        first_test_root = app->profile().paths().identity;
        EXPECT(FileSystem::is_directory(first_test_root));
        EXPECT_NE(app->settings().languages().first(), "fr"sv);
        EXPECT(app->settings().content_blocker_enabled());
        EXPECT(app->settings().custom_content_blocker_filters().is_empty());
        EXPECT(!app->has_content_blocking_rules());

        // Changes made during a test must not affect the next run or the user's profile.
        app->settings().set_languages({ "de"_string });
        app->settings().set_content_blocker_enabled(false);
        EXPECT(!app->content_blocking_enabled());
        app->settings().set_custom_content_blocker_filters("example.com##.test-advertisement"_string);
        EXPECT(app->has_content_blocking_rules());
    }
    EXPECT(!FileSystem::exists(first_test_root));

    ByteString second_test_root;
    {
        auto app = TRY_OR_FAIL(environment.create_application());
        second_test_root = app->profile().paths().identity;
        EXPECT_NE(first_test_root, second_test_root);
        EXPECT_NE(app->settings().languages().first(), "de"sv);
        EXPECT(app->settings().content_blocker_enabled());
        EXPECT(!app->has_content_blocking_rules());
    }
    EXPECT(!FileSystem::exists(second_test_root));

    auto user_settings = WebView::Settings::create(LexicalPath::join(user_config_path, "Settings.json"sv).string());
    EXPECT_EQ(user_settings.languages().first(), "fr"sv);
    EXPECT(!user_settings.content_blocker_enabled());
    EXPECT_EQ(user_settings.custom_content_blocker_filters(), "example.com##.advertisement"sv);
}
