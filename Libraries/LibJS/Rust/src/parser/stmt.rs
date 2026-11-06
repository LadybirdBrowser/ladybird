/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Statements, functions, classes and modules.

use super::base::*;
use super::columns::*;
use super::expr::is_name_like;
use super::expr::starts_expression;
use super::token::*;
use super::unit::Unit;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Context {
    /// In a statement list, where declarations are allowed.
    List,
    If,
    Label,
    /// Any other single-statement position.
    Single,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum VarKind {
    Var,
    Let,
    Const,
    Using,
    AwaitUsing,
}

impl VarKind {
    fn tag(self) -> u8 {
        match self {
            VarKind::Var => tag::VAR,
            VarKind::Let => tag::LET,
            VarKind::Const => tag::CONST,
            VarKind::Using => tag::USING,
            VarKind::AwaitUsing => tag::AWAIT_USING,
        }
    }

    fn bind(self) -> Bind {
        match self {
            VarKind::Var => Bind::Var,
            VarKind::Let => Bind::Let,
            _ => Bind::Const,
        }
    }
}

impl<U: Unit> Parser<'_, U> {
    pub(crate) fn parse_statement_list_item(&mut self, top_level: bool) -> u32 {
        self.parse_statement(Context::List, top_level)
    }

    fn is_let(&mut self, context: Context) -> bool {
        if !self.is_contextual(N_LET) {
            return false;
        }
        let (kind, _, _) = self.peek();
        if kind == T::LBracket {
            return true;
        }
        if context != Context::List {
            return false;
        }
        if kind == T::LBrace || kind == T::Name || kind == T::EscapedKeyword {
            return true;
        }
        kind as u8 >= T::KBreak as u8
    }

    fn is_async_function(&mut self) -> bool {
        if !self.is_contextual(N_ASYNC) {
            return false;
        }
        let (kind, _, newline) = self.peek();
        kind == T::KFunction && !newline
    }

    fn is_using(&mut self, in_for: bool) -> bool {
        if !self.is_contextual(N_USING) {
            return false;
        }
        let (kind, name, newline) = self.peek();
        if newline || kind != T::Name {
            return false;
        }
        if !(in_for && name == N_OF) {
            return true;
        }
        // `for (using of x)` iterates over a variable named `using`, and `for (using of = 1;;)` declares `of`.
        let after = self.look_ahead(|parser| {
            parser.lex();
            parser.peek().0
        });
        matches!(after, T::Assign | T::Semi | T::Comma)
    }

    fn is_await_using(&mut self) -> bool {
        if !self.is_contextual(N_AWAIT) || self.ctx & ctx::CAN_AWAIT == 0 {
            return false;
        }
        self.look_ahead(|parser| {
            parser.lex();
            !parser.nl_before && parser.is_using(false)
        })
    }

    pub(crate) fn parse_statement(&mut self, context: Context, top_level: bool) -> u32 {
        if !self.enter() {
            return 0;
        }
        let statement = self.parse_statement_inner(context, top_level);
        self.leave();
        statement
    }

    fn parse_statement_inner(&mut self, context: Context, top_level: bool) -> u32 {
        let start = self.tok_start;
        match self.tok {
            T::KVar => {
                self.next();
                self.parse_var_statement(start, VarKind::Var)
            }
            T::KIf => {
                self.next();
                self.expect(T::LParen);
                let test = self.parse_expression(false, false);
                self.expect(T::RParen);
                self.mark(tag::MARK_IF_TEST, 0);
                self.if_start = start;
                let consequent = self.parse_statement(Context::If, false);
                let alternate = if self.eat(T::KElse) {
                    self.mark(tag::MARK_ELSE, 0);
                    self.if_start = start;
                    self.parse_statement(Context::If, false)
                } else {
                    0
                };
                let extra = self.ast.extra.len() as u32;
                self.ast.extra.push(consequent);
                self.ast.extra.push(alternate);
                self.ast.push(tag::IF, start, test, extra)
            }
            T::KReturn => {
                if self.ctx & ctx::RETURN == 0 {
                    self.fail(start, "'return' not allowed outside of a function");
                }
                self.next();
                if self.can_insert_semicolon() {
                    return self.ast.push(tag::RETURN, start, 0, 0);
                }
                let argument = if self.tok == T::Semi {
                    0
                } else {
                    self.parse_expression(false, false)
                };
                self.semicolon();
                self.ast.push(tag::RETURN, start, argument, 0)
            }
            T::LBrace => self.parse_block(),
            T::Semi => {
                self.next();
                self.ast.push(tag::EMPTY, start, 0, 0)
            }
            T::KFunction => {
                if context != Context::List && (self.strict || (context != Context::If && context != Context::Label)) {
                    self.unexpected();
                    return 0;
                }
                self.next();
                if context == Context::If {
                    // Annex B.3.4: as if it were in a block of its own.
                    let if_start = self.if_start;
                    let saved_block_mark = (self.block_mark, self.last_function_of_block);
                    self.block_mark = if self.marks { self.ast.tag.len() as u32 } else { NONE };
                    self.last_function_of_block = 0;
                    self.mark(tag::MARK_BLOCK, self.tok_start);
                    self.open_scope(0);
                    let node = self.parse_function_declaration(start, false, true);
                    (self.block_mark, self.last_function_of_block) = saved_block_mark;
                    let scope = self.close_scope();
                    return self.ast.push(tag::IF_FUNCTION_BLOCK, if_start, node, scope);
                }
                self.parse_function_declaration(start, false, context != Context::List)
            }
            T::KClass => {
                if context != Context::List {
                    self.unexpected();
                    return 0;
                }
                self.parse_class(true)
            }
            T::KConst => {
                if context != Context::List {
                    self.unexpected();
                    return 0;
                }
                self.next();
                self.parse_var_statement(start, VarKind::Const)
            }
            T::KFor => self.parse_for(),
            T::KWhile => {
                self.next();
                self.expect(T::LParen);
                self.mark(tag::MARK_LOOP_HEAD, 0);
                let test = self.parse_expression(false, false);
                self.expect(T::RParen);
                self.mark(tag::MARK_WHILE_TEST, 0);
                self.push_loop_label();
                let body = self.parse_statement(Context::Single, false);
                self.labels.pop();
                self.ast.push(tag::WHILE, start, test, body)
            }
            T::KDo => {
                self.next();
                self.mark(tag::MARK_DO_HEAD, 0);
                self.push_loop_label();
                let body = self.parse_statement(Context::Single, false);
                self.labels.pop();
                self.expect(T::KWhile);
                self.expect(T::LParen);
                self.mark(tag::MARK_DO_TEST, 0);
                let test = self.parse_expression(false, false);
                self.expect(T::RParen);
                self.eat(T::Semi);
                self.ast.push(tag::DO_WHILE, start, body, test)
            }
            T::KBreak | T::KContinue => self.parse_break_continue(),
            T::KThrow => {
                self.next();
                if self.nl_before {
                    self.fail(self.prev_end, "Illegal newline after throw");
                    return 0;
                }
                let argument = self.parse_expression(false, false);
                self.semicolon();
                self.ast.push(tag::THROW, start, argument, 0)
            }
            T::KTry => {
                let subtree = self.begin_subtree();
                let statement = self.parse_try(subtree);
                self.end_subtree(subtree, statement)
            }
            T::KSwitch => {
                let subtree = self.begin_subtree();
                let statement = self.parse_switch(subtree);
                self.end_subtree(subtree, statement)
            }
            T::KWith => {
                if self.strict {
                    self.fail(start, "'with' statement not allowed in strict mode");
                    return 0;
                }
                let subtree = self.begin_subtree();
                self.next();
                self.expect(T::LParen);
                let object = self.parse_expression(false, false);
                self.expect(T::RParen);
                // A name used in the body may not be what it looks like, so that the body counts as a function of its
                // own: whatever it uses has to stay reachable by name.
                let summary_row = self.summary_rows[self.cur_func as usize];
                let func = self.new_function_id(NONE);
                self.summary_rows[func as usize] = summary_row;
                self.open_scope_with(scope_flag::WITH, func);
                self.with_depth += 1;
                let body = self.parse_statement(Context::Single, false);
                self.with_depth -= 1;
                let scope = self.close_scope();
                let extra = self.ast.extra.len() as u32;
                self.ast.extra.push(body);
                self.ast.extra.push(scope);
                let statement = self.ast.push(tag::WITH, start, object, extra);
                self.end_subtree(subtree, statement)
            }
            T::KDebugger => {
                self.next();
                self.semicolon();
                self.ast.push(tag::DEBUGGER, start, 0, 0)
            }
            T::KImport | T::KExport => {
                if self.tok == T::KImport {
                    let (kind, _, _) = self.peek();
                    if kind == T::LParen || kind == T::Dot {
                        let expression = self.parse_expression(false, false);
                        self.semicolon();
                        return self.ast.push(tag::EXPR_STMT, start, expression, 0);
                    }
                }
                if !top_level || !self.module {
                    self.fail(
                        start,
                        "'import' and 'export' may only appear at the top level of a module",
                    );
                    return 0;
                }
                let subtree = self.begin_subtree();
                let statement = if self.tok == T::KImport {
                    self.parse_import()
                } else {
                    self.parse_export()
                };
                self.end_subtree(subtree, statement)
            }
            T::Name => self.parse_name_statement(context),
            kind => {
                if !starts_expression(kind) {
                    self.expected(match (context, top_level) {
                        (Context::List, true) if self.ctx & ctx::RETURN == 0 => "statement or declaration",
                        (Context::List, _) => "'}'",
                        _ => "statement",
                    });
                    return 0;
                }
                let expression = self.parse_expression(false, false);
                self.semicolon();
                self.ast.push(tag::EXPR_STMT, start, expression, 0)
            }
        }
    }

    fn parse_name_statement(&mut self, context: Context) -> u32 {
        let start = self.tok_start;
        if self.tok_name < FIRST_PLAIN_NAME {
            if self.is_let(context) {
                if context != Context::List {
                    self.unexpected();
                    return 0;
                }
                self.next();
                return self.parse_var_statement(start, VarKind::Let);
            }
            if self.is_async_function() {
                if context != Context::List {
                    self.unexpected();
                    return 0;
                }
                self.next();
                self.next();
                return self.parse_function_declaration(start, true, false);
            }
            if self.is_using(false) {
                let flags = self.scopes.last().unwrap().flags;
                let at_script_top_level = flags & scope_flag::TOP != 0 && !self.module && self.ctx & ctx::RETURN == 0;
                if context != Context::List || at_script_top_level || flags & scope_flag::SWITCH != 0 {
                    self.fail(start, "'using' is not allowed here");
                    return 0;
                }
                self.next();
                return self.parse_var_statement(start, VarKind::Using);
            }
            if self.is_await_using() {
                let flags = self.scopes.last().unwrap().flags;
                let at_script_top_level = flags & scope_flag::TOP != 0 && !self.module && self.ctx & ctx::RETURN == 0;
                if context != Context::List || at_script_top_level || flags & scope_flag::SWITCH != 0 {
                    self.fail(start, "'await using' is not allowed here");
                    return 0;
                }
                if self.ctx & ctx::ASYNC == 0 {
                    self.has_top_level_await = true;
                }
                self.next();
                self.next();
                return self.parse_var_statement(start, VarKind::AwaitUsing);
            }
        }
        let name = self.tok_name;
        // A label is no use of a variable of that name, so it must not get parsed as one.
        let next = self.at(self.pos as usize);
        if next == 0x3A || ((next <= 0x20 || next == 0x2F) && self.peek().0 == T::Colon) {
            self.check_identifier(name, start);
            self.next();
            self.next();
            return self.parse_labeled(start, name, context);
        }
        let expression = self.parse_expression(false, false);
        self.semicolon();
        self.ast.push(tag::EXPR_STMT, start, expression, 0)
    }

    fn parse_labeled(&mut self, start: u32, name: u32, context: Context) -> u32 {
        let subtree = self.begin_subtree();
        let base = self.labels_base as usize;
        let shadowed = self.label_indices.get(&name).copied();
        if shadowed.is_some_and(|index| index as usize >= base) {
            let label = String::from_utf8_lossy(self.interner.text(name)).into_owned();
            self.fail_with(start, format!("Label '{label}' has already been declared"));
            return 0;
        }
        let kind = match self.tok {
            T::KFor | T::KWhile | T::KDo => 1,
            T::KSwitch => 2,
            _ => 0,
        };
        for i in (base..self.labels.len()).rev() {
            let label = &mut self.labels[i];
            if label.statement_start == start {
                label.statement_start = self.tok_start;
                label.kind = kind;
            } else {
                break;
            }
        }
        self.label_indices.insert(name, self.labels.len() as u32);
        self.labels.push(Label {
            kind,
            statement_start: self.tok_start,
        });
        let body_context = match context {
            Context::If | Context::Single => Context::Single,
            _ => Context::Label,
        };
        let body = self.parse_statement(body_context, false);
        self.labels.pop();
        match shadowed {
            Some(index) => self.label_indices.insert(name, index),
            None => self.label_indices.remove(&name),
        };
        let statement = self.ast.push(tag::LABELED, start, name, body);
        self.end_subtree(subtree, statement)
    }

    #[inline(always)]
    fn push_loop_label(&mut self) {
        self.labels.push(Label {
            kind: 1,
            statement_start: NONE,
        });
    }

    fn parse_break_continue(&mut self) -> u32 {
        let start = self.tok_start;
        let is_break = self.tok == T::KBreak;
        self.next();
        let mut name = NONE;
        let mut name_pos = start;
        if !self.eat(T::Semi) && !self.can_insert_semicolon() {
            if self.tok != T::Name {
                self.unexpected();
                return 0;
            }
            (name, name_pos) = self.parse_ident();
            self.semicolon();
        }
        let base = self.labels_base as usize;
        let found = if name == NONE {
            self.labels[base..]
                .iter()
                .rev()
                .any(|label| label.kind != 0 && (is_break || label.kind == 1))
        } else {
            self.label_indices.get(&name).is_some_and(|&index| {
                let label = &self.labels[index as usize];
                index as usize >= base && (is_break || label.kind == 1)
            })
        };
        if !found {
            let message = if name == NONE {
                if is_break {
                    "Unlabeled 'break' not allowed outside of a loop or switch statement".to_string()
                } else {
                    "'continue' not allowed outside of a loop".to_string()
                }
            } else {
                let label = String::from_utf8_lossy(self.interner.text(name)).into_owned();
                let is_in_scope = self
                    .label_indices
                    .get(&name)
                    .is_some_and(|&index| index as usize >= base);
                if is_in_scope {
                    "labelled continue statement cannot use non iterating statement".to_string()
                } else {
                    format!("Label '{label}' not found")
                }
            };
            self.fail_with(name_pos, message);
        }
        self.ast
            .push(if is_break { tag::BREAK } else { tag::CONTINUE }, start, name, 0)
    }

    fn parse_block(&mut self) -> u32 {
        let start = self.tok_start;
        let saved_block_mark = (self.block_mark, self.last_function_of_block);
        self.block_mark = if self.marks { self.ast.tag.len() as u32 } else { NONE };
        self.last_function_of_block = 0;
        self.mark(tag::MARK_BLOCK, start);
        self.open_scope(0);
        let list = self.parse_block_statements();
        (self.block_mark, self.last_function_of_block) = saved_block_mark;
        let scope = self.close_scope();
        self.ast.push(tag::BLOCK, start, list, scope)
    }

    /// The statements from the `{` through the `}`.
    fn parse_block_statements(&mut self) -> u32 {
        self.next();
        let mark = self.scratch.len();
        while self.tok != T::RBrace && self.tok != T::Eof {
            let statement = self.parse_statement(Context::List, false);
            self.scratch.push(statement);
        }
        self.expect(T::RBrace);
        let list = self.finish_list(mark);
        // A block's list is followed by where the code after the block starts.
        self.ast.extra.push(self.tok_start);
        list
    }

    fn parse_var_statement(&mut self, start: u32, kind: VarKind) -> u32 {
        let list = self.parse_var_declarators(kind, false, start);
        self.semicolon();
        self.ast.push(kind.tag(), start, list, 0)
    }

    fn parse_var_declarators(&mut self, kind: VarKind, is_for: bool, statement_start: u32) -> u32 {
        let mark = self.scratch.len();
        loop {
            let start = self.tok_start;
            if (kind == VarKind::Using || kind == VarKind::AwaitUsing) && self.tok != T::Name {
                self.unexpected();
                break;
            }
            let bind = kind.bind();
            let is_pattern = matches!(self.tok, T::LBracket | T::LBrace);
            let subtree = if is_pattern { self.begin_subtree() } else { NONE };
            if subtree != NONE {
                self.ast.b[subtree as usize] = u32::from(kind.tag());
                self.ast.pos[subtree as usize] = statement_start;
            }
            let target = self.parse_binding_atom(bind);
            if !is_pattern {
                self.mark(tag::MARK_DECLARATION, u32::from(kind.tag()));
            }
            let mut init = 0;
            if self.eat(T::Assign) {
                init = self.parse_assign(is_for, false);
            } else {
                let in_for_head = is_for && (self.tok == T::KIn || self.is_contextual(N_OF));
                let needs_init = kind == VarKind::Const
                    || kind == VarKind::Using
                    || kind == VarKind::AwaitUsing
                    || !self.is_identifier_row(target);
                if needs_init && !in_for_head {
                    self.fail(self.prev_end, "Missing initializer in declaration");
                    break;
                }
            }
            let declarator = self.ast.push(tag::DECLARATOR, start, target, init);
            self.end_subtree(subtree, declarator);
            self.scratch.push(declarator);
            if !self.eat(T::Comma) {
                break;
            }
        }
        self.finish_list(mark)
    }

    /// A binding identifier or pattern, whose names get declared as `bind`.
    pub(crate) fn parse_binding_atom(&mut self, bind: Bind) -> u32 {
        if self.tok != T::LBracket && self.tok != T::LBrace {
            return self.parse_binding_atom_inner(bind);
        }
        if !self.enter() {
            return 0;
        }
        let atom = self.parse_binding_atom_inner(bind);
        self.leave();
        atom
    }

    fn parse_binding_atom_inner(&mut self, bind: Bind) -> u32 {
        let start = self.tok_start;
        match self.tok {
            T::Name => {
                let name = self.tok_name;
                if name == N_LET && matches!(bind, Bind::Let | Bind::Const | Bind::Lexical) {
                    self.fail(start, "'let' is disallowed as a lexically bound name");
                    return 0;
                }
                self.check_binding_name(name, start);
                if name == N_ARGUMENTS || name == N_EVAL {
                    self.might_need_arguments = true;
                }
                self.declare(name, bind, start);
                self.next();
                self.ast.push(tag::IDENT, start, name, 0)
            }
            T::LBracket => {
                self.next();
                let mark = self.scratch.len();
                while self.tok != T::RBracket && self.tok != T::Eof {
                    let item = match self.tok {
                        T::Comma => self.ast.push(tag::HOLE, self.tok_start, 0, 0),
                        T::Ellipsis => {
                            let item_start = self.tok_start;
                            self.next();
                            let target = self.parse_binding_atom(bind);
                            if self.tok != T::RBracket {
                                self.fail(self.tok_start, "Rest element may not be followed by a comma");
                            }
                            self.ast.push(tag::REST, item_start, target, 0)
                        }
                        _ => self.parse_binding_element(bind),
                    };
                    self.scratch.push(item);
                    if self.tok != T::RBracket {
                        self.expect_comma_or(T::RBracket);
                    }
                }
                self.expect(T::RBracket);
                let list = self.finish_list(mark);
                self.ast.push(tag::ARRAY_PATTERN, start, list, 0)
            }
            T::LBrace => {
                self.next();
                let mark = self.scratch.len();
                while self.tok != T::RBrace && self.tok != T::Eof {
                    let property = self.parse_binding_property(bind);
                    self.scratch.push(property);
                    if self.tok != T::RBrace {
                        self.expect_comma_or(T::RBrace);
                    }
                }
                self.expect(T::RBrace);
                let list = self.finish_list(mark);
                self.ast.push(tag::OBJECT_PATTERN, start, list, 0)
            }
            _ => {
                self.unexpected();
                0
            }
        }
    }

    fn parse_binding_property(&mut self, bind: Bind) -> u32 {
        let start = self.tok_start;
        if self.tok == T::Ellipsis {
            self.next();
            if self.tok != T::Name {
                self.unexpected();
                return 0;
            }
            let target = self.parse_binding_atom(bind);
            if self.tok != T::RBrace {
                self.fail(self.tok_start, "Rest element may not be followed by a comma");
            }
            return self.ast.push(tag::REST, start, target, 0);
        }
        let key_kind = self.tok;
        let key_name = self.tok_name;
        let (key, computed) = self.parse_property_name();
        if self.eat(T::Colon) {
            let value = self.parse_binding_element(bind);
            let t = if computed {
                tag::COMPUTED_PROPERTY
            } else {
                tag::PROPERTY
            };
            return self.ast.push(t, start, key, value);
        }
        if computed || key_kind != T::Name {
            self.unexpected();
            return 0;
        }
        if key_name == N_LET && matches!(bind, Bind::Let | Bind::Const | Bind::Lexical) {
            self.fail(start, "'let' is disallowed as a lexically bound name");
            return 0;
        }
        self.check_binding_name(key_name, start);
        self.declare(key_name, bind, start);
        let mut value = key;
        if self.eat(T::Assign) {
            let default = self.parse_assign(false, false);
            value = self.ast.push(tag::ASSIGN_PATTERN, start, value, default);
        }
        self.ast.push(tag::SHORTHAND, start, key_name, value)
    }

    fn parse_binding_element(&mut self, bind: Bind) -> u32 {
        let start = self.tok_start;
        let target = self.parse_binding_atom(bind);
        if self.eat(T::Assign) {
            let default = self.parse_assign(false, false);
            return self.ast.push(tag::ASSIGN_PATTERN, start, target, default);
        }
        target
    }

    fn parse_for(&mut self) -> u32 {
        let start = self.tok_start;
        self.next();
        let mut await_at = NONE;
        if self.ctx & ctx::CAN_AWAIT != 0 && self.is_contextual(N_AWAIT) {
            await_at = self.tok_start;
            if self.ctx & ctx::ASYNC == 0 {
                self.has_top_level_await = true;
            }
            self.next();
        }
        self.push_loop_label();
        let subtree = self.begin_subtree();
        self.open_scope(0);
        self.expect(T::LParen);
        let node = self.parse_for_head(start, await_at);
        let scope = self.close_scope();
        self.labels.pop();
        let statement = self.ast.push(tag::SCOPED, start, node, scope);
        self.end_subtree(subtree, statement)
    }

    fn parse_for_head(&mut self, start: u32, await_at: u32) -> u32 {
        if self.tok == T::Semi {
            if await_at != NONE {
                self.fail(await_at, "Unexpected await");
            }
            return self.parse_plain_for(start, 0);
        }
        let is_let = self.is_let(Context::List);
        let is_using = self.is_using(true);
        let is_await_using = !is_let && !is_using && self.is_await_using();
        if self.tok == T::KVar || self.tok == T::KConst || is_let || is_using || is_await_using {
            let kind = if is_let {
                VarKind::Let
            } else if is_using {
                VarKind::Using
            } else if is_await_using {
                VarKind::AwaitUsing
            } else if self.tok == T::KVar {
                VarKind::Var
            } else {
                VarKind::Const
            };
            let init_start = self.tok_start;
            self.next();
            if is_await_using {
                if self.ctx & ctx::ASYNC == 0 {
                    self.has_top_level_await = true;
                }
                self.next();
            }
            let list = self.parse_var_declarators(kind, true, init_start);
            let init = self.ast.push(kind.tag(), init_start, list, 0);
            if (self.tok == T::KIn || self.is_contextual(N_OF)) && self.list(list).len() == 1 {
                let is_in = self.tok == T::KIn;
                if is_in && (await_at != NONE || kind == VarKind::Using || kind == VarKind::AwaitUsing) {
                    self.unexpected();
                    return 0;
                }
                let declarator = self.list(list)[0];
                let has_init = self.ast.b[declarator as usize] != 0;
                if has_init {
                    let target = self.ast.a[declarator as usize];
                    let allowed = is_in && !self.strict && kind == VarKind::Var && self.is_identifier_row(target);
                    if !allowed {
                        self.fail(init_start, "Variable initializer not allowed in for..in/of");
                        return 0;
                    }
                }
                return self.parse_for_in(start, init, await_at);
            }
            if await_at != NONE {
                self.fail(await_at, "Unexpected await");
            }
            if kind != VarKind::Var {
                // The initializer check waits until here, since `const x` is fine before `of`.
            }
            return self.parse_plain_for(start, init);
        }
        let starts_with_let = self.is_contextual(N_LET);
        let starts_with_async = self.is_contextual(N_ASYNC);
        let init_start = self.tok_start;
        let saved_cover_error = self.cover_error;
        self.cover_error = NONE;
        self.for_await_head = await_at != NONE;
        let init = if await_at != NONE {
            self.parse_left_hand_side(false)
        } else {
            self.parse_expression(true, true)
        };
        self.for_await_head = false;
        let is_of = self.is_contextual(N_OF);
        if self.tok == T::KIn || is_of {
            if await_at != NONE && !is_of {
                self.unexpected();
                return 0;
            }
            if is_of && await_at == NONE && starts_with_async {
                let is_plain_async =
                    self.ast.tag[init as usize] == tag::IDENT && self.ast.pos[init as usize] == init_start;
                if is_plain_async {
                    self.unexpected();
                    return 0;
                }
            }
            if starts_with_let && is_of {
                self.fail(init_start, "The left-hand side of a for-of loop may not be 'let'");
                return 0;
            }
            if self.ast.tag[init as usize] >= tag::ASSIGN_BASE && self.ast.tag[init as usize] < tag::BINARY_BASE {
                self.fail(init_start, "Invalid left-hand side in for loop");
                return 0;
            }
            self.convert_assignment_target(init);
            self.cover_error = saved_cover_error;
            return self.parse_for_in(start, init, await_at);
        }
        if self.cover_error != NONE {
            self.fail(
                self.cover_error,
                "Shorthand property initializers are only valid in patterns",
            );
        }
        self.cover_error = saved_cover_error;
        if await_at != NONE {
            self.fail(await_at, "Unexpected await");
        }
        self.parse_plain_for(start, init)
    }

    fn parse_plain_for(&mut self, start: u32, init: u32) -> u32 {
        // A `const` or pattern without an initializer got past the declarator only for a for-in or for-of head.
        if init != 0
            && matches!(
                self.ast.tag[init as usize],
                tag::CONST | tag::USING | tag::AWAIT_USING | tag::LET | tag::VAR
            )
        {
            let list = self.ast.a[init as usize];
            let kind = self.ast.tag[init as usize];
            for i in 0..self.ast.extra[list as usize] {
                let declarator = self.ast.extra[(list + 1 + i) as usize];
                let missing = self.ast.b[declarator as usize] == 0;
                let pattern = !self.is_identifier_row(self.ast.a[declarator as usize]);
                if missing && (pattern || (kind != tag::LET && kind != tag::VAR)) {
                    self.fail(start, "Missing initializer in declaration");
                }
            }
        }
        self.expect(T::Semi);
        let test = if self.tok == T::Semi {
            0
        } else {
            self.parse_expression(false, false)
        };
        self.expect(T::Semi);
        let update = if self.tok == T::RParen {
            0
        } else {
            self.parse_expression(false, false)
        };
        self.expect(T::RParen);
        let body = self.parse_statement(Context::Single, false);
        let extra = self.ast.extra.len() as u32;
        self.ast.extra.push(test);
        self.ast.extra.push(update);
        self.ast.extra.push(body);
        self.ast.push(tag::FOR, start, init, extra)
    }

    fn parse_for_in(&mut self, _start: u32, left: u32, await_at: u32) -> u32 {
        let is_in = self.tok == T::KIn;
        let start = self.tok_start;
        self.next();
        let right = if is_in {
            self.parse_expression(false, false)
        } else {
            self.parse_assign(false, false)
        };
        self.expect(T::RParen);
        let body = self.parse_statement(Context::Single, false);
        let t = if is_in {
            tag::FOR_IN
        } else if await_at != NONE {
            tag::FOR_AWAIT_OF
        } else {
            tag::FOR_OF
        };
        let extra = self.ast.extra.len() as u32;
        self.ast.extra.push(right);
        self.ast.extra.push(body);
        self.ast.push(t, start, left, extra)
    }

    fn parse_try(&mut self, subtree: u32) -> u32 {
        let start = self.tok_start;
        self.next();
        if self.tok != T::LBrace {
            self.unexpected();
            return 0;
        }
        let block = self.parse_block();
        let mut parameter = 0;
        let mut handler = 0;
        let mut catch_scope = NONE;
        let catch_pos = self.tok_start;
        if self.tok == T::KCatch {
            self.next();
            self.open_scope(scope_flag::CATCH);
            self.scopes.last_mut().expect("the scope just opened").start = catch_pos;
            if self.eat(T::LParen) {
                let simple = self.tok == T::Name;
                parameter = self.parse_binding_atom(if simple { Bind::SimpleCatch } else { Bind::CatchPattern });
                self.expect(T::RParen);
            }
            if self.tok != T::LBrace {
                self.unexpected();
                self.close_scope();
                return 0;
            }
            let handler_start = self.tok_start;
            self.open_scope(scope_flag::CATCH_BODY);
            let saved_block_mark = (self.block_mark, self.last_function_of_block);
            self.block_mark = subtree;
            self.last_function_of_block = 0;
            let list = self.parse_block_statements();
            (self.block_mark, self.last_function_of_block) = saved_block_mark;
            let body_scope = self.close_scope();
            catch_scope = self.close_scope();
            handler = self.ast.push(tag::BLOCK, handler_start, list, body_scope);
        }
        let finalizer = if self.tok == T::KFinally {
            self.next();
            if self.tok != T::LBrace {
                self.unexpected();
                return 0;
            }
            self.parse_block()
        } else {
            0
        };
        if handler == 0 && finalizer == 0 {
            self.fail(start, "Missing catch or finally clause");
        }
        let extra = self.ast.extra.len() as u32;
        self.ast.extra.push(parameter);
        self.ast.extra.push(handler);
        self.ast.extra.push(finalizer);
        self.ast.extra.push(catch_pos);
        self.ast.extra.push(catch_scope);
        self.ast.push(tag::TRY, start, block, extra)
    }

    fn parse_switch(&mut self, subtree: u32) -> u32 {
        let start = self.tok_start;
        self.next();
        self.expect(T::LParen);
        let discriminant = self.parse_expression(false, false);
        self.expect(T::RParen);
        self.expect(T::LBrace);
        self.labels.push(Label {
            kind: 2,
            statement_start: NONE,
        });
        self.open_scope(scope_flag::SWITCH);
        self.scopes.last_mut().expect("the scope just opened").start = start;
        let saved_block_mark = (self.block_mark, self.last_function_of_block);
        self.block_mark = subtree;
        self.last_function_of_block = 0;
        let mark = self.scratch.len();
        let mut saw_default = false;
        while self.tok != T::RBrace && self.tok != T::Eof {
            let case_start = self.tok_start;
            let test = if self.eat(T::KCase) {
                self.parse_expression(false, false)
            } else if self.tok == T::KDefault {
                if saw_default {
                    self.fail(case_start, "Multiple default clauses");
                }
                saw_default = true;
                self.next();
                0
            } else {
                self.unexpected();
                0
            };
            self.expect(T::Colon);
            let statements_mark = self.scratch.len();
            while !matches!(self.tok, T::KCase | T::KDefault | T::RBrace | T::Eof) {
                let statement = self.parse_statement(Context::List, false);
                self.scratch.push(statement);
            }
            let statements = self.finish_list(statements_mark);
            let case = self.ast.push(tag::CASE, case_start, test, statements);
            self.scratch.push(case);
        }
        self.expect(T::RBrace);
        (self.block_mark, self.last_function_of_block) = saved_block_mark;
        let scope = self.close_scope();
        self.labels.pop();
        let list = self.finish_list(mark);
        let extra = self.ast.extra.len() as u32;
        self.ast.extra.push(list);
        self.ast.extra.push(scope);
        self.ast.push(tag::SWITCH, start, discriminant, extra)
    }

    // === Functions ===

    pub(crate) fn empty_function_state(&self) -> FunctionState {
        FunctionState {
            marks: self.ast.marks(),
            row: 0,
            ctx: 0,
            strict: false,
            labels_base: 0,
            yield_pos: 0,
            await_pos: 0,
            await_ident_pos: 0,
            cover_error: 0,
            retain: false,
            duplicate_parameter: 0,
            start: 0,
            flags: 0,
            has_body_scope: false,
            body_start: 0,
            first_body_row: 0,
            uses_this: false,
            uses_this_from_environment: false,
            might_need_arguments: false,
            in_class_of_sloppy_function: self.in_class_of_sloppy_function,
            is_arrow: false,
            summary_ordinal: 0,
        }
    }

    fn function_ctx(is_generator: bool, is_async: bool) -> u32 {
        let mut c = ctx::RETURN | ctx::NEW_TARGET;
        if is_generator {
            c |= ctx::GENERATOR;
        }
        if is_async {
            c |= ctx::ASYNC | ctx::CAN_AWAIT;
        }
        c
    }

    fn function_flags(is_generator: bool, is_async: bool) -> u32 {
        (if is_generator { function_flag::GENERATOR } else { 0 }) | (if is_async { function_flag::ASYNC } else { 0 })
    }

    /// From after `function`.
    fn parse_function_declaration(&mut self, start: u32, is_async: bool, hanging: bool) -> u32 {
        if self.tok == T::Star && hanging {
            self.unexpected();
            return 0;
        }
        let is_generator = self.eat(T::Star);
        let (name, name_pos) = self.parse_ident();
        if is_generator && is_async && (name == N_AWAIT || name == N_YIELD) {
            let text = String::from_utf8_lossy(self.interner.text(name)).into_owned();
            self.fail_with(
                name_pos,
                format!("async generator function is not allowed to be called '{text}'"),
            );
        }
        if !hanging {
            let bind = if self.strict || is_generator || is_async {
                if self.treat_functions_as_var() {
                    Bind::Var
                } else {
                    Bind::Lexical
                }
            } else {
                Bind::Function
            };
            self.declare(name, bind, name_pos);
            if bind == Bind::Function {
                self.queue_annex_b_hoist(name);
            }
        } else {
            self.declare(name, Bind::Function, name_pos);
            self.queue_annex_b_hoist(name);
        }
        let node = self.parse_function_rest(start, Some((name, name_pos)), is_generator, is_async);
        if self.marks && self.block_mark != NONE {
            // NB: These run when the block starts, which a sweep gets to from the mark of the block.
            if self.last_function_of_block == 0 {
                self.ast.b[self.block_mark as usize] = node;
            } else {
                self.ast.a[self.last_function_of_block as usize] = node;
            }
            self.last_function_of_block = node;
        }
        self.ast.push(tag::FUNCTION_DECL, start, node, name)
    }

    /// From after `function`.
    pub(crate) fn parse_function_expression(&mut self, start: u32, is_async: bool) -> u32 {
        let is_generator = self.eat(T::Star);
        let new_ctx = Self::function_ctx(is_generator, is_async);
        let flags = Self::function_flags(is_generator, is_async);
        let mut state = self.begin_function(start, new_ctx, flags);
        let mut name = None;
        if self.tok == T::Name {
            state.flags |= function_flag::NAMED_EXPRESSION;
            let parsed = self.parse_ident();
            if parsed.0 == N_ARGUMENTS || parsed.0 == N_EVAL {
                self.might_need_arguments = true;
            }
            self.declare_function_name(parsed.0);
            name = Some(parsed);
        }
        self.parse_function_tail(&mut state, name, false)
    }

    fn parse_function_rest(&mut self, start: u32, name: Option<(u32, u32)>, is_generator: bool, is_async: bool) -> u32 {
        let new_ctx = Self::function_ctx(is_generator, is_async);
        let flags = Self::function_flags(is_generator, is_async);
        let mut state = self.begin_function(start, new_ctx, flags);
        if let Some((name, _)) = name
            && (name == N_ARGUMENTS || name == N_EVAL)
        {
            self.might_need_arguments = true;
        }
        self.parse_function_tail(&mut state, name, false)
    }

    /// The parameter list, from the `(`.
    pub(crate) fn parse_parameter_list(&mut self, state: &mut FunctionState) -> (u32, u32, u32, bool, bool) {
        let params_start = self.tok_start;
        self.expect(T::LParen);
        let mark = self.scratch.len();
        let mut simple = true;
        let mut has_rest = false;
        let mut length = 0;
        let mut counting = true;
        while self.tok != T::RParen && self.tok != T::Eof {
            let item_start = self.tok_start;
            let item = if self.tok == T::Ellipsis {
                self.next();
                let target = self.parse_binding_atom(Bind::Parameter);
                has_rest = true;
                simple = false;
                counting = false;
                if self.tok != T::RParen {
                    self.fail(self.tok_start, "Rest parameter must be last");
                }
                self.ast.push(tag::REST, item_start, target, 0)
            } else {
                let item = self.parse_binding_element(Bind::Parameter);
                let t = self.ast.tag[item as usize];
                simple &= t == tag::IDENT;
                counting &= t != tag::ASSIGN_PATTERN;
                if counting {
                    length += 1;
                }
                item
            };
            self.scratch.push(item);
            if self.tok != T::RParen {
                self.expect_comma_or(T::RParen);
            }
        }
        self.expect(T::RParen);
        let count = (self.scratch.len() - mark) as u32;
        let list = self.finish_list(mark);
        if self.yield_pos != 0 || self.await_pos != 0 {
            self.fail(params_start, "Yield or await expression in function parameters");
        }
        if simple {
            state.flags |= function_flag::SIMPLE_PARAMETERS;
        }
        (list, count, length, simple, has_rest)
    }

    pub(crate) fn parse_function_tail(
        &mut self,
        state: &mut FunctionState,
        name: Option<(u32, u32)>,
        is_method: bool,
    ) -> u32 {
        self.parse_function_tail_with_info(state, name, is_method).0
    }

    /// The parameters and the body, from the `(`. Returns the node, the parameter count and whether there is a rest
    /// parameter.
    fn parse_function_tail_with_info(
        &mut self,
        state: &mut FunctionState,
        name: Option<(u32, u32)>,
        is_method: bool,
    ) -> (u32, u32, bool) {
        let params_start = self.tok_start;
        if let Some(row) = self.skippable_row(params_start) {
            let moved_state = core::mem::replace(state, self.empty_function_state());
            let node = self.skip_function(moved_state, row);
            return (node, 0, false);
        }
        let (list, count, length, simple, has_rest) = self.parse_parameter_list(state);
        if self.tok != T::LBrace {
            self.unexpected();
        }
        state.body_start = self.pos;
        let body = self.parse_function_body(state, name, list, simple, false, is_method);
        let end = self.prev_end;
        let moved_state = core::mem::replace(state, self.empty_function_state());
        let name = name.map_or(NONE, |(name, _)| name);
        let node = self.end_function(moved_state, end, count, length, params_start, list, body, name, false);
        (node, count, has_rest)
    }

    /// The statements of a function, from the `{` through the `}`.
    pub(crate) fn parse_function_body(
        &mut self,
        state: &mut FunctionState,
        name: Option<(u32, u32)>,
        parameters: u32,
        simple: bool,
        is_arrow: bool,
        is_method: bool,
    ) -> u32 {
        let old_strict = self.strict;
        let mut use_strict = false;
        if !old_strict || !simple {
            use_strict = self.has_use_strict_directive(self.tok_end as usize);
            if use_strict && !simple {
                self.fail(
                    self.tok_start,
                    "Illegal 'use strict' directive in function with non-simple parameter list",
                );
            }
        }
        if use_strict {
            self.strict = true;
        }
        if self.duplicate_parameter != NONE && (self.strict || !simple || is_arrow || is_method) {
            self.fail(self.duplicate_parameter, "Duplicate parameter names not allowed");
        }
        if self.strict {
            if let Some((name, pos)) = name {
                self.check_strict_binding_name(name, pos);
            }
            if !old_strict {
                let count = self.ast.extra[parameters as usize];
                for i in 0..count {
                    let parameter = self.ast.extra[(parameters + 1 + i) as usize];
                    if self.ast.tag[parameter as usize] == tag::IDENT {
                        let (name, pos) = (self.ast.a[parameter as usize], self.ast.pos[parameter as usize]);
                        self.check_strict_binding_name(name, pos);
                    }
                }
            }
        }
        if !simple {
            self.open_function_body_scope(state);
        }
        self.next();
        state.body_start = self.tok_start;
        state.first_body_row = self.ast.tag.len() as u32;
        let mark = self.scratch.len();
        while self.tok != T::RBrace && self.tok != T::Eof {
            let statement = self.parse_statement(Context::List, false);
            self.scratch.push(statement);
        }
        self.expect(T::RBrace);
        self.finish_list(mark)
    }

    /// A method's function, from the `(`. Returns its node, its parameter count and whether it has a rest parameter.
    pub(crate) fn parse_method(
        &mut self,
        start: u32,
        is_generator: bool,
        is_async: bool,
        super_call: bool,
    ) -> (u32, u32, bool) {
        let mut new_ctx = Self::function_ctx(is_generator, is_async) | ctx::SUPER_PROPERTY;
        if super_call {
            new_ctx |= ctx::SUPER_CALL;
        }
        let flags = Self::function_flags(is_generator, is_async) | function_flag::METHOD;
        let mut state = self.begin_function(start, new_ctx, flags);
        self.parse_function_tail_with_info(&mut state, None, true)
    }

    // === Classes ===

    pub(crate) fn parse_class(&mut self, is_statement: bool) -> u32 {
        let subtree = self.begin_subtree();
        let class = self.parse_class_with_its_rows(is_statement);
        self.end_subtree(subtree, class)
    }

    fn parse_class_with_its_rows(&mut self, is_statement: bool) -> u32 {
        if !self.enter() {
            return 0;
        }
        let class = self.parse_class_inner(is_statement);
        self.leave();
        class
    }

    fn parse_class_inner(&mut self, is_statement: bool) -> u32 {
        let start = self.tok_start;
        let saved_class_start = core::mem::replace(&mut self.class_start, start);
        self.next();
        let old_strict = self.strict;
        let was_in_class_of_sloppy_function = self.in_class_of_sloppy_function;
        self.in_class_of_sloppy_function |= !old_strict;
        self.strict = true;
        let mut name = NONE;
        if self.tok == T::Name && !(self.is_contextual(N_IMPLEMENTS)) {
            let (parsed, pos) = self.parse_ident();
            self.check_binding_name(parsed, pos);
            if parsed == N_LET {
                self.fail(pos, "'let' is disallowed as a lexically bound name");
            }
            name = parsed;
            if is_statement {
                self.declare(name, Bind::Lexical, pos);
            }
        } else if is_statement {
            self.unexpected();
        }
        self.open_scope(scope_flag::CLASS);
        if name != NONE {
            self.declare_class_name(name);
        }
        let mut super_class = 0;
        if self.eat(T::KExtends) {
            super_class = self.parse_left_hand_side(false);
        }
        self.class_marks
            .push((self.private_declared.len() as u32, self.private_used.len() as u32));
        self.private_declared.push(std::collections::HashMap::new());
        self.expect(T::LBrace);
        let mark = self.scratch.len();
        let mut had_constructor = false;
        let mut constructor = 0;
        while self.tok != T::RBrace && self.tok != T::Eof {
            if self.eat(T::Semi) {
                continue;
            }
            let was_constructor = had_constructor;
            let element = self.parse_class_element(super_class != 0, &mut had_constructor);
            if had_constructor && !was_constructor {
                constructor = self.ast.b[element as usize];
            }
            // The constructor stays in the list, so that the list keeps the elements in source order.
            self.scratch.push(element);
        }
        self.strict = old_strict;
        self.in_class_of_sloppy_function = was_in_class_of_sloppy_function;
        self.class_start = saved_class_start;
        self.expect(T::RBrace);
        self.exit_class_body();
        let list = self.finish_list(mark);
        let scope = self.close_scope();
        let default_constructor_row = if had_constructor {
            NONE
        } else {
            let derived = if super_class != 0 {
                function_flag::DERIVED_CONSTRUCTOR
            } else {
                0
            };
            self.class_function_row(start, start, function_flag::DEFAULT_CONSTRUCTOR | derived, name, NONE)
        };
        let extra = self.ast.extra.len() as u32;
        self.ast.extra.push(super_class);
        self.ast.extra.push(list);
        self.ast.extra.push(scope);
        self.ast.extra.push(constructor);
        self.ast.extra.push(self.prev_end);
        self.ast.extra.push(default_constructor_row);
        let node = self.ast.push(tag::CLASS, start, name, extra);
        if is_statement {
            return self.ast.push(tag::CLASS_DECL, start, node, 0);
        }
        node
    }

    fn exit_class_body(&mut self) {
        let (_, used_mark) = self.class_marks.pop().unwrap();
        let declared = self.private_declared.pop().unwrap();
        let mut kept = used_mark as usize;
        let is_outermost = self.class_marks.is_empty();
        for i in used_mark as usize..self.private_used.len() {
            let (name, pos) = self.private_used[i];
            if declared.contains_key(&name) {
                continue;
            }
            if is_outermost {
                self.report_undeclared_private_name(name, pos);
                continue;
            }
            self.private_used[kept] = (name, pos);
            kept += 1;
        }
        self.private_used.truncate(kept);
    }

    pub(crate) fn check_unresolved_private_names(&mut self) {
        for (name, pos) in std::mem::take(&mut self.private_used) {
            self.report_undeclared_private_name(name, pos);
        }
    }

    /// The code around an eval may declare the name, which the eval's caller finds out.
    fn report_undeclared_private_name(&mut self, name: u32, pos: u32) {
        if self.initiated_by_eval {
            if !self.eval_referenced_private_names.contains(&name) {
                self.eval_referenced_private_names.push(name);
            }
            return;
        }
        let name = String::from_utf8_lossy(self.interner.text(name)).into_owned();
        self.fail_with(
            pos,
            format!("Reference to undeclared private field or method '#{name}'"),
        );
    }

    /// `kind` is 0 for a field or a method, 1 and 2 for instance accessors, 3 and 4 for static ones.
    fn declare_private_name(&mut self, name: u32, kind: u8, pos: u32) {
        let declared = self.private_declared.last_mut().unwrap();
        match declared.get_mut(&name) {
            Some(previous) => {
                let pair = (*previous == 1 && kind == 2)
                    || (*previous == 2 && kind == 1)
                    || (*previous == 3 && kind == 4)
                    || (*previous == 4 && kind == 3);
                if pair {
                    *previous = 0;
                } else {
                    self.fail(pos, "Duplicate private field or method");
                }
            }
            None => {
                declared.insert(name, kind);
            }
        }
    }

    fn is_class_element_name_start(&self) -> bool {
        is_name_like(self.tok) || matches!(self.tok, T::PrivateName | T::Num | T::BigInt | T::Str | T::LBracket)
    }

    fn key_is(&self, key: u32, computed: bool, word: u32, text: &[u8]) -> bool {
        if computed {
            return false;
        }
        match self.ast.tag[key as usize] {
            tag::IDENT | tag::IDENT_KEY => self.ast.a[key as usize] == word,
            tag::STR | tag::STR_KEY => {
                let start = self.ast.pos[key as usize] as usize;
                self.ast.a[key as usize] as usize - start == text.len() + 2
                    && (0..text.len()).all(|i| self.at(start + 1 + i) == text[i] as u32)
            }
            _ => false,
        }
    }

    fn parse_class_element(&mut self, has_super_class: bool, had_constructor: &mut bool) -> u32 {
        let start = self.tok_start;
        let mut key_name = NONE;
        let mut is_static = false;
        let mut is_async = false;
        let mut is_generator = false;
        let mut accessor = 0; // 1 for a getter, 2 for a setter.
        let mut method_start = start;

        if self.is_contextual(N_STATIC) {
            self.next();
            method_start = self.tok_start;
            if self.tok == T::LBrace {
                return self.parse_class_static_block(self.tok_start, start);
            }
            if self.is_class_element_name_start() || self.tok == T::Star {
                is_static = true;
            } else {
                key_name = N_STATIC;
            }
        }
        if key_name == NONE && self.is_contextual(N_ASYNC) {
            self.next();
            if (self.is_class_element_name_start() || self.tok == T::Star) && !self.nl_before {
                is_async = true;
            } else {
                key_name = N_ASYNC;
            }
        }
        if key_name == NONE && self.eat(T::Star) {
            is_generator = true;
        }
        if key_name == NONE && !is_async && !is_generator && (self.is_contextual(N_GET) || self.is_contextual(N_SET)) {
            let word = self.tok_name;
            self.next();
            if self.is_class_element_name_start() {
                accessor = if word == N_GET { 1 } else { 2 };
            } else {
                key_name = word;
            }
        }

        let key_start = self.tok_start;
        let mut private_name = NONE;
        let (key, computed) = if key_name != NONE {
            let key_pos = if key_name == N_STATIC {
                self.class_start
            } else {
                self.prev_start
            };
            (self.ast.push(tag::IDENT, key_pos, key_name, 0), false)
        } else if self.tok == T::PrivateName {
            private_name = self.tok_name;
            if self.interner.text(private_name) == b"constructor" {
                self.fail(key_start, "Private property with name '#constructor' is not allowed");
            }
            self.next();
            (self.ast.push(tag::PRIVATE_NAME, key_start, private_name, 0), false)
        } else {
            self.parse_property_name()
        };

        if self.tok == T::LParen || accessor != 0 || is_generator || is_async {
            let is_constructor =
                !is_static && private_name == NONE && self.key_is(key, computed, N_CONSTRUCTOR, b"constructor");
            if is_constructor {
                if accessor != 0 {
                    self.fail(key_start, "Constructor can't have a get or set modifier");
                }
                if is_generator || is_async {
                    self.fail(key_start, "Constructor can't be a generator or async");
                }
                if *had_constructor {
                    self.fail(key_start, "Duplicate constructor in the same class");
                }
                *had_constructor = true;
            } else if is_static && private_name == NONE && self.key_is(key, computed, N_PROTOTYPE, b"prototype") {
                self.fail(key_start, "Classes may not have a static property named 'prototype'");
            }
            if private_name != NONE {
                let kind = if accessor == 0 {
                    0
                } else {
                    accessor + if is_static { 2 } else { 0 }
                };
                self.declare_private_name(private_name, kind, key_start);
            }
            let (function, count, has_rest) =
                self.parse_method(method_start, is_generator, is_async, is_constructor && has_super_class);
            if is_constructor && !self.is_reparsing() {
                let row = self.ast.b[function as usize] as usize;
                self.index.flags[row] |= function_flag::INSIGHT_USES_THIS;
                if has_super_class {
                    self.index.flags[row] |= function_flag::INSIGHT_THIS_FROM_ENVIRONMENT;
                }
            }
            if accessor != 0 {
                self.check_accessor_parameters(start, accessor == 1, count, has_rest);
            }
            let t = match (accessor, computed) {
                (0, false) => tag::METHOD,
                (0, true) => tag::COMPUTED_METHOD,
                (1, false) => tag::GETTER,
                (1, true) => tag::COMPUTED_GETTER,
                (_, false) => tag::SETTER,
                (_, true) => tag::COMPUTED_SETTER,
            };
            let element = self.ast.push(t, start, key, function);
            return if is_static {
                self.ast.push(tag::STATIC_ELEMENT, start, element, 0)
            } else {
                element
            };
        }

        if self.key_is(key, computed, N_CONSTRUCTOR, b"constructor") {
            self.fail(key_start, "Classes can't have a field named 'constructor'");
        } else if is_static && self.key_is(key, computed, N_PROTOTYPE, b"prototype") {
            self.fail(key_start, "Classes can't have a static field named 'prototype'");
        }
        if private_name != NONE {
            self.declare_private_name(private_name, 0, key_start);
        }
        let mut value = 0;
        if self.eat(T::Assign) {
            value = self.parse_in_pseudo_function(
                ctx::SUPER_PROPERTY | ctx::NEW_TARGET | ctx::NO_ARGUMENTS,
                scope_flag::CLASS_FIELD,
                |parser| parser.parse_assign(false, false),
            );
            // A parenthesized expression ends at its innermost closing parenthesis.
            let mut inner = value;
            let mut end = self.tok_start;
            while self.ast.tag[inner as usize] == tag::PAREN {
                end = self.ast.b[inner as usize];
                inner = self.ast.a[inner as usize];
            }
            let extra = self.ast.extra.len() as u32;
            self.ast.extra.push(self.last_pseudo_function_row);
            self.ast.extra.push(NONE);
            value = self.ast.push(tag::FIELD_INITIALIZER, end, value, extra);
        }
        self.semicolon();
        if value != 0 {
            let extra = self.ast.b[value as usize];
            let row = self.class_function_row(
                start,
                self.class_start,
                function_flag::FIELD_INITIALIZER | self.pseudo_function_flags,
                NONE,
                self.pseudo_function_row,
            );
            self.ast.extra[extra as usize + 1] = row;
        }
        let element = self.ast.push(
            if computed {
                tag::COMPUTED_CLASS_FIELD
            } else {
                tag::CLASS_FIELD
            },
            start,
            key,
            value,
        );
        if is_static {
            self.ast.push(tag::STATIC_ELEMENT, start, element, 0)
        } else {
            element
        }
    }

    /// Parses code that runs as a function of its own, but is part of the class's tree.
    fn parse_in_pseudo_function(
        &mut self,
        new_ctx: u32,
        scope_flags: u16,
        parse: impl FnOnce(&mut Self) -> u32,
    ) -> u32 {
        // NB: What the code captures goes by its row, which it needs to have from the start for that.
        let row = if self.is_reparsing() {
            NONE
        } else {
            self.open_index_row(self.class_start)
        };
        let func = self.new_function_id(row);
        let saved_summary_ordinal = core::mem::replace(&mut self.summary_ordinal, 0);
        let saved_ctx = core::mem::replace(&mut self.ctx, new_ctx);
        let saved_labels_base = self.labels_base;
        self.labels_base = self.labels.len() as u32;
        let (saved_yield, saved_await, saved_await_ident) = (self.yield_pos, self.await_pos, self.await_ident_pos);
        let saved_in_arrow = self.in_arrow;
        if scope_flags & scope_flag::STATIC_BLOCK != 0 {
            self.in_arrow = false;
        }
        self.open_scope_with(scope_flags, func);
        let id = self.cur_scope;
        let node = parse(self);
        let scope = *self.scopes.last().expect("the scope is still there");
        let has_eval = scope.has_eval;
        self.last_pseudo_function_row = self.close_scope();
        self.summary_ordinal = saved_summary_ordinal;
        self.pseudo_function_flags = if has_eval { function_flag::CONTAINS_EVAL } else { 0 };
        self.resolve_implicit(N_DOT_THIS, id, func);
        self.resolve_implicit(N_DOT_NEW_TARGET, id, func);
        self.resolve_implicit(N_DOT_SUPER, id, func);
        if row != NONE {
            self.note_uses_of_nested_functions(scope.use_mark, func);
        }
        self.pseudo_function_row = row;
        self.ctx = saved_ctx;
        self.in_arrow = saved_in_arrow;
        self.labels_base = saved_labels_base;
        (self.yield_pos, self.await_pos, self.await_ident_pos) = (saved_yield, saved_await, saved_await_ident);
        node
    }

    fn parse_class_static_block(&mut self, start: u32, element_start: u32) -> u32 {
        let new_ctx = ctx::SUPER_PROPERTY | ctx::NEW_TARGET | ctx::STATIC_BLOCK | ctx::NO_ARGUMENTS;
        let list = self.parse_in_pseudo_function(new_ctx, scope_flag::STATIC_BLOCK, |parser| {
            parser.parse_block_statements()
        });
        let row = self.class_function_row(
            element_start,
            self.class_start,
            function_flag::STATIC_BLOCK | self.pseudo_function_flags,
            NONE,
            self.pseudo_function_row,
        );
        let extra = self.ast.extra.len() as u32;
        self.ast.extra.push(self.last_pseudo_function_row);
        self.ast.extra.push(row);
        self.ast.push(tag::CLASS_STATIC_BLOCK, start, list, extra)
    }

    /// The row of the code of a class that runs as a function: the initializer of a field, a static block or a default
    /// constructor, which is known by the offset of the class element or of the class. The code stays part of the tree
    /// of the class, so that nothing else ever reads the row but the compilation of the function.
    fn class_function_row(&mut self, key: u32, start: u32, flags: u32, name: u32, reserved_row: u32) -> u32 {
        if self.is_reparsing() {
            return self
                .first_pass_row_at(key)
                .expect("the first parse made a row for the code of a class");
        }
        let row = if reserved_row == NONE {
            self.open_index_row(start)
        } else {
            reserved_row
        };
        let reaches_source_end = self.tok_end as usize + super::SOURCE_PADDING > self.len as usize;
        let index = &mut self.index;
        index.params_start[row as usize] = key;
        index.end[row as usize] = self.prev_end;
        index.flags[row as usize] = flags
            | function_flag::STRICT
            | if reaches_source_end {
                function_flag::REACHES_SOURCE_END
            } else {
                0
            };
        index.ctx[row as usize] = self.ctx as u16;
        index.name[row as usize] = name;
        row
    }

    /// Parses a class element on its own, for the function that the element is. Returns the element without its
    /// `static`.
    pub(crate) fn reparse_class_element(&mut self) -> u32 {
        self.open_scope(scope_flag::CLASS);
        self.strict = true;
        let mut had_constructor = false;
        let element = self.parse_class_element(false, &mut had_constructor);
        if self.ast.tag[element as usize] == tag::STATIC_ELEMENT {
            self.ast.a[element as usize]
        } else {
            element
        }
    }

    /// A class node of nothing but what the default constructor at `row` reads of its class.
    pub(crate) fn default_constructor_class_node(&mut self, row: u32) -> u32 {
        let (start, end, name, flags) = self.function_row_summary(row);
        let extra = self.ast.extra.len() as u32;
        self.ast
            .extra
            .push(u32::from(flags & function_flag::DERIVED_CONSTRUCTOR != 0));
        self.ast.extra.push(0);
        self.ast.extra.push(NONE);
        self.ast.extra.push(0);
        self.ast.extra.push(end);
        self.ast.extra.push(row);
        self.ast.push(tag::CLASS, start, name, extra)
    }

    // === Modules ===

    fn parse_module_export_name(&mut self) -> u32 {
        let start = self.tok_start;
        if self.tok == T::Str {
            let name = self.intern_string_literal();
            self.next();
            return self.ast.push(tag::STR, start, name, 0);
        }
        if !is_name_like(self.tok) {
            self.unexpected();
            return 0;
        }
        let name = self.tok_name;
        self.next();
        self.ast.push(tag::IDENT, start, name, 0)
    }

    /// The name a string literal spells. Module export names are the only strings that need one, and they must be well
    /// formed, which also rules out lone surrogates written as escapes.
    fn intern_string_literal(&mut self) -> u32 {
        let (start, end) = (self.tok_start as usize + 1, self.tok_end as usize - 1);
        let mut buffer = Vec::with_capacity(end - start);
        let mut p = start;
        while p < end {
            let mut c = self.at(p);
            p += 1;
            if c == 0x5C {
                if self.at(p) == 0x75 {
                    let mut value = 0;
                    if self.at(p + 1) == 0x7B {
                        let mut q = p + 2;
                        while self.at(q) != 0x7D {
                            value = value * 16 + char::from_u32(self.at(q)).and_then(|d| d.to_digit(16)).unwrap_or(0);
                            q += 1;
                        }
                        p = q + 1;
                    } else {
                        for i in 1..=4 {
                            value =
                                value * 16 + char::from_u32(self.at(p + i)).and_then(|d| d.to_digit(16)).unwrap_or(0);
                        }
                        p += 5;
                    }
                    c = value;
                } else {
                    c = self.at(p);
                    p += 1;
                }
            }
            if (0xD800..0xDC00).contains(&c) {
                let (mut low, mut after) = (self.at(p), p + 1);
                if low == 0x5C && self.at(p + 1) == 0x75 {
                    low = 0;
                    for i in 2..=5 {
                        low = low * 16 + char::from_u32(self.at(p + i)).and_then(|d| d.to_digit(16)).unwrap_or(0);
                    }
                    after = p + 6;
                }
                if (0xDC00..0xE000).contains(&low) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    p = after;
                }
            }
            match char::from_u32(c) {
                Some(ch) => {
                    let mut encoded = [0u8; 4];
                    buffer.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
                }
                None => {
                    self.fail(self.tok_start, "Export name is not well formed");
                    break;
                }
            }
        }
        self.interner.intern_bytes(&buffer)
    }

    /// Returns the list of keys and values, as interned names, or zero if there are none.
    fn parse_import_attributes(&mut self) -> u32 {
        if self.tok != T::KWith {
            return 0;
        }
        self.next();
        self.expect(T::LBrace);
        let mark = self.scratch.len();
        let mut seen = std::collections::HashSet::new();
        while self.tok != T::RBrace && self.tok != T::Eof {
            let key = if self.tok == T::Str {
                self.intern_string_literal()
            } else if is_name_like(self.tok) {
                self.tok_name
            } else {
                self.unexpected();
                self.scratch.truncate(mark);
                return 0;
            };
            if !seen.insert(key) {
                self.fail(self.tok_start, "Duplicate import attribute key");
            }
            self.next();
            self.expect(T::Colon);
            if self.tok != T::Str {
                self.unexpected();
                self.scratch.truncate(mark);
                return 0;
            }
            let value = self.intern_string_literal();
            self.scratch.push(key);
            self.scratch.push(value);
            self.next();
            if self.tok != T::RBrace {
                self.expect_comma_or(T::RBrace);
            }
        }
        self.expect(T::RBrace);
        self.finish_list(mark)
    }

    fn parse_import(&mut self) -> u32 {
        let start = self.tok_start;
        self.next();
        let mark = self.scratch.len();
        if self.tok == T::Str {
            let source = self.parse_module_source();
            let attributes = self.parse_import_attributes();
            self.ast.b[source as usize] = attributes;
            self.semicolon();
            let list = self.finish_list(mark);
            return self.ast.push(tag::IMPORT, start, list, source);
        }
        let mut more = true;
        if self.tok == T::Name {
            let specifier = self.parse_import_binding(0);
            self.scratch.push(specifier);
            more = self.eat(T::Comma);
        }
        if more {
            if self.eat(T::Star) {
                if !self.eat_contextual(N_AS) {
                    self.unexpected();
                }
                let specifier = self.parse_import_binding(NONE);
                self.scratch.push(specifier);
            } else {
                self.expect(T::LBrace);
                while self.tok != T::RBrace && self.tok != T::Eof {
                    let was_string = self.tok == T::Str;
                    let keyword = self.tok != T::Name;
                    let imported_start = self.tok_start;
                    let imported_name = self.tok_name;
                    let imported = self.parse_module_export_name();
                    let specifier = if self.eat_contextual(N_AS) {
                        self.parse_import_binding(imported)
                    } else {
                        if was_string || keyword {
                            self.fail(imported_start, "Import specifier needs a local name");
                        }
                        self.check_binding_name(imported_name, imported_start);
                        self.declare(imported_name, Bind::Lexical, imported_start);
                        self.ast.push(tag::SPECIFIER, imported_start, imported, imported_name)
                    };
                    self.scratch.push(specifier);
                    if self.tok != T::RBrace {
                        self.expect_comma_or(T::RBrace);
                    }
                }
                self.expect(T::RBrace);
            }
        }
        if !self.eat_contextual(N_FROM) {
            self.unexpected();
        }
        let source = self.parse_module_source();
        let attributes = self.parse_import_attributes();
        self.ast.b[source as usize] = attributes;
        self.semicolon();
        let list = self.finish_list(mark);
        self.ast.push(tag::IMPORT, start, list, source)
    }

    fn parse_import_binding(&mut self, imported: u32) -> u32 {
        let start = self.tok_start;
        let (name, pos) = self.parse_ident();
        self.check_binding_name(name, pos);
        if name == N_LET {
            self.fail(pos, "'let' is disallowed as a lexically bound name");
        }
        self.declare(name, Bind::Lexical, pos);
        self.ast.push(tag::SPECIFIER, start, imported, name)
    }

    fn parse_module_source(&mut self) -> u32 {
        let start = self.tok_start;
        if self.tok != T::Str {
            self.unexpected();
            return 0;
        }
        let end = self.tok_end;
        self.next();
        self.ast.push(tag::STR, start, end, 0)
    }

    fn add_export_name(&mut self, name: u32, pos: u32) {
        if !self.export_names.insert(name) {
            self.fail(pos, "Duplicate export");
        }
    }

    fn export_pattern_names(&mut self, node: u32) {
        match self.ast.tag[node as usize] {
            tag::IDENT => self.add_export_name(self.ast.a[node as usize], self.ast.pos[node as usize]),
            tag::ARRAY_PATTERN | tag::OBJECT_PATTERN => {
                let list = self.ast.a[node as usize];
                for i in 0..self.ast.extra[list as usize] {
                    let item = self.ast.extra[(list + 1 + i) as usize];
                    self.export_pattern_names(item);
                }
            }
            tag::PROPERTY | tag::COMPUTED_PROPERTY | tag::SHORTHAND => {
                self.export_pattern_names(self.ast.b[node as usize]);
            }
            tag::REST | tag::ASSIGN_PATTERN => self.export_pattern_names(self.ast.a[node as usize]),
            _ => {}
        }
    }

    fn parse_export(&mut self) -> u32 {
        let start = self.tok_start;
        self.next();
        if self.eat(T::Star) {
            let mut exported = 0;
            if self.eat_contextual(N_AS) {
                let name_start = self.tok_start;
                let name = if self.tok == T::Str {
                    self.intern_string_literal()
                } else {
                    self.tok_name
                };
                exported = self.parse_module_export_name();
                self.add_export_name(name, name_start);
            }
            if !self.eat_contextual(N_FROM) {
                self.unexpected();
            }
            let source = self.parse_module_source();
            let attributes = self.parse_import_attributes();
            self.ast.b[source as usize] = attributes;
            self.semicolon();
            return self.ast.push(tag::EXPORT_ALL, start, exported, source);
        }
        if self.tok == T::KDefault {
            let default_pos = self.tok_start;
            self.add_export_name(
                KEYWORDS.iter().position(|(word, _)| *word == "default").unwrap() as u32,
                default_pos,
            );
            self.next();
            let value_start = self.tok_start;
            let is_async_function = self.is_async_function();
            let declaration = if self.tok == T::KFunction || is_async_function {
                if is_async_function {
                    self.next();
                }
                self.next();
                let is_generator = self.eat(T::Star);
                if self.tok == T::Name {
                    let (name, pos) = self.parse_ident();
                    self.declare(name, Bind::Lexical, pos);
                    self.parse_function_rest(value_start, Some((name, pos)), is_generator, is_async_function)
                } else {
                    self.parse_function_rest(value_start, None, is_generator, is_async_function)
                }
            } else if self.tok == T::KClass {
                let (next_kind, _, _) = self.peek();
                let named = next_kind == T::Name && !(self.peek().1 == N_IMPLEMENTS);
                self.parse_class(named)
            } else {
                let expression = self.parse_assign(false, false);
                self.semicolon();
                expression
            };
            let is_declaration = matches!(
                self.ast.tag[declaration as usize],
                tag::FUNCTION | tag::CLASS_DECL | tag::CLASS
            ) && (self.tok_is_declaration_export(value_start));
            return self
                .ast
                .push(tag::EXPORT_DEFAULT, start, declaration, is_declaration as u32);
        }
        if self.tok == T::LBrace {
            self.next();
            let mark = self.scratch.len();
            let mut had_string_local = false;
            let locals_mark = self.export_locals.len();
            let mut pending: Vec<(u32, u32)> = Vec::new();
            while self.tok != T::RBrace && self.tok != T::Eof {
                let local_start = self.tok_start;
                let local_kind = self.tok;
                let local_name = if local_kind == T::Str {
                    self.intern_string_literal()
                } else {
                    self.tok_name
                };
                let local = self.parse_module_export_name();
                had_string_local |= local_kind == T::Str;
                let mut exported_name = local_name;
                let mut exported_pos = local_start;
                let mut exported = local;
                if self.eat_contextual(N_AS) {
                    exported_pos = self.tok_start;
                    exported_name = if self.tok == T::Str {
                        self.intern_string_literal()
                    } else {
                        self.tok_name
                    };
                    exported = self.parse_module_export_name();
                }
                pending.push((exported_name, exported_pos));
                self.export_locals
                    .push((local_name, local_start | ((local_kind != T::Name) as u32) << 31));
                let specifier = self.ast.push(tag::SPECIFIER, local_start, local, exported);
                self.scratch.push(specifier);
                if self.tok != T::RBrace {
                    self.expect_comma_or(T::RBrace);
                }
            }
            self.expect(T::RBrace);
            for (name, pos) in pending {
                self.add_export_name(name, pos);
            }
            let mut source = 0;
            if self.eat_contextual(N_FROM) {
                source = self.parse_module_source();
                let attributes = self.parse_import_attributes();
                self.ast.b[source as usize] = attributes;
                self.export_locals.truncate(locals_mark);
            } else {
                if had_string_local {
                    self.fail(start, "A string cannot be a local export name");
                }
                for i in locals_mark..self.export_locals.len() {
                    let (name, pos) = self.export_locals[i];
                    if pos >> 31 != 0 {
                        self.fail(pos & 0x7FFF_FFFF, "Keyword as a local export name");
                    }
                    self.check_identifier(name, pos);
                    self.note_use(name);
                }
            }
            self.semicolon();
            let list = self.finish_list(mark);
            return self.ast.push(tag::EXPORT_NAMED, start, list, source);
        }
        // `export` of a declaration.
        let is_var = matches!(self.tok, T::KVar | T::KConst) || self.is_let(Context::List);
        if is_var {
            let statement = self.parse_statement(Context::List, false);
            let list = self.ast.a[statement as usize];
            for i in 0..self.ast.extra[list as usize] {
                let declarator = self.ast.extra[(list + 1 + i) as usize];
                self.export_pattern_names(self.ast.a[declarator as usize]);
            }
            return self.ast.push(tag::EXPORT_NAMED, start, 0, statement);
        }
        if !matches!(self.tok, T::KFunction | T::KClass) && !self.is_async_function() {
            self.unexpected();
            return 0;
        }
        let name_pos = self.tok_start;
        let statement = self.parse_statement(Context::List, false);
        let name = match self.ast.tag[statement as usize] {
            tag::FUNCTION_DECL => self.ast.b[statement as usize],
            tag::CLASS_DECL => self.ast.a[self.ast.a[statement as usize] as usize],
            _ => 0,
        };
        self.add_export_name(name, name_pos);
        self.ast.push(tag::EXPORT_NAMED, start, 0, statement)
    }

    fn tok_is_declaration_export(&self, value_start: u32) -> bool {
        // `export default` of a function or class declaration rather than of an expression that happens to be one.
        matches!(self.at(value_start as usize), 0x66 | 0x63 | 0x61)
    }

    pub(crate) fn check_exported_locals(&mut self) {
        for i in 0..self.export_locals.len() {
            let (name, pos) = self.export_locals[i];
            if !self.is_declared_at_top(name) {
                self.fail(pos & 0x7FFF_FFFF, "Export of an undeclared name");
                return;
            }
        }
    }
}
