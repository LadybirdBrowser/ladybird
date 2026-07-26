/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Flap source parser.

use super::ast::*;
use super::diagnostic::{Diagnostic, SourceLocation, SourceSpan};
use crate::types::{BlockTemperature, Type};

#[derive(Debug, Clone, PartialEq, Eq)]
enum TokenKind {
    Identifier(String),
    Integer(i64),
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    LeftAngle,
    RightAngle,
    Colon,
    Comma,
    Semicolon,
    Equals,
    DoubleEquals,
    FatArrow,
    Arrow,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    DoubleAmpersand,
    DoublePipe,
    Ampersand,
    Pipe,
    Caret,
    Bang,
    Tilde,
    At,
    End,
}

#[derive(Debug, Clone)]
struct Token {
    kind: TokenKind,
    span: SourceSpan,
}

enum InfixOperator {
    Binary(BinaryOperator),
    ValueTypeTest(UnaryOperator),
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ScalarMatchArmStyle {
    Block,
    Target,
}

struct Lexer<'a> {
    filename: &'a str,
    source: &'a str,
    offset: usize,
    line: usize,
    column: usize,
}

impl<'a> Lexer<'a> {
    fn new(filename: &'a str, source: &'a str) -> Self {
        Self {
            filename,
            source,
            offset: 0,
            line: 1,
            column: 1,
        }
    }

    fn location(&self) -> SourceLocation {
        SourceLocation {
            offset: self.offset,
            line: self.line,
            column: self.column,
        }
    }

    fn current(&self) -> Option<char> {
        self.source[self.offset..].chars().next()
    }

    fn advance(&mut self) -> Option<char> {
        let character = self.current()?;
        self.offset += character.len_utf8();
        if character == '\n' {
            self.line += 1;
            self.column = 1;
        } else {
            self.column += 1;
        }
        Some(character)
    }

    fn token(&self, kind: TokenKind, start: SourceLocation) -> Token {
        Token {
            kind,
            span: SourceSpan {
                start,
                end: self.location(),
            },
        }
    }

    fn lex_punctuation(&mut self) -> Option<TokenKind> {
        let source = &self.source[self.offset..];
        let (kind, length) = if source.starts_with("=>") {
            (TokenKind::FatArrow, 2)
        } else if source.starts_with("==") {
            (TokenKind::DoubleEquals, 2)
        } else if source.starts_with("->") {
            (TokenKind::Arrow, 2)
        } else if source.starts_with("&&") {
            (TokenKind::DoubleAmpersand, 2)
        } else if source.starts_with("||") {
            (TokenKind::DoublePipe, 2)
        } else {
            let kind = match self.current()? {
                '(' => TokenKind::LeftParen,
                ')' => TokenKind::RightParen,
                '{' => TokenKind::LeftBrace,
                '}' => TokenKind::RightBrace,
                '[' => TokenKind::LeftBracket,
                ']' => TokenKind::RightBracket,
                '<' => TokenKind::LeftAngle,
                '>' => TokenKind::RightAngle,
                ':' => TokenKind::Colon,
                ',' => TokenKind::Comma,
                ';' => TokenKind::Semicolon,
                '=' => TokenKind::Equals,
                '+' => TokenKind::Plus,
                '-' => TokenKind::Minus,
                '*' => TokenKind::Star,
                '/' => TokenKind::Slash,
                '%' => TokenKind::Percent,
                '&' => TokenKind::Ampersand,
                '|' => TokenKind::Pipe,
                '^' => TokenKind::Caret,
                '!' => TokenKind::Bang,
                '~' => TokenKind::Tilde,
                '@' => TokenKind::At,
                _ => return None,
            };
            (kind, 1)
        };
        for _ in 0..length {
            self.advance();
        }
        Some(kind)
    }

    fn lex(mut self) -> Result<Vec<Token>, Diagnostic> {
        let mut tokens = Vec::new();
        while let Some(character) = self.current() {
            if character.is_whitespace() {
                self.advance();
                continue;
            }
            if character == '#' {
                while self.current().is_some_and(|character| character != '\n') {
                    self.advance();
                }
                continue;
            }

            let start = self.location();
            if let Some(kind) = self.lex_punctuation() {
                tokens.push(self.token(kind, start));
                continue;
            }
            let kind = match character {
                '0'..='9' => {
                    let number_start = self.offset;
                    self.advance();
                    let radix = if character == '0' && matches!(self.current(), Some('x' | 'X')) {
                        self.advance();
                        while self
                            .current()
                            .is_some_and(|character| character.is_ascii_hexdigit() || character == '_')
                        {
                            self.advance();
                        }
                        16
                    } else {
                        while self
                            .current()
                            .is_some_and(|character| character.is_ascii_digit() || character == '_')
                        {
                            self.advance();
                        }
                        10
                    };
                    let spelling = &self.source[number_start..self.offset];
                    let digits = spelling
                        .trim_start_matches("0x")
                        .trim_start_matches("0X")
                        .replace('_', "");
                    let value = i64::from_str_radix(&digits, radix).map_err(|_| {
                        Diagnostic::new(
                            self.filename,
                            SourceSpan {
                                start,
                                end: self.location(),
                            },
                            format!("invalid integer literal '{spelling}'"),
                        )
                    })?;
                    TokenKind::Integer(value)
                }
                _ if is_identifier_start(character) => {
                    let identifier_start = self.offset;
                    self.advance();
                    while self.current().is_some_and(is_identifier_continue) {
                        self.advance();
                    }
                    TokenKind::Identifier(self.source[identifier_start..self.offset].to_string())
                }
                _ => {
                    self.advance();
                    return Err(Diagnostic::new(
                        self.filename,
                        SourceSpan {
                            start,
                            end: self.location(),
                        },
                        format!("unexpected character '{character}'"),
                    ));
                }
            };
            tokens.push(self.token(kind, start));
        }
        let location = self.location();
        tokens.push(Token {
            kind: TokenKind::End,
            span: SourceSpan {
                start: location,
                end: location,
            },
        });
        Ok(tokens)
    }
}

fn is_identifier_start(character: char) -> bool {
    character.is_ascii_alphabetic() || matches!(character, '_' | '.')
}

fn is_identifier_continue(character: char) -> bool {
    character.is_ascii_alphanumeric() || matches!(character, '_' | '.')
}

struct Parser<'a> {
    filename: &'a str,
    tokens: Vec<Token>,
    index: usize,
    next_synthetic_binding: u64,
}

impl<'a> Parser<'a> {
    fn new(filename: &'a str, tokens: Vec<Token>) -> Self {
        Self {
            filename,
            tokens,
            index: 0,
            next_synthetic_binding: 0,
        }
    }

    fn current(&self) -> &Token {
        &self.tokens[self.index]
    }

    fn peek(&self, offset: usize) -> &Token {
        &self.tokens[(self.index + offset).min(self.tokens.len() - 1)]
    }

    fn advance(&mut self) -> Token {
        let token = self.current().clone();
        if token.kind != TokenKind::End {
            self.index += 1;
        }
        token
    }

    fn error<T>(&self, message: impl Into<String>) -> Result<T, Diagnostic> {
        Err(Diagnostic::new(self.filename, self.current().span, message))
    }

    fn consume(&mut self, kind: TokenKind, description: &str) -> Result<Token, Diagnostic> {
        if self.current().kind == kind {
            Ok(self.advance())
        } else {
            self.error(format!("expected {description}"))
        }
    }

    fn consume_identifier(&mut self, description: &str) -> Result<(String, SourceSpan), Diagnostic> {
        let token = self.advance();
        if let TokenKind::Identifier(identifier) = token.kind {
            Ok((identifier, token.span))
        } else {
            self.error(format!("expected {description}"))
        }
    }

    fn consume_keyword(&mut self, keyword: &str) -> Result<Token, Diagnostic> {
        match &self.current().kind {
            TokenKind::Identifier(identifier) if identifier == keyword => Ok(self.advance()),
            _ => self.error(format!("expected '{keyword}'")),
        }
    }

