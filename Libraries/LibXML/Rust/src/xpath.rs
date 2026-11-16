/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiles an XPath 1.0 expression into a flat program.
//!
//! The program is a table of expression nodes held as four parallel columns, a kind and three operands, plus side
//! tables for location steps, lists of node indices (function arguments, predicates) and numbers. Strings are
//! ranges of the expression's own text. Nothing in a program points at anything: evaluating one is walking arrays.
//!
//! | kind             | a              | b               | c            |
//! |------------------|----------------|-----------------|--------------|
//! | OR .. UNION      | left operand   | right operand   |              |
//! | NEGATE           | operand        |                 |              |
//! | NUMBER           | number index   |                 |              |
//! | STRING           | offset         | length          |              |
//! | VARIABLE         | offset         | length          |              |
//! | FUNCTION         | function       | first argument  | argument count |
//! | FILTER           | expression     | first predicate | predicate count |
//! | RELATIVE_PATH    |                | first step      | step count   |
//! | ABSOLUTE_PATH    |                | first step      | step count   |
//! | EXPRESSION_PATH  | expression     | first step      | step count   |
//!
//! "First argument" and "first predicate" are indices into the list table, whose entries are node indices. "First
//! step" is an index into the step table.

use crate::chars;

pub mod kind {
    pub const OR: u8 = 0;
    pub const AND: u8 = 1;
    pub const EQUAL: u8 = 2;
    pub const NOT_EQUAL: u8 = 3;
    pub const LESS: u8 = 4;
    pub const LESS_OR_EQUAL: u8 = 5;
    pub const GREATER: u8 = 6;
    pub const GREATER_OR_EQUAL: u8 = 7;
    pub const ADD: u8 = 8;
    pub const SUBTRACT: u8 = 9;
    pub const MULTIPLY: u8 = 10;
    pub const DIVIDE: u8 = 11;
    pub const MODULO: u8 = 12;
    pub const UNION: u8 = 13;
    pub const NEGATE: u8 = 14;
    pub const NUMBER: u8 = 15;
    pub const STRING: u8 = 16;
    pub const VARIABLE: u8 = 17;
    pub const FUNCTION: u8 = 18;
    pub const FILTER: u8 = 19;
    pub const RELATIVE_PATH: u8 = 20;
    pub const ABSOLUTE_PATH: u8 = 21;
    pub const EXPRESSION_PATH: u8 = 22;
}

pub mod axis {
    pub const ANCESTOR: u8 = 0;
    pub const ANCESTOR_OR_SELF: u8 = 1;
    pub const ATTRIBUTE: u8 = 2;
    pub const CHILD: u8 = 3;
    pub const DESCENDANT: u8 = 4;
    pub const DESCENDANT_OR_SELF: u8 = 5;
    pub const FOLLOWING: u8 = 6;
    pub const FOLLOWING_SIBLING: u8 = 7;
    pub const NAMESPACE: u8 = 8;
    pub const PARENT: u8 = 9;
    pub const PRECEDING: u8 = 10;
    pub const PRECEDING_SIBLING: u8 = 11;
    pub const SELF: u8 = 12;
}

pub mod test {
    /// A QName: the step's prefix (which may be empty) and local name.
    pub const NAME: u8 = 0;
    /// `*`
    pub const ANY_NAME: u8 = 1;
    /// `prefix:*`
    pub const ANY_NAME_IN_NAMESPACE: u8 = 2;
    /// `node()`
    pub const NODE: u8 = 3;
    /// `text()`
    pub const TEXT: u8 = 4;
    /// `comment()`
    pub const COMMENT: u8 = 5;
    /// `processing-instruction()`
    pub const PROCESSING_INSTRUCTION: u8 = 6;
    /// `processing-instruction('target')`: the target is the step's local name.
    pub const PROCESSING_INSTRUCTION_WITH_TARGET: u8 = 7;
}

pub mod function {
    pub const BOOLEAN: u32 = 0;
    pub const CEILING: u32 = 1;
    pub const CONCAT: u32 = 2;
    pub const CONTAINS: u32 = 3;
    pub const COUNT: u32 = 4;
    pub const FALSE: u32 = 5;
    pub const FLOOR: u32 = 6;
    pub const ID: u32 = 7;
    pub const LANG: u32 = 8;
    pub const LAST: u32 = 9;
    pub const LOCAL_NAME: u32 = 10;
    pub const NAME: u32 = 11;
    pub const NAMESPACE_URI: u32 = 12;
    pub const NORMALIZE_SPACE: u32 = 13;
    pub const NOT: u32 = 14;
    pub const NUMBER: u32 = 15;
    pub const POSITION: u32 = 16;
    pub const ROUND: u32 = 17;
    pub const STARTS_WITH: u32 = 18;
    pub const STRING: u32 = 19;
    pub const STRING_LENGTH: u32 = 20;
    pub const SUBSTRING: u32 = 21;
    pub const SUBSTRING_AFTER: u32 = 22;
    pub const SUBSTRING_BEFORE: u32 = 23;
    pub const SUM: u32 = 24;
    pub const TRANSLATE: u32 = 25;
    pub const TRUE: u32 = 26;
}

