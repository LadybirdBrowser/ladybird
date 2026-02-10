/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/TemporaryChange.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/AST.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <typeinfo>

namespace JS {

ASTNode::ASTNode(SourceRange source_range)
    : m_source_range(move(source_range))
{
}

ByteString ASTNode::class_name() const
{
    // NOTE: We strip the "JS::" prefix.
    auto const* typename_ptr = typeid(*this).name();
    return demangle({ typename_ptr, strlen(typename_ptr) }).substring(4);
}

Optional<Utf16String> CallExpression::expression_string() const
{
    if (is<Identifier>(*m_callee))
        return static_cast<Identifier const&>(*m_callee).string().to_utf16_string();

    if (is<MemberExpression>(*m_callee))
        return static_cast<MemberExpression const&>(*m_callee).to_string_approximation();

    return {};
}

static Optional<Utf16FlyString> nullopt_or_private_identifier_description(Expression const& expression)
{
    if (is<PrivateIdentifier>(expression))
        return static_cast<PrivateIdentifier const&>(expression).string();
    return {};
}

Optional<Utf16FlyString> ClassField::private_bound_identifier() const
{
    return nullopt_or_private_identifier_description(*m_key);
}

Optional<Utf16FlyString> ClassMethod::private_bound_identifier() const
{
    return nullopt_or_private_identifier_description(*m_key);
}

ThrowCompletionOr<void> ClassDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    if (!m_class_expression->m_name)
        return {};

    return callback(*m_class_expression->m_name);
}

bool BindingPattern::contains_expression() const
{
    for (auto& entry : entries) {
        if (entry.name.has<NonnullRefPtr<Expression const>>())
            return true;
        if (entry.initializer)
            return true;
        if (auto binding_ptr = entry.alias.get_pointer<NonnullRefPtr<BindingPattern const>>(); binding_ptr && (*binding_ptr)->contains_expression())
            return true;
    }
    return false;
}

ThrowCompletionOr<void> BindingPattern::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& entry : entries) {
        auto const& alias = entry.alias;
        if (alias.has<NonnullRefPtr<Identifier const>>()) {
            TRY(callback(alias.get<NonnullRefPtr<Identifier const>>()));
        } else if (alias.has<NonnullRefPtr<BindingPattern const>>()) {
            TRY(alias.get<NonnullRefPtr<BindingPattern const>>()->for_each_bound_identifier(forward<decltype(callback)>(callback)));
        } else {
            auto const& name = entry.name;
            if (name.has<NonnullRefPtr<Identifier const>>())
                TRY(callback(name.get<NonnullRefPtr<Identifier const>>()));
        }
    }
    return {};
}

FunctionNode::FunctionNode(RefPtr<Identifier const> name, Utf16View source_text, NonnullRefPtr<Statement const> body, NonnullRefPtr<FunctionParameters const> parameters, i32 function_length, FunctionKind kind, bool is_strict_mode, FunctionParsingInsights parsing_insights, bool is_arrow_function)
    : m_name(move(name))
    , m_source_text(move(source_text))
    , m_body(move(body))
    , m_parameters(move(parameters))
    , m_function_length(function_length)
    , m_kind(kind)
    , m_is_strict_mode(is_strict_mode)
    , m_is_arrow_function(is_arrow_function)
    , m_parsing_insights(parsing_insights)
{
    if (m_is_arrow_function)
        VERIFY(!parsing_insights.might_need_arguments_object);
}

FunctionNode::~FunctionNode() = default;

void FunctionNode::set_shared_data(GC::Ptr<SharedFunctionInstanceData> shared_data) const
{
    m_shared_data = move(shared_data);
}

GC::Ptr<SharedFunctionInstanceData> FunctionNode::shared_data() const
{
    return m_shared_data.ptr();
}

GC::Ref<SharedFunctionInstanceData> FunctionNode::ensure_shared_data(VM& vm) const
{
    if (auto data = shared_data())
        return *data;

    auto data = vm.heap().allocate<SharedFunctionInstanceData>(
        vm,
        kind(),
        name(),
        function_length(),
        parameters(),
        *body_ptr(),
        source_text(),
        is_strict_mode(),
        is_arrow_function(),
        parsing_insights(),
        local_variables_names());
    set_shared_data(data);
    return *data;
}

ThrowCompletionOr<void> FunctionDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    if (!m_name)
        return {};
    return callback(*m_name);
}

