/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/RootVector.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(to_underlying(EngineErrorKind::Error) == JS_ERROR_KIND_ERROR);
static_assert(to_underlying(EngineErrorKind::EvalError) == JS_ERROR_KIND_EVAL_ERROR);
static_assert(to_underlying(EngineErrorKind::InternalError) == JS_ERROR_KIND_INTERNAL_ERROR);
static_assert(to_underlying(EngineErrorKind::RangeError) == JS_ERROR_KIND_RANGE_ERROR);
static_assert(to_underlying(EngineErrorKind::ReferenceError) == JS_ERROR_KIND_REFERENCE_ERROR);
static_assert(to_underlying(EngineErrorKind::SyntaxError) == JS_ERROR_KIND_SYNTAX_ERROR);
static_assert(to_underlying(EngineErrorKind::TypeError) == JS_ERROR_KIND_TYPE_ERROR);
static_assert(to_underlying(EngineErrorKind::URIError) == JS_ERROR_KIND_URI_ERROR);
static_assert(to_underlying(EngineErrorKind::AggregateError) == JS_ERROR_KIND_AGGREGATE_ERROR);
static_assert(to_underlying(EngineErrorKind::SuppressedError) == JS_ERROR_KIND_SUPPRESSED_ERROR);

static_assert(to_underlying(HandledByHost::Handled) == JS_HANDLED_BY_HOST_HANDLED);
static_assert(to_underlying(HandledByHost::Unhandled) == JS_HANDLED_BY_HOST_UNHANDLED);

static_assert(to_underlying(CompilationType::DirectEval) == JS_COMPILATION_TYPE_DIRECT_EVAL);
static_assert(to_underlying(CompilationType::IndirectEval) == JS_COMPILATION_TYPE_INDIRECT_EVAL);
static_assert(to_underlying(CompilationType::Function) == JS_COMPILATION_TYPE_FUNCTION);
static_assert(to_underlying(CompilationType::Timer) == JS_COMPILATION_TYPE_TIMER);

static_assert(to_underlying(Promise::RejectionOperation::Reject) == JS_PROMISE_REJECTION_OPERATION_REJECT);
static_assert(to_underlying(Promise::RejectionOperation::Handle) == JS_PROMISE_REJECTION_OPERATION_HANDLE);

static_assert(sizeof(Value) == sizeof(JSValue));
static_assert(sizeof(JSScriptOrModule) == sizeof(ScriptOrModule));

VM* VM::s_the = nullptr;
static size_t s_vm_count = 0;

static JSVM* vm_abi(VM const& vm)
{
    return vm_to_abi(const_cast<VM&>(vm));
}

static JSExecutionContext* execution_context_to_abi(ExecutionContext& execution_context)
{
    return reinterpret_cast<JSExecutionContext*>(&execution_context);
}

static ExecutionContext* execution_context_from_abi(JSExecutionContext* execution_context)
{
    return reinterpret_cast<ExecutionContext*>(execution_context);
}

static JSRealm* realm_to_abi(Realm& realm)
{
    return cell_to_abi<JSRealm>(realm);
}

static Realm* realm_from_abi(JSRealm* realm)
{
    return cell_from_abi<Realm>(realm);
}

static ReadonlySpan<Value> values_from_abi(JSValue const* values, size_t count)
{
    if (count == 0)
        return {};
    return { reinterpret_cast<Value const*>(values), count };
}

static ScriptOrModule script_or_module_from_abi(JSScriptOrModule script_or_module)
{
    switch (script_or_module.tag) {
    case JS_LAYOUT_SCRIPT_OR_MODULE_TAG_SCRIPT:
        return GC::Ref { declared_cell_from_abi<Script>(script_or_module.cell) };
    case JS_LAYOUT_SCRIPT_OR_MODULE_TAG_MODULE:
        return GC::Ref { declared_cell_from_abi<Module>(script_or_module.cell) };
    default:
        VERIFY(script_or_module.tag == JS_LAYOUT_SCRIPT_OR_MODULE_TAG_EMPTY);
        return {};
    }
}

