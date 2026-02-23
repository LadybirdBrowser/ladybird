/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! JavaScript parser: recursive descent with precedence climbing.
//!
//! This is the core parser module. It contains the `Parser` struct (parser
//! state + helpers) and delegates actual parsing to submodules:
//!
//! - `expressions` — `parse_expression()`, primary/secondary expressions
//! - `statements` — `parse_statement()`, control flow
//! - `declarations` — functions, classes, variables, import/export
//!
//! ## How parsing works
//!
//! The parser is a single-pass, recursive-descent parser. Expression parsing
//! uses precedence climbing (Pratt-style): `parse_expression(min_precedence)`
//! parses a primary expression, then loops consuming binary/postfix operators
//! whose precedence is >= `min_precedence`.
//!
//! The parser reads tokens one at a time from the Lexer. The "current token"
//! is always available via `self.current_token`. Calling `consume()` returns
//! the current token and advances to the next one.
//!
//! ## Backtracking
//!
//! Some constructs require speculative parsing (e.g., arrow functions:
//! `(a, b) =>` looks like a parenthesized expression until `=>` is seen).
//! The parser supports this via `save_state()` / `load_state()`, which
//! save and restore the full parser state including lexer position, current
//! token, error list, and all boolean flags.

use std::collections::{HashMap, HashSet};

use std::rc::Rc;

use crate::ast::{
    BindingPattern, Expression, ExpressionKind, FunctionParameter, FunctionTable, Identifier,
    PrivateIdentifier, SourceRange, Statement, StatementKind, ScopeData, ProgramData, Utf16String,
};
use crate::lexer::{ch, Lexer};
use crate::scope_collector::{ScopeCollector, ScopeCollectorState};
use crate::token::{Token, TokenType};

mod declarations;
mod expressions;
mod statements;

pub use crate::ast::Position;
pub use crate::ast::DeclarationKind;
pub use crate::ast::FunctionKind;
pub use crate::ast::ProgramType;
pub use crate::ast::FunctionParsingInsights;

// Named precedence levels for parse_expression().
// These correspond to the operator precedence table in ECMA-262.
pub(crate) const PRECEDENCE_COMMA: i32 = 0;
pub(crate) const PRECEDENCE_ASSIGNMENT: i32 = 2;
pub(crate) const PRECEDENCE_UNARY: i32 = 17;
pub(crate) const PRECEDENCE_MEMBER: i32 = 19;

/// Result of parsing a function's formal parameter list.
pub struct ParsedParameters {
    pub parameters: Vec<FunctionParameter>,
    pub function_length: i32,
    pub parameter_info: Vec<ParamInfo>,
    pub is_simple: bool,
}

/// Information about a single parameter name binding.
pub struct ParamInfo {
    pub name: Utf16String,
    pub is_rest: bool,
    pub is_from_pattern: bool,
    pub identifier: Option<Rc<Identifier>>,
}

/// Result of parsing a property key (object literal or class element).
pub(crate) struct PropertyKey {
    pub expression: Expression,
    pub name: Option<Utf16String>,
    pub is_proto: bool,
    pub is_computed: bool,
    pub is_identifier: bool,
}

/// Method kind for parse_method_definition.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MethodKind {
    Normal,
    Getter,
    Setter,
    Constructor,
}

/// Associativity for operator precedence.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Associativity {
    Left,
    Right,
}

/// Tracks which tokens are forbidden in the current expression context.
///
/// This is threaded through `parse_expression()` to prevent ambiguity:
/// - `forbid_in`: in for-loop init position (`for (x in ...)` is for-in, not comparison)
/// - `forbid_logical/forbid_coalesce`: `&&`/`||` and `??` cannot be mixed without parens
/// - `forbid_paren_open`: prevents consuming `(` as call in `new Foo()` callee position
/// - `forbid_question_mark_period`: prevents `?.` in `new Foo?.bar`
/// - `forbid_equals`: prevents `=` from being consumed as assignment in certain contexts
#[derive(Clone, Copy, Default)]
pub struct ForbiddenTokens {
    pub forbid_in: bool,
    pub forbid_logical: bool,
    pub forbid_coalesce: bool,
    pub forbid_paren_open: bool,
    pub forbid_question_mark_period: bool,
    pub forbid_equals: bool,
}

impl ForbiddenTokens {
    pub fn none() -> Self {
        Self::default()
    }

    pub fn with_in() -> Self {
        Self { forbid_in: true, ..Self::default() }
    }

    pub fn allows(&self, token: TokenType) -> bool {
        match token {
            TokenType::In => !self.forbid_in,
            TokenType::DoubleAmpersand | TokenType::DoublePipe => !self.forbid_logical,
            TokenType::DoubleQuestionMark => !self.forbid_coalesce,
            TokenType::ParenOpen => !self.forbid_paren_open,
            TokenType::QuestionMarkPeriod => !self.forbid_question_mark_period,
            TokenType::Equals => !self.forbid_equals,
            _ => true,
        }
    }

    pub fn merge(&self, other: ForbiddenTokens) -> ForbiddenTokens {
        ForbiddenTokens {
            forbid_in: self.forbid_in || other.forbid_in,
            forbid_logical: self.forbid_logical || other.forbid_logical,
            forbid_coalesce: self.forbid_coalesce || other.forbid_coalesce,
            forbid_paren_open: self.forbid_paren_open || other.forbid_paren_open,
            forbid_question_mark_period: self.forbid_question_mark_period || other.forbid_question_mark_period,
            forbid_equals: self.forbid_equals || other.forbid_equals,
        }
    }

