/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Scripting/WorkerAgent.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebAudio/AudioWorkletEnvironmentSettingsObject.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletPageClient.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorInvoker.h>
#include <LibWeb/WebAudio/Worklet/MessagePortTransport.h>
#include <LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.h>

namespace Web::WebAudio::Render {

static bool processor_instance_defines_process(JS::Object& instance, JS::FunctionObject& constructor, JS::VM& vm)
{
    auto process_key = JS::PropertyKey { "process"_utf16_fly_string };
    auto has_own_process_or_error = instance.has_own_property(process_key);
    if (has_own_process_or_error.is_error())
        return false;
    if (has_own_process_or_error.release_value())
        return true;

    auto prototype_value_or_error = constructor.get(vm.names.prototype);
    if (prototype_value_or_error.is_error())
        return false;
    auto prototype_value = prototype_value_or_error.release_value();
    if (!prototype_value.is_object())
        return false;

    auto& prototype_object = prototype_value.as_object();
    auto proto_has_own_or_error = prototype_object.has_own_property(process_key);
    if (proto_has_own_or_error.is_error())
        return false;
    return proto_has_own_or_error.release_value();
}

RealtimeAudioWorkletProcessorHost::RealtimeAudioWorkletProcessorHost(u64 initial_current_frame, float initial_sample_rate, Vector<WorkletModule> modules, Vector<WorkletNodeDefinition> node_definitions, Vector<WorkletPortBinding> port_bindings)
    : m_modules(move(modules))
    , m_initial_current_frame(initial_current_frame)
    , m_initial_sample_rate(initial_sample_rate)
{
    m_processor_port_fds.ensure_capacity(port_bindings.size());
    for (auto& binding : port_bindings) {
        if (binding.processor_port_fd < 0)
            continue;
        m_processor_port_fds.set(binding.node_id, binding.processor_port_fd);
        binding.processor_port_fd = -1;
    }

    m_nodes.ensure_capacity(node_definitions.size());

    for (auto& def : node_definitions) {
        auto node = make<SharedNode>();
        node->node_id = def.node_id;
        node->processor_name = def.processor_name.to_byte_string();
        node->number_of_inputs = def.number_of_inputs;
        node->number_of_outputs = def.number_of_outputs;
        node->output_channel_count = move(def.output_channel_count);
        node->channel_count = max<size_t>(1, def.channel_count);
        node->channel_count_mode = def.channel_count_mode;
        node->channel_interpretation = def.channel_interpretation;
        node->parameter_names.ensure_capacity(def.parameter_names.size());
        for (auto const& name : def.parameter_names)
            node->parameter_names.append(name.to_byte_string());

        node->parameter_data = def.parameter_data;
        node->serialized_processor_options = def.serialized_processor_options;

        m_nodes_by_id.set(node->node_id, node.ptr());
        m_nodes.unchecked_append(move(node));
    }
}

void RealtimeAudioWorkletProcessorHost::enqueue_worklet_module(WorkletModule module)
{
    {
        Threading::MutexLocker locker(m_update_mutex);
        m_pending_modules.append(move(module));
    }
}

void RealtimeAudioWorkletProcessorHost::enqueue_node_definitions(Vector<WorkletNodeDefinition> definitions)
{
    {
        Threading::MutexLocker locker(m_update_mutex);
        m_pending_node_definitions.extend(move(definitions));
    }
}

void RealtimeAudioWorkletProcessorHost::synchronize_node_definitions(Vector<WorkletNodeDefinition> const& definitions)
{
    if (definitions.is_empty()) {
        return;
    }

    Threading::MutexLocker locker(m_nodes_mutex);
    for (auto const& def : definitions) {
        auto it = m_nodes_by_id.find(def.node_id);
        if (it == m_nodes_by_id.end()) {
            auto node = make<SharedNode>();
            node->node_id = def.node_id;
            node->processor_name = def.processor_name.to_byte_string();
            node->number_of_inputs = def.number_of_inputs;
            node->number_of_outputs = def.number_of_outputs;
            node->output_channel_count = def.output_channel_count;
            node->channel_count = max<size_t>(1, def.channel_count);
            node->channel_count_mode = def.channel_count_mode;
            node->channel_interpretation = def.channel_interpretation;

            node->parameter_names.ensure_capacity(def.parameter_names.size());
            for (auto const& name : def.parameter_names)
                node->parameter_names.append(name.to_byte_string());

            node->parameter_data = def.parameter_data;
            node->serialized_processor_options = def.serialized_processor_options;

            m_nodes_by_id.set(node->node_id, node.ptr());
            m_nodes.append(move(node));
            continue;
        }

        auto* existing = it->value;
        existing->processor_name = def.processor_name.to_byte_string();
        existing->number_of_inputs = def.number_of_inputs;
        existing->number_of_outputs = def.number_of_outputs;
        existing->output_channel_count = def.output_channel_count;
        existing->channel_count = max<size_t>(1, def.channel_count);
        existing->channel_count_mode = def.channel_count_mode;
        existing->channel_interpretation = def.channel_interpretation;

        existing->parameter_names.clear();
        existing->parameter_names.ensure_capacity(def.parameter_names.size());
        for (auto const& name : def.parameter_names)
            existing->parameter_names.append(name.to_byte_string());

        existing->parameter_data = def.parameter_data;
        existing->serialized_processor_options = def.serialized_processor_options;
    }
}

void RealtimeAudioWorkletProcessorHost::enqueue_port_bindings(Vector<WorkletPortBinding> const& port_bindings)
{
    {
        Threading::MutexLocker locker(m_update_mutex);
        m_pending_port_bindings.ensure_capacity(m_pending_port_bindings.size() + port_bindings.size());
        for (auto const& binding : port_bindings) {
            if (binding.processor_port_fd < 0)
                continue;
            m_pending_port_bindings.append(binding);
        }
    }
}

void RealtimeAudioWorkletProcessorHost::set_processor_error_callback(Function<void(NodeID)> callback)
{
    Threading::MutexLocker locker(m_callback_mutex);
    m_processor_error_callback = move(callback);
}

void RealtimeAudioWorkletProcessorHost::set_processor_registration_callback(Function<void(String const&, Vector<AudioParamDescriptor> const&, u64)> callback)
{
    Threading::MutexLocker locker(m_callback_mutex);
    m_processor_registration_callback = move(callback);
}

void RealtimeAudioWorkletProcessorHost::set_worklet_module_evaluation_callback(Function<void(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations)> callback)
{
    Threading::MutexLocker locker(m_callback_mutex);
    m_module_evaluation_callback = move(callback);
}

void RealtimeAudioWorkletProcessorHost::notify_processor_registered(String const& name, Vector<AudioParamDescriptor> const& descriptors)
{
    ASSERT_RENDER_THREAD();
    u64 generation = m_processor_registration_generation.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) + 1;
    Threading::MutexLocker locker(m_callback_mutex);
    if (m_processor_registration_callback)
        m_processor_registration_callback(name, descriptors, generation);
}

