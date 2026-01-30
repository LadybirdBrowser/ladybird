/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Notifier.h>
#include <LibCore/SharedBufferStream.h>
#include <LibCore/Timer.h>
#include <LibWeb/WebAudio/Engine/FlowControl.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorRequestPump.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>

namespace WebAudioWorkerClient {

class WebAudioClient;

}

namespace Web::WebAudio::Render {

class WebAudioClientRegistry;

struct PendingClientRenderGraphUpdate {
    u64 client_id { 0 };
    f32 graph_sample_rate { 0 };
    ByteBuffer encoded_graph;
    NonnullOwnPtr<GraphResourceRegistry> resources;
    Vector<WorkletModule> worklet_modules;
    Vector<WorkletNodeDefinition> worklet_node_definitions;
    Vector<WorkletPortBinding> worklet_port_bindings;
};

class ScriptProcessorStreamBindings {
public:
    ScriptProcessorStreamBindings();
    ~ScriptProcessorStreamBindings();

    void set_webaudio_session(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient> const& client, u64 session_id);
    void clear_webaudio_session();

    void set_host(ScriptProcessorHost* host) { m_request_pump.set_host(host); }

    bool published_bindings() const { return m_published_script_processor_stream_bindings; }

    bool update_stream_bindings_and_maybe_reschedule(
        WebAudioClientRegistry& engine,
        PendingClientRenderGraphUpdate& update,
        Function<void(PendingClientRenderGraphUpdate&&)>&& retry_graph_update);

private:
    struct RemoteScriptProcessorStreams final : public RefCounted<RemoteScriptProcessorStreams>
        , public ScriptProcessorNodeState {

        Core::AnonymousBuffer request_pool_buffer;
        Core::AnonymousBuffer request_ready_ring_buffer;
        Core::AnonymousBuffer request_free_ring_buffer;

        Core::AnonymousBuffer response_pool_buffer;
        Core::AnonymousBuffer response_ready_ring_buffer;
        Core::AnonymousBuffer response_free_ring_buffer;

        int notify_read_fd { -1 };
        int notify_write_fd { -1 };
        RefPtr<Core::Notifier> notifier;
    };

    void schedule_publish_retry_with_graph_update(WebAudioClientRegistry& engine, PendingClientRenderGraphUpdate&& update, Function<void(PendingClientRenderGraphUpdate&&)>&& retry_graph_update);
    void schedule_publish_retry_only(WebAudioClientRegistry& engine);
    void schedule_publish_retry(WebAudioClientRegistry& engine);

    TransactionalPublishOutcome try_publish_bindings_for_nodes(Vector<NodeID> const& node_ids);
    TransactionalPublishOutcome try_publish_bindings_for_remote_state();
    void did_publish_bindings(bool have_script_processors);

    void drain_notify_fd_and_process(NodeID node_id);

    RefPtr<WebAudioWorkerClient::WebAudioClient> m_client;
    u64 m_session_id { 0 };

    RefPtr<Core::Timer> m_publish_retry_timer;
    Optional<PendingClientRenderGraphUpdate> m_pending_graph_update_for_retry;
    Function<void(PendingClientRenderGraphUpdate&&)> m_retry_graph_update;
    u32 m_publish_retry_attempts { 0 };

    HashMap<NodeID, NonnullRefPtr<RemoteScriptProcessorStreams>> m_remote_script_processors;

    bool m_published_script_processor_stream_bindings { false };

    ScriptProcessorRequestPump m_request_pump;
};

}
