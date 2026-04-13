/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/StackInfo.h>
#include <AK/Variant.h>
#include <LibCrypto/Forward.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/RootVector.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Operand.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/Export.h>
#include <LibJS/ModuleLoading.h>
#include <LibJS/Runtime/Agent.h>
#include <LibJS/Runtime/CommonPropertyNames.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/InterpreterStack.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class Identifier;
struct BindingPattern;

enum class HandledByHost {
    Handled,
    Unhandled,
};

enum class EvalMode {
    Direct,
    Indirect
};

enum class CompilationType {
    DirectEval,
    IndirectEval,
    Function,
    Timer,
};

class JS_API VM : public RefCounted<VM> {
public:
    static NonnullRefPtr<VM> create();
    ~VM();

    ALWAYS_INLINE static VM& the() { return *s_the; }

    GC::Heap& heap() const { return const_cast<GC::Heap&>(m_heap); }

    VM& vm() { return *this; }
    VM const& vm() const { return *this; }

    [[nodiscard]] Realm& realm() { return *m_running_execution_context->realm; }
    [[nodiscard]] Object& global_object() { return realm().global_object(); }
    [[nodiscard]] DeclarativeEnvironment& global_declarative_environment();

    ThrowCompletionOr<Value> run(Script&, GC::Ptr<Environment> lexical_environment_override = nullptr);
    ThrowCompletionOr<Value> run(SourceTextModule&);

    ThrowCompletionOr<Value> run_executable(ExecutionContext&, Bytecode::Executable&, u32 entry_point = 0);
    ThrowCompletionOr<Value> run_executable(ExecutionContext& context, Bytecode::Executable& executable, u32 entry_point, Value initial_accumulator_value)
    {
        context.registers_and_constants_and_locals_and_arguments_span()[0] = initial_accumulator_value;
        return run_executable(context, executable, entry_point);
    }

    ALWAYS_INLINE Value& accumulator() { return reg(Bytecode::Register::accumulator()); }
    Value& reg(Bytecode::Register const& r)
    {
        return m_running_execution_context->registers_and_constants_and_locals_and_arguments()[r.index()];
    }
    Value reg(Bytecode::Register const& r) const
    {
        return m_running_execution_context->registers_and_constants_and_locals_and_arguments()[r.index()];
    }

    ALWAYS_INLINE Value get(Bytecode::Operand op) const
    {
        return m_running_execution_context->registers_and_constants_and_locals_and_arguments()[op.raw()];
    }
    ALWAYS_INLINE void set(Bytecode::Operand op, Value value)
    {
        m_running_execution_context->registers_and_constants_and_locals_and_arguments_span().data()[op.raw()] = value;
    }

    Value do_yield(Value value, Optional<Bytecode::Label> continuation);
    void do_return(Value value)
    {
        if (value.is_special_empty_value())
            value = js_undefined();
        reg(Bytecode::Register::return_value()) = value;
        reg(Bytecode::Register::exception()) = js_special_empty_value();
    }

    void catch_exception(Bytecode::Operand dst);

    Bytecode::Executable& current_executable() { return *m_running_execution_context->executable; }
    Bytecode::Executable const& current_executable() const { return *m_running_execution_context->executable; }

    [[nodiscard]] Utf16FlyString const& get_identifier(Bytecode::IdentifierTableIndex) const;
    [[nodiscard]] Optional<Utf16FlyString const&> get_identifier(Optional<Bytecode::IdentifierTableIndex> index) const
    {
        if (!index.has_value())
            return {};
        return get_identifier(*index);
    }

    [[nodiscard]] PropertyKey const& get_property_key(Bytecode::PropertyKeyTableIndex) const;

    enum class HandleExceptionResponse {
        ExitFromExecutable,
        ContinueInThisExecutable,
    };
    [[nodiscard]] COLD HandleExceptionResponse handle_exception(u32 program_counter, Value exception);

    NEVER_INLINE void pop_inline_frame(Value return_value);

    ExecutionContext* push_inline_frame(
        ECMAScriptFunctionObject& callee_function,
        Bytecode::Executable& callee_executable,
        ReadonlySpan<Bytecode::Operand> arguments,
        u32 return_pc,
        u32 dst_raw,
        Value this_value,
        Object* new_target,
        bool is_construct);

    void dump_backtrace() const;