    fn at_keyword(&self, keyword: &str) -> bool {
        matches!(&self.current().kind, TokenKind::Identifier(identifier) if identifier == keyword)
    }

    fn parse_program(&mut self) -> Result<Program, Diagnostic> {
        let mut declarations = Vec::new();
        while self.current().kind != TokenKind::End {
            if self.at_keyword("inline") {
                declarations.push(Declaration::InlineFunction(self.parse_inline_function()?));
            } else if self.at_keyword("handler") {
                declarations.push(Declaration::Handler(self.parse_handler()?));
            } else {
                return self.error("expected 'inline fn' or 'handler'");
            }
        }
        Ok(Program { declarations })
    }

    fn parse_inline_function(&mut self) -> Result<InlineFunctionDeclaration, Diagnostic> {
        let start = self.consume_keyword("inline")?.span.start;
        self.consume_keyword("fn")?;
        self.parse_inline_function_after_keyword(start)
    }

    fn parse_inline_function_after_keyword(
        &mut self,
        start: SourceLocation,
    ) -> Result<InlineFunctionDeclaration, Diagnostic> {
        let (name, _) = self.consume_identifier("inline function name")?;
        let parameters = self.parse_parameters()?;
        let return_type = if self.current().kind == TokenKind::Arrow {
            self.advance();
            Some(self.parse_type()?)
        } else {
            None
        };
        let body = if return_type.is_some() {
            self.parse_value_block()?
        } else {
            self.parse_block()?
        };
        Ok(InlineFunctionDeclaration {
            name,
            parameters,
            return_type,
            span: SourceSpan {
                start,
                end: body.span.end,
            },
            body,
        })
    }

    fn parse_handler(&mut self) -> Result<HandlerDeclaration, Diagnostic> {
        let start = self.consume_keyword("handler")?.span.start;
        let (name, _) = self.consume_identifier("handler name")?;
        let parameters = self.parse_parameters()?;
        if parameters.iter().any(|parameter| parameter.is_else) {
            return self.error("a handler cannot declare an else parameter");
        }
        let temperature = if self.current().kind == TokenKind::At {
            self.advance();
            self.consume_keyword("cold")?;
            BlockTemperature::Cold
        } else {
            BlockTemperature::Default
        };
        let body = if self.current().kind == TokenKind::Equals {
            self.advance();
            let expression = self.parse_expression()?;
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            let span = SourceSpan {
                start: expression.span.start,
                end,
            };
            Block {
                span,
                statements: vec![Statement::new(StatementKind::Expression(expression), span)],
                value: None,
            }
        } else {
            self.parse_block()?
        };
        Ok(HandlerDeclaration {
            name,
            parameters,
            temperature,
            span: SourceSpan {
                start,
                end: body.span.end,
            },
            body,
        })
    }

    fn parse_parameters(&mut self) -> Result<Vec<Parameter>, Diagnostic> {
        self.consume(TokenKind::LeftParen, "'('")?;
        let mut parameters = Vec::new();
        while self.current().kind != TokenKind::RightParen {
            let is_else = if self.at_keyword("else") {
                self.advance();
                true
            } else {
                false
            };
            let (name, span) = self.consume_identifier("parameter name")?;
            self.consume(TokenKind::Colon, "':'")?;
            let (mode, ty) = self.parse_parameter_type()?;
            if is_else && (mode != ParameterMode::In || ty != Type::label()) {
                return self.error("an else parameter must have type Label");
            }
            parameters.push(Parameter {
                name,
                mode,
                ty,
                is_else,
                span,
            });
            if self.current().kind != TokenKind::Comma {
                break;
            }
            self.advance();
        }
        self.consume(TokenKind::RightParen, "')'")?;
        if let Some(index) = parameters.iter().position(|parameter| parameter.is_else)
            && index + 1 != parameters.len()
        {
            return self.error("an else parameter must be last");
        }
        Ok(parameters)
    }

    fn parse_argument_list(&mut self) -> Result<Vec<Expression>, Diagnostic> {
        self.consume(TokenKind::LeftParen, "'('")?;
        let mut arguments = Vec::new();
        while self.current().kind != TokenKind::RightParen {
            arguments.push(self.parse_expression()?);
            if self.current().kind != TokenKind::Comma {
                break;
            }
            self.advance();
        }
        self.consume(TokenKind::RightParen, "')'")?;
        Ok(arguments)
    }

    fn parse_parameter_type(&mut self) -> Result<(ParameterMode, Type), Diagnostic> {
        let mode = match &self.current().kind {
            TokenKind::Identifier(identifier) if identifier == "in" => Some(ParameterMode::In),
            TokenKind::Identifier(identifier) if identifier == "out" => Some(ParameterMode::Out),
            TokenKind::Identifier(identifier) if identifier == "inout" => Some(ParameterMode::InOut),
            _ => None,
        };
        if let Some(mode) = mode {
            self.advance();
            let ty = self.parse_type()?;
            Ok((mode, ty))
        } else {
            Ok((ParameterMode::In, self.parse_type()?))
        }
    }

    fn parse_type(&mut self) -> Result<Type, Diagnostic> {
        if self.current().kind == TokenKind::LeftParen {
            self.advance();
            let mut elements = Vec::new();
            loop {
                elements.push(self.parse_type()?);
                if self.current().kind != TokenKind::Comma {
                    break;
                }
                self.advance();
            }
            self.consume(TokenKind::RightParen, "')'")?;
            if elements.len() < 2 {
                return self.error("a tuple type requires at least two elements");
            }
            return Ok(Type::Tuple(elements));
        }
        let (name, span) = self.consume_identifier("type")?;
        if let Some(ty) = Type::from_source_name(&name) {
            return Ok(ty);
        }
        if name == "Value" && self.current().kind != TokenKind::LeftAngle {
            return Ok(Type::Value);
        }
        let constructor = match name.as_str() {
            "Label" => return Ok(Type::label()),
            "Value" | "ptr" | "Sequence" | "Field" | "BinaryOperation" | "CheckedBinaryOperation" => name,
            _ => return Err(Diagnostic::new(self.filename, span, format!("unknown type '{name}'"))),
        };
        self.consume(TokenKind::LeftAngle, "'<'")?;
        let inner = Box::new(self.parse_type()?);
        self.consume(TokenKind::RightAngle, "'>'")?;
        Ok(match constructor.as_str() {
            "Value" => Type::Boxed(inner),
            "ptr" => Type::Pointer(inner),
            "Sequence" => Type::Sequence(inner),
            "Field" => Type::Field(inner),
            "BinaryOperation" => Type::BinaryOperation(inner),
            "CheckedBinaryOperation" => Type::CheckedBinaryOperation(inner),
            _ => unreachable!(),
        })
    }

    fn parse_block(&mut self) -> Result<Block, Diagnostic> {
        let start = self.consume(TokenKind::LeftBrace, "'{'")?.span.start;
        let mut statements = Vec::new();
        while self.current().kind != TokenKind::RightBrace {
            if self.current().kind == TokenKind::End {
                return self.error("expected '}' before end of file");
            }
            statements.push(self.parse_statement()?);
        }
        let end = self.advance().span.end;
        Ok(Block {
            statements,
            value: None,
            span: SourceSpan { start, end },
        })
    }

    fn parse_block_temperature(&mut self) -> Result<BlockTemperature, Diagnostic> {
        if self.current().kind != TokenKind::At {
            return Ok(BlockTemperature::Default);
        }
        self.advance();
        if self.at_keyword("hot") {
            self.advance();
            Ok(BlockTemperature::Hot)
        } else if self.at_keyword("cold") {
            self.advance();
            Ok(BlockTemperature::Cold)
        } else {
            self.error("expected 'hot' or 'cold' block annotation")
        }
    }