void RealtimeAudioWorkletProcessorHost::notify_module_evaluated(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations)
{
    ASSERT_RENDER_THREAD();
    Threading::MutexLocker locker(m_callback_mutex);
    if (m_module_evaluation_callback)
        m_module_evaluation_callback(module_id, required_generation, success, error_name, error_message, move(failed_processor_registrations));
}

RealtimeAudioWorkletProcessorHost::~RealtimeAudioWorkletProcessorHost()
{
    Threading::MutexLocker locker(m_update_mutex);
    for (auto& binding : m_pending_port_bindings) {
        if (binding.processor_port_fd >= 0)
            (void)Core::System::close(binding.processor_port_fd);
        binding.processor_port_fd = -1;
    }
    m_pending_port_bindings.clear();

    for (auto const& it : m_processor_port_fds) {
        if (it.value >= 0)
            (void)Core::System::close(it.value);
    }
}

RealtimeAudioWorkletProcessorHost::SharedNode* RealtimeAudioWorkletProcessorHost::find_node(NodeID id)
{
    ASSERT_RENDER_THREAD();
    Threading::MutexLocker locker(m_nodes_mutex);
    auto it = m_nodes_by_id.find(id);
    if (it == m_nodes_by_id.end())
        return nullptr;
    return it->value;
}

void RealtimeAudioWorkletProcessorHost::consume_pending_updates(Vector<WorkletModule>& out_new_modules, Vector<WorkletNodeDefinition>& out_node_definitions, Vector<WorkletPortBinding>& out_port_bindings)
{
    ASSERT_RENDER_THREAD();
    Threading::MutexLocker locker(m_update_mutex);
    out_new_modules = move(m_pending_modules);
    out_node_definitions = move(m_pending_node_definitions);
    out_port_bindings = move(m_pending_port_bindings);
}

struct RealtimeAudioWorkletProcessorHost::RenderThreadState {
    Core::EventLoop* core_event_loop { nullptr };
    RefPtr<JS::VM> vm;
    HTML::EventLoop* html_event_loop { nullptr };
    Optional<u64> last_processed_frame;
    Optional<MonotonicTime> last_pump_time;
    GC::Ref<AudioWorkletPageClient> page_client;
    GC::Ptr<AudioWorkletGlobalScope> global_scope;
    GC::Ref<JS::Realm> realm;
    GC::Root<HTML::MessagePort> shared_port;
    HashMap<NodeID, GC::Root<HTML::MessagePort>> processor_ports;
    HashTable<NodeID> ports_with_transport;
    HashMap<NodeID, GC::Root<JS::Object>> instances;
    HashMap<NodeID, ByteString> instance_processor_names;
    HashMap<NodeID, bool> instance_has_process;
};

bool RealtimeAudioWorkletProcessorHost::has_pending_worklet_tasks(RenderThreadState& state)
{
    ASSERT_RENDER_THREAD();
    if (state.html_event_loop->task_queue().has_runnable_tasks())
        return true;
    if (!state.html_event_loop->microtask_queue().is_empty())
        return true;

    auto* agent = dynamic_cast<HTML::Agent*>(state.vm->agent());
    VERIFY(agent);
    if (agent->event_loop->task_queue().has_runnable_tasks())
        return true;
    if (!agent->event_loop->microtask_queue().is_empty())
        return true;

    return false;
}

RealtimeAudioWorkletProcessorHost::RenderThreadState& RealtimeAudioWorkletProcessorHost::ensure_render_thread_state()
{
    ASSERT_RENDER_THREAD();
    if (m_render_thread_state)
        return *m_render_thread_state;
    RenderThreadState state = create_render_thread_state();
    evaluate_modules(state, m_modules);
    initialize_ports(state);
    for (auto const& it : m_processor_port_fds)
        try_attach_port_transport(state, it.key);
    ensure_ready_processor_instances(state);

    m_render_thread_state = make<RenderThreadState>(move(state));
    return *m_render_thread_state;
}

