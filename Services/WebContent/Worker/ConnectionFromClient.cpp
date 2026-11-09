/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCompositing/FontServiceClient.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Function.h>
#include <LibImageDecoderClient/Client.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibMediaClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Compositor/CompositorConnection.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/BroadcastChannel.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/LoaderConfig.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/PermissionsAPI/PermissionStore.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/Platform/RemoteImageCodecPlugin.h>
#include <LibWeb/WebRTC/RTCEncodedAudioFrame.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransformer.h>
#include <LibWeb/WebRTC/RTCTransformEvent.h>
#include <WebContent/Worker/ConnectionFromClient.h>
#include <WebContent/Worker/DedicatedWorkerThreadLink.h>
#include <WebContent/Worker/PageHost.h>
#include <WebContent/Worker/WorkerHost.h>
#include <WebContent/Worker/WorkerThread.h>

namespace WebWorker {

void ConnectionFromClient::connect_to_request_server(IPC::TransportHandle handle, int client_id, Optional<Utf16String> top_level_site)
{
    // NB: Without a client a fetch fails as a network error, and without a loader it would take the whole process down.
    if (!Web::ResourceLoader::is_initialized())
        Web::ResourceLoader::initialize(JS::VM::the().heap());

    auto connect = [&] -> ErrorOr<void> {
        auto transport = TRY(handle.create_transport());
        // Querying the client ID runs the event loop and could dispatch a request before the client is ready.
        auto request_client = TRY(try_make_ref_counted<Requests::RequestClient>(move(transport), client_id));
        Web::ResourceLoader::the().set_client(top_level_site, move(request_client));
        return {};
    };
    if (auto result = connect(); result.is_error())
        dbgln("Failed to connect to resource loader: {}", result.error());

    // A real connection loss defers this callback. Exercise the case where the replacement connection arrives before
    // that deferred callback runs.
    if (auto request_server_died_callback = move(m_request_server_died_callback_for_testing))
        request_server_died_callback();
}

void ConnectionFromClient::simulate_request_server_connection_loss_and_reconnect_for_testing(IPC::TransportHandle replacement_handle, int client_id, Optional<Utf16String> top_level_site)
{
    if (auto disconnected_client = Web::ResourceLoader::the().take_client_for_testing(top_level_site))
        m_request_server_died_callback_for_testing = move(disconnected_client->on_request_server_died);

    connect_to_request_server(move(replacement_handle), client_id, move(top_level_site));
}

ErrorOr<NonnullOwnPtr<IPC::Transport>> ConnectionFromClient::request_media_server_transport()
{
    auto response = send_sync_but_allow_failure<Messages::WebWorkerClient::RequestMediaServerConnection>();
    if (!response || !response->handle().has_value())
        return Error::from_string_literal("The Browser did not connect a media server");
    return response->take_handle()->create_transport();
}

ErrorOr<NonnullOwnPtr<IPC::Transport>> ConnectionFromClient::request_image_decoder_transport()
{
    auto response = send_sync_but_allow_failure<Messages::WebWorkerClient::RequestImageDecoderConnection>();
    if (!response || !response->handle().has_value())
        return Error::from_string_literal("The Browser did not connect an image decoder");
    return response->take_handle()->create_transport();
}

void ConnectionFromClient::connect_to_compositor()
{
    // NB: This process may host a page too, so a worker without a compositor goes on without one.
    auto response = send_sync_but_allow_failure<Messages::WebWorkerClient::RequestCompositorConnection>();
    if (!response || !response->handle().has_value())
        return;
    auto transport_or_error = response->take_handle()->create_transport();
    if (transport_or_error.is_error()) {
        dbgln("Failed to connect to the compositor: {}", transport_or_error.error());
        return;
    }
    m_compositor_connection = adopt_ref(*new Web::Compositor::CompositorConnection(transport_or_error.release_value()));
    m_compositor_connection->on_compositor_lost = [this] {
        m_page_host->compositor_process_lost();
    };
}

Web::Compositor::CompositorConnection* ConnectionFromClient::compositor_process_connection()
{
    // NB: Few workers ever draw, so a worker connects when it first has something for the compositor. It asks once: a
    //     compositor that goes away takes the worker's canvases along.
    if (!exchange(m_did_ask_for_compositor_connection, true))
        connect_to_compositor();
    if (!m_compositor_connection || !m_compositor_connection->is_open())
        return nullptr;
    return m_compositor_connection.ptr();
}

void ConnectionFromClient::set_site_compatibility_data(JsonValue data)
{
    auto parsed_data = Web::SiteCompatibilityData::from_json(data);
    if (parsed_data.is_error()) {
        warnln("Ignoring invalid site compatibility data: {}", parsed_data.error());
        return;
    }
    Web::set_site_compatibility_data(parsed_data.release_value());
}

void ConnectionFromClient::set_permission_snapshot(Web::Permissions::PermissionSnapshot snapshot)
{
    Web::PermissionsAPI::PermissionStore::the().apply_snapshot(move(snapshot));
}

void ConnectionFromClient::set_user_agent(String user_agent)
{
    Web::update_loader_config([&](auto& config) { config.user_agent = move(user_agent); });
}

void ConnectionFromClient::set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode mode)
{
    Web::ResourceLoader::set_navigator_compatibility_mode(mode);
}

