/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Utf16String.h>
#include <LibGC/Root.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/ScriptAbortHandle.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransformer.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/HTML/BroadcastChannelMessage.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>
#include <WebContent/WebWorkerClientEndpoint.h>
#include <WebContent/WebWorkerServerEndpoint.h>
#include <WebContent/Worker/Forward.h>
#include <WebContent/Worker/PageHost.h>

namespace WebWorker {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<WebWorkerClientEndpoint, WebWorkerServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    virtual ~ConnectionFromClient() override;

    virtual void die() override;
    virtual void did_lose_peer_during_sync_request() override;

    // Gives the agent of this thread its connections to other processes.
    void set_up_agent_services();

    // Ends the agent of this thread, in order. The connection outlives the heap, as cells in the heap use it until they
    // are gone.
    void shut_down_agent();

    virtual void close_worker() override;

    void request_file(Web::FileRequest);

    DedicatedWorkerThreadLink* dedicated_worker_thread_link() { return m_dedicated_worker_thread_link.ptr(); }
    NonnullRefPtr<Web::HTML::ScriptAbortHandle> script_abort_handle() const { return m_script_abort_handle; }

    PageHost& page_host() { return *m_page_host; }
    PageHost const& page_host() const { return *m_page_host; }

    Web::Compositor::CompositorConnection* compositor_process_connection();

    // Asks the Browser to spawn this worker's MediaServer if it has none, and to connect a client to it.
    ErrorOr<NonnullOwnPtr<IPC::Transport>> request_media_server_transport();
    // Asks the Browser to connect this worker to the ImageDecoder of its process.
    ErrorOr<NonnullOwnPtr<IPC::Transport>> request_image_decoder_transport();

    void rtc_transform_init(u64 transform_id, Web::HTML::SerializedTransferRecord options_record);
    void rtc_transform_teardown(u64 transform_id);
    void rtc_transform_encoded_audio_frame(u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number);

private:
    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, RefPtr<DedicatedWorkerThreadLink>);

    Web::Page& page();
    Web::Page const& page() const;

    virtual void connect_to_request_server(IPC::TransportHandle handle, int client_id, Optional<Utf16String> top_level_site) override;
    virtual void simulate_request_server_connection_loss_and_reconnect_for_testing(IPC::TransportHandle replacement_handle, int client_id, Optional<Utf16String> top_level_site) override;
    virtual void set_site_compatibility_data(JsonValue data) override;
    virtual void set_permission_snapshot(Web::Permissions::PermissionSnapshot snapshot) override;
    virtual void set_user_agent(String user_agent) override;
    virtual void set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode mode) override;
    virtual void set_preferred_languages(Vector<String> preferred_languages) override;
    virtual void start_worker(URL::URL url, Web::HTML::WorkerType type, Web::HTML::RequestCredentials credentials, String name, Web::HTML::TransferDataEncoder, Web::HTML::SerializedEnvironmentSettingsObject, Web::HTML::AgentType, URL::Origin origin, Web::HTML::EnvironmentId environment_id, bool has_process_to_itself) override;
    virtual void connect_shared_worker(Web::HTML::TransferDataEncoder, Web::HTML::SerializedEnvironmentSettingsObject) override;
    virtual void handle_file_return(i32 error, Optional<IPC::File> file, i32 request_id) override;
    virtual void blob_url_entry_removed(Utf16String url) override;
    virtual void broadcast_channel_message(Web::HTML::BroadcastChannelMessage message) override;

    GC::Root<PageHost> m_page_host;

    void connect_to_compositor();

    RefPtr<Web::Compositor::CompositorConnection> m_compositor_connection;
    bool m_did_ask_for_compositor_connection { false };

    // FIXME: Route console messages to the Browser UI using a ConsoleClient

    HashMap<int, Web::FileRequest> m_requested_files {};
    int last_id { 0 };

    RefPtr<DedicatedWorkerThreadLink> m_dedicated_worker_thread_link;
    HashMap<u64, GC::Root<Web::WebRTC::RTCRtpScriptTransformer>> m_transformers;
    HashMap<u64, Web::HTML::SerializedTransferRecord> m_pending_transform_inits;

    void run_transform_init(u64 transform_id, Web::HTML::SerializedTransferRecord options_record);
    void begin_closing();
    void release_agent_state();
    void finish_shutdown();

    RefPtr<WorkerHost> m_worker_host;
    Function<void()> m_request_server_died_callback_for_testing;
    bool m_shutdown_finished { false };
    bool m_was_told_to_close { false };
    bool m_lost_browser_connection { false };
    NonnullRefPtr<Web::HTML::ScriptAbortHandle> m_script_abort_handle;
};

}
