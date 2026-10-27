/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionToServer.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Forward.h>
#include <WebContent/WebContentTestClientEndpoint.h>
#include <WebContent/WebContentTestServerEndpoint.h>

namespace WebView {

// The test-only half of the WebContent protocol. The UI process connects this second transport only when it runs
// tests, so a WebContent process serving a real browsing session cannot send these messages at all.
class WEBVIEW_API WebContentTestClient final
    : public IPC::ConnectionToServer<WebContentTestClientEndpoint, WebContentTestServerEndpoint, WebContentTestClientPageRoutingStub>
    , public WebContentTestClientEndpoint {
    C_OBJECT_ABSTRACT(WebContentTestClient);

public:
    WebContentTestClient(NonnullOwnPtr<IPC::Transport>, WebContentClient&);
    ~WebContentTestClient();

private:
    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle(NonnullOwnPtr<IPC::Message>) override;

    virtual void die() override;

    virtual WebContentTestClientPageStub* page_stub(Web::PageId const&) override;
    virtual void did_expire_cookies_with_time_offset(AK::Duration) override;
    virtual void did_spoof_document_origin_for_testing(Web::PageId, Web::HTML::EnvironmentId environment_id, URL::Origin) override;
    virtual void did_store_hsts_policy_for_testing(String domain, HTTP::HSTS::ParsedHSTSPolicy) override;
    virtual void did_request_clear_storage_for_testing(Web::PageId, Web::StorageAPI::StorageEndpointType, Web::HTML::EnvironmentId environment_id) override;
    virtual Messages::WebContentTestClient::DidRequestUiProcessSessionHistoryForTestingResponse did_request_ui_process_session_history_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestSiteIsolationProcessTreeForTestingResponse did_request_site_isolation_process_tree_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestTabIsPlayingAudioForTestingResponse did_request_tab_is_playing_audio_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestTabAudibilityChangeCountForTestingResponse did_request_tab_audibility_change_count_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestHasPopulatedDocumentForTestingResponse did_request_has_populated_document_for_testing(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual Messages::WebContentTestClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse did_request_capture_session_history_snapshot_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse did_request_restore_session_history_snapshot_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestRegisterSessionStoreTabForTestingResponse did_request_register_session_store_tab_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse did_request_session_store_tab_state_for_testing(Web::PageId page_id) override;
    virtual Messages::WebContentTestClient::SetPermissionForTestingResponse set_permission_for_testing(Web::PageId, Web::HTML::CrossProcessId document_id, String name, String state) override;
    virtual void set_next_permission_prompt_outcome_for_testing(Web::PageId, String name, String outcome, bool held) override;
    virtual void release_held_permission_prompt_for_testing(Web::PageId, u32 waiters) override;
    virtual Messages::WebContentTestClient::AdvancePermissionClockForTestingResponse advance_permission_clock_for_testing(Web::PageId, u64 milliseconds) override;
    virtual Messages::WebContentTestClient::CreateVirtualGamepadResponse create_virtual_gamepad() override;
    virtual void set_virtual_gamepad_button(Web::Gamepad::GamepadHandle handle, i32 button, bool down) override;
    virtual void set_virtual_gamepad_axis(Web::Gamepad::GamepadHandle handle, i32 axis, i16 value) override;
    virtual void disconnect_virtual_gamepad(Web::Gamepad::GamepadHandle handle) override;
    virtual Messages::WebContentTestClient::PumpGamepadEventsResponse pump_gamepad_events() override;
    virtual Messages::WebContentTestClient::GetVirtualGamepadReceivedRumbleEffectsResponse get_virtual_gamepad_received_rumble_effects(Web::Gamepad::GamepadHandle handle) override;

    WebContentClient& m_client;
};

}
