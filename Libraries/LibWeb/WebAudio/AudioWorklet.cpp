/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/WorkletEnvironmentSettingsObject.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioWorklet);

GC::Ref<AudioWorklet> AudioWorklet::create(GC::Ref<BaseAudioContext> context)
{
    return GC::Heap::the().allocate<AudioWorklet>(context);
}

AudioWorklet::AudioWorklet(GC::Ref<BaseAudioContext> context)
    : HTML::Worklet(context->relevant_global_event_target())
    , m_context(context)
    , m_port(HTML::MessagePort::create(context->relevant_global_event_target()))
{
}

AudioWorklet::~AudioWorklet() = default;

GC::Ptr<AudioWorkletGlobalScope> AudioWorklet::audio_worklet_global_scope() const
{
    if (!global_scope())
        return nullptr;
    return &as<AudioWorkletGlobalScope>(*global_scope());
}

// https://webaudio.github.io/web-audio-api/#AudioWorkletGlobalScope
GC::Ref<HTML::WorkletGlobalScope> AudioWorklet::create_global_scope()
{
    auto& outside_settings = HTML::relevant_settings_object(HTML::relevant_window_or_worker_global_scope(*relevant_global()));
    VERIFY(outside_settings.responsible_document());
    auto page = GC::Ref { outside_settings.responsible_document()->page() };

    // 1. Let realmExecutionContext be the result of creating a new realm given agent and the following
    //    customizations: For the global object, create a new AudioWorkletGlobalScope object.
    // AD-HOC: The spec gives each worklet its own agent (and event loop). Ladybird currently has one JS
    //         agent per process, so the worklet realm lives in the window's agent and shares its event
    //         loop. Realm-level isolation (global object, settings object, module map, intrinsics) is
    //         preserved. Revisit when LibGC supports per-agent heaps on separate threads.
    GC::Ptr<AudioWorkletGlobalScope> scope;
    auto realm_execution_context = Bindings::create_a_new_javascript_realm(
        Bindings::main_thread_vm(),
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            scope = AudioWorkletGlobalScope::create(*this, m_context->sample_rate());
            return Bindings::create_global_object_wrapper(realm, GC::Ref { *scope });
        },
        nullptr);
    VERIFY(scope);

    // 2. Set up a worklet environment settings object given realmExecutionContext and outsideSettings.
    (void)HTML::WorkletEnvironmentSettingsObject::setup(page, move(realm_execution_context), *scope, outside_settings);

    // Bindings initialization requires the realm's [[HostDefined]] slot.
    scope->initialize_web_interfaces_impl();

    auto scope_port = HTML::MessagePort::create(*scope);
    m_port->entangle_with(*scope_port);
    m_port->enable();
    scope->set_port(scope_port);

    return *scope;
}

Optional<AudioWorklet::SyncedDefinition> AudioWorklet::find_definition(Utf16String const& name) const
{
    for (auto const& definition : m_synced_definitions) {
        if (definition.name == name)
            return definition;
    }
    return {};
}

void AudioWorklet::add_synced_definition(SyncedDefinition definition)
{
    m_synced_definitions.append(move(definition));
}

void AudioWorklet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
    visitor.visit(m_port);
}

}
