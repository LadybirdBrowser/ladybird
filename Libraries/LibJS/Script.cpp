/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Lexer.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/Script.h>
#include <LibJS/SourceCode.h>

namespace JS {

bool g_dump_ast = false;
bool g_dump_ast_use_color = false;

GC_DEFINE_ALLOCATOR(Script);

// 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
Result<GC::Ref<Script>, Vector<ParserError>> Script::parse(StringView source_text, Realm& realm, StringView filename, HostDefined* host_defined, size_t line_number_offset)
{
    auto rust_compilation = RustIntegration::compile_script(source_text, realm, filename, line_number_offset);
    if (rust_compilation.has_value()) {
        if (rust_compilation->is_error())
            return rust_compilation->release_error();
        return realm.heap().allocate<Script>(realm, filename, move(rust_compilation->value()), host_defined);
    }

    // 1. Let script be ParseText(sourceText, Script).
    auto parser = Parser(Lexer(SourceCode::create(String::from_utf8(filename).release_value_but_fixme_should_propagate_errors(), Utf16String::from_utf8(source_text)), line_number_offset));
    auto script = parser.parse_program();

    // 2. If script is a List of errors, return body.
    if (parser.has_errors())
        return parser.errors();

    // 3. Return Script Record { [[Realm]]: realm, [[ECMAScriptCode]]: script, [[HostDefined]]: hostDefined }.
    return realm.heap().allocate<Script>(realm, filename, move(script), host_defined);
}

Script::Script(Realm& realm, StringView filename, RefPtr<Program> parse_node, HostDefined* host_defined)
    : m_realm(realm)
    , m_parse_node(move(parse_node))
    , m_filename(filename)
    , m_host_defined(host_defined)
{
    auto& vm = realm.vm();
    auto& program = *m_parse_node;

    m_is_strict_mode = program.is_strict_mode();

    // Pre-compute lexically declared names (GDI step 3).
    MUST(program.for_each_lexically_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
        m_lexical_names.append(identifier.string());
        return {};
    }));

    // Pre-compute var declared names (GDI step 4).
    MUST(program.for_each_var_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
        m_var_names.append(identifier.string());
        return {};
    }));

    // Pre-compute functions to initialize and declared function names (GDI steps 7-8).
    MUST(program.for_each_var_function_declaration_in_reverse_order([&](FunctionDeclaration const& function) -> ThrowCompletionOr<void> {
        auto function_name = function.name();
        if (m_declared_function_names.set(function_name) != AK::HashSetResult::InsertedNewEntry)
            return {};
        m_functions_to_initialize.append({ SharedFunctionInstanceData::create_for_function_node(vm, function), function_name });
        return {};
    }));

    // Pre-compute var scoped variable names (GDI step 10).
    MUST(program.for_each_var_scoped_variable_declaration([&](VariableDeclaration const& declaration) {
        return declaration.for_each_bound_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
            m_var_scoped_names.append(identifier.string());
            return {};
        });
    }));

    // Pre-compute AnnexB candidates (GDI step 13).
    if (!m_is_strict_mode) {
        MUST(program.for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) -> ThrowCompletionOr<void> {
            m_annex_b_candidate_names.append(function_declaration.name());
            m_annex_b_function_declarations.append(function_declaration);
            return {};
        }));
    }

    // Pre-compute lexical bindings (GDI step 15).
    MUST(program.for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        return declaration.for_each_bound_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
            m_lexical_bindings.append({ identifier.string(), declaration.is_constant_declaration() });
            return {};
        });
    }));
}

Script::Script(Realm& realm, StringView filename, RustIntegration::ScriptResult&& result, HostDefined* host_defined)
    : m_realm(realm)
    , m_executable(result.executable)
    , m_lexical_names(move(result.lexical_names))
    , m_var_names(move(result.var_names))
    , m_declared_function_names(move(result.declared_function_names))
    , m_var_scoped_names(move(result.var_scoped_names))
    , m_annex_b_candidate_names(move(result.annex_b_candidate_names))
    , m_lexical_bindings(move(result.lexical_bindings))
    , m_is_strict_mode(result.is_strict_mode)
    , m_filename(filename)
    , m_host_defined(host_defined)
{
    m_functions_to_initialize.ensure_capacity(result.functions_to_initialize.size());
    for (auto& f : result.functions_to_initialize)
        m_functions_to_initialize.append({ *f.shared_data, move(f.name) });
}

