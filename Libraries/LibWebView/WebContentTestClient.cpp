/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CookieJar.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebContentTestClient.h>

namespace WebView {

WebContentTestClient::WebContentTestClient(NonnullOwnPtr<IPC::Transport> transport, WebContentClient& client)
    : WebContentTestClientPageRoutingStub(*this, move(transport))
    , m_client(client)
{
}

WebContentTestClient::~WebContentTestClient() = default;

// This transport is separate, so dispatch what the process sent on its main connection before anything from here.
ErrorOr<OwnPtr<IPC::MessageBuffer>> WebContentTestClient::handle(NonnullOwnPtr<IPC::Message> message)
{
    m_client.dispatch_pending_messages();
    return WebContentTestClientEndpoint::Stub::handle(move(message));
}

void WebContentTestClient::die()
{
    // The WebContent process going away is handled by the main connection.
}

WebContentTestClientPageStub* WebContentTestClient::page_stub(Compositing::PageId const& page_id)
{
    return m_client.page(page_id);
}

void WebContentTestClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    m_client.session().cookie_jar->expire_cookies_with_time_offset(offset);
}

void WebContentTestClient::did_store_hsts_policy_for_testing(String domain, HTTP::HSTS::ParsedHSTSPolicy policy)
{
    m_client.session().hsts_store->store_policy(domain, policy);
}

Messages::WebContentTestClient::DidRequestUiProcessSessionHistoryForTestingResponse WebContentTestClient::did_request_ui_process_session_history_for_testing(Compositing::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_ui_process_session_history_for_testing();

    return String {};
}

Messages::WebContentTestClient::DidRequestSiteIsolationProcessTreeForTestingResponse WebContentTestClient::did_request_site_isolation_process_tree_for_testing(Compositing::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_site_isolation_process_tree_for_testing();

    return String {};
}

Messages::WebContentTestClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse WebContentTestClient::did_request_capture_session_history_snapshot_for_testing(Compositing::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_capture_session_history_snapshot_for_testing();

    return false;
}

Messages::WebContentTestClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse WebContentTestClient::did_request_restore_session_history_snapshot_for_testing(Compositing::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_restore_session_history_snapshot_for_testing();

    return false;
}

Messages::WebContentTestClient::DidRequestRegisterSessionStoreTabForTestingResponse WebContentTestClient::did_request_register_session_store_tab_for_testing(Compositing::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_register_session_store_tab_for_testing();

    return false;
}

Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse WebContentTestClient::did_request_session_store_tab_state_for_testing(Compositing::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_session_store_tab_state_for_testing();

    return String {};
}

}
