/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StringBuilder.h>
#include <LibJS/AST.h>
#include <LibJS/Lexer.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/FunctionConstructor.h>
#include <LibJS/SourceRange.h>
#include <LibJS/Token.h>
#include <initializer_list>
#include <stdio.h>

namespace JS {

enum class Associativity {
    Left,
    Right
};

struct FunctionNodeParseOptions {
    enum : u16 {
        CheckForFunctionAndName = 1 << 0,
        AllowSuperPropertyLookup = 1 << 1,
        AllowSuperConstructorCall = 1 << 2,
        IsGetterFunction = 1 << 3,
        IsSetterFunction = 1 << 4,
        IsArrowFunction = 1 << 5,
        IsGeneratorFunction = 1 << 6,
        IsAsyncFunction = 1 << 7,
        HasDefaultExportName = 1 << 8,
    };
};

class ScopePusher;

class Parser {
public:
    struct EvalInitialState {
        bool in_eval_function_context { false };
        bool allow_super_property_lookup { false };
        bool allow_super_constructor_call { false };
        bool in_class_field_initializer { false };
    };

    explicit Parser(Lexer lexer, Program::Type program_type = Program::Type::Script, Optional<EvalInitialState> initial_state_for_eval = {});

    NonnullRefPtr<Program> parse_program(bool starts_in_strict_mode = false);

    template<typename FunctionNodeType>
    NonnullRefPtr<FunctionNodeType> parse_function_node(u16 parse_options = FunctionNodeParseOptions::CheckForFunctionAndName, Optional<Position> const& function_start = {});
    NonnullRefPtr<FunctionParameters const> parse_formal_parameters(int& function_length, u16 parse_options = 0);

    enum class AllowDuplicates {
        Yes,
        No
    };

    enum class AllowMemberExpressions {
        Yes,
        No
    };

    RefPtr<BindingPattern const> parse_binding_pattern(AllowDuplicates is_var_declaration = AllowDuplicates::No, AllowMemberExpressions allow_member_expressions = AllowMemberExpressions::No);

    struct PrimaryExpressionParseResult {
        NonnullRefPtr<Expression const> result;
        bool should_continue_parsing_as_expression { true };
    };

    NonnullRefPtr<Declaration const> parse_declaration();

    enum class AllowLabelledFunction {
        No,
        Yes
    };

    NonnullRefPtr<Statement const> parse_statement(AllowLabelledFunction allow_labelled_function = AllowLabelledFunction::No);
    NonnullRefPtr<BlockStatement const> parse_block_statement();
    NonnullRefPtr<FunctionBody const> parse_function_body(NonnullRefPtr<FunctionParameters const>, FunctionKind function_kind, FunctionParsingInsights&);
    NonnullRefPtr<ReturnStatement const> parse_return_statement();

    enum class IsForLoopVariableDeclaration {
        No,
        Yes
    };

    NonnullRefPtr<VariableDeclaration const> parse_variable_declaration(IsForLoopVariableDeclaration is_for_loop_variable_declaration = IsForLoopVariableDeclaration::No);
    [[nodiscard]] RefPtr<Identifier const> parse_lexical_binding(Optional<DeclarationKind> = {});
    NonnullRefPtr<UsingDeclaration const> parse_using_declaration(IsForLoopVariableDeclaration is_for_loop_variable_declaration = IsForLoopVariableDeclaration::No);
    NonnullRefPtr<Statement const> parse_for_statement();

    enum class IsForAwaitLoop {
        No,
        Yes
    };

    struct ForbiddenTokens {
        ForbiddenTokens(std::initializer_list<TokenType> const& forbidden);
        ForbiddenTokens merge(ForbiddenTokens other) const;
        bool allows(TokenType token) const;
        ForbiddenTokens forbid(std::initializer_list<TokenType> const& forbidden) const;

    private:
        void forbid_tokens(std::initializer_list<TokenType> const& forbidden);
        bool m_forbid_in_token : 1 { false };
        bool m_forbid_logical_tokens : 1 { false };
        bool m_forbid_coalesce_token : 1 { false };
        bool m_forbid_paren_open : 1 { false };
        bool m_forbid_question_mark_period : 1 { false };
        bool m_forbid_equals : 1 { false };
    };

    struct ExpressionResult {
        template<typename T>
        ExpressionResult(NonnullRefPtr<T const> expression, ForbiddenTokens forbidden = {})
            : expression(move(expression))
            , forbidden(forbidden)
        {
        }

        template<typename T>
        ExpressionResult(NonnullRefPtr<T> expression, ForbiddenTokens forbidden = {})
            : expression(move(expression))
            , forbidden(forbidden)
        {
        }
        NonnullRefPtr<Expression const> expression;
        ForbiddenTokens forbidden;
    };

