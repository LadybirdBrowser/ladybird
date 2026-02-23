/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Declaration parsing: variables, functions, classes, imports, exports.

use std::cell::Cell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use crate::ast::*;
use crate::lexer::ch;
use crate::parser::{Associativity, DeclarationKind, ForbiddenTokens, FunctionKind, MethodKind, ParamInfo, ParsedParameters, Parser, Position, ProgramType, PropertyKey, PRECEDENCE_ASSIGNMENT};
use crate::token::TokenType;

fn expression_into_identifier(expression: Expression) -> Rc<Identifier> {
    match expression.inner {
        ExpressionKind::Identifier(id) => id,
        _ => unreachable!("expected Identifier expression"),
    }
}

/// Extract bound names from a declaration for export statements.
fn get_declaration_export_names(statement: &Statement) -> Vec<Utf16String> {
    match &statement.inner {
        StatementKind::VariableDeclaration { declarations, .. } => {
            let mut names = Vec::new();
            for declaration in declarations {
                collect_declarator_names(&declaration.target, &mut names);
            }
            names
        }
        StatementKind::UsingDeclaration { declarations } => {
            let mut names = Vec::new();
            for declaration in declarations {
                if let VariableDeclaratorTarget::Identifier(id) = &declaration.target {
                    names.push(id.name.clone());
                }
            }
            names
        }
        StatementKind::FunctionDeclaration { ref name, .. } => {
            if let Some(ref name) = name {
                vec![name.name.clone()]
            } else {
                Vec::new()
            }
        }
        StatementKind::ClassDeclaration(class) => {
            if let Some(ref name) = class.name {
                vec![name.name.clone()]
            } else {
                Vec::new()
            }
        }
        _ => Vec::new(),
    }
}

fn collect_declarator_names(target: &VariableDeclaratorTarget, names: &mut Vec<Utf16String>) {
    match target {
        VariableDeclaratorTarget::Identifier(id) => names.push(id.name.clone()),
        VariableDeclaratorTarget::BindingPattern(pat) => collect_pattern_names(pat, names),
    }
}

fn collect_pattern_names(pat: &BindingPattern, names: &mut Vec<Utf16String>) {
    for entry in &pat.entries {
        match &entry.alias {
            Some(BindingEntryAlias::Identifier(id)) => names.push(id.name.clone()),
            Some(BindingEntryAlias::BindingPattern(nested)) => collect_pattern_names(nested, names),
            _ => {}
        }
        if entry.alias.is_none() {
            if let Some(BindingEntryName::Identifier(id)) = &entry.name {
                names.push(id.name.clone());
            }
        }
    }
}

impl<'a> Parser<'a> {
    pub(crate) fn parse_declaration(&mut self) -> Statement {
        if self.match_token(TokenType::Async) {
            let next = self.next_token();
            if next.token_type == TokenType::Function && !next.trivia_has_line_terminator {
                return self.parse_function_declaration();
            }
        }

        match self.current_token_type() {
            TokenType::Function => self.parse_function_declaration(),
            TokenType::Class => self.parse_class_declaration(),
            TokenType::Let | TokenType::Const => self.parse_variable_declaration(false),
            TokenType::Identifier if self.token_value(&self.current_token) == utf16!("using") => {
                if !self.scope_collector.can_have_using_declaration() {
                    self.syntax_error("'using' not allowed outside of block, for loop or function");
                }
                self.parse_using_declaration(false)
            }
            _ => {
                self.expected("declaration");
                let start = self.position();
                self.consume();
                self.statement(start, StatementKind::Empty)
            }
        }
    }

    // https://tc39.es/ecma262/#sec-variable-statement
    // https://tc39.es/ecma262/#sec-let-and-const-declarations
    // VariableStatement : `var` VariableDeclarationList `;`
    // LexicalDeclaration : LetOrConst BindingList `;`
    // NB: `var` declarations are hoisted to the enclosing function/script scope,
    // while `let`/`const` are block-scoped (sec-declarations-and-the-variable-statement).
    pub(crate) fn parse_variable_declaration(&mut self, is_for_loop: bool) -> Statement {
        let start = self.position();
        let declaration_line = self.current_token().line_number;
        let declaration_column = self.current_token().line_column;

        let kind = match self.current_token_type() {
            TokenType::Var => DeclarationKind::Var,
            TokenType::Let => DeclarationKind::Let,
            TokenType::Const => DeclarationKind::Const,
            _ => {
                self.expected("variable declaration keyword");
                DeclarationKind::Var
            }
        };
        self.consume();

        let mut declarators: Vec<VariableDeclarator> = Vec::new();
        let mut any_init = false;

        loop {
            let declaration_start = self.position();

            let target = if self.match_identifier() {
                let token = self.consume();
                let value = self.token_value(&token).to_vec();
                self.check_identifier_name_for_assignment_validity(&value, false);
                if kind != DeclarationKind::Var && value == utf16!("let") {
                    self.syntax_error("Lexical binding may not be called 'let'");
                }
                let id = self.make_identifier(declaration_start, value.clone());

                if kind == DeclarationKind::Var {
                    self.scope_collector.add_var_declaration(
                        &[(&value, Some(id.clone()))],
                        declaration_line, declaration_column,
                        Some(DeclarationKind::Var),
                    );
                } else {
                    self.scope_collector.add_lexical_declaration(
                        &[&value as &[u16]],
                        declaration_line, declaration_column,
                    );
                    self.scope_collector.register_identifier(
                        id.clone(), &value, Some(kind),
                    );
                }

                VariableDeclaratorTarget::Identifier(id)
            } else if self.match_token(TokenType::CurlyOpen) || self.match_token(TokenType::BracketOpen) {
                let pat = self.parse_binding_pattern();
                let bound_names = std::mem::take(&mut self.pattern_bound_names);

                for (name, _) in &bound_names {
                    self.check_identifier_name_for_assignment_validity(name, false);
                    if kind != DeclarationKind::Var && name.as_slice() == utf16!("let") {
                        self.syntax_error("Lexical binding may not be called 'let'");
                    }
                }

                if kind != DeclarationKind::Var {
                    let mut seen: HashSet<&[u16]> = HashSet::new();
                    for (name, _) in &bound_names {
                        if !seen.insert(name.as_slice()) {
                            self.syntax_error("Duplicate parameter names in bindings");
                        }
                    }
                }

                // Register bound names with scope collector.
                if kind == DeclarationKind::Var {
                    let entries: Vec<(&[u16], Option<Rc<Identifier>>)> = bound_names.iter()
                        .map(|(n, id)| (n.as_slice(), Some(id.clone())))
                        .collect();
                    // NOTE: Binding pattern identifiers don't get declaration_kind,
                    // matching C++ behavior where only simple identifiers do.
                    self.scope_collector.add_var_declaration(&entries, declaration_line, declaration_column, None);
                } else {
                    let refs: Vec<&[u16]> = bound_names.iter().map(|(n, _)| n.as_slice()).collect();
                    self.scope_collector.add_lexical_declaration(&refs, declaration_line, declaration_column);
                    // Register each binding pattern identifier for scope analysis
                    // so they get is_local() annotations.
                    // NOTE: C++ does not pass declaration_kind for binding pattern identifiers,
                    // only for simple identifier declarations.
                    for (name, id) in &bound_names {
                        self.scope_collector.register_identifier(id.clone(), name, None);
                    }
                }

                VariableDeclaratorTarget::BindingPattern(pat)
            } else {
                self.expected("identifier or a binding pattern");
                self.consume();
                let id = self.make_identifier(declaration_start, Vec::new());
                VariableDeclaratorTarget::Identifier(id)
            };

            let init = if self.match_token(TokenType::Equals) {
                self.consume();
                any_init = true;
                let forbidden = if is_for_loop {
                    ForbiddenTokens::with_in()
                } else {
                    ForbiddenTokens::none()
                };
                Some(self.parse_expression(PRECEDENCE_ASSIGNMENT, Associativity::Right, forbidden))
            } else {
                None
            };

            declarators.push(VariableDeclarator {
                range: self.range_from(start),
                target,
                init,
            });

            if !self.match_token(TokenType::Comma) {
                break;
            }
            self.consume();
        }

        if !is_for_loop {
            self.consume_or_insert_semicolon();
        }

        if is_for_loop {
            self.for_loop_declaration_count = declarators.len();
            self.for_loop_declaration_has_init = any_init;
            self.for_loop_declaration_is_var = kind == DeclarationKind::Var;
        }

        self.statement(start, StatementKind::VariableDeclaration {
            kind,
            declarations: declarators,
        })
    }

