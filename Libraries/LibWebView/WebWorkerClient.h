/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/TransportHandle.h>
#include <LibMediaClient/Client.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/HTML/BroadcastChannelMessage.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/RequestServerSiteBindings.h>
#include <WebContent/WebWorkerClientEndpoint.h>
#include <WebContent/WebWorkerServerEndpoint.h>

namespace WebView {

class WEBVIEW_API WebWorkerClient final
    : public IPC::ConnectionToServer<WebWorkerClientEndpoint, WebWorkerServerEndpoint>
    , public WebWorkerClientEndpoint {
    C_OBJECT_ABSTRACT(WebWorkerClient);

public:
    WebWorkerClient(NonnullOwnPtr<IPC::Transport>, IsPrivate, Web::HTML::WorkerAgentId agent_id);
    ~WebWorkerClient();

    RefPtr<BrowsingSession> session() const { return m_session.strong_ref(); }
    IsPrivate is_private() const { return m_is_private; }

    // The sites this worker asked for the permissions of: at most its top-level site.
    Vector<URL::Origin> const& permission_origins() const { return m_permission_origins; }
    RequestServerSiteBindings& request_server_site_bindings() { return m_request_server_site_bindings; }
    void remove_blob_url_entries();

    // The process the worker's thread runs in.
    u64 child_id() const { return m_host_child_id; }

    // The WebContent process that runs this worker on a thread.
    WebContentClient* host() const { return m_host.ptr(); }
    void set_host(WebContentClient&);

    Optional<CanonicalEnvironmentSettingsObject const&> hosted_environment(Web::HTML::EnvironmentId const& environment_id) const;

    void set_inside_settings(NonnullOwnPtr<CanonicalWorkerEnvironmentSettingsObject>, bool may_read_local_files);
    CanonicalWorkerEnvironmentSettingsObject const* inside_settings() const { return m_inside_settings.ptr(); }
    void forget_inside_settings_of_closing_worker();
    Optional<URL::Origin> const& top_level_origin() const { return m_top_level_origin; }
    bool may_read_local_files() const { return m_may_read_local_files; }
    void keep_script_blob_url_until_script_loads(BlobURLHandle handle) { m_script_blob_url = move(handle); }

    virtual void did_close_worker() override;
    virtual void did_close_shared_worker() override;
    virtual void did_finish_loading_worker_script(bool worker_is_secure_context) override;
    virtual void did_fail_loading_worker_script() override;
    virtual void did_report_outside_settings_content_security_policy_violation(u64 policy_index, String effective_directive, URL::URL resource) override;
    virtual Messages::WebWorkerClient::DidRequestCookieResponse did_request_cookie(URL::URL, HTTP::Cookie::Source) override;
    virtual Messages::WebWorkerClient::DidAddBlobUrlEntryResponse did_add_blob_url_entry(Web::HTML::EnvironmentId environment_id, Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry) override;
    virtual void did_remove_blob_url_entries(Web::HTML::EnvironmentId environment_id, Vector<Utf16String> urls) override;
    virtual Messages::WebWorkerClient::DidRequestBlobUrlEntryResponse did_request_blob_url_entry(Utf16String url, Optional<URL::BlobURLEntry::Token> token) override;
    virtual void did_request_file(ByteString path, i32 request_id) override;
    virtual Messages::WebWorkerClient::DidIsKnownHstsHostResponse did_is_known_hsts_host(String domain) override;
    virtual Messages::WebWorkerClient::DidRequestPermissionSnapshotResponse did_request_permission_snapshot(URL::Origin top_level_origin) override;
    virtual Messages::WebWorkerClient::RequestMediaServerConnectionResponse request_media_server_connection() override;
    virtual Messages::WebWorkerClient::RequestImageDecoderConnectionResponse request_image_decoder_connection() override;
    virtual void did_release_image_decoder() override;
    virtual Messages::WebWorkerClient::RequestCompositorConnectionResponse request_compositor_connection() override;
    virtual void did_post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage) override;
    virtual void register_dedicated_worker(Web::HTML::WorkerAgentStartRequest request, IPC::TransportHandle worker_transport) override;

private:
    virtual void did_misbehave(StringView message_name, StringView reason) override;

    virtual void die() override;

    IsPrivate m_is_private { IsPrivate::No };
    WeakPtr<BrowsingSession> m_session;
    RequestServerSiteBindings m_request_server_site_bindings;

    u64 m_host_child_id { 0 };
    WeakPtr<WebContentClient> m_host;
    Optional<u64> m_image_decoder_client_generation;
    Web::HTML::WorkerAgentId m_agent_id { 0 };
    Vector<URL::Origin> m_permission_origins;

    OwnPtr<CanonicalWorkerEnvironmentSettingsObject> m_inside_settings;
    Optional<URL::Origin> m_top_level_origin;
    bool m_may_read_local_files { false };
    // Keeps the blob URL entry of the worker's script alive until the script is loaded, so that the owner can
    // revoke the URL right after constructing the worker.
    BlobURLHandle m_script_blob_url;

    // The controller connections to the MediaServer and AudioServer spawned for this worker, from its first media use
    // until it ends.
    RefPtr<MediaClient::Client> m_media_server_client;
    RefPtr<AudioServerControlClient> m_audio_server_client;
    u64 m_audio_tab_id { 0 };
};

}