/// The core function library: each function's name, id, and the fewest and the most arguments it takes.
const FUNCTIONS: &[(&[u8], u32, usize, usize)] = &[
    (b"boolean", function::BOOLEAN, 1, 1),
    (b"ceiling", function::CEILING, 1, 1),
    (b"concat", function::CONCAT, 2, usize::MAX),
    (b"contains", function::CONTAINS, 2, 2),
    (b"count", function::COUNT, 1, 1),
    (b"false", function::FALSE, 0, 0),
    (b"floor", function::FLOOR, 1, 1),
    (b"id", function::ID, 1, 1),
    (b"lang", function::LANG, 1, 1),
    (b"last", function::LAST, 0, 0),
    (b"local-name", function::LOCAL_NAME, 0, 1),
    (b"name", function::NAME, 0, 1),
    (b"namespace-uri", function::NAMESPACE_URI, 0, 1),
    (b"normalize-space", function::NORMALIZE_SPACE, 0, 1),
    (b"not", function::NOT, 1, 1),
    (b"number", function::NUMBER, 0, 1),
    (b"position", function::POSITION, 0, 0),
    (b"round", function::ROUND, 1, 1),
    (b"starts-with", function::STARTS_WITH, 2, 2),
    (b"string", function::STRING, 0, 1),
    (b"string-length", function::STRING_LENGTH, 0, 1),
    (b"substring", function::SUBSTRING, 2, 3),
    (b"substring-after", function::SUBSTRING_AFTER, 2, 2),
    (b"substring-before", function::SUBSTRING_BEFORE, 2, 2),
    (b"sum", function::SUM, 1, 1),
    (b"translate", function::TRANSLATE, 3, 3),
    (b"true", function::TRUE, 0, 0),
];

const AXES: &[(&[u8], u8)] = &[
    (b"ancestor", axis::ANCESTOR),
    (b"ancestor-or-self", axis::ANCESTOR_OR_SELF),
    (b"attribute", axis::ATTRIBUTE),
    (b"child", axis::CHILD),
    (b"descendant", axis::DESCENDANT),
    (b"descendant-or-self", axis::DESCENDANT_OR_SELF),
    (b"following", axis::FOLLOWING),
    (b"following-sibling", axis::FOLLOWING_SIBLING),
    (b"namespace", axis::NAMESPACE),
    (b"parent", axis::PARENT),
    (b"preceding", axis::PRECEDING),
    (b"preceding-sibling", axis::PRECEDING_SIBLING),
    (b"self", axis::SELF),
];

/// An expression nested deeper than this is refused, so that neither compiling nor evaluating one can run out of
/// stack.
const MAX_DEPTH: usize = 256;

/// One location step: `axis::test[predicate]...`
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Step {
    pub axis: u8,
    pub test: u8,
    /// The prefix of a name test, as a range of the expression. Empty if there is none.
    pub prefix_offset: u32,
    pub prefix_length: u32,
    /// The local name of a name test, or the target of a processing-instruction test.
    pub local_offset: u32,
    pub local_length: u32,
    /// The step's predicates, as a range of the list table.
    pub first_predicate: u32,
    pub predicate_count: u32,
}

#[derive(Default)]
pub struct Program {
    pub kinds: Vec<u8>,
    pub a: Vec<u32>,
    pub b: Vec<u32>,
    pub c: Vec<u32>,
    pub steps: Vec<Step>,
    pub lists: Vec<u32>,
    pub numbers: Vec<f64>,
    /// The node the whole expression is.
    pub root: u32,
}

#[derive(Debug, PartialEq)]
pub struct SyntaxError;

type R<T> = Result<T, SyntaxError>;

#[derive(Clone, Copy, PartialEq, Debug)]
enum Token {
    End,
    LeftParenthesis,
    RightParenthesis,
    LeftBracket,
    RightBracket,
    At,
    Comma,
    Pipe,
    Dot,
    DotDot,
    Slash,
    SlashSlash,
    Plus,
    Minus,
    /// An operator that is a node kind as it is: =, !=, <, <=, >, >=, *, div, mod, and, or.
    Operator(u8),
    Literal {
        start: u32,
        end: u32,
    },
    Number {
        start: u32,
        end: u32,
    },
    Variable {
        start: u32,
        end: u32,
    },
    Axis(u8),
    /// A node type name that a '(' follows: one of the `test` constants.
    NodeType(u8),
    /// A name that a '(' follows and that isn't a node type.
    FunctionName {
        start: u32,
        end: u32,
    },
    /// `*`, `prefix:*`, `name` or `prefix:name`. The prefix range is empty if there is none, and the local range is
    /// empty for the two wildcards.
    NameTest {
        test: u8,
        prefix: (u32, u32),
        local: (u32, u32),
    },
}

