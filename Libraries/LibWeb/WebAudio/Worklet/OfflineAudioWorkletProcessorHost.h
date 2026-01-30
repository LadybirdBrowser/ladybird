/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorHost.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorInvoker.h>

namespace Web::WebAudio::Render {

class OfflineAudioWorkletProcessorHost final : public AudioWorkletProcessorHost {
public:
    struct WorkletError {
        NodeID node_id;
        JS::Value error;
    };

    OfflineAudioWorkletProcessorHost(GC::Ref<JS::Realm> worklet_realm, HashMap<NodeID, GC::Root<JS::Object>>& processor_instances);

    bool process_audio_worklet(NodeID node_id, RenderContext& process_context, String const&, size_t, size_t, Span<size_t const>, Vector<Vector<AudioBus const*>> const& inputs, Span<AudioBus*> outputs, Span<AudioWorkletProcessorHost::ParameterSpan const> parameters) override;

    bool has_errors() const;
    Vector<WorkletError> take_errors();

private:
    GC::Ref<JS::Realm> m_worklet_realm;
    HashMap<NodeID, GC::Root<JS::Object>>& m_processor_instances;
    Vector<WorkletError> m_errors;
};

}
