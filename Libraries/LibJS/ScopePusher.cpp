/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibJS/Parser.h>
#include <LibJS/ScopePusher.h>

namespace JS {

ScopePusher::ScopePusher(Parser& parser, ScopeNode* node, ScopeLevel scope_level, ScopeType type)
    : m_parser(parser)
    , m_scope_level(scope_level)
    , m_type(type)
{
    m_parent_scope = exchange(m_parser.m_state.current_scope_pusher, this);
    if (type != ScopeType::Function) {
        VERIFY(node || (m_parent_scope && scope_level == ScopeLevel::NotTopLevel));
        if (!node)
            m_node = m_parent_scope->m_node;
        else
            m_node = node;
    }

    if (!is_top_level())
        m_top_level_scope = m_parent_scope->m_top_level_scope;
    else
        m_top_level_scope = this;
}

ScopePusher ScopePusher::function_scope(Parser& parser, RefPtr<Identifier const> function_name)
{
    ScopePusher scope_pusher(parser, nullptr, ScopeLevel::FunctionTopLevel, ScopeType::Function);
    if (function_name) {
        scope_pusher.m_variables.ensure(function_name->string()).flags |= ScopeVariable::IsBound;
    }
    return scope_pusher;
}

ScopePusher ScopePusher::program_scope(Parser& parser, Program& program)
{
    return ScopePusher(parser, &program, program.type() == Program::Type::Script ? ScopeLevel::ScriptTopLevel : ScopeLevel::ModuleTopLevel, ScopeType::Program);
}

ScopePusher ScopePusher::block_scope(Parser& parser, ScopeNode& node)
{
    return ScopePusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::Block);
}

ScopePusher ScopePusher::for_loop_scope(Parser& parser, ScopeNode& node)
{
    return ScopePusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::ForLoop);
}

ScopePusher ScopePusher::with_scope(Parser& parser, ScopeNode& node)
{
    ScopePusher scope_pusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::With);
    return scope_pusher;
}

ScopePusher ScopePusher::catch_scope(Parser& parser)
{
    return ScopePusher(parser, nullptr, ScopeLevel::NotTopLevel, ScopeType::Catch);
}

ScopePusher ScopePusher::static_init_block_scope(Parser& parser, ScopeNode& node)
{
    ScopePusher scope_pusher(parser, &node, ScopeLevel::StaticInitTopLevel, ScopeType::ClassStaticInit);
    return scope_pusher;
}

ScopePusher ScopePusher::class_field_scope(Parser& parser, ScopeNode& node)
{
    ScopePusher scope_pusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::ClassField);
    return scope_pusher;
}

ScopePusher ScopePusher::class_declaration_scope(Parser& parser, RefPtr<Identifier const> class_name)
{
    ScopePusher scope_pusher(parser, nullptr, ScopeLevel::NotTopLevel, ScopeType::ClassDeclaration);
    if (class_name) {
        scope_pusher.m_variables.ensure(class_name->string()).flags |= ScopeVariable::IsBound;
    }
    return scope_pusher;
}

void ScopePusher::add_catch_parameter(RefPtr<BindingPattern const> const& pattern, RefPtr<Identifier const> const& parameter)
{
    if (pattern) {
        // NOTE: Nothing in the callback throws an exception.
        MUST(pattern->for_each_bound_identifier([&](auto const& identifier) {
            auto& var = m_variables.ensure(identifier.string());
            var.flags |= ScopeVariable::IsForbiddenVar | ScopeVariable::IsBound | ScopeVariable::IsCatchParameter;
        }));
    } else if (parameter) {
        auto& var = m_variables.ensure(parameter->string());
        var.flags |= ScopeVariable::IsVar | ScopeVariable::IsBound | ScopeVariable::IsCatchParameter;
        var.var_identifier = parameter.ptr();
    }
}