    pub fn forbid(&self, tokens: &[TokenType]) -> ForbiddenTokens {
        let mut result = *self;
        for &t in tokens {
            match t {
                TokenType::In => result.forbid_in = true,
                TokenType::DoubleAmpersand | TokenType::DoublePipe => result.forbid_logical = true,
                TokenType::DoubleQuestionMark => result.forbid_coalesce = true,
                TokenType::ParenOpen => result.forbid_paren_open = true,
                TokenType::QuestionMarkPeriod => result.forbid_question_mark_period = true,
                TokenType::Equals => result.forbid_equals = true,
                _ => {}
            }
        }
        result
    }
}

pub struct ParserError {
    pub message: String,
    pub line: u32,
    pub column: u32,
}

/// Boolean flags that are saved/restored during speculative parsing.
#[derive(Clone, Copy, Default)]
pub(crate) struct ParserFlags {
    pub strict_mode: bool,
    pub allow_super_property_lookup: bool,
    pub allow_super_constructor_call: bool,
    pub in_function_context: bool,
    pub in_formal_parameter_context: bool,
    pub in_generator_function_context: bool,
    pub await_expression_is_valid: bool,
    pub in_break_context: bool,
    pub in_continue_context: bool,
    pub string_legacy_octal_escape_sequence_in_scope: bool,
    pub in_class_field_initializer: bool,
    pub in_class_static_init_block: bool,
    pub function_might_need_arguments_object: bool,
    pub previous_token_was_period: bool,
    /// Set during property key parsing to suppress eval/arguments check.
    /// C++ uses separate `consume()` and `consume_and_allow_division()` methods;
    /// we emulate this by skipping the check in property key contexts.
    pub in_property_key_context: bool,
}

/// Snapshot of parser state for speculative parsing (backtracking).
struct SavedState {
    token: Token,
    errors_len: usize,
    flags: ParserFlags,
    scope_collector_state: ScopeCollectorState,
}

/// The main JavaScript parser.
///
/// Produces an AST. Parsing methods live in the `expressions`,
/// `statements`, and `declarations` submodules (all `impl Parser`).
pub struct Parser<'a> {
    lexer: Lexer<'a>,
    /// `consume()` returns this and advances to the next token.
    current_token: Token,
    errors: Vec<ParserError>,
    saved_states: Vec<SavedState>,
    program_type: ProgramType,
    /// UTF-16 source text.
    source: &'a [u16],

    // --- Parser state flags (saved/restored during speculative parsing) ---
    pub(crate) flags: ParserFlags,

    // --- Flags NOT saved/restored during speculative parsing ---
    pub(crate) initiated_by_eval: bool,
    pub(crate) in_eval_function_context: bool,

    /// Labels currently in scope. Value is Some(line, col) if a `continue`
    /// statement referenced this label, None otherwise.
    labels_in_scope: HashMap<Utf16String, Option<(u32, u32)>>,

    /// Set by try_parse_labelled_statement to propagate iteration-ness
    /// through nested labels (e.g., `a: b: for(...)`).
    last_inner_label_is_iteration: bool,

    last_function_name: Utf16String,
    last_function_kind: FunctionKind,
    last_class_name: Utf16String,

    /// Bound names collected during parse_binding_pattern.
    /// Caller drains this after calling parse_binding_pattern.
    /// Each entry is (name, identifier) — allows scope analysis to annotate
    /// binding pattern identifiers with local variable info.
    pub(crate) pattern_bound_names: Vec<(Utf16String, Rc<Identifier>)>,

    /// Set during synthesize_binding_pattern to allow MemberExpressions as binding targets.
    allow_member_expressions: bool,

    /// Position of the opening bracket/brace in binding patterns.
    /// Used so all identifiers inside a binding pattern share the pattern's start position,
    /// matching C++ parser behavior.
    binding_pattern_start: Option<Position>,

    /// True while parsing a class body that has an `extends` clause.
    pub(crate) class_has_super_class: bool,
    /// Depth counter for class bodies — used to reject `#name` outside classes.
    pub(crate) class_scope_depth: u32,
    pub(crate) has_default_export_name: bool,

    /// Stack of sets tracking private names referenced inside class bodies.
    /// Each class body pushes a new set. At the end of the class, any names
    /// not found in the class's declared private names are bubbled up to the
    /// outer class, or reported as errors if there is no outer class.
    referenced_private_names_stack: Vec<HashSet<Utf16String>>,

    /// Communication channel from `parse_variable_declaration` back to
    /// `parse_for_statement` when parsing `for (let/const/var ... ; ...)`.
    /// These are set when `is_for_loop` is true and read by the for-loop
    /// parser to validate for-in/of restrictions.
    pub(crate) for_loop_declaration_count: usize,
    pub(crate) for_loop_declaration_has_init: bool,
    pub(crate) for_loop_declaration_is_var: bool,

    pub scope_collector: ScopeCollector,

    /// Track exported names for duplicate detection in modules.
    exported_names: HashSet<Utf16String>,

    /// Side table owning all FunctionData produced during parsing.
    pub function_table: FunctionTable,
}

impl<'a> Parser<'a> {
    pub fn new(source: &'a [u16], program_type: ProgramType) -> Self {
        Self::new_with_line_offset(source, program_type, 1)
    }

