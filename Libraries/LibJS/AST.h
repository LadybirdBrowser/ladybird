/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/FlyString.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibJS/Bytecode/CodeGenerationError.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/Operand.h>
#include <LibJS/Bytecode/ScopedOperand.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ClassFieldDefinition.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/EnvironmentCoordinate.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceRange.h>
#include <LibRegex/Regex.h>

namespace JS {

class Declaration;
class ClassDeclaration;
class FunctionDeclaration;
class Identifier;
class MemberExpression;
class VariableDeclaration;

template<class T, class... Args>
static inline NonnullRefPtr<T>
create_ast_node(SourceRange range, Args&&... args)
{
    return adopt_ref(*new T(move(range), forward<Args>(args)...));
}

class ASTNode : public RefCounted<ASTNode> {
public:
    virtual ~ASTNode() = default;

    // NOTE: This is here to stop ASAN complaining about mismatch between new/delete sizes in ASTNodeWithTailArray.
    void operator delete(void* ptr) { ::operator delete(ptr); }

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const;
    virtual void dump(int indent) const;

    [[nodiscard]] SourceRange source_range() const;
    UnrealizedSourceRange unrealized_source_range() const
    {
        return { m_source_code, m_start_offset, m_end_offset };
    }
    u32 start_offset() const { return m_start_offset; }
    u32 end_offset() const { return m_end_offset; }

    SourceCode const& source_code() const { return *m_source_code; }

    void set_end_offset(Badge<Parser>, u32 end_offset) { m_end_offset = end_offset; }

    ByteString class_name() const;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_new_expression() const { return false; }
    virtual bool is_member_expression() const { return false; }
    virtual bool is_super_expression() const { return false; }
    virtual bool is_function_expression() const { return false; }
    virtual bool is_class_expression() const { return false; }
    virtual bool is_expression_statement() const { return false; }
    virtual bool is_identifier() const { return false; }
    virtual bool is_private_identifier() const { return false; }
    virtual bool is_scope_node() const { return false; }
    virtual bool is_program() const { return false; }
    virtual bool is_class_declaration() const { return false; }
    virtual bool is_function_declaration() const { return false; }
    virtual bool is_variable_declaration() const { return false; }
    virtual bool is_import_call() const { return false; }
    virtual bool is_array_expression() const { return false; }
    virtual bool is_object_expression() const { return false; }
    virtual bool is_numeric_literal() const { return false; }
    virtual bool is_string_literal() const { return false; }
    virtual bool is_update_expression() const { return false; }
    virtual bool is_call_expression() const { return false; }
    virtual bool is_labelled_statement() const { return false; }
    virtual bool is_iteration_statement() const { return false; }
    virtual bool is_class_method() const { return false; }

protected:
    explicit ASTNode(SourceRange);

private:
    // NOTE: These members are carefully ordered so that `m_start_offset` is packed with the padding after RefCounted::m_ref_count.
    //       This creates a 4-byte padding hole after `m_end_offset` which is used to pack subclasses better.
    u32 m_start_offset { 0 };
    RefPtr<SourceCode const> m_source_code;
    u32 m_end_offset { 0 };
};

// This is a helper class that packs an array of T after the AST node, all in the same allocation.
template<typename Derived, typename Base, typename T>
class ASTNodeWithTailArray : public Base {
public:
    virtual ~ASTNodeWithTailArray() override
    {
        for (auto& value : tail_span())
            value.~T();
    }

    ReadonlySpan<T> tail_span() const { return { tail_data(), tail_size() }; }

    T const* tail_data() const { return reinterpret_cast<T const*>(reinterpret_cast<uintptr_t>(this) + sizeof(Derived)); }
    size_t tail_size() const { return m_tail_size; }

protected:
    template<typename ActualDerived = Derived, typename... Args>
    static NonnullRefPtr<ActualDerived> create(size_t tail_size, SourceRange source_range, Args&&... args)
    {
        static_assert(sizeof(ActualDerived) == sizeof(Derived), "This leaf class cannot add more members");
        static_assert(alignof(ActualDerived) % alignof(T) == 0, "Need padding for tail array");
        auto* memory = ::operator new(sizeof(ActualDerived) + tail_size * sizeof(T));
        return adopt_ref(*::new (memory) ActualDerived(move(source_range), forward<Args>(args)...));
    }

    ASTNodeWithTailArray(SourceRange source_range, ReadonlySpan<T> values)
        : Base(move(source_range))
        , m_tail_size(values.size())
    {
        VERIFY(values.size() <= NumericLimits<u32>::max());
        for (size_t i = 0; i < values.size(); ++i)
            new (&tail_data()[i]) T(values[i]);
    }

private:
    T* tail_data() { return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + sizeof(Derived)); }

    u32 m_tail_size { 0 };
};

class Statement : public ASTNode {
public:
    explicit Statement(SourceRange source_range)
        : ASTNode(move(source_range))
    {
    }

    Bytecode::Executable* bytecode_executable() const { return m_bytecode_executable; }
    void set_bytecode_executable(Bytecode::Executable* bytecode_executable) { m_bytecode_executable = make_root(bytecode_executable); }

private:
    GC::Root<Bytecode::Executable> m_bytecode_executable;
};

// 14.13 Labelled Statements, https://tc39.es/ecma262/#sec-labelled-statements
class LabelledStatement final : public Statement {
public:
    LabelledStatement(SourceRange source_range, FlyString label, NonnullRefPtr<Statement const> labelled_item)
        : Statement(move(source_range))
        , m_label(move(label))
        , m_labelled_item(move(labelled_item))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const;

    FlyString const& label() const { return m_label; }
    FlyString& label() { return m_label; }
    NonnullRefPtr<Statement const> const& labelled_item() const { return m_labelled_item; }

private:
    virtual bool is_labelled_statement() const final { return true; }

    FlyString m_label;
    NonnullRefPtr<Statement const> m_labelled_item;
};

class LabelableStatement : public Statement {
public:
    using Statement::Statement;

    Vector<FlyString> const& labels() const { return m_labels; }
    virtual void add_label(FlyString string) { m_labels.append(move(string)); }

protected:
    Vector<FlyString> m_labels;
};

class IterationStatement : public Statement {
public:
    using Statement::Statement;

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const;

private:
    virtual bool is_iteration_statement() const final { return true; }
};

class EmptyStatement final : public Statement {
public:
    explicit EmptyStatement(SourceRange source_range)
        : Statement(move(source_range))
    {
    }
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
};

class ErrorStatement final : public Statement {
public:
    explicit ErrorStatement(SourceRange source_range)
        : Statement(move(source_range))
    {
    }
};

class ExpressionStatement final : public Statement {
public:
    ExpressionStatement(SourceRange source_range, NonnullRefPtr<Expression const> expression)
        : Statement(move(source_range))
        , m_expression(move(expression))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    Expression const& expression() const { return m_expression; }

private:
    virtual bool is_expression_statement() const override { return true; }

    NonnullRefPtr<Expression const> m_expression;
};

template<typename Func, typename... Args>
concept ThrowCompletionOrVoidFunction = requires(Func func, Args... args) {
    {
        func(args...)
    }
    -> SameAs<ThrowCompletionOr<void>>;
};

template<typename... Args>
class ThrowCompletionOrVoidCallback : public Function<ThrowCompletionOr<void>(Args...)> {
public:
    template<typename CallableType>
    ThrowCompletionOrVoidCallback(CallableType&& callable)
    requires(VoidFunction<CallableType, Args...>)
        : Function<ThrowCompletionOr<void>(Args...)>([callable = forward<CallableType>(callable)](Args... args) {
            callable(args...);
            return ThrowCompletionOr<void> {};
        })
    {
    }

    template<typename CallableType>
    ThrowCompletionOrVoidCallback(CallableType&& callable)
    requires(ThrowCompletionOrVoidFunction<CallableType, Args...>)
        : Function<ThrowCompletionOr<void>(Args...)>(forward<CallableType>(callable))
    {
    }
};