    // https://tc39.es/proposal-explicit-resource-management/
    // UsingDeclaration : `using` BindingList `;`
    // NB: `using` declarations have lexical scoping like `const` and invoke
    // the Symbol.dispose method when the enclosing scope exits.
    pub(crate) fn parse_using_declaration(&mut self, is_for_loop: bool) -> Statement {
        let start = self.position();
        let declaration_line = self.current_token().line_number;
        let declaration_column = self.current_token().line_column;
        self.consume(); // consume 'using'

        let mut declarators: Vec<VariableDeclarator> = Vec::new();

        loop {
            let declaration_start = self.position();

            if !self.match_identifier() {
                self.expected("identifier");
                break;
            }
            let token = self.consume();
            let name = self.token_value(&token).to_vec();

            self.check_identifier_name_for_assignment_validity(&name, false);
            if name == utf16!("let") {
                self.syntax_error("Lexical binding may not be called 'let'");
            }

            let id = self.make_identifier(declaration_start, name.clone());

            self.scope_collector.add_lexical_declaration(&[&name as &[u16]], declaration_line, declaration_column);
            // C++ calls parse_lexical_binding() without declaration_kind for using,
            // so we pass None to match.
            self.scope_collector.register_identifier(id.clone(), &name, None);

            let init = if self.match_token(TokenType::Equals) {
                self.consume();
                if is_for_loop {
                    Some(self.parse_expression(PRECEDENCE_ASSIGNMENT, Associativity::Right, ForbiddenTokens::with_in()))
                } else {
                    Some(self.parse_assignment_expression())
                }
            } else if !is_for_loop {
                self.consume_token(TokenType::Equals);
                None
            } else {
                None
            };

            // C++ uses rule_start (using keyword position) for all VariableDeclarators.
            declarators.push(VariableDeclarator {
                range: self.range_from(start),
                target: VariableDeclaratorTarget::Identifier(id),
                init,
            });

            if self.match_token(TokenType::Comma) {
                self.consume();
                continue;
            }
            break;
        }

        if !is_for_loop {
            self.consume_or_insert_semicolon();
        }

        if is_for_loop {
            let any_init = declarators.iter().any(|d| d.init.is_some());
            self.for_loop_declaration_count = declarators.len();
            self.for_loop_declaration_has_init = any_init;
        }

        self.statement(start, StatementKind::UsingDeclaration {
            declarations: declarators,
        })
    }

    // https://tc39.es/ecma262/#sec-function-definitions
    // FunctionDeclaration : `function` BindingIdentifier `(` FormalParameters `)` `{` FunctionBody `}`
    //                     | [+Default] `function` `(` FormalParameters `)` `{` FunctionBody `}`
    // NB: The second form (without name) is only valid in `export default` context.
    pub(crate) fn parse_function_declaration(&mut self) -> Statement {
        let start = self.position();
        let declaration_line = self.current_token().line_number;
        let declaration_column = self.current_token().line_column;

        let saved_might_need_arguments = self.flags.function_might_need_arguments_object;
        self.flags.function_might_need_arguments_object = false;

        let is_async = self.eat(TokenType::Async);
        self.consume_token(TokenType::Function);
        let is_generator = self.eat(TokenType::Asterisk);
        let kind = FunctionKind::from_async_generator(is_async, is_generator);

        // Parse function name.
        let (name, fn_name) = if self.has_default_export_name && !self.match_identifier() {
            let default_name = Utf16String::from(utf16!("*default*"));
            self.last_function_name = default_name.clone();
            (Some(self.make_identifier(start, default_name.clone())), default_name)
        } else if self.match_identifier() {
            let token = self.consume();
            let value = Utf16String::from(self.token_value(&token));
            self.last_function_name = value.clone();
            (Some(self.make_identifier(start, value.clone())), value)
        } else {
            self.last_function_name.0.clear();
            (None, Utf16String::default())
        };
        self.last_function_kind = kind;

        // Register function declaration in parent scope (before opening function scope).
        self.scope_collector.add_function_declaration(
            &fn_name, name.clone(),
            kind, self.flags.strict_mode, declaration_line, declaration_column,
        );

        let fn_name_for_scope = if fn_name.is_empty() { None } else { Some(fn_name.as_slice()) };
        self.scope_collector.open_function_scope(fn_name_for_scope);
        self.scope_collector.set_is_function_declaration();

        let fd = self.parse_function_common(&name, &fn_name, kind, is_async, is_generator, start, saved_might_need_arguments);
        let decl_name = fd.name.clone();
        let decl_kind = fd.kind;
        let function_id = self.function_table.insert(fd);
        self.statement(start, StatementKind::FunctionDeclaration {
            function_id,
            name: decl_name,
            kind: decl_kind,
            is_hoisted: Cell::new(false),
        })
    }

    // https://tc39.es/ecma262/#sec-function-definitions
    // FunctionExpression : `function` BindingIdentifier? `(` FormalParameters `)` `{` FunctionBody `}`
    // NB: The function name, if present, is bound within the function's own scope
    // (not the enclosing scope), allowing recursive self-reference.
    pub(crate) fn parse_function_expression(&mut self) -> Expression {
        let start = self.position();

        let saved_might_need_arguments = self.flags.function_might_need_arguments_object;
        self.flags.function_might_need_arguments_object = false;

        let is_async = self.eat(TokenType::Async);
        self.consume_token(TokenType::Function);
        let is_generator = self.eat(TokenType::Asterisk);
        let kind = FunctionKind::from_async_generator(is_async, is_generator);

        let mut fn_name_value = Utf16String::default();
        let name = if self.match_identifier() {
            let token = self.consume();
            fn_name_value = Utf16String::from(self.token_value(&token));
            Some(self.make_identifier(start, fn_name_value.clone()))
        } else if self.match_token(TokenType::Yield) || self.match_token(TokenType::Await) {
            // C++ explicitly allows yield/await as function expression names
            // even inside generator/async contexts, then validates after.
            let token = self.consume();
            fn_name_value = Utf16String::from(self.token_value(&token));
            Some(self.make_identifier(start, fn_name_value.clone()))
        } else {
            None
        };

        // Register the function expression name in the outer scope, matching C++.
        // This must happen before open_function_scope so that the identifier group
        // exists with declaration_kind=None, preventing later var declarations
        // with the same name from setting a spurious declaration_kind.
        if let Some(ref id) = name {
            self.scope_collector.register_identifier(id.clone(), &fn_name_value, None);
        }

        // Open function scope (function expression name is bound within its own scope).
        let fn_name_for_scope = if fn_name_value.is_empty() { None } else { Some(fn_name_value.as_slice()) };
        self.scope_collector.open_function_scope(fn_name_for_scope);

        let fd = self.parse_function_common(&name, &fn_name_value, kind, is_async, is_generator, start, saved_might_need_arguments);
        let function_id = self.function_table.insert(fd);
        self.expression(start, ExpressionKind::Function(function_id))
    }

    /// Shared logic for parsing formal parameters, function body, and constructing
    /// FunctionData. Called after the function scope has been opened.
    #[allow(clippy::too_many_arguments)]
    fn parse_function_common(
        &mut self,
        name: &Option<Rc<Identifier>>,
        fn_name: &[u16],
        kind: FunctionKind,
        is_async: bool,
        is_generator: bool,
        start: Position,
        saved_might_need_arguments: bool,
    ) -> FunctionData {
        // Validate name against async generator and class static init restrictions.
        if name.is_some() {
            if kind == FunctionKind::AsyncGenerator
                && (fn_name == utf16!("await") || fn_name == utf16!("yield"))
            {
                let name_str = String::from_utf16_lossy(fn_name);
                self.syntax_error(&format!(
                    "async generator function is not allowed to be called '{}'",
                    name_str
                ));
            }
            if self.flags.in_class_static_init_block && fn_name == utf16!("await") {
                self.syntax_error("'await' is a reserved word");
            }
        }

        let in_generator_before = self.flags.in_generator_function_context;
        let await_before = self.flags.await_expression_is_valid;
        let saved_static_init = self.flags.in_class_static_init_block;
        let saved_field_init = self.flags.in_class_field_initializer;
        self.flags.in_generator_function_context = is_generator;
        self.flags.await_expression_is_valid = is_async;
        self.flags.in_class_static_init_block = false;
        self.flags.in_class_field_initializer = false;

        // Save pattern_bound_names so that destructuring patterns in the
        // function body don't steal names from an outer binding context.
        let saved_pattern_bound_names = std::mem::take(&mut self.pattern_bound_names);

        let parsed = self.parse_formal_parameters();
        self.register_function_parameters_with_scope(&parsed.parameters, &parsed.parameter_info);

        self.flags.in_generator_function_context = in_generator_before;
        self.flags.await_expression_is_valid = await_before;

        let (body, has_use_strict, mut insights) = self.parse_function_body(is_async, is_generator, parsed.is_simple);

        self.scope_collector.close_scope();
        self.pattern_bound_names = saved_pattern_bound_names;

        self.flags.in_class_static_init_block = saved_static_init;
        self.flags.in_class_field_initializer = saved_field_init;

        if name.is_some() {
            self.check_identifier_name_for_assignment_validity(fn_name, has_use_strict);
        }
        if has_use_strict || kind != FunctionKind::Normal {
            self.check_parameters_post_body(&parsed.parameter_info, has_use_strict, kind);
        }

        insights.might_need_arguments_object = self.flags.function_might_need_arguments_object;
        self.flags.function_might_need_arguments_object = saved_might_need_arguments;

        FunctionData {
            name: name.clone(),
            source_text_start: start.offset,
            source_text_end: self.source_text_end_offset(),
            body: Box::new(body),
            parameters: parsed.parameters,
            function_length: parsed.function_length,
            kind,
            is_strict_mode: self.flags.strict_mode || has_use_strict,
            is_arrow_function: false,
            parsing_insights: insights,
        }
    }

