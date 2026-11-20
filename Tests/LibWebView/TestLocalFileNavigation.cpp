/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/LocalFileRoots.h>
#include <LibWebView/Utilities.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }
};

}

// The UI process reads the file a file: navigation names only if it's under the local file roots, whoever started the
// navigation. Anywhere else, the navigation gets an error document, as it would for a file that isn't there.

TEST_CASE(local_file_navigation)
{
    Test::HeadlessTestEnvironment environment { "TestLocalFileNavigation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    // The test's own directory takes the place of the usual roots, so that a file next to it is outside them.
    auto directory = TRY_OR_FAIL(FileSystem::real_path(environment.directory()));
    auto inside = LexicalPath::join(directory, "inside"sv).string();
    auto outside = LexicalPath::join(directory, "outside"sv).string();
    TRY_OR_FAIL(Core::Directory::create(inside, Core::Directory::CreateDirectories::Yes));
    TRY_OR_FAIL(Core::Directory::create(outside, Core::Directory::CreateDirectories::Yes));
    auto usual_roots = WebView::LocalFileRoots::replace_for_testing({ inside });
    ScopeGuard restore_usual_roots = [&] { WebView::LocalFileRoots::replace_for_testing(move(usual_roots)); };

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

    view->load(start_page_url);
    wait_for_load_of(start_page_url);
    VERIFY(title == "start"sv);

    // A navigation the page starts, to a file outside the roots, gets an error document.
    view->run_javascript(MUST(String::formatted("location.href = '{}';", outside_page_url.serialize())));
    wait_for_load_of(outside_page_url);
    if (title == "elsewhere"sv)
        FAIL("A page navigated to a file outside the roots, and got its content");

    // So does one the browser starts.
    view->load(start_page_url);
    wait_for_load_of(start_page_url);
    view->load(outside_page_url);
    wait_for_load_of(outside_page_url);
    if (title == "elsewhere"sv)
        FAIL("The browser navigated to a file outside the roots, and got its content");

    // A file under the roots is one a page navigates to.
    view->load(start_page_url);
    wait_for_load_of(start_page_url);
    view->run_javascript("location.href = 'next.html';"_string);
    wait_for_load_of(next_page_url);
    VERIFY(title == "next"sv);
}
