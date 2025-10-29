/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibFileSystem/FileSystem.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BoundFunction.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FinalizationRegistry.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/SourceTextModule.h>
#include <LibJS/SyntheticModule.h>

namespace JS {

NonnullRefPtr<VM> VM::create()
{
    ErrorMessages error_messages {};
    error_messages[to_underlying(ErrorMessage::OutOfMemory)] = ErrorType::OutOfMemory.message();

    auto vm = adopt_ref(*new VM(move(error_messages)));

    WellKnownSymbols well_known_symbols {
#define __JS_ENUMERATE(SymbolName, snake_name) \
    Symbol::create(*vm, "Symbol." #SymbolName##_utf16, false),
        JS_ENUMERATE_WELL_KNOWN_SYMBOLS
#undef __JS_ENUMERATE
    };

    vm->set_well_known_symbols(move(well_known_symbols));
    return vm;
}

template<size_t... code_points>
static constexpr auto make_single_ascii_character_strings(IndexSequence<code_points...>)
{
    return AK::Array { (Utf16String::from_ascii_character(static_cast<u8>(code_points)))... };
}

static constexpr auto single_ascii_character_strings = make_single_ascii_character_strings(MakeIndexSequence<128>());

VM::VM(ErrorMessages error_messages)
    : m_heap(this, [this](HashMap<GC::Cell*, GC::HeapRoot>& roots) {
        gather_roots(roots);
    })
    , m_error_messages(move(error_messages))
{
    m_bytecode_interpreter = make<Bytecode::Interpreter>(*this);

    m_empty_string = m_heap.allocate<PrimitiveString>(String {});

    cached_strings = {
        .number = m_heap.allocate<PrimitiveString>("number"_string),
        .undefined = m_heap.allocate<PrimitiveString>("undefined"_string),
        .object = m_heap.allocate<PrimitiveString>("object"_string),
        .string = m_heap.allocate<PrimitiveString>("string"_string),
        .symbol = m_heap.allocate<PrimitiveString>("symbol"_string),
        .boolean = m_heap.allocate<PrimitiveString>("boolean"_string),
        .bigint = m_heap.allocate<PrimitiveString>("bigint"_string),
        .function = m_heap.allocate<PrimitiveString>("function"_string),
        .object_Object = m_heap.allocate<PrimitiveString>("[object Object]"_string),
    };

    for (size_t i = 0; i < single_ascii_character_strings.size(); ++i)
        m_single_ascii_character_strings[i] = m_heap.allocate<PrimitiveString>(single_ascii_character_strings[i]);

    // Default hook implementations. These can be overridden by the host, for example, LibWeb overrides the default hooks to place promise jobs on the microtask queue.
    host_promise_rejection_tracker = [this](Promise& promise, Promise::RejectionOperation operation) {
        promise_rejection_tracker(promise, operation);
    };

    host_call_job_callback = [this](JobCallback& job_callback, Value this_value, ReadonlySpan<Value> arguments) {
        return call_job_callback(*this, job_callback, this_value, arguments);
    };

    host_enqueue_finalization_registry_cleanup_job = [this](FinalizationRegistry& finalization_registry) {
        enqueue_finalization_registry_cleanup_job(finalization_registry);
    };

    host_enqueue_promise_job = [this](GC::Ref<GC::Function<ThrowCompletionOr<Value>()>> job, Realm* realm) {
        enqueue_promise_job(job, realm);
    };

    host_make_job_callback = [](FunctionObject& function_object) {
        return make_job_callback(function_object);
    };

    host_load_imported_module = [this](ImportedModuleReferrer referrer, ModuleRequest const& module_request, GC::Ptr<GraphLoadingState::HostDefined> load_state, ImportedModulePayload payload) -> void {
        return load_imported_module(referrer, module_request, load_state, move(payload));
    };

    host_get_import_meta_properties = [&](SourceTextModule const&) -> HashMap<PropertyKey, Value> {
        return {};
    };

    host_finalize_import_meta = [&](Object*, SourceTextModule const&) {
    };

    host_get_supported_import_attributes = [&] {
        return Vector<Utf16String> { "type"_utf16 };
    };

    // 1 HostGetCodeForEval ( argument ), https://tc39.es/proposal-dynamic-code-brand-checks/#sec-hostgetcodeforeval
    host_get_code_for_eval = [](Object const&) -> GC::Ptr<PrimitiveString> {
        // The host-defined abstract operation HostGetCodeForEval takes argument argument (an Object) and returns a
        // String or NO-CODE. It allows host environments to return a String of code from argument to be used by eval,
        // rather than eval returning argument.
        //
        // argument represents the Object to be checked for code.
        //
        // The default implementation of HostGetCodeForEval is to return NO-CODE.
        return {};
    };

    // 2 HostEnsureCanCompileStrings ( calleeRealm, parameterStrings, bodyString, codeString, compilationType, parameterArgs, bodyArg ), https://tc39.es/proposal-dynamic-code-brand-checks/#sec-hostensurecancompilestrings
    host_ensure_can_compile_strings = [](Realm&, ReadonlySpan<String>, StringView, StringView, CompilationType, ReadonlySpan<Value>, Value) -> ThrowCompletionOr<void> {
        // The host-defined abstract operation HostEnsureCanCompileStrings takes arguments calleeRealm (a Realm Record),
        // parameterStrings (a List of Strings), bodyString (a String), and direct (a Boolean) and returns either a normal
        // completion containing unused or a throw completion.
        //
        // It allows host environments to block certain ECMAScript functions which allow developers to compile strings into ECMAScript code.
        // An implementation of HostEnsureCanCompileStrings must conform to the following requirements:
        //   - If the returned Completion Record is a normal completion, it must be a normal completion containing unused.
        // The default implementation of HostEnsureCanCompileStrings is to return NormalCompletion(unused).
        return {};
    };

    host_ensure_can_add_private_element = [](Object&) -> ThrowCompletionOr<void> {
        // The host-defined abstract operation HostEnsureCanAddPrivateElement takes argument O (an Object)
        // and returns either a normal completion containing unused or a throw completion.
        // It allows host environments to prevent the addition of private elements to particular host-defined exotic objects.
        // An implementation of HostEnsureCanAddPrivateElement must conform to the following requirements:
        // - If O is not a host-defined exotic object, this abstract operation must return NormalCompletion(unused) and perform no other steps.
        // - Any two calls of this abstract operation with the same argument must return the same kind of Completion Record.
        // The default implementation of HostEnsureCanAddPrivateElement is to return NormalCompletion(unused).
        return {};

        // This abstract operation is only invoked by ECMAScript hosts that are web browsers.
        // NOTE: Since LibJS has no way of knowing whether the current environment is a browser we always
        //       call HostEnsureCanAddPrivateElement when needed.
    };

    // 25.1.3.8 HostResizeArrayBuffer ( buffer, newByteLength ), https://tc39.es/ecma262/#sec-hostresizearraybuffer
    host_resize_array_buffer = [this](ArrayBuffer& buffer, size_t new_byte_length) -> ThrowCompletionOr<HandledByHost> {
        // The host-defined abstract operation HostResizeArrayBuffer takes arguments buffer (an ArrayBuffer) and
        // newByteLength (a non-negative integer) and returns either a normal completion containing either handled or
        // unhandled, or a throw completion. It gives the host an opportunity to perform implementation-defined resizing
        // of buffer. If the host chooses not to handle resizing of buffer, it may return unhandled for the default behaviour.

        // The implementation of HostResizeArrayBuffer must conform to the following requirements:
        // - The abstract operation does not detach buffer.
        // - If the abstract operation completes normally with handled, buffer.[[ArrayBufferByteLength]] is newByteLength.

        // The default implementation of HostResizeArrayBuffer is to return NormalCompletion(unhandled).

        if (auto result = buffer.buffer().try_resize(new_byte_length, ByteBuffer::ZeroFillNewElements::Yes); result.is_error())
            return throw_completion<RangeError>(ErrorType::NotEnoughMemoryToAllocate, new_byte_length);

        return HandledByHost::Handled;
    };

    // 3.6.1 HostInitializeShadowRealm ( realm, context, O ), https://tc39.es/proposal-shadowrealm/#sec-hostinitializeshadowrealm
    host_initialize_shadow_realm = [](Realm&, NonnullOwnPtr<ExecutionContext>, ShadowRealm&) -> ThrowCompletionOr<void> {
        // The host-defined abstract operation HostInitializeShadowRealm takes arguments realm (a Realm Record),
        // context (an execution context), and O (a ShadowRealm object) and returns either a normal completion
        // containing unused or a throw completion. It is used to inform the host of any newly created realms
        // from the ShadowRealm constructor. The idea of this hook is to initialize host data structures related
        // to the ShadowRealm, e.g., for module loading.
        //
        // The host may use this hook to add properties to the ShadowRealm's global object. Those properties must be configurable.
        return {};
    };

    // 2.3.1 HostSystemUTCEpochNanoseconds ( global ), https://tc39.es/proposal-temporal/#sec-hostsystemutcepochnanoseconds
    host_system_utc_epoch_nanoseconds = [](Object const&) {
        // 1. Let ns be the approximate current UTC date and time, in nanoseconds since the epoch.
        Crypto::SignedBigInteger nanoseconds { AK::UnixDateTime::now().nanoseconds_since_epoch() };

        // 2. Return the result of clamping ns between nsMinInstant and nsMaxInstant.
        if (nanoseconds < Temporal::NANOSECONDS_MIN_INSTANT)
            nanoseconds = Temporal::NANOSECONDS_MIN_INSTANT;
        if (nanoseconds > Temporal::NANOSECONDS_MAX_INSTANT)
            nanoseconds = Temporal::NANOSECONDS_MAX_INSTANT;

        return nanoseconds;
    };

    // AD-HOC: Inform the host that we received a date string we were unable to parse.
    host_unrecognized_date_string = [](StringView) {
    };
}

VM::~VM() = default;

Utf16String const& VM::error_message(ErrorMessage type) const
{
    VERIFY(type < ErrorMessage::__Count);

    auto const& message = m_error_messages[to_underlying(type)];
    VERIFY(!message.is_empty());

    return message;
}

struct ExecutionContextRootsCollector : public Cell::Visitor {
    virtual void visit_impl(GC::Cell& cell) override
    {
        roots.set(&cell);
    }

    virtual void visit_possible_values(ReadonlyBytes) override
    {
        VERIFY_NOT_REACHED();
    }

    HashTable<GC::Ptr<GC::Cell>> roots;
};

void VM::gather_roots(HashMap<GC::Cell*, GC::HeapRoot>& roots)
{
    roots.set(m_empty_string, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    for (auto string : m_single_ascii_character_strings)
        roots.set(string, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });

    for (auto string : m_numeric_string_cache)
        roots.set(string, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });

    roots.set(cached_strings.number, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.undefined, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.object, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.string, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.symbol, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.boolean, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.bigint, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.function, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    roots.set(cached_strings.object_Object, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });

#define __JS_ENUMERATE(SymbolName, snake_name) \
    roots.set(m_well_known_symbols.snake_name, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
    JS_ENUMERATE_WELL_KNOWN_SYMBOLS
#undef __JS_ENUMERATE

    for (auto& symbol : m_global_symbol_registry)
        roots.set(symbol.value, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });

    for (auto finalization_registry : m_finalization_registry_cleanup_jobs)
        roots.set(finalization_registry, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });

