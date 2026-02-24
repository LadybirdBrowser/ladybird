/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Expression parsing: primary, secondary (binary/postfix), unary, and
//! precedence climbing.

use std::rc::Rc;

use crate::ast::*;
use crate::lexer::ch;
use crate::parser::{
    is_strict_reserved_word, Associativity, ForbiddenTokens, FunctionKind, MethodKind, ParamInfo,
    ParsedParameters, Parser, Position, PropertyKey, PRECEDENCE_ASSIGNMENT, PRECEDENCE_COMMA,
    PRECEDENCE_MEMBER, PRECEDENCE_UNARY,
};
use crate::token::{Token, TokenType};

#[derive(PartialEq, Eq)]
enum EscapeMode {
    /// Tagged template literals: invalid escapes produce `undefined` cooked value.
    TaggedTemplate,
    /// String literals: invalid escapes emit syntax errors, legacy octals are tracked.
    StringLiteral,
}

impl<'a> Parser<'a> {
    pub(crate) fn match_expression(&mut self) -> bool {
        match self.current_token_type() {
            TokenType::BoolLiteral
            | TokenType::NumericLiteral
            | TokenType::BigIntLiteral
            | TokenType::StringLiteral
            | TokenType::NullLiteral
            | TokenType::RegexLiteral
            | TokenType::TemplateLiteralStart
            | TokenType::This
            | TokenType::Super
            | TokenType::New
            | TokenType::Class
            | TokenType::Function
            | TokenType::ParenOpen
            | TokenType::CurlyOpen
            | TokenType::BracketOpen
            | TokenType::PrivateIdentifier
            | TokenType::Slash
            | TokenType::SlashEquals => true,

            TokenType::Async => true,
            TokenType::Yield => true,
            TokenType::Await => true,

            // https://tc39.es/ecma262/#sec-import-calls
            // https://tc39.es/ecma262/#sec-import-meta
            // `import` is only a valid expression start if followed by `(` or `.`.
            TokenType::Import => {
                let next = self.next_token();
                next.token_type == TokenType::ParenOpen || next.token_type == TokenType::Period
            }

            _ => {
                if self.match_identifier() {
                    return true;
                }
                self.match_unary_prefixed_expression()
            }
        }
    }

    pub(crate) fn match_unary_prefixed_expression(&self) -> bool {
        matches!(
            self.current_token_type(),
            TokenType::PlusPlus
                | TokenType::MinusMinus
                | TokenType::ExclamationMark
                | TokenType::Tilde
                | TokenType::Plus
                | TokenType::Minus
                | TokenType::Typeof
                | TokenType::Void
                | TokenType::Delete
        )
    }

    pub(crate) fn match_secondary_expression(&self, forbidden: &ForbiddenTokens) -> bool {
        let tt = self.current_token_type();
        if !forbidden.allows(tt) {
            return false;
        }
        match tt {
            TokenType::Period
            | TokenType::BracketOpen
            | TokenType::ParenOpen
            | TokenType::QuestionMarkPeriod => true,
            TokenType::PlusPlus | TokenType::MinusMinus => {
                !self.current_token.trivia_has_line_terminator
            }
            TokenType::DoubleAsterisk
            | TokenType::Asterisk
            | TokenType::Slash
            | TokenType::Percent
            | TokenType::Plus
            | TokenType::Minus
            | TokenType::ShiftLeft
            | TokenType::ShiftRight
            | TokenType::UnsignedShiftRight
            | TokenType::LessThan
            | TokenType::LessThanEquals
            | TokenType::GreaterThan
            | TokenType::GreaterThanEquals
            | TokenType::In
            | TokenType::Instanceof
            | TokenType::EqualsEquals
            | TokenType::ExclamationMarkEquals
            | TokenType::EqualsEqualsEquals
            | TokenType::ExclamationMarkEqualsEquals
            | TokenType::Ampersand
            | TokenType::Caret
            | TokenType::Pipe
            | TokenType::DoubleQuestionMark
            | TokenType::DoubleAmpersand
            | TokenType::DoublePipe => true,
            TokenType::QuestionMark => true,
            TokenType::Equals
            | TokenType::PlusEquals
            | TokenType::MinusEquals
            | TokenType::DoubleAsteriskEquals
            | TokenType::AsteriskEquals
            | TokenType::SlashEquals
            | TokenType::PercentEquals
            | TokenType::ShiftLeftEquals
            | TokenType::ShiftRightEquals
            | TokenType::UnsignedShiftRightEquals
            | TokenType::AmpersandEquals
            | TokenType::CaretEquals
            | TokenType::PipeEquals
            | TokenType::DoubleAmpersandEquals
            | TokenType::DoublePipeEquals
            | TokenType::DoubleQuestionMarkEquals => true,
            _ => false,
        }
    }

    pub(crate) fn parse_expression_any(&mut self) -> Expression {
        self.parse_expression(
            PRECEDENCE_COMMA,
            Associativity::Right,
            ForbiddenTokens::none(),
        )
    }

    pub(crate) fn parse_assignment_expression(&mut self) -> Expression {
        self.parse_expression(
            PRECEDENCE_ASSIGNMENT,
            Associativity::Right,
            ForbiddenTokens::none(),
        )
    }

    pub(crate) fn parse_expression(
        &mut self,
        min_precedence: i32,
        associativity: Associativity,
        forbidden: ForbiddenTokens,
    ) -> Expression {
        if self.match_unary_prefixed_expression() {
            let start = self.position();
            let expression = self.parse_unary_prefixed_expression();

            // https://tc39.es/ecma262/#sec-exp-operator
            // ExponentiationExpression :
            //   UnaryExpression
            //   UpdateExpression `**` ExponentiationExpression
            // NB: UnaryExpression cannot be the base of `**`, only UpdateExpression can.
            // This prevents ambiguity like `-x ** y` (is it `(-x) ** y` or `-(x ** y)`?).
            // ++x ** y and --x ** y are valid (they're UpdateExpressions, not UnaryExpressions).
            if self.match_token(TokenType::DoubleAsterisk)
                && !Self::is_update_expression(&expression)
            {
                self.syntax_error(
                    "Unparenthesized unary expression can't appear on the left-hand side of '**'",
                );
            }

            return self.continue_parse_expression(
                start,
                expression,
                min_precedence,
                associativity,
                forbidden,
            );
        }

        let lhs_start = self.position();
        let (expression, should_continue) = self.parse_primary_expression(min_precedence);

        // C++ checks for freestanding `arguments` references here (after
        // parse_primary_expression), NOT during consume(). This avoids
        // falsely flagging parameter names like `function f(arguments)`.
        if let ExpressionKind::Identifier(ref id) = expression.inner {
            if id.name == utf16!("arguments")
                && !self.flags.strict_mode
                && !self
                    .scope_collector
                    .has_declaration_in_current_function(&id.name)
            {
                self.scope_collector
                    .set_contains_access_to_arguments_object_in_non_strict_mode();
            }
        }

        if !should_continue {
            // Yield/Await expressions don't participate in secondary expression
            // parsing (e.g. member access), but they DO participate in comma
            // expressions (e.g. `yield 1, yield 2`). Check for comma here.
            return self.parse_comma_expression(lhs_start, expression, min_precedence, forbidden);
        }

        let expression = self.parse_tagged_template_literals(lhs_start, expression);

        self.continue_parse_expression(
            lhs_start,
            expression,
            min_precedence,
            associativity,
            forbidden,
        )
    }

    fn continue_parse_expression(
        &mut self,
        lhs_start: Position,
        mut expression: Expression,
        min_precedence: i32,
        associativity: Associativity,
        mut forbidden: ForbiddenTokens,
    ) -> Expression {
        let original_forbidden = forbidden;
        while self.match_secondary_expression(&forbidden) {
            let new_precedence = Self::operator_precedence(self.current_token_type());
            if new_precedence < min_precedence {
                break;
            }
            if new_precedence == min_precedence && associativity == Associativity::Left {
                break;
            }

            let result = self.parse_secondary_expression(
                lhs_start,
                expression,
                new_precedence,
                original_forbidden,
            );
            expression = result.0;
            forbidden = forbidden.merge(result.1);

            // Tagged template literals bind tighter than any operator, so we
            // consume them eagerly after each secondary expression — but NOT
            // after update expressions (x++`template` is not valid).
            if !Self::is_update_expression(&expression) {
                expression = self.parse_tagged_template_literals(lhs_start, expression);
            }
        }

        self.parse_comma_expression(lhs_start, expression, min_precedence, forbidden)
    }

    fn parse_comma_expression(
        &mut self,
        start: Position,
        expression: Expression,
        min_precedence: i32,
        forbidden: ForbiddenTokens,
    ) -> Expression {
        if min_precedence <= 1
            && self.match_token(TokenType::Comma)
            && forbidden.allows(TokenType::Comma)
        {
            let mut expressions = vec![expression];
            while self.match_token(TokenType::Comma) {
                self.consume();
                expressions.push(self.parse_assignment_expression());
            }
            return self.expression(start, ExpressionKind::Sequence(expressions));
        }
        expression
    }

