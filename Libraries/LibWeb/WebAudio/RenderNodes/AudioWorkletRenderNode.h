/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorHost.h>

namespace Web::WebAudio::Render {

class AudioWorkletRenderNode final : public RenderNode {
public:
    AudioWorkletRenderNode(NodeID, AudioWorkletGraphNode const&, size_t quantum_size);

    virtual void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;

    virtual size_t output_count() const override { return m_number_of_outputs; }
    virtual AudioBus const& output(size_t output_index) const override;

private:
    static constexpr size_t max_channel_count { 32 };

    bool m_keep_processing { true };

    size_t m_number_of_inputs { 1 };
    size_t m_number_of_outputs { 1 };

    String m_processor_name;

    Optional<Vector<size_t>> m_output_channel_count;
    size_t m_channel_count { 2 };
    ChannelCountMode m_channel_count_mode { ChannelCountMode::Max };
    Vector<String> m_parameter_names;
    Vector<AudioWorkletProcessorHost::ParameterSpan> m_parameters_for_invocation;
    Vector<Vector<AudioBus const*>> m_inputs_for_invocation;
    OwnPtr<AudioBus> m_silent_output;

    Vector<OwnPtr<AudioBus>> m_outputs;
};

}