class ScopeNode : public Statement {
public:
    template<typename T, typename... Args>
    T& append(SourceRange range, Args&&... args)
    {
        auto child = create_ast_node<T>(range, forward<Args>(args)...);
        m_children.append(move(child));
        return static_cast<T&>(*m_children.last());
    }
    void append(NonnullRefPtr<Statement const> child)
    {
        m_children.append(move(child));
    }

    void shrink_to_fit()
    {
        m_children.shrink_to_fit();
        m_lexical_declarations.shrink_to_fit();
        m_var_declarations.shrink_to_fit();
        m_functions_hoistable_with_annexB_extension.shrink_to_fit();
    }

    Vector<NonnullRefPtr<Statement const>> const& children() const { return m_children; }
    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    void add_var_scoped_declaration(NonnullRefPtr<Declaration const> variables);
    void add_lexical_declaration(NonnullRefPtr<Declaration const> variables);
    void add_hoisted_function(NonnullRefPtr<FunctionDeclaration const> declaration);

    [[nodiscard]] bool has_lexical_declarations() const { return !m_lexical_declarations.is_empty(); }
    [[nodiscard]] bool has_non_local_lexical_declarations() const;
    [[nodiscard]] bool has_var_declarations() const { return !m_var_declarations.is_empty(); }

    [[nodiscard]] size_t var_declaration_count() const { return m_var_declarations.size(); }
    [[nodiscard]] size_t lexical_declaration_count() const { return m_lexical_declarations.size(); }

    ThrowCompletionOr<void> for_each_lexically_scoped_declaration(ThrowCompletionOrVoidCallback<Declaration const&>&& callback) const;
    ThrowCompletionOr<void> for_each_lexically_declared_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const;

    ThrowCompletionOr<void> for_each_var_declared_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const;

    ThrowCompletionOr<void> for_each_var_function_declaration_in_reverse_order(ThrowCompletionOrVoidCallback<FunctionDeclaration const&>&& callback) const;
    ThrowCompletionOr<void> for_each_var_scoped_variable_declaration(ThrowCompletionOrVoidCallback<VariableDeclaration const&>&& callback) const;

    void block_declaration_instantiation(VM&, Environment*) const;

    ThrowCompletionOr<void> for_each_function_hoistable_with_annexB_extension(ThrowCompletionOrVoidCallback<FunctionDeclaration&>&& callback) const;

    Vector<FlyString> const& local_variables_names() const { return m_local_variables_names; }
    size_t add_local_variable(FlyString name)
    {
        auto index = m_local_variables_names.size();
        m_local_variables_names.append(move(name));
        return index;
    }

protected:
    explicit ScopeNode(SourceRange source_range)
        : Statement(move(source_range))
    {
    }

private:
    virtual bool is_scope_node() const final { return true; }

    Vector<NonnullRefPtr<Statement const>> m_children;
    Vector<NonnullRefPtr<Declaration const>> m_lexical_declarations;
    Vector<NonnullRefPtr<Declaration const>> m_var_declarations;

    Vector<NonnullRefPtr<FunctionDeclaration const>> m_functions_hoistable_with_annexB_extension;

    Vector<FlyString> m_local_variables_names;
};

// ImportEntry Record, https://tc39.es/ecma262/#table-importentry-record-fields
struct ImportEntry {
    Optional<FlyString> import_name; // [[ImportName]]: stored string if Optional is not empty, NAMESPACE-OBJECT otherwise
    FlyString local_name;            // [[LocalName]]

    ImportEntry(Optional<FlyString> import_name_, FlyString local_name_)
        : import_name(move(import_name_))
        , local_name(move(local_name_))
    {
    }

    bool is_namespace() const { return !import_name.has_value(); }

    ModuleRequest const& module_request() const
    {
        VERIFY(m_module_request);
        return *m_module_request;
    }

private:
    friend class ImportStatement;
    ModuleRequest* m_module_request = nullptr; // [[ModuleRequest]]
};

class ImportStatement final : public Statement {
public:
    explicit ImportStatement(SourceRange source_range, ModuleRequest from_module, Vector<ImportEntry> entries = {})
        : Statement(move(source_range))
        , m_module_request(move(from_module))
        , m_entries(move(entries))
    {
        for (auto& entry : m_entries)
            entry.m_module_request = &m_module_request;
    }

    virtual void dump(int indent) const override;

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    bool has_bound_name(FlyString const& name) const;
    Vector<ImportEntry> const& entries() const { return m_entries; }
    ModuleRequest const& module_request() const { return m_module_request; }

private:
    ModuleRequest m_module_request;
    Vector<ImportEntry> m_entries;
};

// ExportEntry Record, https://tc39.es/ecma262/#table-exportentry-records
struct ExportEntry {
    enum class Kind {
        NamedExport,
        ModuleRequestAll,
        ModuleRequestAllButDefault,
        // EmptyNamedExport is a special type for export {} from "module",
        // which should import the module without getting any of the exports
        // however we don't want give it a fake export name which may get
        // duplicates
        EmptyNamedExport,
    } kind;

    Optional<FlyString> export_name;          // [[ExportName]]
    Optional<FlyString> local_or_import_name; // Either [[ImportName]] or [[LocalName]]

    ExportEntry(Kind export_kind, Optional<FlyString> export_name_, Optional<FlyString> local_or_import_name_)
        : kind(export_kind)
        , export_name(move(export_name_))
        , local_or_import_name(move(local_or_import_name_))
    {
    }

    bool is_module_request() const
    {
        return m_module_request != nullptr;
    }

    static ExportEntry indirect_export_entry(ModuleRequest const& module_request, Optional<FlyString> export_name, Optional<FlyString> import_name)
    {
        ExportEntry entry { Kind::NamedExport, move(export_name), move(import_name) };
        entry.m_module_request = &module_request;
        return entry;
    }

    ModuleRequest const& module_request() const
    {
        VERIFY(m_module_request);
        return *m_module_request;
    }

private:
    ModuleRequest const* m_module_request { nullptr }; // [[ModuleRequest]]
    friend class ExportStatement;

public:
    static ExportEntry named_export(FlyString export_name, FlyString local_name)
    {
        return ExportEntry { Kind::NamedExport, move(export_name), move(local_name) };
    }

    static ExportEntry all_but_default_entry()
    {
        return ExportEntry { Kind::ModuleRequestAllButDefault, {}, {} };
    }

    static ExportEntry all_module_request(FlyString export_name)
    {
        return ExportEntry { Kind::ModuleRequestAll, move(export_name), {} };
    }

    static ExportEntry empty_named_export()
    {
        return ExportEntry { Kind::EmptyNamedExport, {}, {} };
    }
};

class ExportStatement final : public Statement {
public:
    static FlyString local_name_for_default;

    ExportStatement(SourceRange source_range, RefPtr<ASTNode const> statement, Vector<ExportEntry> entries, bool is_default_export, Optional<ModuleRequest> module_request)
        : Statement(move(source_range))
        , m_statement(move(statement))
        , m_entries(move(entries))
        , m_is_default_export(is_default_export)
        , m_module_request(move(module_request))
    {
        if (m_module_request.has_value()) {
            for (auto& entry : m_entries)
                entry.m_module_request = &m_module_request.value();
        }
    }

    virtual void dump(int indent) const override;

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    bool has_export(FlyString const& export_name) const;

    bool has_statement() const { return m_statement; }
    Vector<ExportEntry> const& entries() const { return m_entries; }

    bool is_default_export() const { return m_is_default_export; }

    ASTNode const& statement() const
    {
        VERIFY(m_statement);
        return *m_statement;
    }

    ModuleRequest const& module_request() const
    {
        return m_module_request.value();
    }

private:
    RefPtr<ASTNode const> m_statement;
    Vector<ExportEntry> m_entries;
    bool m_is_default_export { false };
    Optional<ModuleRequest> m_module_request;
};

class Program final : public ScopeNode {
public:
    enum class Type {
        Script,
        Module
    };

    explicit Program(SourceRange source_range, Type program_type)
        : ScopeNode(move(source_range))
        , m_type(program_type)
    {
    }

    bool is_strict_mode() const { return m_is_strict_mode; }
    void set_strict_mode() { m_is_strict_mode = true; }

    Type type() const { return m_type; }

