/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibJS/Parser.h>
#include <LibJS/ScopeCollector.h>

namespace JS {

ScopeCollector::ScopeCollector(Parser& parser)
    : m_parser(parser)
{
}

void ScopeCollector::open_scope(ScopeRecord::ScopeType type, ScopeNode* node, ScopeRecord::ScopeLevel level)
{
    auto record = make<ScopeRecord>();
    record->type = type;
    record->level = level;
    record->parent = m_current;

    if (type != ScopeRecord::ScopeType::Function) {
        VERIFY(node || (m_current && level == ScopeRecord::ScopeLevel::NotTopLevel));
        if (!node)
            record->ast_node = m_current->ast_node;
        else
            record->ast_node = node;
    }

    if (level == ScopeRecord::ScopeLevel::NotTopLevel)
        record->top_level = m_current->top_level;
    else
        record->top_level = record.ptr();

    auto* record_ptr = record.ptr();
    if (m_current) {
        m_current->children.append(move(record));
    } else {
        m_root = move(record);
    }
    m_current = record_ptr;
}

void ScopeCollector::close_scope()
{
    VERIFY(m_current);

    // Propagate flags needed during parsing to parent (stops at function boundaries).
    if (m_current->parent && !m_current->function_parameters) {
        m_current->parent->contains_access_to_arguments_object_in_non_strict_mode |= m_current->contains_access_to_arguments_object_in_non_strict_mode;
        m_current->parent->contains_direct_call_to_eval |= m_current->contains_direct_call_to_eval;
        m_current->parent->contains_await_expression |= m_current->contains_await_expression;
    }

    m_current = m_current->parent;
}

ScopeCollector::ScopeHandle ScopeCollector::open_program_scope(Program& program)
{
    open_scope(ScopeRecord::ScopeType::Program, &program,
        program.type() == Program::Type::Script ? ScopeRecord::ScopeLevel::ScriptTopLevel : ScopeRecord::ScopeLevel::ModuleTopLevel);
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_function_scope(RefPtr<Identifier const> function_name)
{
    open_scope(ScopeRecord::ScopeType::Function, nullptr, ScopeRecord::ScopeLevel::FunctionTopLevel);
    if (function_name)
        m_current->variables.ensure(function_name->string()).flags |= ScopeVariable::IsBound;
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_block_scope(ScopeNode& node)
{
    open_scope(ScopeRecord::ScopeType::Block, &node, ScopeRecord::ScopeLevel::NotTopLevel);
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_for_loop_scope(ScopeNode& node)
{
    open_scope(ScopeRecord::ScopeType::ForLoop, &node, ScopeRecord::ScopeLevel::NotTopLevel);
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_with_scope(ScopeNode& node)
{
    open_scope(ScopeRecord::ScopeType::With, &node, ScopeRecord::ScopeLevel::NotTopLevel);
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_catch_scope()
{
    open_scope(ScopeRecord::ScopeType::Catch, nullptr, ScopeRecord::ScopeLevel::NotTopLevel);
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_static_init_scope(ScopeNode& node)
{
    open_scope(ScopeRecord::ScopeType::ClassStaticInit, &node, ScopeRecord::ScopeLevel::StaticInitTopLevel);
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_class_field_scope(ScopeNode& node)
{
    open_scope(ScopeRecord::ScopeType::ClassField, &node, ScopeRecord::ScopeLevel::NotTopLevel);
    return ScopeHandle(*this);
}

ScopeCollector::ScopeHandle ScopeCollector::open_class_declaration_scope(RefPtr<Identifier const> class_name)
{
    open_scope(ScopeRecord::ScopeType::ClassDeclaration, nullptr, ScopeRecord::ScopeLevel::NotTopLevel);
    if (class_name)
        m_current->variables.ensure(class_name->string()).flags |= ScopeVariable::IsBound;
    return ScopeHandle(*this);
}

void ScopeCollector::add_catch_parameter(RefPtr<BindingPattern const> const& pattern, RefPtr<Identifier const> const& parameter)
{
    if (pattern) {
        // NOTE: Nothing in the callback throws an exception.
        MUST(pattern->for_each_bound_identifier([&](auto const& identifier) {
            auto& var = m_current->variables.ensure(identifier.string());
            var.flags |= ScopeVariable::IsForbiddenVar | ScopeVariable::IsBound | ScopeVariable::IsCatchParameter;
        }));
    } else if (parameter) {
        auto& var = m_current->variables.ensure(parameter->string());
        var.flags |= ScopeVariable::IsVar | ScopeVariable::IsBound | ScopeVariable::IsCatchParameter;
        var.var_identifier = parameter.ptr();
    }
}

void ScopeCollector::add_declaration(NonnullRefPtr<Declaration const> declaration)
{
    if (declaration->is_lexical_declaration()) {
        // NOTE: Nothing in the callback throws an exception.
        MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
            auto const& name = identifier.string();
            auto& var = m_current->variables.ensure(name);
            if (var.flags & (ScopeVariable::IsVar | ScopeVariable::IsForbiddenLexical | ScopeVariable::IsFunction | ScopeVariable::IsLexical))
                throw_identifier_declared(name, declaration);
            var.flags |= ScopeVariable::IsLexical;
        }));

        m_current->ast_node->add_lexical_declaration(move(declaration));
    } else if (!declaration->is_function_declaration()) {
        // NOTE: Nothing in the callback throws an exception.
        MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
            auto const& name = identifier.string();
            auto* scope = m_current;
            while (true) {
                auto& var = scope->variables.ensure(name);
                if (var.flags & (ScopeVariable::IsLexical | ScopeVariable::IsFunction | ScopeVariable::IsForbiddenVar))
                    throw_identifier_declared(name, declaration);

                var.flags |= ScopeVariable::IsVar;
                var.var_identifier = &identifier;
                if (scope->is_top_level())
                    break;

                VERIFY(scope->parent != nullptr);
                scope = scope->parent;
            }
            VERIFY(scope->is_top_level() && scope->ast_node);
            scope->ast_node->add_var_scoped_declaration(declaration);
        }));

        VERIFY(m_current->top_level);
        m_current->top_level->ast_node->add_var_scoped_declaration(move(declaration));
    } else {
        if (m_current->level != ScopeRecord::ScopeLevel::NotTopLevel && m_current->level != ScopeRecord::ScopeLevel::ModuleTopLevel) {
            // Only non-top levels and Module don't var declare the top functions
            // NOTE: Nothing in the callback throws an exception.
            MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
                auto& var = m_current->variables.ensure(identifier.string());
                var.flags |= ScopeVariable::IsVar;
                var.var_identifier = &identifier;
            }));
            m_current->ast_node->add_var_scoped_declaration(move(declaration));
        } else {
            VERIFY(is<FunctionDeclaration>(*declaration));
            auto function_declaration = static_ptr_cast<FunctionDeclaration const>(declaration);
            auto function_name = function_declaration->name();
            auto& var = m_current->variables.ensure(function_name);
            if (var.flags & (ScopeVariable::IsVar | ScopeVariable::IsLexical))
                throw_identifier_declared(function_name, declaration);

            if (function_declaration->kind() != FunctionKind::Normal || m_parser.m_state.strict_mode) {
                if (var.flags & ScopeVariable::IsFunction)
                    throw_identifier_declared(function_name, declaration);

                var.flags |= ScopeVariable::IsLexical;
                m_current->ast_node->add_lexical_declaration(move(declaration));
                return;
            }

            if (!(var.flags & ScopeVariable::IsLexical))
                m_current->functions_to_hoist.append(function_declaration);

            var.flags |= ScopeVariable::IsFunction;
            var.function_declaration = function_declaration;
            m_current->ast_node->add_lexical_declaration(move(declaration));
        }
    }
}

