/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/EnvironmentShape.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

class SharedFunctionInstanceDataList;

struct NoSharedFunctionDataList {
};

// NB: This mirrors Identifier::Local from AST.h, defined here to avoid
//     including the full AST header in this file.
struct FunctionLocal {
    enum Type : u8 {
        None,
        Argument,
        Variable,
    };
    Type type { None };
    u32 index { 0 };

    bool is_argument() const { return type == Argument; }
    bool is_variable() const { return type == Variable; }
};

enum class ThisMode : u8 {
    Lexical,
    Strict,
    Global,
};

enum class ConstructorKind : u8 {
    Base,
    Derived,
};

class JS_API SharedFunctionInstanceData final : public GC::Cell {
    GC_CELL(SharedFunctionInstanceData, GC::Cell);
    GC_DECLARE_ALLOCATOR(SharedFunctionInstanceData);
    friend class SharedFunctionInstanceDataList;
    static constexpr bool OVERRIDES_FINALIZE = true;

public:
    IntrusiveListNode<SharedFunctionInstanceData> m_script_or_module_list_node;
    SharedFunctionInstanceDataList* m_owner_shared_function_data_list { nullptr };

    static constexpr u64 asm_call_metadata_can_inline_call = 1ull << 32;
    static constexpr u64 asm_call_metadata_needs_environment_or_this_value_resolution = 1ull << 33;
    static constexpr u64 asm_call_metadata_uses_this = 1ull << 34;
    static constexpr u64 asm_call_metadata_strict = 1ull << 35;

    virtual ~SharedFunctionInstanceData() override;
    virtual void finalize() override;

    SharedFunctionInstanceData(
        VM& vm,
        FunctionKind,
        Utf16FlyString name,
        i32 function_length,
        u32 formal_parameter_count,
        bool strict,
        bool is_arrow_function,
        bool has_simple_parameter_list,
        Vector<Utf16FlyString> parameter_names_for_mapped_arguments,
        SharedFunctionInstanceDataList&,
        void* rust_function_ast);

    SharedFunctionInstanceData(
        VM& vm,
        FunctionKind,
        Utf16FlyString name,
        i32 function_length,
        u32 formal_parameter_count,
        bool strict,
        bool is_arrow_function,
        bool has_simple_parameter_list,
        Vector<Utf16FlyString> parameter_names_for_mapped_arguments,
        NoSharedFunctionDataList,
        void* rust_function_ast);

    void set_executable(GC::Ptr<Bytecode::Executable>);
    void set_is_class_constructor();
    void update_asm_call_metadata();
    [[nodiscard]] bool can_inline_call() const { return m_can_inline_call; }

    mutable GC::Ptr<Bytecode::Executable> m_executable;
    u64 m_asm_call_metadata { 0 };

    Utf16FlyString m_name;

    Utf16String source_text() const;
    void set_source_text(Utf16View);
    void set_source_text_range(SourceCode const&, size_t source_text_offset, size_t source_text_length);

    // NB: m_source_text_offset and m_source_text_length normally refer to
    //     ranges within the underlying JS::SourceCode we parsed the AST from,
    //     kept alive by m_source_code. m_source_text_owner is used if the
    //     source text needs to be owned by the function data (e.g. for
    //     dynamically created functions via Function constructor).
    RefPtr<SourceCode const> m_source_code;
    Utf16String mutable m_source_text_owner;
    size_t m_source_text_offset { 0 };
    size_t m_source_text_length { 0 }; // [[SourceText]]

    // NB: Some runtime operations replace [[SourceText]] with a wider range
    //     (e.g. class constructors use the full class source). Keep the
    //     original parsed function range for bytecode cache identity.
    size_t m_bytecode_cache_source_text_offset { 0 };
    size_t m_bytecode_cache_source_text_length { 0 };

    Vector<Utf16FlyString> m_local_variables_names;

    i32 m_function_length { 0 };
    u32 m_formal_parameter_count { 0 };
    Vector<Utf16FlyString> m_parameter_names_for_mapped_arguments;