    void gather_roots(HashMap<GC::Cell*, GC::HeapRoot>&);

#define __JS_ENUMERATE(SymbolName, snake_name)             \
    GC::Ref<Symbol> well_known_symbol_##snake_name() const \
    {                                                      \
        return *m_well_known_symbols.snake_name;           \
    }
    JS_ENUMERATE_WELL_KNOWN_SYMBOLS
#undef __JS_ENUMERATE

    HashMap<String, GC::Ptr<PrimitiveString>>& string_cache()
    {
        return m_string_cache;
    }

    HashMap<Utf16String, GC::Ptr<PrimitiveString>>& utf16_string_cache()
    {
        return m_utf16_string_cache;
    }

    auto& numeric_string_cache() { return m_numeric_string_cache; }

    PrimitiveString& empty_string() { return *m_empty_string; }

    PrimitiveString& single_ascii_character_string(u8 character)
    {
        VERIFY(character < 0x80);
        return *m_single_ascii_character_strings[character];
    }

    // This represents the list of errors from ErrorTypes.h whose messages are used in contexts which
    // must not fail to allocate when they are used. For example, we cannot allocate when we raise an
    // out-of-memory error, thus we pre-allocate that error string at VM creation time.
    enum class ErrorMessage {
        OutOfMemory,

        // Keep this last:
        __Count,
    };
    Utf16String const& error_message(ErrorMessage) const;

    bool did_reach_stack_space_limit() const
    {
#if defined(HAS_ADDRESS_SANITIZER)
        // We hit stack limits sooner with ASAN enabled.
        return m_stack_info.size_free() < 96 * KiB;
#else
        return m_stack_info.size_free() < 32 * KiB;
#endif
    }

    // TODO: Rename this function instead of providing a second argument, now that the global object is no longer passed in.
    struct CheckStackSpaceLimitTag { };