struct Lexer<'a> {
    source: &'a [u8],
    pos: usize,
    /// Whether the token before this one can end an operand. Only then are `*`, `and`, `or`, `div` and `mod`
    /// operators: after anything else they are names.
    after_operand: bool,
}

impl Lexer<'_> {
    fn at(&self, pos: usize) -> u8 {
        if pos < self.source.len() { self.source[pos] } else { 0 }
    }

    fn skip_spaces(&mut self) {
        while self.pos < self.source.len() && chars::is_space(self.source[self.pos]) {
            self.pos += 1;
        }
    }

    /// NCName ::= Name - (Char* ':' Char*). Returns its range, or None if there is no name at the cursor.
    fn name(&mut self) -> Option<(u32, u32)> {
        let start = self.pos;
        let mut pos = start;
        let mut first = true;
        while pos < self.source.len() {
            let byte = self.source[pos];
            let (is_part, length) = if byte < 0x80 {
                let class = if first { chars::NAME_START } else { chars::NAME };
                (byte != b':' && chars::CLASS[byte as usize] & class != 0, 1)
            } else {
                let (c, length) = chars::decode_utf8(self.source, pos)?;
                (
                    if first {
                        chars::is_name_start_char(c)
                    } else {
                        chars::is_name_char(c)
                    },
                    length,
                )
            };
            if !is_part {
                break;
            }
            pos += length;
            first = false;
        }
        if pos == start {
            return None;
        }
        self.pos = pos;
        Some((start as u32, pos as u32))
    }

    fn next(&mut self) -> R<Token> {
        let token = self.next_token()?;
        self.after_operand = matches!(
            token,
            Token::RightParenthesis
                | Token::RightBracket
                | Token::Dot
                | Token::DotDot
                | Token::Literal { .. }
                | Token::Number { .. }
                | Token::Variable { .. }
                | Token::NodeType(_)
                | Token::FunctionName { .. }
                | Token::NameTest { .. }
        );
        Ok(token)
    }

    fn next_token(&mut self) -> R<Token> {
        self.skip_spaces();
        if self.pos >= self.source.len() {
            return Ok(Token::End);
        }
        let byte = self.source[self.pos];
        let simple = |lexer: &mut Self, token: Token, length: usize| {
            lexer.pos += length;
            Ok(token)
        };
        match byte {
            b'(' => simple(self, Token::LeftParenthesis, 1),
            b')' => simple(self, Token::RightParenthesis, 1),
            b'[' => simple(self, Token::LeftBracket, 1),
            b']' => simple(self, Token::RightBracket, 1),
            b'@' => simple(self, Token::At, 1),
            b',' => simple(self, Token::Comma, 1),
            b'|' => simple(self, Token::Pipe, 1),
            b'+' => simple(self, Token::Plus, 1),
            b'-' => simple(self, Token::Minus, 1),
            b'=' => simple(self, Token::Operator(kind::EQUAL), 1),
            b'!' if self.at(self.pos + 1) == b'=' => simple(self, Token::Operator(kind::NOT_EQUAL), 2),
            b'<' if self.at(self.pos + 1) == b'=' => simple(self, Token::Operator(kind::LESS_OR_EQUAL), 2),
            b'<' => simple(self, Token::Operator(kind::LESS), 1),
            b'>' if self.at(self.pos + 1) == b'=' => simple(self, Token::Operator(kind::GREATER_OR_EQUAL), 2),
            b'>' => simple(self, Token::Operator(kind::GREATER), 1),
            b'/' if self.at(self.pos + 1) == b'/' => simple(self, Token::SlashSlash, 2),
            b'/' => simple(self, Token::Slash, 1),
            b'.' if self.at(self.pos + 1) == b'.' => simple(self, Token::DotDot, 2),
            b'.' if !self.at(self.pos + 1).is_ascii_digit() => simple(self, Token::Dot, 1),
            b'.' | b'0'..=b'9' => Ok(self.number()),
            b'"' | b'\'' => {
                let start = self.pos + 1;
                let length = self.source[start..]
                    .iter()
                    .position(|&other| other == byte)
                    .ok_or(SyntaxError)?;
                self.pos = start + length + 1;
                Ok(Token::Literal {
                    start: start as u32,
                    end: (start + length) as u32,
                })
            }
            b'*' if self.after_operand => simple(self, Token::Operator(kind::MULTIPLY), 1),
            b'*' => simple(
                self,
                Token::NameTest {
                    test: test::ANY_NAME,
                    prefix: (0, 0),
                    local: (0, 0),
                },
                1,
            ),
            b'$' => {
                self.pos += 1;
                let (start, mut end) = self.name().ok_or(SyntaxError)?;
                if self.at(self.pos) == b':' {
                    self.pos += 1;
                    end = self.name().ok_or(SyntaxError)?.1;
                }
                Ok(Token::Variable { start, end })
            }
            _ => self.name_token(),
        }
    }

    /// Number ::= Digits ('.' Digits?)? | '.' Digits
    fn number(&mut self) -> Token {
        let start = self.pos;
        let mut seen_dot = false;
        while self.pos < self.source.len() {
            match self.source[self.pos] {
                b'0'..=b'9' => {}
                b'.' if !seen_dot => seen_dot = true,
                _ => break,
            }
            self.pos += 1;
        }
        Token::Number {
            start: start as u32,
            end: self.pos as u32,
        }
    }

    /// Everything that starts with a name: an operator name, an axis, a node type, a function name, a name test.
    fn name_token(&mut self) -> R<Token> {
        let (start, end) = self.name().ok_or(SyntaxError)?;
        let name = &self.source[start as usize..end as usize];
        self.skip_spaces();
        if self.after_operand {
            match name {
                b"and" => return Ok(Token::Operator(kind::AND)),
                b"or" => return Ok(Token::Operator(kind::OR)),
                b"mod" => return Ok(Token::Operator(kind::MODULO)),
                b"div" => return Ok(Token::Operator(kind::DIVIDE)),
                _ => {}
            }
        }

        let mut prefix = (0, 0);
        let mut local = (start, end);
        if self.at(self.pos) == b':' {
            let adjacent = self.pos == end as usize;
            self.pos += 1;
            if self.at(self.pos) == b':' {
                self.pos += 1;
                let axis = AXES
                    .iter()
                    .find(|(axis_name, _)| *axis_name == name)
                    .ok_or(SyntaxError)?
                    .1;
                return Ok(Token::Axis(axis));
            }
            // A prefix, its colon and what follows are one token, with no room for white space in it.
            if !adjacent {
                return Err(SyntaxError);
            }
            prefix = (start, end);
            if self.at(self.pos) == b'*' {
                self.pos += 1;
                return Ok(Token::NameTest {
                    test: test::ANY_NAME_IN_NAMESPACE,
                    prefix,
                    local: (0, 0),
                });
            }
            local = self.name().ok_or(SyntaxError)?;
        }

        self.skip_spaces();
        if self.at(self.pos) == b'(' {
            // The '(' stays: it is the next token.
            if prefix.1 == prefix.0 {
                match name {
                    b"node" => return Ok(Token::NodeType(test::NODE)),
                    b"text" => return Ok(Token::NodeType(test::TEXT)),
                    b"comment" => return Ok(Token::NodeType(test::COMMENT)),
                    b"processing-instruction" => return Ok(Token::NodeType(test::PROCESSING_INSTRUCTION)),
                    _ => {}
                }
            }
            return Ok(Token::FunctionName { start, end: local.1 });
        }
        Ok(Token::NameTest {
            test: test::NAME,
            prefix,
            local,
        })
    }
}