    ThisMode m_this_mode : 2 { ThisMode::Global }; // [[ThisMode]]
    FunctionKind m_kind : 3 { FunctionKind::Normal };

    bool m_strict { false };
    bool m_might_need_arguments_object { true };
    bool m_contains_direct_call_to_eval { true };
    bool m_is_arrow_function { false };
    bool m_has_simple_parameter_list { false };
    bool m_is_module_wrapper { false };
    bool m_has_bytecode_cache_source_text_range { false };

    struct VarBinding {
        Utf16FlyString name;
        FunctionLocal local {};
        bool parameter_binding { false };
        bool function_name { false };
    };

    bool m_has_parameter_expressions { false };
    bool m_has_duplicates { false };
    enum class ParameterIsLocal {
        No,
        Yes,
    };
    OrderedHashMap<Utf16FlyString, ParameterIsLocal> m_parameter_names;
    struct FunctionToInitialize {
        GC::Ref<SharedFunctionInstanceData> shared_data;
        Utf16FlyString name;
        FunctionLocal local {};
    };
    Vector<FunctionToInitialize> m_functions_to_initialize;
    bool m_arguments_object_needed { false };
    bool m_function_environment_needed { false };
    bool m_this_value_needs_environment_resolution { false };
    bool m_uses_this { false };
    Vector<VarBinding> m_var_names_to_initialize_binding;
    Vector<Utf16FlyString> m_function_names_to_initialize_binding;

    struct LexicalBinding {
        Utf16FlyString name;
        bool is_constant { false };
    };
    Vector<LexicalBinding> m_lexical_bindings;
    bool m_has_scope_body { false };
    bool m_has_non_local_lexical_declarations { false };

    size_t m_function_environment_bindings_count { 0 };
    size_t m_var_environment_bindings_count { 0 };
    size_t m_lex_environment_bindings_count { 0 };
    mutable GC::Ptr<EnvironmentShape> m_function_environment_shape;
    mutable GC::Ptr<EnvironmentShape> m_var_environment_shape;

    Variant<PropertyKey, PrivateName, Empty> m_class_field_initializer_name; // [[ClassFieldInitializerName]]
    ConstructorKind m_constructor_kind : 1 { ConstructorKind::Base };        // [[ConstructorKind]]
    bool m_is_class_constructor : 1 { false };                               // [[IsClassConstructor]]

    // NB: When non-null, points to a Rust Box<FunctionData> used for
    //     lazy compilation through the Rust pipeline.
    void* m_rust_function_ast { nullptr };
    // NB: When non-null, points to a Rust Box<DecodedExecutableRecord> used
    //     for lazy materialization from the bytecode cache.
    void* m_cached_bytecode_executable { nullptr };
    // NB: When non-null, points to a Rust Box<PrecompiledFunction> used for
    //     lazy materialization from freshly compiled bytecode.
    void* m_precompiled_bytecode_executable { nullptr };
    bool m_use_rust_compilation { false };

    void clear_compile_inputs();
    void clear_non_bytecode_cache_compile_inputs();

private:
    void initialize_after_construction();

    virtual void visit_edges(Visitor&) override;
    virtual size_t external_memory_size() const override;
    void update_can_inline_call();

    bool m_can_inline_call { false };
};

class JS_API SharedFunctionInstanceDataList {
    friend class SharedFunctionInstanceData;

public:
    ~SharedFunctionInstanceDataList();

    void append(SharedFunctionInstanceData&);
    void visit_edges(GC::Cell::Visitor&);
    void clear_non_bytecode_cache_compile_inputs();
    [[nodiscard]] size_t size_slow() const { return m_list.size_slow(); }
    [[nodiscard]] bool contains_rust_function_ast() const;
    [[nodiscard]] bool contains_precompiled_bytecode() const;

    template<typename Callback>
    void for_each(Callback callback)
    {
        for (auto& shared_data : m_list)
            callback(shared_data);
    }

private:
    using List = IntrusiveList<&SharedFunctionInstanceData::m_script_or_module_list_node>;

    void clear();
    void remove(SharedFunctionInstanceData&);

    List m_list;
};

}
