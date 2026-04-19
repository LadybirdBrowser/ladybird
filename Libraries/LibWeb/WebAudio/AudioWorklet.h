/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioWorklet.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/Script/WorkletModule.h>

namespace Web::WebAudio {

class BaseAudioContext;
class AudioWorkletEnvironmentSettingsObject;

// https://webaudio.github.io/web-audio-api/#audioworklet
class AudioWorklet final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioWorklet, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioWorklet);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<AudioWorklet> create(JS::Realm&, GC::Ref<BaseAudioContext>);
    virtual ~AudioWorklet() override = default;

    GC::Ref<WebIDL::Promise> add_module(String const& module_url);
    GC::Ref<HTML::MessagePort> port();
    bool is_processor_registered(String const& name);
    bool is_processor_registration_failed(String const& name);
    Vector<AudioParamDescriptor> const* parameter_descriptors(String const& name);
    void register_processor_from_worker(String const& name, Vector<AudioParamDescriptor> const& descriptors);
    void register_failed_processors_from_worker(Vector<String> const& names);
    void handle_module_evaluated(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message);
    void set_registration_generation(u64 generation);
    u64 registration_generation() const { return m_registration_generation; }

    bool has_loaded_any_module() const { return m_has_loaded_any_module; }
    bool has_pending_module_promises() const;
    bool needs_realtime_worklet_session() const;

    Vector<Render::WorkletModule> loaded_modules() const;

    // Tracks AudioWorkletNodes that exist on the control thread, independent of
    // whether they are currently connected into the destination render graph.
    // This is required because the processor instance is created at node
    // construction time, and messaging via node.port must work even when the
    // node is not connected.
    void register_realtime_node_definition(Render::WorkletNodeDefinition);
    void unregister_realtime_node_definition(NodeID);
    Vector<Render::WorkletNodeDefinition> realtime_node_definitions() const;
    Vector<NodeID> realtime_node_ids() const;

    // Realtime AudioContext uses an out-of-realm worklet VM (in WebAudioWorker).
    // We keep the processor-side transport handle until it is transferred to that VM.
    void set_realtime_processor_port(NodeID node_id, IPC::TransportHandle);
    Optional<IPC::TransportHandle> take_realtime_processor_port_handle(NodeID node_id);
    void set_realtime_global_port_handle(IPC::TransportHandle);
    Optional<IPC::TransportHandle> take_realtime_global_port_handle();
    void prune_realtime_processor_ports(Vector<NodeID> const& live_nodes);

    HTML::EnvironmentSettingsObject& worklet_environment_settings_object();

private:
    struct PendingModuleCompletion {
        u64 required_generation { 0 };
        bool success { false };
        String error_name;
        String error_message;
    };

    explicit AudioWorklet(JS::Realm&, GC::Ref<BaseAudioContext>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    HTML::EnvironmentSettingsObject& ensure_worklet_environment_settings_object();

    GC::Ref<BaseAudioContext> m_context;
    GC::Ptr<AudioWorkletEnvironmentSettingsObject> m_worklet_environment_settings_object;

    // Best-effort module source caching for mirroring into other VMs.
    HashMap<String, ByteString> m_loaded_module_sources;
    HashMap<String, u64> m_module_ids_by_url;
    HashMap<u64, String> m_pending_module_urls;
    HashMap<u64, Vector<GC::Ref<WebIDL::Promise>>> m_pending_module_promises;
    HashMap<u64, PendingModuleCompletion> m_pending_module_completions;
    HashTable<String> m_evaluated_module_urls;
    u64 m_next_module_id { 1 };

    bool m_has_loaded_any_module { false };
    u64 m_registration_generation { 0 };

    HashMap<NodeID, IPC::TransportHandle> m_realtime_processor_port_handles;
    HashMap<NodeID, Render::WorkletNodeDefinition> m_realtime_node_definitions;

    GC::Ptr<HTML::MessagePort> m_port;
    Optional<IPC::TransportHandle> m_realtime_global_port_handle;
};

}
