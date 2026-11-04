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
