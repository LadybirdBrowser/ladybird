/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioNodePrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioNodeOptions
struct AudioNodeOptions {
    Optional<WebIDL::UnsignedLong> channel_count;
    Optional<Bindings::ChannelCountMode> channel_count_mode;
    Optional<Bindings::ChannelInterpretation> channel_interpretation;
};

struct AudioNodeDefaultOptions {
    WebIDL::UnsignedLong channel_count;
    Bindings::ChannelCountMode channel_count_mode;
    Bindings::ChannelInterpretation channel_interpretation;
};

struct AudioNodeConnection {
    GC::Ref<AudioNode> destination_node;
    WebIDL::UnsignedLong output;
    WebIDL::UnsignedLong input;

    bool operator==(AudioNodeConnection const& other) const = default;
};

struct AudioParamConnection {
    GC::Ref<AudioParam> destination_param;
    WebIDL::UnsignedLong output;

    bool operator==(AudioParamConnection const& other) const = default;
};

// https://webaudio.github.io/web-audio-api/#AudioNode
class AudioNode : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(AudioNode, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(AudioNode);

public:
    virtual ~AudioNode() override;

    WebIDL::ExceptionOr<GC::Ref<AudioNode>> connect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output = 0, WebIDL::UnsignedLong input = 0);
    WebIDL::ExceptionOr<void> connect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output = 0);

    void disconnect();
    WebIDL::ExceptionOr<void> disconnect(WebIDL::UnsignedLong output);
    WebIDL::ExceptionOr<void> disconnect(GC::Ref<AudioNode> destination_node);
    WebIDL::ExceptionOr<void> disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output);
    WebIDL::ExceptionOr<void> disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input);
    WebIDL::ExceptionOr<void> disconnect(GC::Ref<AudioParam> destination_param);
    WebIDL::ExceptionOr<void> disconnect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output);

    // https://webaudio.github.io/web-audio-api/#dom-audionode-context
    GC::Ref<BaseAudioContext> context()
    {
        // The BaseAudioContext which owns this AudioNode.
        return m_context;
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-numberofinputs
    virtual WebIDL::UnsignedLong number_of_inputs() = 0;
    // https://webaudio.github.io/web-audio-api/#dom-audionode-numberofoutputs
    virtual WebIDL::UnsignedLong number_of_outputs() = 0;

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
    virtual WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong);
    virtual WebIDL::UnsignedLong channel_count() const { return m_channel_count; }

    virtual WebIDL::ExceptionOr<void> set_channel_count_mode(Bindings::ChannelCountMode);
    Bindings::ChannelCountMode channel_count_mode();
    virtual WebIDL::ExceptionOr<void> set_channel_interpretation(Bindings::ChannelInterpretation);
    Bindings::ChannelInterpretation channel_interpretation();

    WebIDL::ExceptionOr<void> initialize_audio_node_options(AudioNodeOptions const& given_options, AudioNodeDefaultOptions const& default_options);

    NodeID node_id() const { return m_node_id; }

protected:
    AudioNode(JS::Realm&, GC::Ref<BaseAudioContext>, WebIDL::UnsignedLong channel_count = 2);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<BaseAudioContext> m_context;
    WebIDL::UnsignedLong m_channel_count { 2 };
    Bindings::ChannelCountMode m_channel_count_mode { Bindings::ChannelCountMode::Max };
    Bindings::ChannelInterpretation m_channel_interpretation { Bindings::ChannelInterpretation::Speakers };
    // Connections from other AudioNode outputs into this node's inputs.
    Vector<AudioNodeConnection> m_input_connections;
    // Connections from this node's outputs into other AudioNode inputs.
    Vector<AudioNodeConnection> m_output_connections;
    // Connections from this node's outputs into AudioParams.
    Vector<AudioParamConnection> m_param_connections;
    NodeID const m_node_id;
};

}