static ImportedModuleReferrer imported_module_referrer_from_abi(JSImportedModuleReferrer referrer)
{
    switch (referrer.kind) {
    case JS_IMPORTED_MODULE_REFERRER_SCRIPT:
        return GC::Ref { declared_cell_from_abi<Script>(referrer.record) };
    case JS_IMPORTED_MODULE_REFERRER_CYCLIC_MODULE:
        return GC::Ref { declared_cell_from_abi<CyclicModule>(referrer.record) };
    case JS_IMPORTED_MODULE_REFERRER_REALM:
        return GC::Ref { declared_cell_from_abi<Realm>(referrer.record) };
    default:
        VERIFY_NOT_REACHED();
    }
}

static JSImportedModuleReferrer imported_module_referrer_to_abi(ImportedModuleReferrer const& referrer)
{
    return referrer.visit(
        [](GC::Ref<Script> script) { return JSImportedModuleReferrer { JS_IMPORTED_MODULE_REFERRER_SCRIPT, script.ptr() }; },
        [](GC::Ref<CyclicModule> module) { return JSImportedModuleReferrer { JS_IMPORTED_MODULE_REFERRER_CYCLIC_MODULE, module.ptr() }; },
        [](GC::Ref<Realm> realm) { return JSImportedModuleReferrer { JS_IMPORTED_MODULE_REFERRER_REALM, realm.ptr() }; });
}

static ImportedModulePayload imported_module_payload_from_abi(JSImportedModulePayload payload)
{
    switch (payload.kind) {
    case JS_IMPORTED_MODULE_PAYLOAD_GRAPH_LOADING_STATE:
        return GC::Ref { declared_cell_from_abi<GraphLoadingState>(payload.record) };
    case JS_IMPORTED_MODULE_PAYLOAD_PROMISE_CAPABILITY:
        return GC::Ref { declared_cell_from_abi<PromiseCapability>(payload.record) };
    default:
        VERIFY_NOT_REACHED();
    }
}

static JSImportedModulePayload imported_module_payload_to_abi(ImportedModulePayload const& payload)
{
    return payload.visit(
        [](GC::Ref<GraphLoadingState> state) { return JSImportedModulePayload { JS_IMPORTED_MODULE_PAYLOAD_GRAPH_LOADING_STATE, state.ptr() }; },
        [](GC::Ref<PromiseCapability> capability) { return JSImportedModulePayload { JS_IMPORTED_MODULE_PAYLOAD_PROMISE_CAPABILITY, capability.ptr() }; });
}

static ModuleRequest module_request_from_abi(JSModuleRequest const& module_request)
{
    auto attribute_count = js_module_request_attribute_count(&module_request);
    Vector<ImportAttribute> attributes;
    attributes.ensure_capacity(attribute_count);
    for (size_t index = 0; index < attribute_count; ++index) {
        auto attribute = js_module_request_attribute(&module_request, index);
        attributes.unchecked_append({ Utf16String::from_utf16(utf16_view_from_abi(attribute.key)), Utf16String::from_utf16(utf16_view_from_abi(attribute.value)) });
    }
    return ModuleRequest { Utf16FlyString::from_utf16(utf16_view_from_abi(js_module_request_specifier(&module_request))), move(attributes) };
}

// The caller destroys the request with js_module_request_destroy().
static JSModuleRequest* module_request_to_abi(ModuleRequest const& module_request)
{
    Vector<JSImportAttribute> attributes;
    attributes.ensure_capacity(module_request.attributes.size());
    for (auto const& attribute : module_request.attributes)
        attributes.unchecked_append({ utf16_view_to_abi(attribute.key.utf16_view()), utf16_view_to_abi(attribute.value.utf16_view()) });
    return js_module_request_create(utf16_view_to_abi(module_request.module_specifier.view()), attributes.data(), attributes.size());
}

static void append_utf16_string_to_sink(JSStringSink const& sink, Utf16View string)
{
    if (!string.has_ascii_storage()) {
        sink.append(sink.context, reinterpret_cast<u16 const*>(string.utf16_span().data()), string.length_in_code_units());
        return;
    }
    Vector<u16> code_units;
    code_units.ensure_capacity(string.length_in_code_units());
    for (size_t index = 0; index < string.length_in_code_units(); ++index)
        code_units.unchecked_append(string.code_unit_at(index));
    sink.append(sink.context, code_units.data(), code_units.size());
}

