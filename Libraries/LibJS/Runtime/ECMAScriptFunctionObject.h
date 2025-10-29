/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/ClassFieldDefinition.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

template<typename T>
void async_block_start(VM&, T const& async_body, PromiseCapability const&, ExecutionContext&);

template<typename T>
void async_function_start(VM&, PromiseCapability const&, T const& async_function_body);

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

// 10.2 ECMAScript Function Objects, https://tc39.es/ecma262/#sec-ecmascript-function-objects
class JS_API ECMAScriptFunctionObject final : public FunctionObject {
    JS_OBJECT(ECMAScriptFunctionObject, FunctionObject);
    GC_DECLARE_ALLOCATOR(ECMAScriptFunctionObject);

public:
    static GC::Ref<ECMAScriptFunctionObject> create(Realm&, Utf16FlyString name, ByteString source_text, Statement const& ecmascript_code, NonnullRefPtr<FunctionParameters const> parameters, i32 function_length, Vector<LocalVariable> local_variables_names, Environment* parent_environment, PrivateEnvironment* private_environment, FunctionKind, bool is_strict, FunctionParsingInsights, bool is_arrow_function = false, Variant<PropertyKey, PrivateName, Empty> class_field_initializer_name = {});
    static GC::Ref<ECMAScriptFunctionObject> create(Realm&, Utf16FlyString name, Object& prototype, ByteString source_text, Statement const& ecmascript_code, NonnullRefPtr<FunctionParameters const> parameters, i32 function_length, Vector<LocalVariable> local_variables_names, Environment* parent_environment, PrivateEnvironment* private_environment, FunctionKind, bool is_strict, FunctionParsingInsights, bool is_arrow_function = false, Variant<PropertyKey, PrivateName, Empty> class_field_initializer_name = {});

    [[nodiscard]] static GC::Ref<ECMAScriptFunctionObject> create_from_function_node(
        FunctionNode const&,
        Utf16FlyString name,
        GC::Ref<Realm>,
        GC::Ptr<Environment> parent_environment,
        GC::Ptr<PrivateEnvironment>);

    virtual void initialize(Realm&) override;
    virtual ~ECMAScriptFunctionObject() override = default;

