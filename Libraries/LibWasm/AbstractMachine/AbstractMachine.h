/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StackInfo.h>
#include <AK/UFixedBigInt.h>
#include <LibWasm/Types.h>

// NOTE: Special case for Wasm::Result.
#include <LibJS/Runtime/Completion.h>

namespace Wasm {

class Configuration;
class Result;
struct Interpreter;

struct InstantiationError {
    ByteString error { "Unknown error" };
};
struct LinkError {
    enum OtherErrors {
        InvalidImportedModule,
    };
    Vector<ByteString> missing_imports;
    Vector<OtherErrors> other_errors;
};

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, FunctionAddress, Arithmetic, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, ExternAddress, Arithmetic, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, TableAddress, Arithmetic, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, GlobalAddress, Arithmetic, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, ElementAddress, Arithmetic, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, DataAddress, Arithmetic, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, MemoryAddress, Arithmetic, Comparison, Increment);

// FIXME: These should probably be made generic/virtual if/when we decide to do something more
//        fancy than just a dumb interpreter.
class Reference {
public:
    struct Null {
        ValueType type;
    };
    struct Func {
        FunctionAddress address;
        RefPtr<Module> source_module; // null if host function.
    };
    struct Extern {
        ExternAddress address;
    };

    using RefType = Variant<Null, Func, Extern>;
    explicit Reference(RefType ref)
        : m_ref(move(ref))
    {
    }
    explicit Reference()
        : m_ref(Reference::Null { ValueType(ValueType::Kind::FunctionReference) })
    {
    }

    auto& ref() const { return m_ref; }

private:
    RefType m_ref;
};

class Value {
public:
    explicit Value(ValueType type)
        : m_value(u128())
    {
        switch (type.kind()) {
        case ValueType::I32:
        case ValueType::I64:
        case ValueType::F32:
        case ValueType::F64:
        case ValueType::V128:
            break;
        case ValueType::FunctionReference:
            // ref.null funcref
            m_value = u128(0, 2);
            break;
        case ValueType::ExternReference:
            // ref.null externref
            m_value = u128(0, 3);
            break;
        }
    }

    template<typename T>
    requires(sizeof(T) == sizeof(u64)) explicit Value(T raw_value)
        : m_value(u128(bit_cast<i64>(raw_value), 0))
    {
    }

    template<typename T>
    requires(sizeof(T) == sizeof(u32)) explicit Value(T raw_value)
        : m_value(u128(static_cast<i64>(bit_cast<i32>(raw_value)), 0))
    {
    }

    template<typename T>
    requires(sizeof(T) == sizeof(u8) && Signed<T>) explicit Value(T raw_value)
        : m_value(u128(static_cast<i64>(bit_cast<i8>(raw_value)), 0))
    {
    }

    template<typename T>
    requires(sizeof(T) == sizeof(u8) && Unsigned<T>) explicit Value(T raw_value)
        : m_value(u128(static_cast<u64>(bit_cast<u8>(raw_value)), 0))
    {
    }

    template<typename T>
    requires(sizeof(T) == sizeof(u16) && Signed<T>) explicit Value(T raw_value)
        : m_value(u128(static_cast<i64>(bit_cast<i16>(raw_value)), 0))
    {
    }

    template<typename T>
    requires(sizeof(T) == sizeof(u16) && Unsigned<T>) explicit Value(T raw_value)
        : m_value(u128(static_cast<u64>(bit_cast<u16>(raw_value)), 0))
    {
    }

    explicit Value(Reference ref)
    {
        // Reference variant is encoded in the high storage of the u128:
        // 0: funcref
        // 1: externref
        // 2: null funcref
        // 3: null externref
        ref.ref().visit(
            [&](Reference::Func const& func) { m_value = u128(bit_cast<u64>(func.address), bit_cast<u64>(func.source_module.ptr())); },
            [&](Reference::Extern const& func) { m_value = u128(bit_cast<u64>(func.address), 1); },
            [&](Reference::Null const& null) { m_value = u128(0, null.type.kind() == ValueType::Kind::FunctionReference ? 2 : 3); });
    }

    template<SameAs<u128> T>
    explicit Value(T raw_value)
        : m_value(raw_value)
    {
    }