// The runtime calls the VM's host hooks and its agent through these, each with the facade's VM or agent as its data.
struct HostHookThunks {
    static VM& vm_of(void* data)
    {
        return *static_cast<VM*>(data);
    }

    static JSCompletion ensure_can_add_private_element(void* data, JSVM*, JSObject* object)
    {
        return completion_to_abi(vm_of(data).host_ensure_can_add_private_element(object_from_abi(object)));
    }

    static JSCompletion ensure_can_compile_strings(void* data, JSVM*, JSEnsureCanCompileStringsArguments const* arguments)
    {
        Vector<Utf16String> parameter_strings;
        parameter_strings.ensure_capacity(arguments->parameter_string_count);
        for (size_t index = 0; index < arguments->parameter_string_count; ++index)
            parameter_strings.unchecked_append(Utf16String::from_utf16(utf16_view_from_abi(arguments->parameter_strings[index])));

        return completion_to_abi(vm_of(data).host_ensure_can_compile_strings(
            *realm_from_abi(arguments->callee_realm),
            parameter_strings,
            utf16_view_from_abi(arguments->body_string),
            utf16_view_from_abi(arguments->code_string),
            static_cast<CompilationType>(arguments->compilation_type),
            values_from_abi(arguments->parameter_args, arguments->parameter_arg_count),
            value_from_abi(arguments->body_arg)));
    }

    static bool get_code_for_eval(void* data, JSVM*, JSObject* argument, JSValue* code)
    {
        auto code_string = vm_of(data).host_get_code_for_eval(object_from_abi(argument));
        if (!code_string)
            return false;
        *code = value_to_abi(Value { code_string.ptr() });
        return true;
    }

    static void promise_rejection_tracker(void* data, JSVM*, JSObject* promise, u8 operation)
    {
        vm_of(data).host_promise_rejection_tracker(*cell_from_abi<Promise>(promise), static_cast<Promise::RejectionOperation>(operation));
    }

    static JSCompletion call_job_callback(void* data, JSVM*, JSJobCallback* job_callback, JSValue this_value, JSValue const* arguments, size_t argument_count)
    {
        return completion_to_abi(vm_of(data).host_call_job_callback(*cell_from_abi<JobCallback>(job_callback), value_from_abi(this_value), values_from_abi(arguments, argument_count)));
    }

    static void enqueue_finalization_registry_cleanup_job(void* data, JSVM*, JSObject* finalization_registry)
    {
        vm_of(data).host_enqueue_finalization_registry_cleanup_job(declared_cell_from_abi<FinalizationRegistry>(finalization_registry));
    }

    static void enqueue_promise_job(void* data, JSVM*, JSPromiseJob* job, JSRealm* realm)
    {
        VERIFY(job);
        vm_of(data).host_enqueue_promise_job(PromiseJob { *reinterpret_cast<GC::Cell*>(job) }, realm ? realm_from_abi(realm) : nullptr);
    }

    static bool promise_job_queue_is_empty(void* data, JSVM*)
    {
        return vm_of(data).host_promise_job_queue_is_empty();
    }

    static JSJobCallback* make_job_callback(void* data, JSVM*, JSObject* callable)
    {
        return cell_to_abi<JSJobCallback>(*vm_of(data).host_make_job_callback(*cell_from_abi<FunctionObject>(callable)));
    }

    static void get_import_meta_properties(void* data, JSVM*, JSModule* module, JSImportMetaPropertySink* properties)
    {
        auto& vm = vm_of(data);
        auto import_meta_properties = vm.host_get_import_meta_properties(declared_cell_from_abi<SourceTextModule>(module));

        // The values have to stay alive until they are appended, and appending one can collect garbage.
        GC::RootVector<Value> values_being_appended;
        for (auto const& property : import_meta_properties)
            values_being_appended.append(property.value);

        for (auto const& property : import_meta_properties)
            properties->append(properties->context, *property_key_to_abi(property.key), value_to_abi(property.value));
    }