void ScopePusher::add_declaration(NonnullRefPtr<Declaration const> declaration)
{
    if (declaration->is_lexical_declaration()) {
        // NOTE: Nothing in the callback throws an exception.
        MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
            auto const& name = identifier.string();
            auto& var = m_variables.ensure(name);
            if (var.flags & (ScopeVariable::IsVar | ScopeVariable::IsForbiddenLexical | ScopeVariable::IsFunction | ScopeVariable::IsLexical))
                throw_identifier_declared(name, declaration);
            var.flags |= ScopeVariable::IsLexical;
        }));

        m_node->add_lexical_declaration(move(declaration));
    } else if (!declaration->is_function_declaration()) {
        // NOTE: Nothing in the callback throws an exception.
        MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
            auto const& name = identifier.string();
            ScopePusher* pusher = this;
            while (true) {
                auto& var = pusher->m_variables.ensure(name);
                if (var.flags & (ScopeVariable::IsLexical | ScopeVariable::IsFunction | ScopeVariable::IsForbiddenVar))
                    throw_identifier_declared(name, declaration);

                var.flags |= ScopeVariable::IsVar;
                var.var_identifier = &identifier;
                if (pusher->is_top_level())
                    break;

                VERIFY(pusher->m_parent_scope != nullptr);
                pusher = pusher->m_parent_scope;
            }
            VERIFY(pusher->is_top_level() && pusher->m_node);
            pusher->m_node->add_var_scoped_declaration(declaration);
        }));

        VERIFY(m_top_level_scope);
        m_top_level_scope->m_node->add_var_scoped_declaration(move(declaration));
    } else {
        if (m_scope_level != ScopeLevel::NotTopLevel && m_scope_level != ScopeLevel::ModuleTopLevel) {
            // Only non-top levels and Module don't var declare the top functions
            // NOTE: Nothing in the callback throws an exception.
            MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
                auto& var = m_variables.ensure(identifier.string());
                var.flags |= ScopeVariable::IsVar;
                var.var_identifier = &identifier;
            }));
            m_node->add_var_scoped_declaration(move(declaration));
        } else {
            VERIFY(is<FunctionDeclaration>(*declaration));
            auto function_declaration = static_ptr_cast<FunctionDeclaration const>(declaration);
            auto function_name = function_declaration->name();
            auto& var = m_variables.ensure(function_name);
            if (var.flags & (ScopeVariable::IsVar | ScopeVariable::IsLexical))
                throw_identifier_declared(function_name, declaration);

            if (function_declaration->kind() != FunctionKind::Normal || m_parser.m_state.strict_mode) {
                if (var.flags & ScopeVariable::IsFunction)
                    throw_identifier_declared(function_name, declaration);

                var.flags |= ScopeVariable::IsLexical;
                m_node->add_lexical_declaration(move(declaration));
                return;
            }

            if (!(var.flags & ScopeVariable::IsLexical))
                m_functions_to_hoist.append(function_declaration);

            var.flags |= ScopeVariable::IsFunction;
            var.function_declaration = function_declaration;
            m_node->add_lexical_declaration(move(declaration));
        }
    }
}

ScopePusher const* ScopePusher::last_function_scope() const
{
    for (auto scope_ptr = this; scope_ptr; scope_ptr = scope_ptr->m_parent_scope) {
        if (scope_ptr->m_type == ScopeType::Function || scope_ptr->m_type == ScopeType::ClassStaticInit)
            return scope_ptr;
    }
    return nullptr;
}

bool ScopePusher::has_declaration(Utf16FlyString const& name) const
{
    if (has_variable_with_flags(name, ScopeVariable::IsLexical | ScopeVariable::IsVar))
        return true;
    return m_functions_to_hoist.contains([&name](auto& function) { return function->name() == name; });
}

void ScopePusher::set_contains_direct_call_to_eval()
{
    m_contains_direct_call_to_eval = true;
    m_screwed_by_eval_in_scope_chain = true;
    m_eval_in_current_function = true;
}

void ScopePusher::set_function_parameters(NonnullRefPtr<FunctionParameters const> parameters)
{
    m_function_parameters = move(parameters);
    for (auto& parameter : m_function_parameters->parameters()) {
        parameter.binding.visit(
            [&](Identifier const& identifier) {
                register_identifier(fixme_launder_const_through_pointer_cast(identifier));
                auto& var = m_variables.ensure(identifier.string());
                var.flags |= ScopeVariable::IsParameterCandidate | ScopeVariable::IsForbiddenLexical;
            },
            [&](NonnullRefPtr<BindingPattern const> const& binding_pattern) {
                // NOTE: Nothing in the callback throws an exception.
                MUST(binding_pattern->for_each_bound_identifier([&](auto const& identifier) {
                    m_variables.ensure(identifier.string()).flags |= ScopeVariable::IsForbiddenLexical;
                }));
            });
    }
}

