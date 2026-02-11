/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

static FunctionLocal to_function_local(Identifier const& identifier)
{
    if (!identifier.is_local())
        return {};
    auto local = identifier.local_index();
    return { static_cast<FunctionLocal::Type>(local.type), local.index };
}

GC_DEFINE_ALLOCATOR(SharedFunctionInstanceData);

SharedFunctionInstanceData::SharedFunctionInstanceData(
    VM& vm,
    FunctionKind kind,
    Utf16FlyString name,
    i32 function_length,
    NonnullRefPtr<FunctionParameters const> formal_parameters,
    NonnullRefPtr<Statement const> ecmascript_code,
    Utf16View source_text,
    bool strict,
    bool is_arrow_function,
    FunctionParsingInsights const& parsing_insights,
    Vector<LocalVariable> local_variables_names)
    : m_formal_parameters(move(formal_parameters))
    , m_ecmascript_code(move(ecmascript_code))
    , m_name(move(name))
    , m_source_text(move(source_text))
    , m_local_variables_names(move(local_variables_names))
    , m_function_length(function_length)
    , m_kind(kind)
    , m_strict(strict)
    , m_might_need_arguments_object(parsing_insights.might_need_arguments_object)
    , m_contains_direct_call_to_eval(parsing_insights.contains_direct_call_to_eval)
    , m_is_arrow_function(is_arrow_function)
    , m_uses_this(parsing_insights.uses_this)
{
    if (m_is_arrow_function)
        m_this_mode = ThisMode::Lexical;
    else if (m_strict)
        m_this_mode = ThisMode::Strict;
    else
        m_this_mode = ThisMode::Global;

    m_formal_parameter_count = m_formal_parameters->size();

    // 15.1.3 Static Semantics: IsSimpleParameterList, https://tc39.es/ecma262/#sec-static-semantics-issimpleparameterlist
    m_has_simple_parameter_list = all_of(m_formal_parameters->parameters(), [&](auto& parameter) {
        if (parameter.is_rest)
            return false;
        if (parameter.default_value)
            return false;
        if (!parameter.binding.template has<NonnullRefPtr<Identifier const>>())
            return false;
        return true;
    });

    // Pre-extract parameter names for create_mapped_arguments_object.
    // NB: Mapped arguments are only used for non-strict functions with simple parameter lists.
    if (m_has_simple_parameter_list) {
        m_parameter_names_for_mapped_arguments.ensure_capacity(m_formal_parameter_count);
        for (auto const& parameter : m_formal_parameters->parameters())
            m_parameter_names_for_mapped_arguments.append(parameter.binding.get<NonnullRefPtr<Identifier const>>()->string());
    }

    // NOTE: The following steps are from FunctionDeclarationInstantiation that could be executed once
    //       and then reused in all subsequent function instantiations.

    // 2. Let code be func.[[ECMAScriptCode]].
    ScopeNode const* scope_body = nullptr;
    if (is<ScopeNode>(*m_ecmascript_code))
        scope_body = static_cast<ScopeNode const*>(m_ecmascript_code.ptr());
    m_has_scope_body = scope_body != nullptr;

    // 3. Let strict be func.[[Strict]].

    // 4. Let formals be func.[[FormalParameters]].
    auto const& formals = *m_formal_parameters;

    // 5. Let parameterNames be the BoundNames of formals.
    // 6. If parameterNames has any duplicate entries, let hasDuplicates be true. Otherwise, let hasDuplicates be false.

    size_t parameters_in_environment = 0;

    // NOTE: This loop performs step 5, 6, and 8.
    for (auto const& parameter : formals.parameters()) {
        if (parameter.default_value)
            m_has_parameter_expressions = true;

        parameter.binding.visit(
            [&](Identifier const& identifier) {
                if (m_parameter_names.set(identifier.string(), identifier.is_local() ? ParameterIsLocal::Yes : ParameterIsLocal::No) != AK::HashSetResult::InsertedNewEntry)
                    m_has_duplicates = true;
                else if (!identifier.is_local())
                    ++parameters_in_environment;
            },
            [&](NonnullRefPtr<BindingPattern const> const& pattern) {
                if (pattern->contains_expression())
                    m_has_parameter_expressions = true;

                // NOTE: Nothing in the callback throws an exception.
                MUST(pattern->for_each_bound_identifier([&](auto& identifier) {
                    if (m_parameter_names.set(identifier.string(), identifier.is_local() ? ParameterIsLocal::Yes : ParameterIsLocal::No) != AK::HashSetResult::InsertedNewEntry)
                        m_has_duplicates = true;
                    else if (!identifier.is_local())
                        ++parameters_in_environment;
                }));
            });
    }

    // 15. Let argumentsObjectNeeded be true.
    m_arguments_object_needed = m_might_need_arguments_object;

    // 16. If func.[[ThisMode]] is lexical, then
    if (m_this_mode == ThisMode::Lexical) {
        // a. NOTE: Arrow functions never have an arguments object.
        // b. Set argumentsObjectNeeded to false.
        m_arguments_object_needed = false;
    }
    // 17. Else if parameterNames contains "arguments", then
    else if (m_parameter_names.contains(vm.names.arguments.as_string())) {
        // a. Set argumentsObjectNeeded to false.
        m_arguments_object_needed = false;
    }

    // 18. Else if hasParameterExpressions is false, then
    //     a. If functionNames contains "arguments" or lexicalNames contains "arguments", then
    //         i. Set argumentsObjectNeeded to false.
    // NOTE: The block below is a combination of step 14 and step 18.
    if (!scope_body) {
        m_arguments_object_needed = false;
    } else {
        scope_body->ensure_function_scope_data();
        auto const& function_scope_data = *scope_body->function_scope_data();

        for (auto const& decl : function_scope_data.functions_to_initialize) {
            auto shared_data = create_for_function_node(vm, *decl);
            auto const& name_id = *decl->name_identifier();
            m_functions_to_initialize.append({
                .shared_data = shared_data,
                .name = decl->name(),
                .local = to_function_local(name_id),
            });
        }

        if (!m_has_parameter_expressions && function_scope_data.has_function_named_arguments)
            m_arguments_object_needed = false;

        if (!m_has_parameter_expressions && m_arguments_object_needed && function_scope_data.has_lexically_declared_arguments)
            m_arguments_object_needed = false;
    }

    auto arguments_object_needs_binding = m_arguments_object_needed && !m_local_variables_names.contains([](auto const& local) { return local.declaration_kind == LocalVariable::DeclarationKind::ArgumentsObject; });

    size_t* environment_size = nullptr;
    size_t parameter_environment_bindings_count = 0;
    // 19. If strict is true or hasParameterExpressions is false, then
    if (strict || !m_has_parameter_expressions) {
        // a. NOTE: Only a single Environment Record is needed for the parameters, since calls to eval in strict mode code cannot create new bindings which are visible outside of the eval.
        // b. Let env be the LexicalEnvironment of calleeContext
        // NOTE: Here we are only interested in the size of the environment.
        environment_size = &m_function_environment_bindings_count;
    }
    // 20. Else,
    else {
        // a. NOTE: A separate Environment Record is needed to ensure that bindings created by direct eval calls in the formal parameter list are outside the environment where parameters are declared.
        // b. Let calleeEnv be the LexicalEnvironment of calleeContext.
        // c. Let env be NewDeclarativeEnvironment(calleeEnv).
        environment_size = &parameter_environment_bindings_count;
    }

    *environment_size += parameters_in_environment;

    // 22. If argumentsObjectNeeded is true, then
    if (arguments_object_needs_binding)
        (*environment_size)++;

    size_t* var_environment_size = nullptr;

    if (scope_body) {
        auto const& function_scope_data = *scope_body->function_scope_data();

        // 27. If hasParameterExpressions is false, then
        if (!m_has_parameter_expressions) {
            // Use pre-computed non_local_var_count for environment size.
            *environment_size += function_scope_data.non_local_var_count;

            // Directly iterate vars_to_initialize - already deduplicated by parser.
            for (auto const& var : function_scope_data.vars_to_initialize) {
                // Skip vars that shadow parameters or "arguments" if needed.
                if (var.is_parameter)
                    continue;
                if (var.identifier.string() == vm.names.arguments.as_string() && m_arguments_object_needed)
                    continue;

                m_var_names_to_initialize_binding.append({
                    .name = var.identifier.string(),
                    .local = to_function_local(var.identifier),
                });
            }

            // d. Let varEnv be env
            var_environment_size = environment_size;
        } else {
            // a. NOTE: A separate Environment Record is needed to ensure that closures created by
            //          expressions in the formal parameter list do not have visibility of declarations in the function body.

            // b. Let varEnv be NewDeclarativeEnvironment(env).
            var_environment_size = &m_var_environment_bindings_count;

            // Use pre-computed non_local_var_count_for_parameter_expressions for environment size.
            *var_environment_size += function_scope_data.non_local_var_count_for_parameter_expressions;

            // Directly iterate vars_to_initialize - already deduplicated by parser.
            for (auto const& var : function_scope_data.vars_to_initialize) {
                bool is_in_parameter_bindings = var.is_parameter || (var.identifier.string() == vm.names.arguments.as_string() && m_arguments_object_needed);
                m_var_names_to_initialize_binding.append({
                    .name = var.identifier.string(),
                    .local = to_function_local(var.identifier),
                    .parameter_binding = is_in_parameter_bindings,
                    .function_name = var.is_function_name,
                });
            }
        }

        // 29. NOTE: Annex B.3.2.1 adds additional steps at this point.
        // B.3.2.1 Changes to FunctionDeclarationInstantiation, https://tc39.es/ecma262/#sec-web-compat-functiondeclarationinstantiation
        if (!m_strict) {
            HashTable<Utf16FlyString> annexB_seen_names;
            MUST(scope_body->for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) {
                auto function_name = function_declaration.name();

                // Check if function name is in parameter_bindings (parameters + "arguments" if needed).
                if (m_parameter_names.contains(function_name))
                    return;
                if (function_name == vm.names.arguments.as_string() && m_arguments_object_needed)
                    return;

                // Check if function name is already a var or already processed by AnnexB.
                if (!function_scope_data.var_names.contains(function_name) && !annexB_seen_names.contains(function_name)) {
                    m_function_names_to_initialize_binding.append(function_name);
                    annexB_seen_names.set(function_name);
                    (*var_environment_size)++;
                }

                function_declaration.set_should_do_additional_annexB_steps();
            }));
        }
    } else {
        var_environment_size = environment_size;
    }

    size_t* lex_environment_size = nullptr;

    // 30. If strict is false, then
    if (scope_body)
        m_has_non_local_lexical_declarations = scope_body->has_non_local_lexical_declarations();
    if (!m_strict) {
        bool can_elide_declarative_environment = !m_contains_direct_call_to_eval && !m_has_non_local_lexical_declarations;
        if (can_elide_declarative_environment) {
            lex_environment_size = var_environment_size;
        } else {
            // a. Let lexEnv be NewDeclarativeEnvironment(varEnv).
            lex_environment_size = &m_lex_environment_bindings_count;
        }
    } else {
        // a. let lexEnv be varEnv.
        // NOTE: Here we are only interested in the size of the environment.
        lex_environment_size = var_environment_size;
    }

    if (scope_body) {
        MUST(scope_body->for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
            MUST(declaration.for_each_bound_identifier([&](auto const& id) {
                if (!id.is_local()) {
                    (*lex_environment_size)++;
                    m_lexical_bindings.append({
                        .name = id.string(),
                        .is_constant = declaration.is_constant_declaration(),
                    });
                }
            }));
        }));
    }

    m_function_environment_needed = arguments_object_needs_binding || m_function_environment_bindings_count > 0 || m_var_environment_bindings_count > 0 || m_lex_environment_bindings_count > 0 || parsing_insights.uses_this_from_environment || m_contains_direct_call_to_eval;
}