    pub fn new_with_line_offset(source: &'a [u16], program_type: ProgramType, initial_line_number: u32) -> Self {
        let mut lexer = Lexer::new(source, initial_line_number, 0);
        if program_type == ProgramType::Module {
            lexer.disallow_html_comments();
        }
        let first_token = lexer.next();
        Self {
            lexer,
            current_token: first_token,
            errors: Vec::new(),
            saved_states: Vec::new(),
            program_type,
            source,
            flags: ParserFlags::default(),
            initiated_by_eval: false,
            in_eval_function_context: false,
            labels_in_scope: HashMap::new(),
            last_inner_label_is_iteration: false,
            last_function_name: Utf16String::default(),
            last_function_kind: FunctionKind::Normal,
            last_class_name: Utf16String::default(),
            pattern_bound_names: Vec::new(),
            allow_member_expressions: false,
            binding_pattern_start: None,
            class_has_super_class: false,
            class_scope_depth: 0,
            has_default_export_name: false,
            referenced_private_names_stack: Vec::new(),
            for_loop_declaration_count: 0,
            for_loop_declaration_has_init: false,
            for_loop_declaration_is_var: false,
            scope_collector: ScopeCollector::new(),
            exported_names: HashSet::new(),
            function_table: FunctionTable::new(),
        }
    }

    // === AST construction helpers ===

    pub(crate) fn range_from(&self, start: Position) -> SourceRange {
        SourceRange { start, end: self.position() }
    }

    pub(crate) fn expression(&self, start: Position, expression: ExpressionKind) -> Expression {
        Expression::new(self.range_from(start), expression)
    }

    pub(crate) fn statement(&self, start: Position, statement: StatementKind) -> Statement {
        Statement::new(self.range_from(start), statement)
    }

    pub(crate) fn make_identifier(&self, start: Position, name: impl Into<Utf16String>) -> Rc<Identifier> {
        Rc::new(Identifier::new(self.range_from(start), name.into()))
    }

    pub(crate) fn register_function_parameters_with_scope(
        &mut self,
        parameters: &[FunctionParameter],
        parameter_info: &[ParamInfo],
    ) {
        use crate::ast::FunctionParameterBinding;
        use crate::scope_collector::ParameterEntry;
        let mut entries: Vec<ParameterEntry> = Vec::new();
        let mut has_parameter_expressions = false;
        let mut info_index = 0;
        for parameter in parameters {
            if parameter.default_value.is_some() {
                has_parameter_expressions = true;
            }
            match &parameter.binding {
                FunctionParameterBinding::Identifier(id) => {
                    let (name, is_rest, is_from_pattern) = if info_index < parameter_info.len() {
                        let pi = &parameter_info[info_index];
                        info_index += 1;
                        (pi.name.clone(), pi.is_rest, pi.is_from_pattern)
                    } else {
                        (id.name.clone(), parameter.is_rest, false)
                    };
                    entries.push(ParameterEntry { name, identifier: Some(id.clone()), is_rest, is_from_pattern, is_first_from_pattern: false });
                }
                FunctionParameterBinding::BindingPattern(pattern) => {
                    if pattern.contains_expression() {
                        has_parameter_expressions = true;
                    }
                    // Push a placeholder entry for the pattern parameter itself
                    // so subsequent parameters get correct positional indices.
                    entries.push(ParameterEntry { name: Utf16String::default(), identifier: None, is_rest: false, is_from_pattern: true, is_first_from_pattern: true });
                    // Then push bound names from this pattern.
                    while info_index < parameter_info.len() && parameter_info[info_index].is_from_pattern {
                        let pi = &parameter_info[info_index];
                        entries.push(ParameterEntry { name: pi.name.clone(), identifier: pi.identifier.clone(), is_rest: pi.is_rest, is_from_pattern: true, is_first_from_pattern: false });
                        info_index += 1;
                    }
                }
            }
        }
        self.scope_collector.set_function_parameters(&entries, has_parameter_expressions);
    }

    // === Token access ===

    pub(crate) fn current_token(&self) -> &Token {
        &self.current_token
    }

    pub(crate) fn current_token_type(&self) -> TokenType {
        self.current_token.token_type
    }

    pub(crate) fn match_token(&self, tt: TokenType) -> bool {
        self.current_token.token_type == tt
    }

    pub(crate) fn done(&self) -> bool {
        self.match_token(TokenType::Eof)
    }

    // === Token consumption ===

    pub(crate) fn consume(&mut self) -> Token {
        let old = std::mem::replace(&mut self.current_token, self.lexer.next());
        // C++ checks for `arguments`/`eval` in `consume_and_allow_division()` which
        // is used by `consume_identifier()`. We put the check here (in `consume()`)
        // but skip it when parsing property keys, matching C++'s behavior.
        if !self.flags.in_property_key_context {
            self.check_arguments_or_eval(&old);
        }
        self.flags.previous_token_was_period = old.token_type == TokenType::Period;
        old
    }

    pub(crate) fn consume_and_check_identifier(&mut self) -> Token {
        let token = self.consume();
        if self.flags.strict_mode && token.token_type == TokenType::Identifier {
            let value = self.token_value(&token);
            if is_strict_reserved_word(value) {
                let name = String::from_utf16_lossy(value);
                self.syntax_error(&format!(
                    "Identifier must not be a reserved word in strict mode ('{}')",
                    name
                ));
            }
        }
        token
    }

    pub(crate) fn consume_token(&mut self, expected: TokenType) -> Token {
        if self.current_token.token_type != expected {
            self.expected(expected.name());
        }
        self.consume()
    }

    pub(crate) fn eat(&mut self, tt: TokenType) -> bool {
        if self.match_token(tt) {
            self.consume();
            true
        } else {
            false
        }
    }

    fn check_arguments_or_eval(&mut self, token: &Token) {
        if token.token_type == TokenType::Identifier && !self.flags.previous_token_was_period {
            let value: &[u16] = if let Some(ref v) = token.identifier_value {
                v
            } else {
                let start = token.value_start as usize;
                let end = start + token.value_len as usize;
                if end <= self.source.len() { &self.source[start..end] } else { &[] }
            };
            if value == utf16!("arguments") {
                if self.flags.in_class_field_initializer {
                    self.syntax_error("'arguments' is not allowed in class field initializer");
                }
                self.flags.function_might_need_arguments_object = true;
            } else if value == utf16!("eval") {
                self.flags.function_might_need_arguments_object = true;
            }
        }
    }