void ConnectionFromClient::set_preferred_languages(Vector<String> preferred_languages)
{
    Web::update_loader_config([&](auto& config) { config.preferred_languages = move(preferred_languages); });
}

void ConnectionFromClient::close_worker()
{
    m_was_told_to_close = true;
    begin_closing();
}

void ConnectionFromClient::begin_closing()
{
    if (m_worker_host) {
        m_worker_host->terminate();
        return;
    }
    JS::VM::the().request_interrupt();
    Web::HTML::current_event_loop().destroy_once_script_has_finished();
}

void ConnectionFromClient::die()
{
    if (m_shutdown_finished)
        return;

    m_lost_browser_connection = true;
    begin_closing();
}

void ConnectionFromClient::did_lose_peer_during_sync_request()
{
    // NB: Whatever made the request cannot be unwound from here, and a thread cannot take its process with it. So, the
    //     script stops, and the request fails. That is every caller's to handle, as the connection is shut down from the
    //     event loop the script returns to.
    m_lost_browser_connection = true;
    m_script_abort_handle->abort_script();
    deferred_invoke([this] {
        shutdown();
    });
}

void ConnectionFromClient::set_up_agent_services()
{
    m_page_host->ensure_compositor_host();

    MediaClient::Client::set_transport_factory([this] {
        return request_media_server_transport();
    });

    // Many workers never decode an image, so the Browser connects this one to a decoder when it first has one to decode.
    Web::Platform::ImageCodecPlugin::install(*new Web::Platform::RemoteImageCodecPlugin(
        [this] -> ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> {
            return try_make_ref_counted<ImageDecoderClient::Client>(TRY(request_image_decoder_transport()));
        },
        [this] { async_did_release_image_decoder(); }));
}

void ConnectionFromClient::shut_down_agent()
{
    release_agent_state();
    MediaClient::Client::set_transport_factory(nullptr);
    Web::Bindings::shut_down_agent();
    finish_shutdown();
}

void ConnectionFromClient::release_agent_state()
{
    if (is_open())
        async_did_close_worker();

    if (m_dedicated_worker_thread_link) {
        if (m_lost_browser_connection && !m_was_told_to_close)
            m_dedicated_worker_thread_link->report_death_to_owner();
        else
            m_dedicated_worker_thread_link->report_close_to_owner();
    }

    if (m_worker_host)
        m_worker_host->run_closing_steps();
    m_worker_host = nullptr;

    m_transformers.clear();
    m_pending_transform_inits.clear();
    m_requested_files.clear();
    m_request_server_died_callback_for_testing = nullptr;
    if (m_compositor_connection)
        m_compositor_connection->on_compositor_lost = nullptr;
    m_page_host = nullptr;
}

void ConnectionFromClient::finish_shutdown()
{
    m_shutdown_finished = true;
    m_compositor_connection = nullptr;

    // NB: What the worker reported last still has to reach the client.
    transport().flush();
}

void ConnectionFromClient::request_file(Web::FileRequest request)
{
    auto request_id = ++last_id;

    auto path = request.path();
    m_requested_files.set(request_id, move(request));
    async_did_request_file(path, request_id);
}

void ConnectionFromClient::blob_url_entry_removed(Utf16String url)
{
    if (auto url_record = Web::DOMURL::parse(url.utf16_view()); url_record.has_value())
        Web::FileAPI::remove_entry_from_blob_url_store(*url_record);
}

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, RefPtr<DedicatedWorkerThreadLink> dedicated_worker_thread_link)
    : IPC::ConnectionFromClient<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport), 1)
    , m_page_host(PageHost::create(*this))
    , m_dedicated_worker_thread_link(move(dedicated_worker_thread_link))
    , m_script_abort_handle(Web::HTML::surrounding_agent().script_abort_handle())
{
    // The UI process started this worker agent, and die() ends it. So, once the UI process has closed the connection —
    // e.g. because the page terminated the worker while it was still starting — shut down right away, rather than handle
    // startup messages and make sync requests on a closed connection.
    set_peer_owns_this_process(true);

    Web::HTML::current_event_loop().set_on_destroyed([] {
        Core::EventLoop::current().quit(0);
    });

    // NB: The thread that handles close_worker() may be stuck in script, so that message and the loss of the client have
    //     to abort the script from the IO thread, for this thread to get to them at all.
    this->transport().set_incoming_message_observer([script_abort_handle = m_script_abort_handle](ReadonlyBytes message) {
        struct [[gnu::packed]] Header {
            u32 endpoint_magic;
            i32 message_id;
        };
        if (!message.is_empty()) {
            if (message.size() < sizeof(Header))
                return;
            auto const& header = *reinterpret_cast<Header const*>(message.data());
            if (header.endpoint_magic != WebWorkerServerEndpoint::static_magic() || header.message_id != Messages::WebWorkerServer::CloseWorker::static_message_id())
                return;
        }
        script_abort_handle->abort_script();
    });
}