struct Parser<'a> {
    lexer: Lexer<'a>,
    token: Token,
    program: Program,
    depth: usize,
}

/// Compiles `source`, an XPath 1.0 expression as UTF-8.
pub fn compile(source: &[u8]) -> R<Program> {
    if source.len() >= u32::MAX as usize {
        return Err(SyntaxError);
    }
    let mut parser = Parser {
        lexer: Lexer {
            source,
            pos: 0,
            after_operand: false,
        },
        token: Token::End,
        program: Program::default(),
        depth: 0,
    };
    parser.advance()?;
    let root = parser.expression()?;
    if parser.token != Token::End {
        return Err(SyntaxError);
    }
    parser.program.root = root;
    Ok(parser.program)
}

impl Parser<'_> {
    fn advance(&mut self) -> R<()> {
        self.token = self.lexer.next()?;
        Ok(())
    }

    fn expect(&mut self, token: Token) -> R<()> {
        if self.token != token {
            return Err(SyntaxError);
        }
        self.advance()
    }

    fn node(&mut self, kind: u8, a: u32, b: u32, c: u32) -> u32 {
        let index = self.program.kinds.len() as u32;
        self.program.kinds.push(kind);
        self.program.a.push(a);
        self.program.b.push(b);
        self.program.c.push(c);
        index
    }

    /// Appends `nodes` to the list table and returns where they start.
    fn list(&mut self, nodes: &[u32]) -> u32 {
        let start = self.program.lists.len() as u32;
        self.program.lists.extend_from_slice(nodes);
        start
    }

    /// Expr ::= OrExpr
    fn expression(&mut self) -> R<u32> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return Err(SyntaxError);
        }
        let result = self.binary(0);
        self.depth -= 1;
        result
    }

    /// The binary operators, from loosest to tightest. All of them associate to the left.
    ///
    /// OrExpr ::= AndExpr ('or' AndExpr)*, and so on down to MultiplicativeExpr ::= UnaryExpr (MultiplyOperator
    /// UnaryExpr)*
    fn binary(&mut self, level: usize) -> R<u32> {
        const LEVELS: &[&[u8]] = &[
            &[kind::OR],
            &[kind::AND],
            &[kind::EQUAL, kind::NOT_EQUAL],
            &[kind::LESS, kind::LESS_OR_EQUAL, kind::GREATER, kind::GREATER_OR_EQUAL],
            &[kind::ADD, kind::SUBTRACT],
            &[kind::MULTIPLY, kind::DIVIDE, kind::MODULO],
        ];
        if level == LEVELS.len() {
            return self.unary();
        }
        let mut left = self.binary(level + 1)?;
        loop {
            let operator = match self.token {
                Token::Operator(operator) => operator,
                Token::Plus => kind::ADD,
                Token::Minus => kind::SUBTRACT,
                _ => return Ok(left),
            };
            if !LEVELS[level].contains(&operator) {
                return Ok(left);
            }
            self.advance()?;
            let right = self.binary(level + 1)?;
            left = self.node(operator, left, right, 0);
        }
    }

    /// UnaryExpr ::= UnionExpr | '-' UnaryExpr
    fn unary(&mut self) -> R<u32> {
        let mut negations = 0usize;
        while self.token == Token::Minus {
            self.advance()?;
            negations += 1;
        }
        let mut result = self.union()?;
        // Two negations aren't nothing: each one also turns its operand into a number.
        for _ in 0..negations.min(2 - negations % 2) {
            result = self.node(kind::NEGATE, result, 0, 0);
        }
        Ok(result)
    }

    /// UnionExpr ::= PathExpr ('|' PathExpr)*
    fn union(&mut self) -> R<u32> {
        let mut left = self.path()?;
        while self.token == Token::Pipe {
            self.advance()?;
            let right = self.path()?;
            left = self.node(kind::UNION, left, right, 0);
        }
        Ok(left)
    }

    /// PathExpr ::= LocationPath | FilterExpr (('/' | '//') RelativeLocationPath)?
    fn path(&mut self) -> R<u32> {
        match self.token {
            Token::Variable { .. }
            | Token::LeftParenthesis
            | Token::Literal { .. }
            | Token::Number { .. }
            | Token::FunctionName { .. } => {
                let filter = self.filter()?;
                let mut steps = Vec::new();
                match self.token {
                    Token::Slash => self.advance()?,
                    Token::SlashSlash => {
                        self.advance()?;
                        steps.push(DESCENDANT_OR_SELF_NODE);
                    }
                    _ => return Ok(filter),
                }
                self.relative_path(&mut steps)?;
                Ok(self.path_node(kind::EXPRESSION_PATH, filter, steps))
            }
            Token::Slash => {
                self.advance()?;
                let mut steps = Vec::new();
                if self.at_step() {
                    self.relative_path(&mut steps)?;
                }
                Ok(self.path_node(kind::ABSOLUTE_PATH, 0, steps))
            }
            Token::SlashSlash => {
                self.advance()?;
                let mut steps = vec![DESCENDANT_OR_SELF_NODE];
                self.relative_path(&mut steps)?;
                Ok(self.path_node(kind::ABSOLUTE_PATH, 0, steps))
            }
            _ => {
                let mut steps = Vec::new();
                self.relative_path(&mut steps)?;
                Ok(self.path_node(kind::RELATIVE_PATH, 0, steps))
            }
        }
    }

    fn path_node(&mut self, kind: u8, expression: u32, mut steps: Vec<Step>) -> u32 {
        // OPTIMIZATION: "//name" is descendant-or-self::node()/child::name, which visits every node of the subtree
        // and then every child of each of them. With no predicate on the second step to tell the two apart, it
        // selects exactly what descendant::name does, in one walk.
        let mut index = 0;
        while index + 1 < steps.len() {
            let next = steps[index + 1];
            if steps[index] == DESCENDANT_OR_SELF_NODE && next.axis == axis::CHILD && next.predicate_count == 0 {
                steps[index] = Step {
                    axis: axis::DESCENDANT,
                    ..next
                };
                steps.remove(index + 1);
            }
            index += 1;
        }
        let first = self.program.steps.len() as u32;
        let count = steps.len() as u32;
        self.program.steps.extend(steps);
        self.node(kind, expression, first, count)
    }

    fn at_step(&self) -> bool {
        matches!(
            self.token,
            Token::Dot | Token::DotDot | Token::At | Token::Axis(_) | Token::NodeType(_) | Token::NameTest { .. }
        )
    }

    /// RelativeLocationPath ::= Step (('/' | '//') Step)*
    fn relative_path(&mut self, steps: &mut Vec<Step>) -> R<()> {
        loop {
            let step = self.step()?;
            steps.push(step);
            match self.token {
                Token::Slash => self.advance()?,
                Token::SlashSlash => {
                    self.advance()?;
                    steps.push(DESCENDANT_OR_SELF_NODE);
                }
                _ => return Ok(()),
            }
        }
    }

    /// Step ::= AxisSpecifier NodeTest Predicate* | '.' | '..'
    fn step(&mut self) -> R<Step> {
        let mut step = Step {
            axis: axis::CHILD,
            ..DESCENDANT_OR_SELF_NODE
        };
        match self.token {
            Token::Dot => {
                self.advance()?;
                step.axis = axis::SELF;
                return Ok(step);
            }
            Token::DotDot => {
                self.advance()?;
                step.axis = axis::PARENT;
                return Ok(step);
            }
            Token::At => {
                self.advance()?;
                step.axis = axis::ATTRIBUTE;
            }
            Token::Axis(axis) => {
                self.advance()?;
                step.axis = axis;
            }
            _ => {}
        }

        match self.token {
            Token::NameTest { test, prefix, local } => {
                self.advance()?;
                step.test = test;
                (step.prefix_offset, step.prefix_length) = (prefix.0, prefix.1 - prefix.0);
                (step.local_offset, step.local_length) = (local.0, local.1 - local.0);
            }
            Token::NodeType(test) => {
                self.advance()?;
                self.expect(Token::LeftParenthesis)?;
                step.test = test;
                if test == test::PROCESSING_INSTRUCTION
                    && let Token::Literal { start, end } = self.token
                {
                    self.advance()?;
                    step.test = test::PROCESSING_INSTRUCTION_WITH_TARGET;
                    (step.local_offset, step.local_length) = (start, end - start);
                }
                self.expect(Token::RightParenthesis)?;
            }
            _ => return Err(SyntaxError),
        }

        let predicates = self.predicates()?;
        step.first_predicate = self.list(&predicates);
        step.predicate_count = predicates.len() as u32;
        Ok(step)
    }

    /// Predicate* where Predicate ::= '[' Expr ']'
    fn predicates(&mut self) -> R<Vec<u32>> {
        let mut predicates = Vec::new();
        while self.token == Token::LeftBracket {
            self.advance()?;
            predicates.push(self.expression()?);
            self.expect(Token::RightBracket)?;
        }
        Ok(predicates)
    }

    /// FilterExpr ::= PrimaryExpr Predicate*
    fn filter(&mut self) -> R<u32> {
        let primary = self.primary()?;
        let predicates = self.predicates()?;
        if predicates.is_empty() {
            return Ok(primary);
        }
        let first = self.list(&predicates);
        Ok(self.node(kind::FILTER, primary, first, predicates.len() as u32))
    }

    /// PrimaryExpr ::= VariableReference | '(' Expr ')' | Literal | Number | FunctionCall
    fn primary(&mut self) -> R<u32> {
        match self.token {
            Token::Variable { start, end } => {
                self.advance()?;
                Ok(self.node(kind::VARIABLE, start, end - start, 0))
            }
            Token::LeftParenthesis => {
                self.advance()?;
                let expression = self.expression()?;
                self.expect(Token::RightParenthesis)?;
                Ok(expression)
            }
            Token::Literal { start, end } => {
                self.advance()?;
                Ok(self.node(kind::STRING, start, end - start, 0))
            }
            Token::Number { start, end } => {
                self.advance()?;
                let text =
                    std::str::from_utf8(&self.lexer.source[start as usize..end as usize]).map_err(|_| SyntaxError)?;
                let value: f64 = text.parse().map_err(|_| SyntaxError)?;
                let index = self.program.numbers.len() as u32;
                self.program.numbers.push(value);
                Ok(self.node(kind::NUMBER, index, 0, 0))
            }
            Token::FunctionName { start, end } => {
                self.advance()?;
                let name = &self.lexer.source[start as usize..end as usize];
                let &(_, id, fewest, most) = FUNCTIONS.iter().find(|(other, ..)| *other == name).ok_or(SyntaxError)?;
                self.expect(Token::LeftParenthesis)?;
                let mut arguments = Vec::new();
                if self.token != Token::RightParenthesis {
                    loop {
                        arguments.push(self.expression()?);
                        if self.token != Token::Comma {
                            break;
                        }
                        self.advance()?;
                    }
                }
                self.expect(Token::RightParenthesis)?;
                if arguments.len() < fewest || arguments.len() > most {
                    return Err(SyntaxError);
                }
                let first = self.list(&arguments);
                Ok(self.node(kind::FUNCTION, id, first, arguments.len() as u32))
            }
            _ => Err(SyntaxError),
        }
    }
}

