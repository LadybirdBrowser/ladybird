/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ChannelSplitterNode.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

using ChannelSplitterOptions = Bindings::ChannelSplitterOptions;

/// https://webaudio.github.io/web-audio-api/#ChannelSplitterNode
class ChannelSplitterNode final : public AudioNode {
    WEB_WRAPPABLE(ChannelSplitterNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ChannelSplitterNode);

public:
    virtual ~ChannelSplitterNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> create(GC::Ref<BaseAudioContext>, ChannelSplitterOptions const& = {});
    static WebIDL::ExceptionOr<void> validate_options(ChannelSplitterOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> create_for_constructor(GC::Ref<BaseAudioContext>, ChannelSplitterOptions const& = {});

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return m_number_of_outputs; }

    virtual WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;
    virtual WebIDL::ExceptionOr<void> set_channel_count_mode(ChannelCountMode) override;
    virtual WebIDL::ExceptionOr<void> set_channel_interpretation(ChannelInterpretation) override;

private:
    ChannelSplitterNode(GC::Ref<BaseAudioContext>, ChannelSplitterOptions const&);

    WebIDL::UnsignedLong m_number_of_outputs;
};

}
