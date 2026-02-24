/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Statement parsing: if, for, while, switch, try, etc.

use std::rc::Rc;

use crate::ast::*;
use crate::parser::{Associativity, ForbiddenTokens, PRECEDENCE_COMMA, Parser, Position};
use crate::token::TokenType;

/// Used locally during for-statement parsing before converting to `ast::ForInit`.
enum LocalForInit {
    Declaration(Statement),
    Expression(Expression),
}

impl<'a> Parser<'a> {
    pub(crate) fn parse_statement(&mut self, allow_labelled_function: bool) -> Statement {
        let start = self.position();
        let tt = self.current_token_type();

        match tt {
            TokenType::CurlyOpen => self.parse_block_statement(),
            TokenType::Return => self.parse_return_statement(),
            TokenType::Var => self.parse_variable_declaration(false),
            TokenType::For => self.parse_for_statement(),
            TokenType::If => self.parse_if_statement(),
            TokenType::Throw => self.parse_throw_statement(),
            TokenType::Try => self.parse_try_statement(),
            TokenType::Break => self.parse_break_statement(),
            TokenType::Continue => self.parse_continue_statement(),
            TokenType::Switch => self.parse_switch_statement(),
            TokenType::Do => self.parse_do_while_statement(),
            TokenType::While => self.parse_while_statement(),
            TokenType::With => {
                if self.flags.strict_mode {
                    self.syntax_error("'with' statement not allowed in strict mode");
                }
                self.parse_with_statement()
            }
            TokenType::Debugger => self.parse_debugger_statement(),
            TokenType::Semicolon => {
                self.consume();
                self.statement(start, StatementKind::Empty)
            }
            TokenType::Slash | TokenType::SlashEquals => {
                let token = self.lexer.force_slash_as_regex();
                self.current_token = token;
                self.parse_expression_statement()
            }
            _ => {
                if self.match_invalid_escaped_keyword() {
                    self.syntax_error("Keyword must not contain escaped characters");
                }
                if self.match_identifier_name()
                    && let Some(labelled) =
                        self.try_parse_labelled_statement(allow_labelled_function)
                {
                    return labelled;
                }
                if self.match_expression() {
                    self.parse_expression_statement()
                } else {
                    self.expected("statement");
                    self.consume();
                    self.statement(start, StatementKind::Empty)
                }
            }
        }
    }

