/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibWebView/Application.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

TEST_CASE(hsts_store_synchronized_on_destruction)
{
    auto test_directory_prefix = ByteString::formatted("{}/Ladybird-TestApplicationDestruction-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());

    auto config_directory = ByteString::formatted("{}-config", test_directory_prefix);
    auto data_directory = ByteString::formatted("{}-data", test_directory_prefix);

    TRY_OR_FAIL(Core::Directory::create(config_directory, Core::Directory::CreateDirectories::Yes));
    TRY_OR_FAIL(Core::Directory::create(data_directory, Core::Directory::CreateDirectories::Yes));

    ScopeGuard cleanup = [&] {
        MUST(FileSystem::remove(config_directory, FileSystem::RecursionMode::Allowed));
        MUST(FileSystem::remove(data_directory, FileSystem::RecursionMode::Allowed));
    };

    VERIFY(setenv("XDG_CONFIG_HOME", config_directory.characters(), 1) == 0);
    VERIFY(setenv("XDG_DATA_HOME", data_directory.characters(), 1) == 0);

    ByteString database_path;

    {
        auto executable = "ladybird"sv;

        Main::Arguments arguments {
            .argc = 0,
            .argv = nullptr,
            .strings = Span<StringView> { &executable, 1 }
        };

#if defined(LADYBIRD_BINARY_PATH)
        auto app = TRY_OR_FAIL(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
        auto app = TRY_OR_FAIL(TestApplication::create(arguments, OptionalNone {}));
#endif

        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
        auto view = WebView::HeadlessWebView::create(theme, { 1000, 700 });

        database_path = app->profile().paths().data;

        // Push a HSTS policy into the transient storage and then immediately destruct the application to test whether
        // it gets synchronized to the persistent storage.
        view->session().hsts_store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });
    }

    auto database = TRY_OR_FAIL(Database::Database::create(database_path, "Ladybird"sv));
    auto hsts_store = TRY_OR_FAIL(WebView::HSTSStore::create(database));

    VERIFY(hsts_store->is_known_hsts_host("example.test"_string));
}