    /// Parse a primary expression (literal, identifier, `this`, etc.).
    /// Returns `(expression, should_continue)` — `false` means the caller
    /// should not attempt to parse a secondary expression (e.g. arrow).
    fn parse_primary_expression(&mut self, min_precedence: i32) -> (Expression, bool) {
        let start = self.position();
        let token = self.current_token().clone();

        match token.token_type {
            TokenType::ParenOpen => {
                let paren_start = self.position();
                self.consume_token(TokenType::ParenOpen);
                if let Some(arrow) =
                    self.try_parse_arrow_function_expression_impl(true, false, Some(paren_start))
                {
                    return (arrow, false);
                }
                if self.match_token(TokenType::ParenClose) {
                    self.syntax_error("Unexpected token )");
                    self.consume();
                    return (self.expression(start, ExpressionKind::Error), true);
                }
                let expression = self.parse_expression_any();
                self.consume_token(TokenType::ParenClose);
                (expression, true)
            }

            TokenType::This => {
                self.consume();
                self.scope_collector.set_uses_this();
                (self.expression(start, ExpressionKind::This), true)
            }

            TokenType::Class => {
                let expression = self.parse_class_expression(false);
                (expression, true)
            }

            // https://tc39.es/ecma262/#sec-super-keyword
            // SuperProperty : `super` `.` IdentifierName
            //               | `super` `[` Expression `]`
            // SuperCall     : `super` Arguments
            // NB: `super` must be followed by `.`, `[`, or `(`; bare `super` is invalid.
            TokenType::Super => {
                self.consume();
                // C++ creates SuperCall in parse_call_expression which does
                // push_start() after `super` is consumed, so position is at `(`.
                let after_super = self.position();
                if self.scope_collector.has_current_scope() {
                    self.scope_collector.set_uses_new_target();
                }
                if self.match_token(TokenType::ParenOpen) {
                    if !self.flags.allow_super_constructor_call {
                        self.syntax_error("'super' keyword unexpected here");
                    }
                    let arguments = self.parse_arguments();
                    (
                        self.expression(
                            after_super,
                            ExpressionKind::SuperCall(SuperCallData {
                                arguments,
                                is_synthetic: false,
                            }),
                        ),
                        true,
                    )
                } else if self.match_token(TokenType::Period)
                    || self.match_token(TokenType::BracketOpen)
                {
                    if !self.flags.allow_super_property_lookup {
                        self.syntax_error("'super' keyword unexpected here");
                    }
                    (self.expression(start, ExpressionKind::Super), true)
                } else {
                    self.syntax_error("'super' keyword unexpected here");
                    (self.expression(start, ExpressionKind::Super), true)
                }
            }

            TokenType::NumericLiteral => {
                let token = self.consume_and_validate_numeric_literal();
                let value_str = self.token_value(&token);
                let value = parse_numeric_value(value_str);
                (
                    self.expression(start, ExpressionKind::NumericLiteral(value)),
                    true,
                )
            }

            TokenType::BigIntLiteral => {
                let token = self.consume();
                let value = self.token_value(&token);
                // Store the raw value including the 'n' suffix, matching C++.
                let value_utf8: String = value
                    .iter()
                    .map(|&c| {
                        assert!(
                            c < 128,
                            "BigIntLiteral should only contain ASCII characters"
                        );
                        c as u8 as char
                    })
                    .collect();
                (
                    self.expression(start, ExpressionKind::BigIntLiteral(value_utf8)),
                    true,
                )
            }

            TokenType::BoolLiteral => {
                let token = self.consume();
                let value = self.token_value(&token);
                let is_true = value == utf16!("true");
                (
                    self.expression(start, ExpressionKind::BooleanLiteral(is_true)),
                    true,
                )
            }

            TokenType::StringLiteral => {
                let token = self.consume();
                // C++ calls consume() before push_start() for StringLiteral,
                // so its position is the token AFTER the string.
                let after_string = self.position();
                let (value, has_octal) = self.parse_string_value(&token);
                if has_octal {
                    if self.flags.strict_mode {
                        self.syntax_error(
                            "Octal escape sequence in string literal not allowed in strict mode",
                        );
                    } else {
                        self.flags.string_legacy_octal_escape_sequence_in_scope = true;
                    }
                }
                (
                    self.expression(after_string, ExpressionKind::StringLiteral(value)),
                    true,
                )
            }

            TokenType::NullLiteral => {
                self.consume();
                (self.expression(start, ExpressionKind::NullLiteral), true)
            }

            TokenType::CurlyOpen => {
                let expression = self.parse_object_expression();
                (expression, true)
            }

            TokenType::BracketOpen => {
                let expression = self.parse_array_expression();
                (expression, true)
            }

            TokenType::Function => {
                let expression = self.parse_function_expression();
                (expression, true)
            }

            // https://tc39.es/ecma262/#sec-async-function-definitions
            // `async` [no LineTerminator here] `function` starts an async function expression.
            // `async` [no LineTerminator here] ArrowParameters `=>` starts an async arrow.
            // Otherwise, `async` is just an identifier reference.
            TokenType::Async => {
                let next = self.next_token();
                if next.token_type == TokenType::Function && !next.trivia_has_line_terminator {
                    let expression = self.parse_function_expression();
                    return (expression, true);
                }
                if let Some(arrow) = self.try_parse_arrow_function_expression(
                    next.token_type == TokenType::ParenOpen,
                    true,
                ) {
                    return (arrow, false);
                }
                let token = self.consume_and_check_identifier();
                let value = self.token_value(&token).to_vec();
                let id = self.make_identifier(start, value.clone());
                self.scope_collector
                    .register_identifier(id.clone(), &value, None);
                (self.expression(start, ExpressionKind::Identifier(id)), true)
            }

            TokenType::TemplateLiteralStart => {
                let expression = self.parse_template_literal(false);
                (expression, true)
            }

            TokenType::New => {
                let expression = self.parse_new_expression();
                (expression, true)
            }

            // https://tc39.es/ecma262/#sec-import-calls
            // ImportCall : `import` `(` AssignmentExpression `,`? `)
            //            | `import` `(` AssignmentExpression `,` AssignmentExpression `,`? `)`
            // https://tc39.es/ecma262/#sec-import-meta
            // `import.meta` is only valid in module code.
            TokenType::Import => {
                self.consume();
                if self.match_token(TokenType::Period) {
                    self.consume();
                    let meta_token = self.current_token.clone();
                    self.consume_token(TokenType::Identifier);
                    let meta_utf16: [u16; 4] = [ch(b'm'), ch(b'e'), ch(b't'), ch(b'a')];
                    if self.token_original_value(&meta_token) != meta_utf16 {
                        self.syntax_error("Expected 'meta' after 'import.'");
                    }
                    if self.program_type != ProgramType::Module {
                        self.syntax_error("import.meta is only allowed in modules");
                    }
                    (
                        self.expression(
                            start,
                            ExpressionKind::MetaProperty(MetaPropertyType::ImportMeta),
                        ),
                        true,
                    )
                } else if self.match_token(TokenType::ParenOpen) {
                    self.consume();
                    let specifier = self.parse_assignment_expression();
                    let options = if self.match_token(TokenType::Comma) {
                        self.consume();
                        if self.match_token(TokenType::ParenClose) {
                            None
                        } else {
                            let opts = self.parse_assignment_expression();
                            if self.match_token(TokenType::Comma) {
                                self.consume();
                            }
                            Some(Box::new(opts))
                        }
                    } else {
                        None
                    };
                    self.consume_token(TokenType::ParenClose);
                    (
                        self.expression(
                            start,
                            ExpressionKind::ImportCall {
                                specifier: Box::new(specifier),
                                options,
                            },
                        ),
                        true,
                    )
                } else {
                    self.expected("'.' or '('");
                    (self.expression(start, ExpressionKind::Error), true)
                }
            }

            // https://tc39.es/ecma262/#sec-generator-function-definitions
            // YieldExpression : `yield`
            //                 | `yield` [no LineTerminator here] AssignmentExpression
            //                 | `yield` [no LineTerminator here] `*` AssignmentExpression
            // YieldExpression is at AssignmentExpression level (precedence 3).
            // When min_precedence is higher (e.g. void/typeof at 17), yield must
            // be treated as an identifier, not a yield expression.
            TokenType::Yield if self.flags.in_generator_function_context && min_precedence <= 3 => {
                let expression = self.parse_yield_expression();
                (expression, false)
            }

            // https://tc39.es/ecma262/#sec-async-function-definitions
            // AwaitExpression : `await` UnaryExpression
            // NB: Unlike yield (AssignmentExpression level), await is at
            // UnaryExpression level, so `await 1 + 2` is `(await 1) + 2`.
            // We set should_continue=true to allow binary operators.
            TokenType::Await if self.flags.await_expression_is_valid => {
                let expression = self.parse_await_expression();
                (expression, true)
            }

            TokenType::PrivateIdentifier => {
                let id = self.parse_private_identifier(start);
                (
                    self.expression(start, ExpressionKind::PrivateIdentifier(id)),
                    true,
                )
            }

            TokenType::RegexLiteral => {
                let token = self.consume();
                (self.parse_regex_literal(start, &token), true)
            }

            TokenType::Slash | TokenType::SlashEquals => {
                let token = self.lexer.force_slash_as_regex();
                self.current_token = token;
                let token = self.consume();
                (self.parse_regex_literal(start, &token), true)
            }

            _ => {
                // NB: When Await/Yield guards above don't match, those tokens fall
                // through here. match_identifier() may return false for Await in
                // class static init blocks, but we still need to try arrow function
                // parsing and identifier consumption (with appropriate errors).
                // This matches C++'s "goto read_as_identifier" pattern.
                if self.match_identifier()
                    || self.match_token(TokenType::Await)
                    || self.match_token(TokenType::Yield)
                {
                    if let Some(arrow) = self.try_parse_arrow_function_expression(false, false) {
                        return (arrow, false);
                    }
                    if self.match_token(TokenType::Await)
                        && (self.program_type == ProgramType::Module
                            || self.flags.await_expression_is_valid
                            || self.flags.in_class_static_init_block)
                    {
                        self.syntax_error(
                            "'await' is not allowed as an identifier in this context",
                        );
                    }
                    if self.match_token(TokenType::Yield)
                        && (self.flags.strict_mode || self.flags.in_generator_function_context)
                    {
                        self.syntax_error(
                            "'yield' is not allowed as an identifier in this context",
                        );
                    }
                    let token = self.consume_and_check_identifier();
                    let value = self.token_value(&token).to_vec();
                    let id = self.make_identifier(start, value.clone());
                    self.scope_collector
                        .register_identifier(id.clone(), &value, None);
                    (self.expression(start, ExpressionKind::Identifier(id)), true)
                } else if self.match_token(TokenType::EscapedKeyword) {
                    self.syntax_error("Keyword must not contain escaped characters");
                    let token = self.consume_and_check_identifier();
                    let value = self.token_value(&token).to_vec();
                    let id = self.make_identifier(start, value.clone());
                    self.scope_collector
                        .register_identifier(id.clone(), &value, None);
                    (self.expression(start, ExpressionKind::Identifier(id)), true)
                } else {
                    self.expected("primary expression");
                    self.consume();
                    (self.expression(start, ExpressionKind::Error), true)
                }
            }
        }
    }

    fn parse_regex_literal(&mut self, start: Position, token: &Token) -> Expression {
        let value = self.token_value(token);
        let pattern = if value.len() >= 2 {
            value[1..value.len() - 1].to_vec()
        } else {
            value.to_vec()
        };
        let flags = if self.match_token(TokenType::RegexFlags) {
            let ftok = self.consume();
            self.token_value(&ftok).to_vec()
        } else {
            Vec::new()
        };
        self.validate_regex_flags(&flags);
        let compiled_regex = self.compile_regex_pattern(&pattern, &flags);
        self.expression(
            start,
            ExpressionKind::RegExpLiteral(RegExpLiteralData {
                pattern: pattern.into(),
                flags: flags.into(),
                compiled_regex: CompiledRegex::new(compiled_regex),
            }),
        )
    }