    pub(crate) fn consume_identifier(&mut self) -> Token {
        if self.match_identifier() {
            return self.consume_and_check_identifier();
        }
        self.expected("identifier");
        self.consume()
    }

    // https://tc39.es/ecma262/#sec-numeric-literals-early-errors
    // It is a Syntax Error if IsStringWellFormedUnicode of the source text matched
    // by NumericLiteral is not true.
    // The source character immediately following a NumericLiteral must not be an
    // IdentifierStart or DecimalDigit.
    pub(crate) fn consume_and_validate_numeric_literal(&mut self) -> Token {
        let token = self.consume();
        if self.flags.strict_mode {
            // https://tc39.es/ecma262/#sec-additional-syntax-numeric-literals
            // In strict mode, legacy octal literals (0-prefixed) are not permitted.
            let value = self.token_value(&token);
            if value.len() > 1 && value[0] == ch(b'0')
                && value[1] >= ch(b'0') && value[1] <= ch(b'9')
            {
                self.syntax_error("Unprefixed octal number not allowed in strict mode");
            }
        }
        if self.match_identifier_name() && self.current_token.trivia_len == 0 {
            self.syntax_error("Numeric literal must not be immediately followed by identifier");
        }
        token
    }

    // https://tc39.es/ecma262/#sec-automatic-semicolon-insertion
    // A semicolon is automatically inserted when:
    //   1. The offending token is separated from the previous token by at least
    //      one LineTerminator.
    //   2. The offending token is `}`.
    //   3. The previous token is `)` and the inserted semicolon would then be
    //      parsed as the terminating semicolon of a do-while statement.
    //   4. The end of the input stream of tokens is reached.
    pub(crate) fn consume_or_insert_semicolon(&mut self) {
        if self.match_token(TokenType::Semicolon) {
            self.consume();
            return;
        }
        if self.current_token.trivia_has_line_terminator
            || self.match_token(TokenType::CurlyClose)
            || self.done()
        {
            return;
        }
        self.expected("Semicolon");
    }

    // === Lookahead ===

    pub(crate) fn next_token(&mut self) -> Token {
        self.lexer.save_state();
        let token = self.lexer.next();
        self.lexer.load_state();
        token
    }

    // === Position ===

    pub(crate) fn position(&self) -> Position {
        Position {
            line: self.current_token.line_number,
            column: self.current_token.line_column,
            offset: self.current_token.offset,
        }
    }

    pub(crate) fn source_text_end_offset(&self) -> u32 {
        self.current_token.offset - self.current_token.trivia_len
    }

    // === Error reporting ===

    pub(crate) fn syntax_error(&mut self, message: &str) {
        self.errors.push(ParserError {
            message: message.to_string(),
            line: self.current_token.line_number,
            column: self.current_token.line_column,
        });
    }

    pub(crate) fn syntax_error_at(&mut self, message: &str, line: u32, column: u32) {
        self.errors.push(ParserError {
            message: message.to_string(),
            line,
            column,
        });
    }

    pub(crate) fn syntax_error_at_position(&mut self, message: &str, pos: Position) {
        self.syntax_error_at(message, pos.line, pos.column);
    }

    /// Register a referenced private name. Returns true if we're inside a class
    /// body (the reference is valid for now, will be checked at class end).
    /// Returns false if we're outside all class bodies (always invalid).
    pub(crate) fn register_referenced_private_name(&mut self, name: &[u16]) -> bool {
        if let Some(set) = self.referenced_private_names_stack.last_mut() {
            set.insert(Utf16String::from(name));
            true
        } else {
            false
        }
    }

    /// Parse and validate a private identifier token.
    /// Registers the private name reference and emits an error if outside a class body.
    /// The current token must be a PrivateIdentifier.
    pub(crate) fn parse_private_identifier(&mut self, range_start: Position) -> PrivateIdentifier {
        let value = self.token_value(&self.current_token).to_vec();
        if !self.register_referenced_private_name(&value) {
            let name = String::from_utf16_lossy(&value);
            self.syntax_error(&format!(
                "Reference to undeclared private field or method '{}'",
                name
            ));
        }
        let token = self.consume();
        let value = self.token_value(&token).to_vec();
        PrivateIdentifier {
            range: self.range_from(range_start),
            name: value.into(),
        }
    }

    pub(crate) fn expected(&mut self, what: &str) {
        let msg = if let Some(ref message) = self.current_token.message {
            message.clone()
        } else {
            format!(
                "Unexpected token {}. Expected {}",
                self.current_token.token_type.name(),
                what
            )
        };
        self.syntax_error(&msg);
    }

    /// Compile a regex pattern+flags and return the opaque compiled handle.
    /// On error, reports a syntax error and returns null.
    pub(crate) fn compile_regex_pattern(&mut self, pattern: &[u16], flags: &[u16]) -> *mut std::ffi::c_void {
        match crate::bytecode::ffi::compile_regex(pattern, flags) {
            Ok(handle) => handle,
            Err(msg) => {
                self.syntax_error(&msg);
                std::ptr::null_mut()
            }
        }
    }

    pub(crate) fn validate_regex_flags(&mut self, flags: &[u16]) {
        let valid_flags: &[u16] = &[ch(b'd'), ch(b'g'), ch(b'i'), ch(b'm'), ch(b's'), ch(b'u'), ch(b'v'), ch(b'y')];
        let mut seen = [false; 128];
        for &flag in flags {
            if flag >= 128 || !valid_flags.contains(&flag) {
                self.syntax_error(&format!("Invalid RegExp flag '{}'", char::from_u32(flag as u32).unwrap_or('?')));
                return;
            }
            if seen[flag as usize] {
                self.syntax_error(&format!("Repeated RegExp flag '{}'", char::from_u32(flag as u32).unwrap_or('?')));
                return;
            }
            seen[flag as usize] = true;
        }
    }

