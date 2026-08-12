/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/AudioWorkletGlobalScope.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

using AudioParamDescriptor = Bindings::AudioParamDescriptor;

// https://webaudio.github.io/web-audio-api/#AudioWorkletGlobalScope
class WEB_API AudioWorkletGlobalScope final : public HTML::WorkletGlobalScope {
    WEB_WRAPPABLE(AudioWorkletGlobalScope, HTML::WorkletGlobalScope);
    GC_DECLARE_ALLOCATOR(AudioWorkletGlobalScope);

public:
    static GC::Ref<AudioWorkletGlobalScope> create(GC::Ref<AudioWorklet>, float sample_rate);

    virtual ~AudioWorkletGlobalScope() override;

    WebIDL::ExceptionOr<void> register_processor(Utf16String name, GC::Ref<WebIDL::CallbackType> processor_ctor);
    u64 current_frame() const { return m_current_frame; }
    double current_time() const { return m_current_time; }
    float sample_rate() const { return m_sample_rate; }
    WebIDL::UnsignedLong render_quantum_size() const { return m_quantum_size; }
    HTML::MessagePort* port() { return m_port.ptr(); }
    void set_port(GC::Ref<HTML::MessagePort> port) { m_port = port; }

    // https://webaudio.github.io/web-audio-api/#node-name-to-processor-constructor-map
    struct ProcessorDefinition {
        GC::Ref<JS::FunctionObject> constructor;
        Vector<AudioParamDescriptor> parameter_descriptors;
    };
    ProcessorDefinition const* find_definition(Utf16String const& name) const;

    // https://webaudio.github.io/web-audio-api/#pending-processor-construction-data
    // Set by AudioWorkletNode instantiation immediately before invoking the processor constructor;
    // consumed by AudioWorkletProcessor's constructor.
    struct PendingProcessorConstructionData {
        GC::Ref<HTML::MessagePort> port;
    };
    Optional<PendingProcessorConstructionData> take_pending_processor_construction_data();
    void set_pending_processor_construction_data(PendingProcessorConstructionData);

    // Advanced by the render pump as it processes quanta; read by the currentFrame/currentTime getters.
    void set_current_frame_and_time(u64 current_frame, double current_time)
    {
        m_current_frame = current_frame;
        m_current_time = current_time;
    }

    // Defined by the generated bindings (installs the AudioWorklet-exposed interfaces and the
    // global mixin members on the wrapper).
    virtual void initialize_web_interfaces_impl() override;

private:
    AudioWorkletGlobalScope(GC::Ref<AudioWorklet>, float sample_rate);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<AudioWorklet> m_worklet;
    GC::Ptr<HTML::MessagePort> m_port;

    OrderedHashMap<Utf16String, ProcessorDefinition> m_processor_definitions;
    Optional<PendingProcessorConstructionData> m_pending_processor_construction_data;

    u64 m_current_frame { 0 };
    double m_current_time { 0 };
    float m_sample_rate { 0 };
    WebIDL::UnsignedLong m_quantum_size { 128 };
};

}