    pub(crate) fn parse_block_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::CurlyOpen);

        self.scope_collector.open_block_scope(None);

        let mut children = Vec::new();

        while !self.match_token(TokenType::CurlyClose) && !self.done() {
            if self.match_declaration() {
                children.push(self.parse_declaration());
            } else {
                children.push(self.parse_statement(true));
            }
        }

        self.consume_token(TokenType::CurlyClose);
        let scope = ScopeData::shared_with_children(children);
        self.scope_collector.set_scope_node(scope.clone());
        self.scope_collector.close_scope();
        self.statement(start, StatementKind::Block(scope))
    }

    fn parse_expression_statement(&mut self) -> Statement {
        let start = self.position();

        if self.match_token(TokenType::Async) {
            let lookahead = self.next_token();
            if lookahead.token_type == TokenType::Function && !lookahead.trivia_has_line_terminator
            {
                self.syntax_error(
                    "Async function declaration not allowed in single-statement context",
                );
            }
        } else if self.match_token(TokenType::Function) || self.match_token(TokenType::Class) {
            let name = self.current_token.token_type.name();
            self.syntax_error(&format!(
                "{} declaration not allowed in single-statement context",
                name
            ));
        } else if self.match_token(TokenType::Let)
            && self.next_token().token_type == TokenType::BracketOpen
        {
            self.syntax_error("let followed by [ is not allowed in single-statement context");
        }

        let expression = self.parse_expression_any();
        self.consume_or_insert_semicolon();
        self.statement(start, StatementKind::Expression(Box::new(expression)))
    }

    // https://tc39.es/ecma262/#sec-return-statement
    // ReturnStatement : `return` [no LineTerminator here] Expression? `;`
    fn parse_return_statement(&mut self) -> Statement {
        let start = self.position();
        if !self.flags.in_function_context {
            self.syntax_error("'return' not allowed outside of a function");
        }
        self.consume_token(TokenType::Return);

        // [no LineTerminator here]: if a line terminator follows `return`,
        // ASI inserts a semicolon and the return has no argument.
        // NB: Don't consume the next token — it may be `;` which should
        // become an EmptyStatement, not be swallowed by the return.
        if self.current_token.trivia_has_line_terminator {
            return self.statement(start, StatementKind::Return(None));
        }
        if self.match_token(TokenType::Semicolon)
            || self.match_token(TokenType::CurlyClose)
            || self.done()
        {
            self.consume_or_insert_semicolon();
            return self.statement(start, StatementKind::Return(None));
        }

        let argument = self.parse_expression_any();
        self.consume_or_insert_semicolon();
        self.statement(start, StatementKind::Return(Some(Box::new(argument))))
    }

    // https://tc39.es/ecma262/#sec-throw-statement
    // ThrowStatement : `throw` [no LineTerminator here] Expression `;`
    fn parse_throw_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Throw);

        // [no LineTerminator here]: unlike `return`, a line terminator after
        // `throw` is always an error because `throw;` is never valid.
        if self.current_token.trivia_has_line_terminator {
            self.syntax_error("No line break is allowed between 'throw' and its expression");
        }

        let argument = self.parse_expression_any();
        self.consume_or_insert_semicolon();
        self.statement(start, StatementKind::Throw(Box::new(argument)))
    }

    // https://tc39.es/ecma262/#sec-break-statement
    // BreakStatement : `break` [no LineTerminator here] LabelIdentifier? `;`
    fn parse_break_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Break);

        let label = if self.match_token(TokenType::Semicolon) {
            self.consume();
            None
        } else if !self.current_token.trivia_has_line_terminator
            && !self.match_token(TokenType::CurlyClose)
            && !self.done()
            && self.match_identifier()
        {
            let token = self.consume();
            let label_value = Utf16String::from(self.token_value(&token));

            if !self.labels_in_scope.contains_key(label_value.as_slice()) {
                let label_str = String::from_utf16_lossy(&label_value);
                self.syntax_error(&format!("Label '{}' not found", label_str));
            }

            self.consume_or_insert_semicolon();
            Some(label_value)
        } else {
            self.consume_or_insert_semicolon();
            None
        };

        if label.is_none() && !self.flags.in_break_context {
            self.syntax_error(
                "Unlabeled 'break' not allowed outside of a loop or switch statement",
            );
        }

        self.statement(
            start,
            StatementKind::Break {
                target_label: label,
            },
        )
    }

    // https://tc39.es/ecma262/#sec-continue-statement
    // ContinueStatement : `continue` [no LineTerminator here] LabelIdentifier? `;`
    fn parse_continue_statement(&mut self) -> Statement {
        let start = self.position();
        if !self.flags.in_continue_context {
            self.syntax_error("'continue' not allowed outside of a loop");
        }
        self.consume_token(TokenType::Continue);

        let label = if self.match_token(TokenType::Semicolon) {
            None
        } else if !self.current_token.trivia_has_line_terminator
            && !self.match_token(TokenType::CurlyClose)
            && !self.done()
            && self.match_identifier()
        {
            let label_line = self.current_token.line_number;
            let label_col = self.current_token.line_column;
            let token = self.consume();
            let label_value = Utf16String::from(self.token_value(&token));

            if let Some(entry) = self.labels_in_scope.get_mut(label_value.as_slice()) {
                *entry = Some((label_line, label_col));
            } else {
                let label_str = String::from_utf16_lossy(&label_value);
                self.syntax_error(&format!("Label '{}' not found or invalid", label_str));
            }

            Some(label_value)
        } else {
            None
        };

        self.consume_or_insert_semicolon();

        self.statement(
            start,
            StatementKind::Continue {
                target_label: label,
            },
        )
    }

    fn parse_debugger_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Debugger);
        self.consume_or_insert_semicolon();
        self.statement(start, StatementKind::Debugger)
    }

    fn parse_if_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::If);
        self.consume_token(TokenType::ParenOpen);
        let predicate = self.parse_expression_any();
        self.consume_token(TokenType::ParenClose);

        let consequent = if !self.flags.strict_mode && self.match_token(TokenType::Function) {
            self.parse_function_declaration_as_block_statement(start)
        } else {
            self.parse_statement(false)
        };

        let alternate = if self.match_token(TokenType::Else) {
            self.consume();
            if !self.flags.strict_mode && self.match_token(TokenType::Function) {
                Some(Box::new(
                    self.parse_function_declaration_as_block_statement(start),
                ))
            } else {
                Some(Box::new(self.parse_statement(false)))
            }
        } else {
            None
        };

        self.statement(
            start,
            StatementKind::If {
                test: Box::new(predicate),
                consequent: Box::new(consequent),
                alternate,
            },
        )
    }

    /// Annex B: Parse a function declaration as if wrapped in a synthetic block.
    /// See https://tc39.es/ecma262/#sec-functiondeclarations-in-ifstatement-statement-clauses
    fn parse_function_declaration_as_block_statement(&mut self, if_start: Position) -> Statement {
        // C++ uses rule_start from the enclosing if-statement for the block position.
        let start = if_start;
        self.scope_collector.open_block_scope(None);
        let declaration = self.parse_function_declaration();
        let scope = ScopeData::shared_with_children(vec![declaration]);
        self.scope_collector.set_scope_node(scope.clone());
        self.scope_collector.close_scope();
        self.statement(start, StatementKind::Block(scope))
    }

    /// Parse a statement in a loop body context (break and continue allowed).
    fn parse_loop_body(&mut self) -> Statement {
        let break_before = self.flags.in_break_context;
        let continue_before = self.flags.in_continue_context;
        self.flags.in_break_context = true;
        self.flags.in_continue_context = true;
        let body = self.parse_statement(false);
        self.flags.in_break_context = break_before;
        self.flags.in_continue_context = continue_before;
        body
    }

    fn parse_while_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::While);
        self.consume_token(TokenType::ParenOpen);
        let test = self.parse_expression_any();
        self.consume_token(TokenType::ParenClose);

        let body = self.parse_loop_body();

        self.statement(
            start,
            StatementKind::While {
                test: Box::new(test),
                body: Box::new(body),
            },
        )
    }

    fn parse_do_while_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Do);

        let body = self.parse_loop_body();

        self.consume_token(TokenType::While);
        self.consume_token(TokenType::ParenOpen);
        let test = self.parse_expression_any();
        self.consume_token(TokenType::ParenClose);

        // Since ES 2015 a missing semicolon is inserted here, despite
        // the regular ASI rules not applying.
        self.eat(TokenType::Semicolon);

        self.statement(
            start,
            StatementKind::DoWhile {
                test: Box::new(test),
                body: Box::new(body),
            },
        )
    }

    // https://tc39.es/ecma262/#sec-for-statement
    // https://tc39.es/ecma262/#sec-for-in-and-for-of-statements
    fn parse_for_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::For);

        // Open for-loop scope (for let/const/using declarations).
        // scope_data set after the for-loop body is parsed.
        self.scope_collector.open_for_loop_scope(None);

        let is_await = if self.match_token(TokenType::Await) {
            if !self.flags.await_expression_is_valid {
                self.syntax_error("for-await-of not allowed outside of async context");
            }
            self.consume();
            true
        } else {
            false
        };

        self.consume_token(TokenType::ParenOpen);

        if self.match_token(TokenType::Semicolon) && !is_await {
            self.consume();
            let result = self.parse_standard_for_loop(start, None);
            return self.close_for_loop_scope(start, result);
        }

        let init_start = self.position();
        let is_var_init = self.match_token(TokenType::Var);
        let is_using = self.match_for_using_declaration();
        let is_let = self.match_token(TokenType::Let)
            && (self.flags.strict_mode || self.try_match_let_declaration());
        let is_declaration =
            is_var_init || is_using || is_let || self.match_token(TokenType::Const);
        let init = if is_using {
            LocalForInit::Declaration(self.parse_using_declaration(true))
        } else if is_declaration {
            LocalForInit::Declaration(self.parse_variable_declaration(true))
        } else {
            let forbidden = ForbiddenTokens::with_in();
            LocalForInit::Expression(self.parse_expression(
                PRECEDENCE_COMMA,
                Associativity::Right,
                forbidden,
            ))
        };

        // Check for in
        // https://tc39.es/ecma262/#sec-for-in-and-for-of-statements
        // It is a Syntax Error if IsDestructuring of ForBinding is false and
        // Initializer is present. (Only a single, non-initialized ForBinding
        // is allowed; except in Annex B sloppy var with one declarator.)
        if self.match_token(TokenType::In) && !is_await {
            // C++ captures ForInStatement position at the `in` keyword.
            let forin_start = self.position();
            if is_using {
                self.syntax_error("Using declaration not allowed in for-in loop");
            } else if is_declaration {
                if self.for_loop_declaration_count > 1 {
                    self.syntax_error("Multiple declarations not allowed in for..in/of");
                }
                if self.for_loop_declaration_has_init {
                    // https://tc39.es/ecma262/#sec-initializers-in-forin-statement-heads
                    // Annex B: In sloppy mode, a single `var` with an initializer is permitted.
                    if !(self.for_loop_declaration_is_var
                        && self.for_loop_declaration_count == 1
                        && !self.flags.strict_mode)
                    {
                        self.syntax_error("Variable initializer not allowed in for..in/of");
                    }
                }
            } else {
                self.validate_for_in_of_lhs(&init);
            }
            self.consume();
            let rhs = self.parse_expression_any();
            self.consume_token(TokenType::ParenClose);

            let body = self.parse_loop_body();

            let lhs = self.synthesize_for_in_of_lhs(init, init_start);
            let result = self.statement(
                forin_start,
                StatementKind::ForInOf {
                    kind: ForInOfKind::ForIn,
                    lhs,
                    rhs: Box::new(rhs),
                    body: Box::new(body),
                },
            );
            return self.close_for_loop_scope(start, result);
        }

        // Check for of (keyword must not contain escapes)
        if self.match_identifier_name() {
            let value = self.token_original_value(&self.current_token);
            if value == utf16!("of") {
                // C++ captures ForOfStatement position at the `of` keyword.
                let forof_start = self.position();
                if is_declaration {
                    if self.for_loop_declaration_count > 1 {
                        self.syntax_error("Multiple declarations not allowed in for..in/of");
                    }
                    if self.for_loop_declaration_has_init {
                        self.syntax_error("Variable initializer not allowed in for..of");
                    }
                } else {
                    self.validate_for_in_of_lhs(&init);
                    // https://tc39.es/ecma262/#sec-for-in-and-for-of-statements
                    if let LocalForInit::Expression(ref expression) = init
                        && let ExpressionKind::Member { ref object, .. } = expression.inner
                        && let ExpressionKind::Identifier(ref ident) = object.inner
                        && ident.name == utf16!("let")
                    {
                        self.syntax_error("For of statement may not start with let.");
                    }
                }
                self.consume();
                let rhs = self.parse_assignment_expression();
                self.consume_token(TokenType::ParenClose);

                let body = self.parse_loop_body();

                let lhs = self.synthesize_for_in_of_lhs(init, init_start);
                let for_of_kind = if is_await {
                    ForInOfKind::ForAwaitOf
                } else {
                    ForInOfKind::ForOf
                };
                let result = self.statement(
                    forof_start,
                    StatementKind::ForInOf {
                        kind: for_of_kind,
                        lhs,
                        rhs: Box::new(rhs),
                        body: Box::new(body),
                    },
                );
                return self.close_for_loop_scope(start, result);
            }
        }

        // Standard for loop — const requires initializer.
        if let LocalForInit::Declaration(ref declaration) = init
            && let StatementKind::VariableDeclaration {
                kind: DeclarationKind::Const,
                ref declarations,
            } = declaration.inner
        {
            for d in declarations {
                if d.init.is_none() {
                    self.syntax_error("Missing initializer in const declaration");
                }
            }
        }
        self.consume_token(TokenType::Semicolon);
        let for_init = match init {
            LocalForInit::Declaration(declaration) => {
                Some(ForInit::Declaration(Box::new(declaration)))
            }
            LocalForInit::Expression(expression) => Some(ForInit::Expression(Box::new(expression))),
        };
        let result = self.parse_standard_for_loop(start, for_init);
        self.close_for_loop_scope(start, result)
    }

    /// Close the for-loop scope and wrap the for-loop statement in a Block
    /// with scope data.
    fn close_for_loop_scope(&mut self, start: Position, inner: Statement) -> Statement {
        let scope = ScopeData::shared_with_children(vec![inner]);
        self.scope_collector.set_scope_node(scope.clone());
        self.scope_collector.close_scope();
        self.statement(start, StatementKind::Block(scope))
    }

    fn parse_standard_for_loop(&mut self, start: Position, init: Option<ForInit>) -> Statement {
        let test = if self.match_token(TokenType::Semicolon) {
            None
        } else {
            Some(Box::new(self.parse_expression_any()))
        };
        self.consume_token(TokenType::Semicolon);

        let update = if self.match_token(TokenType::ParenClose) {
            None
        } else {
            Some(Box::new(self.parse_expression_any()))
        };
        self.consume_token(TokenType::ParenClose);

        let body = self.parse_loop_body();

        self.statement(
            start,
            StatementKind::For {
                init,
                test,
                update,
                body: Box::new(body),
            },
        )
    }

    // https://tc39.es/ecma262/#sec-with-statement
    // NOTE: The with statement is forbidden in strict mode code.
    fn parse_with_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::With);
        self.consume_token(TokenType::ParenOpen);
        let object = self.parse_expression_any();
        self.consume_token(TokenType::ParenClose);
        self.scope_collector.open_with_scope(None);
        let body = self.parse_statement(false);
        self.scope_collector.close_scope();
        self.statement(
            start,
            StatementKind::With {
                object: Box::new(object),
                body: Box::new(body),
            },
        )
    }

    // https://tc39.es/ecma262/#sec-switch-statement
    // All case clauses in a switch statement share a single block scope.
    fn parse_switch_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Switch);
        self.consume_token(TokenType::ParenOpen);
        let discriminant = self.parse_expression_any();
        self.consume_token(TokenType::ParenClose);

        self.consume_token(TokenType::CurlyOpen);

        self.scope_collector.open_block_scope(None);

        let break_before = self.flags.in_break_context;
        self.flags.in_break_context = true;

        let mut cases = Vec::new();
        let mut has_default = false;
        while !self.match_token(TokenType::CurlyClose) && !self.done() {
            let case = self.parse_switch_case();
            if case.test.is_none() {
                if has_default {
                    self.syntax_error("Multiple 'default' clauses in switch statement");
                }
                has_default = true;
            }
            cases.push(case);
        }

        self.flags.in_break_context = break_before;

        self.consume_token(TokenType::CurlyClose);

        let scope = ScopeData::new_shared();
        self.scope_collector.set_scope_node(scope.clone());
        self.scope_collector.close_scope();

        self.statement(
            start,
            StatementKind::Switch(SwitchStatementData {
                scope,
                discriminant: Box::new(discriminant),
                cases,
            }),
        )
    }

    fn parse_switch_case(&mut self) -> SwitchCase {
        let start = self.position();
        let test = if self.match_token(TokenType::Case) {
            self.consume();
            Some(self.parse_expression_any())
        } else if self.match_token(TokenType::Default) {
            self.consume();
            None
        } else {
            self.expected("'case' or 'default'");
            None
        };

        self.consume_token(TokenType::Colon);

        let mut children = Vec::new();
        while !self.match_token(TokenType::CurlyClose)
            && !self.match_token(TokenType::Case)
            && !self.match_token(TokenType::Default)
            && !self.done()
        {
            if self.match_declaration() {
                children.push(self.parse_declaration());
            } else {
                children.push(self.parse_statement(true));
            }
        }

        SwitchCase {
            range: self.range_from(start),
            scope: ScopeData::shared_with_children(children),
            test,
        }
    }

    // https://tc39.es/ecma262/#sec-try-statement
    // TryStatement :
    //   `try` Block Catch
    //   `try` Block Finally
    //   `try` Block Catch Finally
    fn parse_try_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Try);

        let block = self.parse_block_statement();

        let handler = if self.match_token(TokenType::Catch) {
            Some(self.parse_catch_clause())
        } else {
            None
        };

        let finalizer = if self.match_token(TokenType::Finally) {
            self.consume();
            Some(Box::new(self.parse_block_statement()))
        } else {
            None
        };

        if handler.is_none() && finalizer.is_none() {
            self.syntax_error("try statement must have a catch or finally clause");
        }

        self.statement(
            start,
            StatementKind::Try(TryStatementData {
                block: Box::new(block),
                handler,
                finalizer,
            }),
        )
    }

    // https://tc39.es/ecma262/#sec-try-statement
    // Catch : `catch` `(` CatchParameter `)` Block
    //       | `catch` Block
    // The catch parameter creates its own scope that wraps the block body.
    fn parse_catch_clause(&mut self) -> CatchClause {
        let start = self.position();
        self.consume_token(TokenType::Catch);

        self.scope_collector.open_catch_scope();

        let parameter = if self.match_token(TokenType::ParenOpen) {
            self.consume();
            let parameter = if self.match_token(TokenType::CurlyOpen)
                || self.match_token(TokenType::BracketOpen)
            {
                self.pattern_bound_names.clear();
                let pattern = self.parse_binding_pattern();
                let names_to_check: Vec<Utf16String> = self
                    .pattern_bound_names
                    .iter()
                    .map(|(n, _)| n.clone())
                    .collect();
                for name in &names_to_check {
                    self.check_identifier_name_for_assignment_validity(name, false);
                }
                let bound_names: Vec<&[u16]> = self
                    .pattern_bound_names
                    .iter()
                    .map(|(n, _)| n.as_slice())
                    .collect();
                self.scope_collector
                    .add_catch_parameter_pattern(&bound_names);
                // Register each binding pattern identifier for scope analysis
                // so they get is_local() annotations (matching variable declarations).
                for (name, id) in &self.pattern_bound_names {
                    self.scope_collector
                        .register_identifier(id.clone(), name, None);
                }
                Some(CatchBinding::BindingPattern(pattern))
            } else if self.match_identifier() {
                let parameter_start = self.position();
                let token = self.consume();
                let value = self.token_value(&token).to_vec();
                self.check_identifier_name_for_assignment_validity(&value, false);
                let id = Rc::new(Identifier::new(
                    self.range_from(parameter_start),
                    value.clone().into(),
                ));
                self.scope_collector
                    .register_identifier(id.clone(), &value, None);
                self.scope_collector
                    .add_catch_parameter_identifier(&value, id.clone());
                Some(CatchBinding::Identifier(id))
            } else {
                self.expected("catch parameter");
                None
            };
            self.consume_token(TokenType::ParenClose);
            parameter
        } else {
            None
        };

        let body = self.parse_block_statement();

        self.scope_collector.close_scope();

        CatchClause {
            range: self.range_from(start),
            parameter,
            body: Box::new(body),
        }
    }

    // https://tc39.es/ecma262/#sec-labelled-statements
    // It is a Syntax Error if any source text is matched by this production
    // and a `continue` statement with label targets that production.
    // (i.e., `continue` with a label can only target an iteration statement.)
    fn try_parse_labelled_statement(&mut self, allow_labelled_function: bool) -> Option<Statement> {
        let start = self.position();

        if !self.match_identifier_name() {
            return None;
        }

        self.save_state();
        let token = self.consume();
        let label = Utf16String::from(self.token_value(&token));

        if !self.match_token(TokenType::Colon) {
            self.load_state();
            return None;
        }
        self.discard_saved_state();
        self.consume(); // consume :

        if self.flags.strict_mode
            && (label == utf16!("let") || crate::parser::is_strict_reserved_word(&label))
        {
            self.syntax_error("Strict mode reserved word is not allowed in label");
        }
        if self.flags.in_generator_function_context && label == utf16!("yield") {
            self.syntax_error("'yield' label is not allowed in generator function context");
        }
        if self.flags.await_expression_is_valid && label == utf16!("await") {
            self.syntax_error("'await' label is not allowed in async function context");
        }

        if self.labels_in_scope.contains_key(label.as_slice()) {
            let label_str = String::from_utf16_lossy(&label);
            self.syntax_error(&format!("Label '{}' has already been declared", label_str));
        }

        if self.match_token(TokenType::Function)
            && (!allow_labelled_function || self.flags.strict_mode)
        {
            self.syntax_error("Not allowed to declare a function here");
        }
        if self.match_token(TokenType::Async) {
            let next = self.next_token();
            if next.token_type == TokenType::Function && !next.trivia_has_line_terminator {
                self.syntax_error("Async functions cannot be defined in labelled statements");
            }
        }

        self.labels_in_scope.insert(label.clone(), None);

        let break_before = self.flags.in_break_context;
        self.flags.in_break_context = true;

        let body_starts_iteration = self.match_iteration_start();
        self.last_inner_label_is_iteration = false;
        let body = if self.match_token(TokenType::Function) {
            let fn_decl = self.parse_function_declaration();
            if let StatementKind::FunctionDeclaration { kind, .. } = fn_decl.inner {
                match kind {
                    FunctionKind::Generator | FunctionKind::AsyncGenerator => {
                        self.syntax_error(
                            "Generator functions cannot be defined in labelled statements",
                        );
                    }
                    FunctionKind::Async => {
                        self.syntax_error(
                            "Async functions cannot be defined in labelled statements",
                        );
                    }
                    _ => {}
                }
            }
            fn_decl
        } else {
            self.parse_statement(allow_labelled_function)
        };

        let is_iteration = body_starts_iteration || self.last_inner_label_is_iteration;
        if !is_iteration && let Some(Some((line, col))) = self.labels_in_scope.get(label.as_slice())
        {
            self.syntax_error_at(
                "labelled continue statement cannot use non iterating statement",
                *line,
                *col,
            );
        }

        self.labels_in_scope.remove(label.as_slice());
        self.flags.in_break_context = break_before;
        self.last_inner_label_is_iteration = is_iteration;

        Some(self.statement(
            start,
            StatementKind::Labelled {
                label,
                item: Box::new(body),
            },
        ))
    }

    fn match_for_using_declaration(&mut self) -> bool {
        if !self.match_token(TokenType::Identifier) {
            return false;
        }
        if self.token_value(&self.current_token) != utf16!("using") {
            return false;
        }
        let next = self.next_token();
        if next.trivia_has_line_terminator {
            return false;
        }
        if next.token_type == TokenType::Identifier {
            let next_val = self.token_original_value(&next);
            if next_val == utf16!("of") {
                return false;
            }
        }
        // Must be an actual identifier, not just an identifier-name keyword
        // like `in`. This matches C++ token_is_identifier().
        self.token_is_identifier(&next)
    }

    /// Validate that an expression-form LHS is valid for for-in/for-of.
    fn validate_for_in_of_lhs(&mut self, init: &LocalForInit) {
        if let LocalForInit::Expression(ref expression) = *init
            && !Self::is_identifier(expression)
            && !Self::is_member_expression(expression)
            && !Self::is_call_expression(expression)
            && !Self::is_object_expression(expression)
            && !Self::is_array_expression(expression)
        {
            self.syntax_error("Invalid left-hand side in for-loop");
        }
    }

    /// Convert a `LocalForInit` into a `ForInOfLhs`, synthesizing a binding
    /// pattern when the LHS is an array or object expression.
    fn synthesize_for_in_of_lhs(&mut self, init: LocalForInit, init_start: Position) -> ForInOfLhs {
        match init {
            LocalForInit::Declaration(declaration) => {
                ForInOfLhs::Declaration(Box::new(declaration))
            }
            LocalForInit::Expression(expression) => {
                if Self::is_array_expression(&expression) || Self::is_object_expression(&expression)
                {
                    match self.synthesize_binding_pattern(init_start) {
                        Some(pattern) => {
                            for (name, id) in self.pattern_bound_names.drain(..) {
                                self.scope_collector.register_identifier(id, &name, None);
                            }
                            ForInOfLhs::Pattern(pattern)
                        }
                        _ => ForInOfLhs::Expression(Box::new(expression)),
                    }
                } else {
                    ForInOfLhs::Expression(Box::new(expression))
                }
            }
        }
    }
}
