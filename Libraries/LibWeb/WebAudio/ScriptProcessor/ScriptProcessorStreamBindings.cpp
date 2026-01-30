/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibCore/EventLoop.h>
#include <LibCore/SharedBufferStream.h>
#include <LibCore/System.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/FlowControl.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>
#include <LibWeb/WebAudio/Engine/StreamTransportEventFD.h>
#include <LibWeb/WebAudio/Engine/WebAudioClientRegistry.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorStreamBindings.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorTransport.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>
#include <LibWebAudioWorkerClient/WebAudioClient.h>

namespace Web::WebAudio::Render {

static size_t ring_capacity_for_descriptor_stream(u32 block_count)
{
    size_t required = static_cast<size_t>(block_count) * sizeof(Core::SharedBufferStream::Descriptor);
    size_t capacity = 64;
    while (capacity < required)
        capacity <<= 1;
    return capacity;
}

struct SharedBufferStreamBuffers {
    Core::AnonymousBuffer pool_buffer;
    Core::AnonymousBuffer ready_ring_buffer;
    Core::AnonymousBuffer free_ring_buffer;
    Core::SharedBufferStream stream;
};

static Optional<SharedBufferStreamBuffers> create_shared_buffer_stream_buffers(u32 block_size, u32 block_count, char const* debug_name)
{
    if (block_size == 0 || block_count == 0)
        return {};

    auto pool_bytes = Core::SharedBufferStream::pool_buffer_size_bytes(block_size, block_count);
    auto pool_buffer_or_error = Core::AnonymousBuffer::create_with_size(pool_bytes);
    if (pool_buffer_or_error.is_error()) {
        if (::Web::WebAudio::should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] audioserver: failed to allocate {} stream pool: {}", debug_name, pool_buffer_or_error.error());
        return {};
    }

    auto pool_buffer = pool_buffer_or_error.release_value();
    auto* header = reinterpret_cast<Core::SharedBufferStream::PoolHeader*>(pool_buffer.data<void>());
    if (!header)
        return {};

    __builtin_memset(header, 0, sizeof(*header));
    header->magic = Core::SharedBufferStream::s_pool_magic;
    header->version = Core::SharedBufferStream::s_pool_version;
    header->block_size = block_size;
    header->block_count = block_count;

    size_t ring_capacity_bytes = ring_capacity_for_descriptor_stream(block_count);

    auto ready_ring_or_error = Core::SharedSingleProducerCircularBuffer::create(ring_capacity_bytes);
    if (ready_ring_or_error.is_error()) {
        if (::Web::WebAudio::should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] audioserver: failed to create {} stream ready ring: {}", debug_name, ready_ring_or_error.error());
        return {};
    }

    auto free_ring_or_error = Core::SharedSingleProducerCircularBuffer::create(ring_capacity_bytes);
    if (free_ring_or_error.is_error()) {
        if (::Web::WebAudio::should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] audioserver: failed to create {} stream free ring: {}", debug_name, free_ring_or_error.error());
        return {};
    }

    auto ready_ring = ready_ring_or_error.release_value();
    auto free_ring = free_ring_or_error.release_value();

    for (u32 i = 0; i < block_count; ++i) {
        Core::SharedBufferStream::Descriptor desc { i, 0 };
        auto bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(&desc), sizeof(desc) };
        if (free_ring.try_write(bytes) != sizeof(desc)) {
            if (::Web::WebAudio::should_log_output_driver())
                WA_OUT_DBGLN("[WebAudio] audioserver: failed to seed {} stream free ring (i={})", debug_name, i);
            return {};
        }
    }

    auto ready_ring_buffer = ready_ring.anonymous_buffer();
    auto free_ring_buffer = free_ring.anonymous_buffer();

    auto stream_or_error = Core::SharedBufferStream::attach(pool_buffer, ready_ring_buffer, free_ring_buffer);
    if (stream_or_error.is_error()) {
        if (::Web::WebAudio::should_log_output_driver())
            WA_OUT_DBGLN("[WebAudio] audioserver: failed to attach {} stream: {}", debug_name, stream_or_error.error());
        return {};
    }

