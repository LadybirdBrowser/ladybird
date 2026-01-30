/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/SharedBufferStream.h>
#include <LibCore/System.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/FlowControl.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/SharedMemory.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>
#include <LibWeb/WebAudio/Engine/StreamTransportEventFD.h>
#include <LibWeb/WebAudio/Engine/WebAudioClientRegistry.h>
#include <LibWeb/WebAudio/Engine/WebAudioWorkerSession.h>
#include <LibWeb/WebAudio/RenderGraph.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>
#include <LibWebAudioWorkerClient/WebAudioClient.h>

namespace Web::WebAudio::Render {

static RefPtr<WebAudioWorkerClient::WebAudioClient> s_webaudio_client;

static bool validate_analyser_snapshot_request(u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db)
{
    if (fft_size == 0)
        return false;
    if (out_time_domain.size() != fft_size)
        return false;
    if (!out_frequency_db.is_empty() && out_frequency_db.size() != (fft_size / 2))
        return false;
    return true;
}

static u64 render_quantum_index_from_rendered_frames(u64 rendered_frames_total)
{
    size_t quantum_size = BaseAudioContext::default_render_quantum_size();
    if (quantum_size == 0)
        return 0;
    return rendered_frames_total / quantum_size;
}

void WebAudioWorkerSession::set_webaudio_session(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient> const& client, u64 session_id)
{
    ASSERT_CONTROL_THREAD();
    if (::Web::WebAudio::should_log_info())
        WA_DBGLN("[WebAudio] WebAudioWorker: set_audio_server_session old_session={} new_session={} have_client={}", m_session_id, session_id, true);
    m_client = client;
    m_session_id = session_id;
    m_published_media_element_stream_bindings = false;
    m_published_media_stream_audio_source_bindings = false;
    m_script_processor_stream_bindings.set_webaudio_session(client, session_id);

    if (m_pending_suspend_state.has_value()) {
        auto const& pending = m_pending_suspend_state.value();
        (void)m_client->webaudio_session_set_suspended(m_session_id, pending.suspended, pending.generation);
    }
}

void WebAudioWorkerSession::clear_webaudio_session()
{
    ASSERT_CONTROL_THREAD();
    m_script_processor_stream_bindings.clear_webaudio_session();

    Threading::MutexLocker locker(m_remote_analyser_streams_mutex);
    m_remote_analyser_streams.clear();

    for (auto& it : m_remote_media_element_streams) {
        if (it.value.provider)
            it.value.provider->clear_stream_transport_producer();
        if (it.value.notify_read_fd >= 0)
            (void)Core::System::close(it.value.notify_read_fd);
        if (it.value.notify_write_fd >= 0)
            (void)Core::System::close(it.value.notify_write_fd);
    }
    m_remote_media_element_streams.clear();
    m_media_stream_source_metadata.clear();

    m_session_id = 0;
    m_client = nullptr;
    m_published_media_element_stream_bindings = false;
    m_published_media_stream_audio_source_bindings = false;
}

static Optional<RingStreamView> view_for_ring_stream_buffer(Core::AnonymousBuffer& buffer)
{
    if (!buffer.is_valid() || buffer.size() < sizeof(RingStreamHeader))
        return {};
    auto* header = buffer.data<RingStreamHeader>();
    if (!header)
        return {};

    u64 capacity_frames = header->capacity_frames;
    u32 channel_capacity = header->channel_capacity;
    if (capacity_frames == 0 || channel_capacity == 0)
        return {};

    size_t required_bytes = ring_stream_bytes_total(channel_capacity, capacity_frames);
    if (buffer.size() < required_bytes)
        return {};

    u8* base = buffer.data<u8>();
    if (!base)
        return {};
    size_t data_bytes = ring_stream_bytes_for_data(channel_capacity, capacity_frames);
    auto* data_f32 = reinterpret_cast<f32*>(base + sizeof(RingStreamHeader));
    return RingStreamView { .header = header, .interleaved_frames = Span<f32> { data_f32, data_bytes / sizeof(f32) } };
}

template<typename Key, typename Value, typename Cleanup>
static bool remove_entries_not_in(HashMap<Key, Value>& map, HashTable<Key> const& seen, Cleanup cleanup)
{
    Vector<Key> to_remove;
    for (auto const& it : map) {
        if (!seen.contains(it.key))
            to_remove.append(it.key);
    }

    bool removed_any = false;
    for (auto const& key : to_remove) {
        auto it = map.find(key);
        if (it == map.end())
            continue;
        cleanup(it->value);
        map.remove(it);
        removed_any = true;
    }

    return removed_any;
}

bool WebAudioWorkerSession::update_media_element_stream_bindings(GraphResourceRegistry const& resources, Vector<WorkletPortBinding>& worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();
    // Bind MediaElementAudioSource providers to shared RingStreams.
    // Publish bindings to AudioServer when providers are added/removed, or once per session.
    HashTable<MediaElementAudioSourceProviderID> seen_provider_ids;
    Vector<MediaElementAudioSourceProviderID> publish_provider_ids;
    bool should_publish_media_stream_bindings = false;

    auto const& sources = resources.media_element_audio_sources();
    seen_provider_ids.ensure_capacity(sources.size());
    publish_provider_ids.ensure_capacity(sources.size());

    if (!sources.is_empty() && !m_published_media_element_stream_bindings)
        should_publish_media_stream_bindings = true;

    for (auto const& it : sources) {
        auto provider_id = it.key;
        auto const& provider = it.value;
        seen_provider_ids.set(provider_id);

        auto existing = m_remote_media_element_streams.find(provider_id);
        if (existing == m_remote_media_element_streams.end()) {
            u32 channel_capacity = static_cast<u32>(max<size_t>(1, provider->channel_capacity()));
            u64 capacity_frames = static_cast<u64>(max<size_t>(1, provider->capacity_frames()));

            size_t total_bytes = ring_stream_bytes_total(channel_capacity, capacity_frames);
            auto buffer_or_error = Core::AnonymousBuffer::create_with_size(total_bytes);
            if (buffer_or_error.is_error())
                continue;
            auto buffer = buffer_or_error.release_value();

            auto* header = buffer.data<RingStreamHeader>();
            if (!header)
                continue;

            __builtin_memset(header, 0, sizeof(*header));
            header->version = ring_stream_version;
            header->sample_rate_hz = 0;
            header->channel_count = 1;
            header->channel_capacity = channel_capacity;
            header->capacity_frames = capacity_frames;
            ring_stream_store_read_frame(*header, 0);
            ring_stream_store_write_frame(*header, 0);
            header->timeline_generation = 1;
            header->timeline_sample_rate = 0;

            auto view = view_for_ring_stream_buffer(buffer);
            if (!view.has_value())
                continue;

            auto notify_fds_or_error = create_nonblocking_stream_notify_fds();
            if (notify_fds_or_error.is_error())
                continue;
            auto notify_fds = notify_fds_or_error.release_value();
            int notify_read_fd = notify_fds.read_fd;
            int notify_write_fd = notify_fds.write_fd;

            provider->set_stream_transport_producer(*view, StreamOverflowPolicy::DropOldest, notify_write_fd);

            m_remote_media_element_streams.set(provider_id, RemoteMediaElementStream {
                                                                .shared_memory = buffer,
                                                                .view = *view,
                                                                .notify_read_fd = notify_read_fd,
                                                                .notify_write_fd = notify_write_fd,
                                                                .provider = provider,
                                                            });

            should_publish_media_stream_bindings = true;
            existing = m_remote_media_element_streams.find(provider_id);
        } else {
            provider->set_stream_transport_producer(existing->value.view, StreamOverflowPolicy::DropOldest, existing->value.notify_write_fd);
        }

        if (existing != m_remote_media_element_streams.end())
            publish_provider_ids.unchecked_append(provider_id);
    }

    bool removed_any = remove_entries_not_in(m_remote_media_element_streams, seen_provider_ids, [](RemoteMediaElementStream& state) {
        if (state.provider)
            state.provider->clear_stream_transport_producer();
        if (state.notify_read_fd >= 0)
            (void)Core::System::close(state.notify_read_fd);
        if (state.notify_write_fd >= 0)
            (void)Core::System::close(state.notify_write_fd);
    });
    if (removed_any)
        should_publish_media_stream_bindings = true;

    bool const have_sources = !sources.is_empty();

    if (::Web::WebAudio::should_log_media_element_bridge()) {
        WA_MEDIA_DBGLN("[WebAudio] media-stream-bindings: providers_in_graph={} publishable={} publish_requested={} published_already={} remote_streams={}",
            sources.size(),
            publish_provider_ids.size(),
            should_publish_media_stream_bindings,
            m_published_media_element_stream_bindings,
            m_remote_media_element_streams.size());
    }

    if (!should_publish_media_stream_bindings)
        return false;

    Vector<MediaElementAudioSourceStreamDescriptor> media_stream_bindings;
    media_stream_bindings.ensure_capacity(publish_provider_ids.size());

    for (auto provider_id : publish_provider_ids) {
        auto it = m_remote_media_element_streams.find(provider_id);
        if (it == m_remote_media_element_streams.end())
            continue;
        auto& state = it->value;

        RingStreamDescriptor ring_descriptor;
        ring_descriptor.stream_id = provider_id;
        ring_descriptor.format.channel_capacity = state.view.header->channel_capacity;
        ring_descriptor.format.capacity_frames = state.view.header->capacity_frames;
        // These fields can legitimately change as the media element starts/changes format.
        // Leaving them as 0 makes validation rely on the shared header rather than requiring
        // the descriptor to match a moving target.
        ring_descriptor.format.channel_count = 0;
        ring_descriptor.format.sample_rate_hz = 0;
        ring_descriptor.overflow_policy = StreamOverflowPolicy::DropOldest;
        ring_descriptor.shared_memory = state.shared_memory;
        if (state.notify_read_fd >= 0) {
            auto cloned_or_error = IPC::File::clone_fd(state.notify_read_fd);
            if (!cloned_or_error.is_error())
                ring_descriptor.notify_fd = cloned_or_error.release_value();
        }

        media_stream_bindings.append(MediaElementAudioSourceStreamDescriptor {
            .provider_id = provider_id,
            .ring_stream = move(ring_descriptor),
        });
    }

    auto publish_outcome = transactional_publish_bindings(
        true,
        have_sources,
        false,
        move(media_stream_bindings),
        false,
        [this](Vector<MediaElementAudioSourceStreamDescriptor>&& bindings) {
            auto result = m_client->webaudio_session_set_media_element_audio_source_streams(m_session_id, move(bindings));
            if (result.is_error()) {
                WA_DBGLN("[WebAudio] Failed to send media element stream bindings to AudioServer: {}", result.error());
                return false;
            }
            return true;
        });

    if (publish_outcome == TransactionalPublishOutcome::RetryLater) {
        if (::Web::WebAudio::should_log_media_element_bridge())
            WA_MEDIA_DBGLN("[WebAudio] media-stream-bindings: skipping publish (no streams available)");

        // If this backend update is dropped on the floor, close any worklet fds so they don't leak.
        close_worklet_port_binding_fds(worklet_port_bindings);
        return true;
    }

    if (publish_outcome == TransactionalPublishOutcome::Published)
        m_published_media_element_stream_bindings = have_sources;

    return false;
}

void WebAudioWorkerSession::update_media_stream_audio_source_bindings(GraphResourceRegistry const& resources)
{
    ASSERT_CONTROL_THREAD();
    auto const& metadata_sources = resources.media_stream_audio_source_metadata();
    bool should_publish = false;

    if (!metadata_sources.is_empty() && !m_published_media_stream_audio_source_bindings)
        should_publish = true;

    HashTable<MediaStreamAudioSourceProviderID> seen_provider_ids;
    seen_provider_ids.ensure_capacity(metadata_sources.size());

    for (auto const& it : metadata_sources) {
        seen_provider_ids.set(it.key);
        auto existing = m_media_stream_source_metadata.find(it.key);
        if (existing != m_media_stream_source_metadata.end()) {
            if (existing->value.device_id == it.value.device_id
                && existing->value.sample_rate_hz == it.value.sample_rate_hz
                && existing->value.channel_count == it.value.channel_count
                && existing->value.capacity_frames == it.value.capacity_frames
                && existing->value.overflow_policy == it.value.overflow_policy)
                continue;
        }

        m_media_stream_source_metadata.set(it.key, it.value);
        should_publish = true;
    }

    bool removed_any = remove_entries_not_in(m_media_stream_source_metadata, seen_provider_ids, [](AudioInputStreamMetadata&) { });
    if (removed_any)
        should_publish = true;

    if (!should_publish)
        return;

    Vector<MediaStreamAudioSourceStreamDescriptor> bindings;
    bindings.ensure_capacity(m_media_stream_source_metadata.size());
    for (auto const& it : m_media_stream_source_metadata) {
        if (it.key == 0 || it.value.device_id == 0)
            continue;
        bindings.append(MediaStreamAudioSourceStreamDescriptor {
            .provider_id = it.key,
            .metadata = it.value,
        });
    }

    auto result = m_client->webaudio_session_set_media_stream_audio_source_streams(m_session_id, move(bindings));
    if (result.is_error())
        WA_DBGLN("[WebAudio] Failed to send media stream source bindings to AudioServer: {}", result.error());

    m_published_media_stream_audio_source_bindings = !metadata_sources.is_empty();
}

ErrorOr<void> WebAudioWorkerSession::ensure_output_open(WebAudioClientRegistry& engine, u32 target_latency_ms, u64 page_id)
{
    ASSERT_CONTROL_THREAD();

    if (m_webaudio_session.has_value())
        return {};

    RefPtr<WebAudioWorkerClient::WebAudioClient> client = WebAudioWorkerSession::webaudio_client();
    if (!client) {
        return Error::from_string_literal("WebAudio: WebAudioWorker backend selected but WebAudio client is not available");
    }

    auto webaudio_session = TRY(client->create_webaudio_session(target_latency_ms, page_id));
    m_webaudio_session = move(webaudio_session);
    m_timing_buffer = m_webaudio_session->timing_buffer;

    WebAudioClientRegistry::DeviceFormat format {
        .sample_rate = m_webaudio_session->sample_rate,
        .channel_count = m_webaudio_session->channel_count,
    };
    engine.set_client_device_format(m_client_id, format);

    start_time_sync_notifier_if_needed(engine);

    VERIFY(m_webaudio_session.has_value());
    set_webaudio_session(NonnullRefPtr { *client }, m_webaudio_session->session_id);
    return {};
}

void WebAudioWorkerSession::shutdown_output(WebAudioClientRegistry&)
{
    ASSERT_CONTROL_THREAD();

    stop_time_sync_notifier();

    RefPtr<WebAudioWorkerClient::WebAudioClient> client = WebAudioWorkerSession::webaudio_client();

    if (client && m_session_id != 0)
        (void)client->destroy_webaudio_session(m_session_id);
    m_webaudio_session.clear();
    m_timing_buffer = {};

    clear_webaudio_session();
    m_client = nullptr;
}

void WebAudioWorkerSession::start_time_sync_notifier_if_needed(WebAudioClientRegistry& engine)
{
    ASSERT_CONTROL_THREAD();
    if (m_time_sync_notifier)
        return;
    if (!m_webaudio_session.has_value()) {
        if (should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] WebAudioWorker time sync: no webaudio session");
        return;
    }
    if (!m_timing_buffer.is_valid()) {
        if (should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] WebAudioWorker time sync: invalid timing buffer");
        return;
    }
    if (m_timing_buffer.size() < sizeof(Web::WebAudio::Render::WebAudioTimingPage)) {
        if (should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] WebAudioWorker time sync: timing buffer too small ({} bytes)", m_timing_buffer.size());
        return;
    }

    int notify_fd = m_webaudio_session->timing_notify_fd.fd();
    if (notify_fd < 0) {
        if (should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] WebAudioWorker time sync: invalid notify fd");
        return;
    }

    m_time_sync_notifier = Core::Notifier::construct(notify_fd, Core::Notifier::Type::Read);
    auto weak_engine = engine.make_weak_ptr();
    m_time_sync_notifier->on_activation = [this, weak_engine] {
        if (!weak_engine)
            return;

        auto& engine = *weak_engine;

        ASSERT_CONTROL_THREAD();

        if (!m_webaudio_session.has_value())
            return;
        int fd = m_webaudio_session->timing_notify_fd.fd();
        if (fd < 0)
            return;

        // Drain the pipe to coalesce notifications.
        auto drain_result = drain_nonblocking_notify_fd(fd);
        if (drain_result != DrainNotifyFDResult::Drained) {
            stop_time_sync_notifier();
            m_webaudio_session->timing_notify_fd = {};
            return;
        }

        auto* timing_page = m_timing_buffer.data<Web::WebAudio::Render::WebAudioTimingPage>();
        if (!timing_page)
            return;

        update_current_frames_from_timing_page(engine);
    };
}

void WebAudioWorkerSession::update_current_frames_from_timing_page(WebAudioClientRegistry& engine)
{
    ASSERT_CONTROL_THREAD();
    if (!m_timing_buffer.is_valid())
        return;

    auto* timing_page = m_timing_buffer.data<Web::WebAudio::Render::WebAudioTimingPage>();
    if (!timing_page)
        return;

    Web::WebAudio::Render::WebAudioTimingSnapshot snapshot;
    if (!Web::WebAudio::Render::read_webaudio_timing_page(*timing_page, snapshot)) {
        if (should_log_output_driver()) {
            static Optional<MonotonicTime> s_last_time_sync_log;
            auto now = MonotonicTime::now_coarse();
            if (!s_last_time_sync_log.has_value() || (now - s_last_time_sync_log.value()) >= AK::Duration::from_seconds(1)) {
                WA_OUT_DBGLN("[WebAudio] WebAudioWorker time sync: failed to read timing page");
                s_last_time_sync_log = now;
            }
        }
        return;
    }

    Threading::MutexLocker locker { engine.m_clients_mutex };
    auto it = engine.m_clients.find(m_client_id);
    if (it == engine.m_clients.end())
        return;
    ClientState& state = it->value.state;
    if (state.current_frame)
        state.current_frame->store(snapshot.rendered_frames_total, AK::MemoryOrder::memory_order_release);
    if (state.suspend_state)
        state.suspend_state->store(snapshot.suspend_state, AK::MemoryOrder::memory_order_release);
    if (state.underrun_frames_total)
        state.underrun_frames_total->store(snapshot.underrun_frames_total, AK::MemoryOrder::memory_order_release);
}

void WebAudioWorkerSession::stop_time_sync_notifier()
{
    if (!m_time_sync_notifier)
        return;
    m_time_sync_notifier->set_enabled(false);
    m_time_sync_notifier = nullptr;
}

void WebAudioWorkerSession::update_client_render_graph(
    WebAudioClientRegistry& engine,
    u64 client_id,
    f32 graph_sample_rate,
    ByteBuffer encoded_graph,
    NonnullOwnPtr<GraphResourceRegistry> resources,
    Vector<WorkletModule> worklet_modules,
    Vector<WorkletNodeDefinition> worklet_node_definitions,
    Vector<WorkletPortBinding> worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();

    if (::Web::WebAudio::should_log_info()) {
        WA_DBGLN("[WebAudio] WebAudioWorker update: session={} have_client={} client_id={} graph_sr={} encoded_graph_bytes={} published_sp={} published_media={} event_loop_running={}",
            m_session_id,
            m_client != nullptr,
            client_id,
            graph_sample_rate,
            encoded_graph.size(),
            m_script_processor_stream_bindings.published_bindings(),
            m_published_media_element_stream_bindings,
            Core::EventLoop::is_running());
    }

    // ensure_backend_output_open() may run before the Core event loop has started, which would prevent
    // time-sync notifier setup. Retry here so currentTime can advance once the loop is running.
    start_time_sync_notifier_if_needed(engine);

    if (m_session_id == 0 || !m_client) {
        if (::Web::WebAudio::should_log_info())
            WA_DBGLN("[WebAudio] WebAudioWorker update: skipping (no active AudioServer session)");
        return;
    }

    PendingClientRenderGraphUpdate update {
        .client_id = client_id,
        .graph_sample_rate = graph_sample_rate,
        .encoded_graph = move(encoded_graph),
        .resources = move(resources),
        .worklet_modules = move(worklet_modules),
        .worklet_node_definitions = move(worklet_node_definitions),
        .worklet_port_bindings = move(worklet_port_bindings),
    };

    update_media_stream_audio_source_bindings(*update.resources);

    if (update_media_element_stream_bindings(*update.resources, update.worklet_port_bindings))
        return;

    m_script_processor_stream_bindings.set_host(update.resources->script_processor_host());

    bool did_reschedule = m_script_processor_stream_bindings.update_stream_bindings_and_maybe_reschedule(
        engine,
        update,
        [this, weak_engine = engine.make_weak_ptr()](PendingClientRenderGraphUpdate&& retry_update) {
            if (!weak_engine) {
                close_worklet_port_binding_fds(retry_update.worklet_port_bindings);
                return;
            }

            update_client_render_graph(
                *weak_engine,
                retry_update.client_id,
                retry_update.graph_sample_rate,
                move(retry_update.encoded_graph),
                move(retry_update.resources),
                move(retry_update.worklet_modules),
                move(retry_update.worklet_node_definitions),
                move(retry_update.worklet_port_bindings));
        });
    if (did_reschedule)
        return;

    // The resources snapshot is used above to publish stream transports.
    // Other resource types are currently serialized into the encoded wire graph.

    // Publish AudioWorklet node definitions so the AudioServer-side worklet VM can eagerly
    // construct processor instances (even for unconnected nodes).
    {
        if (::Web::WebAudio::should_log_info()) {
            WA_DBGLN("[WebAudio] Publishing {} worklet node definition(s) to AudioServer", update.worklet_node_definitions.size());
            for (auto const& def : update.worklet_node_definitions)
                WA_DBGLN("[WebAudio]  - worklet node definition node_id={} processor='{}'", def.node_id.value(), def.processor_name);
        }

        auto defs_result = m_client->webaudio_session_set_worklet_node_definitions(m_session_id, update.worklet_node_definitions);
        if (defs_result.is_error())
            WA_DBGLN("[WebAudio] Failed to send worklet node definitions to AudioServer: {}", defs_result.error());
    }

    // Publish worklet node port transports before the graph update so the AudioServer-side
    // worklet VM can attach the processor MessagePort when the graph becomes active.
    {
        Vector<WorkletNodePortDescriptor> port_descriptors;
        port_descriptors.ensure_capacity(update.worklet_port_bindings.size());

        for (auto& binding : update.worklet_port_bindings) {
            if (binding.processor_port_fd < 0)
                continue;

            port_descriptors.append(WorkletNodePortDescriptor {
                .node_id = binding.node_id.value(),
                .processor_port_fd = IPC::File::adopt_fd(binding.processor_port_fd),
            });
            binding.processor_port_fd = -1;
        }

        if (::Web::WebAudio::should_log_info()) {
            WA_DBGLN("[WebAudio] Publishing {} worklet port binding(s) to AudioServer", port_descriptors.size());
            for (auto const& port : port_descriptors)
                WA_DBGLN("[WebAudio]  - worklet port binding node_id={}", port.node_id);
        }

        auto port_result = m_client->webaudio_session_set_worklet_node_ports(m_session_id, move(port_descriptors));
        if (port_result.is_error())
            WA_DBGLN("[WebAudio] Failed to send worklet port bindings to AudioServer: {}", port_result.error());
    }

    for (auto const& module : update.worklet_modules) {
        u64 const ipc_module_id = (client_id << 32) | (module.module_id & 0xffffffffu);
        if (::Web::WebAudio::should_log_info())
            WA_DBGLN("[WebAudio] Publishing worklet module id={} '{}' ({} bytes)", ipc_module_id, module.url, module.source_text.length());
        auto result = m_client->webaudio_session_add_worklet_module(m_session_id, ipc_module_id, module.url, module.source_text);
        if (result.is_error())
            WA_DBGLN("[WebAudio] Failed to send worklet module to AudioServer: {}", result.error());
    }

    auto graph_result = m_client->webaudio_session_set_render_graph(m_session_id, move(update.encoded_graph));
    if (graph_result.is_error())
        WA_DBGLN("[WebAudio] Failed to send render graph to AudioServer: {}", graph_result.error());
}

bool WebAudioWorkerSession::try_copy_analyser_snapshot(
    WebAudioClientRegistry& engine,
    u64,
    NodeID analyser_node_id,
    u32 fft_size,
    Span<f32> out_time_domain,
    Span<f32> out_frequency_db,
    u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    start_time_sync_notifier_if_needed(engine);

    if (!validate_analyser_snapshot_request(fft_size, out_time_domain, out_frequency_db))
        return false;

    if (m_session_id == 0 || !m_client)
        return false;

    {
        Threading::MutexLocker locker(m_remote_analyser_streams_mutex);
        auto it = m_remote_analyser_streams.find(analyser_node_id);
        if (it == m_remote_analyser_streams.end() || it->value.fft_size != fft_size || !it->value.stream.is_valid()) {
            auto stream_or_error = m_client->webaudio_session_create_analyser_stream(
                m_session_id,
                analyser_node_id.value(),
                fft_size,
                4);

            if (stream_or_error.is_error())
                return false;

            m_remote_analyser_streams.set(analyser_node_id, RemoteAnalyserStream { .fft_size = fft_size, .stream = stream_or_error.release_value() });
        }
    }

    Threading::MutexLocker locker(m_remote_analyser_streams_mutex);
    auto it = m_remote_analyser_streams.find(analyser_node_id);
    if (it == m_remote_analyser_streams.end())
        return false;
    auto& stream_state = it->value;
    if (!stream_state.stream.is_valid())
        return false;

    Optional<Core::SharedBufferStream::Descriptor> last_desc;
    while (true) {
        auto desc = stream_state.stream.try_receive_ready_block();
        if (!desc.has_value())
            break;
        if (last_desc.has_value())
            (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        last_desc = desc;
    }

    if (!last_desc.has_value())
        return false;

    auto block = stream_state.stream.block_bytes(last_desc->block_index);
    if (block.is_empty()) {
        (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        return false;
    }

    auto expected_size = webaudio_analyser_snapshot_size_bytes(fft_size);
    if (last_desc->used_size < expected_size || block.size() < expected_size) {
        (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        return false;
    }

    auto const* header = reinterpret_cast<WebAudioAnalyserSnapshotHeader const*>(block.data());
    if (header->version != webaudio_analyser_snapshot_version || header->fft_size != fft_size || header->analyser_node_id != analyser_node_id.value()) {
        (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        return false;
    }

    auto const* floats = reinterpret_cast<f32 const*>(header + 1);
    ReadonlySpan<f32> time_domain { floats, fft_size };
    ReadonlySpan<f32> frequency_db { floats + fft_size, fft_size / 2 };

    time_domain.copy_to(out_time_domain);
    if (!out_frequency_db.is_empty())
        frequency_db.copy_to(out_frequency_db);

    out_render_quantum_index = render_quantum_index_from_rendered_frames(header->rendered_frames_total);

    (void)stream_state.stream.try_release_block_index(last_desc->block_index);
    return true;
}

bool WebAudioWorkerSession::try_copy_dynamics_compressor_reduction(
    WebAudioClientRegistry& engine,
    u64,
    NodeID compressor_node_id,
    f32& out_reduction_db,
    u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    start_time_sync_notifier_if_needed(engine);

    if (m_session_id == 0 || !m_client)
        return false;

    {
        Threading::MutexLocker locker(m_remote_dynamics_compressor_streams_mutex);
        auto it = m_remote_dynamics_compressor_streams.find(compressor_node_id);
        if (it == m_remote_dynamics_compressor_streams.end() || !it->value.stream.is_valid()) {
            auto stream_or_error = m_client->webaudio_session_create_dynamics_compressor_stream(
                m_session_id,
                compressor_node_id.value(),
                4);

            if (stream_or_error.is_error())
                return false;

            m_remote_dynamics_compressor_streams.set(compressor_node_id, RemoteDynamicsCompressorStream { .stream = stream_or_error.release_value() });
        }
    }

    Threading::MutexLocker locker(m_remote_dynamics_compressor_streams_mutex);
    auto it = m_remote_dynamics_compressor_streams.find(compressor_node_id);
    if (it == m_remote_dynamics_compressor_streams.end())
        return false;
    auto& stream_state = it->value;
    if (!stream_state.stream.is_valid())
        return false;

    Optional<Core::SharedBufferStream::Descriptor> last_desc;
    while (true) {
        auto desc = stream_state.stream.try_receive_ready_block();
        if (!desc.has_value())
            break;
        if (last_desc.has_value())
            (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        last_desc = desc;
    }

    if (!last_desc.has_value())
        return false;

    auto block = stream_state.stream.block_bytes(last_desc->block_index);
    if (block.is_empty()) {
        (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        return false;
    }

    auto expected_size = Web::WebAudio::Render::webaudio_dynamics_compressor_snapshot_size_bytes();
    if (last_desc->used_size < expected_size || block.size() < expected_size) {
        (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        return false;
    }

    auto const* header = reinterpret_cast<Web::WebAudio::Render::WebAudioDynamicsCompressorSnapshotHeader const*>(block.data());
    if (header->version != Web::WebAudio::Render::webaudio_dynamics_compressor_snapshot_version || header->compressor_node_id != compressor_node_id.value()) {
        (void)stream_state.stream.try_release_block_index(last_desc->block_index);
        return false;
    }

    out_reduction_db = header->reduction_db;
    out_render_quantum_index = render_quantum_index_from_rendered_frames(header->rendered_frames_total);

    (void)stream_state.stream.try_release_block_index(last_desc->block_index);
    return true;
}

WebAudioWorkerSession::WebAudioWorkerSession(u64 client_id)
    : m_client_id(client_id)
{
}

WebAudioWorkerSession::~WebAudioWorkerSession() = default;

void WebAudioWorkerSession::set_webaudio_client(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient> client)
{
    s_webaudio_client = move(client);
}

RefPtr<WebAudioWorkerClient::WebAudioClient> WebAudioWorkerSession::webaudio_client()
{
    return s_webaudio_client;
}

bool WebAudioWorkerSession::has_output_open(WebAudioClientRegistry const&) const
{
    return m_webaudio_session.has_value();
}

u64 WebAudioWorkerSession::session_id() const
{
    return m_session_id;
}

void WebAudioWorkerSession::set_client_suspended(WebAudioClientRegistry& engine, u64 client_id, bool suspended, u64 generation)
{
    (void)engine;
    (void)client_id;
    ASSERT_CONTROL_THREAD();
    if (m_session_id == 0 || !m_client) {
        m_pending_suspend_state = PendingSuspendState { .suspended = suspended, .generation = generation };
        return;
    }
    m_pending_suspend_state = PendingSuspendState { .suspended = suspended, .generation = generation };
    (void)m_client->webaudio_session_set_suspended(m_session_id, suspended, generation);
}

}
