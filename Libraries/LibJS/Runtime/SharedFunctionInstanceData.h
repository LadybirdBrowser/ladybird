/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGC/Cell.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/FunctionParsingInsights.h>
#include <LibJS/LocalVariable.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

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

class FunctionNode;

class JS_API SharedFunctionInstanceData final : public GC::Cell {
    GC_CELL(SharedFunctionInstanceData, GC::Cell);
    GC_DECLARE_ALLOCATOR(SharedFunctionInstanceData);
    static constexpr bool OVERRIDES_FINALIZE = true;

public:
    virtual ~SharedFunctionInstanceData() override;
    virtual void finalize() override;

    static GC::Ref<SharedFunctionInstanceData> create_for_function_node(VM&, FunctionNode const&);
    static GC::Ref<SharedFunctionInstanceData> create_for_function_node(VM&, FunctionNode const&, Utf16FlyString name);

    SharedFunctionInstanceData(
        VM& vm,
        FunctionKind,
        Utf16FlyString name,
        i32 function_length,
        NonnullRefPtr<FunctionParameters const>,
        NonnullRefPtr<Statement const> ecmascript_code,
        Utf16View source_text,
        bool strict,
        bool is_arrow_function,
        FunctionParsingInsights const&,
        Vector<LocalVariable> local_variables_names);

    // NB: Constructor for the Rust pipeline. Takes pre-computed metadata
    //     instead of a C++ AST. FDI fields are populated later during
    //     lazy compilation by rust_compile_function.
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
        void* rust_function_ast);

    mutable GC::Ptr<Bytecode::Executable> m_executable;

    RefPtr<FunctionParameters const> m_formal_parameters; // [[FormalParameters]]
    RefPtr<Statement const> m_ecmascript_code;            // [[ECMAScriptCode]]

    Utf16FlyString m_name;

    // NB: m_source_text is normally a view into the underlying JS::SourceCode we parsed the AST from,
    //     kept alive by m_source_code. m_source_text_owner is used if the source text needs to be
    //     owned by the function data (e.g. for dynamically created functions via Function constructor).
    RefPtr<SourceCode const> m_source_code;
    Utf16String m_source_text_owner;
    Utf16View m_source_text; // [[SourceText]]

    Vector<LocalVariable> m_local_variables_names;

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

    Variant<PropertyKey, PrivateName, Empty> m_class_field_initializer_name; // [[ClassFieldInitializerName]]
    ConstructorKind m_constructor_kind : 1 { ConstructorKind::Base };        // [[ConstructorKind]]
    bool m_is_class_constructor : 1 { false };                               // [[IsClassConstructor]]

    // NB: When non-null, points to a Rust Box<FunctionData> used for
    //     lazy compilation through the Rust pipeline.
    void* m_rust_function_ast { nullptr };
    bool m_use_rust_compilation { false };

    void clear_compile_inputs();

private:
    virtual void visit_edges(Visitor&) override;
};

}