    fn parse_secondary_expression(
        &mut self,
        _lhs_start: Position,
        lhs: Expression,
        min_precedence: i32,
        forbidden: ForbiddenTokens,
    ) -> (Expression, ForbiddenTokens) {
        let start = self.position();
        let tt = self.current_token_type();

        match tt {
            // === Binary operators ===
            TokenType::Plus
            | TokenType::Minus
            | TokenType::Asterisk
            | TokenType::Slash
            | TokenType::Percent
            | TokenType::DoubleAsterisk
            | TokenType::ShiftLeft
            | TokenType::ShiftRight
            | TokenType::UnsignedShiftRight
            | TokenType::Ampersand
            | TokenType::Caret
            | TokenType::Pipe
            | TokenType::LessThan
            | TokenType::LessThanEquals
            | TokenType::GreaterThan
            | TokenType::GreaterThanEquals
            | TokenType::EqualsEquals
            | TokenType::ExclamationMarkEquals
            | TokenType::EqualsEqualsEquals
            | TokenType::ExclamationMarkEqualsEquals
            | TokenType::In
            | TokenType::Instanceof => {
                let op = token_to_binary_op(tt);
                self.consume();
                let rhs = self.parse_expression(
                    min_precedence,
                    Self::operator_associativity(tt),
                    forbidden,
                );
                (
                    self.expression(
                        start,
                        ExpressionKind::Binary {
                            op,
                            lhs: Box::new(lhs),
                            rhs: Box::new(rhs),
                        },
                    ),
                    ForbiddenTokens::none(),
                )
            }

            // === Logical operators ===
            // https://tc39.es/ecma262/#sec-binary-logical-operators
            // It is a Syntax Error if ShortCircuitExpression includes both
            // LogicalORExpression (||/&&) and CoalesceExpression (??), since
            // their precedence is ambiguous without explicit parentheses.
            TokenType::DoubleAmpersand => {
                self.consume();
                let new_forbidden = forbidden.forbid(&[TokenType::DoubleQuestionMark]);
                let rhs = self.parse_expression(min_precedence, Associativity::Left, new_forbidden);
                (
                    self.expression(
                        start,
                        ExpressionKind::Logical {
                            op: LogicalOp::And,
                            lhs: Box::new(lhs),
                            rhs: Box::new(rhs),
                        },
                    ),
                    new_forbidden,
                )
            }
            TokenType::DoublePipe => {
                self.consume();
                let new_forbidden = forbidden.forbid(&[TokenType::DoubleQuestionMark]);
                let rhs = self.parse_expression(min_precedence, Associativity::Left, new_forbidden);
                (
                    self.expression(
                        start,
                        ExpressionKind::Logical {
                            op: LogicalOp::Or,
                            lhs: Box::new(lhs),
                            rhs: Box::new(rhs),
                        },
                    ),
                    new_forbidden,
                )
            }
            TokenType::DoubleQuestionMark => {
                self.consume();
                let new_forbidden =
                    forbidden.forbid(&[TokenType::DoubleAmpersand, TokenType::DoublePipe]);
                let rhs = self.parse_expression(min_precedence, Associativity::Left, new_forbidden);
                (
                    self.expression(
                        start,
                        ExpressionKind::Logical {
                            op: LogicalOp::NullishCoalescing,
                            lhs: Box::new(lhs),
                            rhs: Box::new(rhs),
                        },
                    ),
                    new_forbidden,
                )
            }

            // === Assignment ===
            TokenType::Equals
            | TokenType::PlusEquals
            | TokenType::MinusEquals
            | TokenType::DoubleAsteriskEquals
            | TokenType::AsteriskEquals
            | TokenType::SlashEquals
            | TokenType::PercentEquals
            | TokenType::ShiftLeftEquals
            | TokenType::ShiftRightEquals
            | TokenType::UnsignedShiftRightEquals
            | TokenType::AmpersandEquals
            | TokenType::CaretEquals
            | TokenType::PipeEquals
            | TokenType::DoubleAmpersandEquals
            | TokenType::DoublePipeEquals
            | TokenType::DoubleQuestionMarkEquals => {
                let op = token_to_assignment_op(tt);
                if op == AssignmentOp::Assignment
                    && (Self::is_object_expression(&lhs) || Self::is_array_expression(&lhs))
                {
                    // Save pattern_bound_names so that an outer binding
                    // pattern parse in progress doesn't lose its entries.
                    let saved_bound_names = std::mem::take(&mut self.pattern_bound_names);
                    // Use the expression's own range start, not the outer
                    // lhs_start. When the expression is parenthesized (e.g.
                    // `([a,b]) = ...`), lhs_start points to `(` but we need
                    // to re-lex from `[` to correctly synthesize the pattern.
                    if let Some(binding_pattern) = self.synthesize_binding_pattern(lhs.range.start)
                    {
                        // Register synthesized identifiers with the scope collector so
                        // they get resolved as locals during analyze().
                        for (name, id) in self.pattern_bound_names.drain(..) {
                            self.scope_collector.register_identifier(id, &name, None);
                        }
                        self.pattern_bound_names = saved_bound_names;
                        self.consume();
                        let rhs =
                            self.parse_expression(min_precedence, Associativity::Right, forbidden);
                        return (
                            self.expression(
                                start,
                                ExpressionKind::Assignment {
                                    op,
                                    lhs: AssignmentLhs::Pattern(binding_pattern),
                                    rhs: Box::new(rhs),
                                },
                            ),
                            ForbiddenTokens::none(),
                        );
                    } else {
                        self.pattern_bound_names = saved_bound_names;
                    }
                }
                let allow_call = !matches!(
                    tt,
                    TokenType::DoubleAmpersandEquals
                        | TokenType::DoublePipeEquals
                        | TokenType::DoubleQuestionMarkEquals
                );
                if !Self::is_simple_assignment_target(&lhs, allow_call) {
                    self.syntax_error("Invalid left-hand side in assignment");
                }
                if let ExpressionKind::Identifier(ref id) = lhs.inner {
                    self.check_identifier_name_for_assignment_validity(&id.name, false);
                }
                self.consume();
                let rhs = self.parse_expression(min_precedence, Associativity::Right, forbidden);
                (
                    self.expression(
                        start,
                        ExpressionKind::Assignment {
                            op,
                            lhs: AssignmentLhs::Expression(Box::new(lhs)),
                            rhs: Box::new(rhs),
                        },
                    ),
                    ForbiddenTokens::none(),
                )
            }

            // === Ternary ===
            TokenType::QuestionMark => {
                self.consume();
                let consequent = self.parse_assignment_expression();
                self.consume_token(TokenType::Colon);
                let alternate =
                    self.parse_expression(PRECEDENCE_ASSIGNMENT, Associativity::Right, forbidden);
                (
                    self.expression(
                        start,
                        ExpressionKind::Conditional {
                            test: Box::new(lhs),
                            consequent: Box::new(consequent),
                            alternate: Box::new(alternate),
                        },
                    ),
                    ForbiddenTokens::none(),
                )
            }

            // === Member access ===
            TokenType::Period => {
                self.consume();
                if self.match_token(TokenType::PrivateIdentifier) {
                    // C++ uses rule_start (period position) for property identifiers.
                    let id = self.parse_private_identifier(start);
                    let property = self.expression(start, ExpressionKind::PrivateIdentifier(id));
                    (
                        self.expression(
                            start,
                            ExpressionKind::Member {
                                object: Box::new(lhs),
                                property: Box::new(property),
                                computed: false,
                            },
                        ),
                        ForbiddenTokens::none(),
                    )
                } else if self.match_identifier_name() {
                    let token = self.consume();
                    let value = self.token_value(&token).to_vec();
                    // C++ uses rule_start (period position) for property identifiers.
                    let property = self.expression(
                        start,
                        ExpressionKind::Identifier(self.make_identifier(start, value)),
                    );
                    (
                        self.expression(
                            start,
                            ExpressionKind::Member {
                                object: Box::new(lhs),
                                property: Box::new(property),
                                computed: false,
                            },
                        ),
                        ForbiddenTokens::none(),
                    )
                } else {
                    self.expected("property name");
                    (lhs, ForbiddenTokens::none())
                }
            }

            // === Computed member access ===
            TokenType::BracketOpen => {
                self.consume();
                let property = self.parse_expression_any();
                self.consume_token(TokenType::BracketClose);
                (
                    self.expression(
                        start,
                        ExpressionKind::Member {
                            object: Box::new(lhs),
                            property: Box::new(property),
                            computed: true,
                        },
                    ),
                    ForbiddenTokens::none(),
                )
            }

            // === Call ===
            TokenType::ParenOpen => {
                let expression = self.parse_call_expression(lhs);
                (expression, ForbiddenTokens::none())
            }

            // === Optional chaining ===
            TokenType::QuestionMarkPeriod => {
                let chain = self.parse_optional_chain(start, lhs);
                (chain, ForbiddenTokens::none())
            }

            // === Postfix ===
            // https://tc39.es/ecma262/#sec-update-expressions
            // UpdateExpression : LeftHandSideExpression [no LineTerminator here] `++`
            //                  | LeftHandSideExpression [no LineTerminator here] `--`
            // NB: The [no LineTerminator here] is enforced by match_secondary_expression
            // which checks trivia_has_line_terminator for PlusPlus/MinusMinus.
            TokenType::PlusPlus => {
                if !Self::is_simple_assignment_target(&lhs, true) {
                    self.syntax_error("Invalid left-hand side in postfix operation");
                }
                if let ExpressionKind::Identifier(ref id) = lhs.inner {
                    self.check_identifier_name_for_assignment_validity(&id.name, false);
                }
                self.consume();
                (
                    self.expression(
                        start,
                        ExpressionKind::Update {
                            op: UpdateOp::Increment,
                            argument: Box::new(lhs),
                            prefixed: false,
                        },
                    ),
                    ForbiddenTokens::none(),
                )
            }
            TokenType::MinusMinus => {
                if !Self::is_simple_assignment_target(&lhs, true) {
                    self.syntax_error("Invalid left-hand side in postfix operation");
                }
                if let ExpressionKind::Identifier(ref id) = lhs.inner {
                    self.check_identifier_name_for_assignment_validity(&id.name, false);
                }
                self.consume();
                (
                    self.expression(
                        start,
                        ExpressionKind::Update {
                            op: UpdateOp::Decrement,
                            argument: Box::new(lhs),
                            prefixed: false,
                        },
                    ),
                    ForbiddenTokens::none(),
                )
            }

            _ => {
                self.expected("secondary expression");
                (lhs, ForbiddenTokens::none())
            }
        }
    }

    fn parse_unary_prefixed_expression(&mut self) -> Expression {
        let start = self.position();
        let tt = self.current_token_type();

        match tt {
            TokenType::PlusPlus => {
                self.consume();
                let expression = self.parse_expression(
                    PRECEDENCE_UNARY,
                    Associativity::Right,
                    ForbiddenTokens::none(),
                );
                if !Self::is_simple_assignment_target(&expression, true) {
                    self.syntax_error("Invalid left-hand side in prefix operation");
                }
                if let ExpressionKind::Identifier(ref id) = expression.inner {
                    self.check_identifier_name_for_assignment_validity(&id.name, false);
                }
                self.expression(
                    start,
                    ExpressionKind::Update {
                        op: UpdateOp::Increment,
                        argument: Box::new(expression),
                        prefixed: true,
                    },
                )
            }
            TokenType::MinusMinus => {
                self.consume();
                let expression = self.parse_expression(
                    PRECEDENCE_UNARY,
                    Associativity::Right,
                    ForbiddenTokens::none(),
                );
                if !Self::is_simple_assignment_target(&expression, true) {
                    self.syntax_error("Invalid left-hand side in prefix operation");
                }
                if let ExpressionKind::Identifier(ref id) = expression.inner {
                    self.check_identifier_name_for_assignment_validity(&id.name, false);
                }
                self.expression(
                    start,
                    ExpressionKind::Update {
                        op: UpdateOp::Decrement,
                        argument: Box::new(expression),
                        prefixed: true,
                    },
                )
            }
            TokenType::ExclamationMark
            | TokenType::Tilde
            | TokenType::Plus
            | TokenType::Minus
            | TokenType::Typeof
            | TokenType::Void => {
                let op = match tt {
                    TokenType::ExclamationMark => UnaryOp::Not,
                    TokenType::Tilde => UnaryOp::BitwiseNot,
                    TokenType::Plus => UnaryOp::Plus,
                    TokenType::Minus => UnaryOp::Minus,
                    TokenType::Typeof => UnaryOp::Typeof,
                    _ => UnaryOp::Void,
                };
                self.consume();
                let expression = self.parse_expression(
                    PRECEDENCE_UNARY,
                    Associativity::Right,
                    ForbiddenTokens::none(),
                );
                self.expression(
                    start,
                    ExpressionKind::Unary {
                        op,
                        operand: Box::new(expression),
                    },
                )
            }
            // https://tc39.es/ecma262/#sec-delete-operator-static-semantics-early-errors
            // It is a Syntax Error if the UnaryExpression is an IdentifierReference
            // and the source text matched by the enclosing Script or Module is strict mode code.
            TokenType::Delete => {
                self.consume();
                let rhs_start = self.position();
                let expression = self.parse_expression(
                    PRECEDENCE_UNARY,
                    Associativity::Right,
                    ForbiddenTokens::none(),
                );
                if self.flags.strict_mode && Self::is_identifier(&expression) {
                    self.syntax_error_at(
                        "Delete of an unqualified identifier in strict mode.",
                        rhs_start.line,
                        rhs_start.column,
                    );
                }
                if let ExpressionKind::Member { property, .. } = &expression.inner {
                    if matches!(property.inner, ExpressionKind::PrivateIdentifier(_)) {
                        self.syntax_error("Private fields cannot be deleted");
                    }
                }
                self.expression(
                    start,
                    ExpressionKind::Unary {
                        op: UnaryOp::Delete,
                        operand: Box::new(expression),
                    },
                )
            }
            _ => {
                self.expected("unary expression");
                self.consume();
                self.expression(start, ExpressionKind::Error)
            }
        }
    }