ThrowCompletionOr<void> VariableDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& entry : declarations()) {
        TRY(entry->target().visit(
            [&](NonnullRefPtr<Identifier const> const& id) {
                return callback(id);
            },
            [&](NonnullRefPtr<BindingPattern const> const& binding) {
                return binding->for_each_bound_identifier([&](auto const& id) {
                    return callback(id);
                });
            }));
    }

    return {};
}

ThrowCompletionOr<void> UsingDeclaration::for_each_bound_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& entry : m_declarations) {
        VERIFY(entry->target().has<NonnullRefPtr<Identifier const>>());
        TRY(callback(entry->target().get<NonnullRefPtr<Identifier const>>()));
    }

    return {};
}

Utf16String MemberExpression::to_string_approximation() const
{
    Utf16View object_string = "<object>"sv;
    if (is<Identifier>(*m_object))
        object_string = static_cast<Identifier const&>(*m_object).string().view();

    if (is_computed())
        return Utf16String::formatted("{}[<computed>]", object_string);
    if (is<PrivateIdentifier>(*m_property))
        return Utf16String::formatted("{}.{}", object_string, as<PrivateIdentifier>(*m_property).string());
    return Utf16String::formatted("{}.{}", object_string, as<Identifier>(*m_property).string());
}

bool MemberExpression::ends_in_private_name() const
{
    if (is_computed())
        return false;
    if (is<PrivateIdentifier>(*m_property))
        return true;
    if (is<MemberExpression>(*m_property))
        return static_cast<MemberExpression const&>(*m_property).ends_in_private_name();
    return false;
}

bool ScopeNode::has_non_local_lexical_declarations() const
{
    bool result = false;
    MUST(for_each_lexically_declared_identifier([&](Identifier const& identifier) {
        if (!identifier.is_local())
            result = true;
    }));
    return result;
}

