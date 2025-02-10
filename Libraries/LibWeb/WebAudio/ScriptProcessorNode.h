/*
 * Copyright (c) 2025, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
class ScriptProcessorNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(ScriptProcessorNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ScriptProcessorNode);

public:
    virtual ~ScriptProcessorNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>,
        WebIDL::UnsignedLong buffer_size, WebIDL::UnsignedLong number_of_input_channels, WebIDL::UnsignedLong number_of_output_channels);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ptr<WebIDL::CallbackType> onaudioprocess();
    void set_onaudioprocess(GC::Ptr<WebIDL::CallbackType>);

    WebIDL::UnsignedLong buffer_size() const { return m_buffer_size; }

private:
    ScriptProcessorNode(JS::Realm&, GC::Ref<BaseAudioContext>,
        WebIDL::UnsignedLong buffer_size, WebIDL::UnsignedLong number_of_input_channels, WebIDL::UnsignedLong number_of_output_channels);

    virtual void initialize(JS::Realm&) override;

    WebIDL::UnsignedLong m_buffer_size;
    WebIDL::UnsignedLong m_number_of_input_channels;
    WebIDL::UnsignedLong m_number_of_output_channels;
};

}