ScopePusher::~ScopePusher()
{
    VERIFY(is_top_level() || m_parent_scope);

    propagate_flags_to_parent();

    if (!m_node) {
        m_parser.m_state.current_scope_pusher = m_parent_scope;
        return;
    }

    propagate_eval_poisoning();
    resolve_identifiers();
    hoist_functions();

    if (m_type == ScopeType::Function && m_function_parameters)
        build_function_scope_data();

    VERIFY(m_parser.m_state.current_scope_pusher == this);
    m_parser.m_state.current_scope_pusher = m_parent_scope;
}

void ScopePusher::propagate_flags_to_parent()
{
    if (m_parent_scope && !m_function_parameters) {
        m_parent_scope->m_contains_access_to_arguments_object_in_non_strict_mode |= m_contains_access_to_arguments_object_in_non_strict_mode;
        m_parent_scope->m_contains_direct_call_to_eval |= m_contains_direct_call_to_eval;
        m_parent_scope->m_contains_await_expression |= m_contains_await_expression;
    }
}

void ScopePusher::propagate_eval_poisoning()
{
    if (m_parent_scope && (m_contains_direct_call_to_eval || m_screwed_by_eval_in_scope_chain)) {
        m_parent_scope->m_screwed_by_eval_in_scope_chain = true;
    }

    // Propagate eval-in-current-function only through block scopes, not across function boundaries.
    // This is used for global identifier marking - eval can only inject vars into its containing
    // function's scope, not into parent function scopes.
    if (m_parent_scope && m_eval_in_current_function && m_type != ScopeType::Function) {
        m_parent_scope->m_eval_in_current_function = true;
    }
}