void ScopeCollector::register_identifier(NonnullRefPtr<Identifier> id, Optional<DeclarationKind> declaration_kind)
{
    if (auto maybe_identifier_group = m_current->identifier_groups.get(id->string()); maybe_identifier_group.has_value()) {
        maybe_identifier_group.value().identifiers.append(id);
    } else {
        m_current->identifier_groups.set(id->string(), IdentifierGroup {
                                                           .captured_by_nested_function = false,
                                                           .identifiers = { id },
                                                           .declaration_kind = declaration_kind,
                                                       });
    }
}

void ScopeCollector::set_function_parameters(NonnullRefPtr<FunctionParameters const> parameters)
{
    m_current->function_parameters = move(parameters);
    for (auto& parameter : m_current->function_parameters->parameters()) {
        parameter.binding.visit(
            [&](Identifier const& identifier) {
                register_identifier(fixme_launder_const_through_pointer_cast(identifier));
                auto& var = m_current->variables.ensure(identifier.string());
                var.flags |= ScopeVariable::IsParameterCandidate | ScopeVariable::IsForbiddenLexical;
            },
            [&](NonnullRefPtr<BindingPattern const> const& binding_pattern) {
                // NOTE: Nothing in the callback throws an exception.
                MUST(binding_pattern->for_each_bound_identifier([&](auto const& identifier) {
                    register_identifier(fixme_launder_const_through_pointer_cast(identifier));
                    auto& var = m_current->variables.ensure(identifier.string());
                    var.flags |= ScopeVariable::IsParameterCandidate | ScopeVariable::IsForbiddenLexical;
                }));
            });
    }
}