    fn parse_statement(&mut self) -> Result<Statement, Diagnostic> {
        let start = self.current().span.start;
        if self.at_keyword("match") {
            return self.parse_match();
        }
        if self.at_keyword("guard") {
            self.advance();
            if self.at_keyword("let") {
                self.advance();
                if self.current().kind == TokenKind::LeftParen {
                    return self.error("tuple return bindings use '[...]'");
                }
                if self.at_keyword("Value") {
                    return self.parse_value_refinement(start);
                }
                return self.parse_fallible_binding(start);
            }
            let condition = self.parse_expression()?;
            self.consume_keyword("else")?;
            let failure = self.parse_else_continuation()?;
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            return Ok(Statement::new(
                StatementKind::Guard { condition, failure },
                SourceSpan { start, end },
            ));
        }
        if self.at_keyword("goto") {
            self.advance();
            let (target, _) = self.consume_identifier("target label")?;
            let arguments = if self.current().kind == TokenKind::LeftParen {
                self.parse_argument_list()?
            } else {
                Vec::new()
            };
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            return Ok(Statement::new(
                StatementKind::ContinuationJump { target, arguments },
                SourceSpan { start, end },
            ));
        }
        if self.at_keyword("let") {
            self.advance();
            if self.at_keyword("Value") {
                return self.error("refutable bindings use 'guard let'");
            }
            if self.current().kind == TokenKind::LeftParen {
                return self.error("tuple return bindings use '[...]'");
            }
            if self.current().kind == TokenKind::LeftBracket {
                let pattern = self.parse_tuple_pattern()?;
                self.consume(TokenKind::Equals, "'='")?;
                let initializer = self.parse_expression()?;
                let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
                return Ok(Statement::new(
                    StatementKind::Let {
                        pattern,
                        initializer: Some(initializer),
                    },
                    SourceSpan { start, end },
                ));
            }
            let (name, _) = self.consume_identifier("binding name")?;
            if self.current().kind == TokenKind::Equals
                && self
                    .tokens
                    .get(self.index + 1)
                    .is_some_and(|token| matches!(token.kind, TokenKind::DoublePipe | TokenKind::Pipe))
            {
                self.advance();
                let parameters = if self.current().kind == TokenKind::DoublePipe {
                    self.advance();
                    Vec::new()
                } else {
                    self.consume(TokenKind::Pipe, "'|' or '||'")?;
                    let mut parameters = Vec::new();
                    while self.current().kind != TokenKind::Pipe {
                        let (parameter_name, span) = self.consume_identifier("continuation parameter")?;
                        self.consume(TokenKind::Colon, "':'")?;
                        let ty = self.parse_type()?;
                        parameters.push(Parameter {
                            name: parameter_name,
                            mode: ParameterMode::In,
                            ty,
                            is_else: false,
                            span,
                        });
                        if self.current().kind != TokenKind::Comma {
                            break;
                        }
                        self.advance();
                    }
                    self.consume(TokenKind::Pipe, "'|'")?;
                    parameters
                };
                let temperature = self.parse_block_temperature()?;
                let body = self.parse_block()?;
                let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
                return Ok(Statement::new(
                    StatementKind::Continuation {
                        name,
                        temperature,
                        parameters,
                        body,
                    },
                    SourceSpan { start, end },
                ));
            }
            let ty = if self.current().kind == TokenKind::Colon {
                self.advance();
                Some(self.parse_type()?)
            } else {
                None
            };
            let initializer = if self.current().kind == TokenKind::Equals {
                self.advance();
                Some(self.parse_expression()?)
            } else {
                None
            };
            if self.at_keyword("else") {
                return self.error("fallible bindings use 'guard let'");
            }
            if ty.is_none() && initializer.is_none() {
                return self.error("an inferred binding requires an initializer");
            }
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            return Ok(Statement::new(
                StatementKind::Let {
                    pattern: Pattern::Binding { name, ty },
                    initializer,
                },
                SourceSpan { start, end },
            ));
        }
        if self.at_keyword("if") {
            self.advance();
            let condition = self.parse_expression()?;
            let temperature = self.parse_block_temperature()?;
            let body = self.parse_block()?;
            if self.at_keyword("else") {
                if temperature != BlockTemperature::Default {
                    return self.error("a two-armed if cannot have a block layout annotation");
                }
                self.advance();
                let else_body = if self.at_keyword("if") {
                    let statement = self.parse_statement()?;
                    Block {
                        span: statement.span,
                        statements: vec![statement],
                        value: None,
                    }
                } else {
                    self.parse_block()?
                };
                let end = else_body.span.end;
                return Ok(Statement::new(
                    StatementKind::If {
                        name: None,
                        ty: None,
                        condition,
                        temperature,
                        then_body: body,
                        else_body: Some(else_body),
                    },
                    SourceSpan { start, end },
                ));
            }
            let end = body.span.end;
            return Ok(Statement::new(
                StatementKind::If {
                    name: None,
                    ty: None,
                    condition,
                    temperature,
                    then_body: body,
                    else_body: None,
                },
                SourceSpan { start, end },
            ));
        }
        if self.at_keyword("while") {
            self.advance();
            let condition = self.parse_expression()?;
            let body = self.parse_block()?;
            let end = body.span.end;
            return Ok(Statement::new(
                StatementKind::While {
                    condition,
                    body,
                    post_tested: false,
                },
                SourceSpan { start, end },
            ));
        }
        if self.at_keyword("do") {
            self.advance();
            let body = self.parse_block()?;
            self.consume_keyword("while")?;
            let condition = self.parse_expression()?;
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            return Ok(Statement::new(
                StatementKind::While {
                    condition,
                    body,
                    post_tested: true,
                },
                SourceSpan { start, end },
            ));
        }
        if matches!(&self.current().kind, TokenKind::Identifier(_))
            && self
                .tokens
                .get(self.index + 1)
                .is_some_and(|token| token.kind == TokenKind::LeftBracket)
        {
            let base = self.parse_primary_expression()?;
            self.consume(TokenKind::LeftBracket, "'['")?;
            let index = self.parse_expression()?;
            self.consume(TokenKind::RightBracket, "']'")?;
            self.consume(TokenKind::Equals, "'='")?;
            let value = self.parse_expression()?;
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            return Ok(Statement::new(
                StatementKind::IndexAssign { base, index, value },
                SourceSpan { start, end },
            ));
        }

        if matches!(&self.current().kind, TokenKind::Identifier(_))
            && self
                .tokens
                .get(self.index + 1)
                .is_some_and(|token| token.kind == TokenKind::Equals)
        {
            let (name, _) = self.consume_identifier("binding name")?;
            self.advance();
            let initializer = self.parse_expression()?;
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            return Ok(Statement::new(
                StatementKind::Assign { name, initializer },
                SourceSpan { start, end },
            ));
        }

        let expression = self.parse_expression()?;
        if self.at_keyword("else") {
            let ExpressionKind::Call { callee, arguments } = expression.kind else {
                return self.error("'else' requires a function call");
            };
            self.advance();
            let failure = self.parse_else_continuation()?;
            let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
            return Ok(Statement::new(
                StatementKind::FallibleCall {
                    callee,
                    arguments,
                    failure,
                },
                SourceSpan { start, end },
            ));
        }
        let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
        Ok(Statement::new(
            StatementKind::Expression(expression),
            SourceSpan { start, end },
        ))
    }

    fn parse_tuple_pattern(&mut self) -> Result<Pattern, Diagnostic> {
        self.consume(TokenKind::LeftBracket, "'['")?;
        let mut patterns = Vec::new();
        loop {
            let (name, _) = self.consume_identifier("tuple binding")?;
            patterns.push(if name == "_" {
                Pattern::Wildcard
            } else {
                Pattern::Binding { name, ty: None }
            });
            if self.current().kind != TokenKind::Comma {
                break;
            }
            self.advance();
        }
        self.consume(TokenKind::RightBracket, "']'")?;
        if patterns.len() < 2 {
            return self.error("a tuple binding requires at least two elements");
        }
        Ok(Pattern::Tuple(patterns))
    }

