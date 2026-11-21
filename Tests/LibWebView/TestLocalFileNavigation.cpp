/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.allow_popups = WebView::AllowPopups::Yes;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }
};

}

// A file the user opens grants its folder to the renderer that hosts it. A navigation the page starts reads a file only
// under that folder, and grants nothing, while one the browser starts reads the file the user chose wherever it is.
TEST_CASE(local_file_navigation)
{
    Test::HeadlessTestEnvironment environment { "TestLocalFileNavigation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto directory = TRY_OR_FAIL(FileSystem::real_path(environment.directory()));
    auto inside = LexicalPath::join(directory, "inside"sv).string();
    auto outside = LexicalPath::join(directory, "outside"sv).string();
    TRY_OR_FAIL(Core::Directory::create(inside, Core::Directory::CreateDirectories::Yes));
    TRY_OR_FAIL(Core::Directory::create(outside, Core::Directory::CreateDirectories::Yes));

    auto write_page = [&](StringView directory, StringView title) {
        auto path = LexicalPath::join(directory, ByteString::formatted("{}.html", title)).string();
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(ByteString::formatted("<title>{}</title>", title)));
        return URL::create_with_file_scheme(path).release_value();
    };
    auto start_page_url = write_page(inside, "start"sv);
    auto next_page_url = write_page(inside, "next"sv);
    auto outside_page_url = write_page(outside, "elsewhere"sv);

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Utf16String title;
    view->on_title_change = [&](auto const& new_title) { title = new_title; };
    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    auto view_is_at = [&](URL::URL const& url) { return view->url().serialize() == url.serialize(); };
    auto wait_for_load_of = [&](URL::URL const& url) {
        auto expected_loads = loads_finished + 1;
        Core::EventLoop::current().spin_until([&]() { return loads_finished >= expected_loads && view_is_at(url); });
    };

    view->load_from_browser_ui(start_page_url);
    wait_for_load_of(start_page_url);
    VERIFY(title == "start"sv);

    // A navigation the page starts, to a file outside the folder the user opened, gets an error document.
    view->run_javascript(MUST(String::formatted("location.href = '{}';", outside_page_url.serialize())));
    wait_for_load_of(outside_page_url);
    if (title == "elsewhere"sv)
        FAIL("A page navigated to a file outside its root, and got its content");

    // One to a file in that folder reads it.
    view->load_from_browser_ui(start_page_url);
    wait_for_load_of(start_page_url);
    view->run_javascript("location.href = 'next.html';"_string);
    wait_for_load_of(next_page_url);
    VERIFY(title == "next"sv);

    // The browser reads a file the user chose wherever it is.
    view->load_from_browser_ui(outside_page_url);
    wait_for_load_of(outside_page_url);
    VERIFY(title == "elsewhere"sv);

    // A page navigation from there still reads nothing in the first folder: the page's navigation grants nothing.
    view->run_javascript(MUST(String::formatted("location.href = '{}';", start_page_url.serialize())));
    wait_for_load_of(start_page_url);
    if (title == "start"sv)
        FAIL("A page navigated to a file outside its root, and got its content");
}

// A sandboxed srcdoc frame reads local files under the root of the file page that made it. The sandboxed documents of
// the file origin share a process within a browsing context group, but not across roots, even for related tabs.
TEST_CASE(sandboxed_frames_of_related_tabs_keep_their_roots_apart)
{
    Test::HeadlessTestEnvironment environment { "TestLocalFileNavigation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto directory = TRY_OR_FAIL(FileSystem::real_path(environment.directory()));
    auto write_page = [&](StringView folder) {
        auto folder_path = LexicalPath::join(directory, folder).string();
        MUST(Core::Directory::create(folder_path, Core::Directory::CreateDirectories::Yes));
        auto path = LexicalPath::join(folder_path, "frame.html"sv).string();
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted("<iframe sandbox srcdoc=\"sandboxed\"></iframe>"sv));
        return URL::create_with_file_scheme(path).release_value();
    };
    auto first_page_url = write_page("first"sv);
    auto second_page_url = write_page("second"sv);

    auto sandboxed_frame_process = [](WebView::HeadlessWebView& view) -> WebView::WebContentClient* {
        WebView::WebContentClient* process = nullptr;
        view.traversable().for_each_in_subtree([&](WebView::CanonicalNavigable& navigable) {
            if (auto host = navigable.active_document().host(); host && navigable.active_document().creation_url() == URL::about_srcdoc())
                process = &host->client();
            return IterationDecision::Break;
        });
        return process;
    };

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    view->load_from_browser_ui(first_page_url);
    Core::EventLoop::current().spin_until([&]() { return sandboxed_frame_process(*view) != nullptr; });
    auto& first_process = *sandboxed_frame_process(*view);

    OwnPtr<WebView::HeadlessWebView> popup;
    view->on_new_web_view = [&](auto, auto, WebView::CanonicalTraversable& traversable) {
        popup = WebView::HeadlessWebView::create_child(*view, traversable);
        return popup->handle();
    };
    view->run_javascript("window.open('about:blank')"_string);
    Core::EventLoop::current().spin_until([&]() { return popup != nullptr; });

    popup->load_from_browser_ui(second_page_url);
    Core::EventLoop::current().spin_until([&]() { return sandboxed_frame_process(*popup) != nullptr; });
    auto& second_process = *sandboxed_frame_process(*popup);

    EXPECT(&first_process != &second_process);
    EXPECT_EQ(first_process.local_file_roots().size(), 1uz);
    EXPECT_EQ(second_process.local_file_roots().size(), 1uz);
}

// Site isolation gives each root a process of its own, so a process refuses a second root rather than read under both.
TEST_CASE(a_process_reads_local_files_under_one_root)
{
    Test::HeadlessTestEnvironment environment { "TestLocalFileNavigation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto& process = view->page().client();

    WebView::LocalFileRoot first { "/first"sv, WebView::LocalFileRoot::Scope::Directory };
    WebView::LocalFileRoot second { "/second"sv, WebView::LocalFileRoot::Scope::Directory };
    TRY_OR_FAIL(process.add_local_file_root(first));
    TRY_OR_FAIL(process.add_local_file_root(first));
    EXPECT(process.add_local_file_root(second).is_error());
    EXPECT(process.local_file_roots() == Vector { first });
}
