/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CookieJar.h>
#include <LibWebView/GamepadManager.h>
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

WebContentTestClientPageStub* WebContentTestClient::page_stub(Web::PageId const& page_id)
{
    return m_client.page(page_id);
}

void WebContentTestClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    m_client.session().cookie_jar->expire_cookies_with_time_offset(offset);
}

void WebContentTestClient::did_spoof_document_origin_for_testing(Web::PageId page_id, Web::HTML::EnvironmentId environment_id, URL::Origin origin)
{
    if (auto* page = m_client.page(page_id))
        page->spoof_document_origin_for_testing(environment_id, move(origin));
}

void WebContentTestClient::did_store_hsts_policy_for_testing(String domain, HTTP::HSTS::ParsedHSTSPolicy policy)
{
    m_client.session().hsts_store->store_policy(domain, policy);
}

Messages::WebContentTestClient::DidRequestUiProcessSessionHistoryForTestingResponse WebContentTestClient::did_request_ui_process_session_history_for_testing(Web::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_ui_process_session_history_for_testing();

    return String {};
}

Messages::WebContentTestClient::DidRequestSiteIsolationProcessTreeForTestingResponse WebContentTestClient::did_request_site_isolation_process_tree_for_testing(Web::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_site_isolation_process_tree_for_testing();

    return String {};
}

Messages::WebContentTestClient::DidRequestHasPopulatedDocumentForTestingResponse WebContentTestClient::did_request_has_populated_document_for_testing(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_has_populated_document_for_testing(navigable_id);

    return false;
}

Messages::WebContentTestClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse WebContentTestClient::did_request_capture_session_history_snapshot_for_testing(Web::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_capture_session_history_snapshot_for_testing();

    return false;
}

Messages::WebContentTestClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse WebContentTestClient::did_request_restore_session_history_snapshot_for_testing(Web::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_restore_session_history_snapshot_for_testing();

    return false;
}

Messages::WebContentTestClient::DidRequestRegisterSessionStoreTabForTestingResponse WebContentTestClient::did_request_register_session_store_tab_for_testing(Web::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_register_session_store_tab_for_testing();

    return false;
}

Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse WebContentTestClient::did_request_session_store_tab_state_for_testing(Web::PageId page_id)
{
    if (auto* page = m_client.page(page_id))
        return page->did_request_session_store_tab_state_for_testing();

    return String {};
}

Messages::WebContentTestClient::CreateVirtualGamepadResponse WebContentTestClient::create_virtual_gamepad()
{
    return GamepadManager::the().create_virtual_gamepad(m_client);
}

void WebContentTestClient::set_virtual_gamepad_button(Web::Gamepad::GamepadHandle handle, i32 button, bool down)
{
    GamepadManager::the().set_virtual_gamepad_button(m_client, handle, button, down);
}

void WebContentTestClient::set_virtual_gamepad_axis(Web::Gamepad::GamepadHandle handle, i32 axis, i16 value)
{
    GamepadManager::the().set_virtual_gamepad_axis(m_client, handle, axis, value);
}

void WebContentTestClient::disconnect_virtual_gamepad(Web::Gamepad::GamepadHandle handle)
{
    GamepadManager::the().disconnect_virtual_gamepad(m_client, handle);
}

Messages::WebContentTestClient::PumpGamepadEventsResponse WebContentTestClient::pump_gamepad_events()
{
    return GamepadManager::the().pump_gamepad_events(m_client);
}

Messages::WebContentTestClient::GetVirtualGamepadReceivedRumbleEffectsResponse WebContentTestClient::get_virtual_gamepad_received_rumble_effects(Web::Gamepad::GamepadHandle handle)
{
    auto& gamepad_manager = GamepadManager::the();
    return { gamepad_manager.virtual_gamepad_received_rumble_effects(m_client, handle), gamepad_manager.virtual_gamepad_received_trigger_rumble_effects(m_client, handle) };
}

}