    fn parse_fallible_binding(&mut self, start: SourceLocation) -> Result<Statement, Diagnostic> {
        let pattern = if self.current().kind == TokenKind::LeftBracket {
            self.parse_tuple_pattern()?
        } else {
            let (name, _) = self.consume_identifier("binding name")?;
            let ty = if self.current().kind == TokenKind::Colon {
                self.advance();
                Some(self.parse_type()?)
            } else {
                None
            };
            Pattern::Binding { name, ty }
        };
        self.consume(TokenKind::Equals, "'='")?;
        let initializer = self.parse_expression()?;
        if !matches!(initializer.kind, ExpressionKind::Call { .. }) {
            return self.error("'guard let' requires a function call");
        }
        self.consume_keyword("else")?;
        let failure = self.parse_else_continuation()?;
        let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
        Ok(Statement::new(
            StatementKind::GuardLet {
                pattern,
                initializer,
                failure,
            },
            SourceSpan { start, end },
        ))
    }

    fn parse_else_continuation(&mut self) -> Result<ElseContinuation, Diagnostic> {
        if self.current().kind == TokenKind::At || self.current().kind == TokenKind::LeftBrace {
            let temperature = self.parse_block_temperature()?;
            if temperature == BlockTemperature::Hot {
                return self.error("an else block cannot be @hot without an explicit success block");
            }
            Ok(ElseContinuation::Block {
                temperature,
                body: self.parse_block()?,
            })
        } else {
            let (target, _) = self.consume_identifier("else continuation")?;
            if self.current().kind == TokenKind::LeftParen {
                Ok(ElseContinuation::Invocation {
                    target,
                    arguments: self.parse_argument_list()?,
                })
            } else {
                Ok(ElseContinuation::Label(target))
            }
        }
    }

    fn parse_value_refinement(&mut self, start: SourceLocation) -> Result<Statement, Diagnostic> {
        self.consume_keyword("Value")?;
        self.consume(TokenKind::LeftAngle, "'<'")?;
        let (refined_type, _) = self.consume_identifier("refined Value type")?;
        self.consume(TokenKind::RightAngle, "'>'")?;
        self.consume(TokenKind::LeftParen, "'('")?;
        let (binding, _) = self.consume_identifier("refined binding name")?;
        self.consume(TokenKind::RightParen, "')'")?;
        self.consume(TokenKind::Equals, "'='")?;
        let value = self.parse_expression()?;
        self.consume_keyword("else")?;
        let failure = self.parse_else_continuation()?;
        let end = self.consume(TokenKind::Semicolon, "';'")?.span.end;
        if !matches!(refined_type.as_str(), "Object" | "PrimitiveString" | "i32" | "f64") {
            return self.error(format!("cannot refine Value<{refined_type}>").as_str());
        }
        Ok(Statement::new(
            StatementKind::GuardLet {
                pattern: Pattern::ValueRepresentation {
                    representation: refined_type,
                    binding: Some(Box::new(Pattern::Binding {
                        name: binding,
                        ty: None,
                    })),
                },
                initializer: value,
                failure,
            },
            SourceSpan { start, end },
        ))
    }

    fn parse_value_block(&mut self) -> Result<Block, Diagnostic> {
        let start = self.consume(TokenKind::LeftBrace, "'{'")?.span.start;
        let mut statements = Vec::new();
        while self.current().kind != TokenKind::RightBrace {
            let tail = if self.at_keyword("match") {
                self.try_parse_tail_value(Self::parse_tail_value_match)
            } else if self.at_keyword("if") {
                self.try_parse_tail_value(Self::parse_tail_value_if)
            } else {
                None
            };
            if let Some((statement, value)) = tail {
                statements.push(statement);
                let end = self.advance().span.end;
                return Ok(Block {
                    statements,
                    value: Some(value),
                    span: SourceSpan { start, end },
                });
            }
            let expression_start = self.index;
            if matches!(
                self.current().kind,
                TokenKind::Identifier(_)
                    | TokenKind::Integer(_)
                    | TokenKind::Minus
                    | TokenKind::Bang
                    | TokenKind::Tilde
                    | TokenKind::Ampersand
                    | TokenKind::LeftBracket
                    | TokenKind::LeftParen
            ) && let Ok(value) = self.parse_expression()
                && self.current().kind == TokenKind::RightBrace
            {
                let end = self.advance().span.end;
                return Ok(Block {
                    statements,
                    value: Some(value),
                    span: SourceSpan { start, end },
                });
            }
            self.index = expression_start;
            statements.push(self.parse_statement()?);
        }
        self.error("value-producing block requires a tail expression")
    }

    fn try_parse_tail_value(
        &mut self,
        parse: fn(&mut Self) -> Result<(Statement, Expression), Diagnostic>,
    ) -> Option<(Statement, Expression)> {
        let start = self.index;
        let result = parse(self)
            .ok()
            .filter(|_| self.current().kind == TokenKind::RightBrace);
        if result.is_none() {
            self.index = start;
        }
        result
    }

    fn parse_tail_value_match(&mut self) -> Result<(Statement, Expression), Diagnostic> {
        let start = self.consume_keyword("match")?.span.start;
        let value = self.parse_expression()?;
        self.consume(TokenKind::LeftBrace, "'{'")?;
        if !self.at_keyword("Value") {
            return self.error("tail Value match requires Value representation patterns");
        }

        let (arms, fallback) = self.parse_value_match_arms(true)?;
        let end = self.consume(TokenKind::RightBrace, "'}'")?.span.end;
        let name = format!("$tail_value_{}", self.next_synthetic_binding);
        self.next_synthetic_binding += 1;
        Ok((
            Statement::new(
                StatementKind::ValueMatch {
                    name: Some(name.clone()),
                    value,
                    arms,
                    fallback,
                },
                SourceSpan { start, end },
            ),
            Expression {
                kind: ExpressionKind::Name(name),
                span: SourceSpan { start, end },
            },
        ))
    }

    fn parse_tail_value_if(&mut self) -> Result<(Statement, Expression), Diagnostic> {
        let start = self.consume_keyword("if")?.span.start;
        let condition = self.parse_expression()?;
        let then_body = self.parse_value_block()?;
        self.consume_keyword("else")?;
        let else_body = self.parse_value_block()?;
        let end = else_body.span.end;
        // '$' is not accepted in source identifiers, so this cannot collide
        // with a user-authored binding.
        let name = format!("$tail_value_{}", self.next_synthetic_binding);
        self.next_synthetic_binding += 1;
        Ok((
            Statement::new(
                StatementKind::If {
                    name: Some(name.clone()),
                    ty: None,
                    condition,
                    temperature: BlockTemperature::Default,
                    then_body,
                    else_body: Some(else_body),
                },
                SourceSpan { start, end },
            ),
            Expression {
                kind: ExpressionKind::Name(name),
                span: SourceSpan { start, end },
            },
        ))
    }