/// What "//" stands for between two steps: descendant-or-self::node()
const DESCENDANT_OR_SELF_NODE: Step = Step {
    axis: axis::DESCENDANT_OR_SELF,
    test: test::NODE,
    prefix_offset: 0,
    prefix_length: 0,
    local_offset: 0,
    local_length: 0,
    first_predicate: 0,
    predicate_count: 0,
};

#[cfg(test)]
mod tests {
    use super::*;

    /// Writes a program back out as a fully parenthesized expression, so that tests can say what they expect.
    fn show(source: &str) -> String {
        let Ok(program) = compile(source.as_bytes()) else {
            return "error".to_string();
        };
        show_node(&program, source, program.root)
    }

    fn text(source: &str, offset: u32, length: u32) -> &str {
        &source[offset as usize..(offset + length) as usize]
    }

    fn show_list(
        program: &Program,
        source: &str,
        first: u32,
        count: u32,
        open: &str,
        separator: &str,
        close: &str,
    ) -> String {
        let items: Vec<String> = (first..first + count)
            .map(|index| show_node(program, source, program.lists[index as usize]))
            .collect();
        if count == 0 && open == "[" {
            String::new()
        } else {
            format!("{open}{}{close}", items.join(separator))
        }
    }

    fn show_steps(program: &Program, source: &str, first: u32, count: u32) -> String {
        let steps: Vec<String> = (first..first + count)
            .map(|index| {
                let step = &program.steps[index as usize];
                let axis = AXES.iter().find(|(_, id)| *id == step.axis).unwrap().0;
                let prefix = text(source, step.prefix_offset, step.prefix_length);
                let local = text(source, step.local_offset, step.local_length);
                let test = match step.test {
                    test::NAME if prefix.is_empty() => local.to_string(),
                    test::NAME => format!("{prefix}:{local}"),
                    test::ANY_NAME => "*".to_string(),
                    test::ANY_NAME_IN_NAMESPACE => format!("{prefix}:*"),
                    test::NODE => "node()".to_string(),
                    test::TEXT => "text()".to_string(),
                    test::COMMENT => "comment()".to_string(),
                    test::PROCESSING_INSTRUCTION => "processing-instruction()".to_string(),
                    _ => format!("processing-instruction('{local}')"),
                };
                let predicates = show_list(
                    program,
                    source,
                    step.first_predicate,
                    step.predicate_count,
                    "[",
                    "][",
                    "]",
                );
                format!("{}::{test}{predicates}", std::str::from_utf8(axis).unwrap())
            })
            .collect();
        steps.join("/")
    }