    return SharedBufferStreamBuffers {
        .pool_buffer = pool_buffer,
        .ready_ring_buffer = ready_ring_buffer,
        .free_ring_buffer = free_ring_buffer,
        .stream = stream_or_error.release_value(),
    };
}

ScriptProcessorStreamBindings::ScriptProcessorStreamBindings() = default;
ScriptProcessorStreamBindings::~ScriptProcessorStreamBindings() = default;

void ScriptProcessorStreamBindings::set_webaudio_session(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient> const& client, u64 session_id)
{
    m_client = client;
    m_session_id = session_id;
    m_published_script_processor_stream_bindings = false;
}

void ScriptProcessorStreamBindings::clear_webaudio_session()
{
    if (m_publish_retry_timer) {
        m_publish_retry_timer->stop();
        m_publish_retry_timer = nullptr;
    }

    if (m_pending_graph_update_for_retry.has_value())
        close_worklet_port_binding_fds(m_pending_graph_update_for_retry->worklet_port_bindings);

    m_pending_graph_update_for_retry.clear();
    m_retry_graph_update = {};
    m_publish_retry_attempts = 0;

    for (auto& it : m_remote_script_processors) {
        auto& state = it.value;
        if (state->notifier)
            state->notifier->set_enabled(false);
        if (state->notify_read_fd >= 0)
            (void)Core::System::close(state->notify_read_fd);
        if (state->notify_write_fd >= 0)
            (void)Core::System::close(state->notify_write_fd);
        state->notify_read_fd = -1;
        state->notify_write_fd = -1;
        state->notifier = nullptr;
    }
    m_remote_script_processors.clear();

    m_session_id = 0;
    m_client = nullptr;
    m_published_script_processor_stream_bindings = false;
}

void ScriptProcessorStreamBindings::schedule_publish_retry_with_graph_update(WebAudioClientRegistry& engine, PendingClientRenderGraphUpdate&& update, Function<void(PendingClientRenderGraphUpdate&&)>&& retry_graph_update)
{
    ASSERT_CONTROL_THREAD();

    if (!Core::EventLoop::is_running()) {
        close_worklet_port_binding_fds(update.worklet_port_bindings);
        return;
    }
    if (m_session_id == 0 || !m_client) {
        close_worklet_port_binding_fds(update.worklet_port_bindings);
        return;
    }

    if (m_pending_graph_update_for_retry.has_value())
        close_worklet_port_binding_fds(m_pending_graph_update_for_retry->worklet_port_bindings);

    m_pending_graph_update_for_retry = move(update);
    m_retry_graph_update = move(retry_graph_update);

    schedule_publish_retry(engine);
}

void ScriptProcessorStreamBindings::schedule_publish_retry_only(WebAudioClientRegistry& engine)
{
    ASSERT_CONTROL_THREAD();

    if (!Core::EventLoop::is_running())
        return;
    if (m_session_id == 0 || !m_client)
        return;

    if (m_pending_graph_update_for_retry.has_value())
        close_worklet_port_binding_fds(m_pending_graph_update_for_retry->worklet_port_bindings);
    m_pending_graph_update_for_retry.clear();
    m_retry_graph_update = {};

    schedule_publish_retry(engine);
}