    /// Parse a `new` expression, handling `new.target` and nested `new` calls.
    // https://tc39.es/ecma262/#sec-new-operator
    // MemberExpression : `new` MemberExpression Arguments
    // NewExpression    : `new` NewExpression
    // https://tc39.es/ecma262/#sec-meta-properties
    // NewTarget : `new` `.` `target`
    fn parse_new_expression(&mut self) -> Expression {
        let start = self.position();
        self.consume_token(TokenType::New);

        if self.match_token(TokenType::Period) {
            self.consume();
            let target_token = self.current_token.clone();
            self.consume_token(TokenType::Identifier);
            if self.token_original_value(&target_token) != utf16!("target") {
                self.syntax_error("Expected 'target' after 'new.'");
            }
            // https://tc39.es/ecma262/#sec-new.target
            // It is a Syntax Error if NewTarget is not enclosed, directly or indirectly
            // (but not crossing function or class static initialization block boundaries),
            // within a FunctionBody, ConciseBody, ClassStaticBlock, or ClassBody.
            if !self.flags.in_function_context
                && !self.in_eval_function_context
                && !self.flags.in_class_static_init_block
            {
                self.syntax_error("'new.target' not allowed outside of a function");
            }
            if self.scope_collector.has_current_scope() {
                self.scope_collector.set_uses_new_target();
            }
            return self.expression(
                start,
                ExpressionKind::MetaProperty(MetaPropertyType::NewTarget),
            );
        }

        let callee = if self.match_token(TokenType::New) {
            self.parse_new_expression()
        } else {
            let forbidden = ForbiddenTokens::none()
                .forbid(&[TokenType::ParenOpen, TokenType::QuestionMarkPeriod]);
            self.parse_expression(PRECEDENCE_MEMBER, Associativity::Right, forbidden)
        };

        if matches!(callee.inner, ExpressionKind::ImportCall { .. }) {
            self.syntax_error("Cannot call new on dynamic import");
        }

        if self.match_token(TokenType::ParenOpen) {
            let arguments = self.parse_arguments();
            self.expression(
                start,
                ExpressionKind::New(CallExpressionData {
                    callee: Box::new(callee),
                    arguments,
                    is_parenthesized: false,
                    is_inside_parens: false,
                }),
            )
        } else {
            self.expression(
                start,
                ExpressionKind::New(CallExpressionData {
                    callee: Box::new(callee),
                    arguments: Vec::new(),
                    is_parenthesized: false,
                    is_inside_parens: false,
                }),
            )
        }
    }

    /// Parse a call expression `callee(arguments...)`.
    // https://tc39.es/ecma262/#sec-function-calls
    // https://tc39.es/ecma262/#sec-function-calls-runtime-semantics-evaluation
    // NB: A direct call to `eval` (bare identifier `eval` as callee) uses the
    // running execution context's variable environment, not a fresh one.
    pub(crate) fn parse_call_expression(&mut self, callee: Expression) -> Expression {
        let start = self.position();
        let arguments = self.parse_arguments();
        // Check the actual callee expression kind, matching C++ which does
        // is<Identifier>(callee) && callee.string() == "eval".
        if let ExpressionKind::Identifier(ref id) = callee.inner {
            if id.name == utf16!("eval") {
                self.scope_collector.set_contains_direct_call_to_eval();
                self.scope_collector.set_uses_this();
            }
        }
        self.expression(
            start,
            ExpressionKind::Call(CallExpressionData {
                callee: Box::new(callee),
                arguments,
                is_parenthesized: false,
                is_inside_parens: false,
            }),
        )
    }

    pub(crate) fn parse_arguments(&mut self) -> Vec<CallArgument> {
        self.consume_token(TokenType::ParenOpen);
        let mut arguments = Vec::new();

        while !self.match_token(TokenType::ParenClose) && !self.done() {
            let is_spread = self.eat(TokenType::TripleDot);
            let value = self.parse_assignment_expression();
            arguments.push(CallArgument { value, is_spread });
            if !self.match_token(TokenType::Comma) {
                break;
            }
            self.consume();
        }

        self.consume_token(TokenType::ParenClose);
        arguments
    }

    /// Parse an optional chaining expression (`a?.b`, `a?.[x]`, `a?.()`).
    // https://tc39.es/ecma262/#sec-optional-chains
    // OptionalExpression : MemberExpression OptionalChain
    // OptionalChain : `?.` Arguments
    //               | `?.` `[` Expression `]`
    //               | `?.` IdentifierName
    //               | `?.` TemplateLiteral  -- NOTE: this is a syntax error (see below)
    //               | OptionalChain Arguments
    //               | OptionalChain `[` Expression `]`
    //               | OptionalChain `.` IdentifierName
    //               | OptionalChain TemplateLiteral  -- also a syntax error
    fn parse_optional_chain(&mut self, start: Position, base: Expression) -> Expression {
        let mut references = Vec::new();

        loop {
            if self.match_token(TokenType::QuestionMarkPeriod) {
                self.consume();
                match self.current_token_type() {
                    TokenType::ParenOpen => {
                        let arguments = self.parse_arguments();
                        references.push(OptionalChainReference::Call {
                            arguments,
                            mode: OptionalChainMode::Optional,
                        });
                    }
                    TokenType::BracketOpen => {
                        self.consume();
                        let expression = self.parse_expression_any();
                        self.consume_token(TokenType::BracketClose);
                        references.push(OptionalChainReference::ComputedReference {
                            expression: Box::new(expression),
                            mode: OptionalChainMode::Optional,
                        });
                    }
                    TokenType::PrivateIdentifier => {
                        let property_start = self.position();
                        let id = self.parse_private_identifier(property_start);
                        references.push(OptionalChainReference::PrivateMemberReference {
                            private_identifier: id,
                            mode: OptionalChainMode::Optional,
                        });
                    }
                    // https://tc39.es/ecma262/#sec-optional-chaining-chain-production
                    // It is a Syntax Error if any code matches this production:
                    //   OptionalChain : `?.` TemplateLiteral
                    // Tagged templates cannot be used with optional chaining.
                    TokenType::TemplateLiteralStart => {
                        self.syntax_error("Invalid tagged template literal after ?.");
                        break;
                    }
                    _ => {
                        if self.match_identifier_name() {
                            let property_start = self.position();
                            let token = self.consume();
                            let value = self.token_value(&token).to_vec();
                            references.push(OptionalChainReference::MemberReference {
                                identifier: self.make_identifier(property_start, value),
                                mode: OptionalChainMode::Optional,
                            });
                        } else {
                            self.syntax_error("Invalid optional chain reference after ?.");
                            break;
                        }
                    }
                }
            } else if self.match_token(TokenType::ParenOpen) {
                let arguments = self.parse_arguments();
                references.push(OptionalChainReference::Call {
                    arguments,
                    mode: OptionalChainMode::NotOptional,
                });
            } else if self.match_token(TokenType::Period) {
                self.consume();
                if self.match_token(TokenType::PrivateIdentifier) {
                    let property_start = self.position();
                    let id = self.parse_private_identifier(property_start);
                    references.push(OptionalChainReference::PrivateMemberReference {
                        private_identifier: id,
                        mode: OptionalChainMode::NotOptional,
                    });
                } else if self.match_identifier_name() {
                    let property_start = self.position();
                    let token = self.consume();
                    let value = self.token_value(&token).to_vec();
                    references.push(OptionalChainReference::MemberReference {
                        identifier: self.make_identifier(property_start, value),
                        mode: OptionalChainMode::NotOptional,
                    });
                } else {
                    self.expected("an identifier");
                    break;
                }
            } else if self.match_token(TokenType::TemplateLiteralStart) {
                self.syntax_error("Invalid tagged template literal after optional chain");
                break;
            } else if self.match_token(TokenType::BracketOpen) {
                self.consume();
                let expression = self.parse_expression_any();
                self.consume_token(TokenType::BracketClose);
                references.push(OptionalChainReference::ComputedReference {
                    expression: Box::new(expression),
                    mode: OptionalChainMode::NotOptional,
                });
            } else {
                break;
            }

            if self.done() {
                break;
            }
        }

        self.expression(
            start,
            ExpressionKind::OptionalChain {
                base: Box::new(base),
                references,
            },
        )
    }

    /// Parse a `yield` or `yield*` expression.
    // https://tc39.es/ecma262/#sec-generator-function-definitions
    // YieldExpression : `yield`
    //                 | `yield` [no LineTerminator here] AssignmentExpression
    //                 | `yield` [no LineTerminator here] `*` AssignmentExpression
    // https://tc39.es/ecma262/#sec-generator-function-definitions-static-semantics-early-errors
    // It is a Syntax Error if YieldExpression appears within FormalParameters.
    fn parse_yield_expression(&mut self) -> Expression {
        let start = self.position();

        if self.flags.in_formal_parameter_context {
            self.syntax_error(
                "'Yield' expression is not allowed in formal parameters of generator function",
            );
        }

        self.consume_token(TokenType::Yield);

        if self.current_token.trivia_has_line_terminator {
            return self.expression(
                start,
                ExpressionKind::Yield {
                    argument: None,
                    is_yield_from: false,
                },
            );
        }

        let is_yield_from = self.match_token(TokenType::Asterisk);
        if is_yield_from {
            self.consume();
        }

        if is_yield_from || self.match_expression() || self.match_token(TokenType::Class) {
            let argument = self.parse_assignment_expression();
            self.expression(
                start,
                ExpressionKind::Yield {
                    argument: Some(Box::new(argument)),
                    is_yield_from,
                },
            )
        } else {
            self.expression(
                start,
                ExpressionKind::Yield {
                    argument: None,
                    is_yield_from: false,
                },
            )
        }
    }

