/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Parser.h"
#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/HashTable.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/TemporaryChange.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibRegex/Regex.h>

namespace JS {

class ScopePusher {

    // NOTE: We really only need ModuleTopLevel and NotModuleTopLevel as the only
    //       difference seems to be in https://tc39.es/ecma262/#sec-static-semantics-varscopeddeclarations
    //       where ModuleItemList only does the VarScopedDeclaration and not the
    //       TopLevelVarScopedDeclarations.
    enum class ScopeLevel {
        NotTopLevel,
        ScriptTopLevel,
        ModuleTopLevel,
        FunctionTopLevel,
        StaticInitTopLevel
    };

public:
    enum class ScopeType {
        Function,
        Program,
        Block,
        ForLoop,
        With,
        Catch,
        ClassStaticInit,
        ClassField,
        ClassDeclaration,
    };

private:
    ScopePusher(Parser& parser, ScopeNode* node, ScopeLevel scope_level, ScopeType type)
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

    bool is_top_level()
    {
        return m_scope_level != ScopeLevel::NotTopLevel;
    }

public:
    static ScopePusher function_scope(Parser& parser, RefPtr<Identifier const> function_name = nullptr)
    {
        ScopePusher scope_pusher(parser, nullptr, ScopeLevel::FunctionTopLevel, ScopeType::Function);
        if (function_name) {
            scope_pusher.m_bound_names.set(function_name->string());
        }
        return scope_pusher;
    }

    static ScopePusher program_scope(Parser& parser, Program& program)
    {
        return ScopePusher(parser, &program, program.type() == Program::Type::Script ? ScopeLevel::ScriptTopLevel : ScopeLevel::ModuleTopLevel, ScopeType::Program);
    }

    static ScopePusher block_scope(Parser& parser, ScopeNode& node)
    {
        return ScopePusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::Block);
    }

    static ScopePusher for_loop_scope(Parser& parser, ScopeNode& node)
    {
        return ScopePusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::ForLoop);
    }

    static ScopePusher with_scope(Parser& parser, ScopeNode& node)
    {
        ScopePusher scope_pusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::With);
        return scope_pusher;
    }

    static ScopePusher catch_scope(Parser& parser, RefPtr<BindingPattern const> const& pattern, FlyString const& parameter)
    {
        ScopePusher scope_pusher(parser, nullptr, ScopeLevel::NotTopLevel, ScopeType::Catch);
        if (pattern) {
            // NOTE: Nothing in the callback throws an exception.
            MUST(pattern->for_each_bound_identifier([&](auto const& identifier) {
                scope_pusher.m_forbidden_var_names.set(identifier.string());
                scope_pusher.m_bound_names.set(identifier.string());
            }));
        } else if (!parameter.is_empty()) {
            scope_pusher.m_var_names.set(parameter);
            scope_pusher.m_bound_names.set(parameter);
        }

        return scope_pusher;
    }

    static ScopePusher static_init_block_scope(Parser& parser, ScopeNode& node)
    {
        ScopePusher scope_pusher(parser, &node, ScopeLevel::StaticInitTopLevel, ScopeType::ClassStaticInit);
        return scope_pusher;
    }

    static ScopePusher class_field_scope(Parser& parser, ScopeNode& node)
    {
        ScopePusher scope_pusher(parser, &node, ScopeLevel::NotTopLevel, ScopeType::ClassField);
        return scope_pusher;
    }

    static ScopePusher class_declaration_scope(Parser& parser, RefPtr<Identifier const> class_name)
    {
        ScopePusher scope_pusher(parser, nullptr, ScopeLevel::NotTopLevel, ScopeType::ClassDeclaration);
        if (class_name) {
            scope_pusher.m_bound_names.set(class_name->string());
        }
        return scope_pusher;
    }

    ScopeType type() const { return m_type; }

    void add_declaration(NonnullRefPtr<Declaration const> declaration)
    {
        if (declaration->is_lexical_declaration()) {
            // NOTE: Nothing in the callback throws an exception.
            MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
                auto const& name = identifier.string();
                if (m_var_names.contains(name) || m_forbidden_lexical_names.contains(name) || m_function_names.contains(name))
                    throw_identifier_declared(name, declaration);

                if (m_lexical_names.set(name) != AK::HashSetResult::InsertedNewEntry)
                    throw_identifier_declared(name, declaration);
            }));

            m_node->add_lexical_declaration(move(declaration));
        } else if (!declaration->is_function_declaration()) {
            // NOTE: Nothing in the callback throws an exception.
            MUST(declaration->for_each_bound_identifier([&](auto const& identifier) {
                auto const& name = identifier.string();
                ScopePusher* pusher = this;
                while (true) {
                    if (pusher->m_lexical_names.contains(name)
                        || pusher->m_function_names.contains(name)
                        || pusher->m_forbidden_var_names.contains(name))
                        throw_identifier_declared(name, declaration);

                    pusher->m_var_names.set(name);
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
                    m_var_names.set(identifier.string());
                }));
                m_node->add_var_scoped_declaration(move(declaration));
            } else {
                VERIFY(is<FunctionDeclaration>(*declaration));
                auto& function_declaration = static_cast<FunctionDeclaration const&>(*declaration);
                auto function_name = function_declaration.name();
                if (m_var_names.contains(function_name) || m_lexical_names.contains(function_name))
                    throw_identifier_declared(function_name, declaration);

                if (function_declaration.kind() != FunctionKind::Normal || m_parser.m_state.strict_mode) {
                    if (m_function_names.contains(function_name))
                        throw_identifier_declared(function_name, declaration);

                    m_lexical_names.set(function_name);
                    m_node->add_lexical_declaration(move(declaration));
                    return;
                }

                m_function_names.set(function_name);
                if (!m_lexical_names.contains(function_name))
                    m_functions_to_hoist.append(static_ptr_cast<FunctionDeclaration const>(declaration));

                m_node->add_lexical_declaration(move(declaration));
            }
        }
    }

    ScopePusher const* last_function_scope() const
    {
        for (auto scope_ptr = this; scope_ptr; scope_ptr = scope_ptr->m_parent_scope) {
            if (scope_ptr->m_function_parameters)
                return scope_ptr;
        }
        return nullptr;
    }

    auto const& function_parameters() const
    {
        return *m_function_parameters;
    }

    ScopePusher* parent_scope() { return m_parent_scope; }
    ScopePusher const* parent_scope() const { return m_parent_scope; }

    [[nodiscard]] bool has_declaration(FlyString const& name) const
    {
        return m_lexical_names.contains(name) || m_var_names.contains(name) || !m_functions_to_hoist.find_if([&name](auto& function) { return function->name() == name; }).is_end();
    }

    bool contains_direct_call_to_eval() const { return m_contains_direct_call_to_eval; }
    bool contains_access_to_arguments_object() const { return m_contains_access_to_arguments_object; }
    void set_contains_direct_call_to_eval()
    {
        m_contains_direct_call_to_eval = true;
        m_screwed_by_eval_in_scope_chain = true;
    }
    void set_contains_access_to_arguments_object() { m_contains_access_to_arguments_object = true; }
    void set_scope_node(ScopeNode* node) { m_node = node; }
    void set_function_parameters(NonnullRefPtr<FunctionParameters const> parameters)
    {
        m_function_parameters = move(parameters);
        for (auto& parameter : m_function_parameters->parameters()) {
            parameter.binding.visit(
                [&](Identifier const& identifier) {
                    register_identifier(identifier);
                    m_function_parameters_candidates_for_local_variables.set(identifier.string());
                    m_forbidden_lexical_names.set(identifier.string());
                },
                [&](NonnullRefPtr<BindingPattern const> const& binding_pattern) {
                    // NOTE: Nothing in the callback throws an exception.
                    MUST(binding_pattern->for_each_bound_identifier([&](auto const& identifier) {
                        m_forbidden_lexical_names.set(identifier.string());
                    }));
                });
        }
    }

    ~ScopePusher()
    {
        VERIFY(is_top_level() || m_parent_scope);

        if (m_parent_scope && !m_function_parameters) {
            m_parent_scope->m_contains_access_to_arguments_object |= m_contains_access_to_arguments_object;
            m_parent_scope->m_contains_direct_call_to_eval |= m_contains_direct_call_to_eval;
            m_parent_scope->m_contains_await_expression |= m_contains_await_expression;
        }

        if (!m_node) {
            m_parser.m_state.current_scope_pusher = m_parent_scope;
            return;
        }

        if (m_parent_scope && (m_contains_direct_call_to_eval || m_screwed_by_eval_in_scope_chain)) {
            m_parent_scope->m_screwed_by_eval_in_scope_chain = true;
        }

        for (auto& it : m_identifier_groups) {
            auto const& identifier_group_name = it.key;
            auto& identifier_group = it.value;

            if (m_parser.m_state.in_catch_parameter_context) {
                // NOTE: The parser currently cannot determine if an identifier captured by a function belongs to the environment created by a catch parameter.
                //       As a result, any identifiers used inside the catch parameter are not considered as candidates for optimization in local or global variable access.
                continue;
            }

            bool scope_has_declaration = false;
            if (is_top_level() && m_var_names.contains(identifier_group_name))
                scope_has_declaration = true;
            else if (m_lexical_names.contains(identifier_group_name) || m_function_names.contains(identifier_group_name))
                scope_has_declaration = true;

            if (m_type == ScopeType::Function && !m_is_arrow_function && identifier_group_name == "arguments"sv)
                scope_has_declaration = true;

            bool hoistable_function_declaration = false;
            for (auto const& function_declaration : m_functions_to_hoist) {
                if (function_declaration->name() == identifier_group_name)
                    hoistable_function_declaration = true;
            }

            if ((m_type == ScopeType::ClassDeclaration || m_type == ScopeType::Catch) && m_bound_names.contains(identifier_group_name)) {
                // NOTE: Currently, the parser cannot recognize that assigning a named function expression creates a scope with a binding for the function name.
                //       As a result, function names are not considered as candidates for optimization in global variable access.
                continue;
            }

            if (m_type == ScopeType::Function && m_bound_names.contains(identifier_group_name)) {
                // NOTE: Currently parser can't determine that named function expression assigment creates scope with binding for funciton name so function names are not considered as candidates to be optmized in global variables access
                identifier_group.might_be_variable_in_lexical_scope_in_named_function_assignment = true;
            }

            if (m_type == ScopeType::ClassDeclaration || m_type == ScopeType::Catch) {
                // NOTE: Class declaration and catch scopes do not have own ScopeNode hence can't contain declaration of any variable
                scope_has_declaration = false;
            }

            if (m_type == ScopeType::Function) {
                if (!m_contains_access_to_arguments_object && m_function_parameters_candidates_for_local_variables.contains(identifier_group_name)) {
                    scope_has_declaration = true;
                } else if (m_forbidden_lexical_names.contains(identifier_group_name)) {
                    // NOTE: If an identifier is used as a function parameter that cannot be optimized locally or globally, it is simply ignored.
                    continue;
                }
            }

            if (m_type == ScopeType::Function && hoistable_function_declaration) {
                // NOTE: Hoistable function declarations are currently not optimized into global or local variables, but future improvements may change that.
                continue;
            }

            if (m_type == ScopeType::Program) {
                auto can_use_global_for_identifier = !(identifier_group.used_inside_with_statement || identifier_group.might_be_variable_in_lexical_scope_in_named_function_assignment || identifier_group.used_inside_scope_with_eval || m_parser.m_state.initiated_by_eval);
                if (can_use_global_for_identifier) {
                    for (auto& identifier : identifier_group.identifiers)
                        identifier->set_is_global();
                }
            } else if (scope_has_declaration) {
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

                    auto local_variable_index = local_scope->m_node->add_local_variable(identifier_group_name);
                    for (auto& identifier : identifier_group.identifiers)
                        identifier->set_local_variable_index(local_variable_index);
                }
            } else {
                if (m_function_parameters || m_type == ScopeType::ClassField || m_type == ScopeType::ClassStaticInit) {
                    // NOTE: Class fields and class static initialization sections implicitly create functions
                    identifier_group.captured_by_nested_function = true;
                }

                if (m_type == ScopeType::With)
                    identifier_group.used_inside_with_statement = true;

                if (m_contains_direct_call_to_eval)
                    identifier_group.used_inside_scope_with_eval = true;

                if (m_parent_scope) {
                    if (auto maybe_parent_scope_identifier_group = m_parent_scope->m_identifier_groups.get(identifier_group_name); maybe_parent_scope_identifier_group.has_value()) {
                        maybe_parent_scope_identifier_group.value().identifiers.extend(identifier_group.identifiers);
                        if (identifier_group.captured_by_nested_function)
                            maybe_parent_scope_identifier_group.value().captured_by_nested_function = true;
                        if (identifier_group.used_inside_with_statement)
                            maybe_parent_scope_identifier_group.value().used_inside_with_statement = true;
                        if (identifier_group.might_be_variable_in_lexical_scope_in_named_function_assignment)
                            maybe_parent_scope_identifier_group.value().might_be_variable_in_lexical_scope_in_named_function_assignment = true;
                        if (identifier_group.used_inside_scope_with_eval)
                            maybe_parent_scope_identifier_group.value().used_inside_scope_with_eval = true;
                    } else {
                        m_parent_scope->m_identifier_groups.set(identifier_group_name, identifier_group);
                    }
                }
            }
        }

        for (size_t i = 0; i < m_functions_to_hoist.size(); i++) {
            auto const& function_declaration = m_functions_to_hoist[i];
            if (m_lexical_names.contains(function_declaration->name()) || m_forbidden_var_names.contains(function_declaration->name()))
                continue;
            if (is_top_level()) {
                m_node->add_hoisted_function(move(m_functions_to_hoist[i]));
            } else {
                if (!m_parent_scope->m_lexical_names.contains(function_declaration->name()) && !m_parent_scope->m_function_names.contains(function_declaration->name()))
                    m_parent_scope->m_functions_to_hoist.append(move(m_functions_to_hoist[i]));
            }
        }

        VERIFY(m_parser.m_state.current_scope_pusher == this);
        m_parser.m_state.current_scope_pusher = m_parent_scope;
    }

    void set_contains_await_expression()
    {
        m_contains_await_expression = true;
    }

    bool contains_await_expression() const
    {
        return m_contains_await_expression;
    }

    bool can_have_using_declaration() const
    {
        return m_scope_level != ScopeLevel::ScriptTopLevel;
    }

    void register_identifier(NonnullRefPtr<Identifier> id, Optional<DeclarationKind> declaration_kind = {})
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

    bool uses_this() const { return m_uses_this; }
    bool uses_this_from_environment() const { return m_uses_this_from_environment; }

    void set_uses_this()
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

    void set_uses_new_target()
    {
        for (auto* scope_ptr = this; scope_ptr; scope_ptr = scope_ptr->m_parent_scope) {
            if (scope_ptr->m_type == ScopeType::Function) {
                scope_ptr->m_uses_this = true;
                scope_ptr->m_uses_this_from_environment = true;
            }
        }
    }

    void set_is_arrow_function()
    {
        m_is_arrow_function = true;
    }

private:
    void throw_identifier_declared(FlyString const& name, NonnullRefPtr<Declaration const> const& declaration)
    {
        m_parser.syntax_error(MUST(String::formatted("Identifier '{}' already declared", name)), declaration->source_range().start);
    }

    Parser& m_parser;
    ScopeNode* m_node { nullptr };
    ScopeLevel m_scope_level { ScopeLevel::NotTopLevel };
    ScopeType m_type;

    ScopePusher* m_parent_scope { nullptr };
    ScopePusher* m_top_level_scope { nullptr };

    HashTable<FlyString> m_lexical_names;
    HashTable<FlyString> m_var_names;
    HashTable<FlyString> m_function_names;

    HashTable<FlyString> m_forbidden_lexical_names;
    HashTable<FlyString> m_forbidden_var_names;
    Vector<NonnullRefPtr<FunctionDeclaration const>> m_functions_to_hoist;

    HashTable<FlyString> m_bound_names;
    HashTable<FlyString> m_function_parameters_candidates_for_local_variables;

    struct IdentifierGroup {
        bool captured_by_nested_function { false };
        bool used_inside_with_statement { false };
        bool used_inside_scope_with_eval { false };
        bool might_be_variable_in_lexical_scope_in_named_function_assignment { false };
        Vector<NonnullRefPtr<Identifier>> identifiers;
        Optional<DeclarationKind> declaration_kind;
    };
    HashMap<FlyString, IdentifierGroup> m_identifier_groups;

    RefPtr<FunctionParameters const> m_function_parameters;

    bool m_contains_access_to_arguments_object { false };
    bool m_contains_direct_call_to_eval { false };
    bool m_contains_await_expression { false };
    bool m_screwed_by_eval_in_scope_chain { false };

    // Function uses this binding from function environment if:
    // 1. It's an arrow function or establish parent scope for an arrow function
    // 2. Uses new.target
    bool m_uses_this_from_environment { false };
    bool m_uses_this { false };
    bool m_is_arrow_function { false };
};

class OperatorPrecedenceTable {
public:
    constexpr OperatorPrecedenceTable()
        : m_token_precedence()
    {
        for (size_t i = 0; i < array_size(m_operator_precedence); ++i) {
            auto& op = m_operator_precedence[i];
            m_token_precedence[static_cast<size_t>(op.token)] = op.precedence;
        }
    }

    constexpr int get(TokenType token) const
    {
        int p = m_token_precedence[static_cast<size_t>(token)];
        if (p == 0) {
            warnln("Internal Error: No precedence for operator {}", Token::name(token));
            VERIFY_NOT_REACHED();
            return -1;
        }

        return p;
    }

    constexpr int get_unary(TokenType token) const
    {
        constexpr int operator_precedence_unary_plus_minus = 17;
        switch (token) {
        case TokenType::Minus:
        case TokenType::Plus:
            return operator_precedence_unary_plus_minus;
        default:
            return get(token);
        }
    }

private:
    int m_token_precedence[cs_num_of_js_tokens];

    struct OperatorPrecedence {
        TokenType token;
        int precedence;
    };

    // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Operators/Operator_Precedence
    static constexpr OperatorPrecedence const m_operator_precedence[] = {
        { TokenType::Period, 20 },
        { TokenType::BracketOpen, 20 },
        { TokenType::ParenOpen, 20 },
        { TokenType::QuestionMarkPeriod, 20 },

        { TokenType::New, 19 },

        { TokenType::PlusPlus, 18 },
        { TokenType::MinusMinus, 18 },

        { TokenType::ExclamationMark, 17 },
        { TokenType::Tilde, 17 },
        { TokenType::Typeof, 17 },
        { TokenType::Void, 17 },
        { TokenType::Delete, 17 },
        { TokenType::Await, 17 },

        { TokenType::DoubleAsterisk, 16 },

        { TokenType::Asterisk, 15 },
        { TokenType::Slash, 15 },
        { TokenType::Percent, 15 },

        { TokenType::Plus, 14 },
        { TokenType::Minus, 14 },

        { TokenType::ShiftLeft, 13 },
        { TokenType::ShiftRight, 13 },
        { TokenType::UnsignedShiftRight, 13 },

        { TokenType::LessThan, 12 },
        { TokenType::LessThanEquals, 12 },
        { TokenType::GreaterThan, 12 },
        { TokenType::GreaterThanEquals, 12 },
        { TokenType::In, 12 },
        { TokenType::Instanceof, 12 },

        { TokenType::EqualsEquals, 11 },
        { TokenType::ExclamationMarkEquals, 11 },
        { TokenType::EqualsEqualsEquals, 11 },
        { TokenType::ExclamationMarkEqualsEquals, 11 },

        { TokenType::Ampersand, 10 },

        { TokenType::Caret, 9 },

        { TokenType::Pipe, 8 },

        { TokenType::DoubleQuestionMark, 7 },

        { TokenType::DoubleAmpersand, 6 },

        { TokenType::DoublePipe, 5 },

        { TokenType::QuestionMark, 4 },

        { TokenType::Equals, 3 },
        { TokenType::PlusEquals, 3 },
        { TokenType::MinusEquals, 3 },
        { TokenType::DoubleAsteriskEquals, 3 },
        { TokenType::AsteriskEquals, 3 },
        { TokenType::SlashEquals, 3 },
        { TokenType::PercentEquals, 3 },
        { TokenType::ShiftLeftEquals, 3 },
        { TokenType::ShiftRightEquals, 3 },
        { TokenType::UnsignedShiftRightEquals, 3 },
        { TokenType::AmpersandEquals, 3 },
        { TokenType::CaretEquals, 3 },
        { TokenType::PipeEquals, 3 },
        { TokenType::DoubleAmpersandEquals, 3 },
        { TokenType::DoublePipeEquals, 3 },
        { TokenType::DoubleQuestionMarkEquals, 3 },

        { TokenType::Yield, 2 },

        { TokenType::Comma, 1 },
    };
};

constexpr OperatorPrecedenceTable g_operator_precedence;

Parser::ParserState::ParserState(Lexer l, Program::Type program_type)
    : lexer(move(l))
{
    if (program_type == Program::Type::Module)
        lexer.disallow_html_comments();
    current_token = lexer.next();
}

Parser::Parser(Lexer lexer, Program::Type program_type, Optional<EvalInitialState> initial_state_for_eval)
    : m_source_code(SourceCode::create(lexer.filename(), String::from_byte_string(lexer.source()).release_value_but_fixme_should_propagate_errors()))
    , m_state(move(lexer), program_type)
    , m_program_type(program_type)
{
    if (initial_state_for_eval.has_value()) {
        m_state.initiated_by_eval = true;
        m_state.in_eval_function_context = initial_state_for_eval->in_eval_function_context;
        m_state.allow_super_property_lookup = initial_state_for_eval->allow_super_property_lookup;
        m_state.allow_super_constructor_call = initial_state_for_eval->allow_super_constructor_call;
        m_state.in_class_field_initializer = initial_state_for_eval->in_class_field_initializer;
    }
}

Associativity Parser::operator_associativity(TokenType type) const
{
    switch (type) {
    case TokenType::Period:
    case TokenType::BracketOpen:
    case TokenType::ParenOpen:
    case TokenType::QuestionMarkPeriod:
    case TokenType::Asterisk:
    case TokenType::Slash:
    case TokenType::Percent:
    case TokenType::Plus:
    case TokenType::Minus:
    case TokenType::ShiftLeft:
    case TokenType::ShiftRight:
    case TokenType::UnsignedShiftRight:
    case TokenType::LessThan:
    case TokenType::LessThanEquals:
    case TokenType::GreaterThan:
    case TokenType::GreaterThanEquals:
    case TokenType::In:
    case TokenType::Instanceof:
    case TokenType::EqualsEquals:
    case TokenType::ExclamationMarkEquals:
    case TokenType::EqualsEqualsEquals:
    case TokenType::ExclamationMarkEqualsEquals:
    case TokenType::Typeof:
    case TokenType::Void:
    case TokenType::Delete:
    case TokenType::Await:
    case TokenType::Ampersand:
    case TokenType::Caret:
    case TokenType::Pipe:
    case TokenType::DoubleQuestionMark:
    case TokenType::DoubleAmpersand:
    case TokenType::DoublePipe:
    case TokenType::Comma:
        return Associativity::Left;
    default:
        return Associativity::Right;
    }
}

bool Parser::parse_directive(ScopeNode& body)
{
    bool found_use_strict = false;
    while (!done() && match(TokenType::StringLiteral)) {
        auto raw_value = m_state.current_token.original_value();
        // It cannot be a labelled function since we hit a string literal.
        auto statement = parse_statement(AllowLabelledFunction::No);
        body.append(statement);

        VERIFY(is<ExpressionStatement>(*statement));
        auto& expression = static_cast<ExpressionStatement const&>(*statement).expression();

        if (!is<StringLiteral>(expression))
            break;

        if (raw_value.is_one_of("'use strict'"sv, "\"use strict\"")) {
            found_use_strict = true;

            if (m_state.string_legacy_octal_escape_sequence_in_scope)
                syntax_error("Octal escape sequence in string literal not allowed in strict mode"_string);
            break;
        }
    }

    m_state.string_legacy_octal_escape_sequence_in_scope = false;
    return found_use_strict;
}

NonnullRefPtr<Program> Parser::parse_program(bool starts_in_strict_mode)
{
    auto rule_start = push_start();
    auto program = adopt_ref(*new Program({ m_source_code, rule_start.position(), position() }, m_program_type));
    ScopePusher program_scope = ScopePusher::program_scope(*this, *program);

    if (m_program_type == Program::Type::Script)
        parse_script(program, starts_in_strict_mode);
    else
        parse_module(program);

    program->set_end_offset({}, position().offset);
    return program;
}

