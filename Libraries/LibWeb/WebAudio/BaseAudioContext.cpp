/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/StringView.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AssociatedTaskQueue.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/BackgroundAudioDecoder.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/ConvolverNode.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/DynamicsCompressorNode.h>
#include <LibWeb/WebAudio/GainNode.h>
#include <LibWeb/WebAudio/IIRFilterNode.h>
#include <LibWeb/WebAudio/OscillatorNode.h>
#include <LibWeb/WebAudio/PannerNode.h>
#include <LibWeb/WebAudio/WaveShaperNode.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

BaseAudioContext::ControlThreadMarker::ControlThreadMarker()
{
    mark_current_thread_as_control_thread();
    ASSERT_CONTROL_THREAD();
}

NodeID BaseAudioContext::next_node_id()
{
    ASSERT_CONTROL_THREAD();
    return m_next_audio_node_id++;
}

BaseAudioContext::BaseAudioContext(JS::Realm& realm, float sample_rate)
    : DOM::EventTarget(realm)
    , m_sample_rate(sample_rate)
    , m_listener(AudioListener::create(realm, *this))
    , m_control_message_queue(make<ControlMessageQueue>())
    , m_associated_task_queue(make<AssociatedTaskQueue>())
{
    mark_current_thread_as_control_thread();
    ASSERT_CONTROL_THREAD();
}

BaseAudioContext::~BaseAudioContext() = default;

void BaseAudioContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BaseAudioContext);
    Base::initialize(realm);
}

void BaseAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_destination);
    visitor.visit(m_pending_promises);
    visitor.visit(m_listener);
    visitor.visit(m_audio_worklet);
}

GC::Ref<AudioWorklet> BaseAudioContext::audio_worklet() const
{
    if (!m_audio_worklet)
        m_audio_worklet = AudioWorklet::create(realm(), const_cast<BaseAudioContext&>(*this));
    return *m_audio_worklet;
}

bool BaseAudioContext::take_pending_promise(GC::Ref<WebIDL::Promise> const& promise)
{
    return m_pending_promises.remove_first_matching([&](auto& pending_promise) {
        return pending_promise == promise;
    });
}

void BaseAudioContext::set_control_state_and_dispatch_statechange(Bindings::AudioContextState state)
{
    if (m_control_thread_state == state)
        return;
    set_control_state(state);
    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::statechange));
}

void BaseAudioContext::set_onstatechange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::statechange, event_handler);
}

void BaseAudioContext::queue_associated_task(AssociatedTaskQueue::Task task)
{
    ASSERT_CONTROL_THREAD();
    m_associated_task_queue->enqueue(move(task));
}