    fn show_node(program: &Program, source: &str, node: u32) -> String {
        let index = node as usize;
        let (a, b, c) = (program.a[index], program.b[index], program.c[index]);
        let binary = |operator: &str| {
            format!(
                "({} {operator} {})",
                show_node(program, source, a),
                show_node(program, source, b)
            )
        };
        match program.kinds[index] {
            kind::OR => binary("or"),
            kind::AND => binary("and"),
            kind::EQUAL => binary("="),
            kind::NOT_EQUAL => binary("!="),
            kind::LESS => binary("<"),
            kind::LESS_OR_EQUAL => binary("<="),
            kind::GREATER => binary(">"),
            kind::GREATER_OR_EQUAL => binary(">="),
            kind::ADD => binary("+"),
            kind::SUBTRACT => binary("-"),
            kind::MULTIPLY => binary("*"),
            kind::DIVIDE => binary("div"),
            kind::MODULO => binary("mod"),
            kind::UNION => binary("|"),
            kind::NEGATE => format!("-{}", show_node(program, source, a)),
            kind::NUMBER => format!("{}", program.numbers[a as usize]),
            kind::STRING => format!("'{}'", text(source, a, b)),
            kind::VARIABLE => format!("${}", text(source, a, b)),
            kind::FUNCTION => {
                let name = FUNCTIONS.iter().find(|(_, id, ..)| *id == a).unwrap().0;
                format!(
                    "{}{}",
                    std::str::from_utf8(name).unwrap(),
                    show_list(program, source, b, c, "(", ", ", ")")
                )
            }
            kind::FILTER => format!(
                "{}{}",
                show_node(program, source, a),
                show_list(program, source, b, c, "[", "][", "]")
            ),
            kind::RELATIVE_PATH => show_steps(program, source, b, c),
            kind::ABSOLUTE_PATH => format!("/{}", show_steps(program, source, b, c)),
            kind::EXPRESSION_PATH => format!(
                "{}/{}",
                show_node(program, source, a),
                show_steps(program, source, b, c)
            ),
            other => panic!("unexpected kind {other}"),
        }
    }

