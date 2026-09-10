/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/FontService.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

Messages::WebWorkerClient::OpenSystemFontResponse WebWorkerClient::open_system_font(u64 generation, u64 face_id)
{
    auto font = Application::font_service().open_font(generation, face_id);
    return { font.face_id, font.ttc_index, to_underlying(font.format), move(font.file) };
}

Messages::WebWorkerClient::MatchSystemFontResponse WebWorkerClient::match_system_font(String family, u16 weight, u16 width, u8 slope)
{
    auto font = Application::font_service().match_font(family, weight, width, slope);
    return { font.face_id, font.ttc_index, to_underlying(font.format), move(font.file) };
}

Messages::WebWorkerClient::MatchSystemFontForCodePointResponse WebWorkerClient::match_system_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    auto font = Application::font_service().match_font_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
    return { font.face_id, font.ttc_index, to_underlying(font.format), move(font.file) };
}

Messages::WebWorkerClient::ResolveGenericFontResponse WebWorkerClient::resolve_generic_font(String family, u16 weight, u8 slope)
{
    auto resolved = Application::font_service().resolve_generic_family(family, weight, slope);
    if (!resolved.has_value())
        return Optional<String> {};
    return Optional<String> { resolved->to_string() };
}

WebWorkerClient::WebWorkerClient(NonnullOwnPtr<IPC::Transport> transport, IsPrivate is_private, Web::HTML::WorkerAgentId agent_id)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
    , m_is_private(is_private)
    , m_agent_id(agent_id)
{
    if (auto session = Application::existing_session(is_private))
        m_session = session->make_weak_ptr();
}

WebWorkerClient::~WebWorkerClient()
{
    remove_blob_url_entries();
}

void WebWorkerClient::die()
{
    remove_blob_url_entries();
    WorkerProcessManager::the().worker_did_die(m_agent_id);

    // Otherwise nested workers we own would outlive us, in violation of the HTML spec.
    WorkerProcessManager::the().remove_web_worker_owner(*this);
}

void WebWorkerClient::did_close_worker()
{
    WorkerProcessManager::the().worker_did_close(m_agent_id);
}

void WebWorkerClient::did_finish_loading_worker_script(bool worker_is_secure_context)
{
    WorkerProcessManager::the().worker_did_finish_loading_script(m_agent_id, worker_is_secure_context);
}

void WebWorkerClient::did_fail_loading_worker_script()
{
    WorkerProcessManager::the().worker_did_fail_loading_script(m_agent_id);
}

void WebWorkerClient::did_report_worker_exception(Utf16String message, Utf16String filename, u32 lineno, u32 colno)
{
    WorkerProcessManager::the().worker_did_report_exception(m_agent_id, move(message), move(filename), lineno, colno);
}

void WebWorkerClient::remove_blob_url_entries()
{
    if (auto session = m_session.strong_ref())
        session->blob_url_store->remove_entries_added_by(WeakPtr<WebWorkerClient> { *this });
}

Messages::WebWorkerClient::DidRequestCookieResponse WebWorkerClient::did_request_cookie(URL::URL url, HTTP::Cookie::Source source)
{
    HTTP::Cookie::VersionedCookie cookie;
    if (auto session = m_session.strong_ref())
        cookie.cookie = session->cookie_jar->get_cookie(url, source);
    return cookie;
}

Messages::WebWorkerClient::DidAddBlobUrlEntryResponse WebWorkerClient::did_add_blob_url_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry)
{
    auto session = m_session.strong_ref();
    if (!session)
        return 0;
    return session->blob_url_store->add_entry(move(url), move(entry), WeakPtr<WebWorkerClient> { *this });
}

void WebWorkerClient::did_remove_blob_url_entries(Vector<Utf16String> urls, URL::Origin origin)
{
    if (auto session = m_session.strong_ref())
        session->blob_url_store->remove_entries(urls, origin, WeakPtr<WebWorkerClient> { *this });
}

Messages::WebWorkerClient::DidRequestBlobUrlEntryResponse WebWorkerClient::did_request_blob_url_entry(Utf16String url, Optional<URL::BlobURLEntry::Token> token)
{
    auto session = m_session.strong_ref();
    if (!session)
        return Optional<Web::FileAPI::SerializedBlobURLEntry> {};
    return session->blob_url_store->resolve(url, token);
}

void WebWorkerClient::did_request_file(ByteString path, i32 request_id)
{
    WorkerProcessManager::the().worker_did_request_file(m_agent_id, move(path), request_id);
}

void WebWorkerClient::did_store_hsts_policy(String domain, HTTP::HSTS::ParsedHSTSPolicy policy)
{
    if (auto session = m_session.strong_ref())
        session->hsts_store->store_policy(domain, policy);
}

Messages::WebWorkerClient::DidIsKnownHstsHostResponse WebWorkerClient::did_is_known_hsts_host(String domain)
{
    auto session = m_session.strong_ref();
    return session ? session->hsts_store->is_known_hsts_host(domain) : false;
}

void WebWorkerClient::did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage message)
{
    WorkerProcessManager::the().worker_did_post_broadcast_channel_message(m_agent_id, move(message));
}

Messages::WebWorkerClient::StartWorkerAgentResponse WebWorkerClient::start_worker_agent(Web::HTML::WorkerAgentStartRequest request)
{
    return WorkerProcessManager::the().start_worker_agent(*this, move(request));
}

void WebWorkerClient::close_worker_agent(Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    WorkerProcessManager::the().close_worker_agent(*this, agent_id, owner_token);
}

}