void ScriptProcessorStreamBindings::schedule_publish_retry(WebAudioClientRegistry& engine)
{
    ASSERT_CONTROL_THREAD();

    if (!m_publish_retry_timer) {
        m_publish_retry_timer = Core::Timer::create_single_shot(Render::SCRIPT_PROCESSOR_PUBLISH_RETRY_INTERVAL_MS, [this, weak_engine = engine.make_weak_ptr()] {
            if (!weak_engine)
                return;
            ASSERT_CONTROL_THREAD();

            if (m_publish_retry_attempts++ > Render::SCRIPT_PROCESSOR_PUBLISH_RETRY_MAX_ATTEMPTS) {
                WA_DBGLN("[WebAudio] ScriptProcessor stream publish retry giving up (session={})", m_session_id);
                if (m_pending_graph_update_for_retry.has_value())
                    close_worklet_port_binding_fds(m_pending_graph_update_for_retry->worklet_port_bindings);
                m_pending_graph_update_for_retry.clear();
                m_retry_graph_update = {};
                return;
            }

            if (m_pending_graph_update_for_retry.has_value()) {
                auto pending = m_pending_graph_update_for_retry.release_value();
                m_pending_graph_update_for_retry.clear();
                auto retry_cb = move(m_retry_graph_update);
                m_retry_graph_update = {};

                WA_DBGLN("[WebAudio] ScriptProcessor stream publish retry firing (session={} attempt={} kind=graph_update)", m_session_id, m_publish_retry_attempts);
                if (retry_cb)
                    retry_cb(move(pending));
                return;
            }

            WA_DBGLN("[WebAudio] ScriptProcessor stream publish retry firing (session={} attempt={} kind=publish_only)", m_session_id, m_publish_retry_attempts);
            auto outcome = try_publish_bindings_for_remote_state();
            if (outcome != TransactionalPublishOutcome::Published)
                schedule_publish_retry(*weak_engine);
        });
    }

    if (!m_publish_retry_timer->is_active())
        m_publish_retry_timer->start();
}

TransactionalPublishOutcome ScriptProcessorStreamBindings::try_publish_bindings_for_remote_state()
{
    ASSERT_CONTROL_THREAD();

    Vector<NodeID> node_ids;
    node_ids.ensure_capacity(m_remote_script_processors.size());
    for (auto const& it : m_remote_script_processors)
        node_ids.append(it.key);

    return try_publish_bindings_for_nodes(node_ids);
}

void ScriptProcessorStreamBindings::did_publish_bindings(bool have_script_processors)
{
    m_published_script_processor_stream_bindings = have_script_processors;
    if (m_publish_retry_timer)
        m_publish_retry_timer->stop();

    if (m_pending_graph_update_for_retry.has_value())
        close_worklet_port_binding_fds(m_pending_graph_update_for_retry->worklet_port_bindings);

    m_pending_graph_update_for_retry.clear();
    m_retry_graph_update = {};
    m_publish_retry_attempts = 0;
}