    auto gather_roots_from_execution_context_stack = [&roots](Vector<ExecutionContext*> const& stack) {
        for (auto const& execution_context : stack) {
            ExecutionContextRootsCollector visitor;
            execution_context->visit_edges(visitor);
            for (auto cell : visitor.roots)
                roots.set(cell, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
        }
    };
    gather_roots_from_execution_context_stack(m_execution_context_stack);
    for (auto& saved_stack : m_saved_execution_context_stacks)
        gather_roots_from_execution_context_stack(saved_stack);

    for (auto& job : m_promise_jobs)
        roots.set(job, GC::HeapRoot { .type = GC::HeapRoot::Type::VM });
}

// 9.1.2.1 GetIdentifierReference ( env, name, strict ), https://tc39.es/ecma262/#sec-getidentifierreference
ThrowCompletionOr<Reference> VM::get_identifier_reference(Environment* environment, Utf16FlyString name, Strict strict, size_t hops)
{
    // 1. If env is the value null, then
    if (!environment) {
        // a. Return the Reference Record { [[Base]]: unresolvable, [[ReferencedName]]: name, [[Strict]]: strict, [[ThisValue]]: empty }.
        return Reference { Reference::BaseType::Unresolvable, move(name), strict };
    }

    // 2. Let exists be ? env.HasBinding(name).
    Optional<size_t> index;
    auto exists = TRY(environment->has_binding(name, &index));

    // Note: This is an optimization for looking up the same reference.
    Optional<EnvironmentCoordinate> environment_coordinate;
    if (index.has_value()) {
        VERIFY(hops <= NumericLimits<u32>::max());
        VERIFY(index.value() <= NumericLimits<u32>::max());
        environment_coordinate = EnvironmentCoordinate { .hops = static_cast<u32>(hops), .index = static_cast<u32>(index.value()) };
    }

    // 3. If exists is true, then
    if (exists) {
        // a. Return the Reference Record { [[Base]]: env, [[ReferencedName]]: name, [[Strict]]: strict, [[ThisValue]]: empty }.
        return Reference { *environment, move(name), strict, environment_coordinate };
    }
    // 4. Else,
    else {
        // a. Let outer be env.[[OuterEnv]].
        // b. Return ? GetIdentifierReference(outer, name, strict).
        return get_identifier_reference(environment->outer_environment(), move(name), strict, hops + 1);
    }
}

// 9.4.2 ResolveBinding ( name [ , env ] ), https://tc39.es/ecma262/#sec-resolvebinding
ThrowCompletionOr<Reference> VM::resolve_binding(Utf16FlyString const& name, Strict strict, Environment* environment)
{
    // 1. If env is not present or if env is undefined, then
    if (!environment) {
        // a. Set env to the running execution context's LexicalEnvironment.
        environment = running_execution_context().lexical_environment;
    }

    // 2. Assert: env is an Environment Record.
    VERIFY(environment);

    // 3. If the source text matched by the syntactic production that is being evaluated is contained in strict mode code, let strict be true; else let strict be false.
    // NOTE: We take this as a parameter.

    // 4. Return ? GetIdentifierReference(env, name, strict).
    return get_identifier_reference(environment, name, strict);

    // NOTE: The spec says:
    //       Note: The result of ResolveBinding is always a Reference Record whose [[ReferencedName]] field is name.
    //       But this is not actually correct as GetIdentifierReference (or really the methods it calls) can throw.
}

// 9.4.4 ResolveThisBinding ( ), https://tc39.es/ecma262/#sec-resolvethisbinding
ThrowCompletionOr<Value> VM::resolve_this_binding()
{
    auto& vm = *this;

    // 1. Let envRec be GetThisEnvironment().
    auto environment = get_this_environment(vm);

    // 2. Return ? envRec.GetThisBinding().
    return TRY(environment->get_this_binding(vm));
}

// 9.4.5 GetNewTarget ( ), https://tc39.es/ecma262/#sec-getnewtarget
Value VM::get_new_target()
{
    // 1. Let envRec be GetThisEnvironment().
    auto env = get_this_environment(*this);

    // 2. Assert: envRec has a [[NewTarget]] field.
    // 3. Return envRec.[[NewTarget]].
    return as<FunctionEnvironment>(*env).new_target();
}

// 13.3.12.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-meta-properties-runtime-semantics-evaluation
// ImportMeta branch only
Object* VM::get_import_meta()
{
    // 1. Let module be GetActiveScriptOrModule().
    auto script_or_module = get_active_script_or_module();

    // 2. Assert: module is a Source Text Module Record.
    auto& module = as<SourceTextModule>(*script_or_module.get<GC::Ref<Module>>());

    // 3. Let importMeta be module.[[ImportMeta]].
    auto* import_meta = module.import_meta();

    // 4. If importMeta is empty, then
    if (import_meta == nullptr) {
        // a. Set importMeta to OrdinaryObjectCreate(null).
        import_meta = Object::create(*current_realm(), nullptr);

        // b. Let importMetaValues be HostGetImportMetaProperties(module).
        auto import_meta_values = host_get_import_meta_properties(module);

        // c. For each Record { [[Key]], [[Value]] } p of importMetaValues, do
        for (auto& entry : import_meta_values) {
            // i. Perform ! CreateDataPropertyOrThrow(importMeta, p.[[Key]], p.[[Value]]).
            MUST(import_meta->create_data_property_or_throw(entry.key, entry.value));
        }

        // d. Perform HostFinalizeImportMeta(importMeta, module).
        host_finalize_import_meta(import_meta, module);

        // e. Set module.[[ImportMeta]] to importMeta.
        module.set_import_meta({}, import_meta);

        // f. Return importMeta.
        return import_meta;
    }
    // 5. Else,
    else {
        // a. Assert: Type(importMeta) is Object.
        // Note: This is always true by the type.

        // b. Return importMeta.
        return import_meta;
    }
}

// 9.4.5 GetGlobalObject ( ), https://tc39.es/ecma262/#sec-getglobalobject
Object& VM::get_global_object()
{
    // 1. Let currentRealm be the current Realm Record.
    auto& current_realm = *this->current_realm();

    // 2. Return currentRealm.[[GlobalObject]].
    return current_realm.global_object();
}

void VM::run_queued_promise_jobs_impl()
{
    dbgln_if(PROMISE_DEBUG, "Running queued promise jobs");

    while (!m_promise_jobs.is_empty()) {
        auto job = m_promise_jobs.take_first();
        dbgln_if(PROMISE_DEBUG, "Calling promise job function");

        [[maybe_unused]] auto result = job->function()();
    }
}

// 9.5.4 HostEnqueuePromiseJob ( job, realm ), https://tc39.es/ecma262/#sec-hostenqueuepromisejob
void VM::enqueue_promise_job(GC::Ref<GC::Function<ThrowCompletionOr<Value>()>> job, Realm*)
{
    // An implementation of HostEnqueuePromiseJob must conform to the requirements in 9.5 as well as the following:
    // - FIXME: If realm is not null, each time job is invoked the implementation must perform implementation-defined steps such that execution is prepared to evaluate ECMAScript code at the time of job's invocation.
    // - FIXME: Let scriptOrModule be GetActiveScriptOrModule() at the time HostEnqueuePromiseJob is invoked. If realm is not null, each time job is invoked the implementation must perform implementation-defined steps
    //          such that scriptOrModule is the active script or module at the time of job's invocation.
    // - Jobs must run in the same order as the HostEnqueuePromiseJob invocations that scheduled them.
    m_promise_jobs.append(job);
}

void VM::run_queued_finalization_registry_cleanup_jobs()
{
    while (!m_finalization_registry_cleanup_jobs.is_empty()) {
        auto registry = m_finalization_registry_cleanup_jobs.take_first();
        // FIXME: Handle any uncatched exceptions here.
        (void)registry->cleanup();
    }
}

// 9.10.4.1 HostEnqueueFinalizationRegistryCleanupJob ( finalizationRegistry ), https://tc39.es/ecma262/#sec-host-cleanup-finalization-registry
void VM::enqueue_finalization_registry_cleanup_job(FinalizationRegistry& registry)
{
    m_finalization_registry_cleanup_jobs.append(&registry);
}

// 27.2.1.9 HostPromiseRejectionTracker ( promise, operation ), https://tc39.es/ecma262/#sec-host-promise-rejection-tracker
void VM::promise_rejection_tracker(Promise& promise, Promise::RejectionOperation operation) const
{
    switch (operation) {
    case Promise::RejectionOperation::Reject:
        // A promise was rejected without any handlers
        if (on_promise_unhandled_rejection)
            on_promise_unhandled_rejection(promise);
        break;
    case Promise::RejectionOperation::Handle:
        // A handler was added to an already rejected promise
        if (on_promise_rejection_handled)
            on_promise_rejection_handled(promise);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

void VM::dump_backtrace() const
{
    for (ssize_t i = m_execution_context_stack.size() - 1; i >= 0; --i) {
        auto& frame = m_execution_context_stack[i];

        if (frame->executable) {
            auto source_range = frame->executable->source_range_at(frame->program_counter).realize();
            dbgln("-> {} @ {}:{},{}", frame->function ? frame->function->name_for_call_stack() : ""_utf16, source_range.filename(), source_range.start.line, source_range.start.column);
        } else {
            dbgln("-> {}", frame->function ? frame->function->name_for_call_stack() : ""_utf16);
        }
    }
}

void VM::save_execution_context_stack()
{
    m_saved_execution_context_stacks.append(move(m_execution_context_stack));
}

void VM::clear_execution_context_stack()
{
    m_execution_context_stack.clear_with_capacity();
}

void VM::restore_execution_context_stack()
{
    m_execution_context_stack = m_saved_execution_context_stacks.take_last();
}

// 9.4.1 GetActiveScriptOrModule ( ), https://tc39.es/ecma262/#sec-getactivescriptormodule
ScriptOrModule VM::get_active_script_or_module() const
{
    // 1. If the execution context stack is empty, return null.
    if (m_execution_context_stack.is_empty())
        return Empty {};

    // 2. Let ec be the topmost execution context on the execution context stack whose ScriptOrModule component is not null.
    for (auto i = m_execution_context_stack.size() - 1; i > 0; i--) {
        if (!m_execution_context_stack[i]->script_or_module.has<Empty>())
            return m_execution_context_stack[i]->script_or_module;
    }

    // 3. If no such execution context exists, return null. Otherwise, return ec's ScriptOrModule.
    // Note: Since it is not empty we have 0 and since we got here all the
    //       above contexts don't have a non-null ScriptOrModule
    return m_execution_context_stack[0]->script_or_module;
}

VM::StoredModule* VM::get_stored_module(ImportedModuleReferrer const&, ByteString const& filename, Utf16String const&)
{
    // Note the spec says:
    // If this operation is called multiple times with the same (referrer, specifier) pair and it performs
    // FinishLoadingImportedModule(referrer, specifier, payload, result) where result is a normal completion,
    // then it must perform FinishLoadingImportedModule(referrer, specifier, payload, result) with the same result each time.

    // Editor's Note from https://tc39.es/ecma262/#sec-hostresolveimportedmodule
    // The above text requires that hosts support JSON modules when imported with type: "json" (and HostLoadImportedModule
    // completes normally), but it does not prohibit hosts from supporting JSON modules when imported without type: "json".

    // FIXME: This should probably check referrer as well.
    auto end_or_module = m_loaded_modules.find_if([&](StoredModule const& stored_module) {
        return stored_module.filename == filename;
    });
    if (end_or_module.is_end())
        return nullptr;
    return &(*end_or_module);
}

ThrowCompletionOr<void> VM::link_and_eval_module(Badge<Bytecode::Interpreter>, SourceTextModule& module)
{
    return link_and_eval_module(module);
}

ThrowCompletionOr<void> VM::link_and_eval_module(CyclicModule& module)
{
    auto filename = module.filename();
    auto& promise_capability = module.load_requested_modules(nullptr);

    if (auto const& promise = as<Promise>(*promise_capability.promise()); promise.state() == Promise::State::Rejected)
        return JS::throw_completion(promise.result());

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] Linking module {}", filename);
    auto linked_or_error = module.link(*this);
    if (linked_or_error.is_error())
        return linked_or_error.throw_completion();

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] Linking passed, now evaluating module {}", filename);
    auto evaluated_or_error = module.evaluate(*this);

    if (evaluated_or_error.is_error())
        return evaluated_or_error.throw_completion();

    auto evaluated_value = evaluated_or_error.value();

    run_queued_promise_jobs();
    VERIFY(m_promise_jobs.is_empty());

    // FIXME: This will break if we start doing promises actually asynchronously.
    VERIFY(evaluated_value->state() != Promise::State::Pending);

    if (evaluated_value->state() == Promise::State::Rejected)
        return JS::throw_completion(evaluated_value->result());

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] Evaluating passed for module {}", module.filename());
    return {};
}