    ALWAYS_INLINE Value(Value const& value) = default;
    ALWAYS_INLINE Value(Value&& value) = default;
    ALWAYS_INLINE Value& operator=(Value&& value) = default;
    ALWAYS_INLINE Value& operator=(Value const& value) = default;

    template<typename T>
    ALWAYS_INLINE T to() const
    {
        static_assert(IsOneOf<T, u128, u64, i64, f32, f64, Reference> || IsIntegral<T>, "Unsupported type for Value::to()");

        if constexpr (IsSame<T, u128>) {
            return m_value;
        }
        if constexpr (IsOneOf<T, u64, i64>) {
            return bit_cast<T>(m_value.low());
        }
        if constexpr (IsIntegral<T> && sizeof(T) < 8) {
            return bit_cast<T>(static_cast<MakeUnsigned<T>>(m_value.low() & NumericLimits<MakeUnsigned<T>>::max()));
        }
        if constexpr (IsSame<T, f32>) {
            u32 low = m_value.low() & 0xFFFFFFFF;
            return bit_cast<f32>(low);
        }
        if constexpr (IsSame<T, f64>) {
            return bit_cast<f64>(m_value.low());
        }
        if constexpr (IsSame<T, Reference>) {
            switch (m_value.high() & 3) {
            case 0:
                return Reference { Reference::Func { bit_cast<FunctionAddress>(m_value.low()), bit_cast<Wasm::Module*>(m_value.high()) } };
            case 1:
                return Reference { Reference::Extern { bit_cast<ExternAddress>(m_value.low()) } };
            case 2:
                return Reference { Reference::Null { ValueType(ValueType::Kind::FunctionReference) } };
            case 3:
                return Reference { Reference::Null { ValueType(ValueType::Kind::ExternReference) } };
            }
        }
        VERIFY_NOT_REACHED();
    }

    auto& value() const { return m_value; }

private:
    u128 m_value;
};

struct Trap {
    ByteString reason;
};

// A variant of Result that does not include external reasons for error (JS::Completion, for now).
class PureResult {
public:
    explicit PureResult(Vector<Value> values)
        : m_result(move(values))
    {
    }

    PureResult(Trap trap)
        : m_result(move(trap))
    {
    }

    auto is_trap() const { return m_result.has<Trap>(); }
    auto& values() const { return m_result.get<Vector<Value>>(); }
    auto& values() { return m_result.get<Vector<Value>>(); }
    auto& trap() const { return m_result.get<Trap>(); }
    auto& trap() { return m_result.get<Trap>(); }

private:
    friend class Result;
    explicit PureResult(Variant<Vector<Value>, Trap>&& result)
        : m_result(move(result))
    {
    }

    Variant<Vector<Value>, Trap> m_result;
};

class Result {
public:
    explicit Result(Vector<Value> values)
        : m_result(move(values))
    {
    }

    Result(Trap trap)
        : m_result(move(trap))
    {
    }

    Result(JS::Completion completion)
        : m_result(move(completion))
    {
        VERIFY(m_result.get<JS::Completion>().is_abrupt());
    }

    Result(PureResult&& result)
        : m_result(result.m_result.downcast<decltype(m_result)>())
    {
    }

    auto is_trap() const { return m_result.has<Trap>(); }
    auto is_completion() const { return m_result.has<JS::Completion>(); }
    auto& values() const { return m_result.get<Vector<Value>>(); }
    auto& values() { return m_result.get<Vector<Value>>(); }
    auto& trap() const { return m_result.get<Trap>(); }
    auto& trap() { return m_result.get<Trap>(); }
    auto& completion() { return m_result.get<JS::Completion>(); }
    auto& completion() const { return m_result.get<JS::Completion>(); }

    PureResult assert_wasm_result() &&
    {
        VERIFY(!is_completion());
        return PureResult(move(m_result).downcast<Vector<Value>, Trap>());
    }

private:
    Variant<Vector<Value>, Trap, JS::Completion> m_result;
};

using ExternValue = Variant<FunctionAddress, TableAddress, MemoryAddress, GlobalAddress>;

class ExportInstance {
public:
    explicit ExportInstance(ByteString name, ExternValue value)
        : m_name(move(name))
        , m_value(move(value))
    {
    }

