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
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioWorkletPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

class BaseAudioContext;
class AudioWorkletEnvironmentSettingsObject;

// https://webaudio.github.io/web-audio-api/#audioworklet
class AudioWorklet final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioWorklet, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioWorklet);

public:
    [[nodiscard]] static GC::Ref<AudioWorklet> create(JS::Realm&, GC::Ref<BaseAudioContext>);
    virtual ~AudioWorklet() override;

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
    // We keep a socket endpoint per AudioWorkletNode so node.port can exchange messages with the
    // processor-side MessagePort living in that worklet VM.
    void set_realtime_processor_port(NodeID node_id, int processor_port_fd);
    Optional<int> clone_realtime_processor_port_fd(NodeID node_id) const;
    void set_realtime_global_port_fd(int processor_port_fd);
    Optional<int> clone_realtime_global_port_fd() const;
    void prune_realtime_processor_ports(Vector<NodeID> const& live_nodes);

    HTML::EnvironmentSettingsObject& worklet_environment_settings_object();

private:
    explicit AudioWorklet(JS::Realm&, GC::Ref<BaseAudioContext>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    HTML::EnvironmentSettingsObject& ensure_worklet_environment_settings_object();

    GC::Ref<BaseAudioContext> m_context;
    GC::Ptr<AudioWorkletEnvironmentSettingsObject> m_worklet_environment_settings_object;

    // Best-effort module source caching for mirroring into other VMs.
    HashMap<String, ByteString> m_loaded_module_sources;
    HashMap<String, u64> m_module_ids_by_url;
    HashMap<u64, String> m_pending_module_urls;
    HashMap<u64, Vector<GC::Ref<WebIDL::Promise>>> m_pending_module_promises;
    HashMap<u64, u64> m_pending_module_generations;
    HashTable<String> m_evaluated_module_urls;
    u64 m_next_module_id { 1 };

    bool m_has_loaded_any_module { false };
    u64 m_registration_generation { 0 };

    HashMap<NodeID, int> m_realtime_processor_port_fds;
    HashMap<NodeID, Render::WorkletNodeDefinition> m_realtime_node_definitions;

    GC::Ptr<HTML::MessagePort> m_port;
    int m_realtime_global_port_fd { -1 };
};

}