// 16.1.7 GlobalDeclarationInstantiation ( script, env ), https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
ThrowCompletionOr<void> Script::global_declaration_instantiation(VM& vm, GlobalEnvironment& global_environment)
{
    auto& realm = *vm.current_realm();

    // 1. Let lexNames be the LexicallyDeclaredNames of script.
    // 2. Let varNames be the VarDeclaredNames of script.
    // 3. For each element name of lexNames, do
    for (auto const& name : m_lexical_names) {
        // a. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
        if (global_environment.has_lexical_declaration(name))
            return vm.throw_completion<SyntaxError>(ErrorType::TopLevelVariableAlreadyDeclared, name);

        // b. Let hasRestrictedGlobal be ? HasRestrictedGlobalProperty(env, name).
        auto has_restricted_global = TRY(global_environment.has_restricted_global_property(name));

        // d. If hasRestrictedGlobal is true, throw a SyntaxError exception.
        if (has_restricted_global)
            return vm.throw_completion<SyntaxError>(ErrorType::RestrictedGlobalProperty, name);
    }

    // 4. For each element name of varNames, do
    for (auto const& name : m_var_names) {
        // a. If env.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
        if (global_environment.has_lexical_declaration(name))
            return vm.throw_completion<SyntaxError>(ErrorType::TopLevelVariableAlreadyDeclared, name);
    }

    // 5. Let varDeclarations be the VarScopedDeclarations of script.
    // 6. Let functionsToInitialize be a new empty List.
    // 7. Let declaredFunctionNames be a new empty List.
    // 8. For each element d of varDeclarations, in reverse List order, do
    for (auto const& function : m_functions_to_initialize) {
        // 1. Let fnDefinable be ? env.CanDeclareGlobalFunction(fn).
        auto function_definable = TRY(global_environment.can_declare_global_function(function.name));

        // 2. If fnDefinable is false, throw a TypeError exception.
        if (!function_definable)
            return vm.throw_completion<TypeError>(ErrorType::CannotDeclareGlobalFunction, function.name);
    }

    // 9. Let declaredVarNames be a new empty List.
    HashTable<Utf16FlyString> declared_var_names;

    // 10. For each element d of varDeclarations, do
    for (auto const& name : m_var_scoped_names) {
        // 1. If vn is not an element of declaredFunctionNames, then
        if (m_declared_function_names.contains(name))
            continue;

        // a. Let vnDefinable be ? env.CanDeclareGlobalVar(vn).
        auto var_definable = TRY(global_environment.can_declare_global_var(name));

        // b. If vnDefinable is false, throw a TypeError exception.
        if (!var_definable)
            return vm.throw_completion<TypeError>(ErrorType::CannotDeclareGlobalVariable, name);

        // c. If vn is not an element of declaredVarNames, then
        // i. Append vn to declaredVarNames.
        declared_var_names.set(name);
    }

    // 12. NOTE: Annex B.3.2.2 adds additional steps at this point.
    // 12. Let strict be IsStrict of script.
    // 13. If strict is false, then
    if (!m_is_strict_mode) {
        // a. Let declaredFunctionOrVarNames be the list-concatenation of declaredFunctionNames and declaredVarNames.
        // b. For each FunctionDeclaration f that is directly contained in the StatementList of a Block, CaseClause, or DefaultClause Contained within script, do
        for (size_t i = 0; i < m_annex_b_candidate_names.size(); ++i) {
            // i. Let F be StringValue of the BindingIdentifier of f.
            auto& function_name = m_annex_b_candidate_names[i];

            // 1. If env.HasLexicalDeclaration(F) is false, then
            if (global_environment.has_lexical_declaration(function_name))
                continue;

            // a. Let fnDefinable be ? env.CanDeclareGlobalVar(F).
            auto function_definable = TRY(global_environment.can_declare_global_function(function_name));
            // b. If fnDefinable is true, then
            if (!function_definable)
                continue;

            // ii. If declaredFunctionOrVarNames does not contain F, then
            if (!m_declared_function_names.contains(function_name) && !declared_var_names.contains(function_name)) {
                // i. Perform ? env.CreateGlobalVarBinding(F, false).
                TRY(global_environment.create_global_var_binding(function_name, false));
            }

            // iii. When the FunctionDeclaration f is evaluated, perform the following steps in place of the FunctionDeclaration Evaluation algorithm provided in 15.2.6:
            if (i < m_annex_b_function_declarations.size())
                m_annex_b_function_declarations[i]->set_should_do_additional_annexB_steps();
        }
    }

    // 14. Let privateEnv be null.
    PrivateEnvironment* private_environment = nullptr;

    // 15. For each element d of lexDeclarations, do
    for (auto const& binding : m_lexical_bindings) {
        // i. If IsConstantDeclaration of d is true, then
        if (binding.is_constant) {
            // 1. Perform ? env.CreateImmutableBinding(dn, true).
            TRY(global_environment.create_immutable_binding(vm, binding.name, true));
        }
        // ii. Else,
        else {
            // 1. Perform ? env.CreateMutableBinding(dn, false).
            TRY(global_environment.create_mutable_binding(vm, binding.name, false));
        }
    }

    // 16. For each Parse Node f of functionsToInitialize, do
    // NB: We iterate in reverse order since we appended the functions
    //     instead of prepending during pre-computation.
    for (auto const& function_to_initialize : m_functions_to_initialize.in_reverse()) {
        // a. Let fn be the sole element of the BoundNames of f.
        // b. Let fo be InstantiateFunctionObject of f with arguments env and privateEnv.
        auto function = ECMAScriptFunctionObject::create_from_function_data(
            realm,
            function_to_initialize.shared_data,
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

void Script::drop_ast()
{
    m_parse_node = nullptr;
    m_annex_b_function_declarations.clear();
}

Script::~Script()
{
}

void Script::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_executable);
    for (auto const& function : m_functions_to_initialize)
        visitor.visit(function.shared_data);
    if (m_host_defined)
        m_host_defined->visit_host_defined_self(visitor);
    for (auto const& loaded_module : m_loaded_modules)
        visitor.visit(loaded_module.module);
}

}