bool RealtimeAudioWorkletProcessorHost::process_audio_worklet(
    NodeID node_id,
    RenderContext& process_context,
    [[maybe_unused]] String const& processor_name,
    [[maybe_unused]] size_t number_of_inputs,
    [[maybe_unused]] size_t number_of_outputs,
    [[maybe_unused]] Span<size_t const> output_channel_count,
    Vector<Vector<AudioBus const*>> const& inputs,
    Span<AudioBus*> outputs,
    Span<ParameterSpan const> parameters)
{
    ASSERT_RENDER_THREAD();
    auto& state = ensure_render_thread_state();
    process_pending_updates(state);

    auto* node = find_node(node_id);
    if (!node)
        return true;

    if (state.last_processed_frame.has_value() && process_context.current_frame != state.last_processed_frame.value())
        pump_event_loops(state);

    state.global_scope->set_current_frame(process_context.current_frame);
    state.global_scope->set_sample_rate(process_context.sample_rate);

    auto* processor = ensure_processor_instance(state, *node);
    if (!processor) {
        Threading::MutexLocker locker(m_callback_mutex);
        if (m_processor_error_callback)
            m_processor_error_callback(node_id);
        return false;
    }

    if (state.instance_has_process.contains(node_id) && !state.instance_has_process.get(node_id).value()) {
        Threading::MutexLocker locker(m_callback_mutex);
        if (m_processor_error_callback)
            m_processor_error_callback(node_id);
        return false;
    }

    JS::ExecutionContext* process_execution_context = nullptr;
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(process_execution_context, 0, 0, 0);
    process_execution_context->realm = state.realm;
    state.vm->push_execution_context(*process_execution_context);

    auto keep_alive_or_error = invoke_audio_worklet_processor_process(
        *state.realm,
        *processor,
        inputs,
        outputs,
        parameters,
        process_context.quantum_size);
    state.vm->pop_execution_context();

    bool keep_alive = true;
    if (keep_alive_or_error.is_error()) {
        keep_alive = false;
        Threading::MutexLocker locker(m_callback_mutex);
        if (m_processor_error_callback)
            m_processor_error_callback(node_id);
    } else {
        keep_alive = keep_alive_or_error.release_value();
    }

    state.last_processed_frame = process_context.current_frame;
    return keep_alive;
}

