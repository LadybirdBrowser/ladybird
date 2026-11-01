/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCompositing/FontServiceClient.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Compositor/CompositorConnection.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/BroadcastChannel.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/EventNames.h>
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
#include <LibWeb/WebRTC/RTCEncodedAudioFrame.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransformer.h>
#include <LibWeb/WebRTC/RTCTransformEvent.h>
#include <WebWorker/ConnectionFromClient.h>
#include <WebWorker/PageHost.h>
#include <WebWorker/WorkerHost.h>

namespace WebWorker {

void ConnectionFromClient::connect_to_request_server(IPC::TransportHandle handle, int client_id, Optional<Utf16String> top_level_site)
{
    if (on_request_server_connection)
        on_request_server_connection(handle, client_id, top_level_site);

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

void ConnectionFromClient::connect_to_image_decoder(IPC::TransportHandle handle)
{
    if (on_image_decoder_connection)
        on_image_decoder_connection(handle);
}

ErrorOr<NonnullOwnPtr<IPC::Transport>> ConnectionFromClient::request_media_server_transport()
{
    auto response = send_sync_but_allow_failure<Messages::WebWorkerClient::RequestMediaServerConnection>();
    if (!response || !response->handle().has_value())
        return Error::from_string_literal("The Browser did not connect a media server");
    return response->take_handle()->create_transport();
}

void ConnectionFromClient::connect_to_compositor(IPC::TransportHandle handle)
{
    auto transport = MUST(handle.create_transport());
    m_compositor_connection = adopt_ref(*new Web::Compositor::CompositorConnection(move(transport)));
    m_compositor_connection->on_compositor_lost = [this] {
        m_page_host->compositor_process_lost();
    };

    m_page_host->ensure_compositor_host();
}

Web::Compositor::CompositorConnection* ConnectionFromClient::compositor_process_connection() const
{
    if (!m_compositor_connection || !m_compositor_connection->is_open())
        return nullptr;
    return m_compositor_connection.ptr();
}

void ConnectionFromClient::set_system_font_family(String family)
{
    Web::Platform::FontPlugin::the().set_system_font_family(FlyString { family });
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
    Web::ResourceLoader::the().set_navigator_compatibility_mode(mode);
}

void ConnectionFromClient::set_preferred_languages(Vector<String> preferred_languages)
{
    Web::update_loader_config([&](auto& config) { config.preferred_languages = move(preferred_languages); });
}

void ConnectionFromClient::close_worker()
{
    async_did_close_worker();

    // FIXME: Invoke a worker shutdown operation that implements the spec
    m_worker_host = nullptr;

    die();
}

void ConnectionFromClient::die()
{
    // FIXME: When handling multiple workers in the same process,
    //     this logic needs to be smarter (only when all workers are dead, etc).
    Core::Process::terminate_immediately(0);
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

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, bool enable_test_mode)
    : IPC::ConnectionFromClient<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport), 1)
    , m_page_host(PageHost::create(*this))
    , m_enable_test_mode(enable_test_mode)
{
    // The UI process spawned this process to run one worker agent, and die() ends it. So, once the UI process has
    // closed the connection — e.g. because the page terminated the worker while this process was still starting — shut
    // down right away, rather than handle startup messages and make sync requests on a closed connection.
    set_peer_owns_this_process(true);
}

ConnectionFromClient::~ConnectionFromClient() = default;

void ConnectionFromClient::set_font_service(IPC::TransportHandle handle, IPC::File catalog, u64 catalog_size, u64 generation)
{
    auto* font_provider = MUST(Compositing::install_font_service(move(handle), move(catalog), catalog_size, generation));
    Web::Platform::FontPlugin::install(*new Web::Platform::FontPlugin(m_enable_test_mode, font_provider));
}

Web::Page& ConnectionFromClient::page()
{
    return m_page_host->page();
}

Web::Page const& ConnectionFromClient::page() const
{
    return m_page_host->page();
}

void ConnectionFromClient::start_worker(URL::URL url, Web::HTML::WorkerType type, Web::HTML::RequestCredentials credentials, String name, Web::HTML::TransferDataEncoder implicit_port, Web::HTML::SerializedEnvironmentSettingsObject outside_settings, Web::HTML::AgentType agent_type, URL::Origin origin, Web::HTML::EnvironmentId environment_id)
{
    m_worker_host = make_ref_counted<WorkerHost>(move(url), type, move(name));

    bool const is_shared = agent_type == Web::HTML::AgentType::SharedWorker;
    VERIFY(is_shared || agent_type == Web::HTML::AgentType::DedicatedWorker);

    // FIXME: Add an assertion that the agent_type passed here is the same that was passed at process creation to initialize_main_thread_vm()

    m_worker_host->set_on_script_ready([this] {
        auto pending = move(m_pending_transform_inits);
        for (auto& entry : pending)
            run_transform_init(entry.key, move(entry.value));
    });

    m_worker_host->run(page(), move(implicit_port), outside_settings, credentials, is_shared, move(origin), move(environment_id));
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

void ConnectionFromClient::did_worker_agent_fail_loading_script(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Web::HTML::WorkerAgentParent::did_fail_loading_worker_script(owner_token);
}

void ConnectionFromClient::did_worker_agent_report_exception(Web::HTML::WorkerAgentOwnerToken owner_token, Utf16String message, Utf16String filename, u32 lineno, u32 colno)
{
    Web::HTML::WorkerAgentParent::did_report_worker_exception(owner_token, move(message), move(filename), lineno, colno);
}

void ConnectionFromClient::did_worker_agent_report_content_security_policy_violation(Web::HTML::WorkerAgentOwnerToken owner_token, u64 policy_index, String effective_directive, URL::URL resource)
{
    Web::HTML::WorkerAgentParent::did_report_content_security_policy_violation(owner_token, policy_index, move(effective_directive), move(resource));
}

void ConnectionFromClient::did_worker_agent_close(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Web::HTML::WorkerAgentParent::did_close_worker(owner_token);
}

void ConnectionFromClient::did_worker_agent_die(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Web::HTML::WorkerAgentParent::did_worker_agent_die(owner_token);
}

void ConnectionFromClient::broadcast_channel_message(Web::HTML::BroadcastChannelMessage message)
{
    Web::HTML::BroadcastChannel::deliver_message_locally(message);
}

void ConnectionFromClient::rtc_transform_init(u64 transform_id, Web::HTML::SerializedTransferRecord options_record)
{
    if (!m_worker_host)
        return;
    if (!m_worker_host->script_has_run()) {
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

    // Return transformed frames to the parent over IPC. Capturing `this` is safe because
    // the connection outlives the transformer; close_worker tears it down via die().
    auto write_encoded_data = GC::create_function(realm.heap(), [this, transform_id](JS::Value chunk) {
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
        async_rtc_transform_encoded_audio_frame_written(transform_id, bytes.bytes(), ssrc, payload_type, rtp_timestamp, sequence_number);
    });
    transformer->set_write_encoded_data_algorithm(write_encoded_data);

    // Spec step 8: queue a global task on the DOM manipulation task source so the
    // event fires on the worker's own event loop, after any pending script setup
    // (e.g. `self.onrtctransform = ...`) has had a chance to run.
    Web::HTML::queue_global_task(Web::HTML::Task::Source::DOMManipulation, Web::HTML::relevant_global_object(*global_scope),
        GC::create_function(realm.heap(), [global_scope, transformer, &realm] {
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
