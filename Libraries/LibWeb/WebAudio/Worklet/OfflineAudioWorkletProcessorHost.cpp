/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Worklet/OfflineAudioWorkletProcessorHost.h>

namespace Web::WebAudio::Render {

OfflineAudioWorkletProcessorHost::OfflineAudioWorkletProcessorHost(GC::Ref<JS::Realm> worklet_realm, HashMap<NodeID, GC::Root<JS::Object>>& processor_instances)
    : m_worklet_realm(worklet_realm)
    , m_processor_instances(processor_instances)
{
}

bool OfflineAudioWorkletProcessorHost::process_audio_worklet(NodeID node_id, RenderContext& process_context, String const&, size_t, size_t, Span<size_t const>, Vector<Vector<AudioBus const*>> const& inputs, Span<AudioBus*> outputs, Span<AudioWorkletProcessorHost::ParameterSpan const> parameters)
{
    auto it = m_processor_instances.find(node_id);
    if (it == m_processor_instances.end())
        return true;
    auto* processor = it->value.ptr();
    if (!processor)
        return true;

    auto& global_scope = as<AudioWorkletGlobalScope>(m_worklet_realm->global_object());
    global_scope.set_current_frame(process_context.current_frame);
    global_scope.set_sample_rate(process_context.sample_rate);

    HTML::TemporaryExecutionContext execution_context(*m_worklet_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
    auto result = invoke_audio_worklet_processor_process(*m_worklet_realm, *processor, inputs, outputs, parameters, process_context.quantum_size);
    if (result.is_error()) {
        m_errors.append(WorkletError { .node_id = node_id, .error = result.release_error().value() });
        return false;
    }

    return result.release_value();
}

bool OfflineAudioWorkletProcessorHost::has_errors() const
{
    return !m_errors.is_empty();
}

Vector<OfflineAudioWorkletProcessorHost::WorkletError> OfflineAudioWorkletProcessorHost::take_errors()
{
    return move(m_errors);
}

}
