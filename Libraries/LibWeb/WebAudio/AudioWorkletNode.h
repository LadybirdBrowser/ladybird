/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/AudioWorkletNode.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio::Rendering {

class AudioWorkletPipe;

}

namespace Web::WebAudio {

using AudioWorkletNodeOptions = Bindings::AudioWorkletNodeOptions;

// https://webaudio.github.io/web-audio-api/#AudioWorkletNode
class WEB_API AudioWorkletNode final : public AudioNode {
    WEB_WRAPPABLE(AudioWorkletNode, AudioNode);
    GC_DECLARE_ALLOCATOR(AudioWorkletNode);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static WebIDL::ExceptionOr<GC::Ref<AudioWorkletNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, Utf16String const& name, AudioWorkletNodeOptions const&);

    virtual ~AudioWorkletNode() override;

    GC::Ref<AudioParamMap> parameters() const { return m_parameter_map; }
    GC::Ref<HTML::MessagePort> port() const { return m_port; }

    void set_onprocessorerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onprocessorerror();

    virtual WebIDL::UnsignedLong number_of_inputs() override { return m_number_of_inputs; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return m_number_of_outputs; }

    void fire_processor_error(JS::Value error);

    GC::Ptr<JS::Object> processor() const { return m_processor; }

private:
    AudioWorkletNode(GC::Ref<BaseAudioContext>, Utf16String name, WebIDL::UnsignedLong number_of_inputs, WebIDL::UnsignedLong number_of_outputs, GC::Ref<AudioParamMap>, GC::Ref<HTML::MessagePort> port);

    // "invoke processor constructor" — deferred to a queued task so the node constructor returns
    // before user code runs (and pre-constructor port messages queue up for delivery afterwards).
    void invoke_processor_constructor(GC::Ref<AudioWorkletGlobalScope>, GC::Ref<HTML::MessagePort> processor_port, GC::Ref<JS::Object> options_object);

    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-audioworkletnode-context-name-options-name
    Utf16String m_name;

    WebIDL::UnsignedLong m_number_of_inputs { 1 };
    WebIDL::UnsignedLong m_number_of_outputs { 1 };
    GC::Ref<AudioParamMap> m_parameter_map;
    GC::Ref<HTML::MessagePort> m_port;

    // The user's AudioWorkletProcessor instance (worklet realm). Keeps the processor alive for the
    // lifetime of the node; the render pump reads it through the processor slot registry.
    GC::Ptr<JS::Object> m_processor;
    bool m_processor_errored { false };

    RefPtr<Rendering::AudioWorkletPipe> m_pipe;
};

}