    ThrowCompletionOr<void> push_execution_context(ExecutionContext& context, CheckStackSpaceLimitTag)
    {
        // Ensure we got some stack space left, so the next function call doesn't kill us.
        if (did_reach_stack_space_limit()) [[unlikely]] {
            return throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);
        }
        context.caller_frame = nullptr;
        context.caller_return_pc = 0;
        context.caller_dst_raw = 0;
        context.caller_is_construct = false;
        m_execution_context_stack.append(&context);
        m_execution_context_stack_previous_running_contexts.append(m_running_execution_context);
        m_running_execution_context = &context;
        return {};
    }

    void push_execution_context(ExecutionContext& context)
    {
        context.caller_frame = nullptr;
        context.caller_return_pc = 0;
        context.caller_dst_raw = 0;
        context.caller_is_construct = false;
        m_execution_context_stack.append(&context);
        m_execution_context_stack_previous_running_contexts.append(m_running_execution_context);
        m_running_execution_context = &context;
    }

    ExecutionContext* pop_execution_context()
    {
        VERIFY(!m_execution_context_stack.is_empty());
        auto* context = m_execution_context_stack.take_last();
        context->caller_frame = nullptr;
        context->caller_return_pc = 0;
        context->caller_dst_raw = 0;
        context->caller_is_construct = false;
        m_running_execution_context = m_execution_context_stack_previous_running_contexts.take_last();
        return context;
    }

    // https://tc39.es/ecma262/#running-execution-context
    // At any point in time, there is at most one execution context per agent that is actually executing code.
    // This is known as the agent's running execution context.
    ExecutionContext& running_execution_context()
    {
        VERIFY(m_running_execution_context);
        return *m_running_execution_context;
    }
    ExecutionContext const& running_execution_context() const
    {
        VERIFY(m_running_execution_context);
        return *m_running_execution_context;
    }

    bool has_running_execution_context() const { return m_running_execution_context != nullptr; }

    // https://tc39.es/ecma262/#execution-context-stack
    // The execution context stack tracks base execution contexts. Inline JS-to-JS
    // frames are threaded through ExecutionContext::caller_frame starting at the
    // running execution context.
    Vector<ExecutionContext*> const& execution_context_stack() const { return m_execution_context_stack; }

    template<typename Callback>
    void for_each_execution_context_top_to_bottom(Callback callback)
    {
        for_each_execution_context_top_to_bottom(m_execution_context_stack, m_execution_context_stack_previous_running_contexts, m_running_execution_context, callback);
    }

    template<typename Callback>
    void for_each_execution_context_top_to_bottom(Callback callback) const
    {
        for_each_execution_context_top_to_bottom(m_execution_context_stack, m_execution_context_stack_previous_running_contexts, m_running_execution_context, callback);
    }

    template<typename Callback>
    Optional<ExecutionContext*> last_execution_context_matching(Callback callback)
    {
        Optional<ExecutionContext*> matching_execution_context;
        for_each_execution_context_top_to_bottom([&](ExecutionContext& execution_context) {
            if (!callback(&execution_context))
                return true;
            matching_execution_context = &execution_context;
            return false;
        });
        return matching_execution_context;
    }

    template<typename Callback>
    Optional<ExecutionContext const*> last_execution_context_matching(Callback callback) const
    {
        Optional<ExecutionContext const*> matching_execution_context;
        for_each_execution_context_top_to_bottom([&](ExecutionContext const& execution_context) {
            if (!callback(&execution_context))
                return true;
            matching_execution_context = &execution_context;
            return false;
        });
        return matching_execution_context;
    }

    ExecutionContext* previous_execution_context() const;

    Environment const* lexical_environment() const { return running_execution_context().lexical_environment; }
    Environment* lexical_environment() { return running_execution_context().lexical_environment; }

    Environment const* variable_environment() const { return running_execution_context().variable_environment; }
    Environment* variable_environment() { return running_execution_context().variable_environment; }

    // https://tc39.es/ecma262/#current-realm
    // The value of the Realm component of the running execution context is also called the current Realm Record.
    Realm const* current_realm() const { return running_execution_context().realm; }
    Realm* current_realm() { return running_execution_context().realm; }

    // https://tc39.es/ecma262/#active-function-object
    // The value of the Function component of the running execution context is also called the active function object.
    FunctionObject const* active_function_object() const { return running_execution_context().function; }
    FunctionObject* active_function_object() { return running_execution_context().function; }

    size_t argument_count() const
    {
        return running_execution_context().argument_count;
    }

    Value argument(size_t index) const
    {
        return running_execution_context().argument(index);
    }

    Value this_value() const
    {
        return running_execution_context().this_value.value();
    }

    ThrowCompletionOr<Value> resolve_this_binding();

    StackInfo const& stack_info() const { return m_stack_info; }

    InterpreterStack& interpreter_stack() { return m_interpreter_stack; }

    HashMap<Utf16String, GC::Ref<Symbol>> const& global_symbol_registry() const { return m_global_symbol_registry; }
    HashMap<Utf16String, GC::Ref<Symbol>>& global_symbol_registry() { return m_global_symbol_registry; }

    u32 execution_generation() const { return m_execution_generation; }
    void finish_execution_generation() { ++m_execution_generation; }

    ThrowCompletionOr<Reference> resolve_binding(Utf16FlyString const&, Strict, Environment* = nullptr);
    ThrowCompletionOr<Reference> get_identifier_reference(Environment*, Utf16FlyString, Strict, size_t hops = 0);

    // 5.2.3.2 Throw an Exception, https://tc39.es/ecma262/#sec-throw-an-exception
    template<typename T, typename... Args>
    COLD Completion throw_completion(Args&&... args)
    {
        auto& realm = *current_realm();
        auto completion = T::create(realm, forward<Args>(args)...);

        return JS::throw_completion(completion);
    }

    template<typename T>
    COLD Completion throw_completion(ErrorType const& type)
    {
        return throw_completion<T>(type.message());
    }

    template<typename T, typename... Args>
    COLD Completion throw_completion(ErrorType const& type, Args&&... args)
    {
        return throw_completion<T>(Utf16String::formatted(type.format(), forward<Args>(args)...));
    }

    Value get_new_target();

    Object* get_import_meta();

    Object& get_global_object();

    CommonPropertyNames names;
    struct {
        GC::Ptr<PrimitiveString> number;
        GC::Ptr<PrimitiveString> undefined;
        GC::Ptr<PrimitiveString> object;
        GC::Ptr<PrimitiveString> string;
        GC::Ptr<PrimitiveString> symbol;
        GC::Ptr<PrimitiveString> boolean;
        GC::Ptr<PrimitiveString> bigint;
        GC::Ptr<PrimitiveString> function;
        GC::Ptr<PrimitiveString> object_Object;
    } cached_strings;

    void run_queued_promise_jobs()
    {
        if (m_promise_jobs.is_empty())
            return;
        run_queued_promise_jobs_impl();
    }

    void enqueue_promise_job(GC::Ref<GC::Function<ThrowCompletionOr<Value>()>> job, Realm*);

    void run_queued_finalization_registry_cleanup_jobs();
    void enqueue_finalization_registry_cleanup_job(FinalizationRegistry&);

    void promise_rejection_tracker(Promise&, Promise::RejectionOperation) const;

    Function<void(Promise&)> on_promise_unhandled_rejection;
    Function<void(Promise&)> on_promise_rejection_handled;
    Function<void(Object const&, PropertyKey const&)> on_unimplemented_property_access;

    void set_agent(OwnPtr<Agent> agent) { m_agent = move(agent); }
    Agent* agent() { return m_agent; }
    Agent const* agent() const { return m_agent; }

    void save_execution_context_stack();
    void clear_execution_context_stack();
    void restore_execution_context_stack();

    ScriptOrModule get_active_script_or_module() const;

    // 16.2.1.10 HostLoadImportedModule ( referrer, moduleRequest, hostDefined, payload ), https://tc39.es/ecma262/#sec-HostLoadImportedModule
    Function<void(ImportedModuleReferrer, ModuleRequest const&, GC::Ptr<GraphLoadingState::HostDefined>, ImportedModulePayload)> host_load_imported_module;

    Function<HashMap<PropertyKey, Value>(SourceTextModule&)> host_get_import_meta_properties;
    Function<void(Object*, SourceTextModule const&)> host_finalize_import_meta;

    Function<Vector<Utf16String>()> host_get_supported_import_attributes;

    void set_dynamic_imports_allowed(bool value) { m_dynamic_imports_allowed = value; }

    Function<void(Promise&, Promise::RejectionOperation)> host_promise_rejection_tracker;
    Function<ThrowCompletionOr<Value>(JobCallback&, Value, ReadonlySpan<Value>)> host_call_job_callback;
    Function<void(FinalizationRegistry&)> host_enqueue_finalization_registry_cleanup_job;
    Function<void(GC::Ref<GC::Function<ThrowCompletionOr<Value>()>>, Realm*)> host_enqueue_promise_job;
    Function<GC::Ref<JobCallback>(FunctionObject&)> host_make_job_callback;
    Function<GC::Ptr<PrimitiveString>(Object const&)> host_get_code_for_eval;
    Function<ThrowCompletionOr<void>(Realm&, ReadonlySpan<String>, StringView, StringView, CompilationType, ReadonlySpan<Value>, Value)> host_ensure_can_compile_strings;
    Function<ThrowCompletionOr<void>(Object&)> host_ensure_can_add_private_element;
    Function<ThrowCompletionOr<HandledByHost>(ArrayBuffer&, size_t)> host_resize_array_buffer;
    Function<ThrowCompletionOr<HandledByHost>(ArrayBuffer&, size_t)> host_grow_shared_array_buffer;
    Function<void(StringView)> host_unrecognized_date_string;
    Function<Crypto::SignedBigInteger(Object const& global)> host_system_utc_epoch_nanoseconds;
    Function<bool()> host_promise_job_queue_is_empty;

    [[nodiscard]] Vector<StackTraceElement> stack_trace() const;