WebIDL::CallbackType* BaseAudioContext::onstatechange()
{
    return event_handler_attribute(HTML::EventNames::statechange);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createanalyser
WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> BaseAudioContext::create_analyser()
{
    return AnalyserNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbiquadfilter
WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> BaseAudioContext::create_biquad_filter()
{
    // Factory method for a BiquadFilterNode representing a second order filter which can be configured as one of several common filter types.
    return BiquadFilterNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> BaseAudioContext::create_buffer(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // Creates an AudioBuffer of the given size. The audio data in the buffer will be zero-initialized (silent).
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    return AudioBuffer::create(realm(), number_of_channels, length, sample_rate);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffersource
WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> BaseAudioContext::create_buffer_source()
{
    // Factory method for a AudioBufferSourceNode.
    return AudioBufferSourceNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelmerger
WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> BaseAudioContext::create_channel_merger(WebIDL::UnsignedLong number_of_inputs)
{
    ChannelMergerOptions options;
    options.number_of_inputs = number_of_inputs;

    return ChannelMergerNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createconstantsource
WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> BaseAudioContext::create_constant_source()
{
    return ConstantSourceNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createconvolver
WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> BaseAudioContext::create_convolver()
{
    return ConvolverNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdelay
WebIDL::ExceptionOr<GC::Ref<DelayNode>> BaseAudioContext::create_delay(double max_delay_time)
{
    DelayOptions options;
    options.max_delay_time = max_delay_time;

    return DelayNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelsplitter
WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> BaseAudioContext::create_channel_splitter(WebIDL::UnsignedLong number_of_outputs)
{
    ChannelSplitterOptions options;
    options.number_of_outputs = number_of_outputs;

    return ChannelSplitterNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createoscillator
WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> BaseAudioContext::create_oscillator()
{
    // Factory method for an OscillatorNode.
    return OscillatorNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdynamicscompressor
WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> BaseAudioContext::create_dynamics_compressor()
{
    // Factory method for a DynamicsCompressorNode.
    return DynamicsCompressorNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-creategain
WebIDL::ExceptionOr<GC::Ref<GainNode>> BaseAudioContext::create_gain()
{
    // Factory method for GainNode.
    return GainNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createiirfilter
WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> BaseAudioContext::create_iir_filter(Vector<double> const& feedforward, Vector<double> const& feedback)
{
    IIRFilterOptions options;
    options.feedforward = feedforward;
    options.feedback = feedback;
    return IIRFilterNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createpanner
WebIDL::ExceptionOr<GC::Ref<PannerNode>> BaseAudioContext::create_panner()
{
    // Factory method for a PannerNode.
    return PannerNode::create(realm(), *this);
}

WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> BaseAudioContext::create_periodic_wave(Vector<float> const& real, Vector<float> const& imag, Optional<PeriodicWaveConstraints> const& constraints)
{
    PeriodicWaveOptions options;
    options.real = real;
    options.imag = imag;
    if (constraints.has_value())
        options.disable_normalization = constraints->disable_normalization;

    return PeriodicWave::construct_impl(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createscriptprocessor
WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> BaseAudioContext::create_script_processor(
    WebIDL::UnsignedLong buffer_size, WebIDL::UnsignedLong number_of_input_channels,
    WebIDL::UnsignedLong number_of_output_channels)
{
    // The bufferSize parameter determines the buffer size in units of sample-frames. If it’s not passed in, or if the
    // value is 0, then the implementation will choose the best buffer size for the given environment, which will be
    // constant power of 2 throughout the lifetime of the node.
    if (buffer_size == 0)
        buffer_size = ScriptProcessorNode::DEFAULT_BUFFER_SIZE;

    return ScriptProcessorNode::create(realm(), *this, static_cast<WebIDL::Long>(buffer_size), number_of_input_channels,
        number_of_output_channels);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createstereopanner
WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> BaseAudioContext::create_stereo_panner()
{
    // Factory method for a StereoPannerNode.
    return StereoPannerNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createwaveshaper
WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> BaseAudioContext::create_wave_shaper()
{
    return WaveShaperNode::create(realm(), *this);
}

WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(JS::Realm& realm, float sample_rate)
{
    if (sample_rate < MIN_SAMPLE_RATE || sample_rate > MAX_SAMPLE_RATE)
        return WebIDL::NotSupportedError::create(realm, "Sample rate is outside of allowed range"_utf16);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(JS::Realm& realm, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.

    if (number_of_channels == 0)
        return WebIDL::NotSupportedError::create(realm, "Number of channels must not be '0'"_utf16);

    if (number_of_channels > MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create(realm, "Number of channels is greater than allowed range"_utf16);

    if (length == 0)
        return WebIDL::NotSupportedError::create(realm, "Length of buffer must be at least 1"_utf16);

    TRY(verify_audio_options_inside_nominal_range(realm, sample_rate));

    return {};
}

void BaseAudioContext::queue_a_media_element_task(StringView label, GC::Ref<GC::Function<void()>> steps)
{
    WA_DBGLN("[WebAudio] {}", label);
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    auto task = HTML::Task::create(vm(), m_media_element_event_task_source.source, associated_document, steps);
    HTML::main_thread_event_loop().task_queue().add(task);
}

void BaseAudioContext::queue_control_message(ControlMessage message)
{
    ASSERT_CONTROL_THREAD();
    m_control_message_queue->enqueue(move(message));
}

void BaseAudioContext::schedule_source_end(AudioScheduledSourceNode& node, double end_time_seconds)
{
    ASSERT_CONTROL_THREAD();

    if (m_sample_rate <= 0)
        return;

    if (!__builtin_isfinite(end_time_seconds))
        return;

    if (end_time_seconds < 0.0)
        end_time_seconds = 0.0;

    if (m_dispatched_source_ends.contains(node.node_id()))
        return;

    WA_DBGLN("[WebAudio] schedule_source_end node_id={} end_time_s={} current_frame={}", node.node_id(), end_time_seconds, current_frame());

    double const end_frame_f64 = AK::ceil(end_time_seconds * static_cast<double>(m_sample_rate));
    u64 end_frame = 0;
    if (end_frame_f64 >= static_cast<double>(AK::NumericLimits<u64>::max()))
        end_frame = AK::NumericLimits<u64>::max();
    else
        end_frame = static_cast<u64>(end_frame_f64);

    bool updated = false;
    m_scheduled_source_end_nodes.set(node.node_id(), GC::Weak<AudioScheduledSourceNode> { node });
    auto it = m_scheduled_source_end_frames.find(node.node_id());
    if (it == m_scheduled_source_end_frames.end()) {
        m_scheduled_source_end_frames.set(node.node_id(), end_frame);
        updated = true;
    } else if (end_frame < it->value) {
        it->value = end_frame;
        updated = true;
    }

    if (updated)
        on_scheduled_source_end_added();

    dispatch_scheduled_source_ends(current_frame());
}

void BaseAudioContext::dispatch_scheduled_source_ends(u64 current_frame)
{
    ASSERT_CONTROL_THREAD();

    if (m_scheduled_source_end_frames.is_empty())
        return;

    Vector<NodeID> due_nodes;
    due_nodes.ensure_capacity(m_scheduled_source_end_frames.size());

    for (auto const& it : m_scheduled_source_end_frames) {
        if (it.value <= current_frame)
            due_nodes.append(it.key);
    }

    for (auto const& node_id : due_nodes) {
        m_scheduled_source_end_frames.remove(node_id);
        m_dispatched_source_ends.set(node_id);
        auto node_it = m_scheduled_source_end_nodes.find(node_id);
        if (node_it == m_scheduled_source_end_nodes.end()) {
            WA_DBGLN("[WebAudio] dispatch_scheduled_source_ends missing node_id={}", node_id);
            continue;
        }

        auto& weak_node = node_it->value;
        if (!weak_node) {
            m_scheduled_source_end_nodes.remove(node_it);
            WA_DBGLN("[WebAudio] dispatch_scheduled_source_ends expired node_id={}", node_id);
            continue;
        }

        auto& target_node = static_cast<AudioScheduledSourceNode&>(*weak_node);
        m_scheduled_source_end_nodes.remove(node_it);

        dispatch_scheduled_source_end_event(target_node);
    }
}

void BaseAudioContext::dispatch_scheduled_source_end_event(AudioScheduledSourceNode& node)
{
    auto node_ref = GC::Ref<AudioScheduledSourceNode> { node };
    queue_a_media_element_task("audio scheduled source ended"sv, GC::create_function(heap(), [node_ref] {
        auto& realm = node_ref->realm();
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        node_ref->dispatch_event(DOM::Event::create(realm, HTML::EventNames::ended));
    }));
}

void BaseAudioContext::notify_audio_graph_changed()
{
    ASSERT_CONTROL_THREAD();

    // Coalesce multiple immediate graph mutations into a single snapshot/update.
    // Many callers (e.g. AudioNode::disconnect + AudioNode::connect) will mutate the graph in
    // quick succession within one JS task; snapshotting each intermediate state is wasteful and
    // can enqueue transient disconnected graphs.
    m_audio_graph_dirty = true;
    if (m_audio_graph_update_task_scheduled)
        return;
    m_audio_graph_update_task_scheduled = true;

    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    auto self = GC::Ref<BaseAudioContext> { *this };

    // GC::Root?????

    auto steps = GC::create_function(heap(), [self] {
        ASSERT_CONTROL_THREAD();

        self->m_audio_graph_update_task_scheduled = false;
        if (!self->m_audio_graph_dirty)
            return;
        self->m_audio_graph_dirty = false;

        self->on_audio_graph_changed();

        // If additional mutations happened while processing, schedule again.
        if (self->m_audio_graph_dirty)
            self->notify_audio_graph_changed();
    });

    HTML::queue_a_microtask(&associated_document, steps);
}

void BaseAudioContext::register_audio_node_for_snapshot(AudioNode& node)
{
    // Keep a weak list of nodes so snapshot_render_graph() can include all nodes in the context.
    m_audio_nodes_for_snapshot.remove_all_matching([](GC::Weak<AudioNode>& existing) {
        return !existing;
    });

    m_audio_nodes_for_snapshot.remove_first_matching([&](GC::Weak<AudioNode> const& existing) {
        return existing.ptr() == &node;
    });

    m_audio_nodes_for_snapshot.append(GC::Weak<AudioNode> { node });
}

void BaseAudioContext::flush_pending_audio_graph_update()
{
    ASSERT_CONTROL_THREAD();

    if (!m_audio_graph_update_task_scheduled)
        return;

    m_audio_graph_update_task_scheduled = false;
    if (!m_audio_graph_dirty)
        return;
    m_audio_graph_dirty = false;

    on_audio_graph_changed();

    if (m_audio_graph_dirty)
        notify_audio_graph_changed();
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
GC::Ref<WebIDL::Promise> BaseAudioContext::decode_audio_data(GC::Root<WebIDL::BufferSource> const& audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    auto& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    return associated_document.background_audio_decoder().decode_audio_data(*this, audio_data, success_callback, error_callback);
}

}
