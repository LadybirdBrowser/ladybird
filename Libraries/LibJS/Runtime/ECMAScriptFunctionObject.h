/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/ClassFieldDefinition.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

template<typename T>
void async_block_start(VM&, T const& async_body, PromiseCapability const&, ExecutionContext&);

template<typename T>
void async_function_start(VM&, PromiseCapability const&, T const& async_function_body);

// 10.2 ECMAScript Function Objects, https://tc39.es/ecma262/#sec-ecmascript-function-objects
class ECMAScriptFunctionObject final : public FunctionObject {
    JS_OBJECT(ECMAScriptFunctionObject, FunctionObject);
    GC_DECLARE_ALLOCATOR(ECMAScriptFunctionObject);

public:
    enum class ConstructorKind : u8 {
        Base,
        Derived,
    };

    enum class ThisMode : u8 {
        Lexical,
        Strict,
        Global,
    };

    static GC::Ref<ECMAScriptFunctionObject> create(Realm&, FlyString name, ByteString source_text, Statement const& ecmascript_code, NonnullRefPtr<FunctionParameters const> parameters, i32 m_function_length, Vector<FlyString> local_variables_names, Environment* parent_environment, PrivateEnvironment* private_environment, FunctionKind, bool is_strict, FunctionParsingInsights, bool is_arrow_function = false, Variant<PropertyKey, PrivateName, Empty> class_field_initializer_name = {});
    static GC::Ref<ECMAScriptFunctionObject> create(Realm&, FlyString name, Object& prototype, ByteString source_text, Statement const& ecmascript_code, NonnullRefPtr<FunctionParameters const> parameters, i32 m_function_length, Vector<FlyString> local_variables_names, Environment* parent_environment, PrivateEnvironment* private_environment, FunctionKind, bool is_strict, FunctionParsingInsights, bool is_arrow_function = false, Variant<PropertyKey, PrivateName, Empty> class_field_initializer_name = {});

    virtual void initialize(Realm&) override;
    virtual ~ECMAScriptFunctionObject() override = default;

    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) override;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ReadonlySpan<Value> arguments_list, FunctionObject& new_target) override;

    void make_method(Object& home_object);

    [[nodiscard]] bool is_module_wrapper() const { return m_is_module_wrapper; }
    void set_is_module_wrapper(bool b) { m_is_module_wrapper = b; }

    Statement const& ecmascript_code() const { return m_ecmascript_code; }
    virtual NonnullRefPtr<FunctionParameters const> const& formal_parameters() const override { return m_formal_parameters; }

    virtual FlyString const& name() const override { return m_name; }
    void set_name(FlyString const& name);

    void set_is_class_constructor() { m_is_class_constructor = true; }

    auto& bytecode_executable() const { return m_bytecode_executable; }

    Environment* environment() { return m_environment; }
    virtual Realm* realm() const override { return m_realm; }

    ConstructorKind constructor_kind() const { return m_constructor_kind; }
    void set_constructor_kind(ConstructorKind constructor_kind) { m_constructor_kind = constructor_kind; }

    ThisMode this_mode() const { return m_this_mode; }

    Object* home_object() const { return m_home_object; }
    void set_home_object(Object* home_object) { m_home_object = home_object; }

    ByteString const& source_text() const { return m_source_text; }
    void set_source_text(ByteString source_text) { m_source_text = move(source_text); }

    Vector<ClassFieldDefinition> const& fields() const { return m_fields; }
    void add_field(ClassFieldDefinition field) { m_fields.append(move(field)); }

    Vector<PrivateElement> const& private_methods() const { return m_private_methods; }
    void add_private_method(PrivateElement method) { m_private_methods.append(move(method)); }

    // This is for IsSimpleParameterList (static semantics)
    bool has_simple_parameter_list() const { return m_has_simple_parameter_list; }

    // Equivalent to absence of [[Construct]]
    virtual bool has_constructor() const override { return m_kind == FunctionKind::Normal && !m_is_arrow_function; }

    virtual Vector<FlyString> const& local_variables_names() const override { return m_local_variables_names; }

    FunctionKind kind() const { return m_kind; }

    // This is used by LibWeb to disassociate event handler attribute callback functions from the nearest script on the call stack.
    // https://html.spec.whatwg.org/multipage/webappapis.html#getting-the-current-value-of-the-event-handler Step 3.11
    void set_script_or_module(ScriptOrModule script_or_module) { m_script_or_module = move(script_or_module); }

    Variant<PropertyKey, PrivateName, Empty> const& class_field_initializer_name() const { return m_class_field_initializer_name; }

    bool allocates_function_environment() const { return m_function_environment_needed; }

    friend class Bytecode::Generator;