    static void get_supported_import_attributes(void* data, JSVM*, JSStringSink* attribute_keys)
    {
        for (auto const& key : vm_of(data).host_get_supported_import_attributes())
            append_utf16_string_to_sink(*attribute_keys, key.utf16_view());
    }

    static void load_imported_module(void* data, JSVM*, JSImportedModuleReferrer referrer, JSModuleRequest const* module_request, void* host_defined, JSImportedModulePayload payload)
    {
        vm_of(data).host_load_imported_module(
            imported_module_referrer_from_abi(referrer),
            module_request_from_abi(*module_request),
            static_cast<GC::Cell*>(host_defined),
            imported_module_payload_from_abi(payload));
    }

    static void unrecognized_date_string(void* data, JSVM*, JSUtf16View date_string)
    {
        vm_of(data).host_unrecognized_date_string(utf16_view_from_abi(date_string));
    }

    static JSCompletion resize_array_buffer(void* data, JSVM*, JSObject* buffer, size_t new_byte_length)
    {
        return completion_to_abi(vm_of(data).host_resize_array_buffer(declared_cell_from_abi<ArrayBuffer>(buffer), new_byte_length));
    }

    static JSCompletion grow_shared_array_buffer(void* data, JSVM*, JSObject* buffer, size_t new_byte_length)
    {
        return completion_to_abi(vm_of(data).host_grow_shared_array_buffer(declared_cell_from_abi<ArrayBuffer>(buffer), new_byte_length));
    }

    static void on_unimplemented_property_access(void* data, JSVM*, JSObject* object, JSPropertyKey key)
    {
        auto& vm = vm_of(data);
        if (vm.on_unimplemented_property_access)
            vm.on_unimplemented_property_access(object_from_abi(object), lent_property_key_from_abi(key));
    }

    static void spin_event_loop_until(void* data, JSVM*, JSGoalCondition goal_condition, void* goal_context)
    {
        auto& agent = *static_cast<Agent*>(data);
        agent.spin_event_loop_until(GC::create_function(VM::the().heap(), [goal_condition, goal_context] {
            return goal_condition(goal_context);
        }));
    }
};

// The hooks that VM has no member for keep the runtime's own behavior.
static constexpr JSVmHostHooks s_host_hook_thunks {
    .ensure_can_add_private_element = HostHookThunks::ensure_can_add_private_element,
    .ensure_can_compile_strings = HostHookThunks::ensure_can_compile_strings,
    .get_code_for_eval = HostHookThunks::get_code_for_eval,
    .promise_rejection_tracker = HostHookThunks::promise_rejection_tracker,
    .call_job_callback = HostHookThunks::call_job_callback,
    .enqueue_finalization_registry_cleanup_job = HostHookThunks::enqueue_finalization_registry_cleanup_job,
    .enqueue_promise_job = HostHookThunks::enqueue_promise_job,
    .promise_job_queue_is_empty = HostHookThunks::promise_job_queue_is_empty,
    .make_job_callback = HostHookThunks::make_job_callback,
    .get_import_meta_properties = HostHookThunks::get_import_meta_properties,
    .finalize_import_meta = nullptr,
    .get_supported_import_attributes = HostHookThunks::get_supported_import_attributes,
    .load_imported_module = HostHookThunks::load_imported_module,
    .unrecognized_date_string = HostHookThunks::unrecognized_date_string,
    .resize_array_buffer = HostHookThunks::resize_array_buffer,
    .grow_shared_array_buffer = HostHookThunks::grow_shared_array_buffer,
    .system_utc_epoch_nanoseconds = nullptr,
    .on_unimplemented_property_access = HostHookThunks::on_unimplemented_property_access,
};

NonnullRefPtr<VM> VM::create()
{
    // NOTE: We only allow a single VM instance per process.
    //       However, test runners need to create and destroy VMs repeatedly,
    //       so we allow recreating the VM as long as the previous one was destroyed.
    VERIFY(s_vm_count == 0);
    ++s_vm_count;

    ErrorMessages error_messages {};
    error_messages[to_underlying(ErrorMessage::OutOfMemory)] = Utf16String::from_utf16(ErrorType::OutOfMemory.message());

    return adopt_ref(*new VM(move(error_messages)));
}

