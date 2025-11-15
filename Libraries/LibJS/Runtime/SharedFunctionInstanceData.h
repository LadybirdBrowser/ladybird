/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibJS/AST.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/FunctionKind.h>

namespace JS {

enum class ThisMode : u8 {
    Lexical,
    Strict,
    Global,
};

enum class ConstructorKind : u8 {
    Base,
    Derived,
};

class SharedFunctionInstanceData final : public GC::Cell {
    GC_CELL(SharedFunctionInstanceData, GC::Cell);
    GC_DECLARE_ALLOCATOR(SharedFunctionInstanceData);

public:
    virtual ~SharedFunctionInstanceData() override;

    SharedFunctionInstanceData(
        VM& vm,
        FunctionKind,
        Utf16FlyString name,
        i32 function_length,
        NonnullRefPtr<FunctionParameters const>,
        NonnullRefPtr<Statement const> ecmascript_code,
        ByteString source_text,
        bool strict,
        bool is_arrow_function,
        FunctionParsingInsights const&,
        Vector<LocalVariable> local_variables_names);

    mutable GC::Ptr<Bytecode::Executable> m_executable;

    RefPtr<FunctionParameters const> m_formal_parameters; // [[FormalParameters]]
    RefPtr<Statement const> m_ecmascript_code;            // [[ECMAScriptCode]]

    Utf16FlyString m_name;
    ByteString m_source_text; // [[SourceText]]

    Vector<LocalVariable> m_local_variables_names;

    i32 m_function_length { 0 };

    ThisMode m_this_mode : 2 { ThisMode::Global }; // [[ThisMode]]
    FunctionKind m_kind : 3 { FunctionKind::Normal };

    bool m_strict { false };
    bool m_might_need_arguments_object { true };
    bool m_contains_direct_call_to_eval { true };
    bool m_is_arrow_function { false };
    bool m_has_simple_parameter_list { false };
    bool m_is_module_wrapper { false };

    struct VariableNameToInitialize {
        Identifier const& identifier;
        bool parameter_binding { false };
        bool function_name { false };
    };

    bool m_has_parameter_expressions { false };
    bool m_has_duplicates { false };
    enum class ParameterIsLocal {
        No,
        Yes,
    };
    HashMap<Utf16FlyString, ParameterIsLocal> m_parameter_names;
    Vector<FunctionDeclaration const&> m_functions_to_initialize;
    bool m_arguments_object_needed { false };
    bool m_function_environment_needed { false };
    bool m_uses_this { false };
    Vector<VariableNameToInitialize> m_var_names_to_initialize_binding;
    Vector<Utf16FlyString> m_function_names_to_initialize_binding;

    size_t m_function_environment_bindings_count { 0 };
    size_t m_var_environment_bindings_count { 0 };
    size_t m_lex_environment_bindings_count { 0 };

    Variant<PropertyKey, PrivateName, Empty> m_class_field_initializer_name; // [[ClassFieldInitializerName]]
    ConstructorKind m_constructor_kind : 1 { ConstructorKind::Base };        // [[ConstructorKind]]
    bool m_is_class_constructor : 1 { false };                               // [[IsClassConstructor]]

private:
    virtual void visit_edges(Visitor&) override;
};

}