RealtimeAudioWorkletProcessorHost::RenderThreadState RealtimeAudioWorkletProcessorHost::create_render_thread_state()
{
    ASSERT_RENDER_THREAD();
    static OwnPtr<Core::EventLoop> s_core_event_loop;
    static RefPtr<JS::VM> s_vm;

    if (!s_core_event_loop)
        s_core_event_loop = make<Core::EventLoop>();

    if (!s_vm) {
        s_vm = JS::VM::create();
        s_vm->set_agent(HTML::WorkerAgent::create(s_vm->heap(), JS::Agent::CanBlock::Yes));
    }

    auto vm = s_vm;

    auto& worklet_vm = *vm;
    auto* html_agent = dynamic_cast<HTML::Agent*>(worklet_vm.agent());
    VERIFY(html_agent);
    VERIFY(html_agent->event_loop);
    HTML::EventLoop& html_event_loop = *html_agent->event_loop;

    GC::Ptr<HTML::EventLoop> event_loop = html_agent->event_loop.ptr();
    worklet_vm.host_enqueue_promise_job = [&worklet_vm, event_loop](GC::Ref<GC::Function<JS::ThrowCompletionOr<JS::Value>()>> job, JS::Realm* realm) {
        VERIFY(realm);
        auto script_or_module = worklet_vm.get_active_script_or_module();
        auto& heap = realm->heap();
        HTML::queue_a_task(HTML::Task::Source::Microtask, event_loop, nullptr, GC::create_function(heap, [realm, job, script_or_module = move(script_or_module)] {
            HTML::TemporaryExecutionContext temporary_context(*realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            HTML::execution_context_of_realm(*realm).script_or_module = script_or_module;
            auto result = job->function()();
            HTML::execution_context_of_realm(*realm).script_or_module = Empty {};
            if (result.is_error())
                HTML::report_exception(result, *realm);
        }));
    };

    GC::Ptr<AudioWorkletGlobalScope> global_scope;
    auto realm_execution_context = Bindings::create_a_new_javascript_realm(
        *vm,
        [&](JS::Realm& realm) -> JS::Object* {
            global_scope = AudioWorkletGlobalScope::create(realm);
            return global_scope.ptr();
        },
        [&](JS::Realm&) -> JS::Object* {
            return global_scope.ptr();
        });

    auto realm_ptr = realm_execution_context->realm;
    VERIFY(realm_ptr);

    GC::Ref<JS::Realm> realm = *realm_ptr;
    GC::Ref<AudioWorkletPageClient> page_client = AudioWorkletPageClient::create(*vm);

    URL::URL global_scope_url;
    if (!m_modules.is_empty()) {
        if (auto parsed_url = URL::Parser::basic_parse(m_modules.first().url.view()); parsed_url.has_value())
            global_scope_url = parsed_url.release_value();
        else
            global_scope_url = URL::URL::about("blank"_string);
    } else {
        global_scope_url = URL::URL::about("blank"_string);
    }

    HTML::SerializedEnvironmentSettingsObject outside_settings {
        .id = "audio-worklet"_string,
        .creation_url = global_scope_url,
        .top_level_creation_url = {},
        .top_level_origin = {},
        .api_base_url = global_scope_url,
        .origin = global_scope_url.origin(),
        .has_cross_site_ancestor = false,
        .policy_container = {},
        .cross_origin_isolated_capability = HTML::CanUseCrossOriginIsolatedAPIs::No,
        .time_origin = 0,
    };

    (void)AudioWorkletEnvironmentSettingsObject::setup(page_client->page_ref(), move(realm_execution_context), outside_settings, global_scope_url);

    GC::Root<HTML::MessagePort> shared_port = GC::make_root(HTML::MessagePort::create(*realm));
    shared_port->set_task_source(HTML::Task::Source::AudioWorklet);
    shared_port->enable();
    global_scope->set_shared_port(*shared_port);
    global_scope->set_processor_registration_callback([this](String const& name, Vector<AudioParamDescriptor> const& descriptors) {
        notify_processor_registered(name, descriptors);
    });

    return RenderThreadState {
        .core_event_loop = s_core_event_loop.ptr(),
        .vm = move(vm),
        .html_event_loop = &html_event_loop,
        .last_processed_frame = {},
        .last_pump_time = {},
        .page_client = page_client,
        .global_scope = global_scope,
        .realm = realm,
        .shared_port = move(shared_port),
        .processor_ports = {},
        .ports_with_transport = {},
        .instances = {},
        .instance_processor_names = {},
        .instance_has_process = {},
    };
}

void RealtimeAudioWorkletProcessorHost::service_render_thread_state(u64 current_frame, float sample_rate)
{
    ASSERT_RENDER_THREAD();
    auto& state = ensure_render_thread_state();
    process_pending_updates(state);
    ensure_ready_processor_instances(state);

    bool frame_advanced = !state.last_processed_frame.has_value() || current_frame != state.last_processed_frame.value();
    bool should_pump = frame_advanced || has_pending_worklet_tasks(state);

    if (!should_pump) {
        constexpr i64 min_pump_interval_ms = 4;
        auto now = MonotonicTime::now_coarse();
        if (!state.last_pump_time.has_value() || (now - state.last_pump_time.value()).to_milliseconds() >= min_pump_interval_ms)
            should_pump = true;
    }

    if (!should_pump)
        return;

    state.global_scope->set_current_frame(current_frame);
    state.global_scope->set_sample_rate(sample_rate);

    pump_event_loops(state);

    state.last_processed_frame = current_frame;
    state.last_pump_time = MonotonicTime::now_coarse();
}

void RealtimeAudioWorkletProcessorHost::evaluate_modules(RenderThreadState& state, Vector<WorkletModule> const& modules)
{
    ASSERT_RENDER_THREAD();
    // Best-effort module evaluation. Imported modules are not supported yet.
    HashTable<ByteString> evaluated_urls;
    evaluated_urls.ensure_capacity(modules.size());
    auto weak_this = make_weak_ptr();

    for (auto const& module : modules) {
        if (evaluated_urls.contains(module.url)) {
            if (auto* self = weak_this.ptr()) {
                Vector<String> failed_registrations;
                if (auto it = self->m_failed_processor_registrations_by_url.find(module.url); it != self->m_failed_processor_registrations_by_url.end())
                    failed_registrations = it->value;
                self->notify_module_evaluated(module.module_id, self->m_processor_registration_generation.load(AK::MemoryOrder::memory_order_relaxed), true, {}, {}, move(failed_registrations));
            }
            continue;
        }
        evaluated_urls.set(module.url);

        auto parse_result = JS::SourceTextModule::parse(module.source_text.view(), *state.realm, module.url.view(), nullptr);
        if (parse_result.is_error()) {
            if (auto* self = weak_this.ptr())
                self->notify_module_evaluated(module.module_id, self->m_processor_registration_generation.load(AK::MemoryOrder::memory_order_relaxed), false, "OperationError"_string, "AudioWorklet module parse error"_string, {});
            continue;
        }

        auto record = parse_result.release_value();
        JS::ExecutionContext* module_execution_context = nullptr;
        ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(module_execution_context, 0, 0, 0);
        module_execution_context->realm = state.realm;
        module_execution_context->script_or_module = GC::Ref<JS::Module> { *record };
        state.vm->push_execution_context(*module_execution_context);

        (void)record->load_requested_modules(nullptr);

        auto link_or_error = record->link(*state.vm);
        if (link_or_error.is_error()) {
            if (should_log_all())
                dbgln("[WebAudio] AudioWorklet: failed to link module {}", module.url);
            state.vm->pop_execution_context();
            if (auto* self = weak_this.ptr())
                self->notify_module_evaluated(module.module_id, self->m_processor_registration_generation.load(AK::MemoryOrder::memory_order_relaxed), false, "OperationError"_string, "AudioWorklet module link error"_string, {});
            continue;
        }

        auto promise_or_error = record->evaluate(*state.vm);
        if (promise_or_error.is_error()) {
            state.vm->pop_execution_context();
            if (auto* self = weak_this.ptr())
                self->notify_module_evaluated(module.module_id, self->m_processor_registration_generation.load(AK::MemoryOrder::memory_order_relaxed), false, "OperationError"_string, "AudioWorklet module evaluation failed"_string, {});
            continue;
        }
        auto evaluation_promise = promise_or_error.release_value();
        state.vm->pop_execution_context();

        if (should_log_all())
            dbgln("[WebAudio] AudioWorklet: evaluated module {}", module.url);

        auto module_id = module.module_id;
        auto module_url = module.url;
        auto* state_ptr = &state;
        auto on_fulfilled_steps = [weak_this, module_id, module_url, state_ptr](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
            if (auto* self = weak_this.ptr()) {
                u64 stable_generation = self->stabilize_registration_generation(*state_ptr);
                Vector<String> failed_registrations;
                if (state_ptr && state_ptr->global_scope)
                    failed_registrations = state_ptr->global_scope->take_failed_processor_registrations();
                if (!failed_registrations.is_empty())
                    self->m_failed_processor_registrations_by_url.set(module_url, failed_registrations);
                self->notify_module_evaluated(module_id, stable_generation, true, {}, {}, move(failed_registrations));
            }
            return JS::js_undefined();
        };

        auto on_rejected_steps = [weak_this, module_id, module_url, state_ptr](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            String message = "AudioWorklet module evaluation rejected"_string;
            String error_name;
            auto reason = vm.argument(0);
            if (reason.is_object()) {
                auto name_value_or_error = reason.as_object().get("name"_utf16_fly_string);
                if (!name_value_or_error.is_error()) {
                    auto name_value = name_value_or_error.release_value();
                    auto name_string_or_error = name_value.to_string(vm);
                    if (!name_string_or_error.is_error())
                        error_name = name_string_or_error.release_value();
                }
            }

            auto reason_string_or_error = reason.to_string(vm);
            if (!reason_string_or_error.is_error())
                message = reason_string_or_error.release_value();

            if (auto* self = weak_this.ptr()) {
                u64 stable_generation = self->stabilize_registration_generation(*state_ptr);
                Vector<String> failed_registrations;
                if (state_ptr && state_ptr->global_scope)
                    failed_registrations = state_ptr->global_scope->take_failed_processor_registrations();
                if (!failed_registrations.is_empty())
                    self->m_failed_processor_registrations_by_url.set(module_url, failed_registrations);
                auto name = error_name.is_empty() ? "OperationError"_string : error_name;
                self->notify_module_evaluated(module_id, stable_generation, false, name, message, move(failed_registrations));
            }
            return JS::js_undefined();
        };

        auto on_fulfilled = JS::NativeFunction::create(*state.realm, move(on_fulfilled_steps), 1, Utf16FlyString {}, state.realm.ptr());
        auto on_rejected = JS::NativeFunction::create(*state.realm, move(on_rejected_steps), 1, Utf16FlyString {}, state.realm.ptr());
        static_cast<JS::Promise&>(*evaluation_promise).perform_then(on_fulfilled, on_rejected, nullptr);

        for (size_t i = 0; i < 64; ++i) {
            if (!has_pending_worklet_tasks(state))
                break;
            pump_event_loops(state);
        }
    }
}

void RealtimeAudioWorkletProcessorHost::initialize_ports(RenderThreadState& state)
{
    ASSERT_RENDER_THREAD();
    Threading::MutexLocker locker(m_nodes_mutex);
    for (auto const& node : m_nodes) {
        if (!state.processor_ports.contains(node->node_id)) {
            auto port = HTML::MessagePort::create(*state.realm);
            port->set_task_source(HTML::Task::Source::AudioWorklet);
            port->enable();
            state.processor_ports.set(node->node_id, GC::make_root(port));
        }
    }
}

void RealtimeAudioWorkletProcessorHost::ensure_node_exists(RenderThreadState& state, WorkletNodeDefinition const& def)
{
    ASSERT_RENDER_THREAD();
    SharedNode* existing = nullptr;
    bool processor_changed = false;
    {
        Threading::MutexLocker locker(m_nodes_mutex);
        auto it = m_nodes_by_id.find(def.node_id);
        if (it != m_nodes_by_id.end())
            existing = it->value;

        if (existing) {
            ByteString const new_processor_name = def.processor_name.to_byte_string();
            if (existing->processor_name != new_processor_name)
                processor_changed = true;

            existing->processor_name = new_processor_name;
            existing->number_of_inputs = def.number_of_inputs;
            existing->number_of_outputs = def.number_of_outputs;
            existing->output_channel_count = def.output_channel_count;
            existing->channel_count = max<size_t>(1, def.channel_count);
            existing->channel_count_mode = def.channel_count_mode;
            existing->channel_interpretation = def.channel_interpretation;

            existing->parameter_names.clear();
            existing->parameter_names.ensure_capacity(def.parameter_names.size());
            for (auto const& name : def.parameter_names)
                existing->parameter_names.append(name.to_byte_string());

            existing->parameter_data = def.parameter_data;
            existing->serialized_processor_options = def.serialized_processor_options;
        } else {
            auto node = make<SharedNode>();
            node->node_id = def.node_id;
            node->processor_name = def.processor_name.to_byte_string();
            node->number_of_inputs = def.number_of_inputs;
            node->number_of_outputs = def.number_of_outputs;
            node->output_channel_count = def.output_channel_count;
            node->channel_count = max<size_t>(1, def.channel_count);
            node->channel_count_mode = def.channel_count_mode;
            node->channel_interpretation = def.channel_interpretation;
            node->parameter_names.ensure_capacity(def.parameter_names.size());
            for (auto const& name : def.parameter_names)
                node->parameter_names.append(name.to_byte_string());

            node->parameter_data = def.parameter_data;
            node->serialized_processor_options = def.serialized_processor_options;

            existing = node.ptr();
            m_nodes_by_id.set(node->node_id, node.ptr());
            m_nodes.append(move(node));
        }
    }

    if (existing && processor_changed) {
        state.instances.remove(def.node_id);
        state.instance_processor_names.remove(def.node_id);
        state.instance_has_process.remove(def.node_id);
    }

    if (!state.processor_ports.contains(def.node_id)) {
        auto port = HTML::MessagePort::create(*state.realm);
        port->set_task_source(HTML::Task::Source::AudioWorklet);
        port->enable();
        state.processor_ports.set(def.node_id, GC::make_root(port));
    }
}

void RealtimeAudioWorkletProcessorHost::try_attach_port_transport(RenderThreadState& state, NodeID node_id)
{
    ASSERT_RENDER_THREAD();
    if (state.ports_with_transport.contains(node_id))
        return;

    auto fd_it = m_processor_port_fds.find(node_id);
    if (fd_it == m_processor_port_fds.end())
        return;

    int fd = fd_it->value;
    if (fd < 0)
        return;

    if (node_id == NodeID { 0 }) {
        MUST(attach_message_port_transport_from_fd(*state.shared_port, fd));
        state.ports_with_transport.set(node_id);
        fd_it->value = -1;
        return;
    }

    auto port_it = state.processor_ports.find(node_id);
    if (port_it == state.processor_ports.end())
        return;

    MUST(attach_message_port_transport_from_fd(*port_it->value, fd));
    state.ports_with_transport.set(node_id);
    fd_it->value = -1;
}

void RealtimeAudioWorkletProcessorHost::ensure_ready_processor_instances(RenderThreadState& state)
{
    ASSERT_RENDER_THREAD();
    try_attach_port_transport(state, NodeID { 0 });

    Vector<SharedNode*> nodes_to_check;
    {
        Threading::MutexLocker locker(m_nodes_mutex);
        nodes_to_check.ensure_capacity(m_nodes.size());
        for (auto& node : m_nodes)
            nodes_to_check.append(node.ptr());
    }

    quick_sort(nodes_to_check, [](SharedNode const* a, SharedNode const* b) {
        if (a == nullptr || b == nullptr)
            return a < b;
        return a->node_id.value() < b->node_id.value();
    });

    for (auto* node : nodes_to_check) {
        if (!node)
            continue;

        if (state.instances.contains(node->node_id))
            continue;

        try_attach_port_transport(state, node->node_id);

        if (!state.ports_with_transport.contains(node->node_id))
            continue;

        auto processor_ctor = state.global_scope->processor_constructor(String::from_byte_string(node->processor_name).release_value_but_fixme_should_propagate_errors());
        if (!processor_ctor.is_function())
            continue;

        state.global_scope->set_current_frame(m_initial_current_frame);
        state.global_scope->set_sample_rate(m_initial_sample_rate);

        auto processor_port_it = state.processor_ports.find(node->node_id);
        if (processor_port_it == state.processor_ports.end())
            continue;

        auto processor_port = processor_port_it->value;
        state.global_scope->set_pending_processor_port(*processor_port);

        auto node_options_object = JS::Object::create(*state.realm, state.realm->intrinsics().object_prototype());
        MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "numberOfInputs"_utf16_fly_string }, JS::Value(node->number_of_inputs)));
        MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "numberOfOutputs"_utf16_fly_string }, JS::Value(node->number_of_outputs)));

        if (node->output_channel_count.has_value()) {
            auto output_channel_count_array_or_error = JS::Array::create(*state.realm, node->output_channel_count->size());
            if (output_channel_count_array_or_error.is_error()) {
                state.global_scope->take_pending_processor_port();
                continue;
            }
            auto output_channel_count_array = output_channel_count_array_or_error.release_value();
            for (size_t i = 0; i < node->output_channel_count->size(); ++i)
                MUST(output_channel_count_array->create_data_property_or_throw(JS::PropertyKey { static_cast<u32>(i) }, JS::Value((*node->output_channel_count)[i])));
            MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "outputChannelCount"_utf16_fly_string }, output_channel_count_array));
        }

        if (node->parameter_data.has_value()) {
            auto parameter_data_object = JS::Object::create(*state.realm, state.realm->intrinsics().object_prototype());
            for (auto const& entry : node->parameter_data.value())
                MUST(parameter_data_object->create_data_property_or_throw(JS::PropertyKey { Utf16String::from_utf8(entry.name) }, JS::Value(entry.value)));
            MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "parameterData"_utf16_fly_string }, parameter_data_object));
        }

        if (node->serialized_processor_options.has_value()) {
            auto processor_options_value_or_error = HTML::structured_deserialize(*state.vm, node->serialized_processor_options.value(), *state.realm);
            if (!processor_options_value_or_error.is_error())
                MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "processorOptions"_utf16_fly_string }, processor_options_value_or_error.release_value()));
        }

        JS::ExecutionContext* ctor_execution_context = nullptr;
        ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(ctor_execution_context, 0, 0, 0);
        ctor_execution_context->realm = state.realm;
        state.vm->push_execution_context(*ctor_execution_context);
        auto instance_or_error = JS::construct(*state.vm, processor_ctor.as_function(), JS::Value(node_options_object));
        state.vm->pop_execution_context();

        state.global_scope->take_pending_processor_port();

        if (instance_or_error.is_error())
            continue;

        auto instance = instance_or_error.release_value();
        state.instances.set(node->node_id, GC::make_root(instance));
        state.instance_processor_names.set(node->node_id, node->processor_name);
        bool has_process = processor_instance_defines_process(*instance, processor_ctor.as_function(), *state.vm);
        state.instance_has_process.set(node->node_id, has_process);
        if (should_log_all())
            dbgln("[WebAudio] AudioWorklet: node {} has_process={} (preconstructed)", node->node_id.value(), has_process);
    }
}

