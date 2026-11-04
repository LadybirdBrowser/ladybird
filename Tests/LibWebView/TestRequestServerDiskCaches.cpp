/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/RequestServerManager.h>

// A RequestServer is sandboxed to the disk cache it opens, so only the one of a site in a normal session may have one.
TEST_CASE(only_sites_of_normal_sessions_have_disk_caches)
{
    Test::HeadlessTestEnvironment environment { "TestRequestServerDiskCaches"sv };
    auto app = TRY_OR_FAIL(environment.create_application());

    auto& manager = WebView::Application::request_server_manager();
    auto& normal_session = WebView::Application::default_session();
    auto private_session = WebView::Application::session_for_new_view(WebView::IsPrivate::Yes);

    auto site_server = TRY_OR_FAIL(manager.instance_for_site(normal_session, "https://normal.example"_utf16));
    EXPECT(FileSystem::is_directory(LexicalPath::join(site_server->cache_path(), "Cache"sv).string()));

    // A directory named after the site would reveal that the site was visited in a private session.
    auto private_site_server = TRY_OR_FAIL(manager.instance_for_site(*private_session, "https://private.example"_utf16));
    EXPECT(!FileSystem::exists(private_site_server->cache_path()));

    auto browser_server = TRY_OR_FAIL(manager.browser_instance(normal_session));
    auto shared_cache = LexicalPath::join(browser_server->cache_path(), "Cache"sv).string();
    EXPECT(!FileSystem::exists(shared_cache));

    // A profile from before sites had RequestServers of their own has a shared cache that nothing uses any more.
    TRY_OR_FAIL(Core::Directory::create(shared_cache, Core::Directory::CreateDirectories::Yes));
    WebView::RequestServerManager new_manager;
    EXPECT(!FileSystem::exists(shared_cache));
}

// A RequestServer can ask whether any path exists, so it must not be able to work out where other sites' caches are.
TEST_CASE(site_cache_directories_cannot_be_named_without_the_key)
{
    Test::HeadlessTestEnvironment environment { "TestRequestServerDiskCaches"sv };
    auto app = TRY_OR_FAIL(environment.create_application());

    auto& manager = WebView::Application::request_server_manager();
    auto& normal_session = WebView::Application::default_session();
    auto private_session = WebView::Application::session_for_new_view(WebView::IsPrivate::Yes);
    auto site = "https://normal.example"_utf16;

    auto site_server = TRY_OR_FAIL(manager.instance_for_site(normal_session, site));
    auto cache_path = LexicalPath { site_server->cache_path() };
    EXPECT(!cache_path.basename().contains("normal"sv));

    auto other_site_server = TRY_OR_FAIL(manager.instance_for_site(normal_session, "https://other.example"_utf16));
    EXPECT_NE(other_site_server->cache_path(), cache_path.string());

    // A site finds its cache again after a restart. A private session's RequestServer keeps this one from opening it.
    {
        WebView::RequestServerManager restarted_manager;
        auto restarted_server = TRY_OR_FAIL(restarted_manager.instance_for_site(*private_session, site));
        EXPECT_EQ(restarted_server->cache_path(), cache_path.string());
    }

    // Directories named by older versions leak the site, and nothing can find them again once the key is gone.
    auto sites_directory = cache_path.parent();
    auto readable_cache = sites_directory.append("https___normal.example-12345678"sv).string();
    TRY_OR_FAIL(Core::Directory::create(readable_cache, Core::Directory::CreateDirectories::Yes));
    TRY_OR_FAIL(FileSystem::remove(sites_directory.parent().append("SiteCacheKey"sv).string(), FileSystem::RecursionMode::Disallowed));

    WebView::RequestServerManager new_manager;
    EXPECT(!FileSystem::exists(readable_cache));
    EXPECT(FileSystem::exists(sites_directory.parent().append("SiteCacheKey"sv).string()));
}