void ScopePusher::resolve_identifiers()
{
    for (auto& it : m_identifier_groups) {
        auto const& identifier_group_name = it.key;
        auto& identifier_group = it.value;

        if (identifier_group.declaration_kind.has_value()) {
            for (auto& identifier : identifier_group.identifiers) {
                identifier->set_declaration_kind(identifier_group.declaration_kind.value());
            }
        }

        auto var_it = m_variables.find(identifier_group_name);
        u16 var_flags = (var_it != m_variables.end()) ? var_it->value.flags : 0;

        Optional<LocalVariable::DeclarationKind> local_variable_declaration_kind;
        if (is_top_level() && (var_flags & ScopeVariable::IsVar)) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::Var;
        } else if (var_flags & ScopeVariable::IsLexical) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::LetOrConst;
        } else if (var_flags & ScopeVariable::IsFunction) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::Function;
        }

        if (m_type == ScopeType::Function && !m_is_arrow_function && identifier_group_name == "arguments"sv) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::ArgumentsObject;
        }

        if (m_type == ScopeType::Catch && (var_flags & ScopeVariable::IsCatchParameter)) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::CatchClauseParameter;
        }

        bool hoistable_function_declaration = m_functions_to_hoist.contains([&](auto const& function_declaration) {
            return function_declaration->name() == identifier_group_name;
        });

        if (m_type == ScopeType::ClassDeclaration && (var_flags & ScopeVariable::IsBound)) {
            // NOTE: Currently, the parser cannot recognize that assigning a named function expression creates a scope with a binding for the function name.
            //       As a result, function names are not considered as candidates for optimization in global variable access.
            continue;
        }

        if (m_type == ScopeType::Function && !m_is_function_declaration && (var_flags & ScopeVariable::IsBound)) {
            // Named function expression: identifiers with this name inside the function may refer
            // to the function's immutable name binding, so they cannot be optimized as globals.
            for (auto& identifier : identifier_group.identifiers)
                identifier->set_is_inside_scope_with_eval();
        }

        if (m_type == ScopeType::ClassDeclaration) {
            // NOTE: Class declaration doesn't not have own ScopeNode hence can't contain declaration of any variable
            local_variable_declaration_kind.clear();
        }

        bool is_function_parameter = false;
        if (m_type == ScopeType::Function) {
            if (!m_contains_access_to_arguments_object_in_non_strict_mode && (var_flags & ScopeVariable::IsParameterCandidate)) {
                is_function_parameter = true;
            } else if (var_flags & ScopeVariable::IsForbiddenLexical) {
                // NOTE: If an identifier is used as a function parameter that cannot be optimized locally or globally, it is simply ignored.
                continue;
            }
        }

        if (m_type == ScopeType::Function && hoistable_function_declaration) {
            // NOTE: Hoistable function declarations are currently not optimized into global or local variables, but future improvements may change that.
            continue;
        }

        if (m_type == ScopeType::Program) {
            auto can_use_global_for_identifier = !(identifier_group.used_inside_with_statement || m_parser.m_state.initiated_by_eval);
            if (can_use_global_for_identifier) {
                for (auto& identifier : identifier_group.identifiers) {
                    // Only mark identifiers as global if they are not inside a function scope
                    // that contains eval() or has eval in its scope chain.
                    if (!identifier->is_inside_scope_with_eval())
                        identifier->set_is_global();
                }
            }
        } else if (local_variable_declaration_kind.has_value() || is_function_parameter) {
            if (hoistable_function_declaration)
                continue;

            if (!identifier_group.captured_by_nested_function && !identifier_group.used_inside_with_statement) {
                if (m_screwed_by_eval_in_scope_chain)
                    continue;

                auto local_scope = last_function_scope();
                if (!local_scope) {
                    // NOTE: If there is no function scope, we are in a *descendant* of the global program scope.
                    //       While we cannot make `let` and `const` into locals in the topmost program scope,
                    //       as that would break expected web behavior where subsequent <script> elements should see
                    //       lexical bindings created by earlier <script> elements, we *can* promote them in descendant scopes.
                    //       Of course, global `var` bindings can never be made into locals, as they get hoisted to the topmost program scope.
                    if (identifier_group.declaration_kind == DeclarationKind::Var)
                        continue;
                    // Add these locals to the top-level scope. (We only produce one executable for the entire program
                    // scope, so that's where the locals have to be stored.)
                    local_scope = m_top_level_scope;
                }

                if (is_function_parameter) {
                    auto argument_index = local_scope->m_function_parameters->get_index_of_parameter_name(identifier_group_name);
                    for (auto& identifier : identifier_group.identifiers)
                        identifier->set_argument_index(argument_index.value());
                } else {
                    auto local_variable_index = local_scope->m_node->add_local_variable(identifier_group_name, *local_variable_declaration_kind);
                    for (auto& identifier : identifier_group.identifiers)
                        identifier->set_local_variable_index(local_variable_index);
                }
            }
        } else {
            if (m_function_parameters || m_type == ScopeType::ClassField || m_type == ScopeType::ClassStaticInit) {
                // NOTE: Class fields and class static initialization sections implicitly create functions
                identifier_group.captured_by_nested_function = true;
            }

            if (m_type == ScopeType::With)
                identifier_group.used_inside_with_statement = true;

            // Mark each identifier individually if it's inside a scope with eval.
            // This allows per-identifier optimization decisions at Program scope.
            // We use m_eval_in_current_function instead of m_screwed_by_eval_in_scope_chain
            // because eval can only inject vars into its containing function's scope,
            // not into parent function scopes.
            if (m_eval_in_current_function) {
                for (auto& identifier : identifier_group.identifiers)
                    identifier->set_is_inside_scope_with_eval();
            }

            if (m_parent_scope) {
                if (auto maybe_parent_scope_identifier_group = m_parent_scope->m_identifier_groups.get(identifier_group_name); maybe_parent_scope_identifier_group.has_value()) {
                    maybe_parent_scope_identifier_group.value().identifiers.extend(identifier_group.identifiers);
                    if (identifier_group.captured_by_nested_function)
                        maybe_parent_scope_identifier_group.value().captured_by_nested_function = true;
                    if (identifier_group.used_inside_with_statement)
                        maybe_parent_scope_identifier_group.value().used_inside_with_statement = true;
                } else {
                    m_parent_scope->m_identifier_groups.set(identifier_group_name, identifier_group);
                }
            }
        }
    }
}

void ScopePusher::hoist_functions()
{
    for (size_t i = 0; i < m_functions_to_hoist.size(); i++) {
        auto const& function_declaration = m_functions_to_hoist[i];
        if (has_variable_with_flags(function_declaration->name(), ScopeVariable::IsLexical | ScopeVariable::IsForbiddenVar))
            continue;
        if (is_top_level()) {
            m_node->add_hoisted_function(move(m_functions_to_hoist[i]));
        } else {
            if (!m_parent_scope->has_variable_with_flags(function_declaration->name(), ScopeVariable::IsLexical | ScopeVariable::IsFunction))
                m_parent_scope->m_functions_to_hoist.append(move(m_functions_to_hoist[i]));
        }
    }
}