ThrowCompletionOr<void> ScopeNode::for_each_lexically_scoped_declaration(ThrowCompletionOrVoidCallback<Declaration const&>&& callback) const
{
    for (auto& declaration : m_lexical_declarations)
        TRY(callback(declaration));

    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_lexically_declared_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto const& declaration : m_lexical_declarations) {
        TRY(declaration->for_each_bound_identifier([&](auto const& identifier) {
            return callback(identifier);
        }));
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_var_declared_identifier(ThrowCompletionOrVoidCallback<Identifier const&>&& callback) const
{
    for (auto& declaration : m_var_declarations) {
        TRY(declaration->for_each_bound_identifier([&](auto const& id) {
            return callback(id);
        }));
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_var_function_declaration_in_reverse_order(ThrowCompletionOrVoidCallback<FunctionDeclaration const&>&& callback) const
{
    for (ssize_t i = m_var_declarations.size() - 1; i >= 0; i--) {
        auto& declaration = m_var_declarations[i];
        if (is<FunctionDeclaration>(declaration))
            TRY(callback(static_cast<FunctionDeclaration const&>(*declaration)));
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_var_scoped_variable_declaration(ThrowCompletionOrVoidCallback<VariableDeclaration const&>&& callback) const
{
    for (auto& declaration : m_var_declarations) {
        if (!is<FunctionDeclaration>(declaration)) {
            VERIFY(is<VariableDeclaration>(declaration));
            TRY(callback(static_cast<VariableDeclaration const&>(*declaration)));
        }
    }
    return {};
}

ThrowCompletionOr<void> ScopeNode::for_each_function_hoistable_with_annexB_extension(ThrowCompletionOrVoidCallback<FunctionDeclaration&>&& callback) const
{
    for (auto& function : m_functions_hoistable_with_annexB_extension) {
        // We need const_cast here since it might have to set a property on function declaration.
        TRY(callback(const_cast<FunctionDeclaration&>(*function)));
    }
    return {};
}

void ScopeNode::add_lexical_declaration(NonnullRefPtr<Declaration const> declaration)
{
    m_lexical_declarations.append(move(declaration));
}

void ScopeNode::add_var_scoped_declaration(NonnullRefPtr<Declaration const> declaration)
{
    m_var_declarations.append(move(declaration));
}

void ScopeNode::add_hoisted_function(NonnullRefPtr<FunctionDeclaration const> declaration)
{
    m_functions_hoistable_with_annexB_extension.append(move(declaration));
}

void ScopeNode::ensure_function_scope_data() const
{
    if (m_function_scope_data)
        return;

    auto data = make<FunctionScopeData>();

    // Extract functions_to_initialize from var-scoped function declarations (in reverse order, deduplicated).
    HashTable<Utf16FlyString> seen_function_names;
    for (ssize_t i = m_var_declarations.size() - 1; i >= 0; i--) {
        auto const& declaration = m_var_declarations[i];
        if (is<FunctionDeclaration>(declaration)) {
            auto& function_decl = static_cast<FunctionDeclaration const&>(*declaration);
            if (seen_function_names.set(function_decl.name()) == AK::HashSetResult::InsertedNewEntry)
                data->functions_to_initialize.append(static_ptr_cast<FunctionDeclaration const>(declaration));
        }
    }

    data->has_function_named_arguments = seen_function_names.contains("arguments"_utf16_fly_string);

    // Check if "arguments" is lexically declared.
    MUST(for_each_lexically_declared_identifier([&](auto const& identifier) {
        if (identifier.string() == "arguments"_utf16_fly_string)
            data->has_lexically_declared_arguments = true;
    }));

    // Extract vars_to_initialize from var declarations.
    HashTable<Utf16FlyString> seen_var_names;
    MUST(for_each_var_declared_identifier([&](Identifier const& identifier) {
        auto const& name = identifier.string();
        if (seen_var_names.set(name) == AK::HashSetResult::InsertedNewEntry) {
            data->vars_to_initialize.append({
                .identifier = identifier,
                .is_parameter = false,
                .is_function_name = seen_function_names.contains(name),
            });

            data->var_names.set(name);

            if (!identifier.is_local()) {
                data->non_local_var_count++;
                data->non_local_var_count_for_parameter_expressions++;
            }
        }
    }));

    m_function_scope_data = move(data);
}

Utf16FlyString ExportStatement::local_name_for_default = "*default*"_utf16_fly_string;

bool ExportStatement::has_export(Utf16FlyString const& export_name) const
{
    return m_entries.contains([&](auto& entry) {
        // Make sure that empty exported names does not overlap with anything
        if (entry.kind != ExportEntry::Kind::NamedExport)
            return false;
        return entry.export_name == export_name;
    });
}

bool ImportStatement::has_bound_name(Utf16FlyString const& name) const
{
    return m_entries.contains([&](auto& entry) { return entry.local_name == name; });
}

// 16.1.7 GlobalDeclarationInstantiation ( script, env ), https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
ThrowCompletionOr<void> Program::global_declaration_instantiation(VM& vm, GlobalEnvironment& global_environment) const
{
    auto& realm = *vm.current_realm();

    // 1. Let lexNames be the LexicallyDeclaredNames of script.
    // 2. Let varNames be the VarDeclaredNames of script.
    // 3. For each element name of lexNames, do
    TRY(for_each_lexically_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
        auto const& name = identifier.string();

        // a. If HasLexicalDeclaration(env, name) is true, throw a SyntaxError exception.
        if (global_environment.has_lexical_declaration(name))
            return vm.throw_completion<SyntaxError>(ErrorType::TopLevelVariableAlreadyDeclared, name);

        // b. Let hasRestrictedGlobal be ? HasRestrictedGlobalProperty(env, name).
        auto has_restricted_global = TRY(global_environment.has_restricted_global_property(name));

        // c. NOTE: Global var and function bindings (except those that are introduced by non-strict direct eval) are
        //    non-configurable and are therefore restricted global properties.

        // d. If hasRestrictedGlobal is true, throw a SyntaxError exception.
        if (has_restricted_global)
            return vm.throw_completion<SyntaxError>(ErrorType::RestrictedGlobalProperty, name);

        return {};
    }));

    // 4. For each element name of varNames, do
    TRY(for_each_var_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
        // a. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
        if (global_environment.has_lexical_declaration(identifier.string()))
            return vm.throw_completion<SyntaxError>(ErrorType::TopLevelVariableAlreadyDeclared, identifier.string());

        return {};
    }));

    // 5. Let varDeclarations be the VarScopedDeclarations of script.
    // 6. Let functionsToInitialize be a new empty List.
    Vector<FunctionDeclaration const&> functions_to_initialize;

    // 7. Let declaredFunctionNames be a new empty List.
    HashTable<Utf16FlyString> declared_function_names;

    // 8. For each element d of varDeclarations, in reverse List order, do

    TRY(for_each_var_function_declaration_in_reverse_order([&](FunctionDeclaration const& function) -> ThrowCompletionOr<void> {
        auto function_name = function.name();

        // a. If d is neither a VariableDeclaration nor a ForBinding nor a BindingIdentifier, then
        // i. Assert: d is either a FunctionDeclaration, a GeneratorDeclaration, an AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration.
        // Note: This is checked in for_each_var_function_declaration_in_reverse_order.

        // ii. NOTE: If there are multiple function declarations for the same name, the last declaration is used.

        // iii. Let fn be the sole element of the BoundNames of d.

        // iv. If fn is not an element of declaredFunctionNames, then
        if (declared_function_names.set(function_name) != AK::HashSetResult::InsertedNewEntry)
            return {};

        // 1. Let fnDefinable be ? env.CanDeclareGlobalFunction(fn).
        auto function_definable = TRY(global_environment.can_declare_global_function(function_name));

        // 2. If fnDefinable is false, throw a TypeError exception.
        if (!function_definable)
            return vm.throw_completion<TypeError>(ErrorType::CannotDeclareGlobalFunction, function_name);

        // 3. Append fn to declaredFunctionNames.
        // Note: Already done in step iv. above.

        // 4. Insert d as the first element of functionsToInitialize.
        // NOTE: Since prepending is much slower, we just append
        //       and iterate in reverse order in step 16 below.
        functions_to_initialize.append(function);
        return {};
    }));

    // 9. Let declaredVarNames be a new empty List.
    HashTable<Utf16FlyString> declared_var_names;

    // 10. For each element d of varDeclarations, do
    TRY(for_each_var_scoped_variable_declaration([&](Declaration const& declaration) {
        // a. If d is a VariableDeclaration, a ForBinding, or a BindingIdentifier, then
        // Note: This is done in for_each_var_scoped_variable_declaration.

        // i. For each String vn of the BoundNames of d, do
        return declaration.for_each_bound_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
            auto const& name = identifier.string();

            // 1. If vn is not an element of declaredFunctionNames, then
            if (declared_function_names.contains(name))
                return {};

            // a. Let vnDefinable be ? env.CanDeclareGlobalVar(vn).
            auto var_definable = TRY(global_environment.can_declare_global_var(name));

            // b. If vnDefinable is false, throw a TypeError exception.
            if (!var_definable)
                return vm.throw_completion<TypeError>(ErrorType::CannotDeclareGlobalVariable, name);

            // c. If vn is not an element of declaredVarNames, then
            // i. Append vn to declaredVarNames.
            declared_var_names.set(name);
            return {};
        });
    }));

    // 11. NOTE: No abnormal terminations occur after this algorithm step if the global object is an ordinary object. However, if the global object is a Proxy exotic object it may exhibit behaviours that cause abnormal terminations in some of the following steps.
    // 12. NOTE: Annex B.3.2.2 adds additional steps at this point.

    // 12. Let strict be IsStrict of script.
    // 13. If strict is false, then
    if (!m_is_strict_mode) {
        // a. Let declaredFunctionOrVarNames be the list-concatenation of declaredFunctionNames and declaredVarNames.
        // b. For each FunctionDeclaration f that is directly contained in the StatementList of a Block, CaseClause, or DefaultClause Contained within script, do
        TRY(for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) -> ThrowCompletionOr<void> {
            // i. Let F be StringValue of the BindingIdentifier of f.
            auto function_name = function_declaration.name();

            // ii. If replacing the FunctionDeclaration f with a VariableStatement that has F as a BindingIdentifier would not produce any Early Errors for script, then
            // Note: This step is already performed during parsing and for_each_function_hoistable_with_annexB_extension so this always passes here.

            // 1. If env.HasLexicalDeclaration(F) is false, then
            if (global_environment.has_lexical_declaration(function_name))
                return {};

            // a. Let fnDefinable be ? env.CanDeclareGlobalVar(F).
            auto function_definable = TRY(global_environment.can_declare_global_function(function_name));
            // b. If fnDefinable is true, then

            if (!function_definable)
                return {};

            // i. NOTE: A var binding for F is only instantiated here if it is neither a VarDeclaredName nor the name of another FunctionDeclaration.

            // ii. If declaredFunctionOrVarNames does not contain F, then

            if (!declared_function_names.contains(function_name) && !declared_var_names.contains(function_name)) {
                // i. Perform ? env.CreateGlobalVarBinding(F, false).
                TRY(global_environment.create_global_var_binding(function_name, false));

                // ii. Append F to declaredFunctionOrVarNames.
                declared_function_names.set(function_name);
            }

            // iii. When the FunctionDeclaration f is evaluated, perform the following steps in place of the FunctionDeclaration Evaluation algorithm provided in 15.2.6:
            //     i. Let genv be the running execution context's VariableEnvironment.
            //     ii. Let benv be the running execution context's LexicalEnvironment.
            //     iii. Let fobj be ! benv.GetBindingValue(F, false).
            //     iv. Perform ? genv.SetMutableBinding(F, fobj, false).
            //     v. Return unused.
            function_declaration.set_should_do_additional_annexB_steps();

            return {};
        }));

        // We should not use declared function names below here anymore since these functions are not in there in the spec.
        declared_function_names.clear();
    }

    // 13. Let lexDeclarations be the LexicallyScopedDeclarations of script.
    // 14. Let privateEnv be null.
    PrivateEnvironment* private_environment = nullptr;

    // 15. For each element d of lexDeclarations, do
    TRY(for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        // a. NOTE: Lexically declared names are only instantiated here but not initialized.
        // b. For each element dn of the BoundNames of d, do
        return declaration.for_each_bound_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
            auto const& name = identifier.string();

            // i. If IsConstantDeclaration of d is true, then
            if (declaration.is_constant_declaration()) {
                // 1. Perform ? env.CreateImmutableBinding(dn, true).
                TRY(global_environment.create_immutable_binding(vm, name, true));
            }
            // ii. Else,
            else {
                // 1. Perform ? env.CreateMutableBinding(dn, false).
                TRY(global_environment.create_mutable_binding(vm, name, false));
            }

            return {};
        });
    }));

    // 16. For each Parse Node f of functionsToInitialize, do
    // NOTE: We iterate in reverse order since we appended the functions
    //       instead of prepending. We append because prepending is much slower
    //       and we only use the created vector here.
    for (auto& declaration : functions_to_initialize.in_reverse()) {
        // a. Let fn be the sole element of the BoundNames of f.
        // b. Let fo be InstantiateFunctionObject of f with arguments env and privateEnv.
        auto function = ECMAScriptFunctionObject::create_from_function_data(
            realm,
            declaration.ensure_shared_data(vm),
            &global_environment,
            private_environment);

        // c. Perform ? env.CreateGlobalFunctionBinding(fn, fo, false).
        TRY(global_environment.create_global_function_binding(function->name(), function, false));
    }

    // 17. For each String vn of declaredVarNames, do
    for (auto& var_name : declared_var_names) {
        // a. Perform ? env.CreateGlobalVarBinding(vn, false).
        TRY(global_environment.create_global_var_binding(var_name, false));
    }

    // 18. Return unused.
    return {};
}

