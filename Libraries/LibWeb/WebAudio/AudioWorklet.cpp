/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/HashTable.h>
#include <AK/Utf16String.h>
#include <LibCore/System.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletEnvironmentSettingsObject.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>
#include <LibWeb/WebAudio/Worklet/MessagePortTransport.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

static bool is_dom_exception_name(StringView name)
{
#define __ENUMERATE(ErrorName) \
    if (name == #ErrorName)    \
        return true;
    ENUMERATE_DOM_EXCEPTION_ERROR_NAMES
#undef __ENUMERATE
    return false;
}

GC_DEFINE_ALLOCATOR(AudioWorklet);

AudioWorklet::AudioWorklet(JS::Realm& realm, GC::Ref<BaseAudioContext> context)
    : Bindings::PlatformObject(realm)
    , m_context(context)
{
}

AudioWorklet::~AudioWorklet()
{
    for (auto const& it : m_realtime_processor_port_fds) {
        if (it.value >= 0)
            (void)Core::System::close(it.value);
    }
    m_realtime_processor_port_fds.clear();

    if (m_realtime_global_port_fd >= 0)
        (void)Core::System::close(m_realtime_global_port_fd);
    m_realtime_global_port_fd = -1;
}

GC::Ref<AudioWorklet> AudioWorklet::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context)
{
    return realm.create<AudioWorklet>(realm, context);
}

bool AudioWorklet::is_processor_registered(String const& name)
{
    auto& worklet_settings_object = ensure_worklet_environment_settings_object();
    auto& global_scope = as<AudioWorkletGlobalScope>(worklet_settings_object.global_object());
    return global_scope.is_processor_registered(name);
}

bool AudioWorklet::is_processor_registration_failed(String const& name)
{
    auto& worklet_settings_object = ensure_worklet_environment_settings_object();
    auto& global_scope = as<AudioWorkletGlobalScope>(worklet_settings_object.global_object());
    return global_scope.is_processor_registration_failed(name);
}

bool AudioWorklet::has_pending_module_promises() const
{
    return !m_pending_module_promises.is_empty();
}

bool AudioWorklet::needs_realtime_worklet_session() const
{
    if (is<OfflineAudioContext>(*m_context))
        return false;
    if (!m_loaded_module_sources.is_empty())
        return true;
    if (!m_pending_module_promises.is_empty())
        return true;
    if (m_realtime_global_port_fd >= 0)
        return true;
    if (!m_realtime_node_definitions.is_empty())
        return true;
    if (!m_realtime_processor_port_fds.is_empty())
        return true;
    return false;
}

Vector<AudioParamDescriptor> const* AudioWorklet::parameter_descriptors(String const& name)
{
    auto& worklet_settings_object = ensure_worklet_environment_settings_object();
    auto& global_scope = as<AudioWorkletGlobalScope>(worklet_settings_object.global_object());
    return global_scope.parameter_descriptors(name);
}

void AudioWorklet::register_processor_from_worker(String const& name, Vector<AudioParamDescriptor> const& descriptors)
{
    auto& worklet_settings_object = ensure_worklet_environment_settings_object();
    auto& global_scope = as<AudioWorkletGlobalScope>(worklet_settings_object.global_object());
    global_scope.register_processor_name(name);

    Vector<AudioParamDescriptor> descriptor_copy;
    descriptor_copy.ensure_capacity(descriptors.size());
    for (auto const& descriptor : descriptors)
        descriptor_copy.unchecked_append(descriptor);
    global_scope.set_parameter_descriptors(name, move(descriptor_copy));
}

void AudioWorklet::register_failed_processors_from_worker(Vector<String> const& names)
{
    if (names.is_empty())
        return;

    auto& worklet_settings_object = ensure_worklet_environment_settings_object();
    auto& global_scope = as<AudioWorkletGlobalScope>(worklet_settings_object.global_object());
    for (auto const& name : names)
        global_scope.mark_processor_registration_failed(name);
}