    #[test]
    fn paths() {
        assert_eq!(show("a"), "child::a");
        assert_eq!(show("a/b"), "child::a/child::b");
        assert_eq!(show("/"), "/");
        assert_eq!(show("/a"), "/child::a");
        assert_eq!(show("//a"), "/descendant::a");
        assert_eq!(show("//a[1]"), "/descendant-or-self::node()/child::a[1]");
        assert_eq!(show("a//b"), "child::a/descendant::b");
        assert_eq!(show(".//@id"), "self::node()/descendant-or-self::node()/attribute::id");
        assert_eq!(show(".."), "parent::node()");
        assert_eq!(show("@*"), "attribute::*");
        assert_eq!(show("p:a/p:*"), "child::p:a/child::p:*");
        assert_eq!(show("ancestor-or-self::node()"), "ancestor-or-self::node()");
        assert_eq!(show("text()|comment()"), "(child::text() | child::comment())");
        assert_eq!(
            show("processing-instruction(' x ')"),
            "child::processing-instruction(' x ')"
        );
        assert_eq!(show("a[b][@c='d']"), "child::a[child::b][(attribute::c = 'd')]");
        assert_eq!(show("id('a')/b"), "id('a')/child::b");
        assert_eq!(show("(a|b)[1]//c"), "(child::a | child::b)[1]/descendant::c");
        assert_eq!(show("$v/a"), "$v/child::a");
    }