    pub fn has_errors(&self) -> bool {
        !self.errors.is_empty()
    }

    pub fn errors(&self) -> &[ParserError] {
        &self.errors
    }

    pub fn error_messages(&self) -> Vec<String> {
        self.errors
            .iter()
            .map(|e| format!("{}:{}: {}", e.line, e.column, e.message))
            .collect()
    }

    // === State save/restore for backtracking ===

    pub(crate) fn save_state(&mut self) {
        self.lexer.save_state();
        self.saved_states.push(SavedState {
            token: self.current_token.clone(),
            errors_len: self.errors.len(),
            flags: self.flags,
            scope_collector_state: self.scope_collector.save_state(),
        });
    }

    pub(crate) fn load_state(&mut self) {
        let state = self.saved_states.pop().expect("No saved state to restore");
        self.current_token = state.token;
        self.errors.truncate(state.errors_len);
        self.flags = state.flags;
        self.scope_collector.load_state(state.scope_collector_state);
        self.lexer.load_state();
    }

    pub(crate) fn discard_saved_state(&mut self) {
        self.saved_states.pop();
        self.lexer.discard_saved_state();
    }

    // === Token matching helpers ===

    pub(crate) fn match_identifier(&self) -> bool {
        self.token_is_identifier(&self.current_token)
    }

    pub(crate) fn token_is_identifier(&self, token: &Token) -> bool {
        let tt = token.token_type;
        tt == TokenType::Identifier
            || (tt == TokenType::EscapedKeyword && !self.match_invalid_escaped_keyword())
            || (tt == TokenType::Let && !self.flags.strict_mode)
            || (tt == TokenType::Yield && !self.flags.strict_mode && !self.flags.in_generator_function_context)
            || (tt == TokenType::Await && !self.flags.await_expression_is_valid && self.program_type != ProgramType::Module && !self.flags.in_class_static_init_block)
            || tt == TokenType::Async
    }

    pub(crate) fn match_identifier_name(&self) -> bool {
        self.current_token.token_type.is_identifier_name()
            || self.match_identifier()
    }

    pub(crate) fn match_invalid_escaped_keyword(&self) -> bool {
        if self.current_token.token_type != TokenType::EscapedKeyword {
            return false;
        }
        let value = self.token_value(&self.current_token);
        if value == utf16!("await") {
            return self.program_type == ProgramType::Module || self.flags.await_expression_is_valid || self.flags.in_class_static_init_block;
        }
        if value == utf16!("async") {
            return false;
        }
        if value == utf16!("yield") {
            return self.flags.in_generator_function_context;
        }
        if self.flags.strict_mode {
            return true;
        }
        // In non-strict mode, "let" and "static" are context-sensitive
        // keywords that are valid as escaped identifiers. All other
        // escaped keywords (break, for, etc.) are always invalid.
        value != utf16!("let") && value != utf16!("static")
    }

    pub(crate) fn check_identifier_name_for_assignment_validity(&mut self, name: &[u16], force_strict: bool) {
        if self.flags.strict_mode || force_strict {
            if name == utf16!("arguments") || name == utf16!("eval") {
                self.syntax_error("Binding pattern target may not be called 'arguments' or 'eval' in strict mode");
            } else if is_strict_reserved_word(name) {
                let name_str = String::from_utf16_lossy(name);
                self.syntax_error(&format!("Identifier must not be a reserved word in strict mode ('{}')", name_str));
            }
        }
    }

    /// Check for duplicate parameter names in arrow functions.
    /// Arrow functions always reject duplicates, regardless of strict mode.
    pub(crate) fn check_arrow_duplicate_parameters(&mut self, parameter_info: &[ParamInfo]) {
        let mut seen_names: HashSet<&[u16]> = HashSet::new();
        for pi in parameter_info {
            let name = &pi.name;
            if name.is_empty() {
                continue;
            }
            if !seen_names.insert(&**name) {
                let name_str = String::from_utf16_lossy(name);
                self.syntax_error(&format!(
                    "Duplicate parameter '{}' not allowed in arrow function",
                    name_str
                ));
            }
        }
    }

    /// Post-body check for function parameters when 'use strict' was found in the
    /// body or the function is a generator/async.
    pub(crate) fn check_parameters_post_body(
        &mut self,
        parameter_info: &[ParamInfo],
        force_strict: bool,
        _kind: FunctionKind,
    ) {
        let mut seen_names: HashSet<&[u16]> = HashSet::new();
        for pi in parameter_info {
            let name = &pi.name;
            if name.is_empty() {
                continue;
            }
            self.check_identifier_name_for_assignment_validity(name, force_strict);
            if !seen_names.insert(&**name) {
                let name_str = String::from_utf16_lossy(name);
                self.syntax_error(&format!(
                    "Duplicate parameter '{}' not allowed in strict mode",
                    name_str
                ));
            }
        }
    }

    pub(crate) fn token_value<'b>(&'b self, token: &'b Token) -> &'b [u16] {
        if let Some(ref value) = token.identifier_value {
            return value;
        }
        let start = token.value_start as usize;
        let end = start + token.value_len as usize;
        assert!(
            end <= self.source.len(),
            "token_value: bounds [{start}..{end}) exceed source length {}",
            self.source.len()
        );
        &self.source[start..end]
    }