void ScopeCollector::set_scope_node(ScopeNode* node) { m_current->ast_node = node; }

void ScopeCollector::set_contains_direct_call_to_eval()
{
    m_current->contains_direct_call_to_eval = true;
    m_current->screwed_by_eval_in_scope_chain = true;
    m_current->eval_in_current_function = true;
}

void ScopeCollector::set_contains_access_to_arguments_object_in_non_strict_mode()
{
    m_current->contains_access_to_arguments_object_in_non_strict_mode = true;
}

void ScopeCollector::set_contains_await_expression() { m_current->contains_await_expression = true; }

void ScopeCollector::set_uses_this()
{
    auto const* closest_function_scope = m_current->last_function_scope();
    auto this_from_env = closest_function_scope && closest_function_scope->is_arrow_function;
    for (auto* scope = m_current; scope; scope = scope->parent) {
        if (scope->type == ScopeRecord::ScopeType::Function) {
            scope->uses_this = true;
            if (this_from_env)
                scope->uses_this_from_environment = true;
        }
    }
}

void ScopeCollector::set_uses_new_target()
{
    for (auto* scope = m_current; scope; scope = scope->parent) {
        if (scope->type == ScopeRecord::ScopeType::Function) {
            scope->uses_this = true;
            scope->uses_this_from_environment = true;
        }
    }
}

void ScopeCollector::set_is_arrow_function() { m_current->is_arrow_function = true; }
void ScopeCollector::set_is_function_declaration() { m_current->is_function_declaration = true; }

bool ScopeCollector::contains_direct_call_to_eval() const { return m_current->contains_direct_call_to_eval; }
bool ScopeCollector::uses_this_from_environment() const { return m_current->uses_this_from_environment; }
bool ScopeCollector::uses_this() const { return m_current->uses_this; }
bool ScopeCollector::contains_await_expression() const { return m_current->contains_await_expression; }

bool ScopeCollector::can_have_using_declaration() const
{
    return m_current->level != ScopeRecord::ScopeLevel::ScriptTopLevel;
}

ScopeRecord::ScopeType ScopeCollector::type() const { return m_current->type; }

bool ScopeCollector::has_declaration(Utf16FlyString const& name) const
{
    if (m_current->has_variable_with_flags(name, ScopeVariable::IsLexical | ScopeVariable::IsVar))
        return true;
    return m_current->functions_to_hoist.contains([&name](auto& function) { return function->name() == name; });
}

ScopeRecord const* ScopeCollector::last_function_scope() const { return m_current->last_function_scope(); }
ScopeRecord* ScopeCollector::parent_scope() { return m_current->parent; }
FunctionParameters const& ScopeCollector::function_parameters() const { return *m_current->function_parameters; }

bool ScopeCollector::has_declaration_in_current_function(Utf16FlyString const& name) const
{
    auto const* function_scope = m_current->last_function_scope();
    auto const* stop = function_scope ? function_scope->parent : nullptr;
    for (auto const* scope = m_current; scope != stop; scope = scope->parent) {
        if (scope->has_variable_with_flags(name, ScopeVariable::IsLexical | ScopeVariable::IsVar | ScopeVariable::IsParameterCandidate))
            return true;
        if (scope->functions_to_hoist.contains([&name](auto& function) { return function->name() == name; }))
            return true;
    }
    return false;
}

void ScopeCollector::throw_identifier_declared(Utf16FlyString const& name, NonnullRefPtr<Declaration const> const& declaration)
{
    m_parser.syntax_error(MUST(String::formatted("Identifier '{}' already declared", name)), declaration->source_range().start);
}

// --- Post-parse analysis ---

void ScopeCollector::analyze()
{
    if (m_root)
        analyze_recursive(*m_root);
}

void ScopeCollector::analyze_recursive(ScopeRecord& scope)
{
    // Process children first (bottom-up).
    for (auto& child : scope.children)
        analyze_recursive(*child);

    if (!scope.ast_node)
        return;

    propagate_eval_poisoning(scope);
    resolve_identifiers(scope, m_parser.m_state.initiated_by_eval);
    hoist_functions(scope);

    if (scope.type == ScopeRecord::ScopeType::Function && scope.function_parameters)
        build_function_scope_data(scope);
}