    virtual ThrowCompletionOr<void> get_stack_frame_size(size_t& registers_and_constants_and_locals_slots, size_t& argument_count) override;
    virtual ThrowCompletionOr<Value> internal_call(ExecutionContext&, Value this_argument) override;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ExecutionContext&, FunctionObject& new_target) override;

    void make_method(Object& home_object);

    [[nodiscard]] bool is_module_wrapper() const { return shared_data().m_is_module_wrapper; }
    void set_is_module_wrapper(bool b) { const_cast<SharedFunctionInstanceData&>(shared_data()).m_is_module_wrapper = b; }

    Statement const& ecmascript_code() const { return *shared_data().m_ecmascript_code; }
    [[nodiscard]] virtual FunctionParameters const& formal_parameters() const override { return *shared_data().m_formal_parameters; }

    virtual Utf16String name_for_call_stack() const override;

    Utf16FlyString const& name() const { return shared_data().m_name; }
    void set_name(Utf16FlyString const& name);

    void set_is_class_constructor() { const_cast<SharedFunctionInstanceData&>(shared_data()).m_is_class_constructor = true; }

    auto& bytecode_executable() const { return shared_data().m_executable; }

    Environment* environment() { return m_environment; }
    virtual Realm* realm() const override { return &shape().realm(); }

    [[nodiscard]] ConstructorKind constructor_kind() const { return shared_data().m_constructor_kind; }
    void set_constructor_kind(ConstructorKind constructor_kind) { const_cast<SharedFunctionInstanceData&>(shared_data()).m_constructor_kind = constructor_kind; }

    [[nodiscard]] ThisMode this_mode() const { return shared_data().m_this_mode; }
    [[nodiscard]] bool is_arrow_function() const { return shared_data().m_is_arrow_function; }
    [[nodiscard]] bool is_class_constructor() const { return shared_data().m_is_class_constructor; }
    [[nodiscard]] bool uses_this() const { return shared_data().m_uses_this; }
    [[nodiscard]] i32 function_length() const { return shared_data().m_function_length; }

    Object* home_object() const { return m_home_object; }
    void set_home_object(Object* home_object) { m_home_object = home_object; }

    [[nodiscard]] ByteString const& source_text() const { return shared_data().m_source_text; }
    void set_source_text(ByteString source_text) { const_cast<SharedFunctionInstanceData&>(shared_data()).m_source_text = move(source_text); }

    Vector<ClassFieldDefinition> const& fields() const { return ensure_class_data().fields; }
    void add_field(ClassFieldDefinition field) { ensure_class_data().fields.append(move(field)); }

    Vector<PrivateElement> const& private_methods() const { return ensure_class_data().private_methods; }
    void add_private_method(PrivateElement method) { ensure_class_data().private_methods.append(move(method)); }

    [[nodiscard]] bool has_class_data() const { return m_class_data; }

    // This is for IsSimpleParameterList (static semantics)
    bool has_simple_parameter_list() const { return shared_data().m_has_simple_parameter_list; }

    // Equivalent to absence of [[Construct]]
    virtual bool has_constructor() const override { return kind() == FunctionKind::Normal && !shared_data().m_is_arrow_function; }

    virtual Vector<LocalVariable> const& local_variables_names() const override { return shared_data().m_local_variables_names; }

    FunctionKind kind() const { return shared_data().m_kind; }

    // This is used by LibWeb to disassociate event handler attribute callback functions from the nearest script on the call stack.
    // https://html.spec.whatwg.org/multipage/webappapis.html#getting-the-current-value-of-the-event-handler Step 3.11
    void set_script_or_module(ScriptOrModule script_or_module) { m_script_or_module = move(script_or_module); }

    Variant<PropertyKey, PrivateName, Empty> const& class_field_initializer_name() const { return shared_data().m_class_field_initializer_name; }

    bool allocates_function_environment() const { return shared_data().m_function_environment_needed; }

    friend class Bytecode::Generator;

private:
    ECMAScriptFunctionObject(
        GC::Ref<SharedFunctionInstanceData>,
        Environment* parent_environment,
        PrivateEnvironment* private_environment,
        Object& prototype);

    virtual bool is_strict_mode() const override { return shared_data().m_strict; }

    ThrowCompletionOr<Value> ordinary_call_evaluate_body(VM&, ExecutionContext&);

    [[nodiscard]] bool function_environment_needed() const { return shared_data().m_function_environment_needed; }
    SharedFunctionInstanceData const& shared_data() const { return m_shared_data; }

    virtual bool is_ecmascript_function_object() const override { return true; }
    virtual void visit_edges(Visitor&) override;

    void prepare_for_ordinary_call(VM&, ExecutionContext& callee_context, Object* new_target);
    void ordinary_call_bind_this(VM&, ExecutionContext&, Value this_argument);

    GC::Ref<SharedFunctionInstanceData> m_shared_data;

    GC::Ptr<PrimitiveString> m_name_string;

    // Internal Slots of ECMAScript Function Objects, https://tc39.es/ecma262/#table-internal-slots-of-ecmascript-function-objects
    GC::Ptr<Environment> m_environment;                // [[Environment]]
    GC::Ptr<PrivateEnvironment> m_private_environment; // [[PrivateEnvironment]]
    ScriptOrModule m_script_or_module;                 // [[ScriptOrModule]]
    GC::Ptr<Object> m_home_object;                     // [[HomeObject]]
    struct ClassData {
        Vector<ClassFieldDefinition> fields;    // [[Fields]]
        Vector<PrivateElement> private_methods; // [[PrivateMethods]]
    };
    ClassData& ensure_class_data() const;
    mutable OwnPtr<ClassData> m_class_data;
};

template<>
inline bool Object::fast_is<ECMAScriptFunctionObject>() const { return is_ecmascript_function_object(); }

}
