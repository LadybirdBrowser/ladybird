/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Expressions. Arrow function parameters and destructuring assignment targets are parsed as expressions, and a tag
//! rewrite in place turns them into patterns once `=>` or `=` shows what they were, without going back over them.

use super::base::*;
use super::columns::*;
use super::token::*;
use super::unit::Unit;

pub(crate) const fn starts_expression(kind: T) -> bool {
    matches!(
        kind,
        T::Name
            | T::PrivateName
            | T::Num
            | T::BigInt
            | T::Str
            | T::Backtick
            | T::LBrace
            | T::LParen
            | T::LBracket
            | T::Inc
            | T::Dec
            | T::Not
            | T::Tilde
            | T::Plus
            | T::Minus
            | T::Slash
            | T::DivAssign
            | T::KClass
            | T::KDelete
            | T::KFalse
            | T::KFunction
            | T::KImport
            | T::KNew
            | T::KNull
            | T::KSuper
            | T::KThis
            | T::KTrue
            | T::KTypeof
            | T::KVoid
    )
}

#[inline(always)]
pub(crate) fn is_name_like(kind: T) -> bool {
    kind == T::Name || kind == T::EscapedKeyword || kind as u8 >= T::KIn as u8
}

impl<U: Unit> Parser<'_, U> {
    pub(crate) fn parse_expression(&mut self, no_in: bool, cover_ok: bool) -> u32 {
        let start = self.tok_start;
        let first = self.parse_assign(no_in, cover_ok);
        if self.tok != T::Comma {
            return first;
        }
        let mark = self.scratch.len();
        self.scratch.push(first);
        while self.eat(T::Comma) {
            let next = self.parse_assign(no_in, cover_ok);
            self.scratch.push(next);
        }
        let list = self.finish_list(mark);
        self.ast.push(tag::SEQUENCE, start, list, 0)
    }

    pub(crate) fn parse_assign(&mut self, no_in: bool, cover_ok: bool) -> u32 {
        if !self.enter() {
            return 0;
        }
        let expression = self.parse_assign_inner(no_in, cover_ok);
        self.leave();
        expression
    }

    fn parse_assign_inner(&mut self, no_in: bool, cover_ok: bool) -> u32 {
        if self.tok == T::Name && self.tok_name == N_YIELD && self.ctx & ctx::GENERATOR != 0 && !self.tok_escaped {
            return self.parse_yield(no_in);
        }
        let saved_cover_error = self.cover_error;
        self.cover_error = NONE;
        let start = self.tok_start;
        if self.tok == T::LParen || self.tok == T::Name {
            self.potential_arrow_at = start;
        }
        let mut left = self.parse_conditional(no_in);
        let kind = self.tok as u8;
        let operator_pos = self.tok_start;
        if kind >= T::Assign as u8 && kind <= T::NullishAssign as u8 {
            if self.tok == T::Assign {
                self.convert_assignment_target(left);
                self.cover_error = NONE;
                self.mark(tag::MARK_TARGET, 0);
            } else {
                let allow_call =
                    self.tok != T::LogAndAssign && self.tok != T::LogOrAssign && self.tok != T::NullishAssign;
                self.check_simple_target(left, allow_call, "Invalid left-hand side in assignment");
                self.mark(tag::MARK_READ_TARGET, 0);
                if !allow_call {
                    self.mark(tag::MARK_LOGICAL_ASSIGNMENT, u32::from(kind));
                }
            }
            self.next();
            let right = self.parse_assign(no_in, false);
            let target = left;
            left = self
                .ast
                .push(tag::ASSIGN_BASE + (kind - T::Assign as u8), operator_pos, left, right);
            if self.marks && matches!(self.ast.tag[target as usize], tag::ARRAY_PATTERN | tag::OBJECT_PATTERN) {
                // NB: The rows of a pattern run after what comes behind them, which a sweep finds out at their start.
                let mark = self.ast.b[target as usize];
                self.ast.tag[mark as usize] = tag::MARK_SUBTREE;
                self.ast.a[mark as usize] = left;
            }
        }
        if self.cover_error != NONE {
            if !cover_ok {
                self.fail(
                    self.cover_error,
                    "Shorthand property initializers are only valid in patterns",
                );
            }
        } else {
            self.cover_error = saved_cover_error;
        }
        left
    }

    fn parse_yield(&mut self, no_in: bool) -> u32 {
        if self.yield_pos == 0 {
            self.yield_pos = self.tok_start;
        }
        let start = self.tok_start;
        self.next();
        if self.tok == T::Semi || self.can_insert_semicolon() || (self.tok != T::Star && !starts_expression(self.tok)) {
            return self.ast.push(tag::YIELD, start, 0, 0);
        }
        let delegate = self.eat(T::Star);
        let argument = self.parse_assign(no_in, false);
        self.ast
            .push(if delegate { tag::YIELD_STAR } else { tag::YIELD }, start, argument, 0)
    }

    #[inline(always)]
    fn parse_conditional(&mut self, no_in: bool) -> u32 {
        let start = self.tok_start;
        let test = self.parse_binary_expression(no_in);
        if self.tok != T::Question || test == self.last_arrow {
            return test;
        }
        let operator_pos = self.tok_start;
        self.mark(tag::MARK_CONDITIONAL_TEST, 0);
        self.next();
        let consequent = self.parse_assign(false, false);
        self.mark(tag::MARK_CONDITIONAL_ELSE, 0);
        self.expect(T::Colon);
        let alternate = self.parse_assign(no_in, false);
        let extra = self.ast.extra.len() as u32;
        self.ast.extra.push(consequent);
        self.ast.extra.push(alternate);
        let _ = start;
        self.ast.push(tag::CONDITIONAL, operator_pos, test, extra)
    }

    #[inline(always)]
    fn parse_binary_expression(&mut self, no_in: bool) -> u32 {
        let start = self.tok_start;
        let left = self.parse_unary(false, false, no_in);
        if PRECEDENCE[self.tok as usize] == 0 || left == self.last_arrow {
            return left;
        }
        self.parse_binary_operators(left, start, 0, no_in)
    }

    fn parse_binary_operators(&mut self, mut left: u32, left_start: u32, min_precedence: u8, no_in: bool) -> u32 {
        loop {
            let kind = self.tok;
            let operator_pos = self.tok_start;
            let precedence = PRECEDENCE[kind as usize];
            if precedence <= min_precedence || (kind == T::KIn && no_in) {
                return left;
            }
            let logical = kind == T::LogOr || kind == T::LogAnd;
            let coalesce = kind == T::Nullish;
            let right_precedence = if coalesce {
                PRECEDENCE[T::LogAnd as usize]
            } else {
                precedence
            };
            if logical || coalesce {
                self.mark(
                    tag::MARK_LOGICAL,
                    u32::from(tag::BINARY_BASE + (kind as u8 - T::Nullish as u8)),
                );
            }
            self.next();
            let right_start = self.tok_start;
            let operand = self.parse_unary(false, false, no_in);
            let right = self.parse_binary_operators(operand, right_start, right_precedence, no_in);
            if self.ast.tag[right as usize] == tag::PRIVATE_NAME {
                self.fail(right_start, "A private name can only be the left side of `in`");
            }
            let _ = left_start;
            left = self.ast.push(
                tag::BINARY_BASE + (kind as u8 - T::Nullish as u8),
                operator_pos,
                left,
                right,
            );
            if (logical && self.tok == T::Nullish) || (coalesce && (self.tok == T::LogOr || self.tok == T::LogAnd)) {
                self.fail(
                    self.tok_start,
                    "`??` cannot be mixed with `||` or `&&` without parentheses",
                );
            }
        }
    }

    fn is_local_variable_access(&self, node: u32) -> bool {
        match self.ast.tag[node as usize] {
            tag::IDENT => true,
            tag::PAREN => self.is_local_variable_access(self.ast.a[node as usize]),
            _ => false,
        }
    }

    fn is_private_field_access(&self, node: u32) -> bool {
        match self.ast.tag[node as usize] {
            tag::PRIVATE_MEMBER | tag::OPT_PRIVATE_MEMBER => true,
            tag::PAREN | tag::CHAIN => self.is_private_field_access(self.ast.a[node as usize]),
            _ => false,
        }
    }

    /// The operand of a unary operator, which can nest without anything else in between.
    fn parse_operand(&mut self, saw_unary: bool, inc_dec: bool, no_in: bool) -> u32 {
        if !self.enter() {
            return 0;
        }
        let operand = self.parse_unary(saw_unary, inc_dec, no_in);
        self.leave();
        operand
    }

    fn parse_unary(&mut self, mut saw_unary: bool, inc_dec: bool, no_in: bool) -> u32 {
        let start = self.tok_start;
        let mut expression;
        match self.tok {
            T::Name if self.tok_name == N_AWAIT && self.ctx & ctx::CAN_AWAIT != 0 && !self.tok_escaped => {
                if self.await_pos == 0 {
                    self.await_pos = start;
                }
                if self.ctx & ctx::ASYNC == 0 {
                    self.has_top_level_await = true;
                }
                self.next();
                let argument = self.parse_operand(true, false, no_in);
                expression = self.ast.push(tag::AWAIT, start, argument, 0);
                saw_unary = true;
            }
            T::Not | T::Tilde | T::Plus | T::Minus | T::KTypeof | T::KVoid | T::KDelete | T::Inc | T::Dec => {
                let kind = self.tok;
                let update = kind == T::Inc || kind == T::Dec;
                self.next();
                let argument = self.parse_operand(true, update, no_in);
                if update {
                    self.check_simple_target(argument, true, "Invalid left-hand side in prefix operation");
                    self.mark_at(tag::MARK_READ_TARGET, 0, start);
                } else {
                    if kind == T::KTypeof {
                        self.mark_at(tag::MARK_TYPEOF, 0, start);
                    }
                    if kind == T::KDelete {
                        self.mark_at(tag::MARK_DELETE, 0, start);
                        if self.strict && self.is_local_variable_access(argument) {
                            self.fail(start, "Deleting a local variable in strict mode");
                        } else if self.is_private_field_access(argument) {
                            self.fail(start, "Private fields cannot be deleted");
                        }
                    }
                    saw_unary = true;
                }
                let t = if update { tag::PREFIX_UPDATE } else { tag::UNARY };
                expression = self.ast.push(t, start, argument, kind as u32);
            }
            T::PrivateName if !saw_unary => {
                if no_in || (self.class_marks.is_empty() && !self.initiated_by_eval) {
                    self.unexpected();
                    return 0;
                }
                expression = self.parse_private_name();
                if self.tok != T::KIn {
                    self.fail(start, "Private identifier must be followed by 'in'");
                }
            }
            _ => {
                expression = self.parse_left_hand_side(no_in);
                if (self.tok == T::Inc || self.tok == T::Dec) && !self.nl_before {
                    while (self.tok == T::Inc || self.tok == T::Dec) && !self.nl_before {
                        let kind = self.tok;
                        let operator_pos = self.tok_start;
                        self.check_simple_target(expression, true, "Invalid left-hand side in postfix operation");
                        self.mark(tag::MARK_READ_TARGET, 0);
                        self.next();
                        expression = self
                            .ast
                            .push(tag::POSTFIX_UPDATE, operator_pos, expression, kind as u32);
                    }
                }
            }
        }
        if !inc_dec && self.tok == T::StarStar {
            if saw_unary {
                self.unexpected();
                return 0;
            }
            let operator_pos = self.tok_start;
            self.next();
            let right = self.parse_operand(false, false, no_in);
            return self.ast.push(
                tag::BINARY_BASE + (T::StarStar as u8 - T::Nullish as u8),
                operator_pos,
                expression,
                right,
            );
        }
        expression
    }

    pub(crate) fn parse_private_name(&mut self) -> u32 {
        let name = self.tok_name;
        let pos = self.tok_start;
        self.private_used.push((name, pos));
        self.next();
        self.ast.push(tag::PRIVATE_NAME, pos, name, 0)
    }

    #[inline(always)]
    pub(crate) fn parse_left_hand_side(&mut self, no_in: bool) -> u32 {
        let start = self.tok_start;
        let atom = self.parse_atom(no_in, false);
        if atom == self.last_arrow && atom != 0 && self.at(self.prev_start as usize) != 0x29 {
            return atom;
        }
        match self.tok {
            T::Dot | T::LBracket | T::LParen | T::Backtick | T::QDot => {
                self.parse_subscripts(atom, start, false, no_in)
            }
            _ => atom,
        }
    }

    fn parse_subscripts(&mut self, mut base: u32, start: u32, no_calls: bool, no_in: bool) -> u32 {
        let base_tag = self.ast.tag[base as usize];
        if (base_tag == tag::OBJECT || base_tag == tag::ARRAY) && self.cover_error != NONE {
            self.fail(
                self.cover_error,
                "Shorthand property initializers are only valid in patterns",
            );
            return base;
        }
        let maybe_async_arrow = base_tag == tag::IDENT
            && self.ast.a[base as usize] == N_ASYNC
            && self.prev_end == start + 5
            && !self.nl_before
            && self.potential_arrow_at == start;
        let mut optional_chained = false;
        let mut chain_pos = start;
        loop {
            // NB: All of an optional chain is where it starts.
            if optional_chained && self.marks {
                self.ast.pos[base as usize] = chain_pos;
            }
            match self.tok {
                T::Dot => {
                    let operator_pos = self.tok_start;
                    self.next();
                    base = self.parse_member_name(base, operator_pos, false);
                }
                T::LBracket => {
                    let operator_pos = self.tok_start;
                    self.next();
                    let property = self.parse_expression(false, false);
                    self.expect(T::RBracket);
                    let index_tag = if self.marks && self.ast.tag[base as usize] == tag::SUPER {
                        tag::SUPER_INDEX
                    } else {
                        tag::INDEX
                    };
                    base = self.ast.push(index_tag, operator_pos, base, property);
                }
                T::LParen if !no_calls => {
                    if maybe_async_arrow && base == self.async_candidate(base) {
                        let result = self.parse_async_call_or_arrow(base, start, no_in);
                        if result == self.last_arrow && result != 0 {
                            if self.marks {
                                // NB: This turned out not to be a reference, and nothing is to evaluate it.
                                self.ast.tag[base as usize] = tag::EMPTY;
                            }
                            return result;
                        }
                        base = result;
                        continue;
                    }
                    let operator_pos = self.tok_start;
                    let mut callee = base;
                    while self.ast.tag[callee as usize] == tag::PAREN {
                        callee = self.ast.a[callee as usize];
                    }
                    let is_eval = self.ast.tag[callee as usize] == tag::IDENT && self.ast.a[callee as usize] == N_EVAL;
                    let position = if optional_chained { chain_pos } else { operator_pos };
                    self.mark_at(tag::MARK_CALLEE, 0, position);
                    let arguments = self.parse_arguments(position);
                    if is_eval {
                        let scope = self.scopes.last_mut().unwrap();
                        scope.has_eval = true;
                        scope.eval_in_function = true;
                        scope.eval_here = true;
                        self.uses_this = true;
                        self.uses_this_from_environment |= self.in_arrow;
                    }
                    let call_tag = if self.ast.tag[callee as usize] == tag::SUPER_CALLEE {
                        tag::SUPER_CALL
                    } else if self.marks && is_eval {
                        if self.arguments_have_spread {
                            tag::CALL_DIRECT_EVAL_SPREAD
                        } else {
                            tag::CALL_DIRECT_EVAL
                        }
                    } else {
                        self.call_tag(tag::CALL)
                    };
                    base = self.ast.push(call_tag, operator_pos, base, arguments);
                }
                T::Backtick => {
                    if optional_chained {
                        self.fail(self.tok_start, "Tagged template in an optional chain");
                        return base;
                    }
                    self.mark_at(tag::MARK_CALLEE, 0, start);
                    let subtree = self.begin_subtree();
                    let quasi = self.parse_template(true);
                    self.end_subtree(subtree, quasi);
                    base = self.ast.push(tag::TAGGED_TEMPLATE, start, base, quasi);
                }
                T::QDot => {
                    if no_calls {
                        self.fail(self.tok_start, "Optional chain in the callee of new");
                        return base;
                    }
                    let operator_pos = self.tok_start;
                    if !optional_chained {
                        chain_pos = operator_pos;
                    }
                    optional_chained = true;
                    self.next();
                    if self.tok == T::LParen {
                        self.mark_at(tag::MARK_CALLEE, 0, chain_pos);
                    }
                    self.mark(tag::MARK_OPTIONAL, u32::from(chain_pos == operator_pos));
                    match self.tok {
                        T::LParen => {
                            let arguments = self.parse_arguments(chain_pos);
                            base = self
                                .ast
                                .push(self.call_tag(tag::OPT_CALL), operator_pos, base, arguments);
                        }
                        T::LBracket => {
                            self.next();
                            let property = self.parse_expression(false, false);
                            self.expect(T::RBracket);
                            base = self.ast.push(tag::OPT_INDEX, operator_pos, base, property);
                        }
                        T::Backtick => {
                            self.fail(self.tok_start, "Tagged template in an optional chain");
                            return base;
                        }
                        _ => base = self.parse_member_name(base, operator_pos, true),
                    }
                }
                _ => break,
            }
        }
        if optional_chained {
            if self.marks {
                self.ast.pos[base as usize] = chain_pos;
            }
            base = self.ast.push(tag::CHAIN, chain_pos, base, 0);
        }
        base
    }

    #[inline(always)]
    fn async_candidate(&self, base: u32) -> u32 {
        if self.ast.tag[base as usize] == tag::IDENT {
            base
        } else {
            0
        }
    }

    #[inline(always)]
    fn parse_member_name(&mut self, base: u32, start: u32, optional: bool) -> u32 {
        if self.tok == T::PrivateName {
            if self.ast.tag[base as usize] == tag::SUPER {
                self.unexpected();
                return base;
            }
            let name = self.tok_name;
            self.private_used.push((name, self.tok_start));
            self.next();
            let t = if optional {
                tag::OPT_PRIVATE_MEMBER
            } else {
                tag::PRIVATE_MEMBER
            };
            return self.ast.push(t, start, base, name);
        }
        if !is_name_like(self.tok) {
            self.unexpected();
            return base;
        }
        let name = self.tok_name;
        if optional && self.tok == T::Name && (name == N_ARGUMENTS || name == N_EVAL) {
            self.might_need_arguments = true;
        }
        self.next();
        let member_tag = if optional {
            tag::OPT_MEMBER
        } else if self.marks && self.ast.tag[base as usize] == tag::SUPER {
            tag::SUPER_MEMBER
        } else {
            tag::MEMBER
        };
        self.ast.push(member_tag, start, base, name)
    }

    /// What a call with the list of arguments that was parsed last is, of a tree for a sweep too.
    #[inline(always)]
    fn call_tag(&self, plain: u8) -> u8 {
        if self.marks && self.arguments_have_spread {
            tag::CALL_SPREAD
        } else {
            plain
        }
    }

    /// The arguments of a call, from the `(`, of a call that is at `position`.
    fn parse_arguments(&mut self, position: u32) -> u32 {
        self.next();
        let mark = self.scratch.len();
        let mut has_spread = false;
        while self.tok != T::RParen && self.tok != T::Eof {
            let item = if self.tok == T::Ellipsis {
                let start = self.tok_start;
                if !has_spread {
                    self.mark_spread_start(self.scratch.len() - mark, position);
                    has_spread = true;
                }
                self.next();
                let argument = self.parse_assign(false, false);
                self.ast.push(tag::SPREAD, start, argument, 0)
            } else {
                let argument = self.parse_assign(false, false);
                if has_spread {
                    self.mark(tag::MARK_APPEND, 0);
                }
                argument
            };
            self.scratch.push(item);
            if self.tok != T::RParen {
                self.expect_comma_or(T::RParen);
            }
        }
        self.expect(T::RParen);
        self.arguments_have_spread = has_spread;
        self.finish_list(mark)
    }

    /// `async(...)`, which is a call unless `=>` follows.
    fn parse_async_call_or_arrow(&mut self, base: u32, start: u32, no_in: bool) -> u32 {
        if let Some(arrow) = self.skip_arrow_function(start, self.tok_start) {
            return arrow;
        }
        let marks = self.ast.marks();
        let (old_yield, old_await, old_await_ident) = (self.yield_pos, self.await_pos, self.await_ident_pos);
        self.yield_pos = 0;
        self.await_pos = 0;
        self.await_ident_pos = 0;
        let saved_cover_error = self.cover_error;
        self.cover_error = NONE;
        self.open_scope(0);
        self.next();
        let mark = self.scratch.len();
        let params_start = self.prev_start;
        let mut has_spread = false;
        while self.tok != T::RParen && self.tok != T::Eof {
            let item = if self.tok == T::Ellipsis {
                let item_start = self.tok_start;
                if !has_spread {
                    self.mark_spread_start(self.scratch.len() - mark, params_start);
                    has_spread = true;
                }
                self.next();
                let argument = self.parse_assign(false, true);
                let trailing = (self.tok == T::Comma) as u32;
                self.ast.push(tag::SPREAD, item_start, argument, trailing)
            } else {
                let argument = self.parse_assign(false, true);
                if has_spread {
                    self.mark(tag::MARK_APPEND, 0);
                }
                argument
            };
            self.scratch.push(item);
            if self.tok != T::RParen {
                self.expect_comma_or(T::RParen);
            }
        }
        self.expect(T::RParen);
        if self.tok == T::Arrow && !self.nl_before {
            if self.yield_pos != 0 || self.await_pos != 0 {
                self.fail(start, "Yield or await expression in arrow function parameters");
            }
            if self.await_ident_pos != 0 {
                self.fail(
                    self.await_ident_pos,
                    "Cannot use 'await' as identifier inside an async function",
                );
            }
            self.yield_pos = old_yield;
            self.await_pos = old_await;
            self.await_ident_pos = old_await_ident;
            self.cover_error = NONE;
            let list = self.finish_list(mark);
            return self.parse_arrow_from_cover(start, params_start, marks, list, true, no_in, saved_cover_error);
        }
        if self.cover_error != NONE {
            self.fail(
                self.cover_error,
                "Shorthand property initializers are only valid in patterns",
            );
        }
        self.cover_error = saved_cover_error;
        if old_yield != 0 {
            self.yield_pos = old_yield;
        }
        if old_await != 0 {
            self.await_pos = old_await;
        }
        if old_await_ident != 0 {
            self.await_ident_pos = old_await_ident;
        }
        self.close_scope();
        let list = self.finish_list(mark);
        self.note_use(N_ASYNC);
        let call_tag = if self.marks && has_spread {
            tag::CALL_SPREAD
        } else {
            tag::CALL
        };
        self.ast.push(call_tag, params_start, base, list)
    }

    fn check_identifier_reserved(&mut self, name: u32, pos: u32) {
        match name {
            N_YIELD => {
                if self.ctx & ctx::GENERATOR != 0 || self.strict {
                    self.fail(pos, "'yield' is not allowed as an identifier in this context");
                }
            }
            N_AWAIT => {
                if self.ctx & (ctx::ASYNC | ctx::STATIC_BLOCK) != 0 || self.module {
                    self.fail(pos, "'await' is not allowed as an identifier in this context");
                }
                if self.await_ident_pos == 0 {
                    self.await_ident_pos = pos;
                }
            }
            N_ARGUMENTS => {
                if self.ctx & ctx::STATIC_BLOCK != 0 {
                    self.fail(pos, "'arguments' is not allowed in class static initialization blocks");
                } else if self.ctx & ctx::NO_ARGUMENTS != 0 {
                    self.fail(pos, "'arguments' is not allowed in class field initializer");
                }
            }
            _ => {
                if self.strict && (N_IMPLEMENTS..N_YIELD).contains(&name) {
                    self.fail(pos, "Identifier must not be a reserved word in strict mode");
                }
            }
        }
    }

    #[inline(always)]
    pub(crate) fn check_identifier(&mut self, name: u32, pos: u32) {
        if name < FIRST_PLAIN_NAME {
            self.check_identifier_reserved(name, pos);
        }
    }

    #[inline(always)]
    pub(crate) fn check_binding_name(&mut self, name: u32, pos: u32) {
        if name < FIRST_PLAIN_NAME {
            self.check_identifier_reserved(name, pos);
            if self.strict && (name == N_EVAL || name == N_ARGUMENTS) {
                self.fail(
                    pos,
                    "Binding pattern target may not be called 'arguments' or 'eval' in strict mode",
                );
            }
        }
    }

    /// What a binding name has to pass once a function turns out to be strict, on top of what it passed when parsed.
    pub(crate) fn check_strict_binding_name(&mut self, name: u32, pos: u32) {
        if name == N_EVAL || name == N_ARGUMENTS || (N_IMPLEMENTS..=N_YIELD).contains(&name) {
            self.fail(
                pos,
                "Binding pattern target may not be called 'arguments' or 'eval' in strict mode",
            );
        }
    }

    /// An identifier in a place that is no reference, which still has to be a valid one.
    pub(crate) fn parse_ident(&mut self) -> (u32, u32) {
        let (name, pos) = (self.tok_name, self.tok_start);
        if self.tok != T::Name {
            self.unexpected();
            return (0, pos);
        }
        self.check_identifier(name, pos);
        self.next();
        (name, pos)
    }

    fn parse_atom(&mut self, no_in: bool, for_new: bool) -> u32 {
        let start = self.tok_start;
        match self.tok {
            T::Name => {
                let name = self.tok_name;
                let escaped = self.tok_escaped;
                let can_be_arrow = self.potential_arrow_at == start;
                self.check_identifier(name, start);
                if name == N_ARGUMENTS || name == N_EVAL {
                    self.might_need_arguments = true;
                    if name == N_ARGUMENTS
                        && (!self.strict || self.in_class_of_sloppy_function)
                        && !self.is_declared_in_current_function(name)
                    {
                        self.scopes.last_mut().unwrap().arguments_access = true;
                    }
                }
                self.next();
                if name == N_ASYNC && !escaped && !self.nl_before {
                    if self.tok == T::KFunction {
                        self.next();
                        return self.parse_function_expression(start, true);
                    }
                    if can_be_arrow && self.tok == T::Name {
                        let param = self.tok_name;
                        let param_pos = self.tok_start;
                        if let Some(arrow) = self.skip_arrow_function(start, param_pos) {
                            return arrow;
                        }
                        if !(self.for_await_head && param == N_OF && !self.tok_escaped) {
                            self.next();
                            if self.tok != T::Arrow || self.nl_before {
                                self.unexpected();
                                return 0;
                            }
                            return self.parse_arrow_single(start, param, param_pos, true, no_in);
                        }
                    }
                }
                if self.tok == T::Arrow && can_be_arrow && !self.nl_before {
                    if let Some(arrow) = self.skip_arrow_function(start, start) {
                        return arrow;
                    }
                    return self.parse_arrow_single(start, name, start, false, no_in);
                }
                self.note_use(name);
                self.ast.push(tag::IDENT, start, name, 0)
            }
            T::Num | T::BigInt | T::Str => self.parse_literal(),
            T::KThis => {
                self.note_use(N_DOT_THIS);
                self.uses_this = true;
                self.uses_this_from_environment |= self.in_arrow;
                self.next();
                self.ast.push(tag::THIS, start, 0, 0)
            }
            T::LParen => self.parse_paren(no_in),
            T::LBracket => self.parse_array(),
            T::LBrace => self.parse_object(),
            T::KFunction => {
                self.next();
                self.parse_function_expression(start, false)
            }
            T::KTrue | T::KFalse | T::KNull => {
                let t = match self.tok {
                    T::KTrue => tag::TRUE,
                    T::KFalse => tag::FALSE,
                    _ => tag::NULL,
                };
                self.next();
                self.ast.push(t, start, 0, 0)
            }
            T::Slash | T::DivAssign => {
                self.rescan_regex();
                if self.tok != T::Regex {
                    return 0;
                }
                let (body_end, end) = (self.regex_body_end, self.tok_end);
                self.next();
                self.ast.push(tag::REGEX, start, body_end, end)
            }
            T::Backtick => {
                let subtree = self.begin_subtree();
                let template = self.parse_template(false);
                self.end_subtree(subtree, template)
            }
            T::KNew => self.parse_new(),
            T::KClass => self.parse_class(false),
            T::KSuper => self.parse_super(),
            T::KImport => self.parse_import_expression(for_new),
            _ => {
                self.expected("primary expression");
                0
            }
        }
    }

    fn parse_super(&mut self) -> u32 {
        let start = self.tok_start;
        if self.ctx & ctx::SUPER_PROPERTY == 0 {
            self.fail(start, "'super' keyword unexpected here");
            return 0;
        }
        self.next();
        if self.tok == T::LParen && self.ctx & ctx::SUPER_CALL == 0 {
            self.fail(start, "'super' keyword unexpected here");
            return 0;
        }
        if self.tok != T::Dot && self.tok != T::LBracket && self.tok != T::LParen {
            self.fail(start, "'super' keyword unexpected here");
            return 0;
        }
        self.note_use(N_DOT_THIS);
        self.note_use(N_DOT_SUPER);
        self.uses_this = true;
        self.uses_this_from_environment = true;
        self.ast.push(tag::SUPER, start, 0, 0)
    }

    fn parse_literal(&mut self) -> u32 {
        let start = self.tok_start;
        let t = match self.tok {
            T::Num => tag::NUM,
            T::BigInt => tag::BIGINT,
            _ => tag::STR,
        };
        if t != tag::BIGINT && self.tok_legacy && self.strict {
            self.fail(
                start,
                if t == tag::NUM {
                    "Unprefixed octal number not allowed in strict mode"
                } else {
                    "Octal escape sequence in string literal not allowed in strict mode"
                },
            );
            return 0;
        }
        let end = self.tok_end;
        self.next();
        // The AST places a string where the next token starts.
        self.ast.push(t, start, end, self.tok_start)
    }

    fn parse_import_expression(&mut self, for_new: bool) -> u32 {
        let start = self.tok_start;
        self.next();
        if self.tok == T::Dot {
            self.next();
            if !self.is_contextual(N_META) {
                self.fail(self.tok_start, "Expected 'meta' after 'import.'");
                return 0;
            }
            if !self.module {
                self.fail(start, "import.meta outside a module");
                return 0;
            }
            self.next();
            return self.ast.push(tag::IMPORT_META, start, 0, 0);
        }
        if self.tok != T::LParen || for_new {
            self.unexpected();
            return 0;
        }
        self.next();
        let specifier = self.parse_assign(false, false);
        let mut options = 0;
        if self.eat(T::Comma) && self.tok != T::RParen {
            options = self.parse_assign(false, false);
            self.eat(T::Comma);
        }
        self.expect(T::RParen);
        self.ast.push(tag::IMPORT_CALL, start, specifier, options)
    }

    fn parse_new(&mut self) -> u32 {
        let start = self.tok_start;
        self.next();
        if self.tok == T::Dot {
            self.next();
            if !self.is_contextual(N_TARGET) {
                self.fail(self.tok_start, "The only valid meta property for new is new.target");
                return 0;
            }
            if self.ctx & ctx::NEW_TARGET == 0 {
                self.fail(start, "'new.target' not allowed outside of a function");
                return 0;
            }
            self.note_use(N_DOT_NEW_TARGET);
            self.uses_this = true;
            self.uses_this_from_environment = true;
            self.next();
            return self.ast.push(tag::NEW_TARGET, start, 0, 0);
        }
        if !self.enter() {
            return 0;
        }
        let callee_start = self.tok_start;
        let atom = self.parse_atom(false, true);
        let callee = self.parse_subscripts(atom, callee_start, true, false);
        self.leave();
        let has_arguments = self.tok == T::LParen;
        let arguments = if has_arguments {
            self.parse_arguments(start)
        } else {
            self.finish_list(self.scratch.len())
        };
        let new_tag = if self.marks && has_arguments && self.arguments_have_spread {
            tag::NEW_SPREAD
        } else {
            tag::NEW
        };
        self.ast.push(new_tag, start, callee, arguments)
    }

    fn parse_template(&mut self, tagged: bool) -> u32 {
        let start = self.tok_start;
        let mark = self.scratch.len();
        loop {
            let chunk = self.pos;
            let (tail, invalid) = self.read_template_chunk();
            if invalid && !tagged {
                self.fail(chunk, "Invalid escape in template");
            }
            if self.err.is_some() {
                self.scratch.truncate(mark);
                return 0;
            }
            let chunk_end = self.pos - if tail { 1 } else { 2 };
            // An empty chunk has no token to take a position from.
            let shown_pos = if chunk_end == chunk { start } else { chunk_end };
            let quasi = self.ast.push(
                tag::TEMPLATE_CHUNK,
                shown_pos,
                chunk,
                invalid as u32 | (chunk_end - chunk) << 1,
            );
            self.scratch.push(quasi);
            if tail {
                self.tok_end = self.pos;
                self.next();
                break;
            }
            self.tok_end = self.pos;
            self.next();
            let expression = self.parse_expression(false, false);
            self.scratch.push(expression);
            if self.tok != T::RBrace {
                self.unexpected();
                self.scratch.truncate(mark);
                return 0;
            }
        }
        let list = self.finish_list(mark);
        self.ast.push(tag::TEMPLATE, start, list, 0)
    }

    fn parse_array(&mut self) -> u32 {
        let start = self.tok_start;
        let subtree = self.begin_subtree();
        self.next();
        let mark = self.scratch.len();
        let mut has_spread = false;
        while self.tok != T::RBracket && self.tok != T::Eof {
            let is_spread = self.tok == T::Ellipsis;
            if is_spread && !has_spread {
                self.mark_spread_start(self.scratch.len() - mark, start);
                has_spread = true;
            }
            let item = match self.tok {
                T::Comma => self.ast.push(tag::HOLE, self.tok_start, 0, 0),
                T::Ellipsis => {
                    let item_start = self.tok_start;
                    self.next();
                    let argument = self.parse_assign(false, true);
                    let trailing = (self.tok == T::Comma) as u32;
                    self.ast.push(tag::SPREAD, item_start, argument, trailing)
                }
                _ => self.parse_assign(false, true),
            };
            if has_spread && !is_spread {
                self.mark(tag::MARK_APPEND, 0);
            }
            self.scratch.push(item);
            if self.tok != T::RBracket {
                self.expect_comma_or(T::RBracket);
            }
        }
        self.expect(T::RBracket);
        let list = self.finish_list(mark);
        let array_tag = if self.marks && has_spread {
            tag::ARRAY_SPREAD
        } else {
            tag::ARRAY
        };
        self.ast
            .push(array_tag, start, list, if self.marks { subtree } else { 0 })
    }

    pub(crate) fn parse_property_name(&mut self) -> (u32, bool) {
        let start = self.tok_start;
        match self.tok {
            T::LBracket => {
                self.next();
                let key = self.parse_assign(false, false);
                self.expect(T::RBracket);
                self.mark(tag::MARK_COMPUTED_KEY, 0);
                (key, true)
            }
            T::Str | T::Num | T::BigInt => {
                let key = self.parse_literal();
                self.mark(tag::MARK_KEY, 0);
                (key, false)
            }
            kind if is_name_like(kind) => {
                let name = self.tok_name;
                self.next();
                let key = self.ast.push(tag::IDENT, start, name, 0);
                self.mark(tag::MARK_KEY, 0);
                (key, false)
            }
            _ => {
                self.unexpected();
                (0, false)
            }
        }
    }

    fn is_proto_key(&self, key: u32, key_kind: T, key_name: u32, key_end: u32) -> bool {
        match key_kind {
            T::Name => key_name == N_PROTO,
            T::Str => {
                // NB: The name of the property is the value of the string, which escape sequences spell out as well.
                let start = self.ast.pos[key as usize] as usize + 1;
                let units = self.source_view(start, key_end as usize - 1);
                super::literals::string_literal_text(units).view() == "__proto__"
            }
            _ => false,
        }
    }

    fn parse_object(&mut self) -> u32 {
        let start = self.tok_start;
        let object_mark = self.ast.tag.len();
        self.mark(tag::MARK_OBJECT, 0);
        self.next();
        let mark = self.scratch.len();
        let mut has_proto = false;
        while self.tok != T::RBrace && self.tok != T::Eof {
            let property = self.parse_property(start, &mut has_proto);
            self.scratch.push(property);
            if self.tok != T::RBrace {
                self.expect_comma_or(T::RBrace);
            }
        }
        self.expect(T::RBrace);
        if self.marks {
            // NB: A literal of nothing but plain properties can cache its shape.
            let is_simple = self.scratch.len() > mark
                && !has_proto
                && self.scratch[mark..].iter().all(|&property| {
                    let property = property as usize;
                    match self.ast.tag[property] {
                        tag::SHORTHAND => true,
                        tag::PROPERTY => {
                            let key = self.ast.a[property] as usize;
                            match self.ast.tag[key] {
                                tag::IDENT | tag::IDENT_KEY => true,
                                tag::STR | tag::STR_KEY => {
                                    let first = self.at(self.ast.pos[key] as usize + 1);
                                    !(0x30..=0x39).contains(&first) && first != 0x5C
                                }
                                _ => false,
                            }
                        }
                        _ => false,
                    }
                });
            self.ast.a[object_mark] = u32::from(is_simple);
        }
        let list = self.finish_list(mark);
        self.ast.push(
            tag::OBJECT,
            start,
            list,
            if self.marks { object_mark as u32 } else { 0 },
        )
    }

    fn parse_property(&mut self, object_start: u32, has_proto: &mut bool) -> u32 {
        let start = self.tok_start;
        if self.tok == T::Ellipsis {
            self.next();
            let argument = self.parse_assign(false, true);
            let trailing = (self.tok == T::Comma) as u32;
            if self.marks {
                return self.ast.push(tag::OBJECT_SPREAD, object_start, argument, trailing);
            }
            return self.ast.push(tag::SPREAD, start, argument, trailing);
        }
        let mut is_generator = self.eat(T::Star);
        let mut is_async = false;
        let mut key_kind = self.tok;
        let mut key_name = self.tok_name;
        let mut key_escaped = key_kind == T::Name && self.tok_escaped;
        let mut key_end = self.tok_end;
        let (mut key, mut computed) = self.parse_property_name();
        if !is_generator
            && !computed
            && key_kind == T::Name
            && key_name == N_ASYNC
            && !key_escaped
            && !self.nl_before
            && (is_name_like(self.tok) || matches!(self.tok, T::Num | T::BigInt | T::Str | T::LBracket | T::Star))
        {
            is_async = true;
            is_generator = self.eat(T::Star);
            key_kind = self.tok;
            key_name = self.tok_name;
            key_escaped = key_kind == T::Name && self.tok_escaped;
            key_end = self.tok_end;
            (key, computed) = self.parse_property_name();
        }
        if self.tok == T::Colon {
            if is_generator || is_async {
                self.unexpected();
                return 0;
            }
            self.next();
            let value = self.parse_assign(false, true);
            if !computed && self.is_proto_key(key, key_kind, key_name, key_end) {
                if *has_proto {
                    self.cover_error = start;
                }
                *has_proto = true;
            }
            let t = if computed {
                tag::COMPUTED_PROPERTY
            } else {
                tag::PROPERTY
            };
            return self.ast.push(t, start, key, value);
        }
        if self.tok == T::LParen {
            let function = self.parse_method(start, is_generator, is_async, false).0;
            let t = if computed { tag::COMPUTED_METHOD } else { tag::METHOD };
            return self.ast.push(t, start, key, function);
        }
        if !computed
            && key_kind == T::Name
            && (key_name == N_GET || key_name == N_SET)
            && !key_escaped
            && !matches!(self.tok, T::Comma | T::RBrace | T::Assign)
        {
            if is_generator || is_async {
                self.unexpected();
                return 0;
            }
            let (accessor_key, accessor_computed) = self.parse_property_name();
            let (function, count, has_rest) = self.parse_method(start, false, false, false);
            self.check_accessor_parameters(start, key_name == N_GET, count, has_rest);
            let t = match (key_name == N_GET, accessor_computed) {
                (true, false) => tag::GETTER,
                (true, true) => tag::COMPUTED_GETTER,
                (false, false) => tag::SETTER,
                (false, true) => tag::COMPUTED_SETTER,
            };
            return self.ast.push(t, start, accessor_key, function);
        }
        if computed || key_kind != T::Name || is_generator || is_async {
            self.unexpected();
            return 0;
        }
        self.check_identifier(key_name, start);
        self.note_use(key_name);
        let mut value = self.ast.push(tag::IDENT, start, key_name, 0);
        if self.tok == T::Assign {
            self.cover_error = self.tok_start;
            self.next();
            let default = self.parse_nested_default();
            value = self.ast.push(tag::ASSIGN_PATTERN, start, value, default);
        }
        self.ast.push(tag::SHORTHAND, start, key_name, value)
    }

    /// An initializer inside a cover grammar, whose own cover errors do not concern the pattern around it.
    fn parse_nested_default(&mut self) -> u32 {
        let saved = self.cover_error;
        let value = self.parse_assign(false, false);
        self.cover_error = saved;
        value
    }

    pub(crate) fn check_accessor_parameters(&mut self, pos: u32, getter: bool, count: u32, has_rest: bool) {
        if !self.is_reparsing() && (count != if getter { 0 } else { 1 } || has_rest) {
            self.fail(pos, "Wrong number of accessor parameters");
        }
    }

    // === Parenthesized expressions and arrow functions ===

    fn parse_paren(&mut self, no_in: bool) -> u32 {
        let start = self.tok_start;
        let is_parenthesized = self.parenthesized_expression_start == start;
        let can_be_arrow = self.potential_arrow_at == start;
        if can_be_arrow && let Some(arrow) = self.skip_arrow_function(start, start) {
            return arrow;
        }
        if !can_be_arrow {
            self.next();
            self.parenthesized_expression_start = self.tok_start;
            let inner = self.parse_expression(false, false);
            self.expect(T::RParen);
            return self.ast.push(tag::PAREN, start, inner, self.prev_start);
        }
        let marks = self.ast.marks();
        let (old_yield, old_await) = (self.yield_pos, self.await_pos);
        self.yield_pos = 0;
        self.await_pos = 0;
        let saved_cover_error = self.cover_error;
        self.cover_error = NONE;
        self.open_scope(0);
        self.next();
        let pos_after_paren = self.tok_start;
        self.parenthesized_expression_start = pos_after_paren;
        let mark = self.scratch.len();
        let mut last_is_comma = false;
        let mut spread_at = NONE;
        let mut first = true;
        while self.tok != T::RParen && self.tok != T::Eof {
            if !first {
                self.expect_comma_or(T::RParen);
                if self.tok == T::RParen {
                    last_is_comma = true;
                    break;
                }
            }
            first = false;
            if self.tok == T::Ellipsis {
                spread_at = self.tok_start;
                self.next();
                let target = self.parse_binding_atom(Bind::None);
                let rest = self.ast.push(tag::REST, spread_at, target, 0);
                self.scratch.push(rest);
                if self.tok == T::Comma {
                    self.fail(self.tok_start, "Comma is not permitted after the rest element");
                }
                break;
            }
            let item = self.parse_assign(false, true);
            self.scratch.push(item);
        }
        let empty = self.scratch.len() == mark;
        self.expect(T::RParen);
        if self.tok == T::Arrow && !self.nl_before {
            if self.yield_pos != 0 || self.await_pos != 0 {
                self.fail(start, "Yield or await expression in arrow function parameters");
            }
            self.yield_pos = old_yield;
            self.await_pos = old_await;
            self.cover_error = NONE;
            let list = self.finish_list(mark);
            self.parenthesized_expression_start = if is_parenthesized { start } else { NONE };
            return self.parse_arrow_from_cover(start, start, marks, list, false, no_in, saved_cover_error);
        }
        if empty || last_is_comma {
            self.unexpected();
            self.scratch.truncate(mark);
            return 0;
        }
        if spread_at != NONE {
            self.fail(spread_at, "Unexpected rest element");
            self.scratch.truncate(mark);
            return 0;
        }
        if self.cover_error != NONE {
            self.fail(
                self.cover_error,
                "Shorthand property initializers are only valid in patterns",
            );
        }
        self.cover_error = saved_cover_error;
        if old_yield != 0 {
            self.yield_pos = old_yield;
        }
        if old_await != 0 {
            self.await_pos = old_await;
        }
        self.close_scope();
        let inner = if self.scratch.len() - mark == 1 {
            self.scratch.pop().unwrap()
        } else {
            let first_item = pos_after_paren;
            let list = self.finish_list(mark);
            self.ast.push(tag::SEQUENCE, first_item, list, 0)
        };
        self.ast.push(tag::PAREN, start, inner, self.prev_start)
    }

    /// An arrow function on its own: its parameters are known to be parameters by now, so no cover grammar is needed.
    pub(crate) fn reparse_arrow_function(&mut self, start: u32, params_start: u32, flags: u32) -> u32 {
        let is_async = flags & function_flag::ASYNC != 0;
        let no_in = flags & function_flag::NO_IN != 0;
        if self.tok == T::Name {
            let (name, name_pos) = (self.tok_name, self.tok_start);
            self.next();
            return self.parse_arrow_single(start, name, name_pos, is_async, no_in);
        }
        let new_ctx = self.ctx;
        let own_flags = function_flag::ARROW | if is_async { function_flag::ASYNC } else { 0 };
        let mut state = self.begin_function(start, new_ctx, own_flags);
        self.scopes.last_mut().unwrap().flags |= scope_flag::ARROW;
        let (list, count, length, simple, _) = self.parse_parameter_list(&mut state);
        self.parse_arrow_body(&mut state, list, count, length, simple, params_start, no_in, start)
    }

    /// In a reparse: goes past the arrow function that the code at `params_start` turns out to be, if it is one.
    fn skip_arrow_function(&mut self, start: u32, params_start: u32) -> Option<u32> {
        let row = self.skippable_arrow_row(params_start)?;
        let state = self.begin_function(start, self.ctx, function_flag::ARROW);
        self.scopes.last_mut().unwrap().flags |= scope_flag::ARROW;
        let node = self.skip_function(state, row);
        self.last_arrow = node;
        Some(node)
    }

    fn arrow_ctx(&self, is_async: bool) -> u32 {
        let inherited = self.ctx & (ctx::SUPER_PROPERTY | ctx::SUPER_CALL | ctx::NEW_TARGET | ctx::NO_ARGUMENTS);
        inherited | ctx::RETURN | if is_async { ctx::ASYNC | ctx::CAN_AWAIT } else { 0 }
    }

    fn parse_arrow_single(&mut self, start: u32, name: u32, name_pos: u32, is_async: bool, no_in: bool) -> u32 {
        let new_ctx = self.arrow_ctx(is_async);
        let flags =
            function_flag::ARROW | function_flag::SIMPLE_PARAMETERS | if is_async { function_flag::ASYNC } else { 0 };
        let mut state = self.begin_function(start, new_ctx, flags);
        self.scopes.last_mut().unwrap().flags |= scope_flag::ARROW;
        self.check_binding_name(name, name_pos);
        self.declare(name, Bind::Parameter, name_pos);
        let param = self.ast.push(tag::IDENT, start, name, 0);
        self.scratch.push(param);
        let list = self.finish_list(self.scratch.len() - 1);
        self.parse_arrow_body(&mut state, list, 1, 1, true, name_pos, no_in, start)
    }

    #[allow(clippy::too_many_arguments)]
    fn parse_arrow_from_cover(
        &mut self,
        start: u32,
        params_start: u32,
        marks: AstMarks,
        list: u32,
        is_async: bool,
        no_in: bool,
        saved_cover_error: u32,
    ) -> u32 {
        let new_ctx = self.arrow_ctx(is_async);
        let flags = function_flag::ARROW | if is_async { function_flag::ASYNC } else { 0 };
        let mut state = self.begin_arrow_from_cover(start, marks, new_ctx, flags);
        state.cover_error = saved_cover_error;
        let count = self.ast.extra[list as usize];
        let mut simple = true;
        let mut length = 0;
        let mut counting = true;
        for i in 0..count {
            let item = self.ast.extra[(list + 1 + i) as usize];
            if self.ast.tag[item as usize] == tag::SPREAD {
                // `async(...rest) =>` was parsed as the arguments of a call.
                if i + 1 != count || self.ast.b[item as usize] != 0 {
                    self.fail(self.ast.pos[item as usize], "Rest parameter must be last");
                }
                let argument = self.ast.a[item as usize];
                if self.is_assignment(argument) {
                    self.fail(
                        self.ast.pos[argument as usize],
                        "Rest parameter may not have an initializer",
                    );
                }
                self.ast.tag[item as usize] = tag::REST;
            }
            self.convert_to_assignable(item, Bind::Parameter);
            let t = self.ast.tag[item as usize];
            simple &= t == tag::IDENT;
            counting &= t != tag::ASSIGN_PATTERN && t != tag::REST;
            if counting {
                length += 1;
            }
        }
        if simple {
            state.flags |= function_flag::SIMPLE_PARAMETERS;
        }
        if self.duplicate_parameter != NONE {
            self.fail(self.duplicate_parameter, "Duplicate parameter name");
        }
        // A parameter named `arguments` looked like a read of it until the `=>`.
        let declares_arguments = self.has_declaration_at_top_of_scope(N_ARGUMENTS);
        if declares_arguments {
            let scope = self.scopes.last_mut().unwrap();
            scope.arguments_access = scope.nested_arguments_access;
        }
        self.parse_arrow_body(&mut state, list, count, length, simple, params_start, no_in, start)
    }

    #[allow(clippy::too_many_arguments)]
    fn parse_arrow_body(
        &mut self,
        state: &mut FunctionState,
        list: u32,
        count: u32,
        length: u32,
        simple: bool,
        params_start: u32,
        no_in: bool,
        start: u32,
    ) -> u32 {
        self.next();
        state.body_start = self.tok_start;
        state.first_body_row = self.ast.tag.len() as u32;
        if no_in {
            state.flags |= function_flag::NO_IN;
        }
        let (body, is_expression) = if self.tok == T::LBrace {
            (self.parse_function_body(state, None, list, simple, true, false), false)
        } else {
            if !simple {
                self.open_function_body_scope(state);
            }
            (self.parse_assign(no_in, false), true)
        };
        let end = self.prev_end;
        let moved_state = core::mem::replace(state, self.empty_function_state());
        let node = self.end_function(
            moved_state,
            end,
            count,
            length,
            params_start,
            list,
            body,
            NONE,
            is_expression,
        );
        self.last_arrow = node;
        let _ = start;
        node
    }

    // === Patterns ===

    /// The target of an assignment, which unlike a target inside a pattern may also be a call in sloppy mode.
    pub(crate) fn convert_assignment_target(&mut self, node: u32) {
        let mut inner = node;
        while self.ast.tag[inner as usize] == tag::PAREN {
            inner = self.ast.a[inner as usize];
        }
        if self.ast.tag[inner as usize] == tag::CALL && !self.strict {
            return;
        }
        self.convert_to_assignable(node, Bind::None);
    }

    /// Rewrites an expression into the pattern it covers, and validates it. With a binding kind, this also declares the
    /// pattern's names.
    pub(crate) fn convert_to_assignable(&mut self, node: u32, bind: Bind) {
        let binding = bind != Bind::None;
        // An inner assignment already converted and checked its target, and a binding walks it only once.
        if !binding && matches!(self.ast.tag[node as usize], tag::OBJECT_PATTERN | tag::ARRAY_PATTERN) {
            return;
        }
        let pos = self.ast.pos[node as usize];
        match self.ast.tag[node as usize] {
            tag::IDENT => {
                let name = self.ast.a[node as usize];
                if binding {
                    self.check_binding_name(name, pos);
                    self.declare(name, bind, pos);
                } else if self.strict && (name == N_EVAL || name == N_ARGUMENTS) {
                    self.fail(
                        pos,
                        "Binding pattern target may not be called 'arguments' or 'eval' in strict mode",
                    );
                }
            }
            tag::MEMBER | tag::INDEX | tag::PRIVATE_MEMBER => {
                if binding {
                    self.fail(pos, "Binding member expression");
                }
            }
            tag::PAREN => {
                if binding {
                    self.fail(pos, "Binding parenthesized expression");
                    return;
                }
                let mut inner = self.ast.a[node as usize];
                while self.ast.tag[inner as usize] == tag::PAREN {
                    inner = self.ast.a[inner as usize];
                }
                match self.ast.tag[inner as usize] {
                    tag::IDENT | tag::MEMBER | tag::INDEX | tag::PRIVATE_MEMBER => {
                        self.convert_to_assignable(inner, bind);
                    }
                    // NB: This went through the first parse, and rows may have changed by now.
                    _ if self.marks => {}
                    _ => self.fail(pos, "Invalid left-hand side in assignment"),
                }
            }
            tag::OBJECT | tag::OBJECT_PATTERN => {
                if self.marks && self.ast.tag[node as usize] == tag::OBJECT {
                    let mark = self.ast.b[node as usize];
                    self.ast.tag[mark as usize] = tag::EMPTY;
                }
                self.ast.tag[node as usize] = tag::OBJECT_PATTERN;
                let list = self.ast.a[node as usize];
                let count = self.ast.extra[list as usize];
                for i in 0..count {
                    let property = self.ast.extra[(list + 1 + i) as usize];
                    match self.ast.tag[property as usize] {
                        tag::PROPERTY | tag::COMPUTED_PROPERTY | tag::SHORTHAND => {
                            self.convert_to_assignable(self.ast.b[property as usize], bind);
                        }
                        tag::SPREAD | tag::OBJECT_SPREAD | tag::REST => {
                            if i + 1 != count || self.ast.b[property as usize] != 0 {
                                self.fail(self.ast.pos[property as usize], "Rest element must be last");
                                return;
                            }
                            self.ast.tag[property as usize] = tag::REST;
                            let argument = self.ast.a[property as usize];
                            let t = self.ast.tag[argument as usize];
                            let valid = self.marks
                                || t == tag::IDENT
                                || (!binding
                                    && matches!(t, tag::MEMBER | tag::INDEX | tag::PRIVATE_MEMBER | tag::PAREN));
                            if !valid {
                                self.fail(self.ast.pos[argument as usize], "Invalid rest element");
                                return;
                            }
                            self.convert_to_assignable(argument, bind);
                        }
                        _ => {
                            self.fail(
                                self.ast.pos[property as usize],
                                "Invalid destructuring assignment target",
                            );
                            return;
                        }
                    }
                }
            }
            tag::ARRAY | tag::ARRAY_SPREAD | tag::ARRAY_PATTERN => {
                self.ast.tag[node as usize] = tag::ARRAY_PATTERN;
                let list = self.ast.a[node as usize];
                let count = self.ast.extra[list as usize];
                for i in 0..count {
                    let element = self.ast.extra[(list + 1 + i) as usize];
                    match self.ast.tag[element as usize] {
                        tag::HOLE => {}
                        tag::SPREAD | tag::REST => {
                            if i + 1 != count || self.ast.b[element as usize] != 0 {
                                self.fail(self.ast.pos[element as usize], "Rest element must be last");
                                return;
                            }
                            self.ast.tag[element as usize] = tag::REST;
                            let argument = self.ast.a[element as usize];
                            if self.is_assignment(argument) || self.ast.tag[argument as usize] == tag::ASSIGN_PATTERN {
                                self.fail(
                                    self.ast.pos[argument as usize],
                                    "Rest element may not have an initializer",
                                );
                                return;
                            }
                            self.convert_to_assignable(argument, bind);
                        }
                        _ => self.convert_to_assignable(element, bind),
                    }
                }
            }
            tag::REST => self.convert_to_assignable(self.ast.a[node as usize], bind),
            tag::ASSIGN_PATTERN => {
                self.convert_to_assignable(self.ast.a[node as usize], bind);
            }
            t if t == tag::ASSIGN_BASE => {
                self.ast.tag[node as usize] = tag::ASSIGN_PATTERN;
                self.convert_to_assignable(self.ast.a[node as usize], bind);
            }
            // NB: This went through the first parse, and rows may have changed by now.
            _ if self.marks => {}
            _ => self.fail(pos, "Invalid left-hand side in assignment"),
        }
    }

    #[inline(always)]
    fn is_assignment(&self, node: u32) -> bool {
        self.ast.tag[node as usize] == tag::ASSIGN_BASE
    }

    /// The target of an update or a compound assignment.
    fn check_simple_target(&mut self, node: u32, allow_call: bool, message: &'static str) {
        let mut inner = node;
        while self.ast.tag[inner as usize] == tag::PAREN {
            inner = self.ast.a[inner as usize];
        }
        let pos = self.ast.pos[inner as usize];
        match self.ast.tag[inner as usize] {
            tag::IDENT => {
                let name = self.ast.a[inner as usize];
                if self.strict && (name == N_EVAL || name == N_ARGUMENTS) {
                    self.fail(
                        pos,
                        "Binding pattern target may not be called 'arguments' or 'eval' in strict mode",
                    );
                }
            }
            tag::MEMBER | tag::INDEX | tag::PRIVATE_MEMBER => {}
            tag::CALL if !self.strict && allow_call => {}
            _ if self.marks => {}
            _ => self.fail(pos, message),
        }
    }
}