void RealtimeAudioWorkletProcessorHost::pump_event_loops(RenderThreadState& state)
{
    ASSERT_RENDER_THREAD();
    state.core_event_loop->pump(Core::EventLoop::WaitMode::PollForEvents);

    for (size_t i = 0; i < 16; ++i) {
        if (state.html_event_loop->task_queue().has_runnable_tasks()) {
            state.html_event_loop->process();
            continue;
        }

        if (!state.html_event_loop->microtask_queue().is_empty()) {
            state.html_event_loop->perform_a_microtask_checkpoint();
            continue;
        }

        break;
    }

    auto* agent = dynamic_cast<HTML::Agent*>(state.vm->agent());
    VERIFY(agent);
    agent->event_loop->process();
}

u64 RealtimeAudioWorkletProcessorHost::stabilize_registration_generation(RenderThreadState& state)
{
    ASSERT_RENDER_THREAD();
    u64 last_generation = m_processor_registration_generation.load(AK::MemoryOrder::memory_order_relaxed);
    for (size_t i = 0; i < 4; ++i) {
        pump_event_loops(state);
        u64 next_generation = m_processor_registration_generation.load(AK::MemoryOrder::memory_order_relaxed);
        if (next_generation == last_generation)
            return next_generation;
        last_generation = next_generation;
    }
    return last_generation;
}