    /// Parse an `await` expression.
    // https://tc39.es/ecma262/#sec-async-function-definitions
    // AwaitExpression : `await` UnaryExpression
    // https://tc39.es/ecma262/#sec-async-function-definitions-static-semantics-early-errors
    // It is a Syntax Error if AwaitExpression appears within FormalParameters.
    fn parse_await_expression(&mut self) -> Expression {
        let start = self.position();

        if self.flags.in_formal_parameter_context {
            self.syntax_error(
                "'Await' expression is not allowed in formal parameters of an async function",
            );
        }

        self.consume_token(TokenType::Await);
        let argument = self.parse_expression(
            PRECEDENCE_UNARY,
            Associativity::Right,
            ForbiddenTokens::none(),
        );
        self.scope_collector.set_contains_await_expression();
        self.expression(start, ExpressionKind::Await(Box::new(argument)))
    }

    fn parse_object_expression(&mut self) -> Expression {
        let start = self.position();
        self.consume_token(TokenType::CurlyOpen);

        let mut properties = Vec::new();
        let mut has_proto_setter = false;
        while !self.match_token(TokenType::CurlyClose) && !self.done() {
            if self.match_token(TokenType::TripleDot) {
                self.consume();
                let expression = self.parse_assignment_expression();
                // C++ uses object expression start position for all ObjectProperty nodes.
                properties.push(ObjectProperty {
                    range: self.range_from(start),
                    property_type: ObjectPropertyType::Spread,
                    key: Box::new(expression),
                    is_computed: false,
                    value: None,
                    is_method: false,
                });
            } else {
                let property = self.parse_object_property(start);
                // https://tc39.es/ecma262/#sec-object-initializer-static-semantics-early-errors
                // It is a Syntax Error if PropertyNameList of PropertyDefinitionList
                // contains any duplicate entries for "__proto__" and at least two of
                // those entries were obtained from productions of the form
                // PropertyDefinition : PropertyName `:` AssignmentExpression.
                if property.property_type == ObjectPropertyType::ProtoSetter {
                    if has_proto_setter {
                        self.syntax_error(
                            "Duplicate __proto__ fields are not allowed in object expressions",
                        );
                    }
                    has_proto_setter = true;
                }
                properties.push(property);
            }

            if !self.match_token(TokenType::Comma) {
                break;
            }
            self.consume();
        }

        self.consume_token(TokenType::CurlyClose);
        self.expression(start, ExpressionKind::Object(properties))
    }

    fn parse_object_property(&mut self, obj_start: Position) -> ObjectProperty {
        let start = self.position();
        let mut is_getter = false;
        let mut is_setter = false;
        let mut is_async = false;
        let mut is_generator = false;

        if self.match_identifier_name() {
            let value = self.token_original_value(&self.current_token).to_vec();
            if value == utf16!("get") && self.match_property_key_ahead() {
                is_getter = true;
                self.consume();
            } else if value == utf16!("set") && self.match_property_key_ahead() {
                is_setter = true;
                self.consume();
            } else if value == utf16!("async") {
                let next = self.next_token();
                if !next.trivia_has_line_terminator
                    && next.token_type != TokenType::ParenOpen
                    && next.token_type != TokenType::Colon
                    && next.token_type != TokenType::Comma
                    && next.token_type != TokenType::CurlyClose
                {
                    is_async = true;
                    self.consume();
                    if self.match_token(TokenType::Asterisk) {
                        is_generator = true;
                        self.consume();
                    }
                }
            }
        }

        if !is_getter && !is_setter && !is_async && self.match_token(TokenType::Asterisk) {
            is_generator = true;
            self.consume();
        }

        // In C++, identifiers matching match_identifier() are consumed directly in
        // parse_object_expression and get the object's rule_start position. This applies
        // even after consuming an `async` prefix. Only get/set prefix and generator `*`
        // cause the key to go through parse_property_key with its own position.
        // NB: C++ match_identifier() returns true for most EscapedKeyword tokens
        // (like escaped reserved words "if", "while", etc.), only rejecting
        // let/yield/await under specific conditions. Rust's match_identifier()
        // is more restrictive, so we add explicit EscapedKeyword handling here.
        let is_cpp_identifier = self.match_identifier()
            || (self.current_token.token_type == TokenType::EscapedKeyword && {
                let value = self.token_value(&self.current_token);
                value != utf16!("let") && value != utf16!("yield") && value != utf16!("await")
            });
        let ident_override = if !is_getter && !is_setter && !is_generator && is_cpp_identifier {
            Some(obj_start)
        } else {
            None
        };
        let PropertyKey {
            expression: key,
            name: key_value,
            is_proto,
            is_computed,
            is_identifier,
        } = self.parse_property_key(ident_override);

        // https://tc39.es/ecma262/#sec-object-initializer
        // Private names are not allowed in object literals, even inside class bodies.
        if let ExpressionKind::PrivateIdentifier(_) = key.inner {
            self.syntax_error("Private field or method is not allowed in object literal");
        }

        if self.match_token(TokenType::ParenOpen) {
            let method_kind = if is_getter {
                MethodKind::Getter
            } else if is_setter {
                MethodKind::Setter
            } else {
                MethodKind::Normal
            };
            let function = self.parse_method_definition(is_async, is_generator, method_kind, start);
            let property_type = if is_getter {
                ObjectPropertyType::Getter
            } else if is_setter {
                ObjectPropertyType::Setter
            } else {
                ObjectPropertyType::KeyValue
            };
            return ObjectProperty {
                range: self.range_from(obj_start),
                property_type,
                key: Box::new(key),
                value: Some(Box::new(function)),
                is_method: true,
                is_computed,
            };
        }

        // async modifier requires a method (must have parens)
        if is_async {
            self.syntax_error("Expected function after async keyword");
        }

        if is_getter || is_setter {
            let method_kind = if is_getter {
                MethodKind::Getter
            } else {
                MethodKind::Setter
            };
            let function = self.parse_method_definition(false, false, method_kind, start);
            let property_type = if is_getter {
                ObjectPropertyType::Getter
            } else {
                ObjectPropertyType::Setter
            };
            return ObjectProperty {
                range: self.range_from(obj_start),
                property_type,
                key: Box::new(key),
                value: Some(Box::new(function)),
                is_method: true,
                is_computed,
            };
        }

        if self.match_token(TokenType::Colon) {
            self.consume();
            let value = self.parse_assignment_expression();
            let property_type = if is_proto {
                ObjectPropertyType::ProtoSetter
            } else {
                ObjectPropertyType::KeyValue
            };
            return ObjectProperty {
                range: self.range_from(obj_start),
                property_type,
                key: Box::new(key),
                value: Some(Box::new(value)),
                is_method: false,
                is_computed,
            };
        }

        // https://tc39.es/ecma262/#sec-object-initializer
        // CoverInitializedName : IdentifierReference Initializer
        // https://tc39.es/ecma262/#sec-object-initializer-static-semantics-early-errors
        // It is a Syntax Error if PropertyDefinitionList contains any CoverInitializedName.
        // NB: This is not a valid object literal, but is a valid destructuring assignment
        // target. We parse the initializer to advance the lexer, but roll back scope records
        // since this expression is discarded. synthesize_binding_pattern will
        // re-parse from source and create the real scope records.
        if self.match_token(TokenType::Equals) && is_identifier {
            if let Some(kv) = &key_value {
                let id = self.make_identifier(obj_start, kv.clone());
                self.scope_collector
                    .register_identifier(id.clone(), &id.name, None);
                let value = self.expression(obj_start, ExpressionKind::Identifier(id));
                self.consume(); // consume '='
                                // NB: Add a syntax error for CoverInitializedName. This error will
                                // be cleared by synthesize_binding_pattern if the containing object
                                // is reinterpreted as a destructuring pattern, but will persist if
                                // the object is used in expression context (e.g. as a member base).
                self.syntax_error("Invalid property in object literal");
                let saved_scope_state = self.scope_collector.save_state();
                let _initializer = self.parse_assignment_expression();
                self.scope_collector.load_state(saved_scope_state);
                return ObjectProperty {
                    range: self.range_from(obj_start),
                    property_type: ObjectPropertyType::KeyValue,
                    key: Box::new(key),
                    value: Some(Box::new(value)),
                    is_method: false,
                    is_computed: false,
                };
            }
        }

        // Shorthand property: { x }
        // Only identifiers can be shorthand properties, not string/numeric literals.
        if let Some(kv) = key_value.filter(|_| is_identifier) {
            // https://tc39.es/ecma262/#sec-object-initializer-static-semantics-early-errors
            // Strict-mode reserved words cannot be used as shorthand properties.
            if self.flags.strict_mode && is_strict_reserved_word(&kv) {
                let name_str = String::from_utf16_lossy(&kv);
                self.syntax_error(&format!("'{}' is a reserved keyword", name_str));
            }
            let id = self.make_identifier(obj_start, kv);
            self.scope_collector
                .register_identifier(id.clone(), &id.name, None);
            let value = self.expression(obj_start, ExpressionKind::Identifier(id));
            return ObjectProperty {
                range: self.range_from(obj_start),
                property_type: ObjectPropertyType::KeyValue,
                key: Box::new(key),
                value: Some(Box::new(value)),
                is_method: false,
                is_computed: false,
            };
        }

        self.expected("':' or '('");
        ObjectProperty {
            range: self.range_from(obj_start),
            property_type: ObjectPropertyType::KeyValue,
            key: Box::new(key),
            value: None,
            is_method: false,
            is_computed,
        }
    }

    pub(crate) fn match_property_key_ahead(&mut self) -> bool {
        let next = self.next_token();
        matches!(
            next.token_type,
            TokenType::BracketOpen
                | TokenType::StringLiteral
                | TokenType::NumericLiteral
                | TokenType::BigIntLiteral
                | TokenType::PrivateIdentifier
        ) || next.token_type.is_identifier_name()
    }

    pub(crate) fn parse_property_key(
        &mut self,
        ident_pos_override: Option<Position>,
    ) -> PropertyKey {
        // Suppress eval/arguments check for property key tokens (C++ uses
        // regular `consume()` not `consume_and_allow_division()` here).
        let saved_property_key_ctx = self.flags.in_property_key_context;
        self.flags.in_property_key_context = true;
        let result = self.parse_property_key_inner(ident_pos_override);
        self.flags.in_property_key_context = saved_property_key_ctx;
        result
    }