    void append_import(NonnullRefPtr<ImportStatement const> import_statement)
    {
        m_imports.append(import_statement);
        append(move(import_statement));
    }

    void append_export(NonnullRefPtr<ExportStatement const> export_statement)
    {
        m_exports.append(export_statement);
        append(move(export_statement));
    }

    Vector<NonnullRefPtr<ImportStatement const>> const& imports() const { return m_imports; }
    Vector<NonnullRefPtr<ExportStatement const>> const& exports() const { return m_exports; }

    Vector<NonnullRefPtr<ImportStatement const>>& imports() { return m_imports; }
    Vector<NonnullRefPtr<ExportStatement const>>& exports() { return m_exports; }

    bool has_top_level_await() const { return m_has_top_level_await; }
    void set_has_top_level_await() { m_has_top_level_await = true; }

    ThrowCompletionOr<void> global_declaration_instantiation(VM&, GlobalEnvironment&) const;

private:
    virtual bool is_program() const override { return true; }

    bool m_is_strict_mode { false };
    Type m_type { Type::Script };

    Vector<NonnullRefPtr<ImportStatement const>> m_imports;
    Vector<NonnullRefPtr<ExportStatement const>> m_exports;
    bool m_has_top_level_await { false };
};

class BlockStatement final : public ScopeNode {
public:
    explicit BlockStatement(SourceRange source_range)
        : ScopeNode(move(source_range))
    {
    }
};

class FunctionBody final : public ScopeNode {
public:
    explicit FunctionBody(SourceRange source_range)
        : ScopeNode(move(source_range))
    {
    }

    void set_strict_mode() { m_in_strict_mode = true; }

    bool in_strict_mode() const { return m_in_strict_mode; }

private:
    bool m_in_strict_mode { false };
};

class Expression : public ASTNode {
public:
    explicit Expression(SourceRange source_range)
        : ASTNode(move(source_range))
    {
    }
};

class Declaration : public Statement {
public:
    explicit Declaration(SourceRange source_range)
        : Statement(move(source_range))
    {
    }

    virtual ThrowCompletionOr<void> for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const = 0;

    // 8.1.3 Static Semantics: IsConstantDeclaration, https://tc39.es/ecma262/#sec-static-semantics-isconstantdeclaration
    virtual bool is_constant_declaration() const { return false; }

    virtual bool is_lexical_declaration() const { return false; }
};

class ErrorDeclaration final : public Declaration {
public:
    explicit ErrorDeclaration(SourceRange source_range)
        : Declaration(move(source_range))
    {
    }

    ThrowCompletionOr<void> for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&&) const override
    {
        VERIFY_NOT_REACHED();
    }
};

struct BindingPattern : RefCounted<BindingPattern> {
    // This covers both BindingProperty and BindingElement, hence the more generic name
    struct BindingEntry {
        // If this entry represents a BindingElement, then name will be Empty
        Variant<NonnullRefPtr<Identifier const>, NonnullRefPtr<Expression const>, Empty> name {};
        Variant<NonnullRefPtr<Identifier const>, NonnullRefPtr<BindingPattern const>, NonnullRefPtr<MemberExpression const>, Empty> alias {};
        RefPtr<Expression const> initializer {};
        bool is_rest { false };

        bool is_elision() const { return name.has<Empty>() && alias.has<Empty>(); }
    };

    enum class Kind {
        Array,
        Object,
    };

    void dump(int indent) const;

    ThrowCompletionOr<void> for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const;

    bool contains_expression() const;

    Bytecode::CodeGenerationErrorOr<void> generate_bytecode(Bytecode::Generator&, Bytecode::Op::BindingInitializationMode initialization_mode, Bytecode::ScopedOperand const& object, bool create_variables) const;

    Vector<BindingEntry> entries;
    Kind kind { Kind::Object };
};

class Identifier final : public Expression {
public:
    explicit Identifier(SourceRange source_range, FlyString string)
        : Expression(move(source_range))
        , m_string(move(string))
    {
    }

    FlyString const& string() const { return m_string; }

    bool is_local() const { return m_local_variable_index.has_value(); }
    size_t local_variable_index() const
    {
        VERIFY(m_local_variable_index.has_value());
        return m_local_variable_index.value();
    }
    void set_local_variable_index(size_t index) { m_local_variable_index = index; }

    bool is_global() const { return m_is_global; }
    void set_is_global() { m_is_global = true; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    virtual bool is_identifier() const override { return true; }

    FlyString m_string;

    Optional<size_t> m_local_variable_index;
    bool m_is_global { false };
};

struct FunctionParameter {
    Variant<NonnullRefPtr<Identifier const>, NonnullRefPtr<BindingPattern const>> binding;
    RefPtr<Expression const> default_value;
    bool is_rest { false };
    GC::Root<Bytecode::Executable> bytecode_executable {};
};

class FunctionParameters : public RefCounted<FunctionParameters> {
public:
    static NonnullRefPtr<FunctionParameters> create(Vector<FunctionParameter> parameters)
    {
        if (parameters.is_empty())
            return empty();
        return adopt_ref(*new FunctionParameters(move(parameters)));
    }

    static NonnullRefPtr<FunctionParameters> empty();

    bool is_empty() const { return m_parameters.is_empty(); }
    size_t size() const { return m_parameters.size(); }
    Vector<FunctionParameter> const& parameters() const { return m_parameters; }

private:
    FunctionParameters(Vector<FunctionParameter> parameters)
        : m_parameters(move(parameters))
    {
    }

    Vector<FunctionParameter> m_parameters;
};

struct FunctionParsingInsights {
    bool uses_this { false };
    bool uses_this_from_environment { false };
    bool contains_direct_call_to_eval { false };
    bool might_need_arguments_object { false };
};

class FunctionNode {
public:
    FlyString name() const { return m_name ? m_name->string() : ""_fly_string; }
    RefPtr<Identifier const> name_identifier() const { return m_name; }
    ByteString const& source_text() const { return m_source_text; }
    Statement const& body() const { return *m_body; }
    auto const& body_ptr() const { return m_body; }
    auto const& parameters() const { return m_parameters; }
    i32 function_length() const { return m_function_length; }
    Vector<FlyString> const& local_variables_names() const { return m_local_variables_names; }
    bool is_strict_mode() const { return m_is_strict_mode; }
    bool might_need_arguments_object() const { return m_parsing_insights.might_need_arguments_object; }
    bool contains_direct_call_to_eval() const { return m_parsing_insights.contains_direct_call_to_eval; }
    bool is_arrow_function() const { return m_is_arrow_function; }
    FunctionParsingInsights const& parsing_insights() const { return m_parsing_insights; }
    FunctionKind kind() const { return m_kind; }
    bool uses_this_from_environment() const { return m_parsing_insights.uses_this_from_environment; }

    virtual bool has_name() const = 0;
    virtual Value instantiate_ordinary_function_expression(VM&, FlyString given_name) const = 0;

    virtual ~FunctionNode() { }

protected:
    FunctionNode(RefPtr<Identifier const> name, ByteString source_text, NonnullRefPtr<Statement const> body, NonnullRefPtr<FunctionParameters const> parameters, i32 function_length, FunctionKind kind, bool is_strict_mode, FunctionParsingInsights parsing_insights, bool is_arrow_function, Vector<FlyString> local_variables_names)
        : m_name(move(name))
        , m_source_text(move(source_text))
        , m_body(move(body))
        , m_parameters(move(parameters))
        , m_function_length(function_length)
        , m_kind(kind)
        , m_is_strict_mode(is_strict_mode)
        , m_is_arrow_function(is_arrow_function)
        , m_parsing_insights(parsing_insights)
        , m_local_variables_names(move(local_variables_names))
    {
        if (m_is_arrow_function)
            VERIFY(!parsing_insights.might_need_arguments_object);
    }

    void dump(int indent, ByteString const& class_name) const;

    RefPtr<Identifier const> m_name { nullptr };

private:
    ByteString m_source_text;
    NonnullRefPtr<Statement const> m_body;
    NonnullRefPtr<FunctionParameters const> m_parameters;
    i32 const m_function_length;
    FunctionKind m_kind;
    bool m_is_strict_mode : 1 { false };
    bool m_is_arrow_function : 1 { false };
    FunctionParsingInsights m_parsing_insights;