VM::VM(ErrorMessages error_messages)
    : m_error_messages(move(error_messages))
{
    VERIFY(static_cast<void*>(m_engine_storage) == static_cast<void*>(this));

    // As with the C++ runtime's VM, the embedder's C++ cells live in the VM's heap, and a SharedArrayBuffer of a fixed
    // length lives in memory that other processes can map.
    JSVmOptions options {
        .become_process_default_heap = true,
        .shared_memory_shared_array_buffers = true,
    };
    VERIFY(js_vm_construct_at(m_engine_storage, sizeof(m_engine_storage), alignof(VM), &options));

    m_heap = reinterpret_cast<GC::Heap*>(js_vm_heap(vm_abi(*this)));
    s_the = this;

    install_default_host_hooks();
    js_vm_set_embedder(vm_abi(*this), &s_host_hook_thunks, this);
}

VM::~VM()
{
    // The hooks and the agent may hold roots, which have to go before the heap does.
    js_vm_set_agent(vm_abi(*this), nullptr);
    js_vm_set_embedder(vm_abi(*this), nullptr, nullptr);
    m_agent = nullptr;
    clear_host_hooks();
    on_unimplemented_property_access = nullptr;

    js_vm_destroy_at(vm_abi(*this));

    if (s_the == this)
        s_the = nullptr;
    --s_vm_count;
    VERIFY(s_vm_count == 0);
}

void VM::unref() const
{
    VERIFY(m_reference_count > 0);
    if (--m_reference_count == 0)
        delete const_cast<VM*>(this);
}

void VM::install_default_host_hooks()
{
    host_load_imported_module = [this](ImportedModuleReferrer referrer, ModuleRequest const& module_request, GC::Ptr<GC::Cell> load_state, ImportedModulePayload payload) {
        auto* abi_module_request = module_request_to_abi(module_request);
        ScopeGuard destroy_abi_module_request = [&] { js_module_request_destroy(abi_module_request); };
        js_vm_default_host_load_imported_module(vm_abi(*this), imported_module_referrer_to_abi(referrer), abi_module_request, load_state.ptr(), imported_module_payload_to_abi(payload));
    };

    host_get_import_meta_properties = [](SourceTextModule&) -> HashMap<PropertyKey, Value> {
        return {};
    };

    host_get_supported_import_attributes = [] {
        return Vector<Utf16String> { "type"_utf16 };
    };

    // The runtime tracks no rejections for an embedder by itself.
    host_promise_rejection_tracker = [](Promise&, Promise::RejectionOperation) {
    };

    host_call_job_callback = [this](JobCallback& job_callback, Value this_value, ReadonlySpan<Value> arguments) -> ThrowCompletionOr<Value> {
        return completion_from_abi<Value>(js_vm_default_host_call_job_callback(vm_abi(*this), cell_to_abi<JSJobCallback>(job_callback), value_to_abi(this_value), reinterpret_cast<JSValue const*>(arguments.data()), arguments.size()));
    };

    host_enqueue_finalization_registry_cleanup_job = [this](FinalizationRegistry& finalization_registry) {
        js_vm_default_host_enqueue_finalization_registry_cleanup_job(vm_abi(*this), declared_cell_to_abi<JSObject>(finalization_registry));
    };

    host_enqueue_promise_job = [this](PromiseJob job, GC::Ptr<Realm> realm) {
        js_vm_default_host_enqueue_promise_job(vm_abi(*this), reinterpret_cast<JSPromiseJob*>(job.m_job.ptr()), realm ? realm_to_abi(*realm) : nullptr);
    };

    host_promise_job_queue_is_empty = [this] {
        return js_vm_default_host_promise_job_queue_is_empty(vm_abi(*this));
    };

    host_make_job_callback = [this](FunctionObject& function_object) {
        return JobCallback::create(*this, function_object, nullptr);
    };

    host_get_code_for_eval = [](Object const&) -> GC::Ptr<PrimitiveString> {
        return {};
    };

    host_ensure_can_compile_strings = [](Realm&, ReadonlySpan<Utf16String>, Utf16View, Utf16View, CompilationType, ReadonlySpan<Value>, Value) -> ThrowCompletionOr<void> {
        return {};
    };

    host_ensure_can_add_private_element = [](Object&) -> ThrowCompletionOr<void> {
        return {};
    };

    host_resize_array_buffer = [this](ArrayBuffer& buffer, size_t new_byte_length) -> ThrowCompletionOr<HandledByHost> {
        return completion_from_abi<HandledByHost>(js_vm_default_host_resize_array_buffer(vm_abi(*this), declared_cell_to_abi<JSObject>(buffer), new_byte_length));
    };

    host_grow_shared_array_buffer = [this](ArrayBuffer& buffer, size_t new_byte_length) -> ThrowCompletionOr<HandledByHost> {
        return completion_from_abi<HandledByHost>(js_vm_default_host_grow_shared_array_buffer(vm_abi(*this), declared_cell_to_abi<JSObject>(buffer), new_byte_length));
    };

    host_unrecognized_date_string = [](Utf16View) {
    };
}