void ScopeCollector::propagate_eval_poisoning(ScopeRecord& scope)
{
    if (scope.parent && (scope.contains_direct_call_to_eval || scope.screwed_by_eval_in_scope_chain)) {
        scope.parent->screwed_by_eval_in_scope_chain = true;
    }

    // Propagate eval-in-current-function only through block scopes, not across function boundaries.
    // This is used for global identifier marking - eval can only inject vars into its containing
    // function's scope, not into parent function scopes.
    if (scope.parent && scope.eval_in_current_function && scope.type != ScopeRecord::ScopeType::Function) {
        scope.parent->eval_in_current_function = true;
    }
}

void ScopeCollector::resolve_identifiers(ScopeRecord& scope, bool initiated_by_eval)
{
    for (auto& it : scope.identifier_groups) {
        auto const& identifier_group_name = it.key;
        auto& identifier_group = it.value;

        if (identifier_group.declaration_kind.has_value()) {
            for (auto& identifier : identifier_group.identifiers) {
                identifier->set_declaration_kind(identifier_group.declaration_kind.value());
            }
        }

        auto var_it = scope.variables.find(identifier_group_name);
        u16 var_flags = (var_it != scope.variables.end()) ? var_it->value.flags : 0;

        Optional<LocalVariable::DeclarationKind> local_variable_declaration_kind;
        if (scope.is_top_level() && (var_flags & ScopeVariable::IsVar)) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::Var;
        } else if (var_flags & ScopeVariable::IsLexical) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::LetOrConst;
        } else if (var_flags & ScopeVariable::IsFunction) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::Function;
        }

        if (scope.type == ScopeRecord::ScopeType::Function && !scope.is_arrow_function && identifier_group_name == "arguments"sv) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::ArgumentsObject;
        }

        if (scope.type == ScopeRecord::ScopeType::Catch && (var_flags & ScopeVariable::IsCatchParameter)) {
            local_variable_declaration_kind = LocalVariable::DeclarationKind::CatchClauseParameter;
        }

        bool hoistable_function_declaration = scope.functions_to_hoist.contains([&](auto const& function_declaration) {
            return function_declaration->name() == identifier_group_name;
        });

        if (scope.type == ScopeRecord::ScopeType::ClassDeclaration && (var_flags & ScopeVariable::IsBound)) {
            continue;
        }

        if (scope.type == ScopeRecord::ScopeType::Function && !scope.is_function_declaration && (var_flags & ScopeVariable::IsBound)) {
            for (auto& identifier : identifier_group.identifiers)
                identifier->set_is_inside_scope_with_eval();
        }

        if (scope.type == ScopeRecord::ScopeType::ClassDeclaration) {
            local_variable_declaration_kind.clear();
        }

        bool is_function_parameter = false;
        if (scope.type == ScopeRecord::ScopeType::Function) {
            if ((var_flags & ScopeVariable::IsParameterCandidate)
                && (!scope.contains_access_to_arguments_object_in_non_strict_mode
                    || (scope.function_parameters && scope.function_parameters->has_rest_parameter_with_name(identifier_group_name)))) {
                // Rest parameters don't participate in the sloppy-mode
                // arguments-parameter linkage, so they can always be optimized.
                is_function_parameter = true;
            } else if (var_flags & ScopeVariable::IsForbiddenLexical) {
                continue;
            }
        }

        if (scope.type == ScopeRecord::ScopeType::Function && hoistable_function_declaration) {
            continue;
        }

        if (scope.type == ScopeRecord::ScopeType::Program) {
            auto can_use_global_for_identifier = !(identifier_group.used_inside_with_statement || initiated_by_eval);
            if (can_use_global_for_identifier) {
                for (auto& identifier : identifier_group.identifiers) {
                    if (!identifier->is_inside_scope_with_eval())
                        identifier->set_is_global();
                }
            }
        } else if (local_variable_declaration_kind.has_value() || is_function_parameter) {
            if (hoistable_function_declaration)
                continue;

            if (!identifier_group.captured_by_nested_function && !identifier_group.used_inside_with_statement) {
                if (scope.screwed_by_eval_in_scope_chain)
                    continue;

                auto local_scope = scope.last_function_scope();
                if (!local_scope) {
                    if (identifier_group.declaration_kind == DeclarationKind::Var)
                        continue;
                    local_scope = scope.top_level;
                }

                if (is_function_parameter) {
                    auto argument_index = local_scope->function_parameters->get_index_of_parameter_name(identifier_group_name);
                    if (argument_index.has_value()) {
                        for (auto& identifier : identifier_group.identifiers)
                            identifier->set_argument_index(argument_index.value());
                    } else {
                        // Destructured parameter binding: the argument slot holds the
                        // whole object/array, so the individual binding goes into a
                        // local variable slot instead.
                        auto local_variable_index = local_scope->ast_node->add_local_variable(identifier_group_name, LocalVariable::DeclarationKind::Var);
                        for (auto& identifier : identifier_group.identifiers)
                            identifier->set_local_variable_index(local_variable_index);
                    }
                } else {
                    auto local_variable_index = local_scope->ast_node->add_local_variable(identifier_group_name, *local_variable_declaration_kind);
                    for (auto& identifier : identifier_group.identifiers)
                        identifier->set_local_variable_index(local_variable_index);
                }
            }
        } else {
            if (scope.function_parameters || scope.type == ScopeRecord::ScopeType::ClassField || scope.type == ScopeRecord::ScopeType::ClassStaticInit) {
                identifier_group.captured_by_nested_function = true;
            }

            if (scope.type == ScopeRecord::ScopeType::With)
                identifier_group.used_inside_with_statement = true;

            if (scope.eval_in_current_function) {
                for (auto& identifier : identifier_group.identifiers)
                    identifier->set_is_inside_scope_with_eval();
            }

            if (scope.parent) {
                if (auto maybe_parent_group = scope.parent->identifier_groups.get(identifier_group_name); maybe_parent_group.has_value()) {
                    maybe_parent_group.value().identifiers.extend(identifier_group.identifiers);
                    if (identifier_group.captured_by_nested_function)
                        maybe_parent_group.value().captured_by_nested_function = true;
                    if (identifier_group.used_inside_with_statement)
                        maybe_parent_group.value().used_inside_with_statement = true;
                } else {
                    scope.parent->identifier_groups.set(identifier_group_name, identifier_group);
                }
            }
        }
    }
}