void Parser::parse_script(Program& program, bool starts_in_strict_mode)
{
    bool strict_before = m_state.strict_mode;
    if (starts_in_strict_mode)
        m_state.strict_mode = true;

    bool has_use_strict = parse_directive(program);

    if (m_state.strict_mode || has_use_strict) {
        program.set_strict_mode();
        m_state.strict_mode = true;
    }

    parse_statement_list(program, AllowLabelledFunction::Yes);
    if (!done()) {
        expected("statement or declaration");
        consume();
    }

    m_state.strict_mode = strict_before;
}

void Parser::parse_module(Program& program)
{
    TemporaryChange strict_mode_rollback(m_state.strict_mode, true);
    TemporaryChange await_expression_valid_rollback(m_state.await_expression_is_valid, true);

    // Since strict mode is already enabled we skip any directive parsing.
    while (!done()) {
        parse_statement_list(program, AllowLabelledFunction::Yes);

        if (done())
            break;

        if (match_export_or_import()) {
            VERIFY(m_state.current_token.type() == TokenType::Export || m_state.current_token.type() == TokenType::Import);
            if (m_state.current_token.type() == TokenType::Export)
                program.append_export(parse_export_statement(program));
            else
                program.append_import(parse_import_statement(program));

        } else {
            expected("statement or declaration");
            consume();
        }
    }

    VERIFY(m_state.current_scope_pusher);
    if (m_state.current_scope_pusher->contains_await_expression())
        program.set_has_top_level_await();

    for (auto& export_statement : program.exports()) {
        if (export_statement->has_statement())
            continue;
        for (auto& entry : export_statement->entries()) {
            if (entry.is_module_request() || entry.kind == ExportEntry::Kind::EmptyNamedExport)
                return;

            auto const& exported_name = entry.local_or_import_name;
            bool found = false;
            // NOTE: Nothing in the callback throws an exception.
            MUST(program.for_each_lexically_declared_identifier([&](auto const& identifier) {
                if (identifier.string() == exported_name)
                    found = true;
            }));
            if (found)
                continue;
            // NOTE: Nothing in the callback throws an exception.
            MUST(program.for_each_var_declared_identifier([&](auto const& identifier) {
                if (identifier.string() == exported_name)
                    found = true;
            }));
            for (auto& import : program.imports()) {
                if (import->has_bound_name(exported_name.value())) {
                    found = true;
                    break;
                }
            }

            if (!found)
                syntax_error(MUST(String::formatted("'{}' in export is not declared", exported_name)), export_statement->source_range().start);
        }
    }
}

NonnullRefPtr<Declaration const> Parser::parse_declaration()
{
    auto rule_start = push_start();
    if (m_state.current_token.type() == TokenType::Async && next_token().type() == TokenType::Function)
        return parse_function_node<FunctionDeclaration>();
    switch (m_state.current_token.type()) {
    case TokenType::Class:
        return parse_class_declaration();
    case TokenType::Function:
        return parse_function_node<FunctionDeclaration>();
    case TokenType::Let:
    case TokenType::Const:
        return parse_variable_declaration();
    case TokenType::Identifier:
        if (m_state.current_token.original_value() == "using"sv) {
            if (!m_state.current_scope_pusher->can_have_using_declaration())
                syntax_error("'using' not allowed outside of block, for loop or function"_string);

            return parse_using_declaration();
        }
        [[fallthrough]];
    default:
        expected("declaration");
        consume();
        return create_ast_node<ErrorDeclaration>({ m_source_code, rule_start.position(), position() });
    }
}

NonnullRefPtr<Statement const> Parser::parse_statement(AllowLabelledFunction allow_labelled_function)
{
    auto rule_start = push_start();
    auto type = m_state.current_token.type();
    switch (type) {
    case TokenType::CurlyOpen:
        return parse_block_statement();
    case TokenType::Return:
        return parse_return_statement();
    case TokenType::Var: {
        auto declaration = parse_variable_declaration();
        m_state.current_scope_pusher->add_declaration(declaration);
        return declaration;
    }
    case TokenType::For:
        return parse_for_statement();
    case TokenType::If:
        return parse_if_statement();
    case TokenType::Throw:
        return parse_throw_statement();
    case TokenType::Try:
        return parse_try_statement();
    case TokenType::Break:
        return parse_break_statement();
    case TokenType::Continue:
        return parse_continue_statement();
    case TokenType::Switch:
        return parse_switch_statement();
    case TokenType::Do:
        return parse_do_while_statement();
    case TokenType::While:
        return parse_while_statement();
    case TokenType::With:
        if (m_state.strict_mode)
            syntax_error("'with' statement not allowed in strict mode"_string);
        return parse_with_statement();
    case TokenType::Debugger:
        return parse_debugger_statement();
    case TokenType::Semicolon:
        consume();
        return create_ast_node<EmptyStatement>({ m_source_code, rule_start.position(), position() });
    case TokenType::Slash:
    case TokenType::SlashEquals:
        m_state.current_token = m_state.lexer.force_slash_as_regex();
        [[fallthrough]];
    default:
        if (match_invalid_escaped_keyword())
            syntax_error("Keyword must not contain escaped characters"_string);

        if (match_identifier_name()) {
            auto result = try_parse_labelled_statement(allow_labelled_function);
            if (!result.is_null())
                return result.release_nonnull();
        }
        if (match_expression()) {
            if (match(TokenType::Async)) {
                auto lookahead_token = next_token();
                if (lookahead_token.type() == TokenType::Function && !lookahead_token.trivia_contains_line_terminator())
                    syntax_error("Async function declaration not allowed in single-statement context"_string);
            } else if (match(TokenType::Function) || match(TokenType::Class)) {
                syntax_error(MUST(String::formatted("{} declaration not allowed in single-statement context", m_state.current_token.name())));
            } else if (match(TokenType::Let) && next_token().type() == TokenType::BracketOpen) {
                syntax_error(MUST(String::formatted("let followed by [ is not allowed in single-statement context")));
            }

            auto expr = parse_expression(0);
            consume_or_insert_semicolon();
            return create_ast_node<ExpressionStatement>({ m_source_code, rule_start.position(), position() }, move(expr));
        }
        expected("statement");
        consume();
        return create_ast_node<ErrorStatement>({ m_source_code, rule_start.position(), position() });
    }
}

bool Parser::match_invalid_escaped_keyword() const
{
    if (m_state.current_token.type() != TokenType::EscapedKeyword)
        return false;
    auto token_value = m_state.current_token.value();
    if (token_value == "await"sv)
        return m_program_type == Program::Type::Module || m_state.await_expression_is_valid;
    if (token_value == "async"sv)
        return false;
    if (token_value == "yield"sv)
        return m_state.in_generator_function_context;
    if (m_state.strict_mode)
        return true;
    return token_value != "let"sv;
}

static constexpr AK::Array<StringView, 9> strict_reserved_words = { "implements"sv, "interface"sv, "let"sv, "package"sv, "private"sv, "protected"sv, "public"sv, "static"sv, "yield"sv };

static bool is_strict_reserved_word(StringView str)
{
    return any_of(strict_reserved_words, [&str](StringView word) {
        return word == str;
    });
}

static bool is_simple_parameter_list(FunctionParameters const& parameters)
{
    return all_of(parameters.parameters(), [](FunctionParameter const& parameter) {
        return !parameter.is_rest && parameter.default_value.is_null() && parameter.binding.has<NonnullRefPtr<Identifier const>>();
    });
}

RefPtr<FunctionExpression const> Parser::try_parse_arrow_function_expression(bool expect_parens, bool is_async)
{
    TemporaryChange in_formal_parameter_context_rollback(m_state.in_formal_parameter_context, false);

    if (is_async)
        VERIFY(match(TokenType::Async));

    if (!expect_parens && !is_async) {
        // NOTE: This is a fast path where we try to fail early in case this can't possibly
        //       be a match. The idea is to avoid the expensive parser state save/load mechanism.
        //       The logic is duplicated below in the "real" !expect_parens branch.
        if (!match_identifier() && !match(TokenType::Yield) && !match(TokenType::Await))
            return nullptr;
        auto token = next_token();
        if (token.trivia_contains_line_terminator())
            return nullptr;
        if (token.type() != TokenType::Arrow)
            return nullptr;
    }

    save_state();
    auto rule_start = (expect_parens && !is_async)
        // Someone has consumed the opening parenthesis for us! Start there.
        ? RulePosition { *this, m_rule_starts.last() }
        // We've not encountered one yet, so the rule start is actually here.
        : push_start();

    ArmedScopeGuard state_rollback_guard = [&] {
        load_state();
    };

    auto function_kind = FunctionKind::Normal;

    if (is_async) {
        consume(TokenType::Async);
        function_kind = FunctionKind::Async;
        if (m_state.current_token.trivia_contains_line_terminator())
            return nullptr;

        // Since we have async it can be followed by paren open in the expect_parens case
        // so we also consume that token.
        if (expect_parens) {
            VERIFY(match(TokenType::ParenOpen));
            consume(TokenType::ParenOpen);
        }
    }

    auto parameters = FunctionParameters::empty();
    i32 function_length = -1;
    FunctionParsingInsights parsing_insights;
    parsing_insights.might_need_arguments_object = false;
    auto function_body_result = [&]() -> RefPtr<FunctionBody const> {
        ScopePusher function_scope = ScopePusher::function_scope(*this);
        function_scope.set_is_arrow_function();

        if (expect_parens) {
            // We have parens around the function parameters and can re-use the same parsing
            // logic used for regular functions: multiple parameters, default values, rest
            // parameter, maybe a trailing comma. If we have a new syntax error afterwards we
            // check if it's about a wrong token (something like duplicate parameter name must
            // not abort), know parsing failed and rollback the parser state.
            auto previous_syntax_errors = m_state.errors.size();
            TemporaryChange in_async_context(m_state.await_expression_is_valid, is_async || m_state.await_expression_is_valid);

            parameters = parse_formal_parameters(function_length, FunctionNodeParseOptions::IsArrowFunction | (is_async ? FunctionNodeParseOptions::IsAsyncFunction : 0));
            if (m_state.errors.size() > previous_syntax_errors && m_state.errors[previous_syntax_errors].message.bytes_as_string_view().starts_with("Unexpected token"sv))
                return nullptr;
            if (!match(TokenType::ParenClose))
                return nullptr;
            consume();
        } else {
            // No parens - this must be an identifier followed by arrow. That's it.
            if (!match_identifier() && !match(TokenType::Yield) && !match(TokenType::Await))
                return nullptr;
            auto token = consume_identifier_reference();
            if (m_state.strict_mode && token.value().is_one_of("arguments"sv, "eval"sv))
                syntax_error("BindingIdentifier may not be 'arguments' or 'eval' in strict mode"_string);
            if (is_async && token.value() == "await"sv)
                syntax_error("'await' is a reserved identifier in async functions"_string);
            auto identifier = create_ast_node<Identifier const>({ m_source_code, rule_start.position(), position() }, token.fly_string_value());
            parameters = FunctionParameters::create(Vector<FunctionParameter> { FunctionParameter { identifier, {} } });
        }

        // If there's a newline between the closing paren and arrow it's not a valid arrow function,
        // ASI should kick in instead (it'll then fail with "Unexpected token Arrow")
        if (m_state.current_token.trivia_contains_line_terminator())
            return nullptr;
        if (!match(TokenType::Arrow))
            return nullptr;
        consume();

        if (function_length == -1)
            function_length = parameters->size();

        auto old_labels_in_scope = move(m_state.labels_in_scope);
        ScopeGuard guard([&]() {
            m_state.labels_in_scope = move(old_labels_in_scope);
        });

        TemporaryChange change(m_state.in_arrow_function_context, true);
        TemporaryChange async_context_change(m_state.await_expression_is_valid, is_async);
        TemporaryChange in_class_static_init_block_change(m_state.in_class_static_init_block, false);

        if (match(TokenType::CurlyOpen)) {
            // Parse a function body with statements
            consume(TokenType::CurlyOpen);
            auto body = parse_function_body(parameters, function_kind, parsing_insights);
            consume(TokenType::CurlyClose);
            return body;
        }

        if (match_expression()) {
            // Parse a function body which returns a single expression

            // FIXME: We synthesize a block with a return statement
            // for arrow function bodies which are a single expression.
            // Esprima generates a single "ArrowFunctionExpression"
            // with a "body" property.
            auto return_block = create_ast_node<FunctionBody>({ m_source_code, rule_start.position(), position() });
            VERIFY(m_state.current_scope_pusher->type() == ScopePusher::ScopeType::Function);
            m_state.current_scope_pusher->set_scope_node(return_block);
            m_state.current_scope_pusher->set_function_parameters(*parameters);
            auto return_expression = parse_expression(2);
            return_block->append<ReturnStatement const>({ m_source_code, rule_start.position(), position() }, move(return_expression));
            if (m_state.strict_mode)
                const_cast<FunctionBody&>(*return_block).set_strict_mode();
            parsing_insights.contains_direct_call_to_eval = m_state.current_scope_pusher->contains_direct_call_to_eval();
            parsing_insights.uses_this_from_environment = m_state.current_scope_pusher->uses_this_from_environment();
            return return_block;
        }
        // Invalid arrow function body
        return nullptr;
    }();

    if (function_body_result.is_null())
        return nullptr;

    auto local_variables_names = function_body_result->local_variables_names();

    state_rollback_guard.disarm();
    discard_saved_state();
    auto body = function_body_result.release_nonnull();

    if (body->in_strict_mode()) {
        for (auto& parameter : parameters->parameters()) {
            parameter.binding.visit(
                [&](Identifier const& identifier) {
                    check_identifier_name_for_assignment_validity(identifier.string(), true);
                },
                [&](auto const&) {});
        }
    }

    auto function_start_offset = rule_start.position().offset;
    auto function_end_offset = position().offset - m_state.current_token.trivia().length();
    auto source_text = ByteString { m_state.lexer.source().substring_view(function_start_offset, function_end_offset - function_start_offset) };
    return create_ast_node<FunctionExpression>(
        { m_source_code, rule_start.position(), position() }, nullptr, move(source_text),
        move(body), move(parameters), function_length, function_kind, body->in_strict_mode(),
        parsing_insights, move(local_variables_names), /* is_arrow_function */ true);
}

RefPtr<LabelledStatement const> Parser::try_parse_labelled_statement(AllowLabelledFunction allow_function)
{
    {
        // NOTE: This is a fast path where we try to fail early to avoid the expensive save_state+load_state.
        if (next_token().type() != TokenType::Colon)
            return {};
    }

    save_state();
    auto rule_start = push_start();
    ArmedScopeGuard state_rollback_guard = [&] {
        load_state();
    };

    if (m_state.current_token.value() == "yield"sv && (m_state.strict_mode || m_state.in_generator_function_context)) {
        return {};
    }

    if (m_state.current_token.value() == "await"sv && (m_program_type == Program::Type::Module || m_state.await_expression_is_valid || m_state.in_class_static_init_block)) {
        return {};
    }

    auto identifier = [&] {
        if (m_state.current_token.value() == "await"sv) {
            return consume().fly_string_value();
        }
        return consume_identifier_reference().fly_string_value();
    }();
    if (!match(TokenType::Colon))
        return {};
    consume(TokenType::Colon);

    if (!match_statement())
        return {};

    state_rollback_guard.disarm();
    discard_saved_state();

    if (m_state.strict_mode && identifier == "let"sv) {
        syntax_error("Strict mode reserved word 'let' is not allowed in label"_string, rule_start.position());
        return {};
    }

    if (match(TokenType::Function) && (allow_function == AllowLabelledFunction::No || m_state.strict_mode)) {
        syntax_error("Not allowed to declare a function here"_string);
        return {};
    }

    if (m_state.labels_in_scope.contains(identifier))
        syntax_error(MUST(String::formatted("Label '{}' has already been declared", identifier)));

    RefPtr<Statement const> labelled_item;

    auto is_iteration_statement = false;

    if (match(TokenType::Function)) {
        m_state.labels_in_scope.set(identifier, {});
        auto function_declaration = parse_function_node<FunctionDeclaration>();
        VERIFY(m_state.current_scope_pusher);
        m_state.current_scope_pusher->add_declaration(function_declaration);
        if (function_declaration->kind() == FunctionKind::Generator)
            syntax_error("Generator functions cannot be defined in labelled statements"_string);
        if (function_declaration->kind() == FunctionKind::Async)
            syntax_error("Async functions cannot be defined in labelled statements"_string);

        labelled_item = move(function_declaration);
    } else {
        m_state.labels_in_scope.set(identifier, {});
        labelled_item = parse_statement(allow_function);
        // Extract the innermost statement from a potentially nested chain of LabelledStatements.
        auto statement = labelled_item;
        while (is<LabelledStatement>(*statement))
            statement = static_cast<LabelledStatement const&>(*statement).labelled_item();
        if (is<IterationStatement>(*statement))
            is_iteration_statement = true;
    }

    if (!is_iteration_statement) {
        if (auto entry = m_state.labels_in_scope.find(identifier); entry != m_state.labels_in_scope.end() && entry->value.has_value())
            syntax_error("labelled continue statement cannot use non iterating statement"_string, m_state.labels_in_scope.get(identifier).value());
    }

    m_state.labels_in_scope.remove(identifier);

    return create_ast_node<LabelledStatement>({ m_source_code, rule_start.position(), position() }, identifier, labelled_item.release_nonnull());
}

RefPtr<MetaProperty const> Parser::try_parse_new_target_expression()
{
    // Optimization which skips the save/load state.
    if (next_token().type() != TokenType::Period)
        return {};

    save_state();
    auto rule_start = push_start();
    ArmedScopeGuard state_rollback_guard = [&] {
        load_state();
    };

    consume(TokenType::New);
    consume(TokenType::Period);
    if (!match(TokenType::Identifier))
        return {};
    // The string 'target' cannot have escapes so we check original value.
    if (consume().original_value() != "target"sv)
        return {};

    state_rollback_guard.disarm();
    discard_saved_state();

    if (m_state.current_scope_pusher)
        m_state.current_scope_pusher->set_uses_new_target();

    return create_ast_node<MetaProperty>({ m_source_code, rule_start.position(), position() }, MetaProperty::Type::NewTarget);
}

RefPtr<MetaProperty const> Parser::try_parse_import_meta_expression()
{
    // Optimization which skips the save/load state.
    if (next_token().type() != TokenType::Period)
        return {};

    save_state();
    auto rule_start = push_start();
    ArmedScopeGuard state_rollback_guard = [&] {
        load_state();
    };

    consume(TokenType::Import);
    consume(TokenType::Period);
    if (!match(TokenType::Identifier))
        return {};
    // The string 'meta' cannot have escapes so we check original value.
    if (consume().original_value() != "meta"sv)
        return {};

    state_rollback_guard.disarm();
    discard_saved_state();
    return create_ast_node<MetaProperty>({ m_source_code, rule_start.position(), position() }, MetaProperty::Type::ImportMeta);
}

NonnullRefPtr<ImportCall const> Parser::parse_import_call()
{
    auto rule_start = push_start();

    // We use the extended definition:
    //  ImportCall[Yield, Await]:
    //      import(AssignmentExpression[+In, ?Yield, ?Await] ,opt)
    //      import(AssignmentExpression[+In, ?Yield, ?Await] ,AssignmentExpression[+In, ?Yield, ?Await] ,opt)
    // From https://tc39.es/proposal-import-attributes/#sec-evaluate-import-call

    consume(TokenType::Import);
    consume(TokenType::ParenOpen);
    auto argument = parse_expression(2);

    RefPtr<Expression const> options;
    if (match(TokenType::Comma)) {
        consume(TokenType::Comma);

        if (!match(TokenType::ParenClose)) {
            options = parse_expression(2);

            // Second optional comma
            if (match(TokenType::Comma))
                consume(TokenType::Comma);
        }
    }

    consume(TokenType::ParenClose);

    return create_ast_node<ImportCall>({ m_source_code, rule_start.position(), position() }, move(argument), move(options));
}

NonnullRefPtr<ClassDeclaration const> Parser::parse_class_declaration()
{
    auto rule_start = push_start();
    return create_ast_node<ClassDeclaration>({ m_source_code, rule_start.position(), position() }, parse_class_expression(true));
}

