/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ChannelMergerNode.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

using ChannelMergerOptions = Bindings::ChannelMergerOptions;

// https://webaudio.github.io/web-audio-api/#ChannelMergerNode
class ChannelMergerNode final : public AudioNode {
    WEB_WRAPPABLE(ChannelMergerNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ChannelMergerNode);

public:
    virtual ~ChannelMergerNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> create(GC::Ref<BaseAudioContext>, ChannelMergerOptions const& = {});
    static WebIDL::ExceptionOr<void> validate_options(ChannelMergerOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> create_for_constructor(GC::Ref<BaseAudioContext>, ChannelMergerOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return m_number_of_inputs; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    // ^AudioNode
    virtual WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;
    virtual WebIDL::ExceptionOr<void> set_channel_count_mode(ChannelCountMode) override;

private:
    ChannelMergerNode(GC::Ref<BaseAudioContext>, ChannelMergerOptions const&);

    WebIDL::UnsignedLong m_number_of_inputs;
};

}