    auto& name() const { return m_name; }
    auto& value() const { return m_value; }

private:
    ByteString m_name;
    ExternValue m_value;
};

class ModuleInstance {
public:
    explicit ModuleInstance(
        Vector<FunctionType> types, Vector<FunctionAddress> function_addresses, Vector<TableAddress> table_addresses,
        Vector<MemoryAddress> memory_addresses, Vector<GlobalAddress> global_addresses, Vector<DataAddress> data_addresses,
        Vector<ExportInstance> exports)
        : m_types(move(types))
        , m_functions(move(function_addresses))
        , m_tables(move(table_addresses))
        , m_memories(move(memory_addresses))
        , m_globals(move(global_addresses))
        , m_datas(move(data_addresses))
        , m_exports(move(exports))
    {
    }

    ModuleInstance() = default;

    auto& types() const { return m_types; }
    auto& functions() const { return m_functions; }
    auto& tables() const { return m_tables; }
    auto& memories() const { return m_memories; }
    auto& globals() const { return m_globals; }
    auto& elements() const { return m_elements; }
    auto& datas() const { return m_datas; }
    auto& exports() const { return m_exports; }

    auto& types() { return m_types; }
    auto& functions() { return m_functions; }
    auto& tables() { return m_tables; }
    auto& memories() { return m_memories; }
    auto& globals() { return m_globals; }
    auto& elements() { return m_elements; }
    auto& datas() { return m_datas; }
    auto& exports() { return m_exports; }

private:
    Vector<FunctionType> m_types;
    Vector<FunctionAddress> m_functions;
    Vector<TableAddress> m_tables;
    Vector<MemoryAddress> m_memories;
    Vector<GlobalAddress> m_globals;
    Vector<ElementAddress> m_elements;
    Vector<DataAddress> m_datas;
    Vector<ExportInstance> m_exports;
};

class WasmFunction {
public:
    explicit WasmFunction(FunctionType const& type, ModuleInstance const& instance, Module const& module, CodeSection::Code const& code)
        : m_type(type)
        , m_module(module.make_weak_ptr())
        , m_module_instance(instance)
        , m_code(code)
    {
    }

    auto& type() const { return m_type; }
    auto& module() const { return m_module_instance; }
    auto& code() const { return m_code; }
    RefPtr<Module const> module_ref() const { return m_module.strong_ref(); }

private:
    FunctionType m_type;
    WeakPtr<Module const> m_module;
    ModuleInstance const& m_module_instance;
    CodeSection::Code const& m_code;
};

class HostFunction {
public:
    explicit HostFunction(AK::Function<Result(Configuration&, Vector<Value>&)> function, FunctionType const& type, ByteString name)
        : m_function(move(function))
        , m_type(type)
        , m_name(move(name))
    {
    }

    auto& function() { return m_function; }
    auto& type() const { return m_type; }
    auto& name() const { return m_name; }

private:
    AK::Function<Result(Configuration&, Vector<Value>&)> m_function;
    FunctionType m_type;
    ByteString m_name;
};

using FunctionInstance = Variant<WasmFunction, HostFunction>;

class TableInstance {
public:
    explicit TableInstance(TableType const& type, Vector<Reference> elements)
        : m_elements(move(elements))
        , m_type(type)
    {
    }

    auto& elements() const { return m_elements; }
    auto& elements() { return m_elements; }
    auto& type() const { return m_type; }

    bool grow(u32 size_to_grow, Reference const& fill_value)
    {
        if (size_to_grow == 0)
            return true;
        size_t new_size = m_elements.size() + size_to_grow;
        if (auto max = m_type.limits().max(); max.has_value()) {
            if (max.value() < new_size)
                return false;
        }
        if (new_size >= NumericLimits<u32>::max()) {
            return false;
        }
        auto previous_size = m_elements.size();
        if (m_elements.try_resize(new_size).is_error())
            return false;
        for (size_t i = previous_size; i < m_elements.size(); ++i)
            m_elements[i] = fill_value;

        m_type = TableType { m_type.element_type(), Limits(m_type.limits().min() + size_to_grow, m_type.limits().max()) };

        return true;
    }

private:
    Vector<Reference> m_elements;
    TableType m_type;
};

