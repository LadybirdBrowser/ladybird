/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/DistinctNumeric.h>
#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/StackInfo.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCrypto/Forward.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/RootVector.h>
#include <LibJS/Breakpoint.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/ModuleLoading.h>
#include <LibJS/Runtime/Agent.h>
#include <LibJS/Runtime/CommonPropertyNames.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/InterpreterStack.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseJob.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

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

// The constructors of the errors that the runtime creates for the embedder, in the order of the runtime's own list.
enum class EngineErrorKind : u8 {
    Error,
    EvalError,
    InternalError,
    RangeError,
    ReferenceError,
    SyntaxError,
    TypeError,
    URIError,
    AggregateError,
    SuppressedError,
};

template<typename T>
struct EngineErrorKindOf;

#define JS_DEFINE_ENGINE_ERROR_KIND_OF(ClassName)                                        \
    template<>                                                                           \
    struct EngineErrorKindOf<ClassName> {                                                \
        static constexpr EngineErrorKind engine_error_kind = EngineErrorKind::ClassName; \
    };
JS_DEFINE_ENGINE_ERROR_KIND_OF(Error)
JS_DEFINE_ENGINE_ERROR_KIND_OF(AggregateError)
JS_DEFINE_ENGINE_ERROR_KIND_OF(SuppressedError)
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    JS_DEFINE_ENGINE_ERROR_KIND_OF(ClassName)
JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
#undef JS_DEFINE_ENGINE_ERROR_KIND_OF

template<typename T>
concept IsEngineErrorType = requires { EngineErrorKindOf<T>::engine_error_kind; };

struct HostHookThunks;

// The VM of the Rust runtime, which this object holds in its first bytes, so that a VM& of C++ and the runtime's VM
// pointer are the same address: the runtime passes it to raw native functions as their VM&. The host hooks, the
// property names and the agent are C++ members after it, and the runtime calls the hooks through one table of thunks.
class JS_API VM {
    AK_MAKE_NONCOPYABLE(VM);
    AK_MAKE_NONMOVABLE(VM);

    // The runtime's VM is constructed here, and must stay at the start of the object.
    alignas(JS_LAYOUT_VM_ALIGN) u8 m_engine_storage[JS_LAYOUT_VM_SIZE];

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullRefPtr<VM> create();
    ~VM();

    // VM is reference counted like an AK::RefCounted, whose count would have to come first.
    void ref() const { ++m_reference_count; }
    void unref() const;

    ALWAYS_INLINE static VM& the() { return *s_the; }

    GC::Heap& heap() const { return *m_heap; }

    VM& vm() { return *this; }
    VM const& vm() const { return *this; }

    [[nodiscard]] Realm& realm() { return *running_execution_context().realm; }
    [[nodiscard]] Object& global_object() { return realm().global_object(); }

    ThrowCompletionOr<Value> run(Script&, GC::Ptr<Environment> lexical_environment_override = nullptr);

    void enable_debugging();
    void disable_debugging();
    [[nodiscard]] bool debugging_enabled() const;
    // The runtime's debugger, which is attached while debugging is enabled. A Debugger handle has the VM's address.
    [[nodiscard]] Debugger* debugger();
    [[nodiscard]] Debugger const* debugger() const;

#define __JS_ENUMERATE(SymbolName, snake_name)                 \
    GC::Ref<Symbol> well_known_symbol_##snake_name() const     \
    {                                                          \
        return well_known_symbol(WellKnownSymbol::snake_name); \
    }
    JS_ENUMERATE_WELL_KNOWN_SYMBOLS
#undef __JS_ENUMERATE

    PrimitiveString& empty_string();

    // This represents the list of errors from ErrorTypes.h whose messages are used in contexts which
    // must not fail to allocate when they are used. For example, we cannot allocate when we raise an
    // out-of-memory error, thus we pre-allocate that error string at VM creation time.
    enum class ErrorMessage {
        OutOfMemory,

        // Keep this last:
        __Count,
    };
    Utf16String const& error_message(ErrorMessage) const;

    bool did_reach_stack_space_limit() const;

    // Pushes onto the runtime's execution context stack in place, as the runtime itself does, unless the stack's
    // storage is full, which only the runtime can grow.
    void push_execution_context(ExecutionContext& execution_context)
    {
        auto length = execution_context_stack_length();
        if (length == execution_context_stack_capacity()) [[unlikely]] {
            push_execution_context_growing_the_stack(execution_context);
            return;
        }
        execution_context.caller_frame = nullptr;
        execution_context.caller_return_pc = 0;
        execution_context.caller_dst_raw = 0;
        execution_context.caller_is_construct = false;
        auto& entry = execution_context_stack_entries()[length];
        entry.execution_context = &execution_context;
        entry.previous_running_execution_context = running_execution_context_or_null();
        set_engine_head_field<size_t>(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_LENGTH_OFFSET, length + 1);
        set_engine_head_field<ExecutionContext*>(JS_LAYOUT_VM_RUNNING_EXECUTION_CONTEXT_OFFSET, &execution_context);
    }