TransactionalPublishOutcome ScriptProcessorStreamBindings::try_publish_bindings_for_nodes(Vector<NodeID> const& node_ids)
{
    ASSERT_CONTROL_THREAD();

    if (!m_client || m_session_id == 0)
        return TransactionalPublishOutcome::Failed;

    bool const have_script_processors = !node_ids.is_empty();

    Vector<ScriptProcessorStreamDescriptor> script_processor_stream_bindings;
    script_processor_stream_bindings.ensure_capacity(node_ids.size());

    bool skipped_any = false;

    for (auto node_id : node_ids) {
        auto it = m_remote_script_processors.find(node_id);
        if (it == m_remote_script_processors.end()) {
            skipped_any = true;
            continue;
        }

        auto const& state = it->value;

        auto notify_write_fd_or_error = IPC::File::clone_fd(state->notify_write_fd);
        if (notify_write_fd_or_error.is_error()) {
            skipped_any = true;
            WA_DBGLN("[WebAudio] ScriptProcessor notify fd clone failed (session={} node={} error={})", m_session_id, node_id, notify_write_fd_or_error.error());
            continue;
        }

        ScriptProcessorStreamDescriptor desc;
        desc.node_id = node_id.value();
        desc.buffer_size = state->buffer_size;
        desc.input_channel_count = state->input_channel_count;
        desc.output_channel_count = state->output_channel_count;
        desc.request_stream = SharedBufferStreamDescriptor {
            .pool_buffer = state->request_pool_buffer,
            .ready_ring_buffer = state->request_ready_ring_buffer,
            .free_ring_buffer = state->request_free_ring_buffer,
        };
        desc.response_stream = SharedBufferStreamDescriptor {
            .pool_buffer = state->response_pool_buffer,
            .ready_ring_buffer = state->response_ready_ring_buffer,
            .free_ring_buffer = state->response_free_ring_buffer,
        };
        desc.request_notify_write_fd = notify_write_fd_or_error.release_value();

        script_processor_stream_bindings.append(move(desc));
    }

    auto outcome = transactional_publish_bindings(
        true,
        have_script_processors,
        true,
        move(script_processor_stream_bindings),
        skipped_any,
        [this](Vector<ScriptProcessorStreamDescriptor>&& bindings) {
            auto result = m_client->webaudio_session_set_script_processor_streams(m_session_id, move(bindings));
            if (result.is_error()) {
                WA_DBGLN("[WebAudio] Failed to send ScriptProcessor stream bindings to AudioServer: {}", result.error());
                return false;
            }
            return true;
        });

    if (outcome == TransactionalPublishOutcome::Published)
        did_publish_bindings(have_script_processors);

    return outcome;
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

void ScriptProcessorStreamBindings::drain_notify_fd_and_process(NodeID node_id)
{
    ASSERT_CONTROL_THREAD();

    auto it = m_remote_script_processors.find(node_id);
    if (it == m_remote_script_processors.end())
        return;

    // Hold a strong reference while processing. ScriptProcessor processing can run JS and trigger
    // graph updates that remove/recreate bindings re-entrantly.
    NonnullRefPtr<RemoteScriptProcessorStreams> state = it->value;

    if (state->notify_read_fd < 0)
        return;

    auto drain_result = drain_nonblocking_notify_fd(state->notify_read_fd);
    if (drain_result != DrainNotifyFDResult::Drained) {
        if (state->notifier)
            state->notifier->set_enabled(false);
        return;
    }

    m_request_pump.process(node_id, *state);
}

bool ScriptProcessorStreamBindings::update_stream_bindings_and_maybe_reschedule(
    WebAudioClientRegistry& engine,
    PendingClientRenderGraphUpdate& update,
    Function<void(PendingClientRenderGraphUpdate&&)>&& retry_graph_update)
{
    ASSERT_CONTROL_THREAD();

    HashTable<NodeID> seen_script_processor_node_ids;
    Vector<NodeID> script_processor_node_ids_in_graph;
    bool should_publish_script_processor_stream_bindings = false;

    auto const& script_processors = update.resources->script_processor_transport_metadata();
    seen_script_processor_node_ids.ensure_capacity(script_processors.size());
    script_processor_node_ids_in_graph.ensure_capacity(script_processors.size());

    if (!script_processors.is_empty() && !m_published_script_processor_stream_bindings)
        should_publish_script_processor_stream_bindings = true;

    if (::Web::WebAudio::should_log_info()) {
        WA_DBGLN("[WebAudio] ScriptProcessor stream bindings state: nodes_in_graph={} publish_requested={} published_already={}",
            script_processors.size(),
            should_publish_script_processor_stream_bindings,
            m_published_script_processor_stream_bindings);
    }

    for (auto const& it : script_processors) {
        auto node_id = it.key;
        auto const& meta = it.value;
        script_processor_node_ids_in_graph.unchecked_append(node_id);
        seen_script_processor_node_ids.set(node_id);

        auto existing = m_remote_script_processors.find(node_id);
        bool needs_new_streams = (existing == m_remote_script_processors.end())
            || (existing->value->buffer_size != meta.buffer_size || existing->value->input_channel_count != meta.input_channel_count || existing->value->output_channel_count != meta.output_channel_count);

        if (!needs_new_streams)
            continue;

        if (existing != m_remote_script_processors.end()) {
            if (existing->value->notifier)
                existing->value->notifier->set_enabled(false);
            if (existing->value->notify_read_fd >= 0)
                (void)Core::System::close(existing->value->notify_read_fd);
            if (existing->value->notify_write_fd >= 0)
                (void)Core::System::close(existing->value->notify_write_fd);
            existing->value->notify_read_fd = -1;
            existing->value->notify_write_fd = -1;
            existing->value->notifier = nullptr;
            m_remote_script_processors.remove(existing);
        }

        u32 request_block_bytes = static_cast<u32>(Render::script_processor_request_fixed_bytes + ((static_cast<size_t>(meta.buffer_size) * static_cast<size_t>(meta.input_channel_count)) * sizeof(f32)));
        u32 response_block_bytes = static_cast<u32>(Render::script_processor_response_fixed_bytes + ((static_cast<size_t>(meta.buffer_size) * static_cast<size_t>(meta.output_channel_count)) * sizeof(f32)));

        constexpr u32 script_processor_stream_block_count = 32;

        auto request_buffers = create_shared_buffer_stream_buffers(request_block_bytes, script_processor_stream_block_count, "script processor request");
        if (!request_buffers.has_value()) {
            WA_DBGLN("[WebAudio] ScriptProcessor stream buffers not available yet (session={} node={} req_block_bytes={} blocks={})", m_session_id, node_id, request_block_bytes, script_processor_stream_block_count);
            continue;
        }

        auto response_buffers = create_shared_buffer_stream_buffers(response_block_bytes, script_processor_stream_block_count, "script processor response");
        if (!response_buffers.has_value()) {
            WA_DBGLN("[WebAudio] ScriptProcessor stream buffers not available yet (session={} node={} resp_block_bytes={} blocks={})", m_session_id, node_id, response_block_bytes, script_processor_stream_block_count);
            continue;
        }

        auto notify_fds_or_error = create_nonblocking_stream_notify_fds();
        if (notify_fds_or_error.is_error()) {
            WA_DBGLN("[WebAudio] ScriptProcessor notify fds not available yet (session={} node={} error={})", m_session_id, node_id, notify_fds_or_error.error());
            continue;
        }

        auto notify_fds = notify_fds_or_error.release_value();
        int notify_read_fd = notify_fds.read_fd;
        int notify_write_fd = notify_fds.write_fd;

        RefPtr<Core::Notifier> notifier = Core::Notifier::construct(notify_read_fd, Core::Notifier::Type::Read);
        notifier->on_activation = [this, weak_engine = engine.make_weak_ptr(), node_id] {
            if (!weak_engine)
                return;
            ASSERT_CONTROL_THREAD();
            drain_notify_fd_and_process(node_id);
        };

        auto state = adopt_ref(*new RemoteScriptProcessorStreams);
        state->buffer_size = meta.buffer_size;
        state->input_channel_count = meta.input_channel_count;
        state->output_channel_count = meta.output_channel_count;

        state->request_pool_buffer = request_buffers->pool_buffer;
        state->request_ready_ring_buffer = request_buffers->ready_ring_buffer;
        state->request_free_ring_buffer = request_buffers->free_ring_buffer;
        state->request_stream = request_buffers->stream;

        state->response_pool_buffer = response_buffers->pool_buffer;
        state->response_ready_ring_buffer = response_buffers->ready_ring_buffer;
        state->response_free_ring_buffer = response_buffers->free_ring_buffer;
        state->response_stream = response_buffers->stream;

        state->notify_read_fd = notify_read_fd;
        state->notify_write_fd = notify_write_fd;
        state->notifier = notifier;

        state->output_scratch.resize(static_cast<size_t>(meta.buffer_size) * static_cast<size_t>(meta.output_channel_count));

        m_remote_script_processors.set(node_id, move(state));
        should_publish_script_processor_stream_bindings = true;
    }

    bool removed_any = remove_entries_not_in(m_remote_script_processors, seen_script_processor_node_ids, [](NonnullRefPtr<RemoteScriptProcessorStreams>& state) {
        if (state->notifier)
            state->notifier->set_enabled(false);
        if (state->notify_read_fd >= 0)
            (void)Core::System::close(state->notify_read_fd);
        if (state->notify_write_fd >= 0)
            (void)Core::System::close(state->notify_write_fd);
        state->notify_read_fd = -1;
        state->notify_write_fd = -1;
        state->notifier = nullptr;
    });
    if (removed_any)
        should_publish_script_processor_stream_bindings = true;

    bool const have_script_processors = !script_processors.is_empty();

    if (should_publish_script_processor_stream_bindings || (have_script_processors && !m_published_script_processor_stream_bindings)) {
        auto outcome = try_publish_bindings_for_nodes(script_processor_node_ids_in_graph);
        if (outcome == TransactionalPublishOutcome::RetryLater) {
            schedule_publish_retry_with_graph_update(engine, move(update), move(retry_graph_update));
            return true;
        }
        if (outcome == TransactionalPublishOutcome::Failed)
            schedule_publish_retry_only(engine);
    }

    return false;
}

}