class MemoryInstance {
public:
    static ErrorOr<MemoryInstance> create(MemoryType const& type)
    {
        MemoryInstance instance { type };

        if (!instance.grow(type.limits().min() * Constants::page_size, GrowType::No))
            return Error::from_string_literal("Failed to grow to requested size");

        return { move(instance) };
    }

    auto& type() const { return m_type; }
    auto size() const { return m_size; }
    auto& data() const { return m_data; }
    auto& data() { return m_data; }

    enum class InhibitGrowCallback {
        No,
        Yes,
    };

    enum class GrowType {
        No,
        Yes,
    };

    bool grow(size_t size_to_grow, GrowType grow_type = GrowType::Yes, InhibitGrowCallback inhibit_callback = InhibitGrowCallback::No)
    {
        if (size_to_grow == 0)
            return true;
        u64 new_size = m_data.size() + size_to_grow;
        // Can't grow past 2^16 pages.
        if (new_size >= Constants::page_size * 65536)
            return false;
        if (auto max = m_type.limits().max(); max.has_value()) {
            if (max.value() * Constants::page_size < new_size)
                return false;
        }
        auto previous_size = m_size;
        if (m_data.try_resize(new_size).is_error())
            return false;
        m_size = new_size;
        // The spec requires that we zero out everything on grow
        __builtin_memset(m_data.offset_pointer(previous_size), 0, size_to_grow);

        // NOTE: This exists because wasm-js-api wants to execute code after a successful grow,
        //       See [this issue](https://github.com/WebAssembly/spec/issues/1635) for more details.
        if (inhibit_callback == InhibitGrowCallback::No && successful_grow_hook)
            successful_grow_hook();

        if (grow_type == GrowType::Yes) {
            // Grow the memory's type. We do this when encountering a `memory.grow`.
            //
            // See relevant spec link:
            // https://www.w3.org/TR/wasm-core-2/#growing-memories%E2%91%A0
            m_type = MemoryType { Limits(m_type.limits().min() + size_to_grow / Constants::page_size, m_type.limits().max()) };
        }

        return true;
    }

    Function<void()> successful_grow_hook;

private:
    explicit MemoryInstance(MemoryType const& type)
        : m_type(type)
    {
    }

    MemoryType m_type;
    size_t m_size { 0 };
    ByteBuffer m_data;
};

class GlobalInstance {
public:
    explicit GlobalInstance(Value value, bool is_mutable, ValueType type)
        : m_mutable(is_mutable)
        , m_value(value)
        , m_type(type)
    {
    }

    auto is_mutable() const { return m_mutable; }
    auto& value() const { return m_value; }
    GlobalType type() const { return { m_type, is_mutable() }; }
    void set_value(Value value)
    {
        VERIFY(is_mutable());
        m_value = move(value);
    }

private:
    bool m_mutable { false };
    Value m_value;
    ValueType m_type;
};

class DataInstance {
public:
    explicit DataInstance(Vector<u8> data)
        : m_data(move(data))
    {
    }

    size_t size() const { return m_data.size(); }

    Vector<u8>& data() { return m_data; }
    Vector<u8> const& data() const { return m_data; }

private:
    Vector<u8> m_data;
};

class ElementInstance {
public:
    explicit ElementInstance(ValueType type, Vector<Reference> references)
        : m_type(move(type))
        , m_references(move(references))
    {
    }

    auto& type() const { return m_type; }
    auto& references() const { return m_references; }

private:
    ValueType m_type;
    Vector<Reference> m_references;
};

class Store {
public:
    Store() = default;

    Optional<FunctionAddress> allocate(ModuleInstance&, Module const&, CodeSection::Code const&, TypeIndex);
    Optional<FunctionAddress> allocate(HostFunction&&);
    Optional<TableAddress> allocate(TableType const&);
    Optional<MemoryAddress> allocate(MemoryType const&);
    Optional<DataAddress> allocate_data(Vector<u8>);
    Optional<GlobalAddress> allocate(GlobalType const&, Value);
    Optional<ElementAddress> allocate(ValueType const&, Vector<Reference>);