protected:
    virtual bool is_strict_mode() const final { return m_strict; }

    virtual Completion ordinary_call_evaluate_body();

private:
    ECMAScriptFunctionObject(FlyString name, ByteString source_text, Statement const& ecmascript_code, NonnullRefPtr<FunctionParameters const>, i32 m_function_length, Vector<FlyString> local_variables_names, Environment* parent_environment, PrivateEnvironment* private_environment, Object& prototype, FunctionKind, bool is_strict, FunctionParsingInsights, bool is_arrow_function, Variant<PropertyKey, PrivateName, Empty> class_field_initializer_name);

    virtual bool is_ecmascript_function_object() const override { return true; }
    virtual void visit_edges(Visitor&) override;

    ThrowCompletionOr<void> prepare_for_ordinary_call(ExecutionContext& callee_context, Object* new_target);
    void ordinary_call_bind_this(ExecutionContext&, Value this_argument);

    FlyString m_name;
    GC::Ptr<PrimitiveString> m_name_string;

    GC::Ptr<Bytecode::Executable> m_bytecode_executable;
    i32 m_function_length { 0 };
    Vector<FlyString> m_local_variables_names;

    // Internal Slots of ECMAScript Function Objects, https://tc39.es/ecma262/#table-internal-slots-of-ecmascript-function-objects
    GC::Ptr<Environment> m_environment;                                      // [[Environment]]
    GC::Ptr<PrivateEnvironment> m_private_environment;                       // [[PrivateEnvironment]]
    NonnullRefPtr<FunctionParameters const> m_formal_parameters;             // [[FormalParameters]]
    NonnullRefPtr<Statement const> m_ecmascript_code;                        // [[ECMAScriptCode]]
    GC::Ptr<Realm> m_realm;                                                  // [[Realm]]
    ScriptOrModule m_script_or_module;                                       // [[ScriptOrModule]]
    GC::Ptr<Object> m_home_object;                                           // [[HomeObject]]
    ByteString m_source_text;                                                // [[SourceText]]
    Vector<ClassFieldDefinition> m_fields;                                   // [[Fields]]
    Vector<PrivateElement> m_private_methods;                                // [[PrivateMethods]]
    Variant<PropertyKey, PrivateName, Empty> m_class_field_initializer_name; // [[ClassFieldInitializerName]]
    ConstructorKind m_constructor_kind : 1 { ConstructorKind::Base };        // [[ConstructorKind]]
    bool m_strict : 1 { false };                                             // [[Strict]]
    bool m_is_class_constructor : 1 { false };                               // [[IsClassConstructor]]
    ThisMode m_this_mode : 2 { ThisMode::Global };                           // [[ThisMode]]

    bool m_might_need_arguments_object : 1 { true };
    bool m_contains_direct_call_to_eval : 1 { true };
    bool m_is_arrow_function : 1 { false };
    bool m_has_simple_parameter_list : 1 { false };
    FunctionKind m_kind : 3 { FunctionKind::Normal };

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
    HashMap<FlyString, ParameterIsLocal> m_parameter_names;
    Vector<FunctionDeclaration const&> m_functions_to_initialize;
    bool m_arguments_object_needed { false };
    bool m_is_module_wrapper { false };
    bool m_function_environment_needed { false };
    bool m_uses_this { false };
    Vector<VariableNameToInitialize> m_var_names_to_initialize_binding;
    Vector<FlyString> m_function_names_to_initialize_binding;

    size_t m_function_environment_bindings_count { 0 };
    size_t m_var_environment_bindings_count { 0 };
    size_t m_lex_environment_bindings_count { 0 };
};

template<>
inline bool Object::fast_is<ECMAScriptFunctionObject>() const { return is_ecmascript_function_object(); }

}