    ExecutionContext* pop_execution_context()
    {
        auto length = execution_context_stack_length();
        VERIFY(length > 0);
        auto const& entry = execution_context_stack_entries()[length - 1];
        set_engine_head_field<size_t>(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_LENGTH_OFFSET, length - 1);
        set_engine_head_field<ExecutionContext*>(JS_LAYOUT_VM_RUNNING_EXECUTION_CONTEXT_OFFSET, entry.previous_running_execution_context);
        return entry.execution_context;
    }

    // https://tc39.es/ecma262/#running-execution-context
    // At any point in time, there is at most one execution context per agent that is actually executing code.
    // This is known as the agent's running execution context.
    ExecutionContext& running_execution_context()
    {
        auto* execution_context = running_execution_context_or_null();
        VERIFY(execution_context);
        return *execution_context;
    }
    ExecutionContext const& running_execution_context() const
    {
        auto const* execution_context = running_execution_context_or_null();
        VERIFY(execution_context);
        return *execution_context;
    }

    bool has_running_execution_context() const { return running_execution_context_or_null() != nullptr; }

    // https://tc39.es/ecma262/#execution-context-stack
    // What LibJS's users need of the execution context stack is how many contexts are on it.
    class ExecutionContextStack {
    public:
        size_t size() const { return m_vm.execution_context_stack_length(); }
        bool is_empty() const { return size() == 0; }

    private:
        friend class VM;

        explicit ExecutionContextStack(VM const& vm)
            : m_vm(vm)
        {
        }

        VM const& m_vm;
    };
    ExecutionContextStack execution_context_stack() const { return ExecutionContextStack { *this }; }

