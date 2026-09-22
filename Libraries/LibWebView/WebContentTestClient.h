/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/PageId.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Forward.h>
#include <WebContent/WebContentTestClientEndpoint.h>
#include <WebContent/WebContentTestServerEndpoint.h>

namespace WebView {

// The test-only half of the WebContent protocol. The UI process connects this second transport only when it runs
// tests, so a WebContent process serving a real browsing session cannot send these messages at all.
class WEBVIEW_API WebContentTestClient final
    : public WebContentTestClientPageRoutingStub<IPC::ConnectionToServer<WebContentTestClientEndpoint, WebContentTestServerEndpoint>>
    , public WebContentTestClientEndpoint {
    C_OBJECT_ABSTRACT(WebContentTestClient);

public:
    WebContentTestClient(NonnullOwnPtr<IPC::Transport>, WebContentClient&);
    ~WebContentTestClient();

private:
    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle(NonnullOwnPtr<IPC::Message>) override;

    virtual void die() override;

    virtual WebContentTestClientPageStub* page_stub(Compositing::PageId const&) override;
    virtual void did_expire_cookies_with_time_offset(AK::Duration) override;
    virtual void did_store_hsts_policy_for_testing(String domain, HTTP::HSTS::ParsedHSTSPolicy) override;
    virtual Messages::WebContentTestClient::DidRequestUiProcessSessionHistoryForTestingResponse did_request_ui_process_session_history_for_testing(Compositing::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestSiteIsolationProcessTreeForTestingResponse did_request_site_isolation_process_tree_for_testing(Compositing::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse did_request_capture_session_history_snapshot_for_testing(Compositing::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse did_request_restore_session_history_snapshot_for_testing(Compositing::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestRegisterSessionStoreTabForTestingResponse did_request_register_session_store_tab_for_testing(Compositing::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse did_request_session_store_tab_state_for_testing(Compositing::PageId page_id) override;

    WebContentClient& m_client;
};

}
