/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioNodePrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
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

// https://webaudio.github.io/web-audio-api/#AudioNode
class AudioNode : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(AudioNode, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(AudioNode);

public:
    virtual ~AudioNode() override;

    WebIDL::ExceptionOr<GC::Ref<AudioNode>> connect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output = 0, WebIDL::UnsignedLong input = 0);
    void connect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output = 0);

    void disconnect();
    void disconnect(WebIDL::UnsignedLong output);
    void disconnect(GC::Ref<AudioNode> destination_node);
    void disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output);
    void disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input);
    void disconnect(GC::Ref<AudioParam> destination_param);
    void disconnect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output);

    // https://webaudio.github.io/web-audio-api/#dom-audionode-context
    GC::Ref<BaseAudioContext const> context() const
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
    WebIDL::ExceptionOr<void> set_channel_interpretation(Bindings::ChannelInterpretation);
    Bindings::ChannelInterpretation channel_interpretation();

    WebIDL::ExceptionOr<void> initialize_audio_node_options(AudioNodeOptions const& given_options, AudioNodeDefaultOptions const& default_options);

protected:
    AudioNode(JS::Realm&, GC::Ref<BaseAudioContext>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<BaseAudioContext> m_context;
    WebIDL::UnsignedLong m_channel_count { 2 };
    Bindings::ChannelCountMode m_channel_count_mode { Bindings::ChannelCountMode::Max };
    Bindings::ChannelInterpretation m_channel_interpretation { Bindings::ChannelInterpretation::Speakers };
};

}