    NonnullRefPtr<Statement const> parse_for_in_of_statement(NonnullRefPtr<ASTNode const> lhs, IsForAwaitLoop is_await);
    NonnullRefPtr<IfStatement const> parse_if_statement();
    NonnullRefPtr<ThrowStatement const> parse_throw_statement();
    NonnullRefPtr<TryStatement const> parse_try_statement();
    NonnullRefPtr<CatchClause const> parse_catch_clause();
    NonnullRefPtr<SwitchStatement const> parse_switch_statement();
    NonnullRefPtr<SwitchCase const> parse_switch_case();
    NonnullRefPtr<BreakStatement const> parse_break_statement();
    NonnullRefPtr<ContinueStatement const> parse_continue_statement();
    NonnullRefPtr<DoWhileStatement const> parse_do_while_statement();
    NonnullRefPtr<WhileStatement const> parse_while_statement();
    NonnullRefPtr<WithStatement const> parse_with_statement();
    NonnullRefPtr<DebuggerStatement const> parse_debugger_statement();
    NonnullRefPtr<ConditionalExpression const> parse_conditional_expression(NonnullRefPtr<Expression const> test, ForbiddenTokens);
    NonnullRefPtr<OptionalChain const> parse_optional_chain(NonnullRefPtr<Expression const> base);
    NonnullRefPtr<Expression const> parse_expression(int min_precedence, Associativity associate = Associativity::Right, ForbiddenTokens forbidden = {});
    PrimaryExpressionParseResult parse_primary_expression();
    NonnullRefPtr<Expression const> parse_unary_prefixed_expression();
    NonnullRefPtr<RegExpLiteral const> parse_regexp_literal();
    NonnullRefPtr<ObjectExpression const> parse_object_expression();
    NonnullRefPtr<ArrayExpression const> parse_array_expression();

    enum class StringLiteralType {
        Normal,
        NonTaggedTemplate,
        TaggedTemplate
    };

    NonnullRefPtr<StringLiteral const> parse_string_literal(Token const& token, StringLiteralType string_literal_type = StringLiteralType::Normal, bool* contains_invalid_escape = nullptr);
    NonnullRefPtr<TemplateLiteral const> parse_template_literal(bool is_tagged);
    ExpressionResult parse_secondary_expression(NonnullRefPtr<Expression const>, int min_precedence, Associativity associate = Associativity::Right, ForbiddenTokens forbidden = {});
    NonnullRefPtr<Expression const> parse_call_expression(NonnullRefPtr<Expression const>);
    NonnullRefPtr<NewExpression const> parse_new_expression();
    NonnullRefPtr<ClassDeclaration const> parse_class_declaration();
    NonnullRefPtr<ClassExpression const> parse_class_expression(bool expect_class_name);
    NonnullRefPtr<YieldExpression const> parse_yield_expression();
    NonnullRefPtr<AwaitExpression const> parse_await_expression();
    NonnullRefPtr<Expression const> parse_property_key();
    NonnullRefPtr<AssignmentExpression const> parse_assignment_expression(AssignmentOp, NonnullRefPtr<Expression const> lhs, int min_precedence, Associativity, ForbiddenTokens forbidden = {});
    NonnullRefPtr<Identifier const> parse_identifier();
    NonnullRefPtr<ImportStatement const> parse_import_statement(Program& program);
    NonnullRefPtr<ExportStatement const> parse_export_statement(Program& program);

    RefPtr<FunctionExpression const> try_parse_arrow_function_expression(bool expect_parens, bool is_async = false);
    RefPtr<LabelledStatement const> try_parse_labelled_statement(AllowLabelledFunction allow_function);
    RefPtr<MetaProperty const> try_parse_new_target_expression();
    RefPtr<MetaProperty const> try_parse_import_meta_expression();
    NonnullRefPtr<ImportCall const> parse_import_call();

    Vector<CallExpression::Argument> parse_arguments();

    bool has_errors() const { return m_state.errors.size(); }
    Vector<ParserError> const& errors() const { return m_state.errors; }
    void print_errors(bool print_hint = true) const
    {
        for (auto& error : m_state.errors) {
            if (print_hint) {
                auto hint = error.source_location_hint(m_state.lexer.source());
                if (!hint.is_empty())
                    warnln("{}", hint);
            }
            warnln("SyntaxError: {}", error.to_byte_string());
        }
    }

    struct TokenMemoization {
        bool try_parse_arrow_function_expression_failed;
    };

    // Needs to mess with m_state, and we're not going to expose a non-const getter for that :^)
    friend ThrowCompletionOr<GC::Ref<ECMAScriptFunctionObject>> FunctionConstructor::create_dynamic_function(VM&, FunctionObject&, FunctionObject*, FunctionKind, ReadonlySpan<String> parameter_args, String const& body_arg);