NonnullRefPtr<ClassExpression const> Parser::parse_class_expression(bool expect_class_name)
{
    auto rule_start = push_start();
    // Classes are always in strict mode.
    TemporaryChange strict_mode_rollback(m_state.strict_mode, true);

    consume(TokenType::Class);

    Vector<NonnullRefPtr<ClassElement const>> elements;
    RefPtr<Expression const> super_class;
    RefPtr<FunctionExpression const> constructor;
    HashTable<FlyString> found_private_names;

    RefPtr<Identifier const> class_name;
    if (expect_class_name || match_identifier() || match(TokenType::Yield) || match(TokenType::Await)) {
        class_name = create_identifier_and_register_in_current_scope({ m_source_code, rule_start.position(), position() }, consume_identifier_reference().fly_string_value());
    }

    ScopePusher class_declaration_scope = ScopePusher::class_declaration_scope(*this, class_name);

    if (class_name)
        check_identifier_name_for_assignment_validity(class_name->string(), true);
    if (m_state.in_class_static_init_block && class_name && class_name->string() == "await"sv)
        syntax_error("Identifier must not be a reserved word in modules ('await')"_string);

    if (match(TokenType::Extends)) {
        consume();
        auto [expression, should_continue_parsing] = parse_primary_expression();

        // Basically a (much) simplified parse_secondary_expression().
        for (;;) {
            if (match(TokenType::TemplateLiteralStart)) {
                auto template_literal = parse_template_literal(true);
                expression = create_ast_node<TaggedTemplateLiteral>({ m_source_code, rule_start.position(), position() }, move(expression), move(template_literal));
                continue;
            }
            if (match(TokenType::BracketOpen) || match(TokenType::Period) || match(TokenType::ParenOpen)) {
                auto precedence = g_operator_precedence.get(m_state.current_token.type());
                expression = parse_secondary_expression(move(expression), precedence).expression;
                continue;
            }
            break;
        }

        super_class = move(expression);
        (void)should_continue_parsing;
    }

    consume(TokenType::CurlyOpen);

    HashTable<FlyString> referenced_private_names;
    HashTable<FlyString>* outer_referenced_private_names = m_state.referenced_private_names;
    m_state.referenced_private_names = &referenced_private_names;
    ScopeGuard restore_private_name_table = [&] {
        m_state.referenced_private_names = outer_referenced_private_names;
    };

    while (!done() && !match(TokenType::CurlyClose)) {
        RefPtr<Expression const> property_key;
        bool is_static = false;
        bool is_constructor = false;
        bool is_generator = false;
        bool is_async = false;
        auto method_kind = ClassMethod::Kind::Method;

        if (match(TokenType::Semicolon)) {
            consume();
            continue;
        }

        auto function_start = position();

        if (match(TokenType::Async)) {
            auto lookahead_token = next_token();
            // If async is followed by a Semicolon or CurlyClose it is a field (CurlyClose indicates end of class)
            // Otherwise if it is followed by a ParenOpen it is a function named async
            if (lookahead_token.type() != TokenType::Semicolon && lookahead_token.type() != TokenType::CurlyClose && lookahead_token.type() != TokenType::ParenOpen
                && !lookahead_token.trivia_contains_line_terminator()) {
                consume();
                is_async = true;
            }
        }

        if (match(TokenType::Asterisk)) {
            consume();
            is_generator = true;
        }

        FlyString name;
        if (match_property_key() || match(TokenType::PrivateIdentifier)) {
            if (!is_generator && !is_async && m_state.current_token.original_value() == "static"sv) {
                if (match(TokenType::Identifier)) {
                    consume();
                    is_static = true;
                    function_start = position();
                    if (match(TokenType::Async)) {
                        consume();
                        is_async = true;
                    }
                    if (match(TokenType::Asterisk)) {
                        consume();
                        is_generator = true;
                    }
                }
            }

            if (match(TokenType::Identifier)) {
                auto identifier_name = m_state.current_token.original_value();

                if (identifier_name == "get"sv) {
                    method_kind = ClassMethod::Kind::Getter;
                    consume();
                } else if (identifier_name == "set"sv) {
                    method_kind = ClassMethod::Kind::Setter;
                    consume();
                }
            }

            if (match_property_key() || match(TokenType::PrivateIdentifier)) {
                switch (m_state.current_token.type()) {
                case TokenType::Identifier:
                    name = consume().fly_string_value();
                    property_key = create_ast_node<StringLiteral>({ m_source_code, rule_start.position(), position() }, name.to_string());
                    break;
                case TokenType::PrivateIdentifier:
                    name = consume().fly_string_value();
                    if (name == "#constructor")
                        syntax_error("Private property with name '#constructor' is not allowed"_string);

                    if (method_kind != ClassMethod::Kind::Method) {
                        // It is a Syntax Error if PrivateBoundIdentifiers of ClassElementList contains any duplicate entries,
                        //   unless the name is used once for a getter and once for a setter and in no other entries,
                        //   and the getter and setter are either both static or both non-static.

                        for (auto& element : elements) {
                            auto private_name = element->private_bound_identifier();
                            if (!private_name.has_value() || private_name.value() != name)
                                continue;

                            if (element->class_element_kind() != ClassElement::ElementKind::Method
                                || element->is_static() != is_static) {
                                syntax_error(MUST(String::formatted("Duplicate private field or method named '{}'", name)));
                                break;
                            }

                            VERIFY(is<ClassMethod>(*element));
                            auto& class_method_element = static_cast<ClassMethod const&>(*element);

                            if (class_method_element.kind() == ClassMethod::Kind::Method || class_method_element.kind() == method_kind) {
                                syntax_error(MUST(String::formatted("Duplicate private field or method named '{}'", name)));
                                break;
                            }
                        }

                        found_private_names.set(name);
                    } else if (found_private_names.set(name) != AK::HashSetResult::InsertedNewEntry) {
                        syntax_error(MUST(String::formatted("Duplicate private field or method named '{}'", name)));
                    }

                    property_key = create_ast_node<PrivateIdentifier>({ m_source_code, rule_start.position(), position() }, name);
                    break;
                case TokenType::StringLiteral: {
                    auto string_literal = parse_string_literal(consume());
                    name = string_literal->value();
                    property_key = move(string_literal);
                    break;
                }
                default:
                    property_key = parse_property_key();
                    break;
                }

                // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
                // ClassElement : static MethodDefinition
                //   It is a Syntax Error if PropName of MethodDefinition is "prototype".
                if (is_static && name == "prototype"sv)
                    syntax_error("Classes may not have a static property named 'prototype'"_string);
            } else if ((match(TokenType::ParenOpen) || match(TokenType::Equals) || match(TokenType::Semicolon) || match(TokenType::CurlyClose)) && (is_static || is_async || method_kind != ClassMethod::Kind::Method)) {
                switch (method_kind) {
                case ClassMethod::Kind::Method:
                    if (is_async) {
                        name = "async"_fly_string;
                        is_async = false;
                    } else {
                        VERIFY(is_static);
                        name = "static"_fly_string;
                        is_static = false;
                    }
                    break;
                case ClassMethod::Kind::Getter:
                    name = "get"_fly_string;
                    method_kind = ClassMethod::Kind::Method;
                    break;
                case ClassMethod::Kind::Setter:
                    name = "set"_fly_string;
                    method_kind = ClassMethod::Kind::Method;
                    break;
                }
                property_key = create_ast_node<StringLiteral>({ m_source_code, rule_start.position(), position() }, name.to_string());
            } else if (match(TokenType::CurlyOpen) && is_static) {
                auto static_start = push_start();
                consume(TokenType::CurlyOpen);

                auto static_init_block = create_ast_node<FunctionBody>({ m_source_code, rule_start.position(), position() });

                TemporaryChange break_context_rollback(m_state.in_break_context, false);
                TemporaryChange continue_context_rollback(m_state.in_continue_context, false);
                TemporaryChange function_context_rollback(m_state.in_function_context, false);
                TemporaryChange generator_function_context_rollback(m_state.in_generator_function_context, false);
                TemporaryChange async_function_context_rollback(m_state.await_expression_is_valid, false);
                TemporaryChange class_field_initializer_rollback(m_state.in_class_field_initializer, true);
                TemporaryChange class_static_init_block_rollback(m_state.in_class_static_init_block, true);
                TemporaryChange super_property_access_rollback(m_state.allow_super_property_lookup, true);

                ScopePusher static_init_scope = ScopePusher::static_init_block_scope(*this, *static_init_block);
                parse_statement_list(static_init_block);

                consume(TokenType::CurlyClose);
                elements.append(create_ast_node<StaticInitializer>({ m_source_code, static_start.position(), position() }, move(static_init_block), static_init_scope.contains_direct_call_to_eval()));
                continue;
            } else {
                expected("property key");
            }

            // Constructor may be a StringLiteral or an Identifier.
            if (!is_static && name == "constructor"sv) {
                if (method_kind != ClassMethod::Kind::Method)
                    syntax_error("Class constructor may not be an accessor"_string);
                if (!constructor.is_null())
                    syntax_error("Classes may not have more than one constructor"_string);
                if (is_generator)
                    syntax_error("Class constructor may not be a generator"_string);
                if (is_async)
                    syntax_error("Class constructor may not be async"_string);

                is_constructor = true;
            }
        }

        if (match(TokenType::ParenOpen)) {
            u8 parse_options = FunctionNodeParseOptions::AllowSuperPropertyLookup;
            if (!super_class.is_null() && !is_static && is_constructor)
                parse_options |= FunctionNodeParseOptions::AllowSuperConstructorCall;
            if (method_kind == ClassMethod::Kind::Getter)
                parse_options |= FunctionNodeParseOptions::IsGetterFunction;
            if (method_kind == ClassMethod::Kind::Setter)
                parse_options |= FunctionNodeParseOptions::IsSetterFunction;
            if (is_generator)
                parse_options |= FunctionNodeParseOptions::IsGeneratorFunction;
            if (is_async)
                parse_options |= FunctionNodeParseOptions::IsAsyncFunction;
            auto function = parse_function_node<FunctionExpression>(parse_options, function_start);
            if (is_constructor) {
                constructor = move(function);
            } else if (!property_key.is_null()) {
                elements.append(create_ast_node<ClassMethod>({ m_source_code, rule_start.position(), position() }, property_key.release_nonnull(), move(function), method_kind, is_static));
            } else {
                syntax_error("No key for class method"_string);
            }
        } else if (is_generator || is_async) {
            expected("ParenOpen");
            consume();
        } else if (property_key.is_null()) {
            expected("property key");
            consume();
        } else {
            if (name == "constructor"sv)
                syntax_error("Class cannot have field named 'constructor'"_string);

            RefPtr<Expression const> initializer;
            bool contains_direct_call_to_eval = false;

            if (match(TokenType::Equals)) {
                consume();

                TemporaryChange super_property_access_rollback(m_state.allow_super_property_lookup, true);
                TemporaryChange field_initializer_rollback(m_state.in_class_field_initializer, true);

                auto class_scope_node = create_ast_node<BlockStatement>({ m_source_code, rule_start.position(), position() });
                auto class_field_scope = ScopePusher::class_field_scope(*this, *class_scope_node);
                initializer = parse_expression(2);
                contains_direct_call_to_eval = class_field_scope.contains_direct_call_to_eval();
            }

            elements.append(create_ast_node<ClassField>({ m_source_code, rule_start.position(), position() }, property_key.release_nonnull(), move(initializer), contains_direct_call_to_eval, is_static));
            consume_or_insert_semicolon();
        }
    }

    consume(TokenType::CurlyClose);

    if (constructor.is_null()) {
        auto constructor_body = create_ast_node<BlockStatement>({ m_source_code, rule_start.position(), position() });
        if (!super_class.is_null()) {
            // Set constructor to the result of parsing the source text
            // constructor(... args){ super (...args);}
            // However: The most notable distinction is that while the aforementioned ECMAScript
            //          source text observably calls the @@iterator method on %Array.prototype%,
            //          this function does not.
            //          So we use a custom version of SuperCall which doesn't use the @@iterator
            //          method on %Array.prototype% visibly.
            auto argument_name = create_ast_node<Identifier const>({ m_source_code, rule_start.position(), position() }, "args"_fly_string);
            auto super_call = create_ast_node<SuperCall>(
                { m_source_code, rule_start.position(), position() },
                SuperCall::IsPartOfSyntheticConstructor::Yes,
                CallExpression::Argument { create_ast_node<Identifier>({ m_source_code, rule_start.position(), position() }, "args"_fly_string), true });
            // NOTE: While the JS approximation above doesn't do `return super(...args)`, the
            // abstract closure is expected to capture and return the result, so we do need a
            // return statement here to create the correct completion.
            constructor_body->append(create_ast_node<ReturnStatement>({ m_source_code, rule_start.position(), position() }, move(super_call)));

            FunctionParsingInsights parsing_insights;
            constructor = create_ast_node<FunctionExpression>(
                { m_source_code, rule_start.position(), position() }, class_name, "",
                move(constructor_body), FunctionParameters::create(Vector { FunctionParameter { move(argument_name), nullptr, true } }), 0, FunctionKind::Normal,
                /* is_strict_mode */ true, parsing_insights, /* local_variables_names */ Vector<FlyString> {});
        } else {
            FunctionParsingInsights parsing_insights;
            constructor = create_ast_node<FunctionExpression>(
                { m_source_code, rule_start.position(), position() }, class_name, "",
                move(constructor_body), FunctionParameters::empty(), 0, FunctionKind::Normal,
                /* is_strict_mode */ true, parsing_insights, /* local_variables_names */ Vector<FlyString> {});
        }
    }

    // We could be in a subclass defined within the main class so must move all non declared private names to outer.
    for (auto& private_name : referenced_private_names) {
        if (found_private_names.contains(private_name))
            continue;
        if (outer_referenced_private_names)
            outer_referenced_private_names->set(private_name);
        else { // FIXME: Make these error appear in the appropriate places.
            syntax_error(MUST(String::formatted("Reference to undeclared private field or method '{}'", private_name)));
        }
    }

    auto function_start_offset = rule_start.position().offset;
    auto function_end_offset = position().offset - m_state.current_token.trivia().length();
    auto source_text = ByteString { m_state.lexer.source().substring_view(function_start_offset, function_end_offset - function_start_offset) };

    return create_ast_node<ClassExpression>({ m_source_code, rule_start.position(), position() }, move(class_name), move(source_text), move(constructor), move(super_class), move(elements));
}

Parser::PrimaryExpressionParseResult Parser::parse_primary_expression()
{
    auto rule_start = push_start();
    if (match_unary_prefixed_expression())
        return { parse_unary_prefixed_expression() };

    auto try_arrow_function_parse_or_fail = [this](Position const& position, bool expect_paren, bool is_async = false) -> RefPtr<FunctionExpression const> {
        if (try_parse_arrow_function_expression_failed_at_position(position))
            return nullptr;
        auto arrow_function = try_parse_arrow_function_expression(expect_paren, is_async);
        if (arrow_function)
            return arrow_function;

        set_try_parse_arrow_function_expression_failed_at_position(position, true);
        return nullptr;
    };

    switch (m_state.current_token.type()) {
    case TokenType::ParenOpen: {
        auto paren_position = position();
        consume(TokenType::ParenOpen);
        if ((match(TokenType::ParenClose) || match_identifier() || match(TokenType::TripleDot) || match(TokenType::CurlyOpen) || match(TokenType::BracketOpen))) {
            if (auto arrow_function_result = try_arrow_function_parse_or_fail(paren_position, true))
                return { arrow_function_result.release_nonnull(), false };
        }
        auto expression = parse_expression(0);
        consume(TokenType::ParenClose);
        if (is<NewExpression>(*expression)) {
            auto& new_expression = static_cast<NewExpression&>(*static_cast<NonnullRefPtr<Expression>>(expression));
            new_expression.set_inside_parens();
        } else if (is<FunctionExpression>(*expression)) {
            auto& function = static_cast<FunctionExpression const&>(*expression);
            if (function.kind() == FunctionKind::Generator && function.name() == "yield"sv)
                syntax_error("function is not allowed to be called 'yield' in this context"_string, function.source_range().start);
            if (function.kind() == FunctionKind::Async && function.name() == "await"sv)
                syntax_error("function is not allowed to be called 'await' in this context"_string, function.source_range().start);
        }
        return { move(expression) };
    }
    case TokenType::This: {
        if (m_state.current_scope_pusher)
            m_state.current_scope_pusher->set_uses_this();
        consume_and_allow_division();
        return { create_ast_node<ThisExpression>({ m_source_code, rule_start.position(), position() }) };
    }
    case TokenType::Class:
        return { parse_class_expression(false) };
    case TokenType::Super:
        consume();
        if (!m_state.allow_super_property_lookup)
            syntax_error("'super' keyword unexpected here"_string);
        if (m_state.current_scope_pusher)
            m_state.current_scope_pusher->set_uses_new_target();
        return { create_ast_node<SuperExpression>({ m_source_code, rule_start.position(), position() }) };
    case TokenType::EscapedKeyword:
        if (match_invalid_escaped_keyword())
            syntax_error("Keyword must not contain escaped characters"_string);
        [[fallthrough]];
    case TokenType::Identifier: {
    read_as_identifier:;
        if (auto arrow_function_result = try_arrow_function_parse_or_fail(position(), false))
            return { arrow_function_result.release_nonnull(), false };

        auto string = m_state.current_token.value();
        // This could be 'eval' or 'arguments' and thus needs a custom check (`eval[1] = true`)
        if (m_state.strict_mode && (string == "let" || is_strict_reserved_word(string)))
            syntax_error(MUST(String::formatted("Identifier must not be a reserved word in strict mode ('{}')", string)));
        return { parse_identifier() };
    }
    case TokenType::NumericLiteral:
        return { create_ast_node<NumericLiteral>({ m_source_code, rule_start.position(), position() }, consume_and_validate_numeric_literal().double_value()) };
    case TokenType::BigIntLiteral:
        return { create_ast_node<BigIntLiteral>({ m_source_code, rule_start.position(), position() }, consume().value()) };
    case TokenType::BoolLiteral:
        return { create_ast_node<BooleanLiteral>({ m_source_code, rule_start.position(), position() }, consume_and_allow_division().bool_value()) };
    case TokenType::StringLiteral:
        return { parse_string_literal(consume()) };
    case TokenType::NullLiteral:
        consume_and_allow_division();
        return { create_ast_node<NullLiteral>({ m_source_code, rule_start.position(), position() }) };
    case TokenType::CurlyOpen:
        return { parse_object_expression() };
    case TokenType::Async: {
        auto lookahead_token = next_token();
        // No valid async function (arrow or not) can have a line terminator after the async since asi would kick in.
        if (lookahead_token.trivia_contains_line_terminator())
            goto read_as_identifier;

        if (lookahead_token.type() == TokenType::Function)
            return { parse_function_node<FunctionExpression>() };

        if (lookahead_token.type() == TokenType::ParenOpen) {
            if (auto arrow_function_result = try_arrow_function_parse_or_fail(position(), true, true))
                return { arrow_function_result.release_nonnull(), false };
        } else if (lookahead_token.is_identifier_name()) {
            if (auto arrow_function_result = try_arrow_function_parse_or_fail(position(), false, true))
                return { arrow_function_result.release_nonnull(), false };
        }
        goto read_as_identifier;
    }
    case TokenType::Function:
        return { parse_function_node<FunctionExpression>() };
    case TokenType::BracketOpen:
        return { parse_array_expression() };
    case TokenType::RegexLiteral:
        return { parse_regexp_literal() };
    case TokenType::TemplateLiteralStart:
        return { parse_template_literal(false) };
    case TokenType::New: {
        auto new_start = position();
        auto new_target_result = try_parse_new_target_expression();
        if (!new_target_result.is_null()) {
            if (!m_state.in_function_context && !m_state.in_eval_function_context && !m_state.in_class_static_init_block)
                syntax_error("'new.target' not allowed outside of a function"_string, new_start);
            return { new_target_result.release_nonnull() };
        }
        return { parse_new_expression() };
    }
    case TokenType::Import: {
        auto lookahead_token = next_token();
        if (lookahead_token.type() == TokenType::ParenOpen)
            return { parse_import_call() };

        if (lookahead_token.type() == TokenType::Period) {
            if (auto import_meta = try_parse_import_meta_expression()) {
                if (m_program_type != Program::Type::Module)
                    syntax_error("import.meta is only allowed in modules"_string);
                return { import_meta.release_nonnull() };
            }
        } else {
            consume();
            expected("import.meta or import call");
        }
        break;
    }
    case TokenType::Yield:
        if (!m_state.in_generator_function_context)
            goto read_as_identifier;
        return { parse_yield_expression(), false };
    case TokenType::Await:
        if (!m_state.await_expression_is_valid)
            goto read_as_identifier;
        return { parse_await_expression() };
    case TokenType::PrivateIdentifier:
        if (!is_private_identifier_valid())
            syntax_error(MUST(String::formatted("Reference to undeclared private field or method '{}'", m_state.current_token.value())));
        if (next_token().type() != TokenType::In)
            syntax_error("Cannot have a private identifier in expression if not followed by 'in'"_string);
        return { create_ast_node<PrivateIdentifier>({ m_source_code, rule_start.position(), position() }, consume().fly_string_value()) };
    default:
        if (match_identifier_name())
            goto read_as_identifier;
        break;
    }
    expected("primary expression");
    consume();
    return { create_ast_node<ErrorExpression>({ m_source_code, rule_start.position(), position() }) };
}

NonnullRefPtr<RegExpLiteral const> Parser::parse_regexp_literal()
{
    auto rule_start = push_start();
    auto pattern = consume().fly_string_value().to_string();
    // Remove leading and trailing slash.
    pattern = MUST(pattern.substring_from_byte_offset(1, pattern.bytes().size() - 2));

    auto flags = String {};
    auto parsed_flags = RegExpObject::default_flags;

    if (match(TokenType::RegexFlags)) {
        auto flags_start = position();
        flags = consume().fly_string_value().to_string();

        auto parsed_flags_or_error = regex_flags_from_string(flags);
        if (parsed_flags_or_error.is_error())
            syntax_error(parsed_flags_or_error.release_error(), flags_start);
        else
            parsed_flags = parsed_flags_or_error.release_value();
    }

    String parsed_pattern;
    auto parsed_pattern_result = parse_regex_pattern(pattern, parsed_flags.has_flag_set(ECMAScriptFlags::Unicode), parsed_flags.has_flag_set(ECMAScriptFlags::UnicodeSets));
    if (parsed_pattern_result.is_error()) {
        syntax_error(parsed_pattern_result.release_error().error, rule_start.position());
        parsed_pattern = ""_string;
    } else {
        parsed_pattern = parsed_pattern_result.release_value();
    }
    auto parsed_regex = Regex<ECMA262>::parse_pattern(parsed_pattern, parsed_flags);

    if (parsed_regex.error != regex::Error::NoError)
        syntax_error(MUST(String::formatted("RegExp compile error: {}", Regex<ECMA262>(parsed_regex, parsed_pattern.to_byte_string(), parsed_flags).error_string())), rule_start.position());

    SourceRange range { m_source_code, rule_start.position(), position() };
    return create_ast_node<RegExpLiteral>(move(range), move(parsed_regex), move(parsed_pattern), move(parsed_flags), move(pattern), move(flags));
}

static bool is_simple_assignment_target(Expression const& expression, bool allow_web_reality_call_expression = true)
{
    return is<Identifier>(expression) || is<MemberExpression>(expression) || (allow_web_reality_call_expression && is<CallExpression>(expression));
}

NonnullRefPtr<Expression const> Parser::parse_unary_prefixed_expression()
{
    auto rule_start = push_start();
    auto precedence = g_operator_precedence.get_unary(m_state.current_token.type());
    auto associativity = operator_associativity(m_state.current_token.type());

    auto verify_next_token_is_not_exponentiation = [this]() {
        auto lookahead_token = next_token();
        if (lookahead_token.type() == TokenType::DoubleAsterisk)
            syntax_error("Unary operator must not be used before exponentiation expression without brackets"_string);
    };

    switch (m_state.current_token.type()) {
    case TokenType::PlusPlus: {
        consume();
        auto rhs_start = position();
        auto rhs = parse_expression(precedence, associativity);
        if (!is_simple_assignment_target(*rhs))
            syntax_error(MUST(String::formatted("Right-hand side of prefix increment operator must be identifier or member expression, got {}", rhs->class_name())), rhs_start);

        if (m_state.strict_mode && is<Identifier>(*rhs)) {
            auto& identifier = static_cast<Identifier const&>(*rhs);
            auto& name = identifier.string();
            check_identifier_name_for_assignment_validity(name);
        }

        return create_ast_node<UpdateExpression>({ m_source_code, rule_start.position(), position() }, UpdateOp::Increment, move(rhs), true);
    }
    case TokenType::MinusMinus: {
        consume();
        auto rhs_start = position();
        auto rhs = parse_expression(precedence, associativity);
        if (!is_simple_assignment_target(*rhs))
            syntax_error(MUST(String::formatted("Right-hand side of prefix decrement operator must be identifier or member expression, got {}", rhs->class_name())), rhs_start);

        if (m_state.strict_mode && is<Identifier>(*rhs)) {
            auto& identifier = static_cast<Identifier const&>(*rhs);
            auto& name = identifier.string();
            check_identifier_name_for_assignment_validity(name);
        }

        return create_ast_node<UpdateExpression>({ m_source_code, rule_start.position(), position() }, UpdateOp::Decrement, move(rhs), true);
    }
    case TokenType::ExclamationMark:
        consume();
        verify_next_token_is_not_exponentiation();
        return create_ast_node<UnaryExpression>({ m_source_code, rule_start.position(), position() }, UnaryOp::Not, parse_expression(precedence, associativity));
    case TokenType::Tilde:
        consume();
        verify_next_token_is_not_exponentiation();
        return create_ast_node<UnaryExpression>({ m_source_code, rule_start.position(), position() }, UnaryOp::BitwiseNot, parse_expression(precedence, associativity));
    case TokenType::Plus:
        consume();
        verify_next_token_is_not_exponentiation();
        return create_ast_node<UnaryExpression>({ m_source_code, rule_start.position(), position() }, UnaryOp::Plus, parse_expression(precedence, associativity));
    case TokenType::Minus:
        consume();
        verify_next_token_is_not_exponentiation();
        return create_ast_node<UnaryExpression>({ m_source_code, rule_start.position(), position() }, UnaryOp::Minus, parse_expression(precedence, associativity));
    case TokenType::Typeof:
        consume();
        verify_next_token_is_not_exponentiation();
        return create_ast_node<UnaryExpression>({ m_source_code, rule_start.position(), position() }, UnaryOp::Typeof, parse_expression(precedence, associativity));
    case TokenType::Void:
        consume();
        verify_next_token_is_not_exponentiation();
        // FIXME: This check is really hiding the fact that we don't deal with different expressions correctly.
        if (match(TokenType::Yield) && m_state.in_generator_function_context)
            syntax_error("'yield' is not an identifier in generator function context"_string);
        return create_ast_node<UnaryExpression>({ m_source_code, rule_start.position(), position() }, UnaryOp::Void, parse_expression(precedence, associativity));
    case TokenType::Delete: {
        consume();
        verify_next_token_is_not_exponentiation();
        auto rhs_start = position();
        auto rhs = parse_expression(precedence, associativity);
        if (is<Identifier>(*rhs) && m_state.strict_mode) {
            syntax_error("Delete of an unqualified identifier in strict mode."_string, rhs_start);
        }
        if (is<MemberExpression>(*rhs)) {
            auto& member_expression = static_cast<MemberExpression const&>(*rhs);
            if (member_expression.ends_in_private_name())
                syntax_error("Private fields cannot be deleted"_string);
        }
        return create_ast_node<UnaryExpression>({ m_source_code, rule_start.position(), position() }, UnaryOp::Delete, move(rhs));
    }
    default:
        expected("primary expression");
        consume();
        return create_ast_node<ErrorExpression>({ m_source_code, rule_start.position(), position() });
    }
}

NonnullRefPtr<Expression const> Parser::parse_property_key()
{
    auto rule_start = push_start();
    if (match(TokenType::StringLiteral)) {
        return parse_string_literal(consume());
    } else if (match(TokenType::NumericLiteral)) {
        return create_ast_node<NumericLiteral>({ m_source_code, rule_start.position(), position() }, consume().double_value());
    } else if (match(TokenType::BigIntLiteral)) {
        return create_ast_node<BigIntLiteral>({ m_source_code, rule_start.position(), position() }, consume().value());
    } else if (match(TokenType::BracketOpen)) {
        consume(TokenType::BracketOpen);
        auto result = parse_expression(2);
        consume(TokenType::BracketClose);
        return result;
    } else {
        if (!match_identifier_name())
            expected("IdentifierName");
        return create_ast_node<StringLiteral>({ m_source_code, rule_start.position(), position() }, consume().fly_string_value().to_string());
    }
}