    fn parse_match(&mut self) -> Result<Statement, Diagnostic> {
        let start = self.consume_keyword("match")?.span.start;
        if self.current().kind != TokenKind::LeftParen {
            let value = self.parse_expression()?;
            self.consume(TokenKind::LeftBrace, "'{'")?;

            if !self.at_keyword("Value") {
                let mut arms = Vec::new();
                let mut style = None;
                while !self.at_keyword("_") {
                    let pattern = self.parse_scalar_match_pattern()?;
                    self.consume(TokenKind::FatArrow, "'=>'")?;
                    let temperature = self.parse_block_temperature()?;
                    let current_style = if self.current().kind == TokenKind::LeftBrace {
                        ScalarMatchArmStyle::Block
                    } else {
                        ScalarMatchArmStyle::Target
                    };
                    if style.is_some_and(|style| style != current_style) {
                        return self.error("scalar match arms must use the same body style");
                    }
                    style = Some(current_style);
                    if current_style == ScalarMatchArmStyle::Target && temperature != BlockTemperature::Default {
                        return self.error("a target match arm cannot have a block layout annotation");
                    }
                    let body = if current_style == ScalarMatchArmStyle::Block {
                        self.parse_block()?
                    } else {
                        let (target, span) = self.consume_identifier("match arm label or closure")?;
                        Self::jump_block(target, span)
                    };
                    arms.push(ScalarMatchArm {
                        pattern,
                        temperature,
                        body,
                    });
                    self.consume(TokenKind::Comma, "','")?;
                }
                if arms.is_empty() {
                    return self.error("expected scalar match arm before fallback");
                }
                self.advance();
                self.consume(TokenKind::FatArrow, "'=>'")?;
                let fallback =
                    if style == Some(ScalarMatchArmStyle::Target) && self.current().kind == TokenKind::LeftBrace {
                        self.parse_block()?
                    } else {
                        let (target, span) = self.consume_identifier("fallback label or closure")?;
                        Self::jump_block(target, span)
                    };
                if self.current().kind == TokenKind::Comma {
                    self.advance();
                }
                let end = self.consume(TokenKind::RightBrace, "'}'")?.span.end;
                return Ok(Statement::new(
                    StatementKind::ScalarMatch { value, arms, fallback },
                    SourceSpan { start, end },
                ));
            }

            let (arms, fallback) = self.parse_value_match_arms(false)?;
            let end = self.consume(TokenKind::RightBrace, "'}'")?.span.end;
            return Ok(Statement::new(
                StatementKind::ValueMatch {
                    name: None,
                    value,
                    arms,
                    fallback,
                },
                SourceSpan { start, end },
            ));
        }

        self.error("pair matches are not supported; use nested matches")
    }

    fn jump_block(target: String, span: SourceSpan) -> Block {
        Block {
            span,
            statements: vec![Statement::new(
                StatementKind::ContinuationJump {
                    target,
                    arguments: Vec::new(),
                },
                span,
            )],
            value: None,
        }
    }

    fn parse_scalar_match_pattern(&mut self) -> Result<Pattern, Diagnostic> {
        let mut alternatives = Vec::new();
        Self::collect_scalar_match_alternatives(self.parse_expression()?, &mut alternatives);
        Ok(if alternatives.len() == 1 {
            Pattern::Expression(alternatives.pop().unwrap())
        } else {
            Pattern::Alternatives(alternatives.into_iter().map(Pattern::Expression).collect())
        })
    }

    fn collect_scalar_match_alternatives(expression: Expression, alternatives: &mut Vec<Expression>) {
        let Expression { kind, span } = expression;
        if let ExpressionKind::Binary {
            operator: BinaryOperator::BitwiseOr,
            lhs,
            rhs,
        } = kind
        {
            Self::collect_scalar_match_alternatives(*lhs, alternatives);
            Self::collect_scalar_match_alternatives(*rhs, alternatives);
        } else {
            alternatives.push(Expression { kind, span });
        }
    }

    fn parse_value_match_pattern(&mut self) -> Result<(String, Option<String>), Diagnostic> {
        self.consume_keyword("Value")?;
        self.consume(TokenKind::LeftAngle, "'<'")?;
        let (representation, _) = self.consume_identifier("Value representation")?;
        self.consume(TokenKind::RightAngle, "'>'")?;
        if self.current().kind != TokenKind::LeftParen {
            return Ok((representation, None));
        }
        self.consume(TokenKind::LeftParen, "'('")?;
        let (binding, _) = self.consume_identifier("pattern binding")?;
        self.consume(TokenKind::RightParen, "')'")?;
        Ok((representation, Some(binding)))
    }

    fn parse_value_match_arms(
        &mut self,
        value_producing: bool,
    ) -> Result<(Vec<ValueMatchArm>, ValueMatchFallback), Diagnostic> {
        let mut arms = Vec::new();
        while !self.at_keyword("_") {
            let (representation, binding) = self.parse_value_match_pattern()?;
            self.consume(TokenKind::FatArrow, "'=>'")?;
            let temperature = self.parse_block_temperature()?;
            let body = if value_producing {
                self.parse_value_block()?
            } else {
                self.parse_block()?
            };
            arms.push(ValueMatchArm {
                representation,
                binding,
                temperature,
                body,
            });
            self.consume(TokenKind::Comma, "','")?;
        }
        let fallback = self.parse_value_match_fallback()?;
        Ok((arms, fallback))
    }

    fn parse_value_match_fallback(&mut self) -> Result<ValueMatchFallback, Diagnostic> {
        let (wildcard, _) = self.consume_identifier("'_' pattern")?;
        if wildcard != "_" {
            return self.error("expected '_' pattern");
        }
        self.consume(TokenKind::FatArrow, "'=>'")?;
        let temperature = self.parse_block_temperature()?;
        let fallback = if self.current().kind == TokenKind::LeftBrace {
            ValueMatchFallback {
                body: self.parse_block()?,
                temperature,
            }
        } else {
            if temperature != BlockTemperature::Default {
                return self.error("a target match fallback cannot have a block layout annotation");
            }
            let (target, span) = self.consume_identifier("fallback label or closure")?;
            ValueMatchFallback {
                body: Self::jump_block(target, span),
                temperature: BlockTemperature::Default,
            }
        };
        if self.current().kind == TokenKind::Comma {
            self.advance();
        }
        Ok(fallback)
    }

    fn parse_expression(&mut self) -> Result<Expression, Diagnostic> {
        self.parse_binary_expression(0)
    }

    fn parse_binary_expression(&mut self, minimum_precedence: u8) -> Result<Expression, Diagnostic> {
        let mut expression = self.parse_unary_expression()?;
        while let Some(precedence) = self.infix_precedence()
            && precedence >= minimum_precedence
        {
            let operator = self.consume_infix_operator(precedence)?;
            if let InfixOperator::ValueTypeTest(operator) = operator {
                expression = Expression {
                    span: SourceSpan {
                        start: expression.span.start,
                        end: self.current().span.start,
                    },
                    kind: ExpressionKind::Unary {
                        operator,
                        operand: Box::new(expression),
                    },
                };
                continue;
            }
            let InfixOperator::Binary(operator) = operator else {
                unreachable!()
            };
            let rhs_precedence = if matches!(operator, BinaryOperator::BitTest { .. }) {
                6
            } else {
                precedence + 1
            };
            let rhs = self.parse_binary_expression(rhs_precedence)?;
            let span = SourceSpan {
                start: expression.span.start,
                end: rhs.span.end,
            };
            expression = Expression {
                kind: ExpressionKind::Binary {
                    operator,
                    lhs: Box::new(expression),
                    rhs: Box::new(rhs),
                },
                span,
            };
        }
        Ok(expression)
    }

    fn infix_precedence(&self) -> Option<u8> {
        Some(match self.current().kind {
            TokenKind::DoubleAmpersand => 1,
            TokenKind::DoubleEquals => 2,
            TokenKind::Bang if matches!(self.peek(1).kind, TokenKind::Equals | TokenKind::DoubleEquals) => 2,
            TokenKind::Identifier(_) if self.at_keyword("has") || self.at_keyword("lacks") || self.at_keyword("is") => {
                2
            }
            TokenKind::Pipe => 3,
            TokenKind::Caret => 4,
            TokenKind::Ampersand => 5,
            TokenKind::LeftAngle | TokenKind::RightAngle if self.peek(1).kind == self.current().kind => 7,
            TokenKind::LeftAngle | TokenKind::RightAngle => 6,
            TokenKind::Plus | TokenKind::Minus => 8,
            TokenKind::Star | TokenKind::Slash | TokenKind::Percent => 9,
            _ => return None,
        })
    }