void RealtimeAudioWorkletProcessorHost::process_pending_updates(RenderThreadState& state)
{
    ASSERT_RENDER_THREAD();
    Vector<WorkletModule> new_modules;
    Vector<WorkletNodeDefinition> new_node_definitions;
    Vector<WorkletPortBinding> new_port_bindings;
    consume_pending_updates(new_modules, new_node_definitions, new_port_bindings);

    if (new_modules.is_empty() && new_node_definitions.is_empty() && new_port_bindings.is_empty())
        return;

    for (auto& binding : new_port_bindings) {
        if (binding.processor_port_fd < 0)
            continue;
        if (binding.node_id != NodeID { 0 }) {
            if (state.ports_with_transport.contains(binding.node_id)) {
                state.ports_with_transport.remove(binding.node_id);
                auto port = HTML::MessagePort::create(*state.realm);
                port->set_task_source(HTML::Task::Source::AudioWorklet);
                state.processor_ports.set(binding.node_id, GC::make_root(port));
                state.instances.remove(binding.node_id);
                state.instance_processor_names.remove(binding.node_id);
                state.instance_has_process.remove(binding.node_id);
            }
        } else {
            if (state.ports_with_transport.contains(binding.node_id))
                state.ports_with_transport.remove(binding.node_id);
        }
        auto it = m_processor_port_fds.find(binding.node_id);
        if (it != m_processor_port_fds.end() && it->value >= 0)
            (void)Core::System::close(it->value);
        m_processor_port_fds.set(binding.node_id, binding.processor_port_fd);
        binding.processor_port_fd = -1;
    }

    Vector<WorkletModule> modules_to_eval;
    for (auto& module : new_modules) {
        bool already_have = false;
        ByteString matched_url;
        for (auto const& existing : m_modules) {
            if (existing.url == module.url) {
                already_have = true;
                break;
            }
            if (!already_have && existing.source_text == module.source_text) {
                already_have = true;
                matched_url = existing.url;
            }
        }
        if (already_have) {
            Vector<String> failed_registrations;
            if (!matched_url.is_empty()) {
                if (auto it = m_failed_processor_registrations_by_url.find(matched_url); it != m_failed_processor_registrations_by_url.end())
                    failed_registrations = it->value;
            } else if (auto it = m_failed_processor_registrations_by_url.find(module.url); it != m_failed_processor_registrations_by_url.end()) {
                failed_registrations = it->value;
            }
            notify_module_evaluated(module.module_id, m_processor_registration_generation.load(AK::MemoryOrder::memory_order_relaxed), true, {}, {}, move(failed_registrations));
            continue;
        }
        modules_to_eval.append(module);
        m_modules.append(move(module));
    }

    evaluate_modules(state, modules_to_eval);

    for (auto const& def : new_node_definitions)
        ensure_node_exists(state, def);

    try_attach_port_transport(state, NodeID { 0 });
    for (auto const& it : m_processor_port_fds) {
        if (it.key == NodeID { 0 })
            continue;
        try_attach_port_transport(state, it.key);
    }

    ensure_ready_processor_instances(state);
}