    Module const* get_module_for(FunctionAddress);
    FunctionInstance* get(FunctionAddress);
    TableInstance* get(TableAddress);
    MemoryInstance* get(MemoryAddress);
    GlobalInstance* get(GlobalAddress);
    DataInstance* get(DataAddress);
    ElementInstance* get(ElementAddress);

private:
    Vector<FunctionInstance> m_functions;
    Vector<TableInstance> m_tables;
    Vector<MemoryInstance> m_memories;
    Vector<GlobalInstance> m_globals;
    Vector<ElementInstance> m_elements;
    Vector<DataInstance> m_datas;
};

class Label {
public:
    explicit Label(size_t arity, InstructionPointer continuation, size_t stack_height)
        : m_arity(arity)
        , m_stack_height(stack_height)
        , m_continuation(continuation)
    {
    }

    auto continuation() const { return m_continuation; }
    auto arity() const { return m_arity; }
    auto stack_height() const { return m_stack_height; }

private:
    size_t m_arity { 0 };
    size_t m_stack_height { 0 };
    InstructionPointer m_continuation { 0 };
};

class Frame {
public:
    explicit Frame(ModuleInstance const& module, Vector<Value> locals, Expression const& expression, size_t arity)
        : m_module(module)
        , m_locals(move(locals))
        , m_expression(expression)
        , m_arity(arity)
    {
    }

    auto& module() const { return m_module; }
    auto& locals() const { return m_locals; }
    auto& locals() { return m_locals; }
    auto& expression() const { return m_expression; }
    auto arity() const { return m_arity; }
    auto label_index() const { return m_label_index; }
    auto& label_index() { return m_label_index; }

private:
    ModuleInstance const& m_module;
    Vector<Value> m_locals;
    Expression const& m_expression;
    size_t m_arity { 0 };
    size_t m_label_index { 0 };
};

using InstantiationResult = AK::ErrorOr<NonnullOwnPtr<ModuleInstance>, InstantiationError>;

class AbstractMachine {
public:
    explicit AbstractMachine() = default;

    // Validate a module; permanently sets the module's validity status.
    ErrorOr<void, ValidationError> validate(Module&);
    // Load and instantiate a module, and link it into this interpreter.
    InstantiationResult instantiate(Module const&, Vector<ExternValue>);
    Result invoke(FunctionAddress, Vector<Value>);
    Result invoke(Interpreter&, FunctionAddress, Vector<Value>);

    auto& store() const { return m_store; }
    auto& store() { return m_store; }

    void enable_instruction_count_limit() { m_should_limit_instruction_count = true; }

private:
    Optional<InstantiationError> allocate_all_initial_phase(Module const&, ModuleInstance&, Vector<ExternValue>&, Vector<Value>& global_values, Vector<FunctionAddress>& own_functions);
    Optional<InstantiationError> allocate_all_final_phase(Module const&, ModuleInstance&, Vector<Vector<Reference>>& elements);
    Store m_store;
    StackInfo m_stack_info;
    bool m_should_limit_instruction_count { false };
};

class Linker {
public:
    struct Name {
        ByteString module;
        ByteString name;
        ImportSection::Import::ImportDesc type;
    };

    explicit Linker(Module const& module)
        : m_module(module)
    {
    }

    // Link a module, the import 'module name' is ignored with this.
    void link(ModuleInstance const&);

    // Link a bunch of qualified values, also matches 'module name'.
    void link(HashMap<Name, ExternValue> const&);

    auto& unresolved_imports()
    {
        populate();
        return m_unresolved_imports;
    }

    AK::ErrorOr<Vector<ExternValue>, LinkError> finish();

private:
    void populate();

    Module const& m_module;
    HashMap<Name, ExternValue> m_resolved_imports;
    HashTable<Name> m_unresolved_imports;
    Vector<Name> m_ordered_imports;
    Optional<LinkError> m_error;
};

}

template<>
struct AK::Traits<Wasm::Linker::Name> : public AK::DefaultTraits<Wasm::Linker::Name> {
    static constexpr bool is_trivial() { return false; }
    static unsigned hash(Wasm::Linker::Name const& entry) { return pair_int_hash(entry.module.hash(), entry.name.hash()); }
    static bool equals(Wasm::Linker::Name const& a, Wasm::Linker::Name const& b) { return a.name == b.name && a.module == b.module; }
};