ModuleRequest::ModuleRequest(Utf16FlyString module_specifier_, Vector<ImportAttribute> attributes)
    : module_specifier(move(module_specifier_))
    , attributes(move(attributes))
{
    // 13.3.10.2 EvaluateImportCall ( specifierExpression [ , optionsExpression ] ), https://tc39.es/ecma262/#sec-evaluate-import-call
    // 16.2.2.4 Static Semantics: WithClauseToAttributes, https://tc39.es/ecma262/#sec-withclausetoattributes
    // 2. Sort attributes according to the lexicographic order of their [[Key]] field, treating the value of each such
    //    field as a sequence of UTF-16 code unit values.
    quick_sort(this->attributes, [](ImportAttribute const& lhs, ImportAttribute const& rhs) {
        return lhs.key < rhs.key;
    });
}

ByteString SourceRange::filename() const
{
    return code->filename().to_byte_string();
}

NonnullRefPtr<CallExpression> CallExpression::create(SourceRange source_range, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens)
{
    return ASTNodeWithTailArray::create<CallExpression>(arguments.size(), move(source_range), move(callee), arguments, invocation_style, inside_parens);
}

NonnullRefPtr<NewExpression> NewExpression::create(SourceRange source_range, NonnullRefPtr<Expression const> callee, ReadonlySpan<Argument> arguments, InvocationStyleEnum invocation_style, InsideParenthesesEnum inside_parens)
{
    return ASTNodeWithTailArray::create<NewExpression>(arguments.size(), move(source_range), move(callee), arguments, invocation_style, inside_parens);
}

NonnullRefPtr<FunctionParameters> FunctionParameters::empty()
{
    static auto empty = adopt_ref(*new FunctionParameters({}));
    return empty;
}

}