    fn consume_infix_operator(&mut self, precedence: u8) -> Result<InfixOperator, Diagnostic> {
        let binary = |operator| Ok(InfixOperator::Binary(operator));
        if self.at_keyword("has") || self.at_keyword("lacks") {
            let set = self.at_keyword("has");
            self.advance();
            return binary(BinaryOperator::BitTest { set });
        }
        if self.at_keyword("is") {
            self.advance();
            let negated = self.at_keyword("not");
            if negated {
                self.advance();
            }
            if self.at_keyword("Value") && self.peek(1).kind == TokenKind::LeftAngle {
                self.advance();
                self.consume(TokenKind::LeftAngle, "'<'")?;
                let (type_name, _) = self.consume_identifier("Value representation type")?;
                self.consume(TokenKind::RightAngle, "'>'")?;
                return Ok(InfixOperator::ValueTypeTest(UnaryOperator::ValueTypeTest {
                    type_name,
                    negated,
                }));
            }
            return binary(BinaryOperator::Identity { negated });
        }
        let token = self.advance().kind;
        binary(match (precedence, token.clone()) {
            (1, TokenKind::DoubleAmpersand) => BinaryOperator::LogicalAnd,
            (2, TokenKind::DoubleEquals) => BinaryOperator::LooseEqual,
            (2, TokenKind::Bang) => {
                if self.current().kind == TokenKind::DoubleEquals {
                    return self.error("'!==' is not supported");
                }
                self.advance();
                BinaryOperator::LooseNotEqual
            }
            (3, TokenKind::Pipe) => BinaryOperator::BitwiseOr,
            (4, TokenKind::Caret) => BinaryOperator::BitwiseXor,
            (5, TokenKind::Ampersand) => BinaryOperator::BitwiseAnd,
            (6, TokenKind::LeftAngle | TokenKind::RightAngle) => {
                let or_equal = self.current().kind == TokenKind::Equals;
                if or_equal {
                    self.advance();
                }
                match (token, or_equal) {
                    (TokenKind::LeftAngle, false) => BinaryOperator::LessThan,
                    (TokenKind::RightAngle, false) => BinaryOperator::GreaterThan,
                    (TokenKind::LeftAngle, true) => BinaryOperator::LessThanOrEqual,
                    (TokenKind::RightAngle, true) => BinaryOperator::GreaterThanOrEqual,
                    _ => unreachable!(),
                }
            }
            (7, TokenKind::LeftAngle | TokenKind::RightAngle) => {
                self.advance();
                if token == TokenKind::RightAngle && self.current().kind == TokenKind::RightAngle {
                    self.advance();
                    BinaryOperator::UnsignedRightShift
                } else if token == TokenKind::LeftAngle {
                    BinaryOperator::LeftShift
                } else {
                    BinaryOperator::RightShift
                }
            }
            (8, TokenKind::Plus) => BinaryOperator::Add,
            (8, TokenKind::Minus) => BinaryOperator::Subtract,
            (9, TokenKind::Star) => BinaryOperator::Multiply,
            (9, TokenKind::Slash) => BinaryOperator::Divide,
            (9, TokenKind::Percent) => BinaryOperator::Modulo,
            _ => unreachable!("infix precedence and token must agree"),
        })
    }

    fn parse_unary_expression(&mut self) -> Result<Expression, Diagnostic> {
        if !matches!(
            self.current().kind,
            TokenKind::Minus | TokenKind::Bang | TokenKind::Tilde | TokenKind::Ampersand
        ) {
            return self.parse_postfix_expression();
        }
        let operator = self.advance();
        let start = operator.span.start;
        let operand = self.parse_unary_expression()?;
        let end = operand.span.end;
        Ok(Expression {
            kind: ExpressionKind::Unary {
                operator: match operator.kind {
                    TokenKind::Minus => UnaryOperator::Negate,
                    TokenKind::Bang => UnaryOperator::LogicalNot,
                    TokenKind::Tilde => UnaryOperator::BitwiseNot,
                    TokenKind::Ampersand => UnaryOperator::Reference,
                    _ => unreachable!(),
                },
                operand: Box::new(operand),
            },
            span: SourceSpan { start, end },
        })
    }

    fn parse_postfix_expression(&mut self) -> Result<Expression, Diagnostic> {
        let mut expression = self.parse_primary_expression()?;
        while self.current().kind == TokenKind::LeftBracket {
            self.advance();
            let index = self.parse_expression()?;
            let end = self.consume(TokenKind::RightBracket, "']'")?.span.end;
            let start = expression.span.start;
            expression = Expression {
                kind: ExpressionKind::Index {
                    base: Box::new(expression),
                    index: Box::new(index),
                },
                span: SourceSpan { start, end },
            };
        }
        Ok(expression)
    }

    fn parse_primary_expression(&mut self) -> Result<Expression, Diagnostic> {
        let token = self.advance();
        let start = token.span.start;
        match token.kind {
            TokenKind::LeftParen => {
                let first = self.parse_expression()?;
                if self.current().kind != TokenKind::Comma {
                    let end = self.consume(TokenKind::RightParen, "')'")?.span.end;
                    return Ok(Expression {
                        kind: first.kind,
                        span: SourceSpan { start, end },
                    });
                }
                let mut elements = vec![first];
                while self.current().kind == TokenKind::Comma {
                    self.advance();
                    elements.push(self.parse_expression()?);
                }
                let end = self.consume(TokenKind::RightParen, "')'")?.span.end;
                Ok(Expression {
                    kind: ExpressionKind::Tuple(elements),
                    span: SourceSpan { start, end },
                })
            }
            TokenKind::LeftBracket => {
                let mut components = Vec::new();
                while self.current().kind != TokenKind::RightBracket {
                    components.push(self.parse_primary_expression()?);
                    if self.current().kind != TokenKind::Comma {
                        break;
                    }
                    self.advance();
                }
                if !(1..=3).contains(&components.len()) {
                    return self.error("a memory operand requires one to three components");
                }
                let end = self.consume(TokenKind::RightBracket, "']'")?.span.end;
                Ok(Expression {
                    kind: ExpressionKind::Memory(components),
                    span: SourceSpan { start, end },
                })
            }
            TokenKind::Minus => {
                let integer = self.advance();
                let TokenKind::Integer(value) = integer.kind else {
                    return self.error("expected an integer literal after '-'");
                };
                Ok(Expression {
                    kind: ExpressionKind::Integer(-value),
                    span: SourceSpan {
                        start,
                        end: integer.span.end,
                    },
                })
            }
            TokenKind::Integer(value) => Ok(Expression {
                kind: ExpressionKind::Integer(value),
                span: token.span,
            }),
            TokenKind::Identifier(name) => {
                if name == "Value" && self.current().kind == TokenKind::LeftAngle {
                    self.advance();
                    let (representation, _) = self.consume_identifier("value representation")?;
                    let end = self.consume(TokenKind::RightAngle, "'>'")?.span.end;
                    return Ok(Expression {
                        kind: ExpressionKind::Name(format!("Value<{representation}>")),
                        span: SourceSpan { start, end },
                    });
                }
                if self.current().kind != TokenKind::LeftParen {
                    return Ok(Expression {
                        kind: ExpressionKind::Name(name),
                        span: token.span,
                    });
                }
                self.advance();
                let mut arguments = Vec::new();
                while self.current().kind != TokenKind::RightParen {
                    arguments.push(self.parse_expression()?);
                    if self.current().kind != TokenKind::Comma {
                        break;
                    }
                    self.advance();
                }
                let end = self.consume(TokenKind::RightParen, "')'")?.span.end;
                Ok(Expression {
                    kind: ExpressionKind::Call {
                        callee: name,
                        arguments,
                    },
                    span: SourceSpan { start, end },
                })
            }
            _ => self.error("expected expression"),
        }
    }
}