void VM::clear_host_hooks()
{
    host_load_imported_module = nullptr;
    host_get_import_meta_properties = nullptr;
    host_get_supported_import_attributes = nullptr;
    host_promise_rejection_tracker = nullptr;
    host_call_job_callback = nullptr;
    host_enqueue_finalization_registry_cleanup_job = nullptr;
    host_enqueue_promise_job = nullptr;
    host_make_job_callback = nullptr;
    host_get_code_for_eval = nullptr;
    host_ensure_can_compile_strings = nullptr;
    host_ensure_can_add_private_element = nullptr;
    host_resize_array_buffer = nullptr;
    host_grow_shared_array_buffer = nullptr;
    host_unrecognized_date_string = nullptr;
    host_promise_job_queue_is_empty = nullptr;
}

ThrowCompletionOr<Value> VM::run(Script& script, GC::Ptr<Environment> lexical_environment_override)
{
    auto* abi_lexical_environment_override = lexical_environment_override ? declared_cell_to_abi<JSEnvironment>(*lexical_environment_override) : nullptr;
    return completion_from_abi<Value>(js_script_run(vm_abi(*this), cell_to_abi<JSScript>(script), abi_lexical_environment_override));
}

void VM::enable_debugging()
{
    js_debugger_enable(vm_abi(*this));
}

void VM::disable_debugging()
{
    js_debugger_disable(vm_abi(*this));
}

bool VM::debugging_enabled() const
{
    return js_debugger_is_enabled(vm_abi(*this));
}

GC::Ref<Symbol> VM::well_known_symbol(WellKnownSymbol symbol) const
{
    static_assert(to_underlying(WellKnownSymbol::async_dispose) == JS_WELL_KNOWN_SYMBOL_ASYNC_DISPOSE);
    static_assert(to_underlying(WellKnownSymbol::iterator) == JS_WELL_KNOWN_SYMBOL_ITERATOR);
    static_assert(to_underlying(WellKnownSymbol::to_string_tag) == JS_WELL_KNOWN_SYMBOL_TO_STRING_TAG);
    static_assert(to_underlying(WellKnownSymbol::unscopables) == JS_WELL_KNOWN_SYMBOL_UNSCOPABLES);
    auto* well_known_symbol = js_symbol_well_known(vm_abi(*this), to_underlying(symbol));
    VERIFY(well_known_symbol);
    return *cell_from_abi<Symbol>(well_known_symbol);
}

PrimitiveString& VM::empty_string()
{
    // The runtime hands out the one empty string it keeps.
    return *cell_from_abi<PrimitiveString>(js_string_create_from_utf16_view(vm_abi(*this), utf16_view_to_abi(Utf16View {})));
}

Utf16String const& VM::error_message(ErrorMessage type) const
{
    VERIFY(type < ErrorMessage::__Count);

    auto const& message = m_error_messages[to_underlying(type)];
    VERIFY(!message.is_empty());

    return message;
}

bool VM::did_reach_stack_space_limit() const
{
    return js_vm_did_reach_stack_space_limit(vm_abi(*this));
}

void VM::push_execution_context(ExecutionContext& execution_context)
{
    js_execution_context_push(vm_abi(*this), execution_context_to_abi(execution_context));
}

