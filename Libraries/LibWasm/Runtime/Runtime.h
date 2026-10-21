/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/BitCast.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWasm/Runtime/Export.h>

namespace Wasm::FFI {

struct WasmModule;
struct WasmStore;

}

// The C++ interface of the WebAssembly engine, which is written in Rust (Libraries/LibWasm/Engine). These types mirror
// the ones of its C ABI (Libraries/LibWasm/Engine/src/capi.rs) exactly.
namespace Wasm::Runtime {

enum class ValueKind : u8 {
    I32,
    I64,
    F32,
    F64,
    V128,
    Reference,
};

enum class HeapKind : u8 {
    Function,
    NoFunction,
    Extern,
    NoExtern,
    Any,
    Eq,
    I31,
    Struct,
    Array,
    None,
    Exception,
    NoException,
    Concrete,
};

// A value type. `heap`, `nullable`, and `type_index` only matter for references; `type_index` indexes the types of
// the module the type comes from.
struct ValueType {
    ValueKind kind { ValueKind::I32 };
    HeapKind heap { HeapKind::Function };
    bool nullable { false };
    u32 type_index { 0 };

    static constexpr ValueType scalar(ValueKind kind) { return { kind, HeapKind::Function, false, 0 }; }
    static constexpr ValueType reference(HeapKind heap, bool nullable = true) { return { ValueKind::Reference, heap, nullable, 0 }; }

    bool is_reference() const { return kind == ValueKind::Reference; }
};

// A value. i32 and f32 bits are zero-extended in `low`, a v128 is `low` and `high`, and a reference is its bits, which
// are zero for null.
struct Value {
    ValueKind kind { ValueKind::I32 };
    u64 low { 0 };
    u64 high { 0 };

    static Value from_i32(i32 value) { return { ValueKind::I32, static_cast<u32>(value), 0 }; }
    static Value from_i64(i64 value) { return { ValueKind::I64, static_cast<u64>(value), 0 }; }
    static Value from_f32_bits(u32 bits) { return { ValueKind::F32, bits, 0 }; }
    static Value from_f64_bits(u64 bits) { return { ValueKind::F64, bits, 0 }; }
    static Value from_f32(float value) { return from_f32_bits(bit_cast<u32>(value)); }
    static Value from_f64(double value) { return from_f64_bits(bit_cast<u64>(value)); }
    static Value from_v128(u64 low, u64 high) { return { ValueKind::V128, low, high }; }
    static Value from_reference(u64 bits) { return { ValueKind::Reference, bits, 0 }; }
    static Value default_for(ValueType type) { return { type.kind, 0, 0 }; }

    i32 to_i32() const { return static_cast<i32>(static_cast<u32>(low)); }
    i64 to_i64() const { return static_cast<i64>(low); }
    u32 f32_bits() const { return static_cast<u32>(low); }
    u64 f64_bits() const { return low; }
    float to_f32() const { return bit_cast<float>(f32_bits()); }
    double to_f64() const { return bit_cast<double>(f64_bits()); }
    u64 reference_bits() const { return low; }
    bool is_null_reference() const { return low == 0; }
};

enum class ExternKind : u8 {
    Function,
    Table,
    Memory,
    Global,
    Tag,
};

// A function, table, memory, or global of a store, by its address.
struct Extern {
    ExternKind kind { ExternKind::Function };
    u32 address { 0 };
};

struct Limits {
    u64 minimum { 0 };
    u64 maximum { 0 };
    bool has_maximum { false };
    bool address64 { false };
    bool shared { false };

    Optional<u64> maximum_if_any() const { return has_maximum ? Optional<u64> { maximum } : Optional<u64> {}; }
};

// What an import or export is: the type index of a function or tag, the limits of a memory or table, the element type
// of a table, and the value type and mutability of a global.
struct ExternType {
    ExternKind kind { ExternKind::Function };
    u32 type_index { 0 };
    Limits limits;
    ValueType value_type;
    bool is_mutable { false };
};

struct FunctionType {
    Vector<ValueType> parameters;
    Vector<ValueType> results;
};

struct Import {
    StringView module;
    StringView name;
    ExternType type;
};

struct ModuleExport {
    StringView name;
    ExternType type;
};

struct InstanceExport {
    StringView name;
    Extern value;
};

// A compiled module, which can be shared across threads and instantiated in any store.
class WASMRUNTIME_API Module : public AtomicRefCounted<Module> {
public:
    static ErrorOr<NonnullRefPtr<Module>, ByteString> compile(ReadonlyBytes);
    ~Module();

    size_t import_count() const;
    Import import(size_t index) const;
    size_t export_count() const;
    ModuleExport module_export(size_t index) const;
    Optional<FunctionType> function_type(u32 type_index) const;

    FFI::WasmModule const* handle() const { return m_module; }

private:
    explicit Module(FFI::WasmModule const* module)
        : m_module(module)
    {
    }

    FFI::WasmModule const* m_module { nullptr };
};

enum class FailureKind : u8 {
    LinkError,
    Trap,
};

struct Failure {
    FailureKind kind { FailureKind::Trap };
    ByteString message;
};

// Functions, tables, memories, globals, and instances, all of one thread.
class WASMRUNTIME_API Store {
    AK_MAKE_NONCOPYABLE(Store);
    AK_MAKE_NONMOVABLE(Store);
    AK_ALLOC_WITH_KMALLOC;

public:
    Store();
    ~Store();

    // A host function writes one result for each result of its type, or returns an error to trap.
    using HostFunction = Function<ErrorOr<void, ByteString>(Store&, ReadonlySpan<Value> arguments, Span<Value> results)>;

    u32 allocate_host_function(FunctionType const&, HostFunction);
    Optional<u32> allocate_memory(Limits);
    u32 allocate_table(ValueType element, Limits, u64 initial);
    u32 allocate_global(ValueType, bool is_mutable, Value);

    ErrorOr<u32, Failure> instantiate(Module const&, ReadonlySpan<Extern> imports);
    Vector<InstanceExport> instance_exports(u32 instance);
    Optional<Extern> find_export(u32 instance, StringView name);

    FunctionType function_type(u32 function);
    ErrorOr<Vector<Value>, ByteString> invoke(u32 function, ReadonlySpan<Value> arguments);
    u64 function_reference(u32 function);
    Optional<u32> function_from_reference(u64 reference);

    // The bytes of a memory move when it grows.
    Bytes memory(u32 memory);
    // The previous size in pages, or -1 if the memory cannot grow.
    i64 grow_memory(u32 memory, u64 delta);
    Limits memory_limits(u32 memory);

    u64 table_size(u32 table);
    ValueType table_element_type(u32 table);
    Limits table_limits(u32 table);
    Optional<u64> table_get(u32 table, u64 index);
    bool table_set(u32 table, u64 index, u64 value);
    // The previous size, or -1 if the table cannot grow.
    i64 grow_table(u32 table, u64 delta, u64 initial);

    ValueType global_type(u32 global, bool& is_mutable);
    Value global_get(u32 global);
    void global_set(u32 global, Value);

    FFI::WasmStore* handle() { return m_store; }

private:
    FFI::WasmStore* m_store { nullptr };
};

}
