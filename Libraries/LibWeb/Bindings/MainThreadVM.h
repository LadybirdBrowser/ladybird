/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/WorkerTypes.h>

namespace Web::Bindings {

struct WebEngineCustomJobCallbackData final : public JS::JobCallback::CustomData {
    WebEngineCustomJobCallbackData(HTML::EnvironmentSettingsObject& incumbent_settings, OwnPtr<JS::ExecutionContext> active_script_context)
        : incumbent_settings(incumbent_settings)
        , active_script_context(move(active_script_context))
    {
    }

    virtual ~WebEngineCustomJobCallbackData() override = default;

    GC::Ref<HTML::EnvironmentSettingsObject> incumbent_settings;
    OwnPtr<JS::ExecutionContext> active_script_context;
};

HTML::Script* active_script();

WEB_API void initialize_main_thread_vm(HTML::AgentType);
WEB_API JS::VM& main_thread_vm();

WEB_API NonnullOwnPtr<JS::ExecutionContext> create_a_new_javascript_realm(JS::VM&, Function<GC::Ref<JS::Object>(JS::Realm&)> create_global_object, Function<GC::Ref<JS::Object>(JS::Realm&)> create_global_this_value);

// Creates a bare Window-backed realm for tests and tools that do not need a full Document.
WEB_API GC::Ref<JS::Realm> create_a_simple_javascript_realm();

// Creates a principal realm backed by a Page and window environment settings object.
WEB_API GC::Ref<JS::Realm> create_a_principal_javascript_realm();

}