    pub(crate) fn token_original_value(&self, token: &Token) -> &'a [u16] {
        let start = token.value_start as usize;
        let end = (token.value_start + token.value_len) as usize;
        assert!(
            end <= self.source.len(),
            "token_original_value: bounds [{start}..{end}) exceed source length {}",
            self.source.len()
        );
        &self.source[start..end]
    }

    /// Re-parse the source range starting at `start` as a binding pattern
    /// with member expressions allowed (for destructuring assignment patterns).
    pub(crate) fn synthesize_binding_pattern(&mut self, start: Position) -> Option<BindingPattern> {
        // Clear any syntax errors that occurred in the range of the expression
        // being reinterpreted as a binding pattern. This matches C++'s behavior
        // where errors like duplicate __proto__ in object literals are cleared
        // when the object is reinterpreted as an assignment target.
        let end_line = self.current_token.line_number;
        let end_column = self.current_token.line_column;
        self.errors.retain(|e| {
            !(e.line > start.line || (e.line == start.line && e.column >= start.column))
                || (e.line > end_line || (e.line == end_line && e.column >= end_column))
        });

        let saved_lexer = std::mem::replace(
            &mut self.lexer,
            Lexer::new_at_offset(self.source, start.offset as usize, start.line, start.column),
        );
        let saved_token = std::mem::replace(&mut self.current_token, Token::new(TokenType::Eof));
        let saved_allow = self.allow_member_expressions;

        self.current_token = self.lexer.next();
        self.allow_member_expressions = true;

        let pattern = self.parse_binding_pattern();

        self.lexer = saved_lexer;
        self.current_token = saved_token;
        self.allow_member_expressions = saved_allow;

        Some(pattern)
    }

    pub(crate) fn is_simple_assignment_target(expression: &Expression, allow_call_expression: bool) -> bool {
        matches!(&expression.inner,
            ExpressionKind::Identifier(_)
            | ExpressionKind::Member { .. }
        ) || (allow_call_expression && matches!(&expression.inner, ExpressionKind::Call(_)))
    }

    fn is_object_expression(expression: &Expression) -> bool {
        matches!(&expression.inner, ExpressionKind::Object(_))
    }

    fn is_array_expression(expression: &Expression) -> bool {
        matches!(&expression.inner, ExpressionKind::Array(_))
    }

    fn is_identifier(expression: &Expression) -> bool {
        matches!(&expression.inner, ExpressionKind::Identifier(_))
    }

    fn is_member_expression(expression: &Expression) -> bool {
        matches!(&expression.inner, ExpressionKind::Member { .. })
    }

    fn is_call_expression(expression: &Expression) -> bool {
        matches!(&expression.inner, ExpressionKind::Call(_))
    }

    fn is_update_expression(expression: &Expression) -> bool {
        matches!(&expression.inner, ExpressionKind::Update { .. })
    }

    // === Main entry point ===

    pub fn parse_program(&mut self, starts_in_strict_mode: bool) -> Statement {
        let start = self.position();

        if self.program_type == ProgramType::Script {
            let (children, is_strict) = self.parse_script(starts_in_strict_mode);
            let scope = ScopeData::shared_with_children(children);
            // Scope was opened in parse_script via open_program_scope.
            // Now close it after children are set.
            self.scope_collector.set_scope_node(scope.clone());
            self.scope_collector.close_scope();
            self.statement(start, StatementKind::Program(ProgramData {
                scope,
                program_type: ProgramType::Script,
                is_strict_mode: is_strict,
                has_top_level_await: false,
            }))
        } else {
            let (children, has_top_level_await) = self.parse_module();
            let scope = ScopeData::shared_with_children(children);
            self.scope_collector.set_scope_node(scope.clone());
            self.scope_collector.close_scope();
            self.statement(start, StatementKind::Program(ProgramData {
                scope,
                program_type: ProgramType::Module,
                is_strict_mode: true,
                has_top_level_await,
            }))
        }
    }

    fn parse_script(&mut self, starts_in_strict_mode: bool) -> (Vec<Statement>, bool) {
        // Open program scope — will be closed in parse_program after ScopeData is created.
        self.scope_collector.open_program_scope(ProgramType::Script);

        let strict_before = self.flags.strict_mode;
        if starts_in_strict_mode {
            self.flags.strict_mode = true;
        }

        let (has_use_strict, mut children) = self.parse_directive();

        if self.flags.strict_mode || has_use_strict {
            self.flags.strict_mode = true;
        }

        children.extend(self.parse_statement_list(true));
        if !self.done() {
            if self.flags.in_function_context {
                self.expected("CurlyClose");
            } else {
                self.expected("statement or declaration");
            }
            self.consume();
        }

        let is_strict = self.flags.strict_mode;
        self.flags.strict_mode = strict_before;
        (children, is_strict)
    }

    // https://tc39.es/ecma262/#sec-modules
    // Module code is always strict mode code.
    fn parse_module(&mut self) -> (Vec<Statement>, bool) {
        // Open program scope — will be closed in parse_program after ScopeData is created.
        self.scope_collector.open_program_scope(ProgramType::Module);

        let strict_before = self.flags.strict_mode;
        let await_before = self.flags.await_expression_is_valid;
        self.flags.strict_mode = true;
        self.flags.await_expression_is_valid = true;

        let mut children = Vec::new();

        while !self.done() {
            children.extend(self.parse_statement_list(true));

            if self.done() {
                break;
            }

            if self.match_export_or_import() {
                if self.match_token(TokenType::Export) {
                    children.push(self.parse_export_statement());
                } else {
                    children.push(self.parse_import_statement());
                }
            } else {
                self.expected("statement or declaration");
                self.consume();
            }
        }

        // Check that all exported bindings are declared in the module.
        self.check_undeclared_exports(&children);

        self.flags.strict_mode = strict_before;
        self.flags.await_expression_is_valid = await_before;
        let has_top_level_await = self.scope_collector.contains_await_expression();
        (children, has_top_level_await)
    }

    fn check_undeclared_exports(&mut self, children: &[Statement]) {
        use crate::ast::*;

        // Collect all declared names at module level.
        let mut declared_names: HashSet<Utf16String> = HashSet::new();
        for child in children {
            match &child.inner {
                StatementKind::VariableDeclaration { declarations, .. } => {
                    for decl in declarations {
                        collect_binding_names(&decl.target, &mut declared_names);
                    }
                }
                StatementKind::FunctionDeclaration { name: Some(ref name), .. } => {
                    declared_names.insert(name.name.clone());
                }
                StatementKind::ClassDeclaration(data) => {
                    if let Some(ref name) = data.name {
                        declared_names.insert(name.name.clone());
                    }
                }
                StatementKind::Import(data) => {
                    for entry in &data.entries {
                        declared_names.insert(entry.local_name.clone());
                    }
                }
                StatementKind::Export(data) => {
                    if let Some(ref statement) = data.statement {
                        match &statement.inner {
                            StatementKind::VariableDeclaration { declarations, .. } => {
                                for decl in declarations {
                                    collect_binding_names(&decl.target, &mut declared_names);
                                }
                            }
                            StatementKind::FunctionDeclaration { name: Some(ref name), .. } => {
                                declared_names.insert(name.name.clone());
                            }
                            StatementKind::ClassDeclaration(class_data) => {
                                if let Some(ref name) = class_data.name {
                                    declared_names.insert(name.name.clone());
                                }
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
        }

        // Check each export's local bindings.
        for child in children {
            if let StatementKind::Export(data) = &child.inner {
                if data.statement.is_some() {
                    continue;
                }
                for entry in &data.entries {
                    if data.module_request.is_some() {
                        continue;
                    }
                    if entry.kind == ExportEntryKind::EmptyNamedExport {
                        continue;
                    }
                    if let Some(ref local_name) = entry.local_or_import_name {
                        if !declared_names.contains(local_name.as_slice()) {
                            self.syntax_error_at_position(
                                &format!(
                                    "'{}' in export is not declared",
                                    String::from_utf16_lossy(local_name.as_slice())
                                ),
                                child.range.start,
                            );
                        }
                    }
                }
            }
        }
    }

    // https://tc39.es/ecma262/#sec-directive-prologues-and-the-use-strict-directive
    // A Directive Prologue is a sequence of ExpressionStatements at the beginning
    // of a FunctionBody, ScriptBody, or ModuleBody that each consist entirely of
    // a StringLiteral followed by semicolon. A "use strict" directive causes
    // subsequent code to be interpreted in strict mode.
    pub(crate) fn parse_directive(&mut self) -> (bool, Vec<Statement>) {
        let mut found_use_strict = false;
        let mut statements = Vec::new();
        while !self.done() && self.match_token(TokenType::StringLiteral) {
            let raw_value = self.token_original_value(&self.current_token);
            let statement = self.parse_statement(false);
            statements.push(statement);

            if is_use_strict(raw_value) {
                found_use_strict = true;
                if self.flags.string_legacy_octal_escape_sequence_in_scope {
                    self.syntax_error("Octal escape sequence in string literal not allowed in strict mode");
                }
                break;
            }
        }
        self.flags.string_legacy_octal_escape_sequence_in_scope = false;
        (found_use_strict, statements)
    }

    pub(crate) fn parse_statement_list(&mut self, allow_labelled_functions: bool) -> Vec<Statement> {
        let mut statements = Vec::new();
        while !self.done() {
            if self.match_export_or_import() {
                break;
            }
            if self.match_declaration() {
                statements.push(self.parse_declaration());
            } else if self.match_statement() {
                statements.push(self.parse_statement(allow_labelled_functions));
            } else {
                break;
            }
        }
        statements
    }

    pub(crate) fn match_statement(&mut self) -> bool {
        matches!(
            self.current_token_type(),
            TokenType::CurlyOpen
                | TokenType::Return
                | TokenType::Var
                | TokenType::For
                | TokenType::If
                | TokenType::Throw
                | TokenType::Try
                | TokenType::Break
                | TokenType::Continue
                | TokenType::Switch
                | TokenType::Do
                | TokenType::While
                | TokenType::With
                | TokenType::Debugger
                | TokenType::Semicolon
                | TokenType::Slash
                | TokenType::SlashEquals
        ) || self.match_expression()
    }

    pub(crate) fn match_declaration(&mut self) -> bool {
        match self.current_token_type() {
            TokenType::Function | TokenType::Class | TokenType::Const => true,
            TokenType::Let => {
                if !self.flags.strict_mode {
                    self.try_match_let_declaration()
                } else {
                    true
                }
            }
            TokenType::Async => {
                let next = self.next_token();
                next.token_type == TokenType::Function && !next.trivia_has_line_terminator
            }
            TokenType::Identifier => {
                let value = self.token_value(&self.current_token);
                if value != utf16!("using") {
                    return false;
                }
                let next = self.next_token();
                !next.trivia_has_line_terminator && next.token_type.is_identifier_name()
            }
            _ => false,
        }
    }

    fn try_match_let_declaration(&mut self) -> bool {
        let next = self.next_token();
        if next.token_type.is_identifier_name() && self.token_value(&next) != utf16!("in") {
            return true;
        }
        if next.token_type == TokenType::CurlyOpen || next.token_type == TokenType::BracketOpen {
            return true;
        }
        false
    }

    fn match_iteration_start(&self) -> bool {
        matches!(self.current_token_type(),
            TokenType::For | TokenType::While | TokenType::Do)
    }

    pub(crate) fn match_export_or_import(&mut self) -> bool {
        if self.match_token(TokenType::Export) {
            return true;
        }
        if self.match_token(TokenType::Import) {
            let next = self.next_token();
            return next.token_type != TokenType::ParenOpen
                && next.token_type != TokenType::Period;
        }
        false
    }

    // === Operator precedence ===

    pub(crate) fn operator_precedence(tt: TokenType) -> i32 {
        match tt {
            TokenType::Period | TokenType::BracketOpen | TokenType::ParenOpen | TokenType::QuestionMarkPeriod => 20,
            TokenType::New => 19,
            TokenType::PlusPlus | TokenType::MinusMinus => 18,
            TokenType::ExclamationMark | TokenType::Tilde | TokenType::Typeof | TokenType::Void | TokenType::Delete | TokenType::Await => 17,
            TokenType::DoubleAsterisk => 16,
            TokenType::Asterisk | TokenType::Slash | TokenType::Percent => 15,
            TokenType::Plus | TokenType::Minus => 14,
            TokenType::ShiftLeft | TokenType::ShiftRight | TokenType::UnsignedShiftRight => 13,
            TokenType::LessThan | TokenType::LessThanEquals | TokenType::GreaterThan | TokenType::GreaterThanEquals | TokenType::In | TokenType::Instanceof => 12,
            TokenType::EqualsEquals | TokenType::ExclamationMarkEquals | TokenType::EqualsEqualsEquals | TokenType::ExclamationMarkEqualsEquals => 11,
            TokenType::Ampersand => 10,
            TokenType::Caret => 9,
            TokenType::Pipe => 8,
            TokenType::DoubleQuestionMark => 7,
            TokenType::DoubleAmpersand => 6,
            TokenType::DoublePipe => 5,
            TokenType::QuestionMark => 4,
            TokenType::Equals | TokenType::PlusEquals | TokenType::MinusEquals
            | TokenType::DoubleAsteriskEquals | TokenType::AsteriskEquals | TokenType::SlashEquals
            | TokenType::PercentEquals | TokenType::ShiftLeftEquals | TokenType::ShiftRightEquals
            | TokenType::UnsignedShiftRightEquals | TokenType::AmpersandEquals | TokenType::CaretEquals
            | TokenType::PipeEquals | TokenType::DoubleAmpersandEquals | TokenType::DoublePipeEquals
            | TokenType::DoubleQuestionMarkEquals => 3,
            TokenType::Yield => 2,
            TokenType::Comma => 1,
            _ => 0,
        }
    }

    pub(crate) fn operator_associativity(tt: TokenType) -> Associativity {
        match tt {
            TokenType::Period | TokenType::BracketOpen | TokenType::ParenOpen | TokenType::QuestionMarkPeriod
            | TokenType::Asterisk | TokenType::Slash | TokenType::Percent
            | TokenType::Plus | TokenType::Minus
            | TokenType::ShiftLeft | TokenType::ShiftRight | TokenType::UnsignedShiftRight
            | TokenType::LessThan | TokenType::LessThanEquals | TokenType::GreaterThan | TokenType::GreaterThanEquals
            | TokenType::In | TokenType::Instanceof
            | TokenType::EqualsEquals | TokenType::ExclamationMarkEquals | TokenType::EqualsEqualsEquals | TokenType::ExclamationMarkEqualsEquals
            | TokenType::Typeof | TokenType::Void | TokenType::Delete | TokenType::Await
            | TokenType::Ampersand | TokenType::Caret | TokenType::Pipe
            | TokenType::DoubleQuestionMark | TokenType::DoubleAmpersand | TokenType::DoublePipe
            | TokenType::Comma => Associativity::Left,
            _ => Associativity::Right,
        }
    }
}

// === Helpers ===

fn is_use_strict(raw: &[u16]) -> bool {
    raw == utf16!("'use strict'") || raw == utf16!("\"use strict\"")
}

/// Collect all binding names introduced by a variable declarator target.
fn collect_binding_names(target: &crate::ast::VariableDeclaratorTarget, names: &mut HashSet<Utf16String>) {
    match target {
        crate::ast::VariableDeclaratorTarget::Identifier(identifier) => {
            names.insert(identifier.name.clone());
        }
        crate::ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            collect_binding_pattern_names(pattern, names);
        }
    }
}

/// Collect all binding names from a binding pattern (object or array destructuring).
fn collect_binding_pattern_names(pattern: &crate::ast::BindingPattern, names: &mut HashSet<Utf16String>) {
    for entry in &pattern.entries {
        if let Some(ref alias) = entry.alias {
            match alias {
                crate::ast::BindingEntryAlias::Identifier(identifier) => {
                    names.insert(identifier.name.clone());
                }
                crate::ast::BindingEntryAlias::BindingPattern(nested) => {
                    collect_binding_pattern_names(nested, names);
                }
                crate::ast::BindingEntryAlias::MemberExpression(_) => {}
            }
        } else if let Some(crate::ast::BindingEntryName::Identifier(identifier)) = &entry.name {
            names.insert(identifier.name.clone());
        }
    }
}

// https://tc39.es/ecma262/#sec-keywords-and-reserved-words
// In strict mode code, the following tokens are also reserved:
// `implements` `interface` `let` `package` `private` `protected` `public` `static` `yield`
pub(crate) fn is_strict_reserved_word(name: &[u16]) -> bool {
    name == utf16!("implements")
        || name == utf16!("interface")
        || name == utf16!("let")
        || name == utf16!("package")
        || name == utf16!("private")
        || name == utf16!("protected")
        || name == utf16!("public")
        || name == utf16!("static")
        || name == utf16!("yield")
}
