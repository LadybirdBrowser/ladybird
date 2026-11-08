/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/AudioServerControlClient.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/ImageDecoderManager.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>
#include <LibWebView/WorkerAgentManager.h>

namespace WebView {

WebWorkerClient::WebWorkerClient(NonnullOwnPtr<IPC::Transport> transport, IsPrivate is_private, Web::HTML::WorkerAgentId agent_id)
    : IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport))
    , m_is_private(is_private)
    , m_request_server_site_bindings([this](RequestServerClientConnection& connection, Optional<Utf16String> const& top_level_site) {
        async_connect_to_request_server(move(connection.handle), connection.client_id, top_level_site);
    })
    , m_agent_id(agent_id)
    , m_audio_tab_id(allocate_audio_tab_id())
{
    if (auto session = Application::existing_session(is_private))
        m_session = session->make_weak_ptr();
}

WebWorkerClient::~WebWorkerClient()
{
    remove_blob_url_entries();
}

void WebWorkerClient::set_host(WebContentClient& host)
{
    m_host = host;
    m_host_child_id = host.child_id();
}

void WebWorkerClient::did_misbehave(StringView message_name, StringView reason)
{
    // NB: A worker on a thread is one trust domain with its process, so all of that goes.
    if (auto host = m_host.strong_ref()) {
        host->did_misbehave(message_name, reason);
        shutdown();
        return;
    }
    shutdown();
}

void WebWorkerClient::die()
{
    remove_blob_url_entries();
    WorkerAgentManager::the().worker_did_die(m_agent_id);

    // NB: The process of a worker on a thread goes on, and so may its use of the decoder.
    if (m_image_decoder_client_generation.has_value())
        ImageDecoderManager::the().process_did_release_decoder(child_id(), *m_image_decoder_client_generation);

    WorkerAgentManager::the().worker_thread_did_exit(*this);
}

void WebWorkerClient::did_close_worker()
{
    forget_inside_settings_of_closing_worker();
    WorkerAgentManager::the().worker_did_close(m_agent_id);
}

void WebWorkerClient::did_close_shared_worker()
{
    WorkerAgentManager::the().shared_worker_did_close(m_agent_id);
}

void WebWorkerClient::did_finish_loading_worker_script(bool worker_is_secure_context)
{
    m_script_blob_url = {};
    WorkerAgentManager::the().worker_did_finish_loading_script(m_agent_id, worker_is_secure_context);
}

void WebWorkerClient::did_fail_loading_worker_script()
{
    WorkerAgentManager::the().worker_did_fail_loading_script(m_agent_id);
}

void WebWorkerClient::did_report_outside_settings_content_security_policy_violation(u64 policy_index, String effective_directive, URL::URL resource)
{
    WorkerAgentManager::the().worker_did_report_outside_settings_content_security_policy_violation(m_agent_id, policy_index, move(effective_directive), move(resource));
}

void WebWorkerClient::remove_blob_url_entries()
{
    if (auto session = m_session.strong_ref())
        session->blob_url_store->remove_entries_added_by(WeakPtr<WebWorkerClient> { *this });
}

void WebWorkerClient::set_inside_settings(NonnullOwnPtr<CanonicalWorkerEnvironmentSettingsObject> inside_settings, bool may_read_local_files)
{
    m_top_level_origin = inside_settings->top_level_origin();
    m_inside_settings = move(inside_settings);
    m_may_read_local_files = may_read_local_files;
}

void WebWorkerClient::forget_inside_settings_of_closing_worker()
{
    m_inside_settings = nullptr;
    m_script_blob_url = {};
}

// The environment a worker names is its own.
Optional<CanonicalEnvironmentSettingsObject const&> WebWorkerClient::hosted_environment(Web::HTML::EnvironmentId const& environment_id) const
{
    if (!m_inside_settings || m_inside_settings->id() != environment_id)
        return {};
    return *m_inside_settings;
}

// A worker may learn the permissions only of its own top-level site; asking about any other gets just the defaults.
Messages::WebWorkerClient::DidRequestPermissionSnapshotResponse WebWorkerClient::did_request_permission_snapshot(URL::Origin top_level_origin)
{
    auto session = m_session.strong_ref();
    if (!session)
        return Web::Permissions::PermissionSnapshot {};

    auto worker_top_level_origin = m_inside_settings ? m_inside_settings->top_level_origin() : Optional<URL::Origin> {};
    if (worker_top_level_origin.has_value() && worker_top_level_origin->is_same_origin(top_level_origin) && !top_level_origin.is_opaque() && !m_permission_origins.contains_slow(top_level_origin))
        m_permission_origins.append(top_level_origin);
    return session->permission_manager->snapshot_for(m_permission_origins);
}

