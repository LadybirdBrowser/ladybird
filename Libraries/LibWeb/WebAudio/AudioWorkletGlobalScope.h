/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

class AudioWorkletGlobalScope final : public HTML::WorkletGlobalScope {
    WEB_PLATFORM_OBJECT(AudioWorkletGlobalScope, HTML::WorkletGlobalScope);
    GC_DECLARE_ALLOCATOR(AudioWorkletGlobalScope);

public:
    [[nodiscard]] static GC::Ref<AudioWorkletGlobalScope> create(JS::Realm&);
    virtual ~AudioWorkletGlobalScope() override;

    void initialize_web_interfaces();

    WebIDL::ExceptionOr<void> register_processor(String const& name, JS::Value processor_constructor);
    void mark_processor_registration_failed(String const& name);
    bool is_processor_registration_failed(String const& name) const;
    bool is_processor_registered(String const& name) const;
    void register_processor_name(String const& name);
    Vector<String> take_failed_processor_registrations();

    JS::Value processor_constructor(String const& name) const;

    void set_pending_processor_port(GC::Ref<HTML::MessagePort>);
    GC::Ptr<HTML::MessagePort> take_pending_processor_port();

    void set_current_frame(u64 current_frame) { m_current_frame = current_frame; }
    void set_sample_rate(float sample_rate) { m_sample_rate = sample_rate; }
    void set_shared_port(GC::Ref<HTML::MessagePort> port) { m_shared_port = port; }

    u64 current_frame() const { return m_current_frame; }
    double current_time() const
    {
        if (m_sample_rate <= 0.0f)
            return 0.0;
        return static_cast<double>(m_current_frame) / static_cast<double>(m_sample_rate);
    }
    float sample_rate() const { return m_sample_rate; }
    GC::Ptr<HTML::MessagePort> shared_port() const { return m_shared_port; }

    Vector<AudioParamDescriptor> const* parameter_descriptors(String const& name) const;
    void set_parameter_descriptors(String const& name, Vector<AudioParamDescriptor>&&);
    void unregister_processor(String const& name);

    void set_processor_registration_callback(Function<void(String const&, Vector<AudioParamDescriptor> const&)> callback)
    {
        m_processor_registration_callback = move(callback);
    }

private:
    explicit AudioWorkletGlobalScope(JS::Realm&);

    JS_DECLARE_NATIVE_FUNCTION(current_frame_getter);
    JS_DECLARE_NATIVE_FUNCTION(current_time_getter);
    JS_DECLARE_NATIVE_FUNCTION(sample_rate_getter);
    JS_DECLARE_NATIVE_FUNCTION(port_getter);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    HashMap<FlyString, JS::Value> m_registered_processors;
    HashMap<FlyString, Vector<AudioParamDescriptor>> m_parameter_descriptors;
    HashTable<FlyString> m_failed_processor_registrations;

    Function<void(String const&, Vector<AudioParamDescriptor> const&)> m_processor_registration_callback;

    GC::Ptr<HTML::MessagePort> m_pending_processor_port;

    u64 m_current_frame { 0 };
    float m_sample_rate { 44100.0f };
    GC::Ptr<HTML::MessagePort> m_shared_port;
};

}