NonnullRefPtr<ObjectExpression const> Parser::parse_object_expression()
{
    auto rule_start = push_start();
    consume(TokenType::CurlyOpen);

    Vector<NonnullRefPtr<ObjectProperty>> properties;
    ObjectProperty::Type property_type;
    Optional<SourceRange> invalid_object_literal_property_range;

    auto skip_to_next_property = [&] {
        while (!done() && !match(TokenType::Comma) && !match(TokenType::CurlyOpen))
            consume();
    };

    // It is a Syntax Error if PropertyNameList of PropertyDefinitionList contains any duplicate
    // entries for "__proto__" and at least two of those entries were obtained from productions  of
    // the form PropertyDefinition : PropertyKey : AssignmentExpression .
    bool has_direct_proto_property = false;

    while (!done() && !match(TokenType::CurlyClose)) {
        property_type = ObjectProperty::Type::KeyValue;
        RefPtr<Expression const> property_key;
        RefPtr<Expression const> property_value;
        FunctionKind function_kind { FunctionKind::Normal };

        if (match(TokenType::TripleDot)) {
            consume();
            property_key = parse_expression(2);
            properties.append(create_ast_node<ObjectProperty>({ m_source_code, rule_start.position(), position() }, *property_key, nullptr, ObjectProperty::Type::Spread, false));
            if (!match(TokenType::Comma))
                break;
            consume(TokenType::Comma);
            continue;
        }

        auto type = m_state.current_token.type();
        auto function_start = position();

        if (match(TokenType::Async)) {
            auto lookahead_token = next_token();

            if (lookahead_token.type() != TokenType::ParenOpen && lookahead_token.type() != TokenType::Colon
                && lookahead_token.type() != TokenType::Comma && lookahead_token.type() != TokenType::CurlyClose
                && !lookahead_token.trivia_contains_line_terminator()) {
                consume(TokenType::Async);
                function_kind = FunctionKind::Async;
            }
        }

        if (match(TokenType::Asterisk)) {
            consume();
            property_type = ObjectProperty::Type::KeyValue;
            property_key = parse_property_key();
            VERIFY(function_kind == FunctionKind::Normal || function_kind == FunctionKind::Async);
            function_kind = function_kind == FunctionKind::Normal ? FunctionKind::Generator : FunctionKind::AsyncGenerator;
        } else if (match_identifier()) {
            auto identifier = consume();
            if (identifier.original_value() == "get"sv && match_property_key()) {
                property_type = ObjectProperty::Type::Getter;
                property_key = parse_property_key();
            } else if (identifier.original_value() == "set"sv && match_property_key()) {
                property_type = ObjectProperty::Type::Setter;
                property_key = parse_property_key();
            } else {
                property_key = create_ast_node<StringLiteral>({ m_source_code, rule_start.position(), position() }, identifier.fly_string_value().to_string());
                property_value = create_identifier_and_register_in_current_scope({ m_source_code, rule_start.position(), position() }, identifier.fly_string_value());
            }
        } else {
            property_key = parse_property_key();
        }

        // 4. Else if propKey is the String value "__proto__" and if IsComputedPropertyKey of PropertyName is false, then
        // a. Let isProtoSetter be true.
        bool is_proto = (type == TokenType::StringLiteral || type == TokenType::Identifier) && is<StringLiteral>(*property_key) && static_cast<StringLiteral const&>(*property_key).value() == "__proto__";

        if (property_type == ObjectProperty::Type::Getter || property_type == ObjectProperty::Type::Setter) {
            if (!match(TokenType::ParenOpen)) {
                expected("'(' for object getter or setter property");
                skip_to_next_property();
                continue;
            }
        }

        if (match(TokenType::Equals)) {
            // Not a valid object literal, but a valid assignment target
            consume();
            // Parse the expression and throw it away
            auto expression = parse_expression(2);
            if (!invalid_object_literal_property_range.has_value())
                invalid_object_literal_property_range = expression->source_range();
        } else if (match(TokenType::ParenOpen)) {
            VERIFY(property_key);
            u8 parse_options = FunctionNodeParseOptions::AllowSuperPropertyLookup;
            if (property_type == ObjectProperty::Type::Getter)
                parse_options |= FunctionNodeParseOptions::IsGetterFunction;
            if (property_type == ObjectProperty::Type::Setter)
                parse_options |= FunctionNodeParseOptions::IsSetterFunction;
            if (function_kind == FunctionKind::Generator || function_kind == FunctionKind::AsyncGenerator)
                parse_options |= FunctionNodeParseOptions::IsGeneratorFunction;
            if (function_kind == FunctionKind::Async || function_kind == FunctionKind::AsyncGenerator)
                parse_options |= FunctionNodeParseOptions::IsAsyncFunction;
            auto function = parse_function_node<FunctionExpression>(parse_options, function_start);
            properties.append(create_ast_node<ObjectProperty>({ m_source_code, rule_start.position(), position() }, *property_key, function, property_type, true));
        } else if (function_kind == FunctionKind::Async) {
            // If we previously parsed an `async` keyword, then a function must follow.
            syntax_error("Expected function after async keyword"_string);
            skip_to_next_property();
            continue;
        } else if (match(TokenType::Colon)) {
            if (!property_key) {
                expected("a property name");
                skip_to_next_property();
                continue;
            }
            consume();
            if (is_proto) {
                if (has_direct_proto_property)
                    syntax_error("Property name '__proto__' must not appear more than once in object literal"_string);
                has_direct_proto_property = true;
            }
            if (is_proto && property_type == ObjectProperty::Type::KeyValue)
                property_type = ObjectProperty::Type::ProtoSetter;

            auto rhs_expression = parse_expression(2);
            bool is_method = is<FunctionExpression>(*rhs_expression);
            properties.append(create_ast_node<ObjectProperty>({ m_source_code, rule_start.position(), position() }, *property_key, move(rhs_expression), property_type, is_method));
        } else if (property_key && property_value) {
            if (m_state.strict_mode && is<StringLiteral>(*property_key)) {
                auto& string_literal = static_cast<StringLiteral const&>(*property_key);
                if (is_strict_reserved_word(string_literal.value()))
                    syntax_error(MUST(String::formatted("'{}' is a reserved keyword", string_literal.value())));
            }

            properties.append(create_ast_node<ObjectProperty>({ m_source_code, rule_start.position(), position() }, *property_key, *property_value, property_type, false));
        } else {
            expected("a property");
            skip_to_next_property();
            continue;
        }

        if (!match(TokenType::Comma))
            break;
        consume(TokenType::Comma);
    }

    consume(TokenType::CurlyClose);

    if (invalid_object_literal_property_range.has_value()) {
        size_t object_expression_offset = rule_start.position().offset;
        VERIFY(!m_state.invalid_property_range_in_object_expression.contains(object_expression_offset));
        m_state.invalid_property_range_in_object_expression.set(object_expression_offset, invalid_object_literal_property_range->start);
    }

    properties.shrink_to_fit();
    return create_ast_node<ObjectExpression>(
        { m_source_code, rule_start.position(), position() },
        move(properties));
}

NonnullRefPtr<ArrayExpression const> Parser::parse_array_expression()
{
    auto rule_start = push_start();
    consume(TokenType::BracketOpen);

    Vector<RefPtr<Expression const>> elements;
    while (match_expression() || match(TokenType::TripleDot) || match(TokenType::Comma)) {
        RefPtr<Expression const> expression;

        if (match(TokenType::TripleDot)) {
            consume(TokenType::TripleDot);
            expression = create_ast_node<SpreadExpression>({ m_source_code, rule_start.position(), position() }, parse_expression(2));
        } else if (match_expression()) {
            expression = parse_expression(2);
        }

        elements.append(expression);
        if (!match(TokenType::Comma))
            break;
        consume(TokenType::Comma);
    }

    consume(TokenType::BracketClose);

    elements.shrink_to_fit();
    return create_ast_node<ArrayExpression>({ m_source_code, rule_start.position(), position() }, move(elements));
}

NonnullRefPtr<StringLiteral const> Parser::parse_string_literal(Token const& token, StringLiteralType string_literal_type, bool* contains_invalid_escape)
{
    auto rule_start = push_start();
    auto status = Token::StringValueStatus::Ok;
    auto string = token.string_value(status);
    // NOTE: Tagged templates should not fail on invalid strings as their raw contents can still be accessed.
    if (status != Token::StringValueStatus::Ok) {
        String message;
        if (status == Token::StringValueStatus::LegacyOctalEscapeSequence) {
            m_state.string_legacy_octal_escape_sequence_in_scope = true;
            // It is a Syntax Error if the [Tagged] parameter was not set and Template{Head, Middle, Tail} Contains NotEscapeSequence.
            if (string_literal_type != StringLiteralType::Normal)
                message = "Octal escape sequence not allowed in template literal"_string;
            else if (m_state.strict_mode)
                message = "Octal escape sequence in string literal not allowed in strict mode"_string;
        } else if (status == Token::StringValueStatus::MalformedHexEscape || status == Token::StringValueStatus::MalformedUnicodeEscape) {
            auto type = status == Token::StringValueStatus::MalformedUnicodeEscape ? "unicode" : "hexadecimal";
            message = MUST(String::formatted("Malformed {} escape sequence", type));
        } else if (status == Token::StringValueStatus::UnicodeEscapeOverflow) {
            message = "Unicode code_point must not be greater than 0x10ffff in escape sequence"_string;
        } else {
            VERIFY_NOT_REACHED();
        }

        if (!message.is_empty()) {
            if (contains_invalid_escape != nullptr) {
                VERIFY(string_literal_type == StringLiteralType::TaggedTemplate);
                *contains_invalid_escape = true;
            } else {
                syntax_error(message, Position { token.line_number(), token.line_column() });
            }
        }
    }

    return create_ast_node<StringLiteral>({ m_source_code, rule_start.position(), position() }, MUST(String::from_byte_string(string)));
}

NonnullRefPtr<TemplateLiteral const> Parser::parse_template_literal(bool is_tagged)
{
    auto rule_start = push_start();
    consume(TokenType::TemplateLiteralStart);

    Vector<NonnullRefPtr<Expression const>> expressions;
    Vector<NonnullRefPtr<Expression const>> raw_strings;

    auto append_empty_string = [this, &rule_start, &expressions, &raw_strings, is_tagged]() {
        auto string_literal = create_ast_node<StringLiteral>({ m_source_code, rule_start.position(), position() }, ""_string);
        expressions.append(string_literal);
        if (is_tagged)
            raw_strings.append(string_literal);
    };

    if (!match(TokenType::TemplateLiteralString))
        append_empty_string();

    while (!done() && !match(TokenType::TemplateLiteralEnd) && !match(TokenType::UnterminatedTemplateLiteral)) {
        if (match(TokenType::TemplateLiteralString)) {
            auto token = consume();
            bool contains_invalid_escape = false;
            auto parsed_string_value = parse_string_literal(token,
                is_tagged ? StringLiteralType::TaggedTemplate : StringLiteralType::NonTaggedTemplate,
                is_tagged ? &contains_invalid_escape : nullptr);
            // An invalid string leads to a cooked value of `undefined` but still gives the raw string.
            if (contains_invalid_escape)
                expressions.append(create_ast_node<NullLiteral>({ m_source_code, rule_start.position(), position() }));
            else
                expressions.append(move(parsed_string_value));
            if (is_tagged)
                raw_strings.append(create_ast_node<StringLiteral>({ m_source_code, rule_start.position(), position() }, MUST(String::from_byte_string(token.raw_template_value()))));
        } else if (match(TokenType::TemplateLiteralExprStart)) {
            consume(TokenType::TemplateLiteralExprStart);
            if (match(TokenType::TemplateLiteralExprEnd)) {
                syntax_error("Empty template literal expression block"_string);
                return create_ast_node<TemplateLiteral>({ m_source_code, rule_start.position(), position() }, expressions);
            }

            expressions.append(parse_expression(0));
            if (match(TokenType::UnterminatedTemplateLiteral)) {
                syntax_error("Unterminated template literal"_string);
                return create_ast_node<TemplateLiteral>({ m_source_code, rule_start.position(), position() }, expressions);
            }
            consume(TokenType::TemplateLiteralExprEnd);

            if (!match(TokenType::TemplateLiteralString))
                append_empty_string();
        } else {
            expected("Template literal string or expression");
            break;
        }
    }

    if (match(TokenType::UnterminatedTemplateLiteral)) {
        syntax_error("Unterminated template literal"_string);
    } else {
        consume(TokenType::TemplateLiteralEnd);
    }

    if (is_tagged)
        return create_ast_node<TemplateLiteral>({ m_source_code, rule_start.position(), position() }, expressions, raw_strings);
    return create_ast_node<TemplateLiteral>({ m_source_code, rule_start.position(), position() }, expressions);
}

NonnullRefPtr<Expression const> Parser::parse_expression(int min_precedence, Associativity associativity, ForbiddenTokens forbidden)
{
    auto rule_start = push_start();
    auto [expression, should_continue_parsing] = parse_primary_expression();
    auto check_for_invalid_object_property = [&](auto& expression) {
        if (is<ObjectExpression>(*expression)) {
            if (auto start_offset = m_state.invalid_property_range_in_object_expression.get(expression->start_offset()); start_offset.has_value())
                syntax_error("Invalid property in object literal"_string, start_offset.value());
        }
    };
    if (is<Identifier>(*expression) && m_state.current_scope_pusher) {
        auto identifier_instance = static_ptr_cast<Identifier const>(expression);
        auto function_scope = m_state.current_scope_pusher->last_function_scope();
        auto function_parent_scope = function_scope ? function_scope->parent_scope() : nullptr;
        bool has_not_been_declared_as_variable = true;
        for (auto scope = m_state.current_scope_pusher; scope != function_parent_scope; scope = scope->parent_scope()) {
            if (scope->has_declaration(identifier_instance->string())) {
                has_not_been_declared_as_variable = false;
                break;
            }
        }

        if (has_not_been_declared_as_variable) {
            if (identifier_instance->string() == "arguments"sv)
                m_state.current_scope_pusher->set_contains_access_to_arguments_object();
        }
    }

    while (match(TokenType::TemplateLiteralStart)) {
        auto template_literal = parse_template_literal(true);
        expression = create_ast_node<TaggedTemplateLiteral>({ m_source_code, rule_start.position(), position() }, move(expression), move(template_literal));
    }
    if (should_continue_parsing) {
        auto original_forbidden = forbidden;
        while (match_secondary_expression(forbidden)) {
            int new_precedence = g_operator_precedence.get(m_state.current_token.type());
            if (new_precedence < min_precedence)
                break;
            if (new_precedence == min_precedence && associativity == Associativity::Left)
                break;
            check_for_invalid_object_property(expression);

            Associativity new_associativity = operator_associativity(m_state.current_token.type());
            auto result = parse_secondary_expression(move(expression), new_precedence, new_associativity, original_forbidden);
            expression = result.expression;
            forbidden = forbidden.merge(result.forbidden);
            while (match(TokenType::TemplateLiteralStart) && !is<UpdateExpression>(*expression)) {
                auto template_literal = parse_template_literal(true);
                expression = create_ast_node<TaggedTemplateLiteral>({ m_source_code, rule_start.position(), position() }, move(expression), move(template_literal));
            }
        }
    }

    if (is<SuperExpression>(*expression))
        syntax_error("'super' keyword unexpected here"_string);

    check_for_invalid_object_property(expression);

    if (is<CallExpression>(*expression) && m_state.current_scope_pusher) {
        auto& callee = static_ptr_cast<CallExpression const>(expression)->callee();
        if (is<Identifier>(callee) && static_cast<Identifier const&>(callee).string() == "eval"sv) {
            m_state.current_scope_pusher->set_contains_direct_call_to_eval();
            m_state.current_scope_pusher->set_uses_this();
        }
    }

    if (match(TokenType::Comma) && min_precedence <= 1) {
        Vector<NonnullRefPtr<Expression const>> expressions;
        expressions.append(expression);
        while (match(TokenType::Comma)) {
            consume();
            expressions.append(parse_expression(2));
        }
        expressions.shrink_to_fit();
        expression = create_ast_node<SequenceExpression>({ m_source_code, rule_start.position(), position() }, move(expressions));
    }
    return expression;
}