    fn parse_property_key_inner(&mut self, ident_pos_override: Option<Position>) -> PropertyKey {
        let proto_name = utf16!("__proto__");
        let start = self.position();
        match self.current_token_type() {
            TokenType::BracketOpen => {
                self.consume();
                let expression = self.parse_assignment_expression();
                self.consume_token(TokenType::BracketClose);
                PropertyKey {
                    expression,
                    name: None,
                    is_proto: false,
                    is_computed: true,
                    is_identifier: false,
                }
            }
            TokenType::StringLiteral => {
                let token = self.consume();
                // C++ calls consume() before push_start() for StringLiteral,
                // so its position is the token AFTER the string.
                let after_string = self.position();
                let (value, has_octal) = self.parse_string_value(&token);
                if has_octal {
                    if self.flags.strict_mode {
                        self.syntax_error(
                            "Octal escape sequence in string literal not allowed in strict mode",
                        );
                    } else {
                        self.flags.string_legacy_octal_escape_sequence_in_scope = true;
                    }
                }
                let is_proto = value == proto_name;
                let expression =
                    self.expression(after_string, ExpressionKind::StringLiteral(value.clone()));
                PropertyKey {
                    expression,
                    name: Some(value),
                    is_proto,
                    is_computed: false,
                    is_identifier: false,
                }
            }
            TokenType::NumericLiteral => {
                let token = self.consume_and_validate_numeric_literal();
                let value_str = self.token_value(&token);
                let value = parse_numeric_value(value_str);
                let expression = self.expression(start, ExpressionKind::NumericLiteral(value));
                PropertyKey {
                    expression,
                    name: None,
                    is_proto: false,
                    is_computed: false,
                    is_identifier: false,
                }
            }
            TokenType::BigIntLiteral => {
                let token = self.consume();
                let value = self.token_value(&token);
                // Store the raw value including the 'n' suffix, matching C++.
                let value_utf8: String = value
                    .iter()
                    .map(|&c| {
                        assert!(
                            c < 128,
                            "BigIntLiteral should only contain ASCII characters"
                        );
                        c as u8 as char
                    })
                    .collect();
                let expression = self.expression(start, ExpressionKind::BigIntLiteral(value_utf8));
                PropertyKey {
                    expression,
                    name: None,
                    is_proto: false,
                    is_computed: false,
                    is_identifier: false,
                }
            }
            // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
            // It is a Syntax Error if the StringValue of PrivateIdentifier is "#constructor".
            TokenType::PrivateIdentifier => {
                let token = self.consume();
                let value = Utf16String::from(self.token_value(&token));
                if value == utf16!("#constructor") {
                    self.syntax_error("Private property with name '#constructor' is not allowed");
                }
                // C++ uses the class start position for private identifiers in class elements.
                let key_start = ident_pos_override.unwrap_or(start);
                let expression = self.expression(
                    key_start,
                    ExpressionKind::PrivateIdentifier(PrivateIdentifier {
                        range: self.range_from(key_start),
                        name: value.clone(),
                    }),
                );
                PropertyKey {
                    expression,
                    name: Some(value),
                    is_proto: false,
                    is_computed: false,
                    is_identifier: false,
                }
            }
            _ => {
                if self.match_identifier_name() {
                    // is_identifier is true only for valid identifier references (not just
                    // identifier names). This matters for shorthand properties: { await }
                    // is not valid in class static blocks since await is not an identifier there.
                    let is_ident = self.match_identifier();
                    let token = self.consume();
                    let value = Utf16String::from(self.token_value(&token));
                    let is_proto = value == proto_name;
                    // C++ uses the object expression start position for identifier-name keys.
                    let key_start = ident_pos_override.unwrap_or(start);
                    let expression =
                        self.expression(key_start, ExpressionKind::StringLiteral(value.clone()));
                    PropertyKey {
                        expression,
                        name: Some(value),
                        is_proto,
                        is_computed: false,
                        is_identifier: is_ident,
                    }
                } else {
                    self.expected("property key");
                    self.consume();
                    let expression =
                        self.expression(start, ExpressionKind::StringLiteral(Utf16String::new()));
                    PropertyKey {
                        expression,
                        name: None,
                        is_proto: false,
                        is_computed: false,
                        is_identifier: false,
                    }
                }
            }
        }
    }

    fn parse_array_expression(&mut self) -> Expression {
        let start = self.position();
        self.consume_token(TokenType::BracketOpen);

        let mut elements: Vec<Option<Expression>> = Vec::new();
        while !self.match_token(TokenType::BracketClose) && !self.done() {
            if self.match_token(TokenType::Comma) {
                elements.push(None);
                self.consume();
                continue;
            }
            if self.match_token(TokenType::TripleDot) {
                self.consume();
                let expression = self.parse_assignment_expression();
                // C++ uses the array's rule_start ([ position) for SpreadExpression.
                elements.push(Some(
                    self.expression(start, ExpressionKind::Spread(Box::new(expression))),
                ));
            } else {
                elements.push(Some(self.parse_assignment_expression()));
            }
            if !self.match_token(TokenType::Comma) {
                break;
            }
            self.consume();
        }

        self.consume_token(TokenType::BracketClose);
        self.expression(start, ExpressionKind::Array(elements))
    }

    /// Parse a template literal (`` `...${expression}...` ``).
    // https://tc39.es/ecma262/#sec-template-literals
    // TemplateLiteral : NoSubstitutionTemplate
    //                 | SubstitutionTemplate
    // SubstitutionTemplate : TemplateHead Expression TemplateSpans
    // NB: In tagged templates, invalid escape sequences produce `undefined` for the
    // cooked value instead of a syntax error (sec-template-literals-static-semantics-early-errors).
    /// Consume any tagged template literals following an expression.
    /// Tagged templates bind tighter than any binary operator, so they
    /// are handled outside the normal precedence loop.
    fn parse_tagged_template_literals(
        &mut self,
        tag_start: Position,
        mut expression: Expression,
    ) -> Expression {
        while self.match_token(TokenType::TemplateLiteralStart) {
            let template = self.parse_template_literal(true);
            expression = self.expression(
                tag_start,
                ExpressionKind::TaggedTemplateLiteral {
                    tag: Box::new(expression),
                    template_literal: Box::new(template),
                },
            );
        }
        expression
    }

    pub(crate) fn parse_template_literal(&mut self, is_tagged: bool) -> Expression {
        let start = self.position();
        self.consume_token(TokenType::TemplateLiteralStart);

        let mut expressions = Vec::new();
        let mut raw_strings = Vec::new();

        let needs_leading_empty = !self.match_token(TokenType::TemplateLiteralString);
        if needs_leading_empty {
            if is_tagged {
                raw_strings.push(Utf16String::new());
            }
            expressions
                .push(self.expression(start, ExpressionKind::StringLiteral(Utf16String::new())));
        }

        // For non-tagged templates, we collect parts as expressions (alternating
        // string parts and interpolation expressions). For tagged templates, we
        // also collect raw strings separately.

        loop {
            if self.match_token(TokenType::TemplateLiteralEnd) {
                self.consume();
                break;
            }
            if self.match_token(TokenType::TemplateLiteralString) {
                let token = self.consume();
                // C++ calls parse_string_literal after consume(), so its position
                // is after the template string token. Match that behavior.
                let string_pos = self.position();
                let raw = self.token_value(&token).to_vec();
                if is_tagged {
                    let raw_value = raw_template_value(&raw);
                    raw_strings.push(raw_value);
                    match self.process_template_escape_sequences(&raw) {
                        Some(cooked) => expressions.push(
                            self.expression(string_pos, ExpressionKind::StringLiteral(cooked)),
                        ),
                        // C++ uses rule_start (template literal start) for NullLiteral.
                        None => {
                            expressions.push(self.expression(start, ExpressionKind::NullLiteral))
                        }
                    }
                } else {
                    let (value, has_octal) = self.process_escape_sequences(&raw);
                    if has_octal {
                        self.syntax_error("Octal escape sequence not allowed in template literal");
                    }
                    expressions
                        .push(self.expression(string_pos, ExpressionKind::StringLiteral(value)));
                }
            } else if self.match_token(TokenType::TemplateLiteralExprStart) {
                self.consume();
                let expression = self.parse_expression_any();
                expressions.push(expression);
                self.consume_token(TokenType::TemplateLiteralExprEnd);
                // After an expression, if no template string follows, insert empty.
                if !self.match_token(TokenType::TemplateLiteralString) {
                    expressions.push(
                        self.expression(start, ExpressionKind::StringLiteral(Utf16String::new())),
                    );
                    if is_tagged {
                        raw_strings.push(Utf16String::new());
                    }
                }
            } else if self.done() {
                self.expected("template literal end");
                break;
            } else {
                self.consume();
            }
        }

        self.expression(
            start,
            ExpressionKind::TemplateLiteral(TemplateLiteralData {
                expressions,
                raw_strings,
            }),
        )
    }

    fn process_template_escape_sequences(&self, raw: &[u16]) -> Option<Utf16String> {
        let result = process_escape_sequences_impl(raw, EscapeMode::TaggedTemplate);
        if result.failed {
            None
        } else {
            Some(result.value)
        }
    }

    /// Parse a string literal token's value, processing escape sequences.
    /// Returns `(value, has_legacy_octal)`.
    pub(crate) fn parse_string_value(&mut self, token: &Token) -> (Utf16String, bool) {
        let raw = self.token_value(token).to_vec();
        if raw.len() < 2 {
            return (Utf16String::default(), false);
        }
        self.process_escape_sequences(&raw[1..raw.len() - 1])
    }

    pub(crate) fn process_escape_sequences(&mut self, inner: &[u16]) -> (Utf16String, bool) {
        let result = process_escape_sequences_impl(inner, EscapeMode::StringLiteral);
        if result.malformed_hex {
            self.syntax_error("Malformed hexadecimal escape sequence");
        }
        if result.malformed_unicode {
            self.syntax_error("Malformed unicode escape sequence");
        }
        (result.value, result.has_legacy_octal)
    }
}

struct EscapeResult {
    value: Utf16String,
    has_legacy_octal: bool,
    /// Tagged template encountered an invalid escape.
    failed: bool,
    malformed_hex: bool,
    malformed_unicode: bool,
}

