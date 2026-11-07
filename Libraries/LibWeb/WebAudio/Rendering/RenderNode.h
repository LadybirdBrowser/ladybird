/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/AudioParamTimeline.h>
#include <LibWeb/WebAudio/Rendering/AudioBus.h>
#include <LibWeb/WebAudio/Rendering/NodeMessage.h>
#include <LibWeb/WebAudio/Types.h>
namespace Web::WebAudio::Rendering {

class RenderGraph;
struct RenderContext {
    double sample_rate { 0 };
    size_t quantum_size { 0 };
    u64 quantum_start_frame { 0 };
    double quantum_start_time() const { return quantum_start_frame / sample_rate; }
    double sample_period() const { return 1 / sample_rate; }
    double frame_time(size_t frame) const { return (quantum_start_frame + frame) / sample_rate; }
};
class WEB_API RenderAudioParam : public AtomicRefCounted<RenderAudioParam> {
public:
    RenderAudioParam(NonnullRefPtr<AudioParamTimeline> timeline, float min_value, float max_value, Bindings::AutomationRate);

    ~RenderAudioParam();

    void set_automation_rate(Bindings::AutomationRate);
    Bindings::AutomationRate automation_rate() const;

    void compute(RenderGraph&, RenderContext const&, Span<float> output);

    void const* rust_param() const { return m_param; }

    void clear_inputs();
    void add_input(NodeID source, size_t output_index);

private:
    void const* m_param { nullptr };
};

class WEB_API RenderNode {
    AK_MAKE_NONCOPYABLE(RenderNode);
    AK_MAKE_NONMOVABLE(RenderNode);

public:
    AK_ALLOC_WITH_KMALLOC;
    RenderNode(NodeID, size_t input_count, size_t output_count, size_t quantum_size, size_t output_channel_count = 1);
    virtual ~RenderNode();
    NodeID node_id() const;
    size_t input_count() const;
    size_t output_count() const;
    AudioBus output(size_t index) const;
    void set_channel_count(size_t);
    void set_channel_count_mode(Bindings::ChannelCountMode);
    void set_channel_interpretation(Bindings::ChannelInterpretation);
    size_t channel_count() const;
    Bindings::ChannelCountMode channel_count_mode() const;
    Bindings::ChannelInterpretation channel_interpretation() const;
    bool has_input_connections(size_t) const;
    size_t outgoing_node_connection_count() const;
    size_t outgoing_param_connection_count() const;
    struct OutgoingNodeConnection {
        NodeID destination;
        size_t output_index;
        size_t input_index;
    };
    struct OutgoingParamConnection {
        NonnullRefPtr<RenderAudioParam> destination;
        size_t output_index;
    };
    virtual void process(RenderGraph&, RenderContext const&) { VERIFY_NOT_REACHED(); }
    virtual void handle_message(NodeMessage const&) { }
    virtual void for_each_param(Function<void(RenderAudioParam&)> const&) { }
    virtual bool take_ended_notification() { return false; }
    void* release_rust_node();
    bool is_external() const { return m_external; }

protected:
    explicit RenderNode(void*);
    AudioBus pull_input(RenderGraph&, RenderContext const&, size_t input_index);

private:
    void* m_node { nullptr };
    bool m_owned { true };
    bool m_external { false };
    bool m_transferred { false };
};

}