static ByteString resolve_module_filename(StringView filename, Utf16View const& module_type)
{
    auto extensions = Vector<StringView, 2> { "js"sv, "mjs"sv };
    if (module_type == "json"sv)
        extensions = { "json"sv };
    if (!FileSystem::exists(filename)) {
        for (auto extension : extensions) {
            // import "./foo" -> import "./foo.ext"
            auto resolved_filepath = ByteString::formatted("{}.{}", filename, extension);
            if (FileSystem::exists(resolved_filepath))
                return resolved_filepath;
        }
    } else if (FileSystem::is_directory(filename)) {
        for (auto extension : extensions) {
            // import "./foo" -> import "./foo/index.ext"
            auto resolved_filepath = LexicalPath::join(filename, ByteString::formatted("index.{}", extension)).string();
            if (FileSystem::exists(resolved_filepath))
                return resolved_filepath;
        }
    }
    return filename;
}

// 16.2.1.8 HostLoadImportedModule ( referrer, specifier, hostDefined, payload ), https://tc39.es/ecma262/#sec-HostLoadImportedModule
void VM::load_imported_module(ImportedModuleReferrer referrer, ModuleRequest const& module_request, GC::Ptr<GraphLoadingState::HostDefined>, ImportedModulePayload payload)
{
    // An implementation of HostLoadImportedModule must conform to the following requirements:
    //
    // - The host environment must perform FinishLoadingImportedModule(referrer, specifier, payload, result),
    //   where result is either a normal completion containing the loaded Module Record or a throw completion,
    //   either synchronously or asynchronously.
    // - If this operation is called multiple times with the same (referrer, specifier) pair and it performs
    //   FinishLoadingImportedModule(referrer, specifier, payload, result) where result is a normal completion,
    //   then it must perform FinishLoadingImportedModule(referrer, specifier, payload, result) with the same result each time.
    // - If moduleRequest.[[Attributes]] has an entry entry such that entry.[[Key]] is "type" and entry.[[Value]] is "json",
    //   when the host environment performs FinishLoadingImportedModule(referrer, moduleRequest, payload, result), result
    //   must either be the Completion Record returned by an invocation of ParseJSONModule or a throw completion.
    // - The operation must treat payload as an opaque value to be passed through to FinishLoadingImportedModule.
    //
    // The actual process performed is host-defined, but typically consists of performing whatever I/O operations are necessary to
    // load the appropriate Module Record. Multiple different (referrer, specifier) pairs may map to the same Module Record instance.
    // The actual mapping semantics is host-defined but typically a normalization process is applied to specifier as part of the
    // mapping process. A typical normalization process would include actions such as expansion of relative and abbreviated path specifiers.

    // Here we check, against the spec, if payload is a promise capability, meaning that this was called for a dynamic import
    if (payload.has<GC::Ref<PromiseCapability>>() && !m_dynamic_imports_allowed) {
        // If you are here because you want to enable dynamic module importing make sure it won't be a security problem
        // by checking the default implementation of HostImportModuleDynamically and creating your own hook or calling
        // vm.allow_dynamic_imports().
        finish_loading_imported_module(referrer, module_request, payload, throw_completion<InternalError>(ErrorType::DynamicImportNotAllowed, module_request.module_specifier));
        return;
    }

    Utf16String module_type;
    for (auto& attribute : module_request.attributes) {
        if (attribute.key == "type"sv) {
            module_type = attribute.value;
            break;
        }
    }

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] module at {} has type {}", module_request.module_specifier, module_type);

    StringView const base_filename = referrer.visit(
        [&](GC::Ref<Realm> const&) {
            // Generally within ECMA262 we always get a referencing_script_or_module. However, ShadowRealm gives an explicit null.
            // To get around this is we attempt to get the active script_or_module otherwise we might start loading "random" files from the working directory.
            return get_active_script_or_module().visit(
                [](Empty) {
                    return "."sv;
                },
                [](auto const& script_or_module) {
                    return script_or_module->filename();
                });
        },
        [&](auto const& script_or_module) {
            return script_or_module->filename();
        });

    LexicalPath base_path { base_filename };
    auto filename = LexicalPath::absolute_path(base_path.dirname(), MUST(module_request.module_specifier.view().to_byte_string()));

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] base path: '{}'", base_path);
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] initial filename: '{}'", filename);

    filename = resolve_module_filename(filename, module_type);

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] resolved filename: '{}'", filename);