/// Unified escape sequence processor for both string and template literals.
///
/// In `TaggedTemplate` mode, `failed` is true when an invalid escape is
/// encountered (the cooked value becomes `undefined`).
fn process_escape_sequences_impl(input: &[u16], mode: EscapeMode) -> EscapeResult {
    let mut result = Utf16String(Vec::with_capacity(input.len()));
    let mut has_legacy_octal = false;
    let mut malformed_hex = false;
    let mut malformed_unicode = false;
    let mut i = 0;

    const N: u16 = ch(b'n');
    const R: u16 = ch(b'r');
    const T: u16 = ch(b't');
    const B: u16 = ch(b'b');
    const F: u16 = ch(b'f');
    const V: u16 = ch(b'v');
    const ZERO: u16 = ch(b'0');
    const ONE: u16 = ch(b'1');
    const SEVEN: u16 = ch(b'7');
    const EIGHT: u16 = ch(b'8');
    const NINE: u16 = ch(b'9');
    const X: u16 = ch(b'x');
    const U: u16 = ch(b'u');
    const LF: u16 = ch(b'\n');
    const CR: u16 = ch(b'\r');
    const LS: u16 = 0x2028;
    const PS: u16 = 0x2029;

    while i < input.len() {
        if input[i] == b'\\' as u16 && i + 1 < input.len() {
            i += 1;
            match input[i] {
                N => result.0.push(ch(b'\n')),
                R => result.0.push(ch(b'\r')),
                T => result.0.push(ch(b'\t')),
                B => result.0.push(8),
                F => result.0.push(12),
                V => result.0.push(11),
                ZERO => {
                    if mode == EscapeMode::TaggedTemplate {
                        if i + 1 < input.len()
                            && (is_octal_char(input[i + 1])
                                || input[i + 1] == EIGHT
                                || input[i + 1] == NINE)
                        {
                            return EscapeResult {
                                value: result,
                                has_legacy_octal: false,
                                failed: true,
                                malformed_hex: false,
                                malformed_unicode: false,
                            };
                        }
                        result.0.push(0);
                    } else if i + 1 < input.len() && is_octal_char(input[i + 1]) {
                        has_legacy_octal = true;
                        let (val, consumed) = parse_octal_escape(input, i);
                        result.0.push(val);
                        i += consumed;
                    } else if i + 1 < input.len() && (input[i + 1] == EIGHT || input[i + 1] == NINE)
                    {
                        has_legacy_octal = true;
                        result.0.push(0);
                    } else {
                        result.0.push(0);
                    }
                }
                ONE..=SEVEN => {
                    if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: result,
                            has_legacy_octal: false,
                            failed: true,
                            malformed_hex: false,
                            malformed_unicode: false,
                        };
                    }
                    has_legacy_octal = true;
                    let (val, consumed) = parse_octal_escape(input, i);
                    result.0.push(val);
                    i += consumed;
                }
                EIGHT | NINE => {
                    if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: result,
                            has_legacy_octal: false,
                            failed: true,
                            malformed_hex: false,
                            malformed_unicode: false,
                        };
                    }
                    has_legacy_octal = true;
                    result.0.push(input[i]);
                }
                X => {
                    if let Some((advance, ch)) = parse_hex_escape(input, i) {
                        result.0.push(ch);
                        i += advance;
                    } else if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: result,
                            has_legacy_octal: false,
                            failed: true,
                            malformed_hex: false,
                            malformed_unicode: false,
                        };
                    } else {
                        malformed_hex = true;
                        result.0.push(input[i]);
                    }
                }
                U => {
                    if let Some((advance, code_point)) = parse_unicode_escape(input, i) {
                        push_code_point(&mut result.0, code_point);
                        i += advance;
                    } else if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: result,
                            has_legacy_octal: false,
                            failed: true,
                            malformed_hex: false,
                            malformed_unicode: false,
                        };
                    } else {
                        malformed_unicode = true;
                        result.0.push(input[i]);
                    }
                }
                LF => { /* line continuation */ }
                CR => {
                    if i + 1 < input.len() && input[i + 1] == LF {
                        i += 1;
                    }
                }
                LS | PS => { /* skip LS/PS */ }
                c => result.0.push(c),
            }
        } else if input[i] == ch(b'\r') {
            // Normalize \r\n and bare \r to \n per spec (12.9.6).
            result.0.push(ch(b'\n'));
            if i + 1 < input.len() && input[i + 1] == ch(b'\n') {
                i += 1;
            }
        } else {
            result.0.push(input[i]);
        }
        i += 1;
    }
    EscapeResult {
        value: result,
        has_legacy_octal,
        failed: false,
        malformed_hex,
        malformed_unicode,
    }
}

impl<'a> Parser<'a> {
    /// Try to parse an arrow function expression. Returns `None` on failure.
    // https://tc39.es/ecma262/#sec-arrow-function-definitions
    // ArrowFunction : ArrowParameters [no LineTerminator here] `=>` ConciseBody
    // ConciseBody   : [lookahead != `{`] ExpressionBody
    //               | `{` FunctionBody `}`
    pub(crate) fn try_parse_arrow_function_expression(
        &mut self,
        expect_parens: bool,
        is_async: bool,
    ) -> Option<Expression> {
        self.try_parse_arrow_function_expression_impl(expect_parens, is_async, None)
    }

    pub(crate) fn try_parse_arrow_function_expression_impl(
        &mut self,
        expect_parens: bool,
        is_async: bool,
        source_start_override: Option<Position>,
    ) -> Option<Expression> {
        let start = source_start_override.unwrap_or_else(|| self.position());

        if !expect_parens && !is_async {
            if !self.match_identifier()
                && !self.match_token(TokenType::Await)
                && !self.match_token(TokenType::Yield)
            {
                return None;
            }
            let next = self.next_token();
            if next.token_type != TokenType::Arrow || next.trivia_has_line_terminator {
                return None;
            }
        }

        // Save and clear pattern_bound_names so that parameter and body
        // parsing inside this arrow function doesn't steal binding names
        // accumulated by an outer binding pattern context.
        let saved_pattern_bound_names = std::mem::take(&mut self.pattern_bound_names);

        self.save_state();

        // Reset in_formal_parameter_context so that yield/await inside
        // arrow function bodies nested in parameter defaults are not
        // rejected.  (load_state restores flags on error paths.)
        let saved_formal_parameter_ctx = self.flags.in_formal_parameter_context;
        self.flags.in_formal_parameter_context = false;

        if is_async {
            self.consume(); // consume 'async'
            if self.current_token.trivia_has_line_terminator {
                self.pattern_bound_names = saved_pattern_bound_names;
                self.load_state();
                return None;
            }
            if expect_parens {
                self.consume_token(TokenType::ParenOpen);
            }
        }

        // Open function scope before parsing parameters so that default
        // value expressions are resolved inside the function scope.
        // save_state() above captured scope collector state, so any
        // load_state() rollback will undo this.
        self.scope_collector.open_function_scope(None);
        self.scope_collector.set_is_arrow_function();

        // Set await_expression_is_valid during parameter parsing so that
        // 'await' is rejected as an identifier in async arrow parameters.
        let saved_await = self.flags.await_expression_is_valid;
        let saved_static_init = self.flags.in_class_static_init_block;
        if is_async {
            self.flags.await_expression_is_valid = true;
        }
        // NB: Do NOT clear in_class_static_init_block here. Arrow functions don't
        // create a new `await` boundary, so `await` in parameter defaults must still
        // be rejected inside class static initializer blocks. The flag is cleared
        // below, after parameter parsing, for the arrow body only.

        let parsed;

        if expect_parens {
            let previous_errors = self.errors.len();
            parsed = self.parse_formal_parameters_impl(true);
            if self.errors.len() > previous_errors {
                self.pattern_bound_names = saved_pattern_bound_names;
                self.load_state();
                return None;
            }
            if !self.match_token(TokenType::ParenClose) {
                self.pattern_bound_names = saved_pattern_bound_names;
                self.load_state();
                return None;
            }
            self.consume(); // consume ')'
        } else if self.match_identifier() || self.match_token(TokenType::Await) {
            let token = self.consume();
            let value = self.token_value(&token).to_vec();
            if is_async && value == utf16!("await") {
                self.syntax_error("'await' is a reserved identifier in async functions");
            }
            // C++ uses rule_start (arrow function start, which is `async` for async arrows).
            let binding = Rc::new(Identifier::new(
                self.range_from(start),
                value.clone().into(),
            ));
            parsed = ParsedParameters {
                parameters: vec![FunctionParameter {
                    binding: FunctionParameterBinding::Identifier(binding.clone()),
                    default_value: None,
                    is_rest: false,
                }],
                function_length: 1,
                parameter_info: vec![ParamInfo {
                    name: value.into(),
                    is_rest: false,
                    is_from_pattern: false,
                    identifier: Some(binding),
                }],
                is_simple: true,
            };
        } else {
            self.flags.await_expression_is_valid = saved_await;
            self.pattern_bound_names = saved_pattern_bound_names;
            self.load_state();
            return None;
        }

        // Restore await flag during arrow-check; it will be set
        // again for the body below.
        self.flags.await_expression_is_valid = saved_await;

        // [no LineTerminator here] before `=>`
        if !self.match_token(TokenType::Arrow) || self.current_token.trivia_has_line_terminator {
            self.pattern_bound_names = saved_pattern_bound_names;
            self.load_state();
            return None;
        }
        self.consume(); // consume =>

        self.discard_saved_state();

        let ParsedParameters {
            parameters,
            function_length,
            parameter_info,
            is_simple,
        } = parsed;

        // Arrow functions always reject duplicate parameter names.
        self.check_arrow_duplicate_parameters(&parameter_info);

        self.register_function_parameters_with_scope(&parameters, &parameter_info);

        let fn_kind = if is_async {
            FunctionKind::Async
        } else {
            FunctionKind::Normal
        };
        let src_start = source_start_override.unwrap_or(start).offset;

        // Set context flags for the arrow body.
        let saved_await_body = self.flags.await_expression_is_valid;
        self.flags.await_expression_is_valid = is_async;
        self.flags.in_class_static_init_block = false;

        if self.match_token(TokenType::CurlyOpen) {
            let (body, has_use_strict, insights) =
                self.parse_function_body(is_async, false, is_simple);

            self.scope_collector.close_scope();
            self.pattern_bound_names = saved_pattern_bound_names;

            if has_use_strict || fn_kind != FunctionKind::Normal {
                self.check_parameters_post_body(&parameter_info, has_use_strict, fn_kind);
            }

            self.flags.await_expression_is_valid = saved_await_body;
            self.flags.in_class_static_init_block = saved_static_init;
            self.flags.in_formal_parameter_context = saved_formal_parameter_ctx;
            let function_id = self.function_table.insert(FunctionData {
                name: None,
                source_text_start: src_start,
                source_text_end: self.source_text_end_offset(),
                body: Box::new(body),
                parameters,
                function_length,
                kind: fn_kind,
                is_strict_mode: self.flags.strict_mode || has_use_strict,
                is_arrow_function: true,
                parsing_insights: insights,
            });
            Some(self.expression(start, ExpressionKind::Function(function_id)))
        } else {
            let expression = self.parse_assignment_expression();
            // C++ uses rule_start (function start) for ReturnStatement and FunctionBody.
            let return_statement = Statement::new(
                self.range_from(start),
                StatementKind::Return(Some(Box::new(expression))),
            );
            let scope = ScopeData::shared_with_children(vec![return_statement]);
            self.scope_collector.set_scope_node(scope.clone());
            let body = Statement::new(
                self.range_from(start),
                StatementKind::FunctionBody {
                    scope,
                    in_strict_mode: self.flags.strict_mode,
                },
            );

            // C++ only sets contains_direct_call_to_eval and uses_this_from_environment
            // for expression-body arrows (not uses_this).
            let insights = FunctionParsingInsights {
                contains_direct_call_to_eval: self.scope_collector.contains_direct_call_to_eval(),
                uses_this_from_environment: self.scope_collector.uses_this_from_environment(),
                ..FunctionParsingInsights::default()
            };

            self.scope_collector.close_scope();
            self.pattern_bound_names = saved_pattern_bound_names;

            self.flags.await_expression_is_valid = saved_await_body;
            self.flags.in_class_static_init_block = saved_static_init;
            self.flags.in_formal_parameter_context = saved_formal_parameter_ctx;
            let function_id = self.function_table.insert(FunctionData {
                name: None,
                source_text_start: src_start,
                source_text_end: self.source_text_end_offset(),
                body: Box::new(body),
                parameters,
                function_length,
                kind: fn_kind,
                is_strict_mode: self.flags.strict_mode,
                is_arrow_function: true,
                parsing_insights: insights,
            });
            Some(self.expression(start, ExpressionKind::Function(function_id)))
        }
    }

