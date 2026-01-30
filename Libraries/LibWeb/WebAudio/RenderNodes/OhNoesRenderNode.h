/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class OhNoesRenderNode final : public RenderNode {
public:
    OhNoesRenderNode(NodeID node_id, size_t quantum_size);
    OhNoesRenderNode(NodeID node_id, size_t quantum_size, OhNoesGraphNode const&);
    virtual ~OhNoesRenderNode() override;

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override;
    void apply_description(GraphNodeDescription const&) override;

private:
    static constexpr size_t max_channel_count { 32 };

    bool m_is_debug_node { false };
    String m_base_path;
    bool m_emit_enabled { true };
    bool m_strip_zero_buffers { false };
    bool m_has_file_error { false };

#ifndef NDEBUG
    struct WavWriter;
    OwnPtr<WavWriter> m_wav_writer;
    Vector<ReadonlySpan<f32>> m_planar_channels;
    Vector<f32> m_interleaved_samples;
#endif

    AudioBus m_output;
};

}
