/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#ChannelMergerOptions
struct ChannelMergerOptions : AudioNodeOptions {
    WebIDL::UnsignedLong number_of_inputs { 6 };
};

// https://webaudio.github.io/web-audio-api/#ChannelMergerNode
class ChannelMergerNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(ChannelMergerNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ChannelMergerNode);

public:
    virtual ~ChannelMergerNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, ChannelMergerOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, ChannelMergerOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return m_number_of_inputs; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    // ^AudioNode
    virtual WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;
    virtual WebIDL::ExceptionOr<void> set_channel_count_mode(Bindings::ChannelCountMode) override;

private:
    ChannelMergerNode(JS::Realm&, GC::Ref<BaseAudioContext>, ChannelMergerOptions const&);

    WebIDL::UnsignedLong m_number_of_inputs;
};

}