    // Calls the callback with each execution context from the running one down, the frames of JavaScript functions
    // that the interpreter calls directly included, until the callback returns false. This is the runtime's own walk:
    // from a context that is on the execution context stack, it goes on to the context that was running when that one
    // was pushed, and from any other context to its caller frame. The stack is read anew at every step, so the
    // callback may push and pop contexts as long as it leaves the stack as it found it.
    template<typename Callback>
    void for_each_execution_context_top_to_bottom(Callback callback) const
    {
        auto stack_index = execution_context_stack_length();
        auto* execution_context = running_execution_context_or_null();
        if (!execution_context) {
            while (stack_index > 0) {
                --stack_index;
                if (stack_index >= execution_context_stack_length())
                    return;
                if (!callback(*execution_context_stack_entries()[stack_index].execution_context))
                    return;
            }
            return;
        }
        while (true) {
            if (!callback(*execution_context))
                return;
            auto* next_execution_context = execution_context->caller_frame;
            if (stack_index > 0 && stack_index - 1 < execution_context_stack_length() && execution_context_stack_entries()[stack_index - 1].execution_context == execution_context) {
                --stack_index;
                next_execution_context = execution_context_stack_entries()[stack_index].previous_running_execution_context;
            }
            if (!next_execution_context)
                return;
            execution_context = next_execution_context;
        }
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

    Environment const* lexical_environment() const { return running_execution_context().lexical_environment.ptr(); }
    Environment* lexical_environment() { return running_execution_context().lexical_environment.ptr(); }

    Environment const* variable_environment() const { return running_execution_context().variable_environment.ptr(); }
    Environment* variable_environment() { return running_execution_context().variable_environment.ptr(); }

    // https://tc39.es/ecma262/#current-realm
    // The value of the Realm component of the running execution context is also called the current Realm Record.
    Realm const* current_realm() const { return running_execution_context().realm.ptr(); }
    Realm* current_realm() { return running_execution_context().realm.ptr(); }

    // https://tc39.es/ecma262/#active-function-object
    // The value of the Function component of the running execution context is also called the active function object.
    FunctionObject const* active_function_object() const { return running_execution_context().function.ptr(); }
    FunctionObject* active_function_object() { return running_execution_context().function.ptr(); }

    size_t argument_count() const { return running_execution_context().argument_count; }
    Value argument(size_t index) const { return running_execution_context().argument(index); }
    Value this_value() const { return running_execution_context().this_value.value(); }

    InterpreterStack& interpreter_stack() { return m_interpreter_stack; }

    // Ends the synchronous run of code during which the targets of the WeakRefs it dereferenced stay alive.
    void finish_execution_generation();

    // 9.4.2 ResolveBinding ( name [ , env ] ), https://tc39.es/ecma262/#sec-resolvebinding
    ThrowCompletionOr<Reference> resolve_binding(Utf16FlyString const&, Strict, GC::Ptr<Environment> = nullptr);

    // Has the TypeErrors that are thrown at the execution context stack depth the scope was created at created in
    // `realm`, until the scope is restored. Callees that push execution contexts are unaffected.
    class TypeErrorRealmScope {
        AK_MAKE_NONCOPYABLE(TypeErrorRealmScope);
        AK_MAKE_NONMOVABLE(TypeErrorRealmScope);

    public:
        TypeErrorRealmScope(VM& vm, Realm& realm)
            : m_vm(vm)
            , m_previous_realm(vm.engine_head_field<Realm*>(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_OFFSET))
            , m_previous_depth(vm.engine_head_field<size_t>(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_DEPTH_OFFSET))
        {
            vm.set_engine_head_field<Realm*>(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_OFFSET, &realm);
            vm.set_engine_head_field<size_t>(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_DEPTH_OFFSET, vm.execution_context_stack_length());
        }

        ~TypeErrorRealmScope()
        {
            restore();
        }

        void restore()
        {
            if (!m_active)
                return;
            m_vm.set_engine_head_field<Realm*>(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_OFFSET, m_previous_realm.ptr());
            m_vm.set_engine_head_field<size_t>(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_DEPTH_OFFSET, m_previous_depth);
            m_active = false;
        }

    private:
        static_assert(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_SIZE == sizeof(Realm*));
        static_assert(JS_LAYOUT_VM_TYPE_ERROR_REALM_OVERRIDE_DEPTH_SIZE == sizeof(size_t));

        VM& m_vm;
        GC::Ptr<Realm> m_previous_realm;
        size_t m_previous_depth { 0 };
        bool m_active { true };
    };

    // 5.2.3.2 Throw an Exception, https://tc39.es/ecma262/#sec-throw-an-exception
    // The runtime creates the errors of its own constructors, in the current realm or the realm of a TypeErrorRealmScope.
    template<typename T, typename... Args>
    COLD Completion throw_completion(Args&&... args)
    {
        if constexpr (IsEngineErrorType<T>) {
            static_assert(sizeof...(Args) == 1, "The runtime's errors are thrown with a message");
            return throw_engine_error(EngineErrorKindOf<T>::engine_error_kind, forward<Args>(args)...);
        } else {
            return JS::throw_completion(T::create(*current_realm(), forward<Args>(args)...));
        }
    }

    template<typename T>
    COLD Completion throw_completion(ErrorType const& type)
    {
        if constexpr (IsEngineErrorType<T>) {
            return throw_engine_error(EngineErrorKindOf<T>::engine_error_kind, type.message());
        } else {
            auto& realm = *current_realm();
            if constexpr (requires { T::create(realm, type.message()); })
                return throw_completion<T>(type.message());
            else
                return throw_completion<T>(Utf16String::from_utf16(type.message()));
        }
    }

    template<typename T, typename... Args>
    COLD Completion throw_completion(ErrorType const& type, Args&&... args)
    {
        return throw_completion<T>(Utf16String::formatted(type.format(), forward<Args>(args)...));
    }

    CommonPropertyNames names;

    Function<void(Object const&, PropertyKey const&)> on_unimplemented_property_access;

    void set_agent(OwnPtr<Agent>);
    Agent* agent() { return m_agent; }
    Agent const* agent() const { return m_agent; }

    void save_execution_context_stack();
    void clear_execution_context_stack();
    void restore_execution_context_stack();

    ScriptOrModule get_active_script_or_module() const;

    // The host hooks, which start out with the runtime's own behavior.

    // 16.2.1.10 HostLoadImportedModule ( referrer, moduleRequest, hostDefined, payload ), https://tc39.es/ecma262/#sec-HostLoadImportedModule
    Function<void(ImportedModuleReferrer, ModuleRequest const&, GC::Ptr<GC::Cell> load_state, ImportedModulePayload)> host_load_imported_module;

    Function<HashMap<PropertyKey, Value>(SourceTextModule&)> host_get_import_meta_properties;

    Function<Vector<Utf16String>()> host_get_supported_import_attributes;

    Function<void(Promise&, Promise::RejectionOperation)> host_promise_rejection_tracker;
    Function<ThrowCompletionOr<Value>(JobCallback&, Value, ReadonlySpan<Value>)> host_call_job_callback;
    Function<void(FinalizationRegistry&)> host_enqueue_finalization_registry_cleanup_job;
    Function<void(PromiseJob, GC::Ptr<Realm>)> host_enqueue_promise_job;
    Function<GC::Ref<JobCallback>(FunctionObject&)> host_make_job_callback;
    Function<GC::Ptr<PrimitiveString>(Object const&)> host_get_code_for_eval;
    Function<ThrowCompletionOr<void>(Realm&, ReadonlySpan<Utf16String>, Utf16View, Utf16View, CompilationType, ReadonlySpan<Value>, Value)> host_ensure_can_compile_strings;
    Function<ThrowCompletionOr<void>(Object&)> host_ensure_can_add_private_element;
    Function<ThrowCompletionOr<HandledByHost>(ArrayBuffer&, size_t)> host_resize_array_buffer;
    Function<ThrowCompletionOr<HandledByHost>(ArrayBuffer&, size_t)> host_grow_shared_array_buffer;
    Function<void(Utf16View)> host_unrecognized_date_string;
    Function<bool()> host_promise_job_queue_is_empty;

    // The frames of the execution context stack from the running one down, with where in its source code each one is.
    [[nodiscard]] Vector<StackTraceElement> stack_trace() const;

private:
    friend class InterpreterStack;
    friend struct HostHookThunks;

    enum class WellKnownSymbol : u8 {
#define __JS_ENUMERATE(SymbolName, snake_name) snake_name,
        JS_ENUMERATE_WELL_KNOWN_SYMBOLS
#undef __JS_ENUMERATE
    };

    using ErrorMessages = AK::Array<Utf16String, to_underlying(ErrorMessage::__Count)>;

    explicit VM(ErrorMessages);

    void install_default_host_hooks();
    void clear_host_hooks();

    GC::Ref<Symbol> well_known_symbol(WellKnownSymbol) const;

    Completion throw_engine_error(EngineErrorKind, Utf16View message);
    Completion throw_engine_error(EngineErrorKind, Utf16String message);

    template<typename Field>
    Field engine_head_field(size_t offset) const
    {
        Field field;
        __builtin_memcpy(&field, m_engine_storage + offset, sizeof(field));
        return field;
    }

    template<typename Field>
    void set_engine_head_field(size_t offset, Field field)
    {
        __builtin_memcpy(m_engine_storage + offset, &field, sizeof(field));
    }

    ExecutionContext* running_execution_context_or_null() const
    {
        static_assert(JS_LAYOUT_VM_RUNNING_EXECUTION_CONTEXT_SIZE == sizeof(ExecutionContext*));
        return engine_head_field<ExecutionContext*>(JS_LAYOUT_VM_RUNNING_EXECUTION_CONTEXT_OFFSET);
    }

    size_t execution_context_stack_length() const
    {
        static_assert(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_LENGTH_SIZE == sizeof(size_t));
        return engine_head_field<size_t>(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_LENGTH_OFFSET);
    }

    size_t execution_context_stack_capacity() const
    {
        static_assert(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_CAPACITY_SIZE == sizeof(size_t));
        return engine_head_field<size_t>(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_CAPACITY_OFFSET);
    }

    // An entry of the runtime's execution context stack: a context pushed onto it, and the context that was running
    // when it was pushed.
    struct ExecutionContextStackEntry {
        ExecutionContext* execution_context;
        ExecutionContext* previous_running_execution_context;
    };
    static_assert(sizeof(ExecutionContextStackEntry) == JS_LAYOUT_EXECUTION_CONTEXT_STACK_ENTRY_SIZE);
    static_assert(__builtin_offsetof(ExecutionContextStackEntry, execution_context) == JS_LAYOUT_EXECUTION_CONTEXT_STACK_ENTRY_EXECUTION_CONTEXT_OFFSET);
    static_assert(__builtin_offsetof(ExecutionContextStackEntry, previous_running_execution_context) == JS_LAYOUT_EXECUTION_CONTEXT_STACK_ENTRY_PREVIOUS_RUNNING_EXECUTION_CONTEXT_OFFSET);

    ExecutionContextStackEntry* execution_context_stack_entries() const
    {
        static_assert(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_ENTRIES_SIZE == sizeof(ExecutionContextStackEntry*));
        return engine_head_field<ExecutionContextStackEntry*>(JS_LAYOUT_VM_EXECUTION_CONTEXT_STACK_ENTRIES_OFFSET);
    }

    void push_execution_context_growing_the_stack(ExecutionContext&);

    static VM* s_the;

    mutable u32 m_reference_count { 1 };
    GC::Heap* m_heap { nullptr };
    InterpreterStack m_interpreter_stack { *this };
    ErrorMessages m_error_messages;
    OwnPtr<Agent> m_agent;
};

template<typename GlobalObjectType, typename... Args>
[[nodiscard]] static NonnullOwnPtr<ExecutionContext> create_simple_execution_context(VM& vm, [[maybe_unused]] Args&&... args)
{
    static_assert(IsSame<GlobalObjectType, GlobalObject> && sizeof...(Args) == 0, "The runtime creates the ordinary global object; a host global object comes from a callback of Realm::initialize_host_defined_realm()");
    return MUST(Realm::initialize_host_defined_realm(vm, nullptr, nullptr));
}

ALWAYS_INLINE VM& Cell::vm() const { return VM::the(); }
ALWAYS_INLINE VM& EngineCell::vm() const { return VM::the(); }

}