ExecutionContext* VM::pop_execution_context()
{
    VERIFY(!execution_context_stack().is_empty());
    return execution_context_from_abi(js_execution_context_pop(vm_abi(*this)));
}

size_t VM::ExecutionContextStack::size() const
{
    return js_execution_context_stack_size(vm_abi(m_vm));
}

ExecutionContext* VM::find_execution_context_from_the_top(ExecutionContextPredicate predicate, void* predicate_context) const
{
    struct PredicateWithContext {
        ExecutionContextPredicate predicate;
        void* context;
    };
    PredicateWithContext predicate_with_context { predicate, predicate_context };

    auto* matching_execution_context = js_execution_context_last_matching(
        vm_abi(*this),
        [](void* context, JSExecutionContext* execution_context) {
            auto const& predicate_with_context = *static_cast<PredicateWithContext const*>(context);
            return predicate_with_context.predicate(predicate_with_context.context, *execution_context_from_abi(execution_context));
        },
        &predicate_with_context);
    return execution_context_from_abi(matching_execution_context);
}

void VM::finish_execution_generation()
{
    js_vm_finish_execution_generation(vm_abi(*this));
}

VM::TypeErrorRealmScope::TypeErrorRealmScope(VM& vm, Realm& realm)
    : m_vm(vm)
{
    auto replaced_scope = js_error_type_error_realm_scope_enter(vm_abi(vm), realm_to_abi(realm));
    m_previous_realm = replaced_scope.previous_realm ? realm_from_abi(replaced_scope.previous_realm) : nullptr;
    m_previous_depth = replaced_scope.previous_depth;
}

void VM::TypeErrorRealmScope::restore()
{
    if (!m_active)
        return;
    js_error_type_error_realm_scope_exit(vm_abi(m_vm), { m_previous_realm ? realm_to_abi(*m_previous_realm) : nullptr, m_previous_depth });
    m_active = false;
}

Completion VM::throw_engine_error(EngineErrorKind kind, Utf16View message)
{
    return throw_completion_from_abi(js_error_throw(vm_abi(*this), to_underlying(kind), utf16_view_to_abi(message)));
}

Completion VM::throw_engine_error(EngineErrorKind kind, Utf16String message)
{
    return throw_completion_from_abi(js_error_throw_with_owned_message(vm_abi(*this), to_underlying(kind), owned_utf16_string_to_abi(move(message))));
}

void VM::set_agent(OwnPtr<Agent> agent)
{
    m_agent = move(agent);
    if (!m_agent) {
        js_vm_set_agent(vm_abi(*this), nullptr);
        return;
    }

    JSAgent abi_agent {
        .can_block = m_agent->can_block() == Agent::CanBlock::Yes,
        .spin_event_loop_until = HostHookThunks::spin_event_loop_until,
        .data = m_agent.ptr(),
    };
    js_vm_set_agent(vm_abi(*this), &abi_agent);
}

void VM::save_execution_context_stack()
{
    js_execution_context_save_stack(vm_abi(*this));
}

void VM::clear_execution_context_stack()
{
    js_execution_context_clear_stack(vm_abi(*this));
}

void VM::restore_execution_context_stack()
{
    js_execution_context_restore_stack(vm_abi(*this));
}

ScriptOrModule VM::get_active_script_or_module() const
{
    return script_or_module_from_abi(js_execution_context_get_active_script_or_module(vm_abi(*this)));
}

void* InterpreterStack::top() const
{
    static_assert(JS_LAYOUT_VM_INTERPRETER_STACK_TOP_SIZE == sizeof(void*));
    return m_vm.engine_head_field<void*>(JS_LAYOUT_VM_INTERPRETER_STACK_TOP_OFFSET);
}

ExecutionContext* InterpreterStack::allocate(u32 registers_and_locals_count, ReadonlySpan<Value> constants, u32 arguments_count)
{
    return execution_context_from_abi(js_execution_context_interpreter_stack_allocate(vm_abi(m_vm), registers_and_locals_count, constants.size(), arguments_count));
}

void InterpreterStack::deallocate(void* mark)
{
    js_execution_context_interpreter_stack_deallocate(vm_abi(m_vm), mark);
}

}