ConnectionFromClient::~ConnectionFromClient() = default;

Web::Page& ConnectionFromClient::page()
{
    return m_page_host->page();
}

Web::Page const& ConnectionFromClient::page() const
{
    return m_page_host->page();
}

void ConnectionFromClient::start_worker(URL::URL url, Web::HTML::WorkerType type, Web::HTML::RequestCredentials credentials, String name, Web::HTML::TransferDataEncoder implicit_port, Web::HTML::SerializedEnvironmentSettingsObject outside_settings, Web::HTML::AgentType agent_type, URL::Origin origin, Web::HTML::EnvironmentId environment_id, bool has_process_to_itself)
{
    // Shutting down the agent drains the event queue after the agent state is released, and that can deliver a start
    // the client sent before the worker was closed. Then there's no page to run the worker in, and nobody waits for it.
    if (!m_page_host)
        return;

    m_worker_host = make_ref_counted<WorkerHost>(move(url), type, move(name));

    bool const is_shared = agent_type == Web::HTML::AgentType::SharedWorker;
    VERIFY(is_shared || agent_type == Web::HTML::AgentType::DedicatedWorker);

    // FIXME: Add an assertion that the agent_type passed here is the same that was passed to initialize_agent()

    m_worker_host->set_on_script_ready(GC::create_function(GC::Heap::the(), GC::captures(make_weak_ptr<ConnectionFromClient>()), [](auto& connection) {
        if (!connection)
            return;
        auto pending = move(connection->m_pending_transform_inits);
        for (auto& entry : pending)
            connection->run_transform_init(entry.key, move(entry.value));
    }));

    m_worker_host->run(page(), move(implicit_port), outside_settings, credentials, is_shared, move(origin), move(environment_id), has_process_to_itself);
}

void ConnectionFromClient::connect_shared_worker(Web::HTML::TransferDataEncoder message_port, Web::HTML::SerializedEnvironmentSettingsObject outside_settings)
{
    if (!m_worker_host)
        return;
    m_worker_host->connect_shared_worker(move(message_port), move(outside_settings));
}

void ConnectionFromClient::handle_file_return(i32 error, Optional<IPC::File> file, i32 request_id)
{
    auto file_request = m_requested_files.take(request_id);

    VERIFY(file_request.has_value());
    VERIFY(file_request.value().on_file_request_finish);

    file_request.value().on_file_request_finish(error != 0 ? Error::from_errno(error) : ErrorOr<i32> { file->take_fd() });
}

void ConnectionFromClient::broadcast_channel_message(Web::HTML::BroadcastChannelMessage message)
{
    Web::HTML::deliver_broadcast_channel_message_locally(move(message));
}

void ConnectionFromClient::rtc_transform_init(u64 transform_id, Web::HTML::SerializedTransferRecord options_record)
{
    if (!m_worker_host || !m_worker_host->script_has_run()) {
        // Wait for the worker script to install onrtctransform so the event is not dropped.
        m_pending_transform_inits.set(transform_id, move(options_record));
        return;
    }
    run_transform_init(transform_id, move(options_record));
}

void ConnectionFromClient::rtc_transform_teardown(u64 transform_id)
{
    m_pending_transform_inits.remove(transform_id);

    // The worker's script may hold on to the transformer, but nothing it writes has anywhere to go anymore.
    if (auto transformer = m_transformers.take(transform_id); transformer.has_value())
        (*transformer)->set_write_encoded_data_algorithm(nullptr);
}