JS::Object* RealtimeAudioWorkletProcessorHost::ensure_processor_instance(RenderThreadState& state, SharedNode& shared)
{
    ASSERT_RENDER_THREAD();
    auto it = state.instances.find(shared.node_id);
    if (it != state.instances.end()) {
        auto name_it = state.instance_processor_names.find(shared.node_id);
        if (name_it != state.instance_processor_names.end() && name_it->value != shared.processor_name) {
            state.instances.remove(it);
            state.instance_processor_names.remove(name_it);
            state.instance_has_process.remove(shared.node_id);
        } else {
            if (!state.instance_has_process.contains(shared.node_id)) {
                auto processor_ctor = state.global_scope->processor_constructor(String::from_byte_string(shared.processor_name).release_value_but_fixme_should_propagate_errors());
                if (processor_ctor.is_function()) {
                    bool has_process = processor_instance_defines_process(*it->value, processor_ctor.as_function(), *state.vm);
                    state.instance_has_process.set(shared.node_id, has_process);
                    if (should_log_all())
                        dbgln("[WebAudio] AudioWorklet: node {} has_process={} (late)", shared.node_id.value(), has_process);
                }
            }
        }
        return it->value.ptr();
    }

    auto port_it = state.processor_ports.find(shared.node_id);
    if (port_it == state.processor_ports.end()) {
        auto processor_port = HTML::MessagePort::create(*state.realm);
        processor_port->set_task_source(HTML::Task::Source::AudioWorklet);
        processor_port->enable();
        state.processor_ports.set(shared.node_id, GC::make_root(processor_port));
        port_it = state.processor_ports.find(shared.node_id);
    }

    try_attach_port_transport(state, shared.node_id);

    auto processor_port = port_it->value;
    processor_port->enable();
    state.global_scope->set_pending_processor_port(*processor_port);

    auto processor_ctor = state.global_scope->processor_constructor(String::from_byte_string(shared.processor_name).release_value_but_fixme_should_propagate_errors());
    if (!processor_ctor.is_function()) {
        if (should_log_all())
            dbgln("[WebAudio] AudioWorklet: missing processor constructor for node {}", shared.node_id.value());
        state.global_scope->take_pending_processor_port();
        return nullptr;
    }

    auto node_options_object = JS::Object::create(*state.realm, state.realm->intrinsics().object_prototype());
    MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "numberOfInputs"_utf16_fly_string }, JS::Value(shared.number_of_inputs)));
    MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "numberOfOutputs"_utf16_fly_string }, JS::Value(shared.number_of_outputs)));

    if (shared.output_channel_count.has_value()) {
        auto output_channel_count_array_or_error = JS::Array::create(*state.realm, shared.output_channel_count->size());
        if (output_channel_count_array_or_error.is_error()) {
            state.global_scope->take_pending_processor_port();
            return nullptr;
        }
        auto output_channel_count_array = output_channel_count_array_or_error.release_value();
        for (size_t i = 0; i < shared.output_channel_count->size(); ++i)
            MUST(output_channel_count_array->create_data_property_or_throw(JS::PropertyKey { static_cast<u32>(i) }, JS::Value((*shared.output_channel_count)[i])));
        MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "outputChannelCount"_utf16_fly_string }, output_channel_count_array));
    }

    if (shared.parameter_data.has_value()) {
        auto parameter_data_object = JS::Object::create(*state.realm, state.realm->intrinsics().object_prototype());
        for (auto const& entry : shared.parameter_data.value())
            MUST(parameter_data_object->create_data_property_or_throw(JS::PropertyKey { Utf16String::from_utf8(entry.name) }, JS::Value(entry.value)));
        MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "parameterData"_utf16_fly_string }, parameter_data_object));
    }

    if (shared.serialized_processor_options.has_value()) {
        auto processor_options_value_or_error = HTML::structured_deserialize(*state.vm, shared.serialized_processor_options.value(), *state.realm);
        if (!processor_options_value_or_error.is_error())
            MUST(node_options_object->create_data_property_or_throw(JS::PropertyKey { "processorOptions"_utf16_fly_string }, processor_options_value_or_error.release_value()));
    }

    JS::ExecutionContext* ctor_execution_context = nullptr;
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(ctor_execution_context, 0, 0, 0);
    ctor_execution_context->realm = state.realm;
    state.vm->push_execution_context(*ctor_execution_context);
    auto instance_or_error = JS::construct(*state.vm, processor_ctor.as_function(), JS::Value(node_options_object));
    state.vm->pop_execution_context();

    state.global_scope->take_pending_processor_port();

    if (instance_or_error.is_error()) {
        if (should_log_all())
            dbgln("[WebAudio] AudioWorklet: failed to construct processor for node {}", shared.node_id.value());
        return nullptr;
    }

    auto instance = instance_or_error.release_value();
    state.instances.set(shared.node_id, GC::make_root(instance));
    state.instance_processor_names.set(shared.node_id, shared.processor_name);
    bool has_process = processor_instance_defines_process(*instance, processor_ctor.as_function(), *state.vm);
    state.instance_has_process.set(shared.node_id, has_process);
    if (should_log_all())
        dbgln("[WebAudio] AudioWorklet: node {} has_process={}", shared.node_id.value(), has_process);
    auto instance_it = state.instances.find(shared.node_id);
    if (instance_it == state.instances.end())
        return nullptr;
    return instance_it->value.ptr();
}

}