void SharedFunctionInstanceData::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_executable);
    for (auto& function : m_functions_to_initialize)
        visitor.visit(function.shared_data);
    m_class_field_initializer_name.visit([&](PropertyKey const& key) { key.visit_edges(visitor); }, [](auto&) {});
}

SharedFunctionInstanceData::~SharedFunctionInstanceData() = default;

GC::Ref<SharedFunctionInstanceData> SharedFunctionInstanceData::create_for_function_node(VM& vm, FunctionNode const& node)
{
    return create_for_function_node(vm, node, node.name());
}

GC::Ref<SharedFunctionInstanceData> SharedFunctionInstanceData::create_for_function_node(VM& vm, FunctionNode const& node, Utf16FlyString name)
{
    auto data = vm.heap().allocate<SharedFunctionInstanceData>(
        vm,
        node.kind(),
        move(name),
        node.function_length(),
        node.parameters(),
        *node.body_ptr(),
        node.source_text(),
        node.is_strict_mode(),
        node.is_arrow_function(),
        node.parsing_insights(),
        node.local_variables_names());

    // NB: Keep the SourceCode alive so that m_source_text (a Utf16View into it) remains valid
    //     even after the AST is dropped.
    data->m_source_code = &node.body().source_code();

    return data;
}

void SharedFunctionInstanceData::clear_compile_inputs()
{
    VERIFY(m_executable);
    m_formal_parameters = nullptr;
    m_ecmascript_code = nullptr;
    m_functions_to_initialize.clear();
    m_var_names_to_initialize_binding.clear();
    m_lexical_bindings.clear();
}

}