private:
    using ErrorMessages = AK::Array<Utf16String, to_underlying(ErrorMessage::__Count)>;

    struct WellKnownSymbols {
#define __JS_ENUMERATE(SymbolName, snake_name) \
    GC::Ptr<Symbol> snake_name;
        JS_ENUMERATE_WELL_KNOWN_SYMBOLS
#undef __JS_ENUMERATE
    };

    explicit VM(ErrorMessages);

    template<typename Callback>
    static void for_each_execution_context_top_to_bottom(Vector<ExecutionContext*> const& execution_context_stack, Vector<ExecutionContext*> const& execution_context_stack_previous_running_contexts, ExecutionContext* running_execution_context, Callback callback)
    {
        VERIFY(execution_context_stack.size() == execution_context_stack_previous_running_contexts.size());

        if (!running_execution_context) {
            for (size_t i = execution_context_stack.size(); i-- > 0;) {
                if (!callback(*execution_context_stack[i]))
                    return;
            }
            return;
        }

        if (execution_context_stack.is_empty()) {
            for (auto* execution_context = running_execution_context; execution_context; execution_context = execution_context->caller_frame) {
                if (!callback(*execution_context))
                    return;
            }
            return;
        }

        auto stack_index = execution_context_stack.size();
        auto* execution_context = running_execution_context;
        while (execution_context) {
            if (!callback(*execution_context))
                return;
            if (stack_index > 0 && execution_context == execution_context_stack[stack_index - 1]) {
                execution_context = execution_context_stack_previous_running_contexts[stack_index - 1];
                --stack_index;
                continue;
            }

            execution_context = execution_context->caller_frame;
        }

        VERIFY(stack_index == 0);
    }

    struct SavedExecutionContextStack {
        Vector<ExecutionContext*> stack;
        Vector<ExecutionContext*> previous_running_contexts;
        ExecutionContext* running_execution_context { nullptr };
    };

    void load_imported_module(ImportedModuleReferrer, ModuleRequest const&, GC::Ptr<GraphLoadingState::HostDefined>, ImportedModulePayload);
    ThrowCompletionOr<void> link_and_eval_module(CyclicModule&);
    ThrowCompletionOr<void> link_and_eval_module(SourceTextModule&);

    void set_well_known_symbols(WellKnownSymbols well_known_symbols) { m_well_known_symbols = move(well_known_symbols); }

    void run_queued_promise_jobs_impl();
    void run_bytecode(size_t entry_point);

    [[nodiscard]] NEVER_INLINE bool try_inline_call(Bytecode::Instruction const&, u32 current_pc);
    [[nodiscard]] NEVER_INLINE bool try_inline_call_construct(Bytecode::Instruction const&, u32 current_pc);

    static VM* s_the;

    HashMap<String, GC::Ptr<PrimitiveString>> m_string_cache;
    HashMap<Utf16String, GC::Ptr<PrimitiveString>> m_utf16_string_cache;

    static constexpr size_t numeric_string_cache_size = 1000;
    AK::Array<GC::Ptr<PrimitiveString>, numeric_string_cache_size> m_numeric_string_cache;

    GC::Heap m_heap;

    Vector<ExecutionContext*> m_execution_context_stack;
    // Base pushes may happen while an inline JS-to-JS frame is running, and
    // TemporaryExecutionContext can push the same context multiple times. Keep
    // the previous running context for each base push so we can restore and
    // walk the full active stack without relying on caller_frame there.
    Vector<ExecutionContext*> m_execution_context_stack_previous_running_contexts;
    ExecutionContext* m_running_execution_context { nullptr };

    Vector<SavedExecutionContextStack> m_saved_execution_context_stacks;

    StackInfo m_stack_info;

    InterpreterStack m_interpreter_stack;

    // GlobalSymbolRegistry, https://tc39.es/ecma262/#table-globalsymbolregistry-record-fields
    HashMap<Utf16String, GC::Ref<Symbol>> m_global_symbol_registry;

    Vector<GC::Ref<GC::Function<ThrowCompletionOr<Value>()>>> m_promise_jobs;

    Vector<GC::Ref<FinalizationRegistry>> m_finalization_registry_cleanup_jobs;

    GC::Ptr<PrimitiveString> m_empty_string;
    GC::Ptr<PrimitiveString> m_single_ascii_character_strings[128] {};
    ErrorMessages m_error_messages;

    struct StoredModule {
        ImportedModuleReferrer referrer;
        ByteString filename;
        String type;
        GC::Root<Module> module;
        bool has_once_started_linking { false };
    };

    StoredModule* get_stored_module(ImportedModuleReferrer const& script_or_module, ByteString const& filename, Utf16String const& type);

    Vector<StoredModule> m_loaded_modules;

    WellKnownSymbols m_well_known_symbols;

    u32 m_execution_generation { 0 };

    OwnPtr<Agent> m_agent;

    bool m_dynamic_imports_allowed { false };
};

template<typename GlobalObjectType, typename... Args>
[[nodiscard]] static NonnullOwnPtr<ExecutionContext> create_simple_execution_context(VM& vm, Args&&... args)
{
    auto root_execution_context = MUST(Realm::initialize_host_defined_realm(
        vm,
        [&](Realm& realm_) -> GlobalObject* {
            return vm.heap().allocate<GlobalObjectType>(realm_, forward<Args>(args)...);
        },
        nullptr));
    return root_execution_context;
}

ALWAYS_INLINE VM& Cell::vm() const { return VM::the(); }

}