    #[test]
    fn operators() {
        assert_eq!(show("1 + 2 * 3"), "(1 + (2 * 3))");
        assert_eq!(show("1 - 2 - 3"), "((1 - 2) - 3)");
        assert_eq!(show("a or b and c"), "(child::a or (child::b and child::c))");
        assert_eq!(show("1 < 2 = 3 > 4"), "((1 < 2) = (3 > 4))");
        assert_eq!(show("-1"), "-1");
        assert_eq!(show("--1"), "--1");
        assert_eq!(show("---1"), "-1");
        assert_eq!(show("- a | b"), "-(child::a | child::b)");
        assert_eq!(show("6 div 3 mod 2"), "((6 div 3) mod 2)");
        assert_eq!(show(".5 + 5."), "(0.5 + 5)");
        assert_eq!(show("1!=2"), "(1 != 2)");
        assert_eq!(show("1<=2"), "(1 <= 2)");
    }

    #[test]
    fn names_that_are_also_operators() {
        // Whether a name or a '*' is an operator depends only on the token before it.
        assert_eq!(show("div"), "child::div");
        assert_eq!(show("div div div"), "(child::div div child::div)");
        assert_eq!(show("and and and"), "(child::and and child::and)");
        assert_eq!(show("* * *"), "(child::* * child::*)");
        assert_eq!(show("mod/or"), "child::mod/child::or");
        assert_eq!(show("a[mod mod mod]"), "child::a[(child::mod mod child::mod)]");
        assert_eq!(show("text"), "child::text");
        assert_eq!(show("node/comment"), "child::node/child::comment");
        assert_eq!(show("1 or 2 or 3"), "((1 or 2) or 3)");
    }

    #[test]
    fn functions() {
        assert_eq!(show("true()"), "true()");
        assert_eq!(show("concat('a', \"b\", 1)"), "concat('a', 'b', 1)");
        assert_eq!(show("substring('abc', 2)"), "substring('abc', 2)");
        assert_eq!(show("count(//a) > 1"), "(count(/descendant::a) > 1)");
        assert_eq!(show("not(position() = last())"), "not((position() = last()))");
        assert_eq!(show("true(1)"), "error");
        assert_eq!(show("concat('a')"), "error");
        assert_eq!(show("nope()"), "error");
        assert_eq!(show("p:true()"), "error");
    }

    #[test]
    fn errors() {
        for source in [
            "", " ", "a b", "a/", "//", "a[", "a[]", "a]", "(a", "a)", "1 +", "+ 1", "'abc", "\"abc", "a::b",
            "child::", "@", "a | ", "!", "a ! b", "$", "$a:", "p:", "a:b:c", "1.2.3", "a[1", "f(", "true(,)", "/ /",
            "..a", "#", "a,b", "\u{b7}a", "p : a", "p: a", "p :a", "p: *", "p :*",
        ] {
            assert_eq!(show(source), "error", "{source:?}");
        }
        assert_eq!(show(&"(".repeat(MAX_DEPTH + 1)), "error");
        let deep = format!("{}1{}", "(".repeat(MAX_DEPTH - 1), ")".repeat(MAX_DEPTH - 1));
        assert_eq!(show(&deep), "1");
        let too_deep = format!("{}1{}", "(".repeat(MAX_DEPTH), ")".repeat(MAX_DEPTH));
        assert_eq!(show(&too_deep), "error");
    }

    #[test]
    fn white_space_and_unicode() {
        assert_eq!(show(" a / b [ 1 ] "), "child::a/child::b[1]");
        assert_eq!(show("child :: a"), "child::a");
        assert_eq!(
            show("\u{4e2d}\u{6587}/@\u{e5}"),
            "child::\u{4e2d}\u{6587}/attribute::\u{e5}"
        );
        assert_eq!(show("'\u{4e2d}'"), "'\u{4e2d}'");
        assert_eq!(show("a-b - c"), "(child::a-b - child::c)");
        assert_eq!(show("a.b"), "child::a.b");
    }
}
