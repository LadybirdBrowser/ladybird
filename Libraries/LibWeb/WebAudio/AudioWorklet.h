/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/AudioWorkletGlobalScope.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Worklet.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#audioworklet
class WEB_API AudioWorklet final : public HTML::Worklet {
    WEB_WRAPPABLE(AudioWorklet, HTML::Worklet);
    GC_DECLARE_ALLOCATOR(AudioWorklet);

public:
    static GC::Ref<AudioWorklet> create(GC::Ref<BaseAudioContext>);

    virtual ~AudioWorklet() override;

    GC::Ptr<AudioWorkletGlobalScope> audio_worklet_global_scope() const;
    HTML::MessagePort* port() { return m_port.ptr(); }

    // https://webaudio.github.io/web-audio-api/#node-name-to-parameter-descriptor-map
    // Control-thread mirror of the worklet-side registrations. The spec ships this via a control
    // message; the worklet global scope runs on the control thread, so it is a direct call from
    // AudioWorkletGlobalScope::register_processor.
    struct SyncedDefinition {
        Utf16String name;
        Vector<Bindings::AudioParamDescriptor> parameter_descriptors;
    };
    Optional<SyncedDefinition> find_definition(Utf16String const& name) const;
    void add_synced_definition(SyncedDefinition);

private:
    explicit AudioWorklet(GC::Ref<BaseAudioContext>);

    virtual GC::Ref<HTML::WorkletGlobalScope> create_global_scope() override;
    virtual Fetch::Infrastructure::Request::Destination worklet_destination() const override
    {
        return Fetch::Infrastructure::Request::Destination::AudioWorklet;
    }

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<BaseAudioContext> m_context;
    GC::Ref<HTML::MessagePort> m_port;
    Vector<SyncedDefinition> m_synced_definitions;
};

}