    // https://tc39.es/ecma262/#sec-class-definitions
    // ClassDeclaration : `class` BindingIdentifier ClassTail
    // ClassExpression  : `class` BindingIdentifier? ClassTail
    // ClassTail        : ClassHeritage? `{` ClassBody `}`
    // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
    // NB: All code within a ClassBody is in strict mode.
    pub(crate) fn parse_class_expression(&mut self, expect_name: bool) -> Expression {
        let start = self.position();

        let strict_before = self.flags.strict_mode;
        self.flags.strict_mode = true;

        self.consume_token(TokenType::Class);

        let (name_id, name_value) = if expect_name || self.match_identifier() {
            if self.match_identifier() {
                let token = self.consume();
                let value = Utf16String::from(self.token_value(&token));
                self.last_class_name = value.clone();
                (Some(self.make_identifier(start, value.clone())), value)
            } else if expect_name {
                self.expected("class name");
                self.last_class_name.0.clear();
                (None, Utf16String::default())
            } else {
                self.last_class_name.0.clear();
                (None, Utf16String::default())
            }
        } else {
            self.last_class_name.0.clear();
            (None, Utf16String::default())
        };

        let saved_class_name = self.last_class_name.clone();

        let class_name_for_scope = if name_value.is_empty() { None } else { Some(name_value.as_slice()) };
        self.scope_collector.open_class_declaration_scope(class_name_for_scope);

        if name_id.is_some() {
            self.check_identifier_name_for_assignment_validity(&name_value, true);
            if self.flags.in_class_static_init_block && name_value == utf16!("await") {
                self.syntax_error("Identifier must not be a reserved word in modules ('await')");
            }
        }

        let super_class = if self.match_token(TokenType::Extends) {
            self.consume();
            Some(Box::new(self.parse_expression_any()))
        } else {
            None
        };

        self.consume_token(TokenType::CurlyOpen);
        let mut elements: Vec<Node<ClassElement>> = Vec::new();
        let mut constructor: Option<Expression> = None;
        let mut found_private_names: HashMap<Utf16String, (Option<ClassMethodKind>, bool)> = HashMap::new();

        self.referenced_private_names_stack.push(HashSet::new());

        let saved_class_has_super = self.class_has_super_class;
        self.class_has_super_class = super_class.is_some();
        self.class_scope_depth += 1;

        while !self.match_token(TokenType::CurlyClose) && !self.done() {
            if self.match_token(TokenType::Semicolon) {
                self.consume();
                continue;
            }

            let (element, maybe_ctor) = self.parse_class_element(start, &mut found_private_names);
            if let Some(ctor) = maybe_ctor {
                // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
                // It is a Syntax Error if PrototypePropertyNameList of ClassElementList
                // contains more than one occurrence of "constructor".
                if constructor.is_some() {
                    self.syntax_error("Classes may not have more than one constructor");
                }
                constructor = Some(ctor);
            } else if let Some(element) = element {
                elements.push(element);
            }
        }

        self.consume_token(TokenType::CurlyClose);
        self.class_scope_depth -= 1;
        self.class_has_super_class = saved_class_has_super;

        // AllPrivateNamesValid: check that all referenced private names were declared.
        let referenced = self.referenced_private_names_stack.pop().unwrap_or_default();
        for name in referenced {
            if found_private_names.contains_key(&name) {
                continue;
            }
            // Bubble up to outer class, or error if no outer class.
            if let Some(outer) = self.referenced_private_names_stack.last_mut() {
                outer.insert(name);
            } else {
                let name_str = String::from_utf16_lossy(&name);
                self.syntax_error(&format!("Reference to undeclared private field or method '{}'", name_str));
            }
        }
        self.flags.strict_mode = strict_before;

        self.scope_collector.close_scope();

        if constructor.is_none() {
            constructor = Some(self.synthesize_default_constructor(
                start, &name_value, super_class.is_some(),
            ));
        }

        self.last_class_name = saved_class_name;

        self.expression(start, ExpressionKind::Class(Box::new(ClassData {
            name: name_id,
            source_text_start: start.offset,
            source_text_end: self.source_text_end_offset(),
            constructor: constructor.map(Box::new),
            super_class,
            elements,
        })))
    }

    pub(crate) fn parse_class_declaration(&mut self) -> Statement {
        let start = self.position();
        let class_expression = self.parse_class_expression(true);
        // Convert the class expression into a class declaration by extracting ClassData.
        match class_expression.inner {
            ExpressionKind::Class(data) => {
                // Register class name as lexical declaration in the outer scope.
                // The inner class scope (opened/closed inside parse_class_expression)
                // binds the name for self-reference. The outer scope needs the name
                // registered as a lexical declaration so it's visible to sibling code.
                if let Some(ref name_ident) = data.name {
                    self.scope_collector.add_lexical_declaration(
                        &[&name_ident.name as &[u16]],
                        start.line, start.column,
                    );
                    self.scope_collector.register_identifier(
                        name_ident.clone(),
                        &name_ident.name,
                        None,
                    );
                }
                self.statement(start, StatementKind::ClassDeclaration(data))
            }
            _ => unreachable!("parse_class_expression must return ExpressionKind::Class"),
        }
    }

    // https://tc39.es/ecma262/#sec-runtime-semantics-classdefinitionevaluation
    // If no constructor is present in the ClassBody:
    //   - Base class: constructor() {}
    //   - Derived class: constructor(...arguments) { super(...arguments); }
    fn synthesize_default_constructor(&mut self, start: Position, class_name: &[u16], has_super: bool) -> Expression {
        let ctor_name = if !class_name.is_empty() {
            Some(self.make_identifier(start, Utf16String::from(class_name)))
        } else {
            None
        };

        // Note: No scope collector calls here. The synthesized constructor AST
        // is stored in the SFD and compiled lazily — scope analysis runs at that point.

        if has_super {
            let arguments_name = Utf16String::from(utf16!("args"));

            let arguments_ref = Rc::new(Identifier::new(self.range_from(start), arguments_name.clone()));
            let arguments_expression = self.expression(start, ExpressionKind::Identifier(arguments_ref));

            let super_call = self.expression(start, ExpressionKind::SuperCall(SuperCallData {
                arguments: vec![CallArgument { value: arguments_expression, is_spread: true }],
                is_synthetic: true,
            }));
            let return_statement = self.statement(start, StatementKind::Return(Some(Box::new(super_call))));
            let body = self.statement(start, StatementKind::Block(
                ScopeData::shared_with_children(vec![return_statement]),
            ));

            let arguments_binding = Rc::new(Identifier::new(self.range_from(start), arguments_name));
            let parameters = vec![FunctionParameter {
                binding: FunctionParameterBinding::Identifier(arguments_binding),
                default_value: None,
                is_rest: true,
            }];

            let function_id = self.function_table.insert(FunctionData {
                name: ctor_name,
                source_text_start: start.offset,
                source_text_end: self.source_text_end_offset(),
                body: Box::new(body),
                parameters,
                function_length: 0,
                kind: FunctionKind::Normal,
                is_strict_mode: true,
                is_arrow_function: false,
                parsing_insights: FunctionParsingInsights {
                    uses_this: true,
                    uses_this_from_environment: true,
                    ..FunctionParsingInsights::default()
                },
            });
            self.expression(start, ExpressionKind::Function(function_id))
        } else {
            let body = self.statement(start, StatementKind::Block(
                ScopeData::shared_with_children(Vec::new()),
            ));

            let function_id = self.function_table.insert(FunctionData {
                name: ctor_name,
                source_text_start: start.offset,
                source_text_end: self.source_text_end_offset(),
                body: Box::new(body),
                parameters: Vec::new(),
                function_length: 0,
                kind: FunctionKind::Normal,
                is_strict_mode: true,
                is_arrow_function: false,
                parsing_insights: FunctionParsingInsights {
                    uses_this: true,
                    uses_this_from_environment: true,
                    ..FunctionParsingInsights::default()
                },
            });
            self.expression(start, ExpressionKind::Function(function_id))
        }
    }