Parser::ExpressionResult Parser::parse_secondary_expression(NonnullRefPtr<Expression const> lhs, int min_precedence, Associativity associativity, ForbiddenTokens forbidden)
{
    auto rule_start = push_start();
    switch (m_state.current_token.type()) {
    case TokenType::Plus:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::Addition, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::PlusEquals:
        return parse_assignment_expression(AssignmentOp::AdditionAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::Minus:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::Subtraction, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::MinusEquals:
        return parse_assignment_expression(AssignmentOp::SubtractionAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::Asterisk:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::Multiplication, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::AsteriskEquals:
        return parse_assignment_expression(AssignmentOp::MultiplicationAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::Slash:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::Division, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::SlashEquals:
        return parse_assignment_expression(AssignmentOp::DivisionAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::Percent:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::Modulo, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::PercentEquals:
        return parse_assignment_expression(AssignmentOp::ModuloAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::DoubleAsterisk:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::Exponentiation, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::DoubleAsteriskEquals:
        return parse_assignment_expression(AssignmentOp::ExponentiationAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::GreaterThan:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::GreaterThan, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::GreaterThanEquals:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::GreaterThanEquals, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::LessThan:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::LessThan, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::LessThanEquals:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::LessThanEquals, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::EqualsEqualsEquals:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::StrictlyEquals, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::ExclamationMarkEqualsEquals:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::StrictlyInequals, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::EqualsEquals:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::LooselyEquals, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::ExclamationMarkEquals:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::LooselyInequals, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::In:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::In, move(lhs), parse_expression(min_precedence, associativity));
    case TokenType::Instanceof:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::InstanceOf, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::Ampersand:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::BitwiseAnd, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::AmpersandEquals:
        return parse_assignment_expression(AssignmentOp::BitwiseAndAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::Pipe:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::BitwiseOr, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::PipeEquals:
        return parse_assignment_expression(AssignmentOp::BitwiseOrAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::Caret:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::BitwiseXor, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::CaretEquals:
        return parse_assignment_expression(AssignmentOp::BitwiseXorAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::ShiftLeft:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::LeftShift, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::ShiftLeftEquals:
        return parse_assignment_expression(AssignmentOp::LeftShiftAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::ShiftRight:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::RightShift, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::ShiftRightEquals:
        return parse_assignment_expression(AssignmentOp::RightShiftAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::UnsignedShiftRight:
        consume();
        return create_ast_node<BinaryExpression>({ m_source_code, rule_start.position(), position() }, BinaryOp::UnsignedRightShift, move(lhs), parse_expression(min_precedence, associativity, forbidden));
    case TokenType::UnsignedShiftRightEquals:
        return parse_assignment_expression(AssignmentOp::UnsignedRightShiftAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::ParenOpen:
        return parse_call_expression(move(lhs));
    case TokenType::Equals:
        return parse_assignment_expression(AssignmentOp::Assignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::Period:
        consume();
        if (match(TokenType::PrivateIdentifier)) {
            if (!is_private_identifier_valid())
                syntax_error(MUST(String::formatted("Reference to undeclared private field or method '{}'", m_state.current_token.value())));
            else if (is<SuperExpression>(*lhs))
                syntax_error(MUST(String::formatted("Cannot access private field or method '{}' on super", m_state.current_token.value())));

            return create_ast_node<MemberExpression>({ m_source_code, rule_start.position(), position() }, move(lhs), create_ast_node<PrivateIdentifier>({ m_source_code, rule_start.position(), position() }, consume().fly_string_value().to_string()));
        } else if (!match_identifier_name()) {
            expected("IdentifierName");
        }

        return create_ast_node<MemberExpression>({ m_source_code, rule_start.position(), position() }, move(lhs), create_ast_node<Identifier>({ m_source_code, rule_start.position(), position() }, consume_and_allow_division().fly_string_value()));
    case TokenType::BracketOpen: {
        consume(TokenType::BracketOpen);
        auto expression = create_ast_node<MemberExpression>({ m_source_code, rule_start.position(), position() }, move(lhs), parse_expression(0), true);
        consume(TokenType::BracketClose);
        return expression;
    }
    case TokenType::PlusPlus:
        if (!is_simple_assignment_target(*lhs))
            syntax_error(MUST(String::formatted("Left-hand side of postfix increment operator must be identifier or member expression, got {}", lhs->class_name())));

        if (m_state.strict_mode && is<Identifier>(*lhs)) {
            auto& identifier = static_cast<Identifier const&>(*lhs);
            auto& name = identifier.string();
            check_identifier_name_for_assignment_validity(name);
        }

        consume();
        return create_ast_node<UpdateExpression>({ m_source_code, rule_start.position(), position() }, UpdateOp::Increment, move(lhs));
    case TokenType::MinusMinus:
        if (!is_simple_assignment_target(*lhs))
            syntax_error(MUST(String::formatted("Left-hand side of postfix increment operator must be identifier or member expression, got {}", lhs->class_name())));

        if (m_state.strict_mode && is<Identifier>(*lhs)) {
            auto& identifier = static_cast<Identifier const&>(*lhs);
            auto& name = identifier.string();
            check_identifier_name_for_assignment_validity(name);
        }
        consume();
        return create_ast_node<UpdateExpression>({ m_source_code, rule_start.position(), position() }, UpdateOp::Decrement, move(lhs));
    case TokenType::DoubleAmpersand: {
        consume();
        auto expression = create_ast_node<LogicalExpression>({ m_source_code, rule_start.position(), position() }, LogicalOp::And, move(lhs), parse_expression(min_precedence, associativity, forbidden.forbid({ TokenType::DoubleQuestionMark })));
        return { expression, { TokenType::DoubleQuestionMark } };
    }
    case TokenType::DoubleAmpersandEquals:
        return parse_assignment_expression(AssignmentOp::AndAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::DoublePipe: {
        consume();
        auto expression = create_ast_node<LogicalExpression>({ m_source_code, rule_start.position(), position() }, LogicalOp::Or, move(lhs), parse_expression(min_precedence, associativity, forbidden.forbid({ TokenType::DoubleQuestionMark })));
        return { expression, { TokenType::DoubleQuestionMark } };
    }
    case TokenType::DoublePipeEquals:
        return parse_assignment_expression(AssignmentOp::OrAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::DoubleQuestionMark: {
        consume();
        auto expression = create_ast_node<LogicalExpression>({ m_source_code, rule_start.position(), position() }, LogicalOp::NullishCoalescing, move(lhs), parse_expression(min_precedence, associativity, forbidden.forbid({ TokenType::DoubleAmpersand, TokenType::DoublePipe })));
        return { expression, { TokenType::DoubleAmpersand, TokenType::DoublePipe } };
    }
    case TokenType::DoubleQuestionMarkEquals:
        return parse_assignment_expression(AssignmentOp::NullishAssignment, move(lhs), min_precedence, associativity, forbidden);
    case TokenType::QuestionMark:
        return parse_conditional_expression(move(lhs), forbidden);
    case TokenType::QuestionMarkPeriod: {
        auto const* lhs_expression = lhs.ptr();
        if (is<NewExpression>(lhs_expression)) {
            auto const& new_expression = static_cast<NewExpression const&>(*lhs_expression);
            if (!new_expression.is_parenthesized() && !new_expression.is_inside_parens()) {
                syntax_error("'new' cannot be used with optional chaining"_string, position());
                consume();
                return lhs;
            }
        }
        return parse_optional_chain(move(lhs));
    }
    default:
        expected("secondary expression");
        consume();
        return create_ast_node<ErrorExpression>({ m_source_code, rule_start.position(), position() });
    }
}

bool Parser::is_private_identifier_valid() const
{
    VERIFY(match(TokenType::PrivateIdentifier));
    if (!m_state.referenced_private_names)
        return false;

    // We might not have hit the declaration yet so class will check this in the end
    m_state.referenced_private_names->set(m_state.current_token.fly_string_value());
    return true;
}

RefPtr<BindingPattern const> Parser::synthesize_binding_pattern(Expression const& expression)
{
    VERIFY(is<ArrayExpression>(expression) || is<ObjectExpression>(expression));
    // Clear any syntax error that has occurred in the range that 'expression' spans.
    m_state.errors.remove_all_matching([range = expression.source_range()](auto const& error) {
        return error.position.has_value() && range.contains(*error.position);
    });
    // Make a parser and parse the source for this expression as a binding pattern.
    // NOTE: There's currently a fundamental problem that we pass the *next* (a.k.a. `current_token`)
    // token's position to most nodes' SourceRange when using `rule_start.position(), position()`.
    // This means that `source` will contain the subsequent token's trivia, if any (which is fine).
    auto source_start_offset = expression.source_range().start.offset;
    auto source_end_offset = expression.source_range().end.offset;
    auto source = m_state.lexer.source().substring_view(source_start_offset, source_end_offset - source_start_offset);
    Lexer lexer { source, m_state.lexer.filename(), expression.source_range().start.line, expression.source_range().start.column };
    Parser parser { lexer };

    parser.m_state.current_scope_pusher = m_state.current_scope_pusher;
    parser.m_state.strict_mode = m_state.strict_mode;
    parser.m_state.allow_super_property_lookup = m_state.allow_super_property_lookup;
    parser.m_state.allow_super_constructor_call = m_state.allow_super_constructor_call;
    parser.m_state.in_function_context = m_state.in_function_context;
    parser.m_state.in_formal_parameter_context = m_state.in_formal_parameter_context;
    parser.m_state.in_generator_function_context = m_state.in_generator_function_context;
    parser.m_state.await_expression_is_valid = m_state.await_expression_is_valid;
    parser.m_state.in_arrow_function_context = m_state.in_arrow_function_context;
    parser.m_state.in_break_context = m_state.in_break_context;
    parser.m_state.in_continue_context = m_state.in_continue_context;
    parser.m_state.string_legacy_octal_escape_sequence_in_scope = m_state.string_legacy_octal_escape_sequence_in_scope;
    parser.m_state.in_class_field_initializer = m_state.in_class_field_initializer;
    parser.m_state.in_class_static_init_block = m_state.in_class_static_init_block;
    parser.m_state.referenced_private_names = m_state.referenced_private_names;

    auto result = parser.parse_binding_pattern(AllowDuplicates::Yes, AllowMemberExpressions::Yes);
    if (parser.has_errors())
        m_state.errors.extend(parser.errors());
    return result;
}

NonnullRefPtr<AssignmentExpression const> Parser::parse_assignment_expression(AssignmentOp assignment_op, NonnullRefPtr<Expression const> lhs, int min_precedence, Associativity associativity, ForbiddenTokens forbidden)
{
    auto rule_start = push_start();
    VERIFY(match(TokenType::Equals)
        || match(TokenType::PlusEquals)
        || match(TokenType::MinusEquals)
        || match(TokenType::AsteriskEquals)
        || match(TokenType::SlashEquals)
        || match(TokenType::PercentEquals)
        || match(TokenType::DoubleAsteriskEquals)
        || match(TokenType::AmpersandEquals)
        || match(TokenType::PipeEquals)
        || match(TokenType::CaretEquals)
        || match(TokenType::ShiftLeftEquals)
        || match(TokenType::ShiftRightEquals)
        || match(TokenType::UnsignedShiftRightEquals)
        || match(TokenType::DoubleAmpersandEquals)
        || match(TokenType::DoublePipeEquals)
        || match(TokenType::DoubleQuestionMarkEquals));
    consume();

    if (assignment_op == AssignmentOp::Assignment) {
        if (is<ArrayExpression>(*lhs) || is<ObjectExpression>(*lhs)) {
            auto binding_pattern = synthesize_binding_pattern(*lhs);
            if (binding_pattern) {
                auto rhs = parse_expression(min_precedence, associativity);
                return create_ast_node<AssignmentExpression>(
                    { m_source_code, rule_start.position(), position() },
                    assignment_op,
                    binding_pattern.release_nonnull(),
                    move(rhs));
            }
        }
    }

    // Note: The web reality is that all but &&=, ||= and ??= do allow left hand side CallExpresions.
    //       These are the exception as they are newer.
    auto has_web_reality_assignment_target_exceptions = assignment_op != AssignmentOp::AndAssignment
        && assignment_op != AssignmentOp::OrAssignment
        && assignment_op != AssignmentOp::NullishAssignment;

    if (!is_simple_assignment_target(*lhs, has_web_reality_assignment_target_exceptions)) {
        syntax_error("Invalid left-hand side in assignment"_string);
    } else if (m_state.strict_mode && is<Identifier>(*lhs)) {
        auto const& name = static_cast<Identifier const&>(*lhs).string();
        check_identifier_name_for_assignment_validity(name);
    }
    auto rhs = parse_expression(min_precedence, associativity, forbidden);
    return create_ast_node<AssignmentExpression>({ m_source_code, rule_start.position(), position() }, assignment_op, move(lhs), move(rhs));
}

NonnullRefPtr<Identifier const> Parser::parse_identifier()
{
    auto identifier_start = position();
    auto token = consume_identifier();
    if (m_state.in_class_field_initializer && token.value() == "arguments"sv)
        syntax_error("'arguments' is not allowed in class field initializer"_string);
    return create_identifier_and_register_in_current_scope({ m_source_code, identifier_start, position() }, token.fly_string_value());
}

Vector<CallExpression::Argument> Parser::parse_arguments()
{
    Vector<CallExpression::Argument> arguments;

    consume(TokenType::ParenOpen);
    while (match_expression() || match(TokenType::TripleDot)) {
        if (match(TokenType::TripleDot)) {
            consume();
            arguments.append({ parse_expression(2), true });
        } else {
            arguments.append({ parse_expression(2), false });
        }
        if (!match(TokenType::Comma))
            break;
        consume();
    }

    consume(TokenType::ParenClose);
    return arguments;
}

NonnullRefPtr<Expression const> Parser::parse_call_expression(NonnullRefPtr<Expression const> lhs)
{
    auto rule_start = push_start();
    if (!m_state.allow_super_constructor_call && is<SuperExpression>(*lhs))
        syntax_error("'super' keyword unexpected here"_string);

    auto arguments = parse_arguments();

    if (is<SuperExpression>(*lhs))
        return create_ast_node<SuperCall>({ m_source_code, rule_start.position(), position() }, move(arguments));

    return CallExpression::create({ m_source_code, rule_start.position(), position() }, move(lhs), arguments.span(), InvocationStyleEnum::Parenthesized, InsideParenthesesEnum::NotInsideParentheses);
}

NonnullRefPtr<NewExpression const> Parser::parse_new_expression()
{
    auto rule_start = push_start();
    consume(TokenType::New);

    auto callee = parse_expression(g_operator_precedence.get(TokenType::New), Associativity::Right, { TokenType::ParenOpen, TokenType::QuestionMarkPeriod });
    if (is<ImportCall>(*callee))
        syntax_error("Cannot call new on dynamic import"_string, callee->source_range().start);

    Vector<CallExpression::Argument> arguments;

    auto is_parenthesized = match(TokenType::ParenOpen);

    if (is_parenthesized) {
        consume();
        while (match_expression() || match(TokenType::TripleDot)) {
            if (match(TokenType::TripleDot)) {
                consume();
                arguments.append({ parse_expression(2), true });
            } else {
                arguments.append({ parse_expression(2), false });
            }
            if (!match(TokenType::Comma))
                break;
            consume();
        }
        consume(TokenType::ParenClose);
    }

    InvocationStyleEnum invocation_style = is_parenthesized ? InvocationStyleEnum::Parenthesized : InvocationStyleEnum::NotParenthesized;

    return NewExpression::create({ m_source_code, rule_start.position(), position() }, move(callee), move(arguments), invocation_style, InsideParenthesesEnum::NotInsideParentheses);
}

NonnullRefPtr<YieldExpression const> Parser::parse_yield_expression()
{
    auto rule_start = push_start();

    if (m_state.in_formal_parameter_context)
        syntax_error("'Yield' expression is not allowed in formal parameters of generator function"_string);

    consume(TokenType::Yield);
    RefPtr<Expression const> argument;
    bool yield_from = false;

    if (!m_state.current_token.trivia_contains_line_terminator()) {
        if (match(TokenType::Asterisk)) {
            consume();
            yield_from = true;
        }

        if (yield_from || match_expression() || match(TokenType::Class))
            argument = parse_expression(2);
    }

    return create_ast_node<YieldExpression>({ m_source_code, rule_start.position(), position() }, move(argument), yield_from);
}

NonnullRefPtr<AwaitExpression const> Parser::parse_await_expression()
{
    auto rule_start = push_start();

    if (m_state.in_formal_parameter_context)
        syntax_error("'Await' expression is not allowed in formal parameters of an async function"_string);

    consume(TokenType::Await);

    auto precedence = g_operator_precedence.get(TokenType::Await);
    auto associativity = operator_associativity(TokenType::Await);
    auto argument = parse_expression(precedence, associativity);

    m_state.current_scope_pusher->set_contains_await_expression();

    return create_ast_node<AwaitExpression>({ m_source_code, rule_start.position(), position() }, move(argument));
}

NonnullRefPtr<ReturnStatement const> Parser::parse_return_statement()
{
    auto rule_start = push_start();
    if (!m_state.in_function_context && !m_state.in_arrow_function_context)
        syntax_error("'return' not allowed outside of a function"_string);

    consume(TokenType::Return);

    // Automatic semicolon insertion: terminate statement when return is followed by newline
    if (m_state.current_token.trivia_contains_line_terminator())
        return create_ast_node<ReturnStatement>({ m_source_code, rule_start.position(), position() }, nullptr);

    if (match_expression()) {
        auto expression = parse_expression(0);
        consume_or_insert_semicolon();
        return create_ast_node<ReturnStatement>({ m_source_code, rule_start.position(), position() }, move(expression));
    }

    consume_or_insert_semicolon();
    return create_ast_node<ReturnStatement>({ m_source_code, rule_start.position(), position() }, nullptr);
}

void Parser::parse_statement_list(ScopeNode& output_node, AllowLabelledFunction allow_labelled_functions)
{
    while (!done()) {
        if (match_declaration(AllowUsingDeclaration::Yes)) {
            auto declaration = parse_declaration();
            VERIFY(m_state.current_scope_pusher);
            m_state.current_scope_pusher->add_declaration(declaration);
            output_node.append(move(declaration));
        } else if (match_statement()) {
            output_node.append(parse_statement(allow_labelled_functions));
        } else {
            break;
        }
    }

    output_node.shrink_to_fit();
}

// FunctionBody, https://tc39.es/ecma262/#prod-FunctionBody
NonnullRefPtr<FunctionBody const> Parser::parse_function_body(NonnullRefPtr<FunctionParameters const> parameters, FunctionKind function_kind, FunctionParsingInsights& parsing_insights)
{
    auto rule_start = push_start();
    auto function_body = create_ast_node<FunctionBody>({ m_source_code, rule_start.position(), position() });

    VERIFY(m_state.current_scope_pusher->type() == ScopePusher::ScopeType::Function);
    m_state.current_scope_pusher->set_scope_node(function_body);
    m_state.current_scope_pusher->set_function_parameters(parameters);

    auto has_use_strict = parse_directive(function_body);
    bool previous_strict_mode = m_state.strict_mode;
    if (has_use_strict) {
        m_state.strict_mode = true;
        function_body->set_strict_mode();
        if (!is_simple_parameter_list(*parameters))
            syntax_error("Illegal 'use strict' directive in function with non-simple parameter list"_string);
    } else if (previous_strict_mode) {
        function_body->set_strict_mode();
    }

    parse_statement_list(function_body);

    // If we're parsing the function body standalone, e.g. via CreateDynamicFunction, we must have reached EOF here.
    // Otherwise, we need a closing curly bracket (which is consumed elsewhere). If we get neither, it's an error.
    if (!match(TokenType::Eof) && !match(TokenType::CurlyClose))
        expected(Token::name(TokenType::CurlyClose));

    // If the function contains 'use strict' we need to check the parameters (again).
    if (function_body->in_strict_mode() || function_kind != FunctionKind::Normal) {
        Vector<StringView> parameter_names;
        for (auto& parameter : parameters->parameters()) {
            parameter.binding.visit(
                [&](Identifier const& identifier) {
                    auto const& parameter_name = identifier.string();

                    check_identifier_name_for_assignment_validity(parameter_name, function_body->in_strict_mode());
                    if (function_kind == FunctionKind::Generator && parameter_name == "yield"sv)
                        syntax_error("Parameter name 'yield' not allowed in this context"_string);

                    if (function_kind == FunctionKind::Async && parameter_name == "await"sv)
                        syntax_error("Parameter name 'await' not allowed in this context"_string);

                    for (auto& previous_name : parameter_names) {
                        if (previous_name == parameter_name) {
                            syntax_error(MUST(String::formatted("Duplicate parameter '{}' not allowed in strict mode", parameter_name)));
                        }
                    }

                    parameter_names.append(parameter_name);
                },
                [&](NonnullRefPtr<BindingPattern const> const& binding) {
                    // NOTE: Nothing in the callback throws an exception.
                    MUST(binding->for_each_bound_identifier([&](auto& bound_identifier) {
                        auto const& bound_name = bound_identifier.string();

                        if (function_kind == FunctionKind::Generator && bound_name == "yield"sv)
                            syntax_error("Parameter name 'yield' not allowed in this context"_string);

                        if (function_kind == FunctionKind::Async && bound_name == "await"sv)
                            syntax_error("Parameter name 'await' not allowed in this context"_string);

                        for (auto& previous_name : parameter_names) {
                            if (previous_name == bound_name) {
                                syntax_error(MUST(String::formatted("Duplicate parameter '{}' not allowed in strict mode", bound_name)));
                                break;
                            }
                        }
                        parameter_names.append(bound_name);
                    }));
                });
        }
    }

    m_state.strict_mode = previous_strict_mode;
    VERIFY(m_state.current_scope_pusher->type() == ScopePusher::ScopeType::Function);
    parsing_insights.contains_direct_call_to_eval = m_state.current_scope_pusher->contains_direct_call_to_eval();
    parsing_insights.uses_this_from_environment = m_state.current_scope_pusher->uses_this_from_environment();
    parsing_insights.uses_this = m_state.current_scope_pusher->uses_this();
    return function_body;
}

NonnullRefPtr<BlockStatement const> Parser::parse_block_statement()
{
    auto rule_start = push_start();
    auto block = create_ast_node<BlockStatement>({ m_source_code, rule_start.position(), position() });
    ScopePusher block_scope = ScopePusher::block_scope(*this, block);
    consume(TokenType::CurlyOpen);
    parse_statement_list(block);
    consume(TokenType::CurlyClose);

    return block;
}

template<typename FunctionNodeType>
NonnullRefPtr<FunctionNodeType> Parser::parse_function_node(u16 parse_options, Optional<Position> const& function_start)
{
    auto rule_start = function_start.has_value()
        ? RulePosition { *this, *function_start }
        : push_start();
    VERIFY(!(parse_options & FunctionNodeParseOptions::IsGetterFunction && parse_options & FunctionNodeParseOptions::IsSetterFunction));

    TemporaryChange super_property_access_rollback(m_state.allow_super_property_lookup, !!(parse_options & FunctionNodeParseOptions::AllowSuperPropertyLookup));
    TemporaryChange super_constructor_call_rollback(m_state.allow_super_constructor_call, !!(parse_options & FunctionNodeParseOptions::AllowSuperConstructorCall));
    TemporaryChange break_context_rollback(m_state.in_break_context, false);
    TemporaryChange continue_context_rollback(m_state.in_continue_context, false);
    TemporaryChange class_field_initializer_rollback(m_state.in_class_field_initializer, false);
    TemporaryChange might_need_arguments_object_rollback(m_state.function_might_need_arguments_object, false);
    TemporaryChange in_formal_parameter_context_rollback(m_state.in_formal_parameter_context, false);

    constexpr auto is_function_expression = IsSame<FunctionNodeType, FunctionExpression>;
    FunctionKind function_kind;
    if ((parse_options & FunctionNodeParseOptions::IsGeneratorFunction) != 0 && (parse_options & FunctionNodeParseOptions::IsAsyncFunction) != 0)
        function_kind = FunctionKind::AsyncGenerator;
    else if ((parse_options & FunctionNodeParseOptions::IsGeneratorFunction) != 0)
        function_kind = FunctionKind::Generator;
    else if ((parse_options & FunctionNodeParseOptions::IsAsyncFunction) != 0)
        function_kind = FunctionKind::Async;
    else
        function_kind = FunctionKind::Normal;
    RefPtr<Identifier const> name;
    if (parse_options & FunctionNodeParseOptions::CheckForFunctionAndName) {
        if (function_kind == FunctionKind::Normal && match(TokenType::Async) && !next_token().trivia_contains_line_terminator()) {
            function_kind = FunctionKind::Async;
            consume(TokenType::Async);
            parse_options |= FunctionNodeParseOptions::IsAsyncFunction;
        }
        consume(TokenType::Function);
        if (match(TokenType::Asterisk)) {
            function_kind = function_kind == FunctionKind::Normal ? FunctionKind::Generator : FunctionKind::AsyncGenerator;
            consume(TokenType::Asterisk);
            parse_options |= FunctionNodeParseOptions::IsGeneratorFunction;
        }

        if (parse_options & FunctionNodeParseOptions::HasDefaultExportName) {
            name = create_identifier_and_register_in_current_scope(
                { m_source_code, rule_start.position(), position() },
                ExportStatement::local_name_for_default);
        } else if (FunctionNodeType::must_have_name() || match_identifier()) {
            name = create_identifier_and_register_in_current_scope(
                { m_source_code, rule_start.position(), position() },
                consume_identifier().fly_string_value());
        } else if (is_function_expression && (match(TokenType::Yield) || match(TokenType::Await))) {
            name = create_identifier_and_register_in_current_scope(
                { m_source_code, rule_start.position(), position() },
                consume().fly_string_value());
        }

        if (name) {
            check_identifier_name_for_assignment_validity(name->string());

            if (function_kind == FunctionKind::AsyncGenerator && (name->string() == "await"sv || name->string() == "yield"sv))
                syntax_error(MUST(String::formatted("async generator function is not allowed to be called '{}'", name->string())));

            if (m_state.in_class_static_init_block && name->string() == "await"sv)
                syntax_error("'await' is a reserved word"_string);
        }
    }
    TemporaryChange class_static_initializer_rollback(m_state.in_class_static_init_block, false);
    TemporaryChange generator_change(m_state.in_generator_function_context, function_kind == FunctionKind::Generator || function_kind == FunctionKind::AsyncGenerator);
    TemporaryChange async_change(m_state.await_expression_is_valid, function_kind == FunctionKind::Async || function_kind == FunctionKind::AsyncGenerator);

    i32 function_length = -1;
    RefPtr<FunctionParameters const> parameters;
    FunctionParsingInsights parsing_insights;
    auto body = [&] {
        ScopePusher function_scope = ScopePusher::function_scope(*this, name);

        consume(TokenType::ParenOpen);
        parameters = parse_formal_parameters(function_length, parse_options);
        consume(TokenType::ParenClose);

        if (function_length == -1)
            function_length = parameters->size();

        TemporaryChange function_context_rollback(m_state.in_function_context, true);

        auto old_labels_in_scope = move(m_state.labels_in_scope);
        ScopeGuard guard([&]() {
            m_state.labels_in_scope = move(old_labels_in_scope);
        });

        consume(TokenType::CurlyOpen);

        return parse_function_body(*parameters, function_kind, parsing_insights);
    }();

    auto local_variables_names = body->local_variables_names();
    consume(TokenType::CurlyClose);

    auto has_strict_directive = body->in_strict_mode();

    if (has_strict_directive && name)
        check_identifier_name_for_assignment_validity(name->string(), true);

    auto function_start_offset = rule_start.position().offset;
    auto function_end_offset = position().offset - m_state.current_token.trivia().length();
    auto source_text = ByteString { m_state.lexer.source().substring_view(function_start_offset, function_end_offset - function_start_offset) };
    parsing_insights.might_need_arguments_object = m_state.function_might_need_arguments_object;
    return create_ast_node<FunctionNodeType>(
        { m_source_code, rule_start.position(), position() },
        name, move(source_text), move(body), parameters.release_nonnull(), function_length,
        function_kind, has_strict_directive, parsing_insights,
        move(local_variables_names));
}

NonnullRefPtr<FunctionParameters const> Parser::parse_formal_parameters(int& function_length, u16 parse_options)
{
    auto rule_start = push_start();
    bool has_default_parameter = false;
    bool has_rest_parameter = false;
    TemporaryChange formal_parameter_context_change { m_state.in_formal_parameter_context, true };

    Vector<FunctionParameter> parameters;

    auto consume_identifier_or_binding_pattern = [&]() -> Variant<NonnullRefPtr<Identifier const>, NonnullRefPtr<BindingPattern const>> {
        if (auto pattern = parse_binding_pattern(AllowDuplicates::No, AllowMemberExpressions::No))
            return pattern.release_nonnull();

        auto token = consume_identifier();
        auto parameter_name = token.fly_string_value();

        check_identifier_name_for_assignment_validity(parameter_name);

        for (auto& parameter : parameters) {
            bool has_same_name = parameter.binding.visit(
                [&](Identifier const& identifier) {
                    return identifier.string() == parameter_name;
                },
                [&](NonnullRefPtr<BindingPattern const> const& bindings) {
                    bool found_duplicate = false;
                    // NOTE: Nothing in the callback throws an exception.
                    MUST(bindings->for_each_bound_identifier([&](auto& bound_identifier) {
                        if (bound_identifier.string() == parameter_name)
                            found_duplicate = true;
                    }));
                    return found_duplicate;
                });

            if (!has_same_name)
                continue;

            String message;
            if (parse_options & FunctionNodeParseOptions::IsArrowFunction)
                message = MUST(String::formatted("Duplicate parameter '{}' not allowed in arrow function", parameter_name));
            else if (m_state.strict_mode)
                message = MUST(String::formatted("Duplicate parameter '{}' not allowed in strict mode", parameter_name));
            else if (has_default_parameter || match(TokenType::Equals))
                message = MUST(String::formatted("Duplicate parameter '{}' not allowed in function with default parameter", parameter_name));
            else if (has_rest_parameter)
                message = MUST(String::formatted("Duplicate parameter '{}' not allowed in function with rest parameter", parameter_name));
            if (!message.is_empty())
                syntax_error(message, Position { token.line_number(), token.line_column() });
            break;
        }
        return create_ast_node<Identifier const>({ m_source_code, rule_start.position(), position() }, token.fly_string_value());
    };

    while (match(TokenType::CurlyOpen) || match(TokenType::BracketOpen) || match_identifier() || match(TokenType::TripleDot)) {
        if (parse_options & FunctionNodeParseOptions::IsGetterFunction)
            syntax_error("Getter function must have no arguments"_string);
        if (parse_options & FunctionNodeParseOptions::IsSetterFunction && (parameters.size() >= 1 || match(TokenType::TripleDot)))
            syntax_error("Setter function must have one argument"_string);
        auto is_rest = false;
        if (match(TokenType::TripleDot)) {
            consume();
            has_rest_parameter = true;
            function_length = parameters.size();
            is_rest = true;
        }
        auto parameter = consume_identifier_or_binding_pattern();
        RefPtr<Expression const> default_value;
        if (match(TokenType::Equals)) {
            consume();

            if (is_rest)
                syntax_error("Rest parameter may not have a default initializer"_string);

            TemporaryChange change(m_state.in_function_context, true);
            has_default_parameter = true;
            function_length = parameters.size();
            default_value = parse_expression(2);

            bool is_generator = parse_options & FunctionNodeParseOptions::IsGeneratorFunction;
            if ((is_generator || m_state.strict_mode) && default_value && default_value->fast_is<Identifier>() && static_cast<Identifier const&>(*default_value).string() == "yield"sv)
                syntax_error("Generator function parameter initializer cannot contain a reference to an identifier named \"yield\""_string);
        }
        parameters.append({ move(parameter), default_value, is_rest });
        if (!match(TokenType::Comma) || is_rest)
            break;
        consume(TokenType::Comma);
    }
    if (parse_options & FunctionNodeParseOptions::IsSetterFunction && parameters.is_empty())
        syntax_error("Setter function must have one argument"_string);
    // If we're parsing the parameters standalone, e.g. via CreateDynamicFunction, we must have reached EOF here.
    // Otherwise, we need a closing parenthesis (which is consumed elsewhere). If we get neither, it's an error.
    if (!match(TokenType::Eof) && !match(TokenType::ParenClose))
        expected(Token::name(TokenType::ParenClose));

    parameters.shrink_to_fit();
    return FunctionParameters::create(move(parameters));
}

static AK::Array<FlyString, 36> s_reserved_words = { "break"_fly_string, "case"_fly_string, "catch"_fly_string, "class"_fly_string, "const"_fly_string, "continue"_fly_string, "debugger"_fly_string, "default"_fly_string, "delete"_fly_string, "do"_fly_string, "else"_fly_string, "enum"_fly_string, "export"_fly_string, "extends"_fly_string, "false"_fly_string, "finally"_fly_string, "for"_fly_string, "function"_fly_string, "if"_fly_string, "import"_fly_string, "in"_fly_string, "instanceof"_fly_string, "new"_fly_string, "null"_fly_string, "return"_fly_string, "super"_fly_string, "switch"_fly_string, "this"_fly_string, "throw"_fly_string, "true"_fly_string, "try"_fly_string, "typeof"_fly_string, "var"_fly_string, "void"_fly_string, "while"_fly_string, "with"_fly_string };

RefPtr<BindingPattern const> Parser::parse_binding_pattern(Parser::AllowDuplicates allow_duplicates, Parser::AllowMemberExpressions allow_member_expressions)
{
    auto rule_start = push_start();

    TokenType closing_token;
    bool is_object = true;

    if (match(TokenType::BracketOpen)) {
        consume();
        closing_token = TokenType::BracketClose;
        is_object = false;
    } else if (match(TokenType::CurlyOpen)) {
        consume();
        closing_token = TokenType::CurlyClose;
    } else {
        return {};
    }

    Vector<BindingPattern::BindingEntry> entries;

    while (!match(closing_token)) {
        if (!is_object && match(TokenType::Comma)) {
            consume();
            entries.append(BindingPattern::BindingEntry {});
            continue;
        }

        auto is_rest = false;

        if (match(TokenType::TripleDot)) {
            consume();
            is_rest = true;
        }

        decltype(BindingPattern::BindingEntry::name) name = Empty {};
        decltype(BindingPattern::BindingEntry::alias) alias = Empty {};
        RefPtr<Expression const> initializer = {};

        if (is_object) {
            bool needs_alias = false;
            if (allow_member_expressions == AllowMemberExpressions::Yes && is_rest) {
                auto expression_position = position();
                auto expression = parse_expression(2, Associativity::Right, { TokenType::Equals });
                if (is<MemberExpression>(*expression)) {
                    alias = static_ptr_cast<MemberExpression const>(expression);
                } else if (is<Identifier>(*expression)) {
                    name = static_ptr_cast<Identifier const>(expression);
                } else {
                    syntax_error("Invalid destructuring assignment target"_string, expression_position);
                    return {};
                }
            } else if (match_identifier_name() || match(TokenType::StringLiteral) || match(TokenType::NumericLiteral) || match(TokenType::BigIntLiteral)) {
                if (match(TokenType::StringLiteral) || match(TokenType::NumericLiteral))
                    needs_alias = true;

                if (match(TokenType::StringLiteral)) {
                    auto token = consume(TokenType::StringLiteral);
                    auto string_literal = parse_string_literal(token);
                    name = create_identifier_and_register_in_current_scope({ m_source_code, rule_start.position(), position() }, string_literal->value());
                } else if (match(TokenType::BigIntLiteral)) {
                    auto string_value = consume().fly_string_value();
                    VERIFY(string_value.bytes_as_string_view().ends_with("n"sv));
                    name = create_identifier_and_register_in_current_scope({ m_source_code, rule_start.position(), position() }, FlyString(MUST(string_value.to_string().substring_from_byte_offset(0, string_value.bytes().size() - 1))));
                } else {
                    name = create_identifier_and_register_in_current_scope({ m_source_code, rule_start.position(), position() }, consume().fly_string_value());
                }
            } else if (match(TokenType::BracketOpen)) {
                consume();
                auto expression = parse_expression(0);

                name = move(expression);
                consume(TokenType::BracketClose);
            } else {
                expected("identifier or computed property name");
                return {};
            }

            if (!is_rest && match(TokenType::Colon)) {
                consume();
                if (allow_member_expressions == AllowMemberExpressions::Yes) {
                    auto expression_position = position();
                    auto expression = parse_expression(2, Associativity::Right, { TokenType::Equals });
                    if (is<ArrayExpression>(*expression) || is<ObjectExpression>(*expression)) {
                        if (auto synthesized_binding_pattern = synthesize_binding_pattern(*expression)) {
                            alias = synthesized_binding_pattern.release_nonnull();
                        } else {
                            syntax_error("Invalid destructuring assignment target"_string, expression_position);
                            return {};
                        }
                    } else if (is<MemberExpression>(*expression)) {
                        alias = static_ptr_cast<MemberExpression const>(expression);
                    } else if (is<Identifier>(*expression)) {
                        alias = static_ptr_cast<Identifier const>(expression);
                    } else {
                        syntax_error("Invalid destructuring assignment target"_string, expression_position);
                        return {};
                    }
                } else if (match(TokenType::CurlyOpen) || match(TokenType::BracketOpen)) {
                    auto binding_pattern = parse_binding_pattern(allow_duplicates, allow_member_expressions);
                    if (!binding_pattern)
                        return {};
                    alias = binding_pattern.release_nonnull();
                } else if (match_identifier_name()) {
                    alias = create_identifier_and_register_in_current_scope({ m_source_code, rule_start.position(), position() }, consume().fly_string_value());
                } else {
                    expected("identifier or binding pattern");
                    return {};
                }
            } else if (needs_alias) {
                expected("alias for string or numeric literal name");
                return {};
            }
        } else {
            if (allow_member_expressions == AllowMemberExpressions::Yes) {
                auto expression_position = position();
                auto expression = parse_expression(2, Associativity::Right, { TokenType::Equals });

                if (is<ArrayExpression>(*expression) || is<ObjectExpression>(*expression)) {
                    if (auto synthesized_binding_pattern = synthesize_binding_pattern(*expression)) {
                        alias = synthesized_binding_pattern.release_nonnull();
                    } else {
                        syntax_error("Invalid destructuring assignment target"_string, expression_position);
                        return {};
                    }
                } else if (is<MemberExpression>(*expression)) {
                    alias = static_ptr_cast<MemberExpression const>(expression);
                } else if (is<Identifier>(*expression)) {
                    alias = static_ptr_cast<Identifier const>(expression);
                } else {
                    syntax_error("Invalid destructuring assignment target"_string, expression_position);
                    return {};
                }
            } else if (match(TokenType::BracketOpen) || match(TokenType::CurlyOpen)) {
                auto pattern = parse_binding_pattern(allow_duplicates, allow_member_expressions);
                if (!pattern) {
                    expected("binding pattern");
                    return {};
                }
                alias = pattern.release_nonnull();
            } else if (match_identifier_name()) {
                // BindingElement must always have an Empty name field
                auto identifier_name = consume_identifier().fly_string_value();
                alias = create_identifier_and_register_in_current_scope({ m_source_code, rule_start.position(), position() }, identifier_name);
            } else {
                expected("identifier or binding pattern");
                return {};
            }
        }

        if (match(TokenType::Equals)) {
            if (is_rest) {
                syntax_error("Unexpected initializer after rest element"_string);
                return {};
            }

            consume();

            initializer = parse_expression(2);
            if (!initializer) {
                expected("initialization expression");
                return {};
            }
        }

        entries.append(BindingPattern::BindingEntry { move(name), move(alias), move(initializer), is_rest });

        if (match(TokenType::Comma)) {
            if (is_rest) {
                syntax_error("Rest element may not be followed by a comma"_string);
                return {};
            }
            consume();
        } else if (is_object && !match(TokenType::CurlyClose)) {
            consume(TokenType::Comma);
        }
    }

    while (!is_object && match(TokenType::Comma))
        consume();

    consume(closing_token);

    auto kind = is_object ? BindingPattern::Kind::Object : BindingPattern::Kind::Array;
    auto pattern = adopt_ref(*new BindingPattern);
    pattern->entries = move(entries);
    pattern->kind = kind;

    Vector<StringView> bound_names;
    // NOTE: Nothing in the callback throws an exception.
    MUST(pattern->for_each_bound_identifier([&](auto& identifier) {
        auto const& name = identifier.string();
        if (allow_duplicates == AllowDuplicates::No) {
            if (bound_names.contains_slow(name))
                syntax_error("Duplicate parameter names in bindings"_string);
            bound_names.append(name);
        }
        check_identifier_name_for_assignment_validity(name);
    }));

    return pattern;
}

RefPtr<Identifier const> Parser::parse_lexical_binding(Optional<DeclarationKind> declaration_kind)
{
    auto binding_start = push_start();

    if (match_identifier()) {
        return create_identifier_and_register_in_current_scope({ m_source_code, binding_start.position(), position() }, consume_identifier().fly_string_value(), declaration_kind);
    }
    if (!m_state.in_generator_function_context && match(TokenType::Yield)) {
        if (m_state.strict_mode)
            syntax_error("Identifier must not be a reserved word in strict mode ('yield')"_string);

        return create_identifier_and_register_in_current_scope({ m_source_code, binding_start.position(), position() }, consume().fly_string_value(), declaration_kind);
    }
    if (!m_state.await_expression_is_valid && match(TokenType::Async)) {
        if (m_program_type == Program::Type::Module)
            syntax_error("Identifier must not be a reserved word in modules ('async')"_string);

        return create_identifier_and_register_in_current_scope({ m_source_code, binding_start.position(), position() }, consume().fly_string_value(), declaration_kind);
    }

    return {};
}

NonnullRefPtr<VariableDeclaration const> Parser::parse_variable_declaration(IsForLoopVariableDeclaration is_for_loop_variable_declaration)
{
    auto rule_start = push_start();
    DeclarationKind declaration_kind;

    switch (m_state.current_token.type()) {
    case TokenType::Var:
        declaration_kind = DeclarationKind::Var;
        break;
    case TokenType::Let:
        declaration_kind = DeclarationKind::Let;
        break;
    case TokenType::Const:
        declaration_kind = DeclarationKind::Const;
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    consume();

    Vector<NonnullRefPtr<VariableDeclarator const>> declarations;
    for (;;) {
        Variant<NonnullRefPtr<Identifier const>, NonnullRefPtr<BindingPattern const>, Empty> target {};
        if (auto pattern = parse_binding_pattern(declaration_kind != DeclarationKind::Var ? AllowDuplicates::No : AllowDuplicates::Yes, AllowMemberExpressions::No)) {
            if ((declaration_kind == DeclarationKind::Let || declaration_kind == DeclarationKind::Const)) {
                // NOTE: Nothing in the callback throws an exception.
                MUST(pattern->for_each_bound_identifier([this](auto& identifier) {
                    if (identifier.string() == "let"sv)
                        syntax_error("Lexical binding may not be called 'let'"_string);
                }));
            }

            target = pattern.release_nonnull();
        } else if (auto lexical_binding = parse_lexical_binding(declaration_kind)) {
            check_identifier_name_for_assignment_validity(lexical_binding->string());
            if ((declaration_kind == DeclarationKind::Let || declaration_kind == DeclarationKind::Const) && lexical_binding->string() == "let"sv)
                syntax_error("Lexical binding may not be called 'let'"_string);

            target = lexical_binding.release_nonnull();
        }

        if (target.has<Empty>()) {
            expected("identifier or a binding pattern");
            if (match(TokenType::Comma)) {
                consume();
                continue;
            }
            break;
        }

        RefPtr<Expression const> init;
        if (match(TokenType::Equals)) {
            consume();
            // In a for loop 'in' can be ambiguous so we do not allow it
            // 14.7.4 The for Statement, https://tc39.es/ecma262/#prod-ForStatement and 14.7.5 The for-in, for-of, and for-await-of Statements, https://tc39.es/ecma262/#prod-ForInOfStatement
            if (is_for_loop_variable_declaration == IsForLoopVariableDeclaration::Yes)
                init = parse_expression(2, Associativity::Right, { TokenType::In });
            else
                init = parse_expression(2);
        } else if (is_for_loop_variable_declaration == IsForLoopVariableDeclaration::No && declaration_kind == DeclarationKind::Const) {
            syntax_error("Missing initializer in 'const' variable declaration"_string);
        } else if (is_for_loop_variable_declaration == IsForLoopVariableDeclaration::No && target.has<NonnullRefPtr<BindingPattern const>>()) {
            syntax_error("Missing initializer in destructuring assignment"_string);
        }

        declarations.append(create_ast_node<VariableDeclarator>(
            { m_source_code, rule_start.position(), position() },
            move(target).downcast<NonnullRefPtr<Identifier const>, NonnullRefPtr<BindingPattern const>>(),
            move(init)));

        if (match(TokenType::Comma)) {
            consume();
            continue;
        }
        break;
    }
    if (is_for_loop_variable_declaration == IsForLoopVariableDeclaration::No)
        consume_or_insert_semicolon();

    declarations.shrink_to_fit();

    auto declaration = create_ast_node<VariableDeclaration>({ m_source_code, rule_start.position(), position() }, declaration_kind, move(declarations));
    return declaration;
}

NonnullRefPtr<UsingDeclaration const> Parser::parse_using_declaration(IsForLoopVariableDeclaration is_for_loop_variable_declaration)
{
    //  using [no LineTerminator here] BindingList[?In, ?Yield, ?Await, +Using] ;
    auto rule_start = push_start();
    VERIFY(m_state.current_token.original_value() == "using"sv);
    consume(TokenType::Identifier);
    VERIFY(!m_state.current_token.trivia_contains_line_terminator());
    Vector<NonnullRefPtr<VariableDeclarator const>> declarations;

    for (;;) {
        auto lexical_binding = parse_lexical_binding();
        if (!lexical_binding) {
            expected("lexical binding");
            break;
        }

        check_identifier_name_for_assignment_validity(lexical_binding->string());
        if (lexical_binding->string() == "let"sv)
            syntax_error("Lexical binding may not be called 'let'"_string);

        RefPtr<Expression const> initializer;
        if (match(TokenType::Equals)) {
            consume();

            if (is_for_loop_variable_declaration == IsForLoopVariableDeclaration::Yes)
                initializer = parse_expression(2, Associativity::Right, { TokenType::In });
            else
                initializer = parse_expression(2);
        } else if (is_for_loop_variable_declaration == IsForLoopVariableDeclaration::No) {
            consume(TokenType::Equals);
        }

        declarations.append(create_ast_node<VariableDeclarator>(
            { m_source_code, rule_start.position(), position() },
            lexical_binding.release_nonnull(),
            move(initializer)));

        if (match(TokenType::Comma)) {
            consume();
            continue;
        }
        break;
    }
    if (is_for_loop_variable_declaration == IsForLoopVariableDeclaration::No)
        consume_or_insert_semicolon();

    return create_ast_node<UsingDeclaration>({ m_source_code, rule_start.position(), position() }, move(declarations));
}

NonnullRefPtr<ThrowStatement const> Parser::parse_throw_statement()
{
    auto rule_start = push_start();
    consume(TokenType::Throw);

    // Automatic semicolon insertion: terminate statement when throw is followed by newline
    if (m_state.current_token.trivia_contains_line_terminator()) {
        syntax_error("No line break is allowed between 'throw' and its expression"_string);
        return create_ast_node<ThrowStatement>({ m_source_code, rule_start.position(), position() }, create_ast_node<ErrorExpression>({ m_source_code, rule_start.position(), position() }));
    }

    auto expression = parse_expression(0);
    consume_or_insert_semicolon();
    return create_ast_node<ThrowStatement>({ m_source_code, rule_start.position(), position() }, move(expression));
}

NonnullRefPtr<BreakStatement const> Parser::parse_break_statement()
{
    auto rule_start = push_start();
    consume(TokenType::Break);
    Optional<FlyString> target_label;
    if (match(TokenType::Semicolon)) {
        consume();
    } else {
        if (!m_state.current_token.trivia_contains_line_terminator() && match_identifier()) {
            target_label = consume().fly_string_value();

            auto label = m_state.labels_in_scope.find(target_label.value());
            if (label == m_state.labels_in_scope.end())
                syntax_error(MUST(String::formatted("Label '{}' not found", target_label.value())));
        }
        consume_or_insert_semicolon();
    }

    if (!target_label.has_value() && !m_state.in_break_context)
        syntax_error("Unlabeled 'break' not allowed outside of a loop or switch statement"_string);

    return create_ast_node<BreakStatement>({ m_source_code, rule_start.position(), position() }, target_label);
}

NonnullRefPtr<ContinueStatement const> Parser::parse_continue_statement()
{
    auto rule_start = push_start();
    if (!m_state.in_continue_context)
        syntax_error("'continue' not allow outside of a loop"_string);

    consume(TokenType::Continue);
    Optional<FlyString> target_label;
    if (match(TokenType::Semicolon)) {
        consume();
        return create_ast_node<ContinueStatement>({ m_source_code, rule_start.position(), position() }, target_label);
    }
    if (!m_state.current_token.trivia_contains_line_terminator() && match_identifier()) {
        auto label_position = position();
        target_label = consume().fly_string_value();

        auto label = m_state.labels_in_scope.find(target_label.value());
        if (label == m_state.labels_in_scope.end())
            syntax_error(MUST(String::formatted("Label '{}' not found or invalid", target_label.value())));
        else
            label->value = label_position;
    }
    consume_or_insert_semicolon();
    return create_ast_node<ContinueStatement>({ m_source_code, rule_start.position(), position() }, target_label);
}

NonnullRefPtr<ConditionalExpression const> Parser::parse_conditional_expression(NonnullRefPtr<Expression const> test, ForbiddenTokens forbidden)
{
    auto rule_start = push_start();
    consume(TokenType::QuestionMark);
    auto consequent = parse_expression(2);
    consume(TokenType::Colon);
    auto alternate = parse_expression(2, Associativity::Right, forbidden);
    return create_ast_node<ConditionalExpression>({ m_source_code, rule_start.position(), position() }, move(test), move(consequent), move(alternate));
}

NonnullRefPtr<OptionalChain const> Parser::parse_optional_chain(NonnullRefPtr<Expression const> base)
{
    auto rule_start = push_start();
    Vector<OptionalChain::Reference> chain;
    do {
        if (match(TokenType::QuestionMarkPeriod)) {
            consume(TokenType::QuestionMarkPeriod);
            switch (m_state.current_token.type()) {
            case TokenType::ParenOpen:
                chain.append(OptionalChain::Call { parse_arguments(), OptionalChain::Mode::Optional });
                break;
            case TokenType::BracketOpen:
                consume();
                chain.append(OptionalChain::ComputedReference { parse_expression(0), OptionalChain::Mode::Optional });
                consume(TokenType::BracketClose);
                break;
            case TokenType::PrivateIdentifier: {
                if (!is_private_identifier_valid())
                    syntax_error(MUST(String::formatted("Reference to undeclared private field or method '{}'", m_state.current_token.value())));

                auto start = position();
                auto private_identifier = consume();
                chain.append(OptionalChain::PrivateMemberReference {
                    create_ast_node<PrivateIdentifier>({ m_source_code, start, position() }, private_identifier.fly_string_value()),
                    OptionalChain::Mode::Optional });
                break;
            }
            case TokenType::TemplateLiteralStart:
                // 13.3.1.1 - Static Semantics: Early Errors
                // OptionalChain :
                //        ?. TemplateLiteral
                //        OptionalChain TemplateLiteral
                // This is a hard error.
                syntax_error("Invalid tagged template literal after ?."_string, position());
                break;
            default:
                if (match_identifier_name()) {
                    auto start = position();
                    auto identifier = consume_and_allow_division();
                    chain.append(OptionalChain::MemberReference {
                        create_ast_node<Identifier>({ m_source_code, start, position() }, identifier.fly_string_value()),
                        OptionalChain::Mode::Optional,
                    });
                } else {
                    syntax_error("Invalid optional chain reference after ?."_string, position());
                }
                break;
            }
        } else if (match(TokenType::ParenOpen)) {
            chain.append(OptionalChain::Call { parse_arguments(), OptionalChain::Mode::NotOptional });
        } else if (match(TokenType::Period)) {
            consume();
            if (match(TokenType::PrivateIdentifier)) {
                if (!is_private_identifier_valid())
                    syntax_error(MUST(String::formatted("Reference to undeclared private field or method '{}'", m_state.current_token.value())));

                auto start = position();
                auto private_identifier = consume();
                chain.append(OptionalChain::PrivateMemberReference {
                    create_ast_node<PrivateIdentifier>({ m_source_code, start, position() }, private_identifier.fly_string_value()),
                    OptionalChain::Mode::NotOptional,
                });
            } else if (match_identifier_name()) {
                auto start = position();
                auto identifier = consume_and_allow_division();
                chain.append(OptionalChain::MemberReference {
                    create_ast_node<Identifier>({ m_source_code, start, position() }, identifier.fly_string_value()),
                    OptionalChain::Mode::NotOptional,
                });
            } else {
                expected("an identifier");
                break;
            }
        } else if (match(TokenType::TemplateLiteralStart)) {
            // 13.3.1.1 - Static Semantics: Early Errors
            // OptionalChain :
            //        ?. TemplateLiteral
            //        OptionalChain TemplateLiteral
            syntax_error("Invalid tagged template literal after optional chain"_string, position());
            break;
        } else if (match(TokenType::BracketOpen)) {
            consume();
            chain.append(OptionalChain::ComputedReference { parse_expression(2), OptionalChain::Mode::NotOptional });
            consume(TokenType::BracketClose);
        } else {
            break;
        }
    } while (!done());

    return create_ast_node<OptionalChain>(
        { m_source_code, rule_start.position(), position() },
        move(base),
        move(chain));
}

NonnullRefPtr<TryStatement const> Parser::parse_try_statement()
{
    auto rule_start = push_start();
    consume(TokenType::Try);

    auto block = parse_block_statement();

    RefPtr<CatchClause const> handler;
    if (match(TokenType::Catch))
        handler = parse_catch_clause();

    RefPtr<BlockStatement const> finalizer;
    if (match(TokenType::Finally)) {
        consume();
        finalizer = parse_block_statement();
    }

    if (!handler && !finalizer)
        syntax_error("try statement must have a 'catch' or 'finally' clause"_string);

    return create_ast_node<TryStatement>({ m_source_code, rule_start.position(), position() }, move(block), move(handler), move(finalizer));
}

NonnullRefPtr<DoWhileStatement const> Parser::parse_do_while_statement()
{
    auto rule_start = push_start();
    consume(TokenType::Do);

    auto body = [&]() -> NonnullRefPtr<Statement const> {
        TemporaryChange break_change(m_state.in_break_context, true);
        TemporaryChange continue_change(m_state.in_continue_context, true);
        return parse_statement();
    }();

    consume(TokenType::While);
    consume(TokenType::ParenOpen);

    auto test = parse_expression(0);

    consume(TokenType::ParenClose);

    // Since ES 2015 a missing semicolon is inserted here, despite the regular ASI rules not applying
    if (match(TokenType::Semicolon))
        consume();

    return create_ast_node<DoWhileStatement>({ m_source_code, rule_start.position(), position() }, move(test), move(body));
}

NonnullRefPtr<WhileStatement const> Parser::parse_while_statement()
{
    auto rule_start = push_start();
    consume(TokenType::While);
    consume(TokenType::ParenOpen);

    auto test = parse_expression(0);

    consume(TokenType::ParenClose);

    TemporaryChange break_change(m_state.in_break_context, true);
    TemporaryChange continue_change(m_state.in_continue_context, true);
    auto body = parse_statement();

    return create_ast_node<WhileStatement>({ m_source_code, rule_start.position(), position() }, move(test), move(body));
}

NonnullRefPtr<SwitchStatement const> Parser::parse_switch_statement()
{
    auto rule_start = push_start();
    consume(TokenType::Switch);

    consume(TokenType::ParenOpen);
    auto determinant = parse_expression(0);
    consume(TokenType::ParenClose);

    consume(TokenType::CurlyOpen);

    Vector<NonnullRefPtr<SwitchCase>> cases;

    auto switch_statement = create_ast_node<SwitchStatement>({ m_source_code, rule_start.position(), position() }, move(determinant));

    ScopePusher switch_scope = ScopePusher::block_scope(*this, switch_statement);

    auto has_default = false;
    while (match(TokenType::Case) || match(TokenType::Default)) {
        if (match(TokenType::Default)) {
            if (has_default)
                syntax_error("Multiple 'default' clauses in switch statement"_string);
            has_default = true;
        }
        switch_statement->add_case(parse_switch_case());
    }

    consume(TokenType::CurlyClose);

    return switch_statement;
}

NonnullRefPtr<WithStatement const> Parser::parse_with_statement()
{
    auto rule_start = push_start();
    consume(TokenType::With);
    consume(TokenType::ParenOpen);

    auto object = parse_expression(0);

    consume(TokenType::ParenClose);

    auto with_scope_node = create_ast_node<BlockStatement>({ m_source_code, rule_start.position(), position() });
    ScopePusher with_scope = ScopePusher::with_scope(*this, with_scope_node);

    auto body = parse_statement();
    return create_ast_node<WithStatement>({ m_source_code, rule_start.position(), position() }, move(object), move(body));
}

NonnullRefPtr<SwitchCase const> Parser::parse_switch_case()
{
    auto rule_start = push_start();
    RefPtr<Expression const> test;

    if (consume().type() == TokenType::Case) {
        test = parse_expression(0);
    }

    consume(TokenType::Colon);

    Vector<NonnullRefPtr<Statement>> consequent;
    TemporaryChange break_change(m_state.in_break_context, true);
    auto switch_case = create_ast_node<SwitchCase>({ m_source_code, rule_start.position(), position() }, move(test));
    parse_statement_list(switch_case);

    return switch_case;
}

NonnullRefPtr<CatchClause const> Parser::parse_catch_clause()
{
    auto rule_start = push_start();
    consume(TokenType::Catch);

    FlyString parameter;
    RefPtr<BindingPattern const> pattern_parameter;
    auto should_expect_parameter = false;
    if (match(TokenType::ParenOpen)) {
        TemporaryChange catch_parameter_context_change { m_state.in_catch_parameter_context, true };
        should_expect_parameter = true;
        consume();
        if (match_identifier_name()
            && (!match(TokenType::Yield) || !m_state.in_generator_function_context)
            && (!match(TokenType::Async) || !m_state.await_expression_is_valid)
            && (!match(TokenType::Await) || !m_state.in_class_static_init_block))
            parameter = consume().fly_string_value();
        else
            pattern_parameter = parse_binding_pattern(AllowDuplicates::No, AllowMemberExpressions::No);
        consume(TokenType::ParenClose);
    }

    if (should_expect_parameter && parameter.is_empty() && !pattern_parameter)
        expected("an identifier or a binding pattern");

    HashTable<FlyString> bound_names;

    if (pattern_parameter) {
        // NOTE: Nothing in the callback throws an exception.
        MUST(pattern_parameter->for_each_bound_identifier(
            [&](auto& identifier) {
                check_identifier_name_for_assignment_validity(identifier.string());
                bound_names.set(identifier.string());
            }));
    }

    if (!parameter.is_empty()) {
        check_identifier_name_for_assignment_validity(parameter);
        bound_names.set(parameter);
    }

    ScopePusher catch_scope = ScopePusher::catch_scope(*this, pattern_parameter, parameter);
    auto body = parse_block_statement();

    // NOTE: Nothing in the callback throws an exception.
    MUST(body->for_each_lexically_declared_identifier([&](auto const& identifier) {
        if (bound_names.contains(identifier.string()))
            syntax_error(MUST(String::formatted("Identifier '{}' already declared as catch parameter", identifier.string())));
    }));

    if (pattern_parameter) {
        return create_ast_node<CatchClause>(
            { m_source_code, rule_start.position(), position() },
            pattern_parameter.release_nonnull(),
            move(body));
    }

    return create_ast_node<CatchClause>(
        { m_source_code, rule_start.position(), position() },
        move(parameter),
        move(body));
}

NonnullRefPtr<IfStatement const> Parser::parse_if_statement()
{
    auto rule_start = push_start();
    auto parse_function_declaration_as_block_statement = [&] {
        // https://tc39.es/ecma262/#sec-functiondeclarations-in-ifstatement-statement-clauses
        // This production only applies when parsing non-strict code. Source text matched
        // by this production is processed as if each matching occurrence of
        // FunctionDeclaration[?Yield, ?Await, ~Default] was the sole StatementListItem
        // of a BlockStatement occupying that position in the source text.
        // The semantics of such a synthetic BlockStatement includes the web legacy
        // compatibility semantics specified in B.3.2.
        VERIFY(match(TokenType::Function));
        auto block = create_ast_node<BlockStatement>({ m_source_code, rule_start.position(), position() });
        ScopePusher block_scope = ScopePusher::block_scope(*this, *block);
        auto declaration = parse_declaration();
        VERIFY(m_state.current_scope_pusher);
        block_scope.add_declaration(declaration);

        VERIFY(is<FunctionDeclaration>(*declaration));
        auto& function_declaration = static_cast<FunctionDeclaration const&>(*declaration);
        if (function_declaration.kind() == FunctionKind::Generator)
            syntax_error("Generator functions can only be declared in top-level or within a block"_string);
        if (function_declaration.kind() == FunctionKind::Async)
            syntax_error("Async functions can only be declared in top-level or within a block"_string);
        block->append(move(declaration));
        return block;
    };

    consume(TokenType::If);
    consume(TokenType::ParenOpen);
    auto predicate = parse_expression(0);
    consume(TokenType::ParenClose);

    RefPtr<Statement const> consequent;
    if (!m_state.strict_mode && match(TokenType::Function))
        consequent = parse_function_declaration_as_block_statement();
    else
        consequent = parse_statement();

    RefPtr<Statement const> alternate;
    if (match(TokenType::Else)) {
        consume();
        if (!m_state.strict_mode && match(TokenType::Function))
            alternate = parse_function_declaration_as_block_statement();
        else
            alternate = parse_statement();
    }
    return create_ast_node<IfStatement>({ m_source_code, rule_start.position(), position() }, move(predicate), move(*consequent), move(alternate));
}

NonnullRefPtr<Statement const> Parser::parse_for_statement()
{
    auto rule_start = push_start();
    auto is_await_loop = IsForAwaitLoop::No;

    auto loop_scope_node = create_ast_node<BlockStatement>({ m_source_code, rule_start.position(), position() });
    ScopePusher for_loop_scope = ScopePusher::for_loop_scope(*this, *loop_scope_node);

    auto match_of = [&](Token const& token) {
        return token.type() == TokenType::Identifier && token.original_value() == "of"sv;
    };

    auto match_for_in_of = [&]() {
        bool is_of = match_of(m_state.current_token);
        if (is_await_loop == IsForAwaitLoop::Yes) {
            if (!is_of)
                syntax_error("for await loop is only valid with 'of'"_string);
            else if (!m_state.await_expression_is_valid)
                syntax_error("for await loop is only valid in async function or generator"_string);
            return true;
        }

        return match(TokenType::In) || is_of;
    };

    consume(TokenType::For);

    if (match(TokenType::Await)) {
        consume();
        if (!m_state.await_expression_is_valid)
            syntax_error("for-await-of is only allowed in async function context"_string);
        is_await_loop = IsForAwaitLoop::Yes;
    }

    consume(TokenType::ParenOpen);

    Optional<ScopePusher> scope_pusher;

    RefPtr<ASTNode const> init;
    if (!match(TokenType::Semicolon)) {

        auto match_for_using_declaration = [&] {
            if (!match(TokenType::Identifier) || m_state.current_token.original_value() != "using"sv)
                return false;

            auto lookahead = next_token();
            if (lookahead.trivia_contains_line_terminator())
                return false;

            if (lookahead.original_value() == "of"sv)
                return false;

            return token_is_identifier(lookahead);
        };

        if (match_for_using_declaration()) {
            auto declaration = parse_using_declaration(IsForLoopVariableDeclaration::Yes);

            if (match_of(m_state.current_token)) {
                if (declaration->declarations().size() != 1)
                    syntax_error("Must have exactly one declaration in for using of"_string);
                else if (declaration->declarations().first()->init())
                    syntax_error("Using declaration cannot have initializer"_string);

                return parse_for_in_of_statement(move(declaration), is_await_loop);
            }

            if (match(TokenType::In))
                syntax_error("Using declaration not allowed in for-in loop"_string);

            init = move(declaration);
        } else if (match_variable_declaration()) {
            auto declaration = parse_variable_declaration(IsForLoopVariableDeclaration::Yes);
            m_state.current_scope_pusher->add_declaration(declaration);
            if (match_for_in_of()) {
                if (declaration->declarations().size() > 1)
                    syntax_error("Multiple declarations not allowed in for..in/of"_string);
                else if (declaration->declarations().size() < 1)
                    syntax_error("Need exactly one variable declaration in for..in/of"_string);

                return parse_for_in_of_statement(move(declaration), is_await_loop);
            }
            if (declaration->declaration_kind() == DeclarationKind::Const) {
                for (auto const& variable : declaration->declarations()) {
                    if (!variable->init())
                        syntax_error("Missing initializer in 'const' variable declaration"_string);
                }
            }

            init = move(declaration);
        } else if (match_expression()) {
            auto lookahead_token = next_token();
            bool starts_with_async_of = match(TokenType::Async) && match_of(lookahead_token);

            init = parse_expression(0, Associativity::Right, { TokenType::In });
            if (match_for_in_of()) {
                if (is_await_loop != IsForAwaitLoop::Yes
                    && starts_with_async_of && match_of(m_state.current_token))
                    syntax_error("for-of loop may not start with async of"_string);
                return parse_for_in_of_statement(*init, is_await_loop);
            }
        } else {
            syntax_error("Unexpected token in for loop"_string);
        }
    }
    consume(TokenType::Semicolon);

    RefPtr<Expression const> test;
    if (!match(TokenType::Semicolon))
        test = parse_expression(0);

    consume(TokenType::Semicolon);

    RefPtr<Expression const> update;
    if (!match(TokenType::ParenClose))
        update = parse_expression(0);

    consume(TokenType::ParenClose);

    TemporaryChange break_change(m_state.in_break_context, true);
    TemporaryChange continue_change(m_state.in_continue_context, true);

    auto body = parse_statement();

    return create_ast_node<ForStatement>({ m_source_code, rule_start.position(), position() }, move(init), move(test), move(update), move(body));
}

NonnullRefPtr<Statement const> Parser::parse_for_in_of_statement(NonnullRefPtr<ASTNode const> lhs, IsForAwaitLoop is_for_await_loop)
{
    Variant<NonnullRefPtr<ASTNode const>, NonnullRefPtr<BindingPattern const>> for_declaration = lhs;
    auto rule_start = push_start();

    auto has_annexB_for_in_init_extension = false;

    if (is<VariableDeclaration>(*lhs)) {
        auto& declaration = static_cast<VariableDeclaration const&>(*lhs);
        // Syntax errors for wrong amounts of declaration should have already been hit.
        if (!declaration.declarations().is_empty()) {
            // AnnexB extension B.3.5 Initializers in ForIn Statement Heads, https://tc39.es/ecma262/#sec-initializers-in-forin-statement-heads
            auto& variable = declaration.declarations().first();
            if (variable->init()) {
                if (m_state.strict_mode || declaration.declaration_kind() != DeclarationKind::Var || !variable->target().has<NonnullRefPtr<Identifier const>>())
                    syntax_error("Variable initializer not allowed in for..in/of"_string);
                else
                    has_annexB_for_in_init_extension = true;
            }
        }
    } else if (!lhs->is_identifier() && !is<MemberExpression>(*lhs) && !is<CallExpression>(*lhs) && !is<UsingDeclaration>(*lhs)) {
        bool valid = false;
        if (is<ObjectExpression>(*lhs) || is<ArrayExpression>(*lhs)) {
            auto synthesized_binding_pattern = synthesize_binding_pattern(static_cast<Expression const&>(*lhs));
            if (synthesized_binding_pattern) {
                for_declaration = synthesized_binding_pattern.release_nonnull();
                valid = true;
            }
        }
        if (!valid)
            syntax_error(MUST(String::formatted("Invalid left-hand side in for-loop ('{}')", lhs->class_name())));
    }
    auto in_or_of = consume();
    auto is_in = in_or_of.type() == TokenType::In;

    if (!is_in) {
        if (is<MemberExpression>(*lhs)) {
            auto& member = static_cast<MemberExpression const&>(*lhs);
            if (member.object().is_identifier() && static_cast<Identifier const&>(member.object()).string() == "let"sv)
                syntax_error("For of statement may not start with let."_string);
        }
        if (has_annexB_for_in_init_extension)
            syntax_error("Variable initializer not allowed in for..of"_string, rule_start.position());
    }

    auto rhs = parse_expression(is_in ? 0 : 2);
    consume(TokenType::ParenClose);

    TemporaryChange break_change(m_state.in_break_context, true);
    TemporaryChange continue_change(m_state.in_continue_context, true);

    auto body = parse_statement();
    if (is_in)
        return create_ast_node<ForInStatement>({ m_source_code, rule_start.position(), position() }, move(for_declaration), move(rhs), move(body));
    if (is_for_await_loop == IsForAwaitLoop::Yes)
        return create_ast_node<ForAwaitOfStatement>({ m_source_code, rule_start.position(), position() }, move(for_declaration), move(rhs), move(body));
    return create_ast_node<ForOfStatement>({ m_source_code, rule_start.position(), position() }, move(for_declaration), move(rhs), move(body));
}

NonnullRefPtr<DebuggerStatement const> Parser::parse_debugger_statement()
{
    auto rule_start = push_start();
    consume(TokenType::Debugger);
    consume_or_insert_semicolon();
    return create_ast_node<DebuggerStatement>({ m_source_code, rule_start.position(), position() });
}

bool Parser::match(TokenType type) const
{
    return m_state.current_token.type() == type;
}

bool Parser::match_expression() const
{
    auto type = m_state.current_token.type();
    if (type == TokenType::Import) {
        auto lookahead_token = next_token();
        return lookahead_token.type() == TokenType::Period || lookahead_token.type() == TokenType::ParenOpen;
    }

    return type == TokenType::BoolLiteral
        || type == TokenType::NumericLiteral
        || type == TokenType::BigIntLiteral
        || type == TokenType::StringLiteral
        || type == TokenType::TemplateLiteralStart
        || type == TokenType::NullLiteral
        || match_identifier()
        || type == TokenType::PrivateIdentifier
        || type == TokenType::Await
        || type == TokenType::New
        || type == TokenType::Class
        || type == TokenType::CurlyOpen
        || type == TokenType::BracketOpen
        || type == TokenType::ParenOpen
        || type == TokenType::Function
        || type == TokenType::Async
        || type == TokenType::This
        || type == TokenType::Super
        || type == TokenType::RegexLiteral
        || type == TokenType::Slash       // Wrongly recognized regex by lexer
        || type == TokenType::SlashEquals // Wrongly recognized regex by lexer (/=a/ is a valid regex)
        || type == TokenType::Yield
        || match_unary_prefixed_expression();
}

bool Parser::match_unary_prefixed_expression() const
{
    auto type = m_state.current_token.type();
    return type == TokenType::PlusPlus
        || type == TokenType::MinusMinus
        || type == TokenType::ExclamationMark
        || type == TokenType::Tilde
        || type == TokenType::Plus
        || type == TokenType::Minus
        || type == TokenType::Typeof
        || type == TokenType::Void
        || type == TokenType::Delete;
}

bool Parser::match_secondary_expression(ForbiddenTokens forbidden) const
{
    auto type = m_state.current_token.type();
    if (!forbidden.allows(type))
        return false;
    return type == TokenType::Plus
        || type == TokenType::PlusEquals
        || type == TokenType::Minus
        || type == TokenType::MinusEquals
        || type == TokenType::Asterisk
        || type == TokenType::AsteriskEquals
        || type == TokenType::Slash
        || type == TokenType::SlashEquals
        || type == TokenType::Percent
        || type == TokenType::PercentEquals
        || type == TokenType::DoubleAsterisk
        || type == TokenType::DoubleAsteriskEquals
        || type == TokenType::Equals
        || type == TokenType::EqualsEqualsEquals
        || type == TokenType::ExclamationMarkEqualsEquals
        || type == TokenType::EqualsEquals
        || type == TokenType::ExclamationMarkEquals
        || type == TokenType::GreaterThan
        || type == TokenType::GreaterThanEquals
        || type == TokenType::LessThan
        || type == TokenType::LessThanEquals
        || type == TokenType::ParenOpen
        || type == TokenType::Period
        || type == TokenType::BracketOpen
        || (type == TokenType::PlusPlus && !m_state.current_token.trivia_contains_line_terminator())
        || (type == TokenType::MinusMinus && !m_state.current_token.trivia_contains_line_terminator())
        || type == TokenType::In
        || type == TokenType::Instanceof
        || type == TokenType::QuestionMark
        || type == TokenType::Ampersand
        || type == TokenType::AmpersandEquals
        || type == TokenType::Pipe
        || type == TokenType::PipeEquals
        || type == TokenType::Caret
        || type == TokenType::CaretEquals
        || type == TokenType::ShiftLeft
        || type == TokenType::ShiftLeftEquals
        || type == TokenType::ShiftRight
        || type == TokenType::ShiftRightEquals
        || type == TokenType::UnsignedShiftRight
        || type == TokenType::UnsignedShiftRightEquals
        || type == TokenType::DoubleAmpersand
        || type == TokenType::DoubleAmpersandEquals
        || type == TokenType::DoublePipe
        || type == TokenType::DoublePipeEquals
        || type == TokenType::DoubleQuestionMark
        || type == TokenType::DoubleQuestionMarkEquals
        || type == TokenType::QuestionMarkPeriod;
}

bool Parser::match_statement() const
{
    auto type = m_state.current_token.type();
    return match_expression()
        || type == TokenType::Return
        || type == TokenType::Yield
        || type == TokenType::Do
        || type == TokenType::If
        || type == TokenType::Throw
        || type == TokenType::Try
        || type == TokenType::While
        || type == TokenType::With
        || type == TokenType::For
        || type == TokenType::CurlyOpen
        || type == TokenType::Switch
        || type == TokenType::Break
        || type == TokenType::Continue
        || type == TokenType::Var
        || type == TokenType::Debugger
        || type == TokenType::Semicolon;
}

bool Parser::match_export_or_import() const
{
    auto type = m_state.current_token.type();
    return type == TokenType::Export
        || type == TokenType::Import;
}

bool Parser::match_declaration(AllowUsingDeclaration allow_using) const
{
    auto type = m_state.current_token.type();

    if (type == TokenType::Let && !m_state.strict_mode) {
        return try_match_let_declaration();
    }

    if (type == TokenType::Async) {
        auto lookahead_token = next_token();
        return lookahead_token.type() == TokenType::Function && !lookahead_token.trivia_contains_line_terminator();
    }

    if (allow_using == AllowUsingDeclaration::Yes && type == TokenType::Identifier && m_state.current_token.original_value() == "using"sv)
        return try_match_using_declaration();

    return type == TokenType::Function
        || type == TokenType::Class
        || type == TokenType::Const
        || type == TokenType::Let;
}

Token Parser::next_token(size_t steps) const
{
    Lexer lookahead_lexer = m_state.lexer;

    Token lookahead_token;

    while (steps > 0) {
        lookahead_token = lookahead_lexer.next();
        steps--;
    }

    return lookahead_token;
}

bool Parser::try_match_let_declaration() const
{
    VERIFY(m_state.current_token.type() == TokenType::Let);
    auto token_after = next_token();

    if (token_after.is_identifier_name() && token_after.value() != "in"sv)
        return true;

    if (token_after.type() == TokenType::CurlyOpen || token_after.type() == TokenType::BracketOpen)
        return true;

    return false;
}

bool Parser::try_match_using_declaration() const
{
    VERIFY(m_state.current_token.type() == TokenType::Identifier);
    VERIFY(m_state.current_token.original_value() == "using"sv);

    auto token_after = next_token();
    if (token_after.trivia_contains_line_terminator())
        return false;

    return token_after.is_identifier_name();
}

bool Parser::match_variable_declaration() const
{
    auto type = m_state.current_token.type();

    if (type == TokenType::Let && !m_state.strict_mode) {
        return try_match_let_declaration();
    }

    return type == TokenType::Var
        || type == TokenType::Let
        || type == TokenType::Const;
}

bool Parser::match_identifier() const
{
    return token_is_identifier(m_state.current_token);
}

bool Parser::token_is_identifier(Token const& token) const
{
    if (token.type() == TokenType::EscapedKeyword) {
        if (token.value() == "let"sv)
            return !m_state.strict_mode;
        if (token.value() == "yield"sv)
            return !m_state.strict_mode && !m_state.in_generator_function_context;
        if (token.value() == "await"sv)
            return m_program_type != Program::Type::Module && !m_state.await_expression_is_valid && !m_state.in_class_static_init_block;
        return true;
    }

    return token.type() == TokenType::Identifier
        || token.type() == TokenType::Async
        || (token.type() == TokenType::Let && !m_state.strict_mode)
        || (token.type() == TokenType::Await && m_program_type != Program::Type::Module && !m_state.await_expression_is_valid && !m_state.in_class_static_init_block)
        || (token.type() == TokenType::Yield && !m_state.in_generator_function_context && !m_state.strict_mode); // See note in Parser::parse_identifier().
}

bool Parser::match_identifier_name() const
{
    return m_state.current_token.is_identifier_name();
}

bool Parser::match_property_key() const
{
    auto type = m_state.current_token.type();
    return match_identifier_name()
        || type == TokenType::BracketOpen
        || type == TokenType::StringLiteral
        || type == TokenType::NumericLiteral
        || type == TokenType::BigIntLiteral;
}

bool Parser::done() const
{
    return match(TokenType::Eof);
}

Token Parser::consume()
{
    auto old_token = m_state.current_token;
    m_state.current_token = m_state.lexer.next();

    // If an IdentifierName is not parsed as an Identifier a slash after it should not be a division
    if (old_token.is_identifier_name() && (m_state.current_token.type() == TokenType::Slash || m_state.current_token.type() == TokenType::SlashEquals)) {
        m_state.current_token = m_state.lexer.force_slash_as_regex();
    }
    m_state.previous_token_was_period = old_token.type() == TokenType::Period;
    return old_token;
}

Token Parser::consume_and_allow_division()
{
    auto old_token = m_state.current_token;
    m_state.current_token = m_state.lexer.next();

    // NOTE: This is the bare minimum needed to decide whether we might need an `arguments` object
    //       in a function expression or declaration. ("Might" because ESFO implements some further
    //       conditions from the spec that rule out the need for allocating one.)
    //       Basically any freestanding use of `arguments` in a function body. This is not perfect
    //       but avoids a lot of unnecessary arguments objects. We check if the previous token was
    //       a period to avoid creating `arguments` due to an unrelated property access (`o.arguments`)
    if (old_token.type() == TokenType::Identifier && old_token.value().is_one_of("arguments"sv, "eval"sv) && !m_state.previous_token_was_period)
        m_state.function_might_need_arguments_object = true;

    m_state.previous_token_was_period = old_token.type() == TokenType::Period;
    return old_token;
}

void Parser::consume_or_insert_semicolon()
{
    // Semicolon was found and will be consumed
    if (match(TokenType::Semicolon)) {
        consume();
        return;
    }
    // Insert semicolon if...
    // ...token is preceded by one or more newlines
    if (m_state.current_token.trivia_contains_line_terminator())
        return;
    // ...token is a closing curly brace
    if (match(TokenType::CurlyClose))
        return;
    // ...token is eof
    if (match(TokenType::Eof))
        return;

    // No rule for semicolon insertion applies -> syntax error
    expected("Semicolon");
}

Token Parser::consume_identifier()
{
    if (match(TokenType::Identifier))
        return consume(TokenType::Identifier);

    if (match(TokenType::EscapedKeyword))
        return consume(TokenType::EscapedKeyword);

    // Note that 'let' is not a reserved keyword, but our lexer considers it such
    // As it's pretty nice to have that (for syntax highlighting and such), we'll
    // special-case it here instead.
    if (match(TokenType::Let)) {
        if (m_state.strict_mode)
            syntax_error("'let' is not allowed as an identifier in strict mode"_string);
        return consume_and_allow_division();
    }

    if (match(TokenType::Yield)) {
        if (m_state.strict_mode || m_state.in_generator_function_context)
            syntax_error("Identifier must not be a reserved word in strict mode ('yield')"_string);
        return consume_and_allow_division();
    }

    if (match(TokenType::Await)) {
        if (m_program_type == Program::Type::Module || m_state.await_expression_is_valid || m_state.in_class_static_init_block)
            syntax_error("Identifier must not be a reserved word in modules ('await')"_string);
        return consume_and_allow_division();
    }

    if (match(TokenType::Async))
        return consume_and_allow_division();

    expected("Identifier");
    return consume_and_allow_division();
}

// https://tc39.es/ecma262/#prod-IdentifierReference
Token Parser::consume_identifier_reference()
{
    if (match(TokenType::Identifier))
        return consume(TokenType::Identifier);

    if (match(TokenType::EscapedKeyword)) {
        auto name = m_state.current_token.value();
        if (m_state.strict_mode && (name == "let"sv || name == "yield"sv))
            syntax_error(MUST(String::formatted("'{}' is not allowed as an identifier in strict mode", name)));
        if (m_program_type == Program::Type::Module && name == "await"sv)
            syntax_error("'await' is not allowed as an identifier in module"_string);

        return consume_and_allow_division();
    }

    // See note in Parser::parse_identifier().
    if (match(TokenType::Let)) {
        if (m_state.strict_mode)
            syntax_error("'let' is not allowed as an identifier in strict mode"_string);
        return consume_and_allow_division();
    }

    if (match(TokenType::Yield)) {
        if (m_state.strict_mode)
            syntax_error("Identifier reference may not be 'yield' in strict mode"_string);
        return consume_and_allow_division();
    }

    if (match(TokenType::Await)) {
        if (m_program_type == Program::Type::Module)
            syntax_error("'await' is not allowed as an identifier in module"_string);
        return consume_and_allow_division();
    }

    if (match(TokenType::Async))
        return consume_and_allow_division();

    expected(Token::name(TokenType::Identifier));
    return consume_and_allow_division();
}

Token Parser::consume(TokenType expected_type)
{
    if (!match(expected_type)) {
        expected(Token::name(expected_type));
    }
    auto token = expected_type == TokenType::Identifier ? consume_and_allow_division() : consume();
    if (expected_type == TokenType::Identifier) {
        if (m_state.strict_mode && is_strict_reserved_word(token.value()))
            syntax_error(MUST(String::formatted("Identifier must not be a reserved word in strict mode ('{}')", token.value())));
    }
    return token;
}

Token Parser::consume_and_validate_numeric_literal()
{
    auto is_unprefixed_octal_number = [](StringView value) {
        return value.length() > 1 && value[0] == '0' && is_ascii_digit(value[1]);
    };
    auto literal_start = position();
    auto token = consume(TokenType::NumericLiteral);
    if (m_state.strict_mode && is_unprefixed_octal_number(token.value()))
        syntax_error("Unprefixed octal number not allowed in strict mode"_string, literal_start);
    if (match_identifier_name() && m_state.current_token.trivia().is_empty())
        syntax_error("Numeric literal must not be immediately followed by identifier"_string);
    return token;
}

void Parser::expected(char const* what)
{
    auto message = MUST(String::from_utf8(m_state.current_token.message()));
    if (message.is_empty())
        message = MUST(String::formatted("Unexpected token {}. Expected {}", m_state.current_token.name(), what));
    syntax_error(message);
}

Position Parser::position() const
{
    return {
        m_state.current_token.line_number(),
        m_state.current_token.line_column(),
        m_state.current_token.offset(),
    };
}

bool Parser::try_parse_arrow_function_expression_failed_at_position(Position const& position) const
{
    auto it = m_token_memoizations.find(position.offset);
    if (it == m_token_memoizations.end())
        return false;

    return (*it).value.try_parse_arrow_function_expression_failed;
}

void Parser::set_try_parse_arrow_function_expression_failed_at_position(Position const& position, bool failed)
{
    m_token_memoizations.set(position.offset, { failed });
}

void Parser::syntax_error(String const& message, Optional<Position> position)
{
    if (!position.has_value())
        position = this->position();
    m_state.errors.append({ message, position });
}

void Parser::save_state()
{
    m_saved_state.append(m_state);
}

void Parser::load_state()
{
    VERIFY(!m_saved_state.is_empty());
    m_state = m_saved_state.take_last();
}

void Parser::discard_saved_state()
{
    m_saved_state.take_last();
}

void Parser::check_identifier_name_for_assignment_validity(FlyString const& name, bool force_strict)
{
    // FIXME: this is now called from multiple places maybe the error message should be dynamic?
    if (any_of(s_reserved_words, [&](auto& value) { return name == value; })) {
        syntax_error("Binding pattern target may not be a reserved word"_string);
    } else if (m_state.strict_mode || force_strict) {
        if (name.is_one_of("arguments"sv, "eval"sv))
            syntax_error("Binding pattern target may not be called 'arguments' or 'eval' in strict mode"_string);
        else if (is_strict_reserved_word(name))
            syntax_error(MUST(String::formatted("Binding pattern target may not be called '{}' in strict mode", name)));
    }
}

FlyString Parser::consume_string_value()
{
    VERIFY(match(TokenType::StringLiteral));
    auto string_token = consume();
    FlyString value = parse_string_literal(string_token)->value();

    // This also checks IsStringWellFormedUnicode which makes sure there is no unpaired surrogate
    // Surrogates are at least 3 bytes
    if (value.bytes().size() < 3)
        return value;

    Utf8View view { value.bytes_as_string_view().substring_view(value.bytes().size() - 3) };
    VERIFY(view.length() <= 3);
    auto codepoint = *view.begin();
    if (Utf16View::is_high_surrogate(codepoint)) {
        syntax_error("StringValue ending with unpaired high surrogate"_string);
        VERIFY(view.length() == 1);
    }

    return value;
}

// WithClause, https://tc39.es/proposal-import-attributes/#prod-WithClause
ModuleRequest Parser::parse_module_request()
{
    // Does not include the 'from' since that is not always required.

    if (!match(TokenType::StringLiteral)) {
        expected("ModuleSpecifier (string)");
        return ModuleRequest { "!!invalid!!"_fly_string };
    }

    ModuleRequest request { consume_string_value() };

    if (!match(TokenType::With))
        return request;

    consume(TokenType::With);
    consume(TokenType::CurlyOpen);

    while (!done() && !match(TokenType::CurlyClose)) {
        String key;
        if (match(TokenType::StringLiteral)) {
            key = parse_string_literal(m_state.current_token)->value();
            consume();
        } else if (match_identifier_name()) {
            key = consume().fly_string_value().to_string();
        } else {
            expected("IdentifierName or StringValue as WithKey");
            consume();
        }

        consume(TokenType::Colon);

        if (match(TokenType::StringLiteral)) {
            for (auto& entries : request.attributes) {
                if (entries.key == key)
                    syntax_error(MUST(String::formatted("Duplicate attribute clauses with name: {}", key)));
            }
            request.add_attribute(move(key), parse_string_literal(m_state.current_token)->value());
        }
        consume(TokenType::StringLiteral);

        if (match(TokenType::Comma))
            consume(TokenType::Comma);
        else
            break;
    }

    consume(TokenType::CurlyClose);

    return request;
}

static FlyString default_string_value = "default"_fly_string;

NonnullRefPtr<ImportStatement const> Parser::parse_import_statement(Program& program)
{
    // We use the extended syntax which adds:
    //     ImportDeclaration:
    //         import ImportClause FromClause WithClause[opt] ;
    //         import ModuleSpecifier WithClause[opt] ;
    // From: https://tc39.es/proposal-import-attributes/#prod-ImportDeclaration

    auto rule_start = push_start();
    if (program.type() != Program::Type::Module)
        syntax_error("Cannot use import statement outside a module"_string);

    consume(TokenType::Import);

    if (match(TokenType::StringLiteral)) {
        //  import ModuleSpecifier ;
        auto module_request = parse_module_request();
        return create_ast_node<ImportStatement>({ m_source_code, rule_start.position(), position() }, move(module_request));
    }

    auto match_imported_binding = [&] {
        return match_identifier() || match(TokenType::Yield) || match(TokenType::Await);
    };

    auto match_as = [&] {
        return match(TokenType::Identifier) && m_state.current_token.original_value() == "as"sv;
    };

    bool continue_parsing = true;

    struct ImportWithLocation {
        ImportEntry entry;
        Position position;
    };

    Vector<ImportWithLocation> entries_with_location;

    // import ImportClause FromClause ;
    //  ImportClause :
    //      ImportedDefaultBinding
    //      NameSpaceImport
    //      NamedImports
    //      ImportedDefaultBinding , NameSpaceImport
    //      ImportedDefaultBinding , NamedImports

    if (match_imported_binding()) {
        //  ImportedDefaultBinding : ImportedBinding
        auto id_position = position();
        auto bound_name = consume().fly_string_value();
        entries_with_location.append({ { default_string_value, bound_name }, id_position });

        if (match(TokenType::Comma)) {
            consume(TokenType::Comma);
        } else {
            continue_parsing = false;
        }
    }

    if (!continue_parsing) {
        // skip the rest
    } else if (match(TokenType::Asterisk)) {
        //  NameSpaceImport : * as ImportedBinding
        consume(TokenType::Asterisk);

        if (!match_as())
            syntax_error(MUST(String::formatted("Unexpected token: {}", m_state.current_token.name())));

        consume(TokenType::Identifier);

        if (match_imported_binding()) {
            auto namespace_position = position();
            auto namespace_name = consume().fly_string_value();
            entries_with_location.append({ ImportEntry({}, namespace_name), namespace_position });
        } else {
            syntax_error(MUST(String::formatted("Unexpected token: {}", m_state.current_token.name())));
        }

    } else if (match(TokenType::CurlyOpen)) {
        // NamedImports :
        //     { ImportSpecifier ,_opt } (repeated any amount of times)

        consume(TokenType::CurlyOpen);
        while (!done() && !match(TokenType::CurlyClose)) {
            if (match_identifier_name()) {
                // ImportSpecifier :  ImportedBinding
                auto require_as = !match_imported_binding();
                auto name_position = position();
                auto name = consume().fly_string_value();

                if (match_as()) {
                    consume(TokenType::Identifier);

                    auto alias_position = position();
                    auto alias = consume_identifier().fly_string_value();
                    check_identifier_name_for_assignment_validity(alias);

                    entries_with_location.append({ { name, alias }, alias_position });
                } else if (require_as) {
                    syntax_error(MUST(String::formatted("Unexpected reserved word '{}'", name)));
                } else {
                    check_identifier_name_for_assignment_validity(name);

                    entries_with_location.append({ { name, name }, name_position });
                }
            } else if (match(TokenType::StringLiteral)) {
                // ImportSpecifier : ModuleExportName as ImportedBinding
                auto name = consume_string_value();

                if (!match_as())
                    expected("as");

                consume(TokenType::Identifier);

                auto alias_position = position();
                auto alias = consume_identifier().fly_string_value();
                check_identifier_name_for_assignment_validity(alias);

                entries_with_location.append({ { move(name), alias }, alias_position });
            } else {
                expected("identifier");
                break;
            }

            if (!match(TokenType::Comma))
                break;

            consume(TokenType::Comma);
        }

        consume(TokenType::CurlyClose);
    } else {
        expected("import clauses");
    }

    auto from_statement = consume(TokenType::Identifier).original_value();
    if (from_statement != "from"sv)
        syntax_error(MUST(String::formatted("Expected 'from' got {}", from_statement)));

    auto module_request = parse_module_request();

    Vector<ImportEntry> entries;
    entries.ensure_capacity(entries_with_location.size());

    for (auto& entry : entries_with_location) {
        for (auto& import_statement : program.imports()) {
            if (import_statement->has_bound_name(entry.entry.local_name))
                syntax_error(MUST(String::formatted("Identifier '{}' already declared", entry.entry.local_name)), entry.position);
        }

        for (auto& new_entry : entries) {
            if (new_entry.local_name == entry.entry.local_name)
                syntax_error(MUST(String::formatted("Identifier '{}' already declared", entry.entry.local_name)), entry.position);
        }

        entries.append(move(entry.entry));
    }

    return create_ast_node<ImportStatement>({ m_source_code, rule_start.position(), position() }, move(module_request), move(entries));
}

NonnullRefPtr<ExportStatement const> Parser::parse_export_statement(Program& program)
{
    // We use the extended syntax which adds:
    //  ExportDeclaration:
    //      export ExportFromClause FromClause [no LineTerminator here] WithClause ;
    // From:  https://tc39.es/proposal-import-attributes/#sec-exports

    auto rule_start = push_start();
    if (program.type() != Program::Type::Module)
        syntax_error("Cannot use export statement outside a module"_string);

    auto match_as = [&] {
        return match(TokenType::Identifier) && m_state.current_token.original_value() == "as"sv;
    };

    auto match_from = [&] {
        return match(TokenType::Identifier) && m_state.current_token.original_value() == "from"sv;
    };

    auto match_default = [&] {
        return match(TokenType::Default) && m_state.current_token.original_value() == "default"sv;
    };

    consume(TokenType::Export);

    struct EntryAndLocation {
        ExportEntry entry;
        Position position;
    };

    Vector<EntryAndLocation> entries_with_location;

    RefPtr<ASTNode const> expression = {};
    bool is_default = false;
    Optional<ModuleRequest> from_specifier;

    if (match_default()) {
        is_default = true;
        auto default_position = position();
        consume(TokenType::Default);

        Optional<FlyString> local_name;

        auto lookahead_token = next_token();

        enum class MatchesFunctionDeclaration {
            Yes,
            No,
            WithoutName,
        };

        // Note: For some reason the spec here has declaration which can have no name
        //       and the rest of the parser is just not setup for that. With these
        //       hacks below we get through most things but we should probably figure
        //       out a better solution. I have attempted to explain why/what these "hacks" do below.
        //       The summary is treat named declarations just as declarations and hack around unnamed
        //       declarations with expression see also SourceTextModule::initialize_environment.
        //       As far as I'm aware the only problem (which is a tricky one) is:
        //          `export default function() {}()`
        //       Since we parse this as an expression you are immediately allowed to call it
        //       which is incorrect and this should give a SyntaxError.

        auto has_name = [&](Token const& token) {
            if (token.type() != TokenType::ParenOpen)
                return MatchesFunctionDeclaration::Yes;

            return MatchesFunctionDeclaration::WithoutName;
        };

        auto match_function_declaration = [&] {
            // Hack part 1.
            // Match a function declaration with a name, since we have async and generator
            // and asyncgenerator variants this is quite complicated.
            auto current_type = m_state.current_token.type();
            Lexer lookahead_lexer = m_state.lexer;
            lookahead_lexer.next();

            if (current_type == TokenType::Function) {
                if (lookahead_token.type() == TokenType::Asterisk)
                    return has_name(lookahead_lexer.next()); // function * [name]
                else
                    return has_name(lookahead_token); // function [name]
            }

            if (current_type == TokenType::Async) {
                if (lookahead_token.type() != TokenType::Function)
                    return MatchesFunctionDeclaration::No;

                if (lookahead_token.trivia_contains_line_terminator())
                    return MatchesFunctionDeclaration::No;

                auto lookahead_two_token = lookahead_lexer.next();
                if (lookahead_two_token.type() == TokenType::Asterisk)
                    return has_name(lookahead_lexer.next()); // async function * [name]
                else
                    return has_name(lookahead_two_token); // async function [name]
            }

            return MatchesFunctionDeclaration::No;
        };

        if (auto matches_function = match_function_declaration(); matches_function != MatchesFunctionDeclaration::No) {

            auto function_declaration = parse_function_node<FunctionDeclaration>(
                (matches_function == MatchesFunctionDeclaration::WithoutName ? FunctionNodeParseOptions::HasDefaultExportName : 0)
                | FunctionNodeParseOptions::CheckForFunctionAndName);

            m_state.current_scope_pusher->add_declaration(function_declaration);
            if (matches_function == MatchesFunctionDeclaration::WithoutName)
                local_name = ExportStatement::local_name_for_default;
            else
                local_name = function_declaration->name();

            expression = move(function_declaration);
        } else if (match(TokenType::Class) && lookahead_token.type() != TokenType::CurlyOpen && lookahead_token.type() != TokenType::Extends) {
            // Hack part 2.
            // Attempt to detect classes with names only as those are declarations,
            //   this actually seems to cover all cases already.
            auto class_expression = parse_class_declaration();
            m_state.current_scope_pusher->add_declaration(class_expression);
            local_name = class_expression->name();
            expression = move(class_expression);

        } else if (match_expression()) {
            // Hack part 3.
            // Even though the unnamed declarations look like expression we should
            //   not treat them as such and thus not consume a semicolon after them.

            bool special_case_declaration_without_name = match(TokenType::Class) || match(TokenType::Function) || (match(TokenType::Async) && lookahead_token.type() == TokenType::Function && !lookahead_token.trivia_contains_line_terminator());
            expression = parse_expression(2);

            if (!special_case_declaration_without_name)
                consume_or_insert_semicolon();

            if (is<ClassExpression>(*expression)) {
                auto const& class_expression = static_cast<ClassExpression const&>(*expression);
                if (class_expression.has_name())
                    local_name = class_expression.name();
            }
        } else {
            expected("Declaration or assignment expression");
            local_name = "!!invalid!!"_fly_string;
        }

        if (!local_name.has_value())
            local_name = ExportStatement::local_name_for_default;

        entries_with_location.append({ ExportEntry::named_export(default_string_value, local_name.release_value()), default_position });
    } else {
        enum class FromSpecifier {
            NotAllowed,
            Optional,
            Required
        } check_for_from { FromSpecifier::NotAllowed };

        auto parse_module_export_name = [&](bool lhs) -> FlyString {
            // https://tc39.es/ecma262/#prod-ModuleExportName
            //  ModuleExportName :
            //      IdentifierName
            //      StringLiteral
            if (match_identifier_name()) {
                return consume().fly_string_value();
            }
            if (match(TokenType::StringLiteral)) {
                // It is a Syntax Error if ReferencedBindings of NamedExports contains any StringLiterals.
                // Only for export { "a" as "b" }; // <-- no from
                if (lhs)
                    check_for_from = FromSpecifier::Required;
                return consume_string_value();
            }
            expected("ExportSpecifier (string or identifier)");
            return {};
        };

        if (match(TokenType::Asterisk)) {
            auto asterisk_position = position();
            consume(TokenType::Asterisk);

            if (match_as()) {
                //  * as ModuleExportName
                consume(TokenType::Identifier);
                auto namespace_position = position();
                auto exported_name = parse_module_export_name(false);
                entries_with_location.append({ ExportEntry::all_module_request(exported_name), namespace_position });
            } else {
                entries_with_location.append({ ExportEntry::all_but_default_entry(), asterisk_position });
            }
            check_for_from = FromSpecifier::Required;
        } else if (match_declaration()) {
            auto decl_position = position();
            auto declaration = parse_declaration();
            m_state.current_scope_pusher->add_declaration(declaration);
            if (is<FunctionDeclaration>(*declaration)) {
                auto& func = static_cast<FunctionDeclaration const&>(*declaration);
                entries_with_location.append({ ExportEntry::named_export(func.name(), func.name()), func.source_range().start });
            } else if (is<ClassDeclaration>(*declaration)) {
                auto& class_declaration = static_cast<ClassDeclaration const&>(*declaration);
                entries_with_location.append({ ExportEntry::named_export(class_declaration.name(), class_declaration.name()), class_declaration.source_range().start });
            } else {
                VERIFY(is<VariableDeclaration>(*declaration));
                auto& variables = static_cast<VariableDeclaration const&>(*declaration);
                VERIFY(variables.is_lexical_declaration());
                for (auto& decl : variables.declarations()) {
                    decl->target().visit(
                        [&](NonnullRefPtr<Identifier const> const& identifier) {
                            entries_with_location.append({ ExportEntry::named_export(identifier->string(), identifier->string()), identifier->source_range().start });
                        },
                        [&](NonnullRefPtr<BindingPattern const> const& binding) {
                            // NOTE: Nothing in the callback throws an exception.
                            MUST(binding->for_each_bound_identifier([&](auto& identifier) {
                                auto const& name = identifier.string();
                                entries_with_location.append({ ExportEntry::named_export(name, name), decl_position });
                            }));
                        });
                }
            }
            expression = declaration;
        } else if (match(TokenType::Var)) {
            auto variable_position = position();
            auto variable_declaration = parse_variable_declaration();
            m_state.current_scope_pusher->add_declaration(variable_declaration);
            for (auto& decl : variable_declaration->declarations()) {
                decl->target().visit(
                    [&](NonnullRefPtr<Identifier const> const& identifier) {
                        entries_with_location.append({ ExportEntry::named_export(identifier->string(), identifier->string()), identifier->source_range().start });
                    },
                    [&](NonnullRefPtr<BindingPattern const> const& binding) {
                        // NOTE: Nothing in the callback throws an exception.
                        MUST(binding->for_each_bound_identifier(
                            [&](auto& identifier) {
                                auto const& name = identifier.string();
                                entries_with_location.append({ ExportEntry::named_export(name, name), variable_position });
                            }));
                    });
            }
            expression = variable_declaration;
        } else if (match(TokenType::CurlyOpen)) {
            consume(TokenType::CurlyOpen);
            check_for_from = FromSpecifier::Optional;

            // FIXME: Even when empty should add module to requiredModules!
            while (!done() && !match(TokenType::CurlyClose)) {
                auto identifier_position = position();
                auto identifier = parse_module_export_name(true);

                if (match_as()) {
                    consume(TokenType::Identifier);
                    auto export_name = parse_module_export_name(false);

                    entries_with_location.append({ ExportEntry::named_export(move(export_name), move(identifier)), identifier_position });
                } else {
                    entries_with_location.append({ ExportEntry::named_export(identifier, identifier), identifier_position });
                }

                if (!match(TokenType::Comma))
                    break;

                consume(TokenType::Comma);
            }

            if (entries_with_location.is_empty()) {
                // export {} from "module"; Since this will never be a
                // duplicate we can give a slightly wrong location.
                entries_with_location.append({ ExportEntry::empty_named_export(), position() });
            }

            consume(TokenType::CurlyClose);

        } else {
            syntax_error("Unexpected token 'export'"_string, rule_start.position());
        }

        if (check_for_from != FromSpecifier::NotAllowed && match_from()) {
            consume(TokenType::Identifier);
            from_specifier = parse_module_request();
        } else if (check_for_from == FromSpecifier::Required) {
            expected("from");
        }

        if (check_for_from != FromSpecifier::NotAllowed)
            consume_or_insert_semicolon();
    }

    Vector<ExportEntry> entries;
    entries.ensure_capacity(entries_with_location.size());

    for (auto& entry : entries_with_location) {
        for (auto& export_statement : program.exports()) {
            if (export_statement->has_export(entry.entry.export_name.value_or(""_fly_string)))
                syntax_error(MUST(String::formatted("Duplicate export with name: '{}'", entry.entry.export_name)), entry.position);
        }

        for (auto& new_entry : entries) {
            if (new_entry.kind != ExportEntry::Kind::EmptyNamedExport && new_entry.export_name == entry.entry.export_name)
                syntax_error(MUST(String::formatted("Duplicate export with name: '{}'", entry.entry.export_name)), entry.position);
        }

        entries.append(move(entry.entry));
    }

    return create_ast_node<ExportStatement>({ m_source_code, rule_start.position(), position() }, move(expression), move(entries), is_default, move(from_specifier));
}

Parser::ForbiddenTokens::ForbiddenTokens(std::initializer_list<TokenType> const& forbidden)
{
    forbid_tokens(forbidden);
}

void Parser::ForbiddenTokens::forbid_tokens(std::initializer_list<TokenType> const& forbidden)
{
    for (auto token : forbidden) {
        switch (token) {
        case TokenType::In:
            m_forbid_in_token = true;
            break;
        case TokenType::DoubleAmpersand:
        case TokenType::DoublePipe:
            m_forbid_logical_tokens = true;
            break;
        case TokenType::DoubleQuestionMark:
            m_forbid_coalesce_token = true;
            break;
        case TokenType::QuestionMarkPeriod:
            m_forbid_question_mark_period = true;
            break;
        case TokenType::ParenOpen:
            m_forbid_paren_open = true;
            break;
        case TokenType::Equals:
            m_forbid_equals = true;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
}

bool Parser::ForbiddenTokens::allows(TokenType token) const
{
    switch (token) {
    case TokenType::In:
        return !m_forbid_in_token;
    case TokenType::DoubleAmpersand:
    case TokenType::DoublePipe:
        return !m_forbid_logical_tokens;
    case TokenType::DoubleQuestionMark:
        return !m_forbid_coalesce_token;
    case TokenType::QuestionMarkPeriod:
        return !m_forbid_question_mark_period;
    case TokenType::ParenOpen:
        return !m_forbid_paren_open;
    case TokenType::Equals:
        return !m_forbid_equals;
    default:
        return true;
    }
}

Parser::ForbiddenTokens Parser::ForbiddenTokens::merge(ForbiddenTokens other) const
{
    ForbiddenTokens result = *this;
    result.m_forbid_in_token |= other.m_forbid_in_token;
    result.m_forbid_logical_tokens |= other.m_forbid_logical_tokens;
    result.m_forbid_coalesce_token |= other.m_forbid_coalesce_token;
    result.m_forbid_paren_open |= other.m_forbid_paren_open;
    result.m_forbid_question_mark_period |= other.m_forbid_question_mark_period;
    result.m_forbid_equals |= other.m_forbid_equals;
    return result;
}

Parser::ForbiddenTokens Parser::ForbiddenTokens::forbid(std::initializer_list<TokenType> const& forbidden) const
{
    ForbiddenTokens result = *this;
    result.forbid_tokens(forbidden);
    return result;
}

template NonnullRefPtr<FunctionExpression> Parser::parse_function_node(u16, Optional<Position> const&);
template NonnullRefPtr<FunctionDeclaration> Parser::parse_function_node(u16, Optional<Position> const&);

NonnullRefPtr<Identifier const> Parser::create_identifier_and_register_in_current_scope(SourceRange range, FlyString string, Optional<DeclarationKind> declaration_kind)
{
    auto id = create_ast_node<Identifier const>(range, string);
    if (m_state.current_scope_pusher)
        m_state.current_scope_pusher->register_identifier(const_cast<Identifier&>(*id), declaration_kind);
    return id;
}

Parser Parser::parse_function_body_from_string(ByteString const& body_string, u16 parse_options, NonnullRefPtr<FunctionParameters const> parameters, FunctionKind kind, FunctionParsingInsights& parsing_insights)
{
    RefPtr<FunctionBody const> function_body;

    auto body_parser = Parser { Lexer { body_string } };
    {
        // Set up some parser state to accept things like return await, and yield in the plain function body.
        body_parser.m_state.in_function_context = true;
        auto function_scope = ScopePusher::function_scope(body_parser);
        if ((parse_options & FunctionNodeParseOptions::IsAsyncFunction) != 0)
            body_parser.m_state.await_expression_is_valid = true;
        if ((parse_options & FunctionNodeParseOptions::IsGeneratorFunction) != 0)
            body_parser.m_state.in_generator_function_context = true;
        function_body = body_parser.parse_function_body(move(parameters), kind, parsing_insights);
    }

    return body_parser;
}

}