void ScopeCollector::hoist_functions(ScopeRecord& scope)
{
    for (size_t i = 0; i < scope.functions_to_hoist.size(); i++) {
        auto const& function_declaration = scope.functions_to_hoist[i];
        if (scope.has_variable_with_flags(function_declaration->name(), ScopeVariable::IsLexical | ScopeVariable::IsForbiddenVar))
            continue;
        if (scope.is_top_level()) {
            scope.ast_node->add_hoisted_function(move(scope.functions_to_hoist[i]));
        } else {
            if (!scope.parent->has_variable_with_flags(function_declaration->name(), ScopeVariable::IsLexical | ScopeVariable::IsFunction))
                scope.parent->functions_to_hoist.append(move(scope.functions_to_hoist[i]));
        }
    }
}

void ScopeCollector::build_function_scope_data(ScopeRecord& scope)
{
    auto data = make<FunctionScopeData>();

    HashTable<Utf16FlyString> seen_function_names;
    for (ssize_t i = scope.ast_node->var_declaration_count() - 1; i >= 0; i--) {
        auto const& declaration = scope.ast_node->var_declarations()[i];
        if (is<FunctionDeclaration>(declaration)) {
            auto& function_decl = static_cast<FunctionDeclaration const&>(*declaration);
            if (seen_function_names.set(function_decl.name()) == AK::HashSetResult::InsertedNewEntry)
                data->functions_to_initialize.append(static_ptr_cast<FunctionDeclaration const>(declaration));
        }
    }

    data->has_function_named_arguments = seen_function_names.contains("arguments"_utf16_fly_string);
    data->has_argument_parameter = scope.has_variable_with_flags("arguments"_utf16_fly_string, ScopeVariable::IsForbiddenLexical);

    MUST(scope.ast_node->for_each_lexically_declared_identifier([&](auto const& identifier) {
        if (identifier.string() == "arguments"_utf16_fly_string)
            data->has_lexically_declared_arguments = true;
    }));

    for (auto& [name, var] : scope.variables) {
        if (!(var.flags & ScopeVariable::IsVar))
            continue;

        bool is_parameter = var.flags & ScopeVariable::IsForbiddenLexical;
        bool is_non_local = !var.var_identifier->is_local();

        data->vars_to_initialize.append({
            .identifier = *var.var_identifier,
            .is_parameter = is_parameter,
            .is_function_name = seen_function_names.contains(name),
        });

        data->var_names.set(name);

        if (is_non_local) {
            data->non_local_var_count_for_parameter_expressions++;
            if (!is_parameter)
                data->non_local_var_count++;
        }
    }

    scope.ast_node->set_function_scope_data(move(data));
}

}
