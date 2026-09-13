/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentTestClient.h>

namespace WebView {

WebContentTestClient::WebContentTestClient(NonnullOwnPtr<IPC::Transport> transport, WebContentClient& client)
    : IPC::ConnectionToServer<WebContentTestClientEndpoint, WebContentTestServerEndpoint>(*this, move(transport))
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

void WebContentTestClient::did_finish_test(u64 page_id, String text)
{
    m_client.did_finish_test(page_id, move(text));
}

void WebContentTestClient::did_set_test_timeout(u64 page_id, double milliseconds)
{
    m_client.did_set_test_timeout(page_id, milliseconds);
}

void WebContentTestClient::did_receive_reference_test_metadata(u64 page_id, JsonValue metadata)
{
    m_client.did_receive_reference_test_metadata(page_id, move(metadata));
}

void WebContentTestClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    m_client.did_expire_cookies_with_time_offset(offset);
}

void WebContentTestClient::did_simulate_worker_request_server_connection_loss(u64 page_id)
{
    m_client.did_simulate_worker_request_server_connection_loss(page_id);
}

Messages::WebContentTestClient::DidRequestUiProcessSessionHistoryForTestingResponse WebContentTestClient::did_request_ui_process_session_history_for_testing(u64 page_id)
{
    return m_client.did_request_ui_process_session_history_for_testing(page_id);
}

Messages::WebContentTestClient::DidRequestSiteIsolationProcessTreeForTestingResponse WebContentTestClient::did_request_site_isolation_process_tree_for_testing(u64 page_id)
{
    return m_client.did_request_site_isolation_process_tree_for_testing(page_id);
}

void WebContentTestClient::did_request_crash_of_remote_frame_processes_for_testing(u64 page_id)
{
    m_client.did_request_crash_of_remote_frame_processes_for_testing(page_id);
}

void WebContentTestClient::did_reset_session_history_for_testing(u64 page_id, Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    m_client.did_reset_session_history_for_testing(page_id, move(active_entry));
}

Messages::WebContentTestClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse WebContentTestClient::did_request_capture_session_history_snapshot_for_testing(u64 page_id)
{
    return m_client.did_request_capture_session_history_snapshot_for_testing(page_id);
}

Messages::WebContentTestClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse WebContentTestClient::did_request_restore_session_history_snapshot_for_testing(u64 page_id)
{
    return m_client.did_request_restore_session_history_snapshot_for_testing(page_id);
}

Messages::WebContentTestClient::DidRequestRegisterSessionStoreTabForTestingResponse WebContentTestClient::did_request_register_session_store_tab_for_testing(u64 page_id)
{
    return m_client.did_request_register_session_store_tab_for_testing(page_id);
}

Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse WebContentTestClient::did_request_session_store_tab_state_for_testing(u64 page_id)
{
    return m_client.did_request_session_store_tab_state_for_testing(page_id);
}

}