void ConnectionFromClient::run_transform_init(u64 transform_id, Web::HTML::SerializedTransferRecord options_record)
{
    if (!m_worker_host)
        return;
    auto global_scope = m_worker_host->global_scope();
    if (!global_scope) {
        dbgln("ConnectionFromClient::rtc_transform_init: no worker global scope yet (transform_id={})", transform_id);
        return;
    }
    auto& realm = Web::HTML::relevant_realm(*global_scope);
    Web::HTML::TemporaryExecutionContext context(realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    // Spec step 8.1: deserialize serializedOptions in the worker realm.
    auto deserialized_or_err = Web::HTML::structured_deserialize_with_transfer(options_record, realm);
    if (deserialized_or_err.is_exception()) {
        dbgln("ConnectionFromClient::rtc_transform_init: failed to deserialize options for id={}", transform_id);
        return;
    }
    auto options = deserialized_or_err.release_value().deserialized;

    // Spec step 8.2: create an RTCRtpScriptTransformer with the deserialized options.
    auto transformer_or_err = Web::WebRTC::RTCRtpScriptTransformer::create(realm, options);
    if (transformer_or_err.is_exception()) {
        dbgln("ConnectionFromClient::rtc_transform_init: failed to construct transformer for id={}", transform_id);
        return;
    }
    auto transformer = transformer_or_err.release_value();
    m_transformers.set(transform_id, GC::make_root(transformer));

    auto write_encoded_data = GC::create_function(realm.heap(), GC::captures(NonnullRefPtr { *m_dedicated_worker_thread_link }, transform_id), [](auto& link, auto& transform_id, JS::Value chunk) {
        auto* frame = chunk.is_object() ? Web::Bindings::impl_from<Web::WebRTC::RTCEncodedAudioFrame>(&chunk.as_object()) : nullptr;
        if (!frame) {
            dbgln_if(WEBRTC_DEBUG, "worker: write algorithm got non-frame value (chunk_is_object={}, class={}) for id={}",
                chunk.is_object(),
                chunk.is_object() ? chunk.as_object().class_name() : "n/a"sv,
                transform_id);
            return;
        }
        dbgln_if(WEBRTC_DEBUG, "worker: write algorithm received frame id={}", transform_id);
        auto data = frame->data();
        auto bytes = data->copy_to_byte_buffer(0, data->byte_length()).release_value_but_fixme_should_propagate_errors();
        auto metadata = frame->get_metadata();
        u32 ssrc = metadata.synchronization_source.value_or(0);
        u8 payload_type = metadata.payload_type.value_or(0);
        u32 rtp_timestamp = metadata.rtp_timestamp.value_or(0);
        u16 sequence_number = static_cast<u16>(metadata.sequence_number.value_or(0));
        link->post_to_owner([owner_token = link->owner_token(), transform_id, payload = move(bytes), ssrc, payload_type, rtp_timestamp, sequence_number]() mutable {
            Web::HTML::WorkerAgentParent::did_write_rtc_transform_encoded_audio_frame(owner_token, transform_id, move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
        });
    });
    transformer->set_write_encoded_data_algorithm(write_encoded_data);

    // Spec step 8: queue a global task on the DOM manipulation task source so the
    // event fires on the worker's own event loop, after any pending script setup
    // (e.g. `self.onrtctransform = ...`) has had a chance to run.
    Web::HTML::queue_global_task(Web::HTML::Task::Source::DOMManipulation, Web::HTML::relevant_global_object(*global_scope),
        GC::create_function(realm.heap(), GC::captures(global_scope, transformer, realm), [](auto& global_scope, auto& transformer, auto& realm) {
            Web::HTML::TemporaryExecutionContext context(realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto event = Web::WebRTC::RTCTransformEvent::create(Web::HTML::EventNames::rtctransform, transformer,
                Web::HighResolutionTime::current_high_resolution_time(Web::HTML::relevant_global_object(*global_scope)));
            global_scope->dispatch_event(event);
        }));
}

void ConnectionFromClient::rtc_transform_encoded_audio_frame(u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    auto entry = m_transformers.get(transform_id);
    if (!entry.has_value()) {
        dbgln_if(WEBRTC_DEBUG, "worker: rtc_transform_encoded_audio_frame for unknown id={} (have {} transformers)", transform_id, m_transformers.size());
        return;
    }
    auto transformer = *entry;
    dbgln_if(WEBRTC_DEBUG, "worker: rtc_transform_encoded_audio_frame id={} ssrc={} seq={} len={}", transform_id, ssrc, sequence_number, payload.size());

    if (!m_worker_host || !m_worker_host->global_scope())
        return;
    auto& realm = Web::HTML::relevant_realm(*m_worker_host->global_scope());
    Web::HTML::TemporaryExecutionContext context(realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    auto frame = Web::WebRTC::RTCEncodedAudioFrame::create_from_packet(realm, move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
    auto wrapped_frame = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, frame);
    if (auto result = transformer->enqueue_encoded_frame(wrapped_frame); result.is_exception())
        dbgln_if(WEBRTC_DEBUG, "rtc_transform_encoded_audio_frame: enqueue failed for transform_id={}", transform_id);
}

}