Messages::WebWorkerClient::DidRequestCookieResponse WebWorkerClient::did_request_cookie(URL::URL url, HTTP::Cookie::Source source)
{
    // RequestServer handles the cookies of HTTP requests itself, so a worker has no use for HttpOnly cookies.
    if (source == HTTP::Cookie::Source::Http) {
        did_misbehave("did_request_cookie"sv, "HTTP cookie source"sv);
        return HTTP::Cookie::VersionedCookie {};
    }

    if (!m_inside_settings || !m_inside_settings->may_use_cookies_of(url))
        return HTTP::Cookie::VersionedCookie {};

    auto top_level_site = network_isolation_top_level_site(*m_inside_settings);
    if (!top_level_site.has_value())
        return HTTP::Cookie::VersionedCookie {};

    HTTP::Cookie::PartitionContext partition_context { top_level_site.release_value(), m_inside_settings->has_cross_site_ancestor() };
    HTTP::Cookie::VersionedCookie cookie;
    if (auto session = m_session.strong_ref())
        cookie.cookie = session->cookie_jar->get_cookie(url, source, partition_context, HTTP::Cookie::non_http_same_site_context(partition_context));
    return cookie;
}

Messages::WebWorkerClient::DidAddBlobUrlEntryResponse WebWorkerClient::did_add_blob_url_entry(Web::HTML::EnvironmentId environment_id, Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry)
{
    auto session = m_session.strong_ref();
    auto environment = hosted_environment(environment_id);
    if (!session || !environment.has_value())
        return 0;
    entry.origin = environment->origin();
    return session->blob_url_store->add_entry(move(url), move(entry), WeakPtr<WebWorkerClient> { *this });
}

void WebWorkerClient::did_remove_blob_url_entries(Web::HTML::EnvironmentId environment_id, Vector<Utf16String> urls)
{
    auto environment = hosted_environment(environment_id);
    if (!environment.has_value())
        return;
    if (auto session = m_session.strong_ref())
        session->blob_url_store->remove_entries(urls, environment->origin(), WeakPtr<WebWorkerClient> { *this });
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
    if (!m_inside_settings)
        return;

    // NB: As a page, a worker reads a local file only through this broker, and only local file content may.
    if (!m_may_read_local_files) {
        async_handle_file_return(EACCES, {}, request_id);
        return;
    }

    auto file = open_local_file_for_renderer(path);
    if (file.is_error())
        async_handle_file_return(file.error().code(), {}, request_id);
    else
        async_handle_file_return(0, file.release_value(), request_id);
}

Messages::WebWorkerClient::DidIsKnownHstsHostResponse WebWorkerClient::did_is_known_hsts_host(String domain)
{
    auto session = m_session.strong_ref();
    return session ? session->hsts_store->is_known_hsts_host(domain) : false;
}

Messages::WebWorkerClient::RequestMediaServerConnectionResponse WebWorkerClient::request_media_server_connection()
{
    auto connection = connect_new_media_server_client(m_media_server_client, child_id(), [weak_self = make_weak_ptr<WebWorkerClient>()]() -> ErrorOr<IPC::TransportHandle> {
        auto self = weak_self.strong_ref();
        if (!self)
            return Error::from_string_literal("The worker is gone");
        TRY(ensure_audio_server_is_running(self->m_audio_server_client, self->child_id()));
        return connect_new_audio_server_client(*self->m_audio_server_client, self->m_audio_tab_id);
    });
    if (connection.is_error()) {
        warnln("Unable to connect a MediaServer client: {}", connection.error());
        return OptionalNone {};
    }
    return move(connection.value().handle);
}

Messages::WebWorkerClient::RequestImageDecoderConnectionResponse WebWorkerClient::request_image_decoder_connection()
{
    // NB: A thread is part of its process, and what the decoder of a process may see goes by all that the process hosts.
    auto host = m_host.strong_ref();
    if (!host)
        return Optional<IPC::TransportHandle> {};
    return host->connect_new_image_decoder_client(m_image_decoder_client_generation);
}

void WebWorkerClient::did_release_image_decoder()
{
    if (auto generation = exchange(m_image_decoder_client_generation, {}); generation.has_value())
        ImageDecoderManager::the().process_did_release_decoder(child_id(), *generation);
}

Messages::WebWorkerClient::RequestCompositorConnectionResponse WebWorkerClient::request_compositor_connection()
{
    auto handle = Application::the().connect_new_compositor_canvas_client();
    if (handle.is_error()) {
        warnln("Unable to connect a worker to the compositor: {}", handle.error());
        return Optional<IPC::TransportHandle> {};
    }
    return handle.release_value();
}

void WebWorkerClient::did_post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage message)
{
    if (auto settings = hosted_environment(message.environment_id); settings.has_value())
        WorkerAgentManager::the().post_broadcast_channel_message(move(message), *settings, *this, m_is_private);
}

void WebWorkerClient::register_dedicated_worker(Web::HTML::WorkerAgentStartRequest request, IPC::TransportHandle worker_transport)
{
    auto outside_settings = hosted_environment(request.outside_settings.id);
    WorkerAgentManager::the().register_dedicated_worker(*this, outside_settings, move(request), move(worker_transport));
}

}
