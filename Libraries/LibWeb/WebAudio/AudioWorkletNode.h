/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioWorkletNodePrototype.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebAudio/AudioNode.h>

#include <LibGC/Root.h>

namespace Web::WebAudio {

class AudioParamMap;

// https://webaudio.github.io/web-audio-api/#AudioWorkletNodeOptions
struct AudioWorkletNodeOptions : AudioNodeOptions {
    WebIDL::UnsignedLong number_of_inputs { 1 };
    WebIDL::UnsignedLong number_of_outputs { 1 };
    Optional<Vector<WebIDL::UnsignedLong>> output_channel_count;
    Optional<OrderedHashMap<String, double>> parameter_data;
    Optional<GC::Root<JS::Object>> processor_options;
};

// https://webaudio.github.io/web-audio-api/#AudioWorkletNode
class AudioWorkletNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(AudioWorkletNode, AudioNode);
    GC_DECLARE_ALLOCATOR(AudioWorkletNode);

public:
    virtual ~AudioWorkletNode() override;

    static WebIDL::ExceptionOr<GC::Ref<AudioWorkletNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, String const& name, AudioWorkletNodeOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<AudioWorkletNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, String const& name, AudioWorkletNodeOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return m_number_of_inputs; }
    WebIDL::UnsignedLong number_of_outputs() override { return m_number_of_outputs; }

    GC::Ptr<WebIDL::CallbackType> onprocessorerror();
    void set_onprocessorerror(GC::Ptr<WebIDL::CallbackType>);

    String const& name() const { return m_name; }
    Optional<Vector<size_t>> const& output_channel_count() const { return m_output_channel_count; }
    JS::Object* processor_instance() const { return m_processor_instance; }

    GC::Ref<HTML::MessagePort> port() const { return m_port; }
    GC::Ref<AudioParamMap> parameters() const { return m_parameters; }

private:
    AudioWorkletNode(JS::Realm&, GC::Ref<BaseAudioContext>, String const& name, AudioWorkletNodeOptions const&, Optional<Vector<size_t>> output_channel_count, HTML::MessagePort&, AudioParamMap&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    String m_name;
    WebIDL::UnsignedLong m_number_of_inputs { 0 };
    WebIDL::UnsignedLong m_number_of_outputs { 0 };

    Optional<Vector<size_t>> m_output_channel_count;

    GC::Ref<HTML::MessagePort> m_port;
    GC::Ref<AudioParamMap> m_parameters;

    GC::Ptr<JS::Object> m_processor_instance;
};

}