#if JS_MODULE_DEBUG
    ByteString referencing_module_string = referrer.visit(
        [&](Empty) -> ByteString {
            return ".";
        },
        [&](auto& script_or_module) {
            if constexpr (IsSame<Script*, decltype(script_or_module)>) {
                return ByteString::formatted("Script @ {}", script_or_module.ptr());
            }
            return ByteString::formatted("Module @ {}", script_or_module.ptr());
        });

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] load_imported_module({}, {})", referencing_module_string, filename);
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE]     resolved {} + {} -> {}", base_path, module_request.module_specifier, filename);
#endif

    if (auto* loaded_module = get_stored_module(referrer, filename, module_type)) {
        dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] load_imported_module({}) already loaded at {}", filename, loaded_module->module.ptr());
        finish_loading_imported_module(referrer, module_request, payload, *loaded_module->module);
        return;
    }

    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] reading and parsing module {}", filename);

    auto file_or_error = Core::File::open(filename, Core::File::OpenMode::Read);

    if (file_or_error.is_error()) {
        finish_loading_imported_module(referrer, module_request, payload, throw_completion<SyntaxError>(ErrorType::ModuleNotFound, module_request.module_specifier));
        return;
    }

    // FIXME: Don't read the file in one go.
    auto file_content_or_error = file_or_error.value()->read_until_eof();

    if (file_content_or_error.is_error()) {
        if (file_content_or_error.error().code() == ENOMEM) {
            finish_loading_imported_module(referrer, module_request, payload, throw_completion<JS::InternalError>(error_message(::JS::VM::ErrorMessage::OutOfMemory)));
            return;
        }
        finish_loading_imported_module(referrer, module_request, payload, throw_completion<SyntaxError>(ErrorType::ModuleNotFound, module_request.module_specifier));
        return;
    }

    StringView const content_view { file_content_or_error.value().bytes() };

    auto module = [&, content = file_content_or_error.release_value()]() -> ThrowCompletionOr<GC::Ref<Module>> {
        // If moduleRequest.[[Attributes]] has an entry entry such that entry.[[Key]] is "type" and entry.[[Value]] is "json",
        // when the host environment performs FinishLoadingImportedModule(referrer, moduleRequest, payload, result), result
        // must either be the Completion Record returned by an invocation of ParseJSONModule or a throw completion.
        if (module_type == "json"sv) {
            dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] reading and parsing JSON module {}", filename);
            return parse_json_module(*current_realm(), content_view, filename);
        }

        dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] reading and parsing as SourceTextModule module {}", filename);
        // Note: We treat all files as module, so if a script does not have exports it just runs it.
        auto module_or_errors = SourceTextModule::parse(content_view, *current_realm(), filename);

        if (module_or_errors.is_error()) {
            VERIFY(module_or_errors.error().size() > 0);
            return throw_completion<SyntaxError>(module_or_errors.error().first().to_byte_string());
        }

        return module_or_errors.release_value();
    }();

    if (!module.is_error()) {
        m_loaded_modules.empend(
            referrer,
            module.value()->filename(),
            String {}, // Null type
            make_root(module.value()),
            true);
    }

    finish_loading_imported_module(referrer, module_request, payload, module);
}

static RefPtr<CachedSourceRange> get_source_range(ExecutionContext const* context)
{
    // native function
    if (!context->executable)
        return {};

    if (!context->cached_source_range
        || context->cached_source_range->program_counter != context->program_counter) {
        auto unrealized_source_range = context->executable->source_range_at(context->program_counter);
        context->cached_source_range = adopt_ref(*new CachedSourceRange(
            context->program_counter,
            move(unrealized_source_range)));
    }
    return context->cached_source_range;
}

Vector<StackTraceElement> VM::stack_trace() const
{
    Vector<StackTraceElement> stack_trace;
    for (ssize_t i = m_execution_context_stack.size() - 1; i >= 0; i--) {
        auto* context = m_execution_context_stack[i];
        stack_trace.append({
            .execution_context = context,
            .source_range = get_source_range(context),
        });
    }

    return stack_trace;
}

}
