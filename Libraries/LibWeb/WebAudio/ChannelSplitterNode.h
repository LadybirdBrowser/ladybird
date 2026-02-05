/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#ChannelSplitterOptions
struct ChannelSplitterOptions : AudioNodeOptions {
    WebIDL::UnsignedLong number_of_outputs { 6 };
};

/// https://webaudio.github.io/web-audio-api/#ChannelSplitterNode
class ChannelSplitterNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(ChannelSplitterNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ChannelSplitterNode);

public:
    virtual ~ChannelSplitterNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, ChannelSplitterOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, ChannelSplitterOptions const& = {});

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return m_number_of_outputs; }

    virtual WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;
    virtual WebIDL::ExceptionOr<void> set_channel_count_mode(Bindings::ChannelCountMode) override;
    virtual WebIDL::ExceptionOr<void> set_channel_interpretation(Bindings::ChannelInterpretation) override;

private:
    ChannelSplitterNode(JS::Realm&, GC::Ref<BaseAudioContext>, ChannelSplitterOptions const&);

    virtual void initialize(JS::Realm&) override;

    WebIDL::UnsignedLong m_number_of_outputs;
};

}
