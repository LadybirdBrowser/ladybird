/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/AudioParamTimeline.h>
#include <LibWeb/WebAudio/Rendering/AudioBus.h>
#include <LibWeb/WebAudio/Rendering/NodeMessage.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Rendering {

class RenderGraph;

// State for a single render quantum that is passed to every node being processed.
struct RenderContext {
    double sample_rate { 0 };
    size_t quantum_size { 0 };
    u64 quantum_start_frame { 0 };

    double quantum_start_time() const { return quantum_start_frame / sample_rate; }
    double sample_period() const { return 1 / sample_rate; }
    double frame_time(size_t frame) const { return (quantum_start_frame + frame) / sample_rate; }
};

// The rendering thread's view of an AudioParam. The automation timeline is shared with the AudioParam on the control
// thread; input connections are only manipulated by the rendering thread.
// https://webaudio.github.io/web-audio-api/#computedvalue
class WEB_API RenderAudioParam : public AtomicRefCounted<RenderAudioParam> {
public:
    RenderAudioParam(NonnullRefPtr<AudioParamTimeline> timeline, float min_value, float max_value, Bindings::AutomationRate);

    void set_automation_rate(Bindings::AutomationRate automation_rate) { m_automation_rate.store(automation_rate); }
    Bindings::AutomationRate automation_rate() const { return m_automation_rate.load(); }

    void compute(RenderGraph&, RenderContext const&, Span<float> output);

    struct Input {
        NodeID source;
        size_t output_index;
    };
    Vector<Input>& inputs() { return m_inputs; }

private:
    NonnullRefPtr<AudioParamTimeline> m_timeline;
    float m_min_value { 0 };
    float m_max_value { 0 };
    Atomic<Bindings::AutomationRate> m_automation_rate;

    Vector<Input> m_inputs;
    OwnPtr<AudioBus> m_input_scratch;
};

// Base class for the rendering thread's counterparts of AudioNodes. Render nodes are constructed on the control thread
// and transferred to the rendering thread through the control message queue; after that, all access happens on the
// rendering thread.
class WEB_API RenderNode {
    AK_MAKE_NONCOPYABLE(RenderNode);
    AK_MAKE_NONMOVABLE(RenderNode);

public:
    RenderNode(NodeID, size_t input_count, size_t output_count, size_t quantum_size, size_t output_channel_count = 1);
    virtual ~RenderNode() = default;

    NodeID node_id() const { return m_node_id; }

    size_t input_count() const { return m_input_connections.size(); }
    size_t output_count() const { return m_outputs.size(); }

    AudioBus& output(size_t index) { return m_outputs[index]; }
    AudioBus const& output(size_t index) const { return m_outputs[index]; }

    void set_channel_count(size_t channel_count) { m_channel_count = channel_count; }
    void set_channel_count_mode(Bindings::ChannelCountMode mode) { m_channel_count_mode = mode; }
    void set_channel_interpretation(Bindings::ChannelInterpretation interpretation) { m_channel_interpretation = interpretation; }
    size_t channel_count() const { return m_channel_count; }
    Bindings::ChannelCountMode channel_count_mode() const { return m_channel_count_mode; }
    Bindings::ChannelInterpretation channel_interpretation() const { return m_channel_interpretation; }

    struct Connection {
        NodeID source;
        size_t output_index;
    };
    Vector<Connection>& input_connections(size_t input_index) { return m_input_connections[input_index]; }

    struct OutgoingNodeConnection {
        NodeID destination;
        size_t output_index;
        size_t input_index;
    };
    struct OutgoingParamConnection {
        NonnullRefPtr<RenderAudioParam> destination;
        size_t output_index;
    };
    void set_outgoing_connections(Vector<OutgoingNodeConnection> node_connections, Vector<OutgoingParamConnection> param_connections);
    Vector<OutgoingNodeConnection> const& outgoing_node_connections() const { return m_outgoing_node_connections; }
    Vector<OutgoingParamConnection> const& outgoing_param_connections() const { return m_outgoing_param_connections; }

    virtual void process(RenderGraph&, RenderContext const&) = 0;
    virtual void handle_message(NodeMessage const&) { }
    virtual void for_each_param(Function<void(RenderAudioParam&)> const&) { }

    // Returns true exactly once after this node has stopped playing, so the main thread can fire an ended event.
    virtual bool take_ended_notification() { return false; }

    // Bookkeeping used by RenderGraph to process every node at most once per render quantum.
    bool processed_in_quantum(u64 quantum) const { return m_processed_quantum == quantum; }
    void mark_processed_in_quantum(u64 quantum) { m_processed_quantum = quantum; }
    bool is_processing() const { return m_is_processing; }
    void set_is_processing(bool is_processing) { m_is_processing = is_processing; }

protected:
    AudioBus const& pull_input(RenderGraph&, RenderContext const&, size_t input_index);

private:
    NodeID m_node_id;
    size_t m_channel_count { 2 };
    Bindings::ChannelCountMode m_channel_count_mode { Bindings::ChannelCountMode::Max };
    Bindings::ChannelInterpretation m_channel_interpretation { Bindings::ChannelInterpretation::Speakers };

    Vector<Vector<Connection>> m_input_connections;
    Vector<AudioBus> m_input_buses;
    Vector<AudioBus const*, 4> m_pulled_source_buses;
    Vector<AudioBus> m_outputs;

    Vector<OutgoingNodeConnection> m_outgoing_node_connections;
    Vector<OutgoingParamConnection> m_outgoing_param_connections;

    u64 m_processed_quantum { NumericLimits<u64>::max() };
    bool m_is_processing { false };
};

}
