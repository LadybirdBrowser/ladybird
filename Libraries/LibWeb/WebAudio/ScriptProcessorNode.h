/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
class ScriptProcessorNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(ScriptProcessorNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ScriptProcessorNode);

public:
    static constexpr WebIDL::Long DEFAULT_BUFFER_SIZE = 1024;

    virtual ~ScriptProcessorNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>,
        WebIDL::Long buffer_size, WebIDL::UnsignedLong number_of_input_channels,
        WebIDL::UnsignedLong number_of_output_channel);

    // ^AudioNode
    virtual WebIDL::UnsignedLong channel_count() const override;
    virtual WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;
    virtual WebIDL::ExceptionOr<void> set_channel_count_mode(Bindings::ChannelCountMode) override;
    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ptr<WebIDL::CallbackType> onaudioprocess();
    void set_onaudioprocess(GC::Ptr<WebIDL::CallbackType>);

    WebIDL::Long buffer_size() const { return m_buffer_size; }
    WebIDL::ExceptionOr<void> set_buffer_size(WebIDL::Long buffer_size);

private:
    ScriptProcessorNode(JS::Realm&, GC::Ref<BaseAudioContext>, u8 number_of_input_channels,
        u8 number_of_output_channels);

    virtual void initialize(JS::Realm&) override;

    WebIDL::Long m_buffer_size { 0 };
    u8 m_number_of_input_channels { 0 };
    u8 m_number_of_output_channels { 0 };
};

}