    pub(crate) fn parse_method_definition(
        &mut self,
        is_async: bool,
        is_generator: bool,
        method_kind: MethodKind,
        function_start: Position,
    ) -> Expression {
        let start = function_start;

        let saved_might_need_arguments = self.flags.function_might_need_arguments_object;
        self.flags.function_might_need_arguments_object = false;

        let fn_kind = FunctionKind::from_async_generator(is_async, is_generator);

        // Open function scope for method.
        self.scope_collector.open_function_scope(None);

        let in_generator_before = self.flags.in_generator_function_context;
        let await_before = self.flags.await_expression_is_valid;
        let saved_static_init = self.flags.in_class_static_init_block;
        let saved_field_init = self.flags.in_class_field_initializer;
        let saved_allow_super_call = self.flags.allow_super_constructor_call;
        let saved_allow_super_lookup = self.flags.allow_super_property_lookup;
        self.flags.in_generator_function_context = is_generator;
        self.flags.await_expression_is_valid = is_async;
        self.flags.in_class_static_init_block = false;
        self.flags.in_class_field_initializer = false;
        self.flags.allow_super_constructor_call =
            method_kind == MethodKind::Constructor && self.class_has_super_class;
        self.flags.allow_super_property_lookup = true;

        // Save pattern_bound_names so that destructuring patterns in the
        // method body don't steal names from an outer binding context.
        let saved_pattern_bound_names = std::mem::take(&mut self.pattern_bound_names);

        let parsed = self.parse_formal_parameters();

        self.register_function_parameters_with_scope(&parsed.parameters, &parsed.parameter_info);

        if method_kind == MethodKind::Getter && !parsed.parameters.is_empty() {
            self.syntax_error("Getter function must have no arguments");
        }
        if method_kind == MethodKind::Setter
            && (parsed.parameters.len() != 1
                || parsed.parameters.first().is_some_and(|p| p.is_rest))
        {
            self.syntax_error("Setter function must have one argument");
        }

        self.flags.in_generator_function_context = in_generator_before;
        self.flags.await_expression_is_valid = await_before;

        let (body, has_use_strict, mut insights) =
            self.parse_function_body(is_async, is_generator, parsed.is_simple);
        self.flags.allow_super_constructor_call = saved_allow_super_call;
        self.flags.allow_super_property_lookup = saved_allow_super_lookup;

        self.scope_collector.close_scope();
        self.pattern_bound_names = saved_pattern_bound_names;

        self.flags.in_class_static_init_block = saved_static_init;
        self.flags.in_class_field_initializer = saved_field_init;

        if has_use_strict || fn_kind != FunctionKind::Normal {
            self.check_parameters_post_body(&parsed.parameter_info, has_use_strict, fn_kind);
        }

        insights.might_need_arguments_object = self.flags.function_might_need_arguments_object;
        self.flags.function_might_need_arguments_object = saved_might_need_arguments;

        // Class constructors always need a function environment for `this` binding
        // management (super() binds this in derived constructors, and base constructors
        // need it for OrdinaryCallBindThis).
        if method_kind == MethodKind::Constructor {
            insights.uses_this = true;
            insights.uses_this_from_environment = true;
        }

        let function_id = self.function_table.insert(FunctionData {
            name: None,
            source_text_start: function_start.offset,
            source_text_end: self.source_text_end_offset(),
            body: Box::new(body),
            parameters: parsed.parameters,
            function_length: parsed.function_length,
            kind: fn_kind,
            is_strict_mode: self.flags.strict_mode || has_use_strict,
            is_arrow_function: false,
            parsing_insights: insights,
        });
        self.expression(start, ExpressionKind::Function(function_id))
    }
}

fn hex_digit(c: u16) -> Option<u16> {
    match c {
        0x30..=0x39 => Some(c - 0x30),
        0x41..=0x46 => Some(c - 0x41 + 10),
        0x61..=0x66 => Some(c - 0x61 + 10),
        _ => None,
    }
}

fn is_octal_char(c: u16) -> bool {
    c >= ch(b'0') && c <= ch(b'7')
}

fn parse_octal_escape(inner: &[u16], i: usize) -> (u16, usize) {
    let first = (inner[i] - ch(b'0')) as u32;
    let mut value = first;
    let mut consumed = 0;

    if i + 1 < inner.len() && is_octal_char(inner[i + 1]) {
        value = value * 8 + (inner[i + 1] - ch(b'0')) as u32;
        consumed = 1;

        if i + 2 < inner.len() && is_octal_char(inner[i + 2]) && first <= 3 {
            value = value * 8 + (inner[i + 2] - ch(b'0')) as u32;
            consumed = 2;
        }
    }
    (value as u16, consumed)
}

fn parse_hex_escape(raw: &[u16], i: usize) -> Option<(usize, u16)> {
    if i + 2 >= raw.len() {
        return None;
    }
    let high = hex_digit(raw[i + 1])?;
    let low = hex_digit(raw[i + 2])?;
    Some((2, high * 16 + low))
}

fn parse_unicode_escape(raw: &[u16], i: usize) -> Option<(usize, u32)> {
    if i + 1 >= raw.len() {
        return None;
    }
    if raw[i + 1] == ch(b'{') {
        let mut j = i + 2;
        let mut value: u32 = 0;
        let mut digits = 0;
        while j < raw.len() && raw[j] != ch(b'}') {
            let d = hex_digit(raw[j])? as u32;
            value = value * 16 + d;
            if value > 0x10FFFF {
                return None;
            }
            digits += 1;
            j += 1;
        }
        if j >= raw.len() || digits == 0 {
            return None;
        }
        Some((j - i, value))
    } else {
        if i + 4 >= raw.len() {
            return None;
        }
        let d0 = hex_digit(raw[i + 1])? as u32;
        let d1 = hex_digit(raw[i + 2])? as u32;
        let d2 = hex_digit(raw[i + 3])? as u32;
        let d3 = hex_digit(raw[i + 4])? as u32;
        Some((4, (d0 << 12) | (d1 << 8) | (d2 << 4) | d3))
    }
}

fn push_code_point(result: &mut Vec<u16>, code_point: u32) {
    if code_point > 0xFFFF {
        let adjusted = code_point - 0x10000;
        result.push((0xD800 | ((adjusted >> 10) & 0x3FF)) as u16);
        result.push((0xDC00 | (adjusted & 0x3FF)) as u16);
    } else {
        result.push(code_point as u16);
    }
}

fn raw_template_value(raw: &[u16]) -> Utf16String {
    let mut result = Utf16String(Vec::with_capacity(raw.len()));
    let mut i = 0;
    while i < raw.len() {
        if raw[i] == ch(b'\r') {
            result.0.push(ch(b'\n'));
            if i + 1 < raw.len() && raw[i + 1] == ch(b'\n') {
                i += 1;
            }
        } else {
            result.0.push(raw[i]);
        }
        i += 1;
    }
    result
}

fn token_to_binary_op(tt: TokenType) -> BinaryOp {
    match tt {
        TokenType::Plus => BinaryOp::Addition,
        TokenType::Minus => BinaryOp::Subtraction,
        TokenType::Asterisk => BinaryOp::Multiplication,
        TokenType::Slash => BinaryOp::Division,
        TokenType::Percent => BinaryOp::Modulo,
        TokenType::DoubleAsterisk => BinaryOp::Exponentiation,
        TokenType::EqualsEqualsEquals => BinaryOp::StrictlyEquals,
        TokenType::ExclamationMarkEqualsEquals => BinaryOp::StrictlyInequals,
        TokenType::EqualsEquals => BinaryOp::LooselyEquals,
        TokenType::ExclamationMarkEquals => BinaryOp::LooselyInequals,
        TokenType::GreaterThan => BinaryOp::GreaterThan,
        TokenType::GreaterThanEquals => BinaryOp::GreaterThanEquals,
        TokenType::LessThan => BinaryOp::LessThan,
        TokenType::LessThanEquals => BinaryOp::LessThanEquals,
        TokenType::Ampersand => BinaryOp::BitwiseAnd,
        TokenType::Pipe => BinaryOp::BitwiseOr,
        TokenType::Caret => BinaryOp::BitwiseXor,
        TokenType::ShiftLeft => BinaryOp::LeftShift,
        TokenType::ShiftRight => BinaryOp::RightShift,
        TokenType::UnsignedShiftRight => BinaryOp::UnsignedRightShift,
        TokenType::In => BinaryOp::In,
        TokenType::Instanceof => BinaryOp::InstanceOf,
        _ => unreachable!("unexpected token {:?} in binary expression", tt),
    }
}

fn token_to_assignment_op(tt: TokenType) -> AssignmentOp {
    match tt {
        TokenType::Equals => AssignmentOp::Assignment,
        TokenType::PlusEquals => AssignmentOp::AdditionAssignment,
        TokenType::MinusEquals => AssignmentOp::SubtractionAssignment,
        TokenType::AsteriskEquals => AssignmentOp::MultiplicationAssignment,
        TokenType::SlashEquals => AssignmentOp::DivisionAssignment,
        TokenType::PercentEquals => AssignmentOp::ModuloAssignment,
        TokenType::DoubleAsteriskEquals => AssignmentOp::ExponentiationAssignment,
        TokenType::AmpersandEquals => AssignmentOp::BitwiseAndAssignment,
        TokenType::PipeEquals => AssignmentOp::BitwiseOrAssignment,
        TokenType::CaretEquals => AssignmentOp::BitwiseXorAssignment,
        TokenType::ShiftLeftEquals => AssignmentOp::LeftShiftAssignment,
        TokenType::ShiftRightEquals => AssignmentOp::RightShiftAssignment,
        TokenType::UnsignedShiftRightEquals => AssignmentOp::UnsignedRightShiftAssignment,
        TokenType::DoubleAmpersandEquals => AssignmentOp::AndAssignment,
        TokenType::DoublePipeEquals => AssignmentOp::OrAssignment,
        TokenType::DoubleQuestionMarkEquals => AssignmentOp::NullishAssignment,
        _ => unreachable!("unexpected token {:?} in assignment expression", tt),
    }
}

pub(crate) fn parse_numeric_value(value: &[u16]) -> f64 {
    let s: String = value
        .iter()
        .filter(|&&c| c != '_' as u16)
        .map(|&c| c as u8 as char)
        .collect();

    if s.starts_with("0x") || s.starts_with("0X") {
        parse_integer_with_radix(&s[2..], 16)
    } else if s.starts_with("0o") || s.starts_with("0O") {
        parse_integer_with_radix(&s[2..], 8)
    } else if s.starts_with("0b") || s.starts_with("0B") {
        parse_integer_with_radix(&s[2..], 2)
    } else if s.starts_with('0') && s.len() > 1 && s.as_bytes()[1].is_ascii_digit() {
        let digits = &s[1..];
        if digits.bytes().all(|b| (b'0'..=b'7').contains(&b)) {
            parse_integer_with_radix(digits, 8)
        } else {
            s.parse::<f64>().unwrap_or(f64::NAN)
        }
    } else {
        s.parse::<f64>().unwrap_or(f64::NAN)
    }
}

fn parse_integer_with_radix(digits: &str, radix: u32) -> f64 {
    if let Ok(v) = u64::from_str_radix(digits, radix) {
        return v as f64;
    }
    let mut result: f64 = 0.0;
    for ch in digits.chars() {
        let digit = ch.to_digit(radix);
        if let Some(d) = digit {
            result = result * (radix as f64) + (d as f64);
        }
    }
    result
}