pub(crate) fn parse(filename: &str, source: &str) -> Result<Program, Diagnostic> {
    let tokens = Lexer::new(filename, source).lex()?;
    Parser::new(filename, tokens).parse_program()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_declaration(source: &str, index: usize) -> Declaration {
        parse("test.flap", source).unwrap().declarations.remove(index)
    }

    fn parse_handler(source: &str) -> HandlerDeclaration {
        let Declaration::Handler(handler) = parse_declaration(source, 0) else {
            panic!("expected handler declaration")
        };
        handler
    }

    fn parse_inline_function(source: &str) -> InlineFunctionDeclaration {
        let Declaration::InlineFunction(function) = parse_declaration(source, 0) else {
            panic!("expected inline function declaration")
        };
        function
    }

    #[test]
    fn rejects_removed_syntax() {
        for source in [
            "handler Check(value: u32) { match value { 0..4 => { dispatch_next; }, _ => slow, } let slow = || { dispatch_next; }; }",
            "handler Check(lhs: u32, rhs: u32) { guard lhs === rhs else slow; let slow = || { dispatch_next; }; }",
            "handler Check(lhs: u32, rhs: u32) { guard lhs !== rhs else slow; let slow = || { dispatch_next; }; }",
            "handler Check() { { dispatch_next; } }",
            "handler Check(value: u32) { let result: u32 = if value == 0 { 1 } else { 2 }; dispatch_next; }",
        ] {
            assert!(parse("test.flap", source).is_err(), "{source}");
        }
    }

    #[test]
    fn parses_typed_declarations_and_refinement() {
        let program = parse(
            "interpreter.flap",
            r#"
inline fn check(value: in Value, result: out i32, fail: Label) {
    guard let Value<i32>(result) = value else fail;
}

handler Add(dst: out Operand, lhs: in Operand) {
    let value: Value = load_operand(lhs);
    guard let Value<i32>(integer) = value else .slow;
    store_operand(dst, value);
    dispatch_next;
}

handler Mod(lhs: Value, rhs: Value) {
    guard let Value<i32>(lhs_integer) = lhs else slow;
    guard let Value<i32>(rhs_integer) = rhs else slow;
    dispatch_next;
    let slow = || { dispatch_next; };
}
"#,
        )
        .unwrap();

        assert_eq!(program.declarations.len(), 3);
        let Declaration::InlineFunction(declaration) = &program.declarations[0] else {
            panic!("expected inline function declaration")
        };
        assert_eq!(declaration.parameters[0].mode, ParameterMode::In);
        assert_eq!(declaration.parameters[1].mode, ParameterMode::Out);
        assert!(matches!(
            declaration.body.statements[0].kind,
            StatementKind::GuardLet {
                pattern: Pattern::ValueRepresentation { ref representation, .. },
                ..
            } if representation == "i32"
        ));
        let Declaration::Handler(declaration) = &program.declarations[1] else {
            panic!("expected handler declaration")
        };
        assert!(matches!(
            declaration.body.statements[1].kind,
            StatementKind::GuardLet {
                pattern: Pattern::ValueRepresentation { ref representation, .. },
                ..
            } if representation == "i32"
        ));
    }

    #[test]
    fn rejects_legacy_declaration_and_binding_syntax() {
        let cases = [
            ("macro check() {}", "expected 'inline fn' or 'handler'"),
            (
                "handler Check(value: Value) { let Value<i32>(integer) = value else .slow; }",
                "refutable bindings use 'guard let'",
            ),
            (
                "handler Check(value: i32) { let result = checked_sub(value, value) else .slow; }",
                "fallible bindings use 'guard let'",
            ),
            (
                "handler Check(value: Value) { if let Int32(integer) = value using tag else .slow; }",
                "expected '{'",
            ),
            (
                "handler Pair(value: u32) { let (first, second) = pair(value); }",
                "tuple return bindings use '[...]'",
            ),
        ];
        for (source, message) in cases {
            assert_eq!(parse("test.flap", source).unwrap_err().message, message);
        }
    }

    #[test]
    fn parses_expression_bodied_handlers() {
        let handler = parse_handler("handler Exp() @cold = call_slow_path(exp);");

        assert_eq!(handler.temperature, BlockTemperature::Cold);
        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::Expression(Expression {
                kind: ExpressionKind::Call { callee, .. },
                ..
            }) if callee == "call_slow_path"
        ));
    }

    #[test]
    fn parses_parameterized_continuations() {
        let handler = parse_handler(
            "handler Box(value: i32) { let finish = |raw: i32| @hot { dispatch_next; }; guard value == 0 else finish(value); goto finish(value); }",
        );

        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::Continuation { temperature: BlockTemperature::Hot, parameters, .. }
                if parameters.len() == 1 && parameters[0].name == "raw" && parameters[0].ty == Type::I32
        ));
        assert!(matches!(
            &handler.body.statements[2].kind,
            StatementKind::ContinuationJump { target, arguments }
                if target == "finish" && arguments.len() == 1
        ));
        assert!(matches!(
            &handler.body.statements[1].kind,
            StatementKind::Guard {
                failure: ElseContinuation::Invocation { target, arguments },
                ..
            } if target == "finish" && arguments.len() == 1
        ));
    }

    #[test]
    fn reports_source_location_for_unknown_types() {
        let error = parse("bad.flap", "handler Add(value: Mystery) { dispatch_next; }").unwrap_err();

        assert_eq!(error.to_string(), "bad.flap:1:20: error: unknown type 'Mystery'");
    }

    #[test]
    fn parses_sized_scalar_types() {
        let declaration = parse_inline_function(
            "inline fn scalars(a: i8, b: i16, c: i32, d: i64, e: u8, f: u16, g: u32, h: u64, i: f32, j: f64) {}",
        );
        let types: Vec<_> = declaration
            .parameters
            .iter()
            .map(|parameter| parameter.ty.clone())
            .collect();

        assert_eq!(
            types,
            vec![
                Type::I8,
                Type::I16,
                Type::I32,
                Type::I64,
                Type::U8,
                Type::U16,
                Type::U32,
                Type::U64,
                Type::F32,
                Type::F64,
            ]
        );
    }

    #[test]
    fn parses_tuple_returns_and_bindings() {
        let program = parse(
            "tuple.flap",
            "inline fn pair(narrow: u32, wide: u64) -> (u32, u64) { (narrow, wide) }\n\
             handler Use(narrow: u32, wide: u64) { let [a, b] = pair(narrow, wide); dispatch_next; }",
        )
        .unwrap();
        let Declaration::InlineFunction(function) = &program.declarations[0] else {
            panic!("expected inline function declaration")
        };
        assert_eq!(function.return_type, Some(Type::Tuple(vec![Type::U32, Type::U64])));
        assert!(matches!(
            function.body.value.as_ref().map(|value| &value.kind),
            Some(ExpressionKind::Tuple(elements)) if elements.len() == 2
        ));
        let Declaration::Handler(handler) = &program.declarations[1] else {
            panic!("expected handler declaration")
        };
        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::Let {
                pattern: Pattern::Tuple(patterns),
                ..
            } if matches!(patterns.as_slice(), [Pattern::Binding { name: a, .. }, Pattern::Binding { name: b, .. }] if a == "a" && b == "b")
        ));
    }

    #[test]
    fn parses_boolean_first_value_matches_with_hot_fallbacks() {
        let handler = parse_handler(
            "handler Branch(value: Value) { match value { Value<bool>(boolean) => { dispatch_next; }, Value<i32> => { dispatch_next; }, _ => { dispatch_next; }, } }",
        );
        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::ValueMatch {
                arms,
                fallback: ValueMatchFallback { temperature: BlockTemperature::Default, .. },
                ..
            } if matches!(
                arms.first(),
                Some(ValueMatchArm {
                    representation,
                    binding: Some(binding),
                    ..
                }) if representation == "bool" && binding == "boolean"
            )
        ));
    }

    #[test]
    fn parses_block_layout_annotations_after_match_arrows() {
        let handler = parse_handler(
            "handler Choose(kind: u8) { match kind { KIND_FAST => @hot { dispatch_next; }, _ => slow, } let slow = || { dispatch_next; }; }",
        );

        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::ScalarMatch { arms, .. }
                if matches!(
                    arms.first(),
                    Some(ScalarMatchArm {
                        temperature: BlockTemperature::Hot,
                        ..
                    })
                )
        ));
    }

    #[test]
    fn parses_cold_value_result_arms() {
        let function = parse_inline_function(
            r#"
inline fn convert(value: Value, else failure: Label) -> i32 {
    match value {
        Value<i32> => { 0 },
        Value<f64> => @cold { 0 },
        _ => failure,
    }
}
"#,
        );
        assert!(matches!(
            &function.body.statements[0].kind,
            StatementKind::ValueMatch { arms, .. }
                if arms.iter().any(|arm| {
                    arm.representation == "f64"
                        && arm.temperature == BlockTemperature::Cold
                })
        ));
    }

    #[test]
    fn parses_double_first_value_matches() {
        let handler = parse_handler(
            "handler Abs(value: Value) { match value { Value<f64> => { dispatch_next; }, Value<i32>(integer) => { dispatch_next; }, _ => @cold { exit; }, } }",
        );
        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::ValueMatch { arms, .. }
                if matches!(
                    arms.as_slice(),
                    [
                        ValueMatchArm { representation: first, .. },
                        ValueMatchArm {
                            representation: second,
                            binding: Some(binding),
                            ..
                        },
                    ] if first == "f64" && second == "i32" && binding == "integer"
                )
        ));
    }

    #[test]
    fn parses_two_armed_if_statements() {
        let handler =
            parse_handler("handler Choose(value: u32) { if value == 0 { dispatch_next; } else { dispatch_next; } }");

        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::If {
                then_body,
                else_body: Some(else_body),
                ..
            }
                if then_body.statements.len() == 1 && else_body.statements.len() == 1
        ));
    }

    #[test]
    fn parses_value_if_as_an_inline_function_tail_expression() {
        let declaration =
            parse_inline_function("inline fn max(lhs: u32, rhs: u32) -> u32 { if lhs >= rhs { lhs } else { rhs } }");

        assert!(matches!(
            declaration.body.statements.as_slice(),
            [Statement {
                kind: StatementKind::If {
                    name: Some(_),
                    then_body,
                    else_body: Some(else_body),
                    ..
                },
                ..
            }] if then_body.value.is_some() && else_body.value.is_some()
        ));
        assert!(matches!(
            &declaration.body.value,
            Some(Expression { kind: ExpressionKind::Name(name), .. }) if name.starts_with("$tail_value_")
        ));
    }

    #[test]
    fn parses_value_match_as_an_inline_function_tail_expression() {
        let declaration = parse_inline_function(
            "inline fn number(value: Value, else failure: Label) -> f64 { match value { Value<i32>(integer) => { to_f64(integer) }, Value<f64>(number) => { number }, _ => failure, } }",
        );

        assert!(matches!(
            declaration.body.statements.as_slice(),
            [Statement {
                kind: StatementKind::ValueMatch { arms, .. },
                ..
            }] if matches!(
                arms.as_slice(),
                [
                    ValueMatchArm { binding: Some(integer), .. },
                    ValueMatchArm { binding: Some(number), .. },
                ] if integer == "integer" && number == "number"
            )
        ));
    }

    #[test]
    fn parses_else_if_statements() {
        let handler = parse_handler(
            "handler Clamp(value: i32) { if value < 0 { value = 0; } else if value >= 255 { value = 255; } dispatch_next; }",
        );

        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::If { else_body: Some(else_body), .. }
                if matches!(else_body.statements.as_slice(), [Statement { kind: StatementKind::If { .. }, .. }])
        ));
    }

    #[test]
    fn parses_fallible_tuple_bindings() {
        let program = parse(
            "tuple.flap",
            "inline fn pair(value: u32, else failure: Label) -> (u32, u32) { (value, value) }\n\
             handler Use(value: u32) { guard let [a, b] = pair(value) else slow; let slow = || { dispatch_next; }; }",
        )
        .unwrap();
        let Declaration::Handler(handler) = &program.declarations[1] else {
            panic!("expected handler declaration")
        };
        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::GuardLet {
                pattern: Pattern::Tuple(patterns),
                initializer: Expression { kind: ExpressionKind::Call { callee, .. }, .. },
                ..
            } if patterns.len() == 2 && callee == "pair"
        ));
    }

    #[test]
    fn parses_typed_bytecode_fields() {
        let declaration = parse_handler("handler Call(argument_count: Field<u32>) { dispatch_next; }");

        assert_eq!(declaration.parameters[0].ty, Type::Field(Box::new(Type::U32)));
    }

    #[test]
    fn parses_refined_value_types() {
        let declaration = parse_inline_function(
            "inline fn box(object: Object) -> Value<Object> { let value: Value<Object>; mov(value, object); value }",
        );

        assert_eq!(declaration.return_type, Some(Type::Boxed(Box::new(Type::Object))));
    }

    #[test]
    fn parses_readable_integer_literals() {
        let tokens = Lexer::new("integers.flap", "0xffff_ffff 4_294_967_295").lex().unwrap();

        assert_eq!(tokens[0].kind, TokenKind::Integer(0xffff_ffff));
        assert_eq!(tokens[1].kind, TokenKind::Integer(0xffff_ffff));
    }

    #[test]
    fn parses_subtraction_and_checked_subtraction() {
        let handler = parse_handler(
            r#"
handler Sub(lhs: i32, rhs: i32) {
    guard let difference = checked_sub(lhs, rhs) else @cold { dispatch_next; };
    let negative_one: i32 = identity(-1);
    let result = difference-negative_one;
    dispatch_next;
}
"#,
        );

        assert!(matches!(
            &handler.body.statements[0].kind,
            StatementKind::GuardLet {
                initializer: Expression { kind: ExpressionKind::Call { callee, .. }, .. },
                ..
            } if callee == "checked_sub"
        ));
        assert!(matches!(
            handler.body.statements[2].kind,
            StatementKind::Let {
                initializer: Some(Expression {
                    kind: ExpressionKind::Binary {
                        operator: BinaryOperator::Subtract,
                        ..
                    },
                    ..
                }),
                ..
            }
        ));
    }

    #[test]
    fn reports_source_location_for_syntax_errors() {
        let error = parse("bad.flap", "handler Add(value: Value) { let result: i32 = value }").unwrap_err();

        assert_eq!(error.to_string(), "bad.flap:1:53: error: expected ';'");
    }

    #[test]
    fn parses_single_byte_mutations_without_panicking() {
        let seed = b"handler Add(lhs: i32, rhs: i32) { let value = lhs + rhs; assert_nonzero(value); dispatch_next; }";
        for index in 0..seed.len() {
            for replacement in [b' ', b'\n', b'{', b'}', b'(', b')', b';', b'0', b'a'] {
                let mut mutated = seed.to_vec();
                mutated[index] = replacement;
                let source = std::str::from_utf8(&mutated).unwrap();
                let _ = parse("mutated.flap", source);
            }
        }
    }

    #[test]
    fn binds_bitwise_arithmetic_before_comparisons() {
        let handler = parse_handler(
            "handler Check(flags: i32) { guard flags & 2 != 0 else slow; let slow = || { dispatch_next; }; }",
        );
        let StatementKind::Guard { condition, .. } = &handler.body.statements[0].kind else {
            panic!("expected guard statement")
        };

        assert!(matches!(
            condition.kind,
            ExpressionKind::Binary {
                operator: BinaryOperator::LooseNotEqual,
                ref lhs,
                ..
            } if matches!(
                lhs.kind,
                ExpressionKind::Binary {
                    operator: BinaryOperator::BitwiseAnd,
                    ..
                }
            )
        ));
    }
}
