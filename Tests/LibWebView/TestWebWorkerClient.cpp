/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/WebWorkerClient.h>

TEST_CASE(every_host_is_a_known_hsts_host_to_a_worker_without_a_browsing_session)
{
    Core::EventLoop event_loop;

    RefPtr<WebView::BrowsingSession> session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    auto pair = MUST(IPC::Transport::create_paired());
    auto client = make_ref_counted<WebView::WebWorkerClient>(move(pair.local), *session, 1);
    session = nullptr;
    EXPECT(!client->session());
    EXPECT(client->did_is_known_hsts_host("example.test"_string).result());
}