void AudioWorklet::handle_module_evaluated(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message)
{
    auto promise_it = m_pending_module_promises.find(module_id);
    if (promise_it == m_pending_module_promises.end())
        return;

    if (required_generation > m_registration_generation) {
        m_pending_module_generations.set(module_id, required_generation);
        return;
    }

    auto url_it = m_pending_module_urls.find(module_id);
    Optional<String> maybe_url;
    if (url_it != m_pending_module_urls.end())
        maybe_url = url_it->value;

    auto promises = move(promise_it->value);
    m_pending_module_promises.remove(promise_it);
    if (url_it != m_pending_module_urls.end())
        m_pending_module_urls.remove(url_it);
    m_pending_module_generations.remove(module_id);

    if (success && maybe_url.has_value())
        m_evaluated_module_urls.set(maybe_url.value());

    if (success)
        m_has_loaded_any_module = true;

    for (auto& promise : promises) {
        auto& outside_global_object = HTML::relevant_global_object(*m_context);
        GC::Ref<JS::Realm> outside_realm = promise->promise()->shape().realm();

        if (success) {
            HTML::queue_global_task(HTML::Task::Source::Networking, outside_global_object, GC::create_function(outside_realm->heap(), [promise, outside_realm] {
                HTML::TemporaryExecutionContext context(*outside_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                WebIDL::resolve_promise(*outside_realm, promise, JS::js_undefined());
            }));
        } else {
            String message = error_message.is_empty() ? "Failed to evaluate AudioWorklet module"_string : error_message;
            auto error_name_copy = error_name;
            HTML::queue_global_task(HTML::Task::Source::Networking, outside_global_object, GC::create_function(outside_realm->heap(), [promise, outside_realm, error_name = move(error_name_copy), message = move(message)] {
                HTML::TemporaryExecutionContext context(*outside_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                if (!is_dom_exception_name(error_name)) {
                    WebIDL::reject_promise(*outside_realm, promise, WebIDL::OperationError::create(*outside_realm, Utf16String::from_utf8(message.bytes_as_string_view())));
                    return;
                }

                WebIDL::reject_promise(*outside_realm, promise, WebIDL::DOMException::create(*outside_realm, FlyString { error_name }, Utf16String::from_utf8(message.bytes_as_string_view())));
            }));
        }
    }
}

void AudioWorklet::set_registration_generation(u64 generation)
{
    if (generation <= m_registration_generation)
        return;

    m_registration_generation = generation;

    Vector<u64> ready_modules;
    ready_modules.ensure_capacity(m_pending_module_generations.size());
    for (auto const& it : m_pending_module_generations) {
        if (it.value <= m_registration_generation)
            ready_modules.append(it.key);
    }

    for (auto module_id : ready_modules) {
        auto promise_it = m_pending_module_promises.find(module_id);
        if (promise_it == m_pending_module_promises.end()) {
            m_pending_module_generations.remove(module_id);
            continue;
        }

        auto url_it = m_pending_module_urls.find(module_id);
        Optional<String> maybe_url;
        if (url_it != m_pending_module_urls.end())
            maybe_url = url_it->value;

        auto promises = move(promise_it->value);
        m_pending_module_promises.remove(promise_it);
        if (url_it != m_pending_module_urls.end())
            m_pending_module_urls.remove(url_it);
        m_pending_module_generations.remove(module_id);

        if (maybe_url.has_value())
            m_evaluated_module_urls.set(maybe_url.value());

        m_has_loaded_any_module = true;

        for (auto& promise : promises) {
            auto& outside_global_object = HTML::relevant_global_object(*m_context);
            GC::Ref<JS::Realm> outside_realm = promise->promise()->shape().realm();
            HTML::queue_global_task(HTML::Task::Source::Networking, outside_global_object, GC::create_function(outside_realm->heap(), [promise, outside_realm] {
                HTML::TemporaryExecutionContext context(*outside_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                WebIDL::resolve_promise(*outside_realm, promise, JS::js_undefined());
            }));
        }
    }
}

Vector<Render::WorkletModule> AudioWorklet::loaded_modules() const
{
    Vector<Render::WorkletModule> modules;
    modules.ensure_capacity(m_loaded_module_sources.size());
    for (auto const& it : m_loaded_module_sources) {
        u64 module_id = 0;
        if (auto id = m_module_ids_by_url.get(it.key); id.has_value())
            module_id = id.value();
        modules.unchecked_append(Render::WorkletModule { .module_id = module_id, .url = it.key.to_byte_string(), .source_text = it.value });
    }
    return modules;
}

GC::Ref<HTML::MessagePort> AudioWorklet::port()
{
    if (m_port)
        return *m_port;

    bool const is_offline = is<OfflineAudioContext>(*m_context);
    auto& realm = this->realm();

    // OfflineAudioContext worklets are executed in-process, so the global port can be an entangled pair.
    if (is_offline) {
        auto worklet_port = HTML::MessagePort::create(realm);

        auto& worklet_settings_object = ensure_worklet_environment_settings_object();
        GC::Ref<JS::Realm> worklet_realm = worklet_settings_object.realm();
        auto& global_scope = as<AudioWorkletGlobalScope>(worklet_settings_object.global_object());

        // If the global scope already has a shared port (e.g. module code accessed the port), reuse it.
        GC::Ref<HTML::MessagePort> processor_port = global_scope.shared_port() ? GC::Ref<HTML::MessagePort> { *global_scope.shared_port() } : HTML::MessagePort::create(*worklet_realm);
        global_scope.set_shared_port(processor_port);

        worklet_port->entangle_with(*processor_port);

        m_port = worklet_port;
        return *m_port;
    }

    // Realtime AudioContext worklets are executed out-of-process; use an fd-backed transport and
    // publish the peer fd to the renderer via render graph updates.
    auto pair_or_error = Render::create_message_port_transport_pair(realm);
    if (pair_or_error.is_error()) {
        // Best-effort: return a detached port.
        m_port = HTML::MessagePort::create(realm);
        return *m_port;
    }

    auto pair = pair_or_error.release_value();
    m_port = pair.port;
    set_realtime_global_port_fd(pair.peer_fd);
    m_context->notify_audio_graph_changed();
    return *m_port;
}

void AudioWorklet::set_realtime_processor_port(NodeID node_id, int processor_port_fd)
{
    if (node_id.value() == 0 || processor_port_fd < 0)
        return;

    if (auto it = m_realtime_processor_port_fds.find(node_id); it != m_realtime_processor_port_fds.end()) {
        if (it->value >= 0)
            (void)Core::System::close(it->value);
        it->value = processor_port_fd;
        return;
    }

    m_realtime_processor_port_fds.set(node_id, processor_port_fd);
}

void AudioWorklet::set_realtime_global_port_fd(int processor_port_fd)
{
    if (processor_port_fd < 0)
        return;
    if (m_realtime_global_port_fd >= 0)
        (void)Core::System::close(m_realtime_global_port_fd);
    m_realtime_global_port_fd = processor_port_fd;
}

void AudioWorklet::register_realtime_node_definition(Render::WorkletNodeDefinition definition)
{
    if (definition.node_id.value() == 0)
        return;
    m_realtime_node_definitions.set(definition.node_id, move(definition));
}

void AudioWorklet::unregister_realtime_node_definition(NodeID node_id)
{
    if (node_id.value() == 0)
        return;

    m_realtime_node_definitions.remove(node_id);

    auto it = m_realtime_processor_port_fds.find(node_id);
    if (it == m_realtime_processor_port_fds.end())
        return;
    if (it->value >= 0)
        (void)Core::System::close(it->value);
    m_realtime_processor_port_fds.remove(it);
}

Vector<Render::WorkletNodeDefinition> AudioWorklet::realtime_node_definitions() const
{
    Vector<Render::WorkletNodeDefinition> definitions;
    definitions.ensure_capacity(m_realtime_node_definitions.size());
    for (auto const& it : m_realtime_node_definitions)
        definitions.unchecked_append(it.value);
    return definitions;
}

Vector<NodeID> AudioWorklet::realtime_node_ids() const
{
    Vector<NodeID> ids;
    ids.ensure_capacity(m_realtime_node_definitions.size());
    for (auto const& it : m_realtime_node_definitions)
        ids.unchecked_append(it.key);
    return ids;
}

Optional<int> AudioWorklet::clone_realtime_global_port_fd() const
{
    if (m_realtime_global_port_fd < 0)
        return {};

    auto dup_fd_or_error = Core::System::dup(m_realtime_global_port_fd);
    if (dup_fd_or_error.is_error())
        return {};
    return dup_fd_or_error.release_value();
}

Optional<int> AudioWorklet::clone_realtime_processor_port_fd(NodeID node_id) const
{
    auto it = m_realtime_processor_port_fds.find(node_id);
    if (it == m_realtime_processor_port_fds.end())
        return {};

    if (it->value < 0)
        return {};

    auto dup_fd_or_error = Core::System::dup(it->value);
    if (dup_fd_or_error.is_error())
        return {};
    return dup_fd_or_error.release_value();
}

void AudioWorklet::prune_realtime_processor_ports(Vector<NodeID> const& live_nodes)
{
    HashTable<NodeID> live;
    live.ensure_capacity(live_nodes.size());
    for (auto node_id : live_nodes)
        live.set(node_id);

    Vector<NodeID> to_remove;
    to_remove.ensure_capacity(m_realtime_processor_port_fds.size());
    for (auto const& it : m_realtime_processor_port_fds) {
        if (!live.contains(it.key))
            to_remove.append(it.key);
    }

    for (auto node_id : to_remove) {
        auto it = m_realtime_processor_port_fds.find(node_id);
        if (it == m_realtime_processor_port_fds.end())
            continue;
        if (it->value >= 0)
            (void)Core::System::close(it->value);
        m_realtime_processor_port_fds.remove(it);
    }
}

HTML::EnvironmentSettingsObject& AudioWorklet::worklet_environment_settings_object()
{
    return ensure_worklet_environment_settings_object();
}

HTML::EnvironmentSettingsObject& AudioWorklet::ensure_worklet_environment_settings_object()
{
    if (m_worklet_environment_settings_object)
        return *m_worklet_environment_settings_object;

    auto& outside_settings_object = HTML::relevant_settings_object(*m_context);
    auto serialized_outside_settings = outside_settings_object.serialize();

    auto& outside_realm = outside_settings_object.realm();
    auto& page = Bindings::principal_host_defined_page(outside_realm);

    URL::URL global_scope_url = outside_settings_object.api_base_url();

    GC::Ptr<AudioWorkletGlobalScope> global_scope;
    auto execution_context = Bindings::create_a_new_javascript_realm(
        Bindings::main_thread_vm(),
        [&](JS::Realm& realm) -> JS::Object* {
            global_scope = AudioWorkletGlobalScope::create(realm);
            return global_scope.ptr();
        },
        [&](JS::Realm&) -> JS::Object* {
            return global_scope.ptr();
        });

    VERIFY(global_scope);
    global_scope->set_current_frame(0);
    global_scope->set_sample_rate(m_context->sample_rate());

    m_worklet_environment_settings_object = AudioWorkletEnvironmentSettingsObject::setup(page, move(execution_context), serialized_outside_settings, global_scope_url);

    // The AudioWorkletGlobalScope must have its Web interfaces installed before any module runs.
    // Some processors read sampleRate while computing parameterDescriptors during registration.
    global_scope->initialize_web_interfaces();
    return *m_worklet_environment_settings_object;
}

// https://html.spec.whatwg.org/multipage/worklets.html#dom-audioworklet-addmodule
GC::Ref<WebIDL::Promise> AudioWorklet::add_module(String const& module_url)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 11.3.2 addModule 1. Let outsideSettings be the relevant settings object of this.
    auto& relevant_settings_object = HTML::relevant_settings_object(*m_context);
    auto base_url = relevant_settings_object.api_base_url();
    // 11.3.2 addModule 2. Let moduleURLRecord be the result of encoding-parsing a URL given moduleURL.
    auto url_record = DOMURL::parse(module_url, base_url);
    if (!url_record.has_value()) {
        // 11.3.2 addModule 3. If moduleURLRecord is failure, return a promise rejected with SyntaxError.
        auto promise = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "Invalid URL"_utf16));
        return promise;
    }

    // 11.3.2 addModule 4. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 11.3.2 addModule 5. Let workletInstance be this.
    // AD-HOC: We use the AudioWorklet instance directly below.

    // 11.3.2 addModule 6. Run the following steps in parallel.
    // AD-HOC: We run these steps synchronously on the control thread.

    // 11.3.2 addModule 6.1. If global scopes is empty, create a worklet global scope.
    // AD-HOC: ensure_worklet_environment_settings_object creates the AudioWorkletGlobalScope on demand.
    auto& worklet_settings_object = ensure_worklet_environment_settings_object();
    GC::Ref<JS::Realm> worklet_realm = worklet_settings_object.realm();

    auto& outside_global_object = HTML::relevant_global_object(*m_context);
    GC::Ref<JS::Object> outside_global = outside_global_object;

    auto resolved_url = url_record->serialize();
    bool const is_offline = is<OfflineAudioContext>(*m_context);

    // 11.3.2 addModule 6.2. Let pendingTasks be global scopes size.
    // AD-HOC: m_pending_module_promises tracks pending worklet module tasks.

    // 11.3.2 addModule 6.3. Let addedSuccessfully be false.
    // AD-HOC: We track this using m_evaluated_module_urls and m_has_loaded_any_module.

    // 11.3.2 addModule 6.4. For each workletGlobalScope, queue a global task to fetch a worklet script graph.
    // AD-HOC: fetch_worklet_module_worker_script_graph encapsulates the fetch and module script creation.

    // AD-HOC: If a module with the same URL is already evaluated, resolve the promise.
    if (!is_offline && m_evaluated_module_urls.contains(resolved_url)) {
        m_has_loaded_any_module = true;
        GC::Ref<JS::Realm> outside_realm = realm;
        HTML::queue_global_task(HTML::Task::Source::Networking, *outside_global, GC::create_function(outside_realm->heap(), [promise, outside_realm] {
            HTML::TemporaryExecutionContext context(*outside_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::resolve_promise(*outside_realm, promise, JS::js_undefined());
        }));
        return promise;
    }

    // 11.3.2 addModule 6.4. Queue a global task to fetch a worklet script graph.
    auto on_complete = HTML::create_on_fetch_script_complete(vm.heap(), [this, promise, outside_global, worklet_realm, resolved_url, is_offline](GC::Ptr<HTML::Script> result) mutable {
        GC::Ref<AudioWorklet> self = GC::Ref<AudioWorklet> { *this };
        GC::Ref<JS::Realm> outside_realm = this->realm();

        if (!result) {
            // 11.3.2 addModule 6.4.1. If script is null, reject promise with AbortError.
            // AD-HOC: Network failures from fetch_worklet_module_worker_script_graph map to NetworkError.
            HTML::queue_global_task(HTML::Task::Source::Networking, *outside_global, GC::create_function(outside_realm->heap(), [promise, outside_realm]() {
                HTML::TemporaryExecutionContext context(*outside_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                WebIDL::reject_promise(*outside_realm, promise, WebIDL::NetworkError::create(*outside_realm, "Failed to load module"_utf16));
            }));
            return;
        }

        auto& module_script = as<HTML::JavaScriptModuleScript>(*result);

        // 11.3.2 addModule 6.4.2. If script error to rethrow is not null, reject promise.
        // AD-HOC: JavaScriptModuleScript.run delivers errors via its promise.

        // AD-HOC: Cache the fetched module source so the realtime renderer can mirror it.
        m_loaded_module_sources.set(resolved_url, module_script.source_text());

        if (is_offline) {
            // 11.3.2 addModule 6.4.3. If addedSuccessfully is false, append moduleURLRecord to added modules list.
            // AD-HOC: Worklet module tracking is handled by m_evaluated_module_urls.

            // 11.3.2 addModule 6.4.4. Run a module script given script.
            // AD-HOC: Offline contexts run the module here.
            auto* evaluation_promise = module_script.run(HTML::JavaScriptModuleScript::PreventErrorReporting::Yes);

            auto on_fulfilled_steps = [self, promise, outside_global](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                GC::Ref<JS::Realm> outside_realm = promise->promise()->shape().realm();
                HTML::queue_global_task(HTML::Task::Source::Networking, *outside_global, GC::create_function(outside_realm->heap(), [self, promise, outside_realm]() {
                    HTML::TemporaryExecutionContext context(*outside_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                    self->m_has_loaded_any_module = true;
                    WebIDL::resolve_promise(*outside_realm, promise, JS::js_undefined());
                }));
                return JS::js_undefined();
            };

            auto on_rejected_steps = [promise, outside_global](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto reason = vm.argument(0);
                GC::Ref<JS::Realm> outside_realm = promise->promise()->shape().realm();
                HTML::queue_global_task(HTML::Task::Source::Networking, *outside_global, GC::create_function(outside_realm->heap(), [promise, outside_realm, reason]() {
                    HTML::TemporaryExecutionContext context(*outside_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                    WebIDL::reject_promise(*outside_realm, promise, reason);
                }));
                return JS::js_undefined();
            };

            auto on_fulfilled = JS::NativeFunction::create(*worklet_realm, move(on_fulfilled_steps), 1);
            auto on_rejected = JS::NativeFunction::create(*worklet_realm, move(on_rejected_steps), 1);
            static_cast<JS::Promise&>(*evaluation_promise).perform_then(on_fulfilled, on_rejected, nullptr);
            return;
        }

        // 11.3.2 addModule 6.4.4. Run a module script given script.
        // AD-HOC: Realtime contexts defer module evaluation to the worklet host.
        u64 module_id = 0;
        auto id_it = self->m_module_ids_by_url.find(resolved_url);
        if (id_it == self->m_module_ids_by_url.end()) {
            module_id = self->m_next_module_id++;
            self->m_module_ids_by_url.set(resolved_url, module_id);
        } else {
            module_id = id_it->value;
        }

        bool first_pending_for_module = false;
        auto pending_it = self->m_pending_module_promises.find(module_id);
        if (pending_it == self->m_pending_module_promises.end()) {
            self->m_pending_module_promises.set(module_id, {});
            pending_it = self->m_pending_module_promises.find(module_id);
            first_pending_for_module = true;
        }
        pending_it->value.append(promise);
        self->m_pending_module_urls.set(module_id, resolved_url);

        // AD-HOC: Ensure the global port transport exists so module evaluation can run.
        if (self->m_realtime_global_port_fd < 0)
            (void)self->port();

        // 11.3.2 addModule 6.4.5. Queue a global task to resolve promise when pendingTasks reaches zero.
        // AD-HOC: Resolve or reject happens in handle_module_evaluated.
        if (first_pending_for_module)
            self->m_context->notify_audio_graph_changed();
    });

    auto fetch_result = HTML::fetch_worklet_module_worker_script_graph(
        *url_record,
        relevant_settings_object,
        Fetch::Infrastructure::Request::Destination::AudioWorklet,
        worklet_settings_object,
        nullptr,
        on_complete);

    if (fetch_result.is_exception())
        WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), fetch_result.release_error()).release_value());

    return promise;
}

void AudioWorklet::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioWorklet);
    Base::initialize(realm);
}

void AudioWorklet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
    visitor.visit(m_worklet_environment_settings_object);
    for (auto& it : m_pending_module_promises) {
        for (auto& promise : it.value)
            visitor.visit(promise);
    }
}

}
