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
#include <LibGC/ConservativeVector.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibWasm/Export.h>

namespace Wasm::FFI {

struct WasmModule;
struct WasmStore;
struct WasmWasi;

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

// A function, table, memory, global, tag, or instance: an entity of the store, which is a cell of the store's heap.
// The store keeps no entity alive: an entity lives while something visits or roots its cell, or while another
// entity that refers to it lives (an instance refers to what it defines and imports, a function to its instance).
// Embedders pass the store only entities it made, or the runtime aborts.
using Entity = GC::ForeignCell*;

struct Extern {
    ExternKind kind { ExternKind::Function };
    GC::RawPtr<GC::ForeignCell> entity;
};

struct Limits {
    u64 minimum { 0 };
    u64 maximum { 0 };
    bool has_maximum { false };
    bool address64 { false };
    bool shared { false };
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

struct InstanceExport {
    StringView name;
    Extern value;
};

// Values that may be references to cells, which the garbage collector finds while the vector lives.
using Values = GC::ConservativeVector<Value, 8>;

// A compiled module, which can be shared across threads and instantiated in any store.
class WASM_API Module : public AtomicRefCounted<Module> {
public:
    static ErrorOr<NonnullRefPtr<Module>, ByteString> compile(ReadonlyBytes);
    ~Module();

    size_t import_count() const;
    Import import(size_t index) const;
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
    // An exception was thrown and not caught: `exception` refers to it.
    Exception,
    StackExhausted,
};

struct Failure {
    FailureKind kind { FailureKind::Trap };
    ByteString message;
    u64 exception { 0 };
};

// How a host function fails: it traps with a message.
struct HostError {
    HostError(ByteString message)
        : message(move(message))
    {
    }

    ByteString message;
};

// What the entities of one thread share: their heap, types, and the interpreter's state. One store serves a whole
// agent.
class WASM_API Store {
    AK_MAKE_NONCOPYABLE(Store);
    AK_MAKE_NONMOVABLE(Store);
    AK_ALLOC_WITH_KMALLOC;

public:
    // Entities and GC objects live in the heap, and memories in LibGC's primitive storage.
    explicit Store(GC::Heap&);
    ~Store();

    // A host function writes one result for each result of its type, or returns an error to trap or throw. Its
    // function visits the cells it captures, so it must not capture roots.
    using HostFunction = Function<ErrorOr<void, HostError>(Store&, ReadonlySpan<Value> arguments, Span<Value> results)>;

    Entity allocate_host_function(FunctionType const&, HostFunction);
    Optional<Entity> allocate_memory(Limits);
    Optional<Entity> allocate_table(ValueType element, Limits, u64 initial);
    Entity allocate_global(ValueType, bool is_mutable, Value);

    // Returns the instance.
    ErrorOr<Entity, Failure> instantiate(Module const&, ReadonlySpan<Extern> imports);
    Vector<InstanceExport> instance_exports(Entity instance);
    Optional<Extern> find_export(Entity instance, StringView name);

    FunctionType function_type(Entity function);
    ErrorOr<Values, Failure> invoke(Entity function, ReadonlySpan<Value> arguments);
    u64 function_reference(Entity function);
    // The function a non-null function reference refers to.
    Entity function_from_reference(u64 reference);

    ValueType global_type(Entity global, bool& is_mutable);
    Value global_get(Entity global);

    FFI::WasmStore* handle() { return m_store; }

private:
    FFI::WasmStore* m_store { nullptr };
};

// The state of a WASI (wasi_snapshot_preview1) command. Its functions only work on Unix hosts.
class WASM_API Wasi {
    AK_MAKE_NONCOPYABLE(Wasi);
    AK_MAKE_NONMOVABLE(Wasi);
    AK_ALLOC_WITH_KMALLOC;

public:
    struct Preopen {
        ByteString host_path;
        ByteString guest_path;
    };

    // The environment is a list of NAME=value strings.
    Wasi(ReadonlySpan<ByteString> arguments, ReadonlySpan<ByteString> environment, ReadonlySpan<Preopen> preopens);
    ~Wasi();

    static constexpr StringView module_name = "wasi_snapshot_preview1"sv;

    // Add the WASI function `name` to the store, if there is one.
    Optional<Entity> allocate_function(Store&, StringView name);
    // Have the functions work on `memory`, usually the one the command exports.
    void set_memory(Entity memory);
    // The code the command passed to proc_exit, if it called it.
    Optional<i32> exit_code() const;

private:
    FFI::WasmWasi* m_wasi { nullptr };
};

}