    static Parser parse_function_body_from_string(ByteString const& body_string, u16 parse_options, NonnullRefPtr<FunctionParameters const>, FunctionKind kind, FunctionParsingInsights&);

private:
    friend class ScopePusher;

    void parse_script(Program& program, bool starts_in_strict_mode);
    void parse_module(Program& program);

    Associativity operator_associativity(TokenType) const;
    bool match_expression() const;
    bool match_unary_prefixed_expression() const;
    bool match_secondary_expression(ForbiddenTokens forbidden = {}) const;
    bool match_statement() const;
    bool match_export_or_import() const;

    enum class AllowUsingDeclaration {
        No,
        Yes
    };

    bool match_declaration(AllowUsingDeclaration allow_using = AllowUsingDeclaration::No) const;
    bool try_match_let_declaration() const;
    bool try_match_using_declaration() const;
    bool match_variable_declaration() const;
    bool match_identifier() const;
    bool token_is_identifier(Token const&) const;
    bool match_identifier_name() const;
    bool match_property_key() const;
    bool is_private_identifier_valid() const;
    bool match(TokenType type) const;
    bool done() const;
    void expected(char const* what);
    void syntax_error(String const& message, Optional<Position> = {});
    Token consume();
    Token consume_and_allow_division();
    Token consume_identifier();
    Token consume_identifier_reference();
    Token consume(TokenType type);
    Token consume_and_validate_numeric_literal();
    void consume_or_insert_semicolon();
    void save_state();
    void load_state();
    void discard_saved_state();
    Position position() const;

    RefPtr<BindingPattern const> synthesize_binding_pattern(Expression const& expression);

    Token next_token(size_t steps = 1) const;

    void check_identifier_name_for_assignment_validity(FlyString const&, bool force_strict = false);

    bool try_parse_arrow_function_expression_failed_at_position(Position const&) const;
    void set_try_parse_arrow_function_expression_failed_at_position(Position const&, bool);

    bool match_invalid_escaped_keyword() const;

    bool parse_directive(ScopeNode& body);
    void parse_statement_list(ScopeNode& output_node, AllowLabelledFunction allow_labelled_functions = AllowLabelledFunction::No);

    FlyString consume_string_value();
    ModuleRequest parse_module_request();

    struct RulePosition {
        AK_MAKE_NONCOPYABLE(RulePosition);
        AK_MAKE_NONMOVABLE(RulePosition);

    public:
        RulePosition(Parser& parser, Position position)
            : m_parser(parser)
            , m_position(position)
        {
            m_parser.m_rule_starts.append(position);
        }

        ~RulePosition()
        {
            auto last = m_parser.m_rule_starts.take_last();
            VERIFY(last.line == m_position.line);
            VERIFY(last.column == m_position.column);
        }

        Position const& position() const { return m_position; }

    private:
        Parser& m_parser;
        Position m_position;
    };

    [[nodiscard]] RulePosition push_start() { return { *this, position() }; }

    struct ParserState {
        Lexer lexer;
        Token current_token;
        bool previous_token_was_period { false };
        Vector<ParserError> errors;
        ScopePusher* current_scope_pusher { nullptr };

        HashMap<FlyString, Optional<Position>> labels_in_scope;
        HashMap<size_t, Position> invalid_property_range_in_object_expression;
        HashTable<FlyString>* referenced_private_names { nullptr };

        bool strict_mode { false };
        bool allow_super_property_lookup { false };
        bool allow_super_constructor_call { false };
        bool in_function_context { false };
        bool initiated_by_eval { false };
        bool in_eval_function_context { false }; // This controls if we allow new.target or not. Note that eval("return") is not allowed, so we have to have a separate state variable for eval.
        bool in_formal_parameter_context { false };
        bool in_catch_parameter_context { false };
        bool in_generator_function_context { false };
        bool await_expression_is_valid { false };
        bool in_arrow_function_context { false };
        bool in_break_context { false };
        bool in_continue_context { false };
        bool string_legacy_octal_escape_sequence_in_scope { false };
        bool in_class_field_initializer { false };
        bool in_class_static_init_block { false };
        bool function_might_need_arguments_object { false };

        ParserState(Lexer, Program::Type);
    };

    [[nodiscard]] NonnullRefPtr<Identifier const> create_identifier_and_register_in_current_scope(SourceRange range, FlyString string, Optional<DeclarationKind> = {});

    NonnullRefPtr<SourceCode const> m_source_code;
    Vector<Position> m_rule_starts;
    ParserState m_state;
    Vector<ParserState> m_saved_state;
    HashMap<size_t, TokenMemoization> m_token_memoizations;
    Program::Type m_program_type;
};
}