    Vector<FlyString> m_local_variables_names;
};

class FunctionDeclaration final
    : public Declaration
    , public FunctionNode {
public:
    static bool must_have_name() { return true; }

    FunctionDeclaration(SourceRange source_range, RefPtr<Identifier const> name, ByteString source_text, NonnullRefPtr<Statement const> body, NonnullRefPtr<FunctionParameters const> parameters, i32 function_length, FunctionKind kind, bool is_strict_mode, FunctionParsingInsights insights, Vector<FlyString> local_variables_names)
        : Declaration(move(source_range))
        , FunctionNode(move(name), move(source_text), move(body), move(parameters), function_length, kind, is_strict_mode, insights, false, move(local_variables_names))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    ThrowCompletionOr<void> for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&&) const override;

    virtual bool is_function_declaration() const override { return true; }

    void set_should_do_additional_annexB_steps() { m_is_hoisted = true; }

    bool has_name() const override { return true; }
    Value instantiate_ordinary_function_expression(VM&, FlyString) const override { VERIFY_NOT_REACHED(); }

    virtual ~FunctionDeclaration() { }

private:
    bool m_is_hoisted { false };
};

class FunctionExpression final
    : public Expression
    , public FunctionNode {
public:
    static bool must_have_name() { return false; }

    FunctionExpression(SourceRange source_range, RefPtr<Identifier const> name, ByteString source_text, NonnullRefPtr<Statement const> body, NonnullRefPtr<FunctionParameters const> parameters, i32 function_length, FunctionKind kind, bool is_strict_mode, FunctionParsingInsights insights, Vector<FlyString> local_variables_names, bool is_arrow_function = false)
        : Expression(move(source_range))
        , FunctionNode(move(name), move(source_text), move(body), move(parameters), function_length, kind, is_strict_mode, insights, is_arrow_function, move(local_variables_names))
    {
    }

    virtual void dump(int indent) const override;

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode_with_lhs_name(Bytecode::Generator&, Optional<Bytecode::IdentifierTableIndex> lhs_name, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const;

    bool has_name() const override { return !name().is_empty(); }

    Value instantiate_ordinary_function_expression(VM&, FlyString given_name) const override;

    virtual ~FunctionExpression() { }

private:
    virtual bool is_function_expression() const override { return true; }
};

class ErrorExpression final : public Expression {
public:
    explicit ErrorExpression(SourceRange source_range)
        : Expression(move(source_range))
    {
    }
};

class YieldExpression final : public Expression {
public:
    explicit YieldExpression(SourceRange source_range, RefPtr<Expression const> argument, bool is_yield_from)
        : Expression(move(source_range))
        , m_argument(move(argument))
        , m_is_yield_from(is_yield_from)
    {
    }

    Expression const* argument() const { return m_argument; }
    bool is_yield_from() const { return m_is_yield_from; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    RefPtr<Expression const> m_argument;
    bool m_is_yield_from { false };
};

class AwaitExpression final : public Expression {
public:
    explicit AwaitExpression(SourceRange source_range, NonnullRefPtr<Expression const> argument)
        : Expression(move(source_range))
        , m_argument(move(argument))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_argument;
};

class ReturnStatement final : public Statement {
public:
    explicit ReturnStatement(SourceRange source_range, RefPtr<Expression const> argument)
        : Statement(move(source_range))
        , m_argument(move(argument))
    {
    }

    Expression const* argument() const { return m_argument; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    RefPtr<Expression const> m_argument;
};

class IfStatement final : public Statement {
public:
    IfStatement(SourceRange source_range, NonnullRefPtr<Expression const> predicate, NonnullRefPtr<Statement const> consequent, RefPtr<Statement const> alternate)
        : Statement(move(source_range))
        , m_predicate(move(predicate))
        , m_consequent(move(consequent))
        , m_alternate(move(alternate))
    {
    }

    Expression const& predicate() const { return *m_predicate; }
    Statement const& consequent() const { return *m_consequent; }
    Statement const* alternate() const { return m_alternate; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_predicate;
    NonnullRefPtr<Statement const> m_consequent;
    RefPtr<Statement const> m_alternate;
};

class WhileStatement final : public IterationStatement {
public:
    WhileStatement(SourceRange source_range, NonnullRefPtr<Expression const> test, NonnullRefPtr<Statement const> body)
        : IterationStatement(move(source_range))
        , m_test(move(test))
        , m_body(move(body))
    {
    }

    Expression const& test() const { return *m_test; }
    Statement const& body() const { return *m_body; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_test;
    NonnullRefPtr<Statement const> m_body;
};

class DoWhileStatement final : public IterationStatement {
public:
    DoWhileStatement(SourceRange source_range, NonnullRefPtr<Expression const> test, NonnullRefPtr<Statement const> body)
        : IterationStatement(move(source_range))
        , m_test(move(test))
        , m_body(move(body))
    {
    }

    Expression const& test() const { return *m_test; }
    Statement const& body() const { return *m_body; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_test;
    NonnullRefPtr<Statement const> m_body;
};

class WithStatement final : public Statement {
public:
    WithStatement(SourceRange source_range, NonnullRefPtr<Expression const> object, NonnullRefPtr<Statement const> body)
        : Statement(move(source_range))
        , m_object(move(object))
        , m_body(move(body))
    {
    }

    Expression const& object() const { return *m_object; }
    Statement const& body() const { return *m_body; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_object;
    NonnullRefPtr<Statement const> m_body;
};

class ForStatement final : public IterationStatement {
public:
    ForStatement(SourceRange source_range, RefPtr<ASTNode const> init, RefPtr<Expression const> test, RefPtr<Expression const> update, NonnullRefPtr<Statement const> body)
        : IterationStatement(move(source_range))
        , m_init(move(init))
        , m_test(move(test))
        , m_update(move(update))
        , m_body(move(body))
    {
    }

    ASTNode const* init() const { return m_init; }
    Expression const* test() const { return m_test; }
    Expression const* update() const { return m_update; }
    Statement const& body() const { return *m_body; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    RefPtr<ASTNode const> m_init;
    RefPtr<Expression const> m_test;
    RefPtr<Expression const> m_update;
    NonnullRefPtr<Statement const> m_body;
};

class ForInStatement final : public IterationStatement {
public:
    ForInStatement(SourceRange source_range, Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> lhs, NonnullRefPtr<Expression const> rhs, NonnullRefPtr<Statement const> body)
        : IterationStatement(move(source_range))
        , m_lhs(move(lhs))
        , m_rhs(move(rhs))
        , m_body(move(body))
    {
    }

    auto const& lhs() const { return m_lhs; }
    Expression const& rhs() const { return *m_rhs; }
    Statement const& body() const { return *m_body; }

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual void dump(int indent) const override;

private:
    Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> m_lhs;
    NonnullRefPtr<Expression const> m_rhs;
    NonnullRefPtr<Statement const> m_body;
};

class ForOfStatement final : public IterationStatement {
public:
    ForOfStatement(SourceRange source_range, Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> lhs, NonnullRefPtr<Expression const> rhs, NonnullRefPtr<Statement const> body)
        : IterationStatement(move(source_range))
        , m_lhs(move(lhs))
        , m_rhs(move(rhs))
        , m_body(move(body))
    {
    }

    auto const& lhs() const { return m_lhs; }
    Expression const& rhs() const { return *m_rhs; }
    Statement const& body() const { return *m_body; }

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual void dump(int indent) const override;

private:
    Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> m_lhs;
    NonnullRefPtr<Expression const> m_rhs;
    NonnullRefPtr<Statement const> m_body;
};

class ForAwaitOfStatement final : public IterationStatement {
public:
    ForAwaitOfStatement(SourceRange source_range, Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> lhs, NonnullRefPtr<Expression const> rhs, NonnullRefPtr<Statement const> body)
        : IterationStatement(move(source_range))
        , m_lhs(move(lhs))
        , m_rhs(move(rhs))
        , m_body(move(body))
    {
    }

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual void dump(int indent) const override;

private:
    Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> m_lhs;
    NonnullRefPtr<Expression const> m_rhs;
    NonnullRefPtr<Statement const> m_body;
};

enum class BinaryOp {
    Addition,
    Subtraction,
    Multiplication,
    Division,
    Modulo,
    Exponentiation,
    StrictlyEquals,
    StrictlyInequals,
    LooselyEquals,
    LooselyInequals,
    GreaterThan,
    GreaterThanEquals,
    LessThan,
    LessThanEquals,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    LeftShift,
    RightShift,
    UnsignedRightShift,
    In,
    InstanceOf,
};

class BinaryExpression final : public Expression {
public:
    BinaryExpression(SourceRange source_range, BinaryOp op, NonnullRefPtr<Expression const> lhs, NonnullRefPtr<Expression const> rhs)
        : Expression(move(source_range))
        , m_op(op)
        , m_lhs(move(lhs))
        , m_rhs(move(rhs))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    BinaryOp m_op;
    NonnullRefPtr<Expression const> m_lhs;
    NonnullRefPtr<Expression const> m_rhs;
};

enum class LogicalOp {
    And,
    Or,
    NullishCoalescing,
};

class LogicalExpression final : public Expression {
public:
    LogicalExpression(SourceRange source_range, LogicalOp op, NonnullRefPtr<Expression const> lhs, NonnullRefPtr<Expression const> rhs)
        : Expression(move(source_range))
        , m_op(op)
        , m_lhs(move(lhs))
        , m_rhs(move(rhs))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    LogicalOp m_op;
    NonnullRefPtr<Expression const> m_lhs;
    NonnullRefPtr<Expression const> m_rhs;
};

enum class UnaryOp {
    BitwiseNot,
    Not,
    Plus,
    Minus,
    Typeof,
    Void,
    Delete,
};

class UnaryExpression final : public Expression {
public:
    UnaryExpression(SourceRange source_range, UnaryOp op, NonnullRefPtr<Expression const> lhs)
        : Expression(move(source_range))
        , m_op(op)
        , m_lhs(move(lhs))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    UnaryOp m_op;
    NonnullRefPtr<Expression const> m_lhs;
};

class SequenceExpression final : public Expression {
public:
    SequenceExpression(SourceRange source_range, Vector<NonnullRefPtr<Expression const>> expressions)
        : Expression(move(source_range))
        , m_expressions(move(expressions))
    {
        VERIFY(m_expressions.size() >= 2);
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    Vector<NonnullRefPtr<Expression const>> m_expressions;
};

class PrimitiveLiteral : public Expression {
public:
    virtual Value value() const = 0;

protected:
    explicit PrimitiveLiteral(SourceRange source_range)
        : Expression(move(source_range))
    {
    }
};

class BooleanLiteral final : public PrimitiveLiteral {
public:
    explicit BooleanLiteral(SourceRange source_range, bool value)
        : PrimitiveLiteral(move(source_range))
        , m_value(value)
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    virtual Value value() const override { return Value(m_value); }

private:
    bool m_value { false };
};

class NumericLiteral final : public PrimitiveLiteral {
public:
    explicit NumericLiteral(SourceRange source_range, double value)
        : PrimitiveLiteral(move(source_range))
        , m_value(value)
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    virtual Value value() const override { return m_value; }

private:
    virtual bool is_numeric_literal() const override { return true; }

    Value m_value;
};

class BigIntLiteral final : public Expression {
public:
    explicit BigIntLiteral(SourceRange source_range, ByteString value)
        : Expression(move(source_range))
        , m_value(move(value))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    ByteString m_value;
};

class StringLiteral final : public Expression {
public:
    explicit StringLiteral(SourceRange source_range, String value)
        : Expression(move(source_range))
        , m_value(move(value))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    String const& value() const { return m_value; }

private:
    virtual bool is_string_literal() const override { return true; }

    String m_value;
};

class NullLiteral final : public PrimitiveLiteral {
public:
    explicit NullLiteral(SourceRange source_range)
        : PrimitiveLiteral(move(source_range))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    virtual Value value() const override { return js_null(); }
};

class RegExpLiteral final : public Expression {
public:
    RegExpLiteral(SourceRange source_range, regex::Parser::Result parsed_regex, String parsed_pattern, regex::RegexOptions<ECMAScriptFlags> parsed_flags, String pattern, String flags)
        : Expression(move(source_range))
        , m_parsed_regex(move(parsed_regex))
        , m_parsed_pattern(move(parsed_pattern))
        , m_parsed_flags(parsed_flags)
        , m_pattern(move(pattern))
        , m_flags(move(flags))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    regex::Parser::Result const& parsed_regex() const { return m_parsed_regex; }
    String const& parsed_pattern() const { return m_parsed_pattern; }
    regex::RegexOptions<ECMAScriptFlags> const& parsed_flags() const { return m_parsed_flags; }
    String const& pattern() const { return m_pattern; }
    String const& flags() const { return m_flags; }

private:
    regex::Parser::Result m_parsed_regex;
    String m_parsed_pattern;
    regex::RegexOptions<ECMAScriptFlags> m_parsed_flags;
    String m_pattern;
    String m_flags;
};

class PrivateIdentifier final : public Expression {
public:
    explicit PrivateIdentifier(SourceRange source_range, FlyString string)
        : Expression(move(source_range))
        , m_string(move(string))
    {
    }

    FlyString const& string() const { return m_string; }

    virtual void dump(int indent) const override;

    virtual bool is_private_identifier() const override { return true; }

private:
    FlyString m_string;
};

class ClassElement : public ASTNode {
public:
    ClassElement(SourceRange source_range, bool is_static)
        : ASTNode(move(source_range))
        , m_is_static(is_static)
    {
    }

    enum class ElementKind {
        Method,
        Field,
        StaticInitializer,
    };

    virtual ElementKind class_element_kind() const = 0;
    bool is_static() const { return m_is_static; }

    // We use the Completion also as a ClassStaticBlockDefinition Record.
    using ClassValue = Variant<ClassFieldDefinition, Completion, PrivateElement>;
    virtual ThrowCompletionOr<ClassValue> class_element_evaluation(VM&, Object& home_object, Value) const = 0;

    virtual Optional<FlyString> private_bound_identifier() const { return {}; }

private:
    bool m_is_static { false };
};

class ClassMethod final : public ClassElement {
public:
    enum class Kind {
        Method,
        Getter,
        Setter,
    };

    ClassMethod(SourceRange source_range, NonnullRefPtr<Expression const> key, NonnullRefPtr<FunctionExpression const> function, Kind kind, bool is_static)
        : ClassElement(move(source_range), is_static)
        , m_key(move(key))
        , m_function(move(function))
        , m_kind(kind)
    {
    }

    Expression const& key() const { return *m_key; }
    Kind kind() const { return m_kind; }
    virtual ElementKind class_element_kind() const override { return ElementKind::Method; }

    virtual void dump(int indent) const override;
    virtual ThrowCompletionOr<ClassValue> class_element_evaluation(VM&, Object& home_object, Value property_key) const override;
    virtual Optional<FlyString> private_bound_identifier() const override;

private:
    virtual bool is_class_method() const override { return true; }
    NonnullRefPtr<Expression const> m_key;
    NonnullRefPtr<FunctionExpression const> m_function;
    Kind m_kind;
};

class ClassField final : public ClassElement {
public:
    ClassField(SourceRange source_range, NonnullRefPtr<Expression const> key, RefPtr<Expression const> init, bool contains_direct_call_to_eval, bool is_static)
        : ClassElement(move(source_range), is_static)
        , m_key(move(key))
        , m_initializer(move(init))
        , m_contains_direct_call_to_eval(contains_direct_call_to_eval)
    {
    }

    Expression const& key() const { return *m_key; }
    RefPtr<Expression const> const& initializer() const { return m_initializer; }
    RefPtr<Expression const>& initializer() { return m_initializer; }

    virtual ElementKind class_element_kind() const override { return ElementKind::Field; }

    virtual void dump(int indent) const override;
    virtual ThrowCompletionOr<ClassValue> class_element_evaluation(VM&, Object& home_object, Value property_key) const override;
    virtual Optional<FlyString> private_bound_identifier() const override;

private:
    NonnullRefPtr<Expression const> m_key;
    RefPtr<Expression const> m_initializer;
    bool m_contains_direct_call_to_eval { false };
};

class StaticInitializer final : public ClassElement {
public:
    StaticInitializer(SourceRange source_range, NonnullRefPtr<FunctionBody> function_body, bool contains_direct_call_to_eval)
        : ClassElement(move(source_range), true)
        , m_function_body(move(function_body))
        , m_contains_direct_call_to_eval(contains_direct_call_to_eval)
    {
    }

    virtual ElementKind class_element_kind() const override { return ElementKind::StaticInitializer; }
    virtual ThrowCompletionOr<ClassValue> class_element_evaluation(VM&, Object& home_object, Value property_key) const override;

    virtual void dump(int indent) const override;

private:
    NonnullRefPtr<FunctionBody> m_function_body;
    bool m_contains_direct_call_to_eval { false };
};

class SuperExpression final : public Expression {
public:
    explicit SuperExpression(SourceRange source_range)
        : Expression(move(source_range))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    virtual bool is_super_expression() const override { return true; }
};

class ClassExpression final : public Expression {
public:
    ClassExpression(SourceRange source_range, RefPtr<Identifier const> name, ByteString source_text, RefPtr<FunctionExpression const> constructor, RefPtr<Expression const> super_class, Vector<NonnullRefPtr<ClassElement const>> elements)
        : Expression(move(source_range))
        , m_name(move(name))
        , m_source_text(move(source_text))
        , m_constructor(move(constructor))
        , m_super_class(move(super_class))
        , m_elements(move(elements))
    {
    }

    FlyString name() const { return m_name ? m_name->string() : ""_fly_string; }

    ByteString const& source_text() const { return m_source_text; }
    RefPtr<FunctionExpression const> constructor() const { return m_constructor; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode_with_lhs_name(Bytecode::Generator&, Optional<Bytecode::IdentifierTableIndex> lhs_name, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const;

    bool has_name() const { return m_name; }

    ThrowCompletionOr<ECMAScriptFunctionObject*> create_class_constructor(VM&, Environment* class_environment, Environment* environment, Value super_class, ReadonlySpan<Value> element_keys, Optional<FlyString> const& binding_name = {}, FlyString const& class_name = {}) const;

private:
    virtual bool is_class_expression() const override { return true; }

    friend ClassDeclaration;

    RefPtr<Identifier const> m_name;
    ByteString m_source_text;
    RefPtr<FunctionExpression const> m_constructor;
    RefPtr<Expression const> m_super_class;
    Vector<NonnullRefPtr<ClassElement const>> m_elements;
};

class ClassDeclaration final : public Declaration {
public:
    ClassDeclaration(SourceRange source_range, NonnullRefPtr<ClassExpression const> class_expression)
        : Declaration(move(source_range))
        , m_class_expression(move(class_expression))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    ThrowCompletionOr<void> for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&&) const override;

    virtual bool is_lexical_declaration() const override { return true; }

    FlyString name() const { return m_class_expression->name(); }

private:
    virtual bool is_class_declaration() const override { return true; }

    friend ExportStatement;

    NonnullRefPtr<ClassExpression const> m_class_expression;
};

// We use this class to mimic  Initializer : = AssignmentExpression of
// 10.2.1.3 Runtime Semantics: EvaluateBody, https://tc39.es/ecma262/#sec-runtime-semantics-evaluatebody
class ClassFieldInitializerStatement final : public Statement {
public:
    ClassFieldInitializerStatement(SourceRange source_range, NonnullRefPtr<Expression const> expression, FlyString field_name)
        : Statement(move(source_range))
        , m_expression(move(expression))
        , m_class_field_identifier_name(move(field_name))
    {
    }

    virtual void dump(int) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_expression;
    FlyString m_class_field_identifier_name; // [[ClassFieldIdentifierName]]
};

class SpreadExpression final : public Expression {
public:
    explicit SpreadExpression(SourceRange source_range, NonnullRefPtr<Expression const> target)
        : Expression(move(source_range))
        , m_target(move(target))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_target;
};

class ThisExpression final : public Expression {
public:
    explicit ThisExpression(SourceRange source_range)
        : Expression(move(source_range))
    {
    }
    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
};

struct CallExpressionArgument {
    NonnullRefPtr<Expression const> value;
    bool is_spread;
};

enum InvocationStyleEnum {
    Parenthesized,
    NotParenthesized,
};

enum InsideParenthesesEnum {
    InsideParentheses,
    NotInsideParentheses,
};

class CallExpression : public ASTNodeWithTailArray<CallExpression, Expression, CallExpressionArgument> {
    friend class ASTNodeWithTailArray;

    InvocationStyleEnum m_invocation_style;
    InsideParenthesesEnum m_inside_parentheses;

public:
    using Argument = CallExpressionArgument;

    static NonnullRefPtr<CallExpression> create(SourceRange, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens);

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    Expression const& callee() const { return m_callee; }

    ReadonlySpan<Argument> arguments() const { return tail_span(); }

    bool is_parenthesized() const { return m_invocation_style == InvocationStyleEnum::Parenthesized; }
    bool is_inside_parens() const { return m_inside_parentheses == InsideParenthesesEnum::InsideParentheses; }
    void set_inside_parens() { m_inside_parentheses = InsideParenthesesEnum::InsideParentheses; }

protected:
    CallExpression(SourceRange source_range, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens = InsideParenthesesEnum::NotInsideParentheses)
        : ASTNodeWithTailArray(move(source_range), arguments)
        , m_invocation_style(invocation_style)
        , m_inside_parentheses(inside_parens)
        , m_callee(move(callee))
    {
    }

    virtual bool is_call_expression() const override { return true; }

    Optional<String> expression_string() const;

    NonnullRefPtr<Expression const> m_callee;
};

class NewExpression final : public CallExpression {
    friend class ASTNodeWithTailArray;

public:
    static NonnullRefPtr<NewExpression> create(SourceRange, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens);

    virtual bool is_new_expression() const override { return true; }

private:
    NewExpression(SourceRange source_range, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens)
        : CallExpression(move(source_range), move(callee), arguments, invocation_style, inside_parens)
    {
    }
};

static_assert(sizeof(NewExpression) == sizeof(CallExpression), "Adding members to NewExpression will break CallExpression memory layout");

class SuperCall final : public Expression {
public:
    // This is here to be able to make a constructor like
    // constructor(...args) { super(...args); } which does not use @@iterator of %Array.prototype%.
    enum class IsPartOfSyntheticConstructor {
        No,
        Yes,
    };

    SuperCall(SourceRange source_range, Vector<CallExpression::Argument> arguments)
        : Expression(move(source_range))
        , m_arguments(move(arguments))
        , m_is_synthetic(IsPartOfSyntheticConstructor::No)
    {
    }

    SuperCall(SourceRange source_range, IsPartOfSyntheticConstructor is_part_of_synthetic_constructor, CallExpression::Argument constructor_argument)
        : Expression(move(source_range))
        , m_arguments({ move(constructor_argument) })
        , m_is_synthetic(IsPartOfSyntheticConstructor::Yes)
    {
        VERIFY(is_part_of_synthetic_constructor == IsPartOfSyntheticConstructor::Yes);
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    Vector<CallExpression::Argument> const m_arguments;
    IsPartOfSyntheticConstructor const m_is_synthetic;
};

enum class AssignmentOp {
    Assignment,
    AdditionAssignment,
    SubtractionAssignment,
    MultiplicationAssignment,
    DivisionAssignment,
    ModuloAssignment,
    ExponentiationAssignment,
    BitwiseAndAssignment,
    BitwiseOrAssignment,
    BitwiseXorAssignment,
    LeftShiftAssignment,
    RightShiftAssignment,
    UnsignedRightShiftAssignment,
    AndAssignment,
    OrAssignment,
    NullishAssignment,
};

class AssignmentExpression final : public Expression {
public:
    AssignmentExpression(SourceRange source_range, AssignmentOp op, NonnullRefPtr<Expression const> lhs, NonnullRefPtr<Expression const> rhs)
        : Expression(move(source_range))
        , m_op(op)
        , m_lhs(move(lhs))
        , m_rhs(move(rhs))
    {
    }

    AssignmentExpression(SourceRange source_range, AssignmentOp op, NonnullRefPtr<BindingPattern const> lhs, NonnullRefPtr<Expression const> rhs)
        : Expression(move(source_range))
        , m_op(op)
        , m_lhs(move(lhs))
        , m_rhs(move(rhs))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    AssignmentOp m_op;
    Variant<NonnullRefPtr<Expression const>, NonnullRefPtr<BindingPattern const>> m_lhs;
    NonnullRefPtr<Expression const> m_rhs;
};

enum class UpdateOp {
    Increment,
    Decrement,
};

class UpdateExpression final : public Expression {
public:
    UpdateExpression(SourceRange source_range, UpdateOp op, NonnullRefPtr<Expression const> argument, bool prefixed = false)
        : Expression(move(source_range))
        , m_op(op)
        , m_argument(move(argument))
        , m_prefixed(prefixed)
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    virtual bool is_update_expression() const override { return true; }

    UpdateOp m_op;
    NonnullRefPtr<Expression const> m_argument;
    bool m_prefixed;
};

enum class DeclarationKind {
    Var,
    Let,
    Const,
};

class VariableDeclarator final : public ASTNode {
public:
    VariableDeclarator(SourceRange source_range, NonnullRefPtr<Identifier const> id)
        : ASTNode(move(source_range))
        , m_target(move(id))
    {
    }

    VariableDeclarator(SourceRange source_range, NonnullRefPtr<Identifier const> target, RefPtr<Expression const> init)
        : ASTNode(move(source_range))
        , m_target(move(target))
        , m_init(move(init))
    {
    }

    VariableDeclarator(SourceRange source_range, Variant<NonnullRefPtr<Identifier const>, NonnullRefPtr<BindingPattern const>> target, RefPtr<Expression const> init)
        : ASTNode(move(source_range))
        , m_target(move(target))
        , m_init(move(init))
    {
    }

    auto& target() const { return m_target; }
    Expression const* init() const { return m_init; }

    virtual void dump(int indent) const override;

private:
    Variant<NonnullRefPtr<Identifier const>, NonnullRefPtr<BindingPattern const>> m_target;
    RefPtr<Expression const> m_init;
};

class VariableDeclaration final : public Declaration {
public:
    VariableDeclaration(SourceRange source_range, DeclarationKind declaration_kind, Vector<NonnullRefPtr<VariableDeclarator const>> declarations)
        : Declaration(move(source_range))
        , m_declaration_kind(declaration_kind)
        , m_declarations(move(declarations))
    {
    }

    DeclarationKind declaration_kind() const { return m_declaration_kind; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    Vector<NonnullRefPtr<VariableDeclarator const>> const& declarations() const { return m_declarations; }

    ThrowCompletionOr<void> for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&&) const override;

    virtual bool is_constant_declaration() const override { return m_declaration_kind == DeclarationKind::Const; }

    virtual bool is_lexical_declaration() const override { return m_declaration_kind != DeclarationKind::Var; }

private:
    virtual bool is_variable_declaration() const override { return true; }

    DeclarationKind m_declaration_kind;
    Vector<NonnullRefPtr<VariableDeclarator const>> m_declarations;
};

class UsingDeclaration final : public Declaration {
public:
    UsingDeclaration(SourceRange source_range, Vector<NonnullRefPtr<VariableDeclarator const>> declarations)
        : Declaration(move(source_range))
        , m_declarations(move(declarations))
    {
    }

    virtual void dump(int indent) const override;

    ThrowCompletionOr<void> for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&&) const override;

    virtual bool is_constant_declaration() const override { return true; }

    virtual bool is_lexical_declaration() const override { return true; }

    Vector<NonnullRefPtr<VariableDeclarator const>> const& declarations() const { return m_declarations; }

private:
    Vector<NonnullRefPtr<VariableDeclarator const>> m_declarations;
};

class ObjectProperty final : public ASTNode {
public:
    enum class Type : u8 {
        KeyValue,
        Getter,
        Setter,
        Spread,
        ProtoSetter,
    };

    ObjectProperty(SourceRange source_range, NonnullRefPtr<Expression const> key, RefPtr<Expression const> value, Type property_type, bool is_method)
        : ASTNode(move(source_range))
        , m_property_type(property_type)
        , m_is_method(is_method)
        , m_key(move(key))
        , m_value(move(value))
    {
    }

    Expression const& key() const { return m_key; }
    Expression const& value() const
    {
        VERIFY(m_value);
        return *m_value;
    }

    Type type() const { return m_property_type; }
    bool is_method() const { return m_is_method; }

    virtual void dump(int indent) const override;

private:
    Type m_property_type;
    bool m_is_method { false };
    NonnullRefPtr<Expression const> m_key;
    RefPtr<Expression const> m_value;
};

class ObjectExpression final : public Expression {
public:
    explicit ObjectExpression(SourceRange source_range, Vector<NonnullRefPtr<ObjectProperty>> properties = {})
        : Expression(move(source_range))
        , m_properties(move(properties))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    virtual bool is_object_expression() const override { return true; }

    Vector<NonnullRefPtr<ObjectProperty>> m_properties;
};

class ArrayExpression final : public Expression {
public:
    ArrayExpression(SourceRange source_range, Vector<RefPtr<Expression const>> elements)
        : Expression(move(source_range))
        , m_elements(move(elements))
    {
    }

    Vector<RefPtr<Expression const>> const& elements() const { return m_elements; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    virtual bool is_array_expression() const override { return true; }

    Vector<RefPtr<Expression const>> m_elements;
};

class TemplateLiteral final : public Expression {
public:
    TemplateLiteral(SourceRange source_range, Vector<NonnullRefPtr<Expression const>> expressions)
        : Expression(move(source_range))
        , m_expressions(move(expressions))
    {
    }

    TemplateLiteral(SourceRange source_range, Vector<NonnullRefPtr<Expression const>> expressions, Vector<NonnullRefPtr<Expression const>> raw_strings)
        : Expression(move(source_range))
        , m_expressions(move(expressions))
        , m_raw_strings(move(raw_strings))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    Vector<NonnullRefPtr<Expression const>> const& expressions() const { return m_expressions; }
    Vector<NonnullRefPtr<Expression const>> const& raw_strings() const { return m_raw_strings; }

private:
    Vector<NonnullRefPtr<Expression const>> const m_expressions;
    Vector<NonnullRefPtr<Expression const>> const m_raw_strings;
};

class TaggedTemplateLiteral final : public Expression {
public:
    TaggedTemplateLiteral(SourceRange source_range, NonnullRefPtr<Expression const> tag, NonnullRefPtr<TemplateLiteral const> template_literal)
        : Expression(move(source_range))
        , m_tag(move(tag))
        , m_template_literal(move(template_literal))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> const m_tag;
    NonnullRefPtr<TemplateLiteral const> const m_template_literal;
};

class MemberExpression final : public Expression {
public:
    MemberExpression(SourceRange source_range, NonnullRefPtr<Expression const> object, NonnullRefPtr<Expression const> property, bool computed = false)
        : Expression(move(source_range))
        , m_computed(computed)
        , m_object(move(object))
        , m_property(move(property))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    bool is_computed() const { return m_computed; }
    Expression const& object() const { return *m_object; }
    Expression const& property() const { return *m_property; }

    [[nodiscard]] String to_string_approximation() const;

    bool ends_in_private_name() const;

private:
    virtual bool is_member_expression() const override { return true; }

    bool m_computed { false };
    NonnullRefPtr<Expression const> m_object;
    NonnullRefPtr<Expression const> m_property;
};

class OptionalChain final : public Expression {
public:
    enum class Mode {
        Optional,
        NotOptional,
    };

    struct Call {
        Vector<CallExpression::Argument> arguments;
        Mode mode;
    };
    struct ComputedReference {
        NonnullRefPtr<Expression const> expression;
        Mode mode;
    };
    struct MemberReference {
        NonnullRefPtr<Identifier const> identifier;
        Mode mode;
    };
    struct PrivateMemberReference {
        NonnullRefPtr<PrivateIdentifier const> private_identifier;
        Mode mode;
    };

    using Reference = Variant<Call, ComputedReference, MemberReference, PrivateMemberReference>;

    OptionalChain(SourceRange source_range, NonnullRefPtr<Expression const> base, Vector<Reference> references)
        : Expression(move(source_range))
        , m_base(move(base))
        , m_references(move(references))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    Expression const& base() const { return *m_base; }
    Vector<Reference> const& references() const { return m_references; }

private:
    NonnullRefPtr<Expression const> m_base;
    Vector<Reference> m_references;
};

class MetaProperty final : public Expression {
public:
    enum class Type {
        NewTarget,
        ImportMeta,
    };

    MetaProperty(SourceRange source_range, Type type)
        : Expression(move(source_range))
        , m_type(type)
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    Type m_type;
};

class ImportCall final : public Expression {
public:
    ImportCall(SourceRange source_range, NonnullRefPtr<Expression const> specifier, RefPtr<Expression const> options)
        : Expression(move(source_range))
        , m_specifier(move(specifier))
        , m_options(move(options))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    virtual bool is_import_call() const override { return true; }

    NonnullRefPtr<Expression const> m_specifier;
    RefPtr<Expression const> m_options;
};

class ConditionalExpression final : public Expression {
public:
    ConditionalExpression(SourceRange source_range, NonnullRefPtr<Expression const> test, NonnullRefPtr<Expression const> consequent, NonnullRefPtr<Expression const> alternate)
        : Expression(move(source_range))
        , m_test(move(test))
        , m_consequent(move(consequent))
        , m_alternate(move(alternate))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_test;
    NonnullRefPtr<Expression const> m_consequent;
    NonnullRefPtr<Expression const> m_alternate;
};

class CatchClause final : public ASTNode {
public:
    CatchClause(SourceRange source_range, FlyString parameter, NonnullRefPtr<BlockStatement const> body)
        : ASTNode(move(source_range))
        , m_parameter(move(parameter))
        , m_body(move(body))
    {
    }

    CatchClause(SourceRange source_range, NonnullRefPtr<BindingPattern const> parameter, NonnullRefPtr<BlockStatement const> body)
        : ASTNode(move(source_range))
        , m_parameter(move(parameter))
        , m_body(move(body))
    {
    }

    auto& parameter() const { return m_parameter; }
    BlockStatement const& body() const { return m_body; }

    virtual void dump(int indent) const override;

private:
    Variant<FlyString, NonnullRefPtr<BindingPattern const>> m_parameter;
    NonnullRefPtr<BlockStatement const> m_body;
};

class TryStatement final : public Statement {
public:
    TryStatement(SourceRange source_range, NonnullRefPtr<BlockStatement const> block, RefPtr<CatchClause const> handler, RefPtr<BlockStatement const> finalizer)
        : Statement(move(source_range))
        , m_block(move(block))
        , m_handler(move(handler))
        , m_finalizer(move(finalizer))
    {
    }

    BlockStatement const& block() const { return m_block; }
    CatchClause const* handler() const { return m_handler; }
    BlockStatement const* finalizer() const { return m_finalizer; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<BlockStatement const> m_block;
    RefPtr<CatchClause const> m_handler;
    RefPtr<BlockStatement const> m_finalizer;
};

class ThrowStatement final : public Statement {
public:
    explicit ThrowStatement(SourceRange source_range, NonnullRefPtr<Expression const> argument)
        : Statement(move(source_range))
        , m_argument(move(argument))
    {
    }

    Expression const& argument() const { return m_argument; }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    NonnullRefPtr<Expression const> m_argument;
};

class SwitchCase final : public ScopeNode {
public:
    SwitchCase(SourceRange source_range, RefPtr<Expression const> test)
        : ScopeNode(move(source_range))
        , m_test(move(test))
    {
    }

    Expression const* test() const { return m_test; }

    virtual void dump(int indent) const override;

private:
    RefPtr<Expression const> m_test;
};

class SwitchStatement final : public ScopeNode {
public:
    SwitchStatement(SourceRange source_range, NonnullRefPtr<Expression const> discriminant)
        : ScopeNode(move(source_range))
        , m_discriminant(move(discriminant))
    {
    }

    virtual void dump(int indent) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_labelled_evaluation(Bytecode::Generator&, Vector<FlyString> const&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const;

    void add_case(NonnullRefPtr<SwitchCase const> switch_case) { m_cases.append(move(switch_case)); }

private:
    NonnullRefPtr<Expression const> m_discriminant;
    Vector<NonnullRefPtr<SwitchCase const>> m_cases;
};

class BreakStatement final : public Statement {
public:
    BreakStatement(SourceRange source_range, Optional<FlyString> target_label)
        : Statement(move(source_range))
        , m_target_label(move(target_label))
    {
    }

    Optional<FlyString> const& target_label() const { return m_target_label; }
    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

private:
    Optional<FlyString> m_target_label;
};

class ContinueStatement final : public Statement {
public:
    ContinueStatement(SourceRange source_range, Optional<FlyString> target_label)
        : Statement(move(source_range))
        , m_target_label(move(target_label))
    {
    }

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;

    Optional<FlyString> const& target_label() const { return m_target_label; }

private:
    Optional<FlyString> m_target_label;
};

class DebuggerStatement final : public Statement {
public:
    explicit DebuggerStatement(SourceRange source_range)
        : Statement(move(source_range))
    {
    }

    virtual Bytecode::CodeGenerationErrorOr<Optional<Bytecode::ScopedOperand>> generate_bytecode(Bytecode::Generator&, Optional<Bytecode::ScopedOperand> preferred_dst = {}) const override;
};

class SyntheticReferenceExpression final : public Expression {
public:
    explicit SyntheticReferenceExpression(SourceRange source_range, Reference reference, Value value)
        : Expression(move(source_range))
        , m_reference(move(reference))
        , m_value(value)
    {
    }

private:
    Reference m_reference;
    Value m_value;
};

template<>
inline bool ASTNode::fast_is<NewExpression>() const { return is_new_expression(); }

template<>
inline bool ASTNode::fast_is<MemberExpression>() const { return is_member_expression(); }

template<>
inline bool ASTNode::fast_is<SuperExpression>() const { return is_super_expression(); }

template<>
inline bool ASTNode::fast_is<FunctionExpression>() const { return is_function_expression(); }

template<>
inline bool ASTNode::fast_is<ClassExpression>() const { return is_class_expression(); }

template<>
inline bool ASTNode::fast_is<Identifier>() const { return is_identifier(); }

template<>
inline bool ASTNode::fast_is<PrivateIdentifier>() const { return is_private_identifier(); }

template<>
inline bool ASTNode::fast_is<ExpressionStatement>() const { return is_expression_statement(); }

template<>
inline bool ASTNode::fast_is<ScopeNode>() const { return is_scope_node(); }

template<>
inline bool ASTNode::fast_is<Program>() const { return is_program(); }

template<>
inline bool ASTNode::fast_is<ClassDeclaration>() const { return is_class_declaration(); }

template<>
inline bool ASTNode::fast_is<FunctionDeclaration>() const { return is_function_declaration(); }

template<>
inline bool ASTNode::fast_is<VariableDeclaration>() const { return is_variable_declaration(); }

template<>
inline bool ASTNode::fast_is<ArrayExpression>() const { return is_array_expression(); }

template<>
inline bool ASTNode::fast_is<ObjectExpression>() const { return is_object_expression(); }

template<>
inline bool ASTNode::fast_is<ImportCall>() const { return is_import_call(); }

template<>
inline bool ASTNode::fast_is<StringLiteral>() const { return is_string_literal(); }

template<>
inline bool ASTNode::fast_is<UpdateExpression>() const { return is_update_expression(); }

template<>
inline bool ASTNode::fast_is<CallExpression>() const { return is_call_expression(); }

template<>
inline bool ASTNode::fast_is<LabelledStatement>() const { return is_labelled_statement(); }

template<>
inline bool ASTNode::fast_is<IterationStatement>() const { return is_iteration_statement(); }

template<>
inline bool ASTNode::fast_is<ClassMethod>() const { return is_class_method(); }

}