    // https://tc39.es/ecma262/#sec-class-definitions
    // ClassElement : MethodDefinition
    //             | `static` MethodDefinition
    //             | FieldDefinition `;`
    //             | `static` FieldDefinition `;`
    //             | ClassStaticBlock
    //             | `;`
    fn parse_class_element(
        &mut self,
        class_start: Position,
        found_private_names: &mut HashMap<Utf16String, (Option<ClassMethodKind>, bool)>,
    ) -> (Option<Node<ClassElement>>, Option<Expression>) {
        // C++ lexes "static" as Identifier and checks original_value() == "static".
        let mut is_static = if self.match_identifier()
            && self.token_original_value(&self.current_token) == utf16!("static") {
            self.consume();
            // https://tc39.es/ecma262/#sec-class-static-initialization-blocks
            // ClassStaticBlock : `static` `{` ClassStaticBlockBody `}`
            if self.match_token(TokenType::CurlyOpen) {
                // C++ captures static_start (push_start) before consuming '{'.
                let static_start = self.position();
                self.consume(); // consume '{'
                let saved_flags = self.flags;
                self.flags.in_break_context = false;
                self.flags.in_continue_context = false;
                self.flags.in_function_context = false;
                self.flags.in_generator_function_context = false;
                self.flags.await_expression_is_valid = false;
                self.flags.in_class_field_initializer = true;
                self.flags.in_class_static_init_block = true;
                self.flags.allow_super_property_lookup = true;
                self.scope_collector.open_static_init_scope(None);
                let children = self.parse_statement_list(false);
                self.flags = saved_flags;
                self.consume_token(TokenType::CurlyClose);
                let scope = ScopeData::shared_with_children(children);
                self.scope_collector.set_scope_node(scope.clone());
                self.scope_collector.close_scope();
                // C++ uses rule_start (class start) for FunctionBody position.
                let body = self.statement(class_start, StatementKind::FunctionBody {
                    scope,
                    in_strict_mode: self.flags.strict_mode,
                });
                // C++ uses static_start (after '{') for StaticInitializer position.
                return (Some(Node::new(self.range_from(static_start), ClassElement::StaticInitializer {
                    body: Box::new(body),
                })), None);
            }
            true
        } else {
            false
        };

        let mut is_async = false;
        let mut is_generator = false;
        let mut is_getter = false;
        let mut is_setter = false;
        let function_start = self.position();

        // Check modifiers (must not contain escape sequences).
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
                    && next.token_type != TokenType::Semicolon
                    && next.token_type != TokenType::Equals
                {
                    is_async = true;
                    self.consume();
                }
            }
        }

        if self.match_token(TokenType::Asterisk) {
            is_generator = true;
            self.consume();
        }

        // If we consumed a modifier keyword (static/async/get/set) but the next token
        // is one that can't start a property key (`;`, `=`, `(`, `}`), the keyword was
        // actually the field/method name, not a modifier.
        let PropertyKey { expression: key, name: key_value, .. } = if (is_static || is_async || is_getter || is_setter)
            && (self.match_token(TokenType::Semicolon)
                || self.match_token(TokenType::Equals)
                || self.match_token(TokenType::ParenOpen)
                || self.match_token(TokenType::CurlyClose))
        {
            let name: &[u16] = if is_async {
                is_async = false;
                utf16!("async")
            } else if is_getter {
                is_getter = false;
                utf16!("get")
            } else if is_setter {
                is_setter = false;
                utf16!("set")
            } else {
                is_static = false;
                utf16!("static")
            };
            let expression = self.expression(class_start, ExpressionKind::StringLiteral(Utf16String(name.to_vec())));
            PropertyKey {
                expression,
                name: Some(Utf16String::from(name)),
                is_proto: false,
                is_computed: false,
                is_identifier: false,
            }
        } else {
            // C++ only uses class start position for Identifier and PrivateIdentifier
            // tokens (handled directly in the switch). Keywords like `return` go through
            // parse_property_key which uses its own position.
            let key_override = if self.current_token.token_type == TokenType::Identifier
                || self.current_token.token_type == TokenType::PrivateIdentifier
            {
                Some(class_start)
            } else {
                None
            };
            self.parse_property_key(key_override)
        };

        // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
        // It is a Syntax Error if PropName of ClassElement is "prototype"
        // and ClassElement is `static` MethodDefinition or `static` FieldDefinition.
        if is_static && key_value.as_deref() == Some(utf16!("prototype")) {
            self.syntax_error("Classes may not have a static property named 'prototype'");
        }

        // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
        // It is a Syntax Error if PrivateBoundIdentifiers of ClassElementList contains
        // any duplicate entries, unless the name is used once for a getter and once for
        // a setter and in no other entries, and they are either both static or both non-static.
        let is_private = key_value.as_ref().is_some_and(|v| v.first() == Some(&ch(b'#')));
        if is_private {
            let name = key_value.as_ref().unwrap();
            let current_kind = if is_getter { Some(ClassMethodKind::Getter) } else if is_setter { Some(ClassMethodKind::Setter) } else { None };
            let is_accessor = is_getter || is_setter;
            if is_accessor {
                // Getter or setter: check against existing private names
                if let Some(&(existing_kind, existing_static)) = found_private_names.get(name) {
                    let is_error = match existing_kind {
                        // Existing is not a method (field/plain method) → error
                        None => true,
                        // Existing is a getter/setter
                        Some(ek) => {
                            // Different staticness → error
                            existing_static != is_static
                            // Same kind (getter+getter or setter+setter) → error
                            // Plain method → error
                            || ek == ClassMethodKind::Method
                            || ek == current_kind.unwrap()
                        }
                    };
                    if is_error {
                        let name_str = String::from_utf16_lossy(name);
                        self.syntax_error(&format!("Duplicate private field or method named '{}'", name_str));
                    }
                }
                found_private_names.insert(name.clone(), (current_kind, is_static));
            } else if found_private_names.insert(name.clone(), (current_kind, is_static)).is_some() {
                let name_str = String::from_utf16_lossy(name);
                self.syntax_error(&format!("Duplicate private field or method named '{}'", name_str));
            }
        }

        if self.match_token(TokenType::ParenOpen) {
            let ctor_name = utf16!("constructor");
            let is_constructor = !is_static
                && !is_getter && !is_setter
                && key_value.as_deref() == Some(ctor_name);

            // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
            // It is a Syntax Error if SpecialMethod of MethodDefinition is true
            // and PropName of MethodDefinition is "constructor".
            if is_constructor {
                if is_getter || is_setter {
                    self.syntax_error("Class constructor may not be an accessor");
                }
                if is_generator {
                    self.syntax_error("Class constructor may not be a generator");
                }
                if is_async {
                    self.syntax_error("Class constructor may not be async");
                }
            }

            let method_kind = if is_constructor { MethodKind::Constructor } else if is_getter { MethodKind::Getter } else if is_setter { MethodKind::Setter } else { MethodKind::Normal };
            let function = self.parse_method_definition(is_async, is_generator, method_kind, function_start);
            let class_method_kind = if is_getter {
                ClassMethodKind::Getter
            } else if is_setter {
                ClassMethodKind::Setter
            } else {
                ClassMethodKind::Method
            };

            if is_constructor {
                return (None, Some(function));
            }

            return (Some(Node::new(self.range_from(class_start), ClassElement::Method {
                key: Box::new(key),
                function: Box::new(function),
                kind: class_method_kind,
                is_static,
            })), None);
        }

        // https://tc39.es/ecma262/#sec-class-definitions-static-semantics-early-errors
        // It is a Syntax Error if PropName of ClassElement is "constructor"
        // and ClassElement is FieldDefinition.
        if key_value.as_deref() == Some(utf16!("constructor")) {
            self.syntax_error("Class cannot have field named 'constructor'");
        }

        let init = if self.match_token(TokenType::Equals) {
            self.consume();
            let saved_field_init = self.flags.in_class_field_initializer;
            let saved_super_lookup = self.flags.allow_super_property_lookup;
            self.flags.in_class_field_initializer = true;
            self.flags.allow_super_property_lookup = true;
            self.scope_collector.open_class_field_scope(None);
            let expression = self.parse_assignment_expression();
            self.scope_collector.close_scope();
            self.flags.in_class_field_initializer = saved_field_init;
            self.flags.allow_super_property_lookup = saved_super_lookup;
            Some(Box::new(expression))
        } else {
            None
        };

        self.consume_or_insert_semicolon();
        (Some(Node::new(self.range_from(class_start), ClassElement::Field {
            key: Box::new(key),
            initializer: init,
            is_static,
        })), None)
    }

    // https://tc39.es/ecma262/#sec-function-definitions-static-semantics-early-errors
    // It is a Syntax Error if FunctionBodyContainsUseStrict of FunctionBody is true
    // and IsSimpleParameterList of FormalParameters is false.
    pub(crate) fn parse_function_body(&mut self, is_async: bool, is_generator: bool, is_simple: bool) -> (Statement, bool, FunctionParsingInsights) {
        self.consume_token(TokenType::CurlyOpen);
        // C++ captures FunctionBody position AFTER consuming `{`.
        let start = self.position();

        let in_function_before = self.flags.in_function_context;
        let in_generator_before = self.flags.in_generator_function_context;
        let await_before = self.flags.await_expression_is_valid;
        let formal_parameter_before = self.flags.in_formal_parameter_context;
        let old_labels = std::mem::take(&mut self.labels_in_scope);
        self.flags.in_function_context = true;
        self.flags.in_generator_function_context = is_generator;
        self.flags.await_expression_is_valid = is_async;
        self.flags.in_formal_parameter_context = false;

        let (has_use_strict, mut children) = self.parse_directive();
        let body_is_strict = has_use_strict || self.flags.strict_mode;

        let strict_before = self.flags.strict_mode;
        if has_use_strict {
            self.flags.strict_mode = true;
            if !is_simple {
                self.syntax_error("Illegal 'use strict' directive in function with non-simple parameter list");
            }
        }

        children.extend(self.parse_statement_list(false));

        self.flags.strict_mode = strict_before;
        self.flags.in_function_context = in_function_before;
        self.flags.in_generator_function_context = in_generator_before;
        self.flags.await_expression_is_valid = await_before;
        self.flags.in_formal_parameter_context = formal_parameter_before;
        self.labels_in_scope = old_labels;

        // Read scope analysis flags before the function scope is closed.
        let insights = FunctionParsingInsights {
            contains_direct_call_to_eval: self.scope_collector.contains_direct_call_to_eval(),
            uses_this: self.scope_collector.uses_this(),
            uses_this_from_environment: self.scope_collector.uses_this_from_environment(),
            ..FunctionParsingInsights::default()
        };

        self.consume_token(TokenType::CurlyClose);

        let scope = ScopeData::shared_with_children(children);
        self.scope_collector.set_scope_node(scope.clone());

        let body = self.statement(start, StatementKind::FunctionBody {
            scope,
            in_strict_mode: body_is_strict,
        });

        (body, has_use_strict, insights)
    }

    // https://tc39.es/ecma262/#sec-function-definitions
    // FormalParameters : [empty]
    //                  | FunctionRestParameter
    //                  | FormalParameterList
    //                  | FormalParameterList `,`
    //                  | FormalParameterList `,` FunctionRestParameter
    pub(crate) fn parse_formal_parameters(&mut self) -> ParsedParameters {
        self.consume_token(TokenType::ParenOpen);
        let result = self.parse_formal_parameters_impl(false);
        self.consume_token(TokenType::ParenClose);
        result
    }

    pub(crate) fn parse_formal_parameters_impl(&mut self, is_arrow: bool) -> ParsedParameters {
        let saved_formal_parameter_ctx = self.flags.in_formal_parameter_context;
        self.flags.in_formal_parameter_context = true;

        // Save and clear pattern_bound_names so that nested function parsing
        // (e.g. arrow functions in default values) doesn't steal binding names
        // accumulated by an outer binding pattern context.
        let saved_pattern_bound_names = std::mem::take(&mut self.pattern_bound_names);

        if self.match_token(TokenType::ParenClose) {
            self.flags.in_formal_parameter_context = saved_formal_parameter_ctx;
            self.pattern_bound_names = saved_pattern_bound_names;
            return ParsedParameters {
                parameters: Vec::new(),
                function_length: 0,
                parameter_info: Vec::new(),
                is_simple: true,
            };
        }

        let mut parameters: Vec<FunctionParameter> = Vec::new();
        let mut function_length: i32 = 0;
        let mut has_seen_default = false;
        let mut has_seen_rest = false;
        let mut parameter_info: Vec<ParamInfo> = Vec::new();
        let mut seen_parameter_names: HashSet<Utf16String> = HashSet::new();

        // C++ uses the position at the start of parse_formal_parameters for all
        // parameter identifiers (i.e., the position of the first parameter).
        let formal_parameters_start = self.position();

        loop {
            let parameter_start = self.position();
            let rest = self.eat(TokenType::TripleDot);
            if rest {
                has_seen_rest = true;
            }

            let (binding, _is_pat) = if self.match_identifier()
                || self.match_token(TokenType::Await)
                || self.match_token(TokenType::Yield)
            {
                // Emit errors for await/yield used as parameter names in
                // contexts where they are reserved.
                if self.current_token_type() == TokenType::Await
                    && (self.program_type == ProgramType::Module
                        || self.flags.await_expression_is_valid
                        || self.flags.in_class_static_init_block)
                {
                    self.syntax_error("'await' is not allowed as an identifier in this context");
                }
                if self.current_token_type() == TokenType::Yield
                    && (self.flags.strict_mode || self.flags.in_generator_function_context)
                {
                    self.syntax_error("'yield' is not allowed as an identifier in this context");
                }
                let token = self.consume();
                let value = Utf16String::from(self.token_value(&token));
                self.check_identifier_name_for_assignment_validity(&value, false);
                // https://tc39.es/ecma262/#sec-function-definitions-static-semantics-early-errors
                // It is a Syntax Error if IsSimpleParameterList is false and
                // BoundNames of FormalParameters contains any duplicate elements.
                // In strict mode, duplicates are always an error.
                // Arrow functions check duplicates post-confirmation (after =>).
                // Inline duplicate checks would cause speculative arrow parsing
                // to bail out, so skip them when is_arrow is true.
                if !is_arrow && seen_parameter_names.contains(value.as_slice()) {
                    if self.flags.strict_mode {
                        let name_str = String::from_utf16_lossy(&value);
                        self.syntax_error(&format!("Duplicate parameter '{}' not allowed in strict mode", name_str));
                    } else if has_seen_default {
                        let name_str = String::from_utf16_lossy(&value);
                        self.syntax_error(&format!("Duplicate parameter '{}' not allowed in function with default parameter", name_str));
                    } else if has_seen_rest {
                        let name_str = String::from_utf16_lossy(&value);
                        self.syntax_error(&format!("Duplicate parameter '{}' not allowed in function with rest parameter", name_str));
                    }
                }
                seen_parameter_names.insert(value.clone());
                let id = Rc::new(Identifier::new(self.range_from(formal_parameters_start), value.clone()));
                parameter_info.push(ParamInfo { name: value, is_rest: rest, is_from_pattern: false, identifier: Some(id.clone()) });
                (FunctionParameterBinding::Identifier(id), false)
            } else if self.match_token(TokenType::CurlyOpen) || self.match_token(TokenType::BracketOpen) {
                let pat = self.parse_binding_pattern();
                for (n, id) in std::mem::take(&mut self.pattern_bound_names) {
                    seen_parameter_names.insert(n.clone());
                    parameter_info.push(ParamInfo { name: n, is_rest: rest, is_from_pattern: true, identifier: Some(id) });
                }
                (FunctionParameterBinding::BindingPattern(pat), true)
            } else {
                self.expected("parameter name");
                self.consume();
                let id = Rc::new(Identifier::new(self.range_from(parameter_start), Utf16String::default()));
                (FunctionParameterBinding::Identifier(id), false)
            };

            let default_value = if !rest && self.match_token(TokenType::Equals) {
                self.consume();
                has_seen_default = true;
                let saved_in_function = self.flags.in_function_context;
                self.flags.in_function_context = true;
                let expr = self.parse_expression(PRECEDENCE_ASSIGNMENT, Associativity::Right, ForbiddenTokens::with_in());
                self.flags.in_function_context = saved_in_function;
                Some(expr)
            } else {
                None
            };

            if !rest && !has_seen_default && default_value.is_none() {
                function_length += 1;
            }

            parameters.push(FunctionParameter {
                binding,
                default_value,
                is_rest: rest,
            });

            if rest || !self.match_token(TokenType::Comma) {
                break;
            }
            self.consume();

            if self.match_token(TokenType::ParenClose) {
                break;
            }
        }

        self.flags.in_formal_parameter_context = saved_formal_parameter_ctx;
        self.pattern_bound_names = saved_pattern_bound_names;

        let is_simple = !has_seen_default && !has_seen_rest && !parameters.iter().any(|p| matches!(&p.binding, FunctionParameterBinding::BindingPattern(_)));
        ParsedParameters { parameters, function_length, parameter_info, is_simple }
    }

    // https://tc39.es/ecma262/#sec-destructuring-binding-patterns
    // BindingPattern : ObjectBindingPattern | ArrayBindingPattern
    // ObjectBindingPattern : `{` `}`
    //                      | `{` BindingRestProperty `}`
    //                      | `{` BindingPropertyList `}`
    //                      | `{` BindingPropertyList `,` BindingRestProperty? `}`
    // ArrayBindingPattern  : `[` Elision? BindingRestElement? `]`
    //                      | `[` BindingElementList `]`
    //                      | `[` BindingElementList `,` Elision? BindingRestElement? `]`
    pub(crate) fn parse_binding_pattern(&mut self) -> BindingPattern {
        let is_object = self.match_token(TokenType::CurlyOpen);
        let is_array = self.match_token(TokenType::BracketOpen);
        if !is_object && !is_array {
            return BindingPattern { kind: BindingPatternKind::Object, entries: Vec::new() };
        }
        // Save the position before consuming '[' or '{'. C++ uses
        // rule_start.position() (from push_start()) for all identifiers inside
        // the binding pattern. Each recursive call gets its own push_start(),
        // so nested patterns use the inner pattern's start position.
        let outer_pattern_start = self.binding_pattern_start;
        self.binding_pattern_start = Some(self.position());
        self.consume();

        let kind = if is_object { BindingPatternKind::Object } else { BindingPatternKind::Array };
        let closing_token = if is_object { TokenType::CurlyClose } else { TokenType::BracketClose };
        let mut entries: Vec<BindingEntry> = Vec::new();

        while !self.match_token(closing_token) && !self.done() {
            // Array elision: bare comma.
            if !is_object && self.match_token(TokenType::Comma) {
                self.consume();
                entries.push(BindingEntry {
                    name: None,
                    alias: None,
                    initializer: None,
                    is_rest: false,
                });
                continue;
            }

            let is_rest = self.eat(TokenType::TripleDot);

            let mut entry_name = None;
            let mut entry_alias = None;

            if is_object {
                if self.allow_member_expressions && is_rest {
                    // Destructuring assignment: rest target can be MemberExpression or Identifier.
                    let expression = self.parse_expression(PRECEDENCE_ASSIGNMENT, Associativity::Right, ForbiddenTokens::none().forbid(&[TokenType::Equals]));
                    if Self::is_member_expression(&expression) {
                        entry_alias = Some(BindingEntryAlias::MemberExpression(Box::new(expression)));
                    } else if Self::is_identifier(&expression) {
                        entry_name = Some(BindingEntryName::Identifier(expression_into_identifier(expression)));
                    } else {
                        self.syntax_error("Invalid destructuring assignment target");
                        break;
                    }
                } else {
                    let mut needs_alias = false;
                    let mut entry_name_value = Utf16String::new();
                    let mut entry_is_keyword = false;

                    if self.match_identifier_name() || self.match_token(TokenType::StringLiteral) || self.match_token(TokenType::NumericLiteral) || self.match_token(TokenType::BigIntLiteral) {
                        // C++ uses the binding pattern start position for all name identifiers.
                        let entry_start = self.binding_pattern_start.unwrap_or_else(|| self.position());

                        if self.match_token(TokenType::StringLiteral) || self.match_token(TokenType::NumericLiteral) {
                            needs_alias = true;
                        }

                        entry_is_keyword = self.current_token.token_type.is_identifier_name()
                            && !self.match_identifier();

                        // Suppress eval/arguments check for binding pattern property
                        // keys. C++ uses regular consume() here (no arguments check),
                        // not consume_and_allow_division().
                        let saved_prop_key_ctx = self.flags.in_property_key_context;
                        self.flags.in_property_key_context = true;

                        if self.match_token(TokenType::StringLiteral) {
                            let token = self.consume();
                            let (value, _has_octal) = self.parse_string_value(&token);
                            let id = self.make_identifier(entry_start, value);
                            self.scope_collector.register_identifier(id.clone(), &id.name, None);
                            entry_name = Some(BindingEntryName::Identifier(id));
                        } else if self.match_token(TokenType::BigIntLiteral) {
                            let token = self.consume();
                            let value = self.token_value(&token);
                            let name_value = if value.last() == Some(&ch(b'n')) {
                                value[..value.len() - 1].to_vec()
                            } else {
                                value.to_vec()
                            };
                            let id = self.make_identifier(entry_start, name_value);
                            self.scope_collector.register_identifier(id.clone(), &id.name, None);
                            entry_name = Some(BindingEntryName::Identifier(id));
                        } else {
                            let token = self.consume();
                            let value = self.token_value(&token).to_vec();
                            entry_name_value = value.clone().into();
                            let id = self.make_identifier(entry_start, value);
                            // C++ calls parse_identifier() for binding pattern property
                            // keys, which registers them. Do the same here.
                            self.scope_collector.register_identifier(id.clone(), &id.name, None);
                            entry_name = Some(BindingEntryName::Identifier(id));
                        }

                        self.flags.in_property_key_context = saved_prop_key_ctx;
                    } else if self.match_token(TokenType::BracketOpen) {
                        self.consume();
                        let expression = self.parse_expression_any();
                        entry_name = Some(BindingEntryName::Expression(Box::new(expression)));
                        self.consume_token(TokenType::BracketClose);
                    } else {
                        self.expected("identifier or computed property name");
                        break;
                    }

                    if !is_rest && self.match_token(TokenType::Colon) {
                        self.consume();
                        if self.allow_member_expressions {
                            let expression_start = self.position();
                            let expression = self.parse_expression(PRECEDENCE_ASSIGNMENT, Associativity::Right, ForbiddenTokens::none().forbid(&[TokenType::Equals]));
                            if Self::is_object_expression(&expression) || Self::is_array_expression(&expression) {
                                if let Some(pattern) = self.synthesize_binding_pattern(expression_start) {
                                    entry_alias = Some(BindingEntryAlias::BindingPattern(Box::new(pattern)));
                                }
                            } else if Self::is_member_expression(&expression) {
                                entry_alias = Some(BindingEntryAlias::MemberExpression(Box::new(expression)));
                            } else if Self::is_identifier(&expression) {
                                entry_alias = Some(BindingEntryAlias::Identifier(expression_into_identifier(expression)));
                            } else {
                                self.syntax_error("Invalid destructuring assignment target");
                                break;
                            }
                        } else if self.match_token(TokenType::CurlyOpen) || self.match_token(TokenType::BracketOpen) {
                            let nested = self.parse_binding_pattern();
                            entry_alias = Some(BindingEntryAlias::BindingPattern(Box::new(nested)));
                        } else if self.match_identifier_name() {
                            let alias_start = self.binding_pattern_start.unwrap_or_else(|| self.position());
                            let token = self.consume();
                            let value = self.token_value(&token).to_vec();
                            let id = self.make_identifier(alias_start, value.clone());
                            self.pattern_bound_names.push((value.into(), id.clone()));
                            entry_alias = Some(BindingEntryAlias::Identifier(id));
                        } else {
                            self.expected("identifier or binding pattern");
                            break;
                        }
                    } else if needs_alias {
                        self.expected("alias for string or numeric literal name");
                        break;
                    } else if !entry_name_value.is_empty() {
                        // Shorthand: name is the bound identifier.
                        if entry_is_keyword {
                            self.syntax_error("Binding pattern target may not be a reserved word");
                        }
                        if let Some(BindingEntryName::Identifier(ref id)) = entry_name {
                            self.pattern_bound_names.push((entry_name_value, id.clone()));
                        }
                    }
                }
            } else if self.allow_member_expressions {
                let expression_start = self.position();
                let expression = self.parse_expression(PRECEDENCE_ASSIGNMENT, Associativity::Right, ForbiddenTokens::none().forbid(&[TokenType::Equals]));
                if Self::is_object_expression(&expression) || Self::is_array_expression(&expression) {
                    if let Some(pattern) = self.synthesize_binding_pattern(expression_start) {
                        entry_alias = Some(BindingEntryAlias::BindingPattern(Box::new(pattern)));
                    }
                } else if Self::is_member_expression(&expression) {
                    entry_alias = Some(BindingEntryAlias::MemberExpression(Box::new(expression)));
                } else if Self::is_identifier(&expression) {
                    let id = expression_into_identifier(expression);
                    self.pattern_bound_names.push((id.name.clone(), id.clone()));
                    entry_alias = Some(BindingEntryAlias::Identifier(id));
                } else {
                    self.syntax_error("Invalid destructuring assignment target");
                    break;
                }
            } else if self.match_token(TokenType::CurlyOpen) || self.match_token(TokenType::BracketOpen) {
                let nested = self.parse_binding_pattern();
                entry_alias = Some(BindingEntryAlias::BindingPattern(Box::new(nested)));
            } else if self.match_identifier_name() {
                let alias_start = self.binding_pattern_start.unwrap_or_else(|| self.position());
                let token = self.consume();
                let value = self.token_value(&token).to_vec();
                let id = self.make_identifier(alias_start, value.clone());
                self.pattern_bound_names.push((value.into(), id.clone()));
                entry_alias = Some(BindingEntryAlias::Identifier(id));
            } else {
                self.expected("identifier or binding pattern");
                break;
            }

            let initializer = if self.match_token(TokenType::Equals) {
                if is_rest {
                    self.syntax_error("Unexpected initializer after rest element");
                }
                self.consume();
                Some(self.parse_assignment_expression())
            } else {
                None
            };

            entries.push(BindingEntry {
                name: entry_name,
                alias: entry_alias,
                initializer,
                is_rest,
            });

            if is_rest {
                if self.match_token(TokenType::Comma) {
                    self.syntax_error("Rest element may not be followed by a comma");
                    self.consume();
                }
                break;
            }

            if self.match_token(TokenType::Comma) {
                self.consume();
            } else if is_object && !self.match_token(closing_token) {
                self.consume_token(TokenType::Comma);
            }
        }

        // Consume trailing commas for arrays.
        if !is_object {
            while self.match_token(TokenType::Comma) {
                self.consume();
            }
        }

        self.consume_token(closing_token);
        self.binding_pattern_start = outer_pattern_start;
        BindingPattern { kind, entries }
    }

    // https://tc39.es/ecma262/#sec-imports
    // ImportDeclaration : `import` ImportClause FromClause `;`
    //                   | `import` ModuleSpecifier `;`
    // ImportClause : ImportedDefaultBinding
    //             | NameSpaceImport
    //             | NamedImports
    //             | ImportedDefaultBinding `,` NameSpaceImport
    //             | ImportedDefaultBinding `,` NamedImports
    pub(crate) fn parse_import_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Import);

        if self.program_type != ProgramType::Module {
            self.syntax_error("Cannot use 'import' outside a module");
        }

        if self.match_token(TokenType::StringLiteral) {
            let module_specifier = self.consume_module_specifier();
            let attributes = self.parse_with_clause();
            self.consume_or_insert_semicolon();
            return self.statement(start, StatementKind::Import(ImportStatementData {
                module_request: ModuleRequest { module_specifier, attributes },
                entries: Vec::new(),
            }));
        }

        let mut entries: Vec<ImportEntry> = Vec::new();
        let mut continue_parsing = true;

        if self.match_imported_binding() {
            let token = self.consume();
            let local_name: Utf16String = self.token_value(&token).into();
            entries.push(ImportEntry {
                import_name: Some(utf16!("default").into()),
                local_name,
            });
            if self.match_token(TokenType::Comma) {
                self.consume();
            } else {
                continue_parsing = false;
            }
        }

        if continue_parsing {
            if self.match_token(TokenType::Asterisk) {
                // NameSpaceImport: * as ImportedBinding
                self.consume();
                if !self.match_as() {
                    self.expected("'as'");
                }
                self.consume(); // consume 'as'
                if self.match_imported_binding() {
                    let token = self.consume();
                    let namespace_name: Utf16String = self.token_value(&token).into();
                    entries.push(ImportEntry {
                        import_name: None,
                        local_name: namespace_name,
                    });
                } else {
                    self.expected("identifier");
                }
            } else if self.match_token(TokenType::CurlyOpen) {
                // NamedImports: { ImportSpecifier, ... }
                self.consume();
                while !self.done() && !self.match_token(TokenType::CurlyClose) {
                    if self.match_identifier_name() {
                        let require_as = !self.match_imported_binding();
                        let name_pos = self.position();
                        let token = self.consume();
                        let name = self.token_value(&token).to_vec();

                        if self.match_as() {
                            self.consume(); // consume 'as'
                            let alias_token = self.consume_identifier();
                            let alias = self.token_value(&alias_token).to_vec();
                            self.check_identifier_name_for_assignment_validity(&alias, false);
                            entries.push(ImportEntry {
                                import_name: Some(name.into()),
                                local_name: alias.into(),
                            });
                        } else if require_as {
                            self.syntax_error_at_position(
                                &format!("Unexpected reserved word '{}'", String::from_utf16_lossy(&name)),
                                name_pos,
                            );
                        } else {
                            self.check_identifier_name_for_assignment_validity(&name, false);
                            let name: Utf16String = name.into();
                            entries.push(ImportEntry {
                                import_name: Some(name.clone()),
                                local_name: name,
                            });
                        }
                    } else if self.match_token(TokenType::StringLiteral) {
                        let token = self.consume();
                        let (name, _) = self.parse_string_value(&token);
                        if let Some(&last) = name.last() {
                            if (0xD800..=0xDBFF).contains(&last) {
                                self.syntax_error("StringValue ending with unpaired high surrogate");
                            }
                        }

                        if !self.match_as() {
                            self.expected("'as'");
                        }
                        self.consume(); // consume 'as'

                        let alias_token = self.consume_identifier();
                        let alias = self.token_value(&alias_token).to_vec();
                        self.check_identifier_name_for_assignment_validity(&alias, false);
                        entries.push(ImportEntry {
                            import_name: Some(name),
                            local_name: alias.into(),
                        });
                    } else {
                        self.expected("identifier");
                        break;
                    }

                    if !self.match_token(TokenType::Comma) {
                        break;
                    }
                    self.consume();
                }
                self.consume_token(TokenType::CurlyClose);
            } else {
                self.expected("import clauses");
            }
        }

        if !self.match_from() {
            self.expected("'from'");
        }
        self.consume(); // consume 'from'

        let module_specifier = self.consume_module_specifier();
        let attributes = self.parse_with_clause();
        self.consume_or_insert_semicolon();

        self.statement(start, StatementKind::Import(ImportStatementData {
            module_request: ModuleRequest { module_specifier, attributes },
            entries,
        }))
    }

    // https://tc39.es/ecma262/#sec-exports
    // ExportDeclaration : `export` ExportFromClause FromClause `;`
    //                   | `export` NamedExports `;`
    //                   | `export` VariableStatement
    //                   | `export` Declaration
    //                   | `export` `default` HoistableDeclaration
    //                   | `export` `default` ClassDeclaration
    //                   | `export` `default` AssignmentExpression `;`
    pub(crate) fn parse_export_statement(&mut self) -> Statement {
        let start = self.position();
        self.consume_token(TokenType::Export);

        if self.program_type != ProgramType::Module {
            self.syntax_error("Cannot use 'export' outside a module");
        }

        let mut entries: Vec<ExportEntry> = Vec::new();
        let mut statement: Option<Box<Statement>> = None;
        let mut is_default = false;
        let mut from_specifier: Option<Utf16String> = None;

        if self.match_token(TokenType::Default) {
            is_default = true;
            self.consume();

            let mut local_name: Option<Utf16String> = None;

            let matches_function = self.match_function_declaration_for_export();

            if matches_function != MatchesFunctionDeclaration::No {
                let has_default_name = matches_function == MatchesFunctionDeclaration::WithoutName;
                let declaration = self.parse_function_declaration_for_export(has_default_name);
                if !has_default_name {
                    if let StatementKind::FunctionDeclaration { name: Some(ref name_id), .. } = declaration.inner {
                        local_name = Some(name_id.name.clone());
                    }
                }
                statement = Some(Box::new(declaration));
            } else if self.match_token(TokenType::Class) {
                let next = self.next_token();
                if next.token_type != TokenType::CurlyOpen && next.token_type != TokenType::Extends {
                    let declaration = self.parse_class_declaration();
                    if let StatementKind::ClassDeclaration(ref class) = declaration.inner {
                        if let Some(ref name_id) = class.name {
                            local_name = Some(name_id.name.clone());
                        }
                    }
                    statement = Some(Box::new(declaration));
                } else {
                    // Unnamed class declaration - don't consume semicolon,
                    // matching the C++ parser's special_case_declaration_without_name.
                    let expression = self.parse_assignment_expression();
                    let expression_range = expression.range;
                    statement = Some(Box::new(Statement::new(expression_range, StatementKind::Expression(Box::new(expression)))));
                }
            } else if self.match_expression() {
                // Check if this is an unnamed function/class declaration that
                // should NOT consume a trailing semicolon.
                let special_case_declaration_without_name = self.match_token(TokenType::Class)
                    || self.match_token(TokenType::Function)
                    || (self.match_token(TokenType::Async) && {
                        let next = self.next_token();
                        next.token_type == TokenType::Function && !next.trivia_has_line_terminator
                    });
                let expression = self.parse_assignment_expression();
                if !special_case_declaration_without_name {
                    self.consume_or_insert_semicolon();
                }
                let expression_range = expression.range;
                statement = Some(Box::new(Statement::new(expression_range, StatementKind::Expression(Box::new(expression)))));
            } else {
                self.expected("declaration or assignment expression");
            }

            if local_name.is_none() {
                local_name = Some(utf16!("*default*").into());
            }

            entries.push(ExportEntry {
                kind: ExportEntryKind::NamedExport,
                export_name: Some(utf16!("default").into()),
                local_or_import_name: local_name,
            });
        } else {
            #[derive(Clone, Copy, Debug, PartialEq, Eq)]
            enum FromSpecifier { NotAllowed, Optional, Required }
            let mut check_for_from = FromSpecifier::NotAllowed;

            if self.match_token(TokenType::Asterisk) {
                self.consume();
                if self.match_as() {
                    self.consume(); // consume 'as'
                    let (exported_name, _) = self.parse_module_export_name();
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::ModuleRequestAll,
                        export_name: Some(exported_name),
                        local_or_import_name: None,
                    });
                } else {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::ModuleRequestAllButDefault,
                        export_name: None,
                        local_or_import_name: None,
                    });
                }
                check_for_from = FromSpecifier::Required;
            } else if self.match_declaration() {
                let declaration = self.parse_declaration();
                let names = get_declaration_export_names(&declaration);
                for name in &names {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::NamedExport,
                        export_name: Some(name.clone()),
                        local_or_import_name: Some(name.clone()),
                    });
                }
                statement = Some(Box::new(declaration));
            } else if self.match_token(TokenType::Var) {
                let var_declaration = self.parse_variable_declaration(false);
                let names = get_declaration_export_names(&var_declaration);
                for name in &names {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::NamedExport,
                        export_name: Some(name.clone()),
                        local_or_import_name: Some(name.clone()),
                    });
                }
                statement = Some(Box::new(var_declaration));
            } else if self.match_token(TokenType::CurlyOpen) {
                self.consume();
                check_for_from = FromSpecifier::Optional;

                while !self.done() && !self.match_token(TokenType::CurlyClose) {
                    let (identifier, was_string) = self.parse_module_export_name();
                    if was_string {
                        check_for_from = FromSpecifier::Required;
                    }

                    if self.match_as() {
                        self.consume(); // consume 'as'
                        let (export_name, _) = self.parse_module_export_name();
                        entries.push(ExportEntry {
                            kind: ExportEntryKind::NamedExport,
                            export_name: Some(export_name),
                            local_or_import_name: Some(identifier),
                        });
                    } else {
                        entries.push(ExportEntry {
                            kind: ExportEntryKind::NamedExport,
                            export_name: Some(identifier.clone()),
                            local_or_import_name: Some(identifier),
                        });
                    }

                    if !self.match_token(TokenType::Comma) {
                        break;
                    }
                    self.consume();
                }

                if entries.is_empty() {
                    entries.push(ExportEntry {
                        kind: ExportEntryKind::EmptyNamedExport,
                        export_name: None,
                        local_or_import_name: None,
                    });
                }

                self.consume_token(TokenType::CurlyClose);
            } else {
                self.syntax_error("Unexpected token 'export'");
            }

            if check_for_from != FromSpecifier::NotAllowed && self.match_from() {
                self.consume(); // consume 'from'
                from_specifier = Some(self.consume_module_specifier());
            } else if check_for_from == FromSpecifier::Required {
                self.expected("'from'");
            }

            if from_specifier.is_none() && check_for_from != FromSpecifier::NotAllowed {
                self.consume_or_insert_semicolon();
            }
        }

        let module_request = if let Some(specifier) = from_specifier {
            let attributes = self.parse_with_clause();
            self.consume_or_insert_semicolon();
            Some(ModuleRequest { module_specifier: specifier, attributes })
        } else {
            None
        };

        // Check for duplicate exported names.
        for entry in &entries {
            if let Some(ref name) = entry.export_name {
                if !self.exported_names.insert(name.clone()) {
                    self.syntax_error_at_position(
                        &format!(
                            "Duplicate export with name: '{}'",
                            String::from_utf16_lossy(name.as_slice())
                        ),
                        start,
                    );
                }
            }
        }

        self.statement(start, StatementKind::Export(ExportStatementData {
            statement,
            entries,
            is_default_export: is_default,
            module_request,
        }))
    }

    fn match_imported_binding(&self) -> bool {
        self.match_identifier() || self.match_token(TokenType::Yield) || self.match_token(TokenType::Await)
    }

    fn match_as(&self) -> bool {
        self.match_token(TokenType::Identifier) && self.token_original_value(&self.current_token) == utf16!("as")
    }

    fn match_from(&self) -> bool {
        self.match_token(TokenType::Identifier) && self.token_original_value(&self.current_token) == utf16!("from")
    }

    fn consume_module_specifier(&mut self) -> Utf16String {
        if !self.match_token(TokenType::StringLiteral) {
            self.expected("module specifier (string)");
            return utf16!("!!invalid!!").into();
        }
        let token = self.consume();
        let (value, _) = self.parse_string_value(&token);
        value
    }

    fn parse_module_export_name(&mut self) -> (Utf16String, bool) {
        if self.match_identifier_name() {
            let token = self.consume();
            (self.token_value(&token).into(), false)
        } else if self.match_token(TokenType::StringLiteral) {
            let token = self.consume();
            let (value, _) = self.parse_string_value(&token);
            // https://tc39.es/ecma262/#sec-module-semantics-static-semantics-early-errors
            // It is a Syntax Error if IsStringWellFormedUnicode of the StringValue
            // of StringLiteral is false.
            if let Some(&last) = value.last() {
                if (0xD800..=0xDBFF).contains(&last) {
                    self.syntax_error("StringValue ending with unpaired high surrogate");
                }
            }
            (value, true)
        } else {
            self.expected("export specifier (string or identifier)");
            (Utf16String::default(), false)
        }
    }

    // https://tc39.es/ecma262/#sec-imports
    // WithClause : `with` `{` WithEntries `}`
    // WithEntries : AttributeKey `:` StringLiteral
    fn parse_with_clause(&mut self) -> Vec<ImportAttribute> {
        if !self.match_token(TokenType::With) {
            return Vec::new();
        }
        self.consume();
        self.consume_token(TokenType::CurlyOpen);

        let mut attributes = Vec::new();
        while !self.done() && !self.match_token(TokenType::CurlyClose) {
            let key: Utf16String = if self.match_token(TokenType::StringLiteral) {
                let token = self.consume();
                let (value, _) = self.parse_string_value(&token);
                value
            } else if self.match_identifier_name() {
                let token = self.consume();
                self.token_value(&token).into()
            } else {
                self.expected("identifier or string as attribute key");
                self.consume();
                continue;
            };

            self.consume_token(TokenType::Colon);

            if self.match_token(TokenType::StringLiteral) {
                let token = self.consume();
                let (value, _) = self.parse_string_value(&token);
                attributes.push(ImportAttribute { key, value });
            } else {
                self.expected("string as attribute value");
                self.consume();
            }

            if self.match_token(TokenType::Comma) {
                self.consume();
            } else {
                break;
            }
        }
        self.consume_token(TokenType::CurlyClose);
        attributes
    }

    fn match_function_declaration_for_export(&mut self) -> MatchesFunctionDeclaration {
        if self.match_token(TokenType::Function) {
            let next = self.next_token();
            if next.token_type == TokenType::Asterisk {
                self.save_state();
                self.consume(); // function
                self.consume(); // *
                let result = if self.match_token(TokenType::ParenOpen) {
                    MatchesFunctionDeclaration::WithoutName
                } else {
                    MatchesFunctionDeclaration::Yes
                };
                self.load_state();
                return result;
            }
            return if next.token_type == TokenType::ParenOpen {
                MatchesFunctionDeclaration::WithoutName
            } else {
                MatchesFunctionDeclaration::Yes
            };
        }

        if self.match_token(TokenType::Async) {
            let next = self.next_token();
            if next.token_type != TokenType::Function || next.trivia_has_line_terminator {
                return MatchesFunctionDeclaration::No;
            }
            self.save_state();
            self.consume(); // async
            self.consume(); // function
            if self.match_token(TokenType::Asterisk) {
                self.consume(); // *
            }
            let result = if self.match_token(TokenType::ParenOpen) {
                MatchesFunctionDeclaration::WithoutName
            } else {
                MatchesFunctionDeclaration::Yes
            };
            self.load_state();
            return result;
        }

        MatchesFunctionDeclaration::No
    }

    fn parse_function_declaration_for_export(&mut self, has_default_name: bool) -> Statement {
        if has_default_name {
            self.has_default_export_name = true;
            let result = self.parse_function_declaration();
            self.has_default_export_name = false;
            result
        } else {
            self.parse_function_declaration()
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum MatchesFunctionDeclaration {
    No,
    Yes,
    WithoutName,
}