void ScopePusher::build_function_scope_data()
{
    auto data = make<FunctionScopeData>();

    // Extract functions_to_initialize from var-scoped function declarations (in reverse order, deduplicated).
    // This matches what for_each_var_function_declaration_in_reverse_order does.
    HashTable<Utf16FlyString> seen_function_names;
    for (ssize_t i = m_node->var_declaration_count() - 1; i >= 0; i--) {
        auto const& declaration = m_node->var_declarations()[i];
        if (is<FunctionDeclaration>(declaration)) {
            auto& function_decl = static_cast<FunctionDeclaration const&>(*declaration);
            if (seen_function_names.set(function_decl.name()) == AK::HashSetResult::InsertedNewEntry)
                data->functions_to_initialize.append(static_ptr_cast<FunctionDeclaration const>(declaration));
        }
    }

    // Check if "arguments" is a function name.
    data->has_function_named_arguments = seen_function_names.contains("arguments"_utf16_fly_string);

    // Check if "arguments" is a parameter name.
    data->has_argument_parameter = has_variable_with_flags("arguments"_utf16_fly_string, ScopeVariable::IsForbiddenLexical);

    // Check if "arguments" is lexically declared.
    MUST(m_node->for_each_lexically_declared_identifier([&](auto const& identifier) {
        if (identifier.string() == "arguments"_utf16_fly_string)
            data->has_lexically_declared_arguments = true;
    }));

    // Extract vars_to_initialize from variables with the IsVar flag.
    // Also count non-local vars for environment size pre-computation.
    for (auto& [name, var] : m_variables) {
        if (!(var.flags & ScopeVariable::IsVar))
            continue;

        bool is_parameter = var.flags & ScopeVariable::IsForbiddenLexical;
        bool is_non_local = !var.var_identifier->is_local();

        data->vars_to_initialize.append({
            .identifier = *var.var_identifier,
            .is_parameter = is_parameter,
            .is_function_name = seen_function_names.contains(name),
        });

        // Store var name for AnnexB checks.
        data->var_names.set(name);

        // Count non-local vars for environment size calculation.
        // Note: vars named "arguments" may be skipped at runtime if arguments object is needed,
        // but we count them here and adjust at runtime.
        if (is_non_local) {
            data->non_local_var_count_for_parameter_expressions++;
            if (!is_parameter)
                data->non_local_var_count++;
        }
    }

    m_node->set_function_scope_data(move(data));
}

void ScopePusher::register_identifier(NonnullRefPtr<Identifier> id, Optional<DeclarationKind> declaration_kind)
{
    if (auto maybe_identifier_group = m_identifier_groups.get(id->string()); maybe_identifier_group.has_value()) {
        maybe_identifier_group.value().identifiers.append(id);
    } else {
        m_identifier_groups.set(id->string(), IdentifierGroup {
                                                   .captured_by_nested_function = false,
                                                   .identifiers = { id },
                                                   .declaration_kind = declaration_kind,
                                               });
    }
}

void ScopePusher::set_uses_this()
{
    auto const* closest_function_scope = last_function_scope();
    auto uses_this_from_environment = closest_function_scope && closest_function_scope->m_is_arrow_function;
    for (auto* scope_ptr = this; scope_ptr; scope_ptr = scope_ptr->m_parent_scope) {
        if (scope_ptr->m_type == ScopeType::Function) {
            scope_ptr->m_uses_this = true;
            if (uses_this_from_environment)
                scope_ptr->m_uses_this_from_environment = true;
        }
    }
}

void ScopePusher::set_uses_new_target()
{
    for (auto* scope_ptr = this; scope_ptr; scope_ptr = scope_ptr->m_parent_scope) {
        if (scope_ptr->m_type == ScopeType::Function) {
            scope_ptr->m_uses_this = true;
            scope_ptr->m_uses_this_from_environment = true;
        }
    }
}

void ScopePusher::throw_identifier_declared(Utf16FlyString const& name, NonnullRefPtr<Declaration const> const& declaration)
{
    m_parser.syntax_error(MUST(String::formatted("Identifier '{}' already declared", name)), declaration->source_range().start);
}

}
