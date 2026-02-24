/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AST dump matching the C++ ASTDump.cpp output format exactly.
//!
//! Produces tree-drawing output to stdout via `println!`, matching
//! the C++ `outln` calls.

use crate::ast::*;
use std::cell::RefCell;
use std::fmt::Write;

unsafe extern "C" {
    // FIXME: This FFI workaround exists only to match C++ float-to-string
    //        formatting in the AST dump. Once the C++ pipeline is removed,
    //        this can be deleted and we can use our own formatting.
    fn rust_format_double(value: f64, buffer: *mut u8, buffer_len: usize) -> usize;
}

/// Defines a function that maps enum variants to static string slices.
macro_rules! op_to_string {
    ($name:ident, $enum_type:ty, { $($variant:ident => $str:literal),+ $(,)? }) => {
        fn $name(op: $enum_type) -> &'static str {
            match op {
                $(<$enum_type>::$variant => $str),+
            }
        }
    };
}

/// Prints a node header with the node name, optional extras, and source position.
macro_rules! dump_node {
    ($state:expr_2021, $name:expr_2021, $range:expr_2021) => {
        print_node(
            $state,
            &format!("{}{}", color_node_name($state, $name), format_position($state, $range)),
        )
    };
    ($state:expr_2021, $name:expr_2021, $range:expr_2021, $($extra:expr_2021),+ $(,)?) => {
        print_node(
            $state,
            &{
                let mut description = color_node_name($state, $name);
                $(description.push_str(&format!(" {}", $extra));)+
                description.push_str(&format_position($state, $range));
                description
            },
        )
    };
}

// ANSI color codes matching C++ ASTDump.cpp.
const RESET: &str = "\x1b[0m";
const DIM: &str = "\x1b[2m";
const GREEN: &str = "\x1b[32m";
const YELLOW: &str = "\x1b[33m";
const CYAN: &str = "\x1b[36m";
const MAGENTA: &str = "\x1b[35m";
const WHITE_BOLD: &str = "\x1b[1;37m";

struct DumpState<'a> {
    prefix: String,
    is_last: bool,
    is_root: bool,
    use_color: bool,
    output: Option<&'a RefCell<String>>,
    function_table: &'a FunctionTable,
}

impl DumpState<'_> {
    fn function_table(&self) -> &FunctionTable {
        self.function_table
    }
}

fn print_node(state: &DumpState, text: &str) {
    let line = if state.is_root {
        text.to_string()
    } else {
        let connector = if state.is_last {
            "\u{2514}\u{2500} "
        } else {
            "\u{251c}\u{2500} "
        };
        if state.use_color {
            format!("{}{}{}{}{}", state.prefix, DIM, connector, RESET, text)
        } else {
            format!("{}{}{}", state.prefix, connector, text)
        }
    };
    if let Some(output) = state.output {
        let _ = writeln!(output.borrow_mut(), "{}", line);
    } else {
        println!("{}", line);
    }
}

fn child_prefix(state: &DumpState) -> String {
    if state.is_root {
        return String::new();
    }
    let branch = if state.is_last { "   " } else { "\u{2502}  " };
    if state.use_color {
        format!("{}{}{}{}", state.prefix, DIM, branch, RESET)
    } else {
        format!("{}{}", state.prefix, branch)
    }
}

fn child_state<'a>(state: &DumpState<'a>, is_last: bool) -> DumpState<'a> {
    DumpState {
        prefix: child_prefix(state),
        is_last,
        is_root: false,
        use_color: state.use_color,
        output: state.output,
        function_table: state.function_table,
    }
}

fn format_position(state: &DumpState, range: &SourceRange) -> String {
    if range.start.line == 0 {
        return String::new();
    }
    if state.use_color {
        format!(
            " {}@{}:{}{}",
            DIM, range.start.line, range.start.column, RESET
        )
    } else {
        format!(" @{}:{}", range.start.line, range.start.column)
    }
}

fn color_node_name(state: &DumpState, name: &str) -> String {
    if !state.use_color {
        return name.to_string();
    }
    format!("{}{}{}", WHITE_BOLD, name, RESET)
}

fn color_string(state: &DumpState, value: &str) -> String {
    if !state.use_color {
        return format!("\"{}\"", value);
    }
    format!("{}\"{}\"{}", GREEN, value, RESET)
}

fn color_string_utf16(state: &DumpState, value: &[u16]) -> String {
    color_string(state, &utf16_to_string(value))
}

fn color_number_f64(state: &DumpState, value: f64) -> String {
    // Match C++ format: integers print as integers, floats as floats.
    let s = format_f64(value);
    if !state.use_color {
        return s;
    }
    format!("{}{}{}", MAGENTA, s, RESET)
}

fn color_number_bool(state: &DumpState, value: bool) -> String {
    let s = if value { "true" } else { "false" };
    if !state.use_color {
        return s.to_string();
    }
    format!("{}{}{}", MAGENTA, s, RESET)
}

fn color_number_str(state: &DumpState, value: &str) -> String {
    if !state.use_color {
        return value.to_string();
    }
    format!("{}{}{}", MAGENTA, value, RESET)
}

fn color_op(state: &DumpState, op: &str) -> String {
    if !state.use_color {
        return format!("({})", op);
    }
    format!("({}{}{})", YELLOW, op, RESET)
}

fn color_label(state: &DumpState, label: &str) -> String {
    if !state.use_color {
        return label.to_string();
    }
    format!("{}{}{}", DIM, label, RESET)
}

fn color_local(state: &DumpState, kind: &str, index: u32) -> String {
    if !state.use_color {
        return format!("[{}:{}]", kind, index);
    }
    format!("{}[{}:{}]{}", CYAN, kind, index, RESET)
}

fn color_global(state: &DumpState) -> String {
    if !state.use_color {
        return "[global]".to_string();
    }
    format!("{}[global]{}", YELLOW, RESET)
}

fn color_flag(state: &DumpState, flag: &str) -> String {
    if !state.use_color {
        return format!("[{}]", flag);
    }
    format!("{}[{}]{}", DIM, flag, RESET)
}

/// Convert UTF-16 to a valid UTF-8 string, replacing lone surrogates with U+FFFD.
fn utf16_to_string(s: &[u16]) -> String {
    char::decode_utf16(s.iter().copied())
        .map(|r| r.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect()
}

/// Format f64 matching the C++ AK::Formatter<double> output exactly.
///
/// FIXME: This calls into C++ via FFI to guarantee identical output.
///        Once the C++ pipeline is removed, this can be replaced with
///        a native implementation.
fn format_f64(value: f64) -> String {
    // C++ AST dump formats JS::Value which uses to_string_without_side_effects(),
    // producing "Infinity"/"-Infinity"/"NaN". The rust_format_double FFI uses
    // AK's double formatter which produces "inf"/"-inf"/"nan" instead.
    if value.is_nan() {
        return "NaN".to_string();
    }
    if value.is_infinite() {
        return if value > 0.0 {
            "Infinity".to_string()
        } else {
            "-Infinity".to_string()
        };
    }
    let mut buffer = [0u8; 128];
    let length = unsafe { rust_format_double(value, buffer.as_mut_ptr(), buffer.len()) };
    std::str::from_utf8(&buffer[..length])
        .expect("C++ produced invalid UTF-8")
        .to_string()
}

op_to_string!(binary_op_to_string, BinaryOp, {
    Addition => "+", Subtraction => "-", Multiplication => "*", Division => "/",
    Modulo => "%", Exponentiation => "**", StrictlyEquals => "===",
    StrictlyInequals => "!==", LooselyEquals => "==", LooselyInequals => "!=",
    GreaterThan => ">", GreaterThanEquals => ">=", LessThan => "<",
    LessThanEquals => "<=", BitwiseAnd => "&", BitwiseOr => "|", BitwiseXor => "^",
    LeftShift => "<<", RightShift => ">>", UnsignedRightShift => ">>>",
    In => "in", InstanceOf => "instanceof",
});

op_to_string!(logical_op_to_string, LogicalOp, {
    And => "&&", Or => "||", NullishCoalescing => "??",
});

op_to_string!(unary_op_to_string, UnaryOp, {
    BitwiseNot => "~", Not => "!", Plus => "+", Minus => "-",
    Typeof => "typeof", Void => "void", Delete => "delete",
});

op_to_string!(assignment_op_to_string, AssignmentOp, {
    Assignment => "=", AdditionAssignment => "+=", SubtractionAssignment => "-=",
    MultiplicationAssignment => "*=", DivisionAssignment => "/=",
    ModuloAssignment => "%=", ExponentiationAssignment => "**=",
    BitwiseAndAssignment => "&=", BitwiseOrAssignment => "|=",
    BitwiseXorAssignment => "^=", LeftShiftAssignment => "<<=",
    RightShiftAssignment => ">>=", UnsignedRightShiftAssignment => ">>>=",
    AndAssignment => "&&=", OrAssignment => "||=", NullishAssignment => "??=",
});

op_to_string!(update_op_to_string, UpdateOp, {
    Increment => "++", Decrement => "--",
});

op_to_string!(declaration_kind_to_string, DeclarationKind, {
    Let => "let", Var => "var", Const => "const",
});

op_to_string!(optional_mode_str, OptionalChainMode, {
    Optional => "optional", NotOptional => "not optional",
});

op_to_string!(class_method_kind_to_string, ClassMethodKind, {
    Method => "method", Getter => "getter", Setter => "setter",
});

fn dump_labeled_expression(label: &str, expression: &Expression, is_last: bool, state: &DumpState) {
    let label_state = child_state(state, is_last);
    print_node(&label_state, &color_label(state, label));
    dump_expression(expression, &child_state(&label_state, true));
}

fn dump_labeled_statement(label: &str, statement: &Statement, is_last: bool, state: &DumpState) {
    let label_state = child_state(state, is_last);
    print_node(&label_state, &color_label(state, label));
    dump_statement(statement, &child_state(&label_state, true));
}

// ============================================================================
// Entry point
// ============================================================================

pub fn dump_program(program: &Statement, use_color: bool, function_table: &FunctionTable) {
    let state = DumpState {
        prefix: String::new(),
        is_last: false,
        is_root: true,
        use_color,
        output: None,
        function_table,
    };
    dump_statement(program, &state);
    println!();
}

pub fn dump_program_to_string(program: &Statement, function_table: &FunctionTable) -> String {
    let output = RefCell::new(String::new());
    let state = DumpState {
        prefix: String::new(),
        is_last: false,
        is_root: true,
        use_color: false,
        output: Some(&output),
        function_table,
    };
    dump_statement(program, &state);
    output.into_inner()
}

// ============================================================================
// Statement dumpers
// ============================================================================

fn dump_statement(statement: &Statement, state: &DumpState) {
    match &statement.inner {
        StatementKind::Empty => {
            dump_node!(state, "EmptyStatement", &statement.range);
        }

        StatementKind::Debugger => {
            dump_node!(state, "DebuggerStatement", &statement.range);
        }

        StatementKind::Expression(expression) => {
            dump_node!(state, "ExpressionStatement", &statement.range);
            dump_expression(expression, &child_state(state, true));
        }

        StatementKind::Block(scope) => {
            let s = scope.borrow();
            // The parser wraps for-loops in a Block for scope. The C++
            // parser does not, so skip the wrapper and dump the child directly.
            if s.children.len() == 1
                && matches!(
                    s.children[0].inner,
                    StatementKind::For { .. } | StatementKind::ForInOf { .. }
                )
            {
                dump_statement(&s.children[0], state);
                return;
            }
            dump_scope_node("BlockStatement", &s, &statement.range, state);
        }

        StatementKind::FunctionBody { scope, .. } => {
            let s = scope.borrow();
            dump_scope_node("FunctionBody", &s, &statement.range, state);
        }

        StatementKind::Program(data) => {
            let scope = data.scope.borrow();
            let mut desc = color_node_name(state, "Program");
            let type_str = if data.program_type == ProgramType::Module {
                "module"
            } else {
                "script"
            };
            desc.push_str(&format!(" {}", color_op(state, type_str)));
            if data.is_strict_mode {
                desc.push_str(&format!(" {}", color_flag(state, "strict")));
            }
            if data.has_top_level_await {
                desc.push_str(&format!(" {}", color_flag(state, "top-level-await")));
            }
            desc.push_str(&format_position(state, &statement.range));
            print_node(state, &desc);
            let children = &scope.children;
            for (i, child) in children.iter().enumerate() {
                dump_statement(child, &child_state(state, i == children.len() - 1));
            }
        }

        StatementKind::If {
            test,
            consequent,
            alternate,
        } => {
            dump_node!(state, "IfStatement", &statement.range);
            let has_alternate = alternate.is_some();
            dump_labeled_expression("test", test, false, state);
            dump_labeled_statement("consequent", consequent, !has_alternate, state);
            if let Some(alt) = alternate {
                dump_labeled_statement("alternate", alt, true, state);
            }
        }

        StatementKind::While { test, body } => {
            dump_node!(state, "WhileStatement", &statement.range);
            dump_labeled_expression("test", test, false, state);
            dump_labeled_statement("body", body, true, state);
        }

        StatementKind::DoWhile { test, body } => {
            dump_node!(state, "DoWhileStatement", &statement.range);
            dump_labeled_statement("body", body, false, state);
            dump_labeled_expression("test", test, true, state);
        }

        StatementKind::For {
            init,
            test,
            update,
            body,
        } => {
            dump_node!(state, "ForStatement", &statement.range);
            if let Some(init) = init {
                let init_state = child_state(state, false);
                print_node(&init_state, &color_label(state, "init"));
                match init {
                    ForInit::Expression(expr) => {
                        dump_expression(expr, &child_state(&init_state, true))
                    }
                    ForInit::Declaration(decl) => {
                        dump_statement(decl, &child_state(&init_state, true))
                    }
                }
            }
            if let Some(test) = test {
                dump_labeled_expression("test", test, false, state);
            }
            if let Some(update) = update {
                dump_labeled_expression("update", update, false, state);
            }
            dump_labeled_statement("body", body, true, state);
        }

        StatementKind::ForInOf {
            kind,
            lhs,
            rhs,
            body,
        } => {
            let name = match kind {
                ForInOfKind::ForIn => "ForInStatement",
                ForInOfKind::ForOf => "ForOfStatement",
                ForInOfKind::ForAwaitOf => "ForAwaitOfStatement",
            };
            dump_node!(state, name, &statement.range);
            let lhs_state = child_state(state, false);
            print_node(&lhs_state, &color_label(state, "lhs"));
            dump_for_in_of_lhs(lhs, &child_state(&lhs_state, true));
            dump_labeled_expression("rhs", rhs, false, state);
            dump_labeled_statement("body", body, true, state);
        }

        StatementKind::Switch(data) => {
            dump_node!(state, "SwitchStatement", &statement.range);
            dump_labeled_expression(
                "discriminant",
                &data.discriminant,
                data.cases.is_empty(),
                state,
            );
            for (i, case) in data.cases.iter().enumerate() {
                dump_switch_case(case, &child_state(state, i == data.cases.len() - 1), state);
            }
        }

        StatementKind::With { object, body } => {
            dump_node!(state, "WithStatement", &statement.range);
            dump_labeled_expression("object", object, false, state);
            dump_labeled_statement("body", body, true, state);
        }

        StatementKind::Labelled { label, item } => {
            dump_node!(
                state,
                "LabelledStatement",
                &statement.range,
                color_string_utf16(state, label)
            );
            dump_statement(item, &child_state(state, true));
        }

        StatementKind::Break { .. } => {
            dump_node!(state, "BreakStatement", &statement.range);
        }

        StatementKind::Continue { .. } => {
            dump_node!(state, "ContinueStatement", &statement.range);
        }

        StatementKind::Return(argument) => {
            dump_node!(state, "ReturnStatement", &statement.range);
            if let Some(argument) = argument {
                dump_expression(argument, &child_state(state, true));
            }
        }

        StatementKind::Throw(argument) => {
            dump_node!(state, "ThrowStatement", &statement.range);
            dump_expression(argument, &child_state(state, true));
        }

        StatementKind::Try(data) => {
            dump_node!(state, "TryStatement", &statement.range);
            let has_handler = data.handler.is_some();
            let has_finalizer = data.finalizer.is_some();
            dump_labeled_statement("block", &data.block, !has_handler && !has_finalizer, state);
            if let Some(ref handler) = data.handler {
                let handler_state = child_state(state, !has_finalizer);
                print_node(&handler_state, &color_label(state, "handler"));
                dump_catch_clause(handler, &child_state(&handler_state, true), state);
            }
            if let Some(ref finalizer) = data.finalizer {
                dump_labeled_statement("finalizer", finalizer, true, state);
            }
        }

        StatementKind::VariableDeclaration { kind, declarations } => {
            dump_node!(
                state,
                "VariableDeclaration",
                &statement.range,
                color_op(state, declaration_kind_to_string(*kind))
            );
            for (i, declaration) in declarations.iter().enumerate() {
                dump_variable_declarator(
                    declaration,
                    &child_state(state, i == declarations.len() - 1),
                    state,
                );
            }
        }

        StatementKind::UsingDeclaration { declarations } => {
            dump_node!(state, "UsingDeclaration", &statement.range);
            for (i, declaration) in declarations.iter().enumerate() {
                dump_variable_declarator(
                    declaration,
                    &child_state(state, i == declarations.len() - 1),
                    state,
                );
            }
        }

        StatementKind::FunctionDeclaration { function_id, .. } => {
            let function_data = state.function_table().get(*function_id);
            dump_function(
                function_data,
                "FunctionDeclaration",
                &statement.range,
                state,
            );
        }

        StatementKind::ClassDeclaration(class_data) => {
            dump_node!(state, "ClassDeclaration", &statement.range);
            dump_class(
                class_data,
                &statement.range,
                &child_state(state, true),
                state,
            );
        }

        StatementKind::Import(data) => {
            let module_spec = utf16_to_string(&data.module_request.module_specifier);
            let assert_clauses = format_assert_clauses(&data.module_request);
            dump_node!(
                state,
                "ImportStatement",
                &statement.range,
                format!(
                    "from {}{}",
                    color_string(state, &module_spec),
                    assert_clauses
                )
            );
            if !data.entries.is_empty() {
                for (i, entry) in data.entries.iter().enumerate() {
                    let import_name = match &entry.import_name {
                        Some(name) => utf16_to_string(name),
                        None => "None".to_string(),
                    };
                    let local_name = utf16_to_string(&entry.local_name);
                    print_node(
                        &child_state(state, i == data.entries.len() - 1),
                        &format!("ImportName: {}, LocalName: {}", import_name, local_name),
                    );
                }
            }
        }

        StatementKind::Export(data) => {
            dump_node!(state, "ExportStatement", &statement.range);

            let has_statement = data.statement.is_some();
            let has_entries = !data.entries.is_empty();

            if has_entries {
                print_node(
                    &child_state(state, !has_statement),
                    &color_label(state, "entries"),
                );
                let entries_state = child_state(state, !has_statement);
                for (i, entry) in data.entries.iter().enumerate() {
                    let export_name = match &entry.export_name {
                        Some(name) => format!("\"{}\"", utf16_to_string(name)),
                        None => "null".to_string(),
                    };
                    // When the entry is a module re-export, C++ prints
                    // "null" for LocalName regardless of the stored value.
                    let local_name = if data.module_request.is_some() {
                        "null".to_string()
                    } else {
                        match &entry.local_or_import_name {
                            Some(name) => format!("\"{}\"", utf16_to_string(name)),
                            None => "null".to_string(),
                        }
                    };
                    let mut desc =
                        format!("ExportName: {}, LocalName: {}", export_name, local_name);
                    if let Some(ref module_request) = data.module_request {
                        desc.push_str(&format!(
                            ", ModuleRequest: {}{}",
                            utf16_to_string(&module_request.module_specifier),
                            format_assert_clauses(module_request)
                        ));
                    }
                    print_node(
                        &child_state(&entries_state, i == data.entries.len() - 1),
                        &desc,
                    );
                }
            }

            if let Some(ref statement) = data.statement {
                print_node(&child_state(state, true), &color_label(state, "statement"));
                let inner_state = &child_state(&child_state(state, true), true);
                // For `export default <expression>`, the C++ AST stores the
                // expression directly without an ExpressionStatement wrapper.
                // Match that by unwrapping StatementKind::Expression here.
                if let StatementKind::Expression(ref expression) = statement.inner {
                    dump_expression(expression, inner_state);
                } else {
                    dump_statement(statement, inner_state);
                }
            }
        }

        StatementKind::ClassFieldInitializer { .. } => {
            // This should not be dumped as it is never part of an actual AST.
        }

        StatementKind::Error | StatementKind::ErrorDeclaration => {
            dump_node!(state, "ErrorStatement", &statement.range);
        }
    }
}

// ============================================================================
// Expression dumpers
// ============================================================================

fn dump_expression(expression: &Expression, state: &DumpState) {
    match &expression.inner {
        ExpressionKind::NumericLiteral(value) => {
            dump_node!(
                state,
                "NumericLiteral",
                &expression.range,
                color_number_f64(state, *value)
            );
        }

        ExpressionKind::StringLiteral(value) => {
            dump_node!(
                state,
                "StringLiteral",
                &expression.range,
                color_string_utf16(state, value)
            );
        }

        ExpressionKind::BooleanLiteral(value) => {
            dump_node!(
                state,
                "BooleanLiteral",
                &expression.range,
                color_number_bool(state, *value)
            );
        }

        ExpressionKind::NullLiteral => {
            dump_node!(state, "NullLiteral", &expression.range);
        }

        ExpressionKind::BigIntLiteral(value) => {
            dump_node!(
                state,
                "BigIntLiteral",
                &expression.range,
                color_number_str(state, value)
            );
        }

        ExpressionKind::RegExpLiteral(data) => {
            let pattern = utf16_to_string(&data.pattern);
            let flags = utf16_to_string(&data.flags);
            dump_node!(
                state,
                "RegExpLiteral",
                &expression.range,
                format!("/{}/{}", pattern, flags)
            );
        }

        ExpressionKind::Identifier(ident) => {
            dump_identifier(ident, &expression.range, state);
        }

        ExpressionKind::PrivateIdentifier(ident) => {
            dump_node!(
                state,
                "PrivateIdentifier",
                &expression.range,
                color_string_utf16(state, &ident.name)
            );
        }

        ExpressionKind::Binary { op, lhs, rhs } => {
            dump_node!(
                state,
                "BinaryExpression",
                &expression.range,
                color_op(state, binary_op_to_string(*op))
            );
            dump_expression(lhs, &child_state(state, false));
            dump_expression(rhs, &child_state(state, true));
        }

        ExpressionKind::Logical { op, lhs, rhs } => {
            dump_node!(
                state,
                "LogicalExpression",
                &expression.range,
                color_op(state, logical_op_to_string(*op))
            );
            dump_expression(lhs, &child_state(state, false));
            dump_expression(rhs, &child_state(state, true));
        }

        ExpressionKind::Unary { op, operand } => {
            dump_node!(
                state,
                "UnaryExpression",
                &expression.range,
                color_op(state, unary_op_to_string(*op))
            );
            dump_expression(operand, &child_state(state, true));
        }

        ExpressionKind::Update {
            op,
            argument,
            prefixed,
        } => {
            let prefix_str = if *prefixed { "prefix" } else { "postfix" };
            dump_node!(
                state,
                "UpdateExpression",
                &expression.range,
                format!("({}, {})", update_op_to_string(*op), prefix_str)
            );
            dump_expression(argument, &child_state(state, true));
        }

        ExpressionKind::Assignment { op, lhs, rhs } => {
            dump_node!(
                state,
                "AssignmentExpression",
                &expression.range,
                color_op(state, assignment_op_to_string(*op))
            );
            match lhs {
                AssignmentLhs::Expression(expression) => {
                    dump_expression(expression, &child_state(state, false));
                }
                AssignmentLhs::Pattern(pattern) => {
                    dump_binding_pattern(pattern, &child_state(state, false), state);
                }
            }
            dump_expression(rhs, &child_state(state, true));
        }

        ExpressionKind::Conditional {
            test,
            consequent,
            alternate,
        } => {
            dump_node!(state, "ConditionalExpression", &expression.range);
            dump_labeled_expression("test", test, false, state);
            dump_labeled_expression("consequent", consequent, false, state);
            dump_labeled_expression("alternate", alternate, true, state);
        }

        ExpressionKind::Sequence(expressions) => {
            dump_node!(state, "SequenceExpression", &expression.range);
            for (i, child) in expressions.iter().enumerate() {
                dump_expression(child, &child_state(state, i == expressions.len() - 1));
            }
        }

        ExpressionKind::Member {
            object,
            property,
            computed,
        } => {
            let name = if *computed {
                "MemberExpression [computed]"
            } else {
                "MemberExpression"
            };
            dump_node!(state, name, &expression.range);
            dump_expression(object, &child_state(state, false));
            dump_expression(property, &child_state(state, true));
        }

        ExpressionKind::OptionalChain { base, references } => {
            dump_node!(state, "OptionalChain", &expression.range);
            dump_expression(base, &child_state(state, references.is_empty()));
            for (i, reference) in references.iter().enumerate() {
                let ref_state = child_state(state, i == references.len() - 1);
                match reference {
                    OptionalChainReference::Call { arguments, mode } => {
                        print_node(&ref_state, &format!("Call({})", optional_mode_str(*mode)));
                        for (j, argument) in arguments.iter().enumerate() {
                            dump_expression(
                                &argument.value,
                                &child_state(&ref_state, j == arguments.len() - 1),
                            );
                        }
                    }
                    OptionalChainReference::ComputedReference { expression, mode } => {
                        print_node(
                            &ref_state,
                            &format!("ComputedReference({})", optional_mode_str(*mode)),
                        );
                        dump_expression(expression, &child_state(&ref_state, true));
                    }
                    OptionalChainReference::MemberReference { identifier, mode } => {
                        print_node(
                            &ref_state,
                            &format!("MemberReference({})", optional_mode_str(*mode)),
                        );
                        dump_identifier(
                            identifier,
                            &identifier.range,
                            &child_state(&ref_state, true),
                        );
                    }
                    OptionalChainReference::PrivateMemberReference {
                        private_identifier,
                        mode,
                    } => {
                        print_node(
                            &ref_state,
                            &format!("PrivateMemberReference({})", optional_mode_str(*mode)),
                        );
                        print_node(
                            &child_state(&ref_state, true),
                            &format!(
                                "{} {}{}",
                                color_node_name(state, "PrivateIdentifier"),
                                color_string_utf16(state, &private_identifier.name),
                                format_position(state, &private_identifier.range)
                            ),
                        );
                    }
                }
            }
        }

        ExpressionKind::Call(data) => {
            dump_node!(state, "CallExpression", &expression.range);
            dump_expression(&data.callee, &child_state(state, data.arguments.is_empty()));
            for (i, argument) in data.arguments.iter().enumerate() {
                dump_expression(
                    &argument.value,
                    &child_state(state, i == data.arguments.len() - 1),
                );
            }
        }

        ExpressionKind::New(data) => {
            dump_node!(state, "NewExpression", &expression.range);
            dump_expression(&data.callee, &child_state(state, data.arguments.is_empty()));
            for (i, argument) in data.arguments.iter().enumerate() {
                dump_expression(
                    &argument.value,
                    &child_state(state, i == data.arguments.len() - 1),
                );
            }
        }

        ExpressionKind::SuperCall(data) => {
            dump_node!(state, "SuperCall", &expression.range);
            for (i, argument) in data.arguments.iter().enumerate() {
                dump_expression(
                    &argument.value,
                    &child_state(state, i == data.arguments.len() - 1),
                );
            }
        }

        ExpressionKind::Spread(target) => {
            dump_node!(state, "SpreadExpression", &expression.range);
            dump_expression(target, &child_state(state, true));
        }

        ExpressionKind::This => {
            dump_node!(state, "ThisExpression", &expression.range);
        }

        ExpressionKind::Super => {
            dump_node!(state, "SuperExpression", &expression.range);
        }

        ExpressionKind::Function(function_id) => {
            let function_data = state.function_table().get(*function_id);
            dump_function(
                function_data,
                "FunctionExpression",
                &expression.range,
                state,
            );
        }

        ExpressionKind::Class(class_data) => {
            dump_class(class_data, &expression.range, state, state);
        }

        ExpressionKind::Array(elements) => {
            dump_node!(state, "ArrayExpression", &expression.range);
            for (i, element) in elements.iter().enumerate() {
                let cs = child_state(state, i == elements.len() - 1);
                if let Some(element) = element {
                    dump_expression(element, &cs);
                } else {
                    print_node(&cs, "<elision>");
                }
            }
        }

        ExpressionKind::Object(properties) => {
            dump_node!(state, "ObjectExpression", &expression.range);
            for (i, property) in properties.iter().enumerate() {
                dump_object_property(
                    property,
                    &child_state(state, i == properties.len() - 1),
                    state,
                );
            }
        }

        ExpressionKind::TemplateLiteral(data) => {
            dump_node!(state, "TemplateLiteral", &expression.range);
            for (i, child) in data.expressions.iter().enumerate() {
                dump_expression(child, &child_state(state, i == data.expressions.len() - 1));
            }
        }

        ExpressionKind::TaggedTemplateLiteral {
            tag,
            template_literal,
        } => {
            dump_node!(state, "TaggedTemplateLiteral", &expression.range);
            dump_labeled_expression("tag", tag, false, state);
            dump_labeled_expression("template", template_literal, true, state);
        }

        ExpressionKind::MetaProperty(meta_type) => {
            let name = match meta_type {
                MetaPropertyType::NewTarget => "new.target",
                MetaPropertyType::ImportMeta => "import.meta",
            };
            dump_node!(state, "MetaProperty", &expression.range, name);
        }

        ExpressionKind::ImportCall { specifier, options } => {
            dump_node!(state, "ImportCall", &expression.range);
            dump_expression(specifier, &child_state(state, options.is_none()));
            if let Some(opts) = options {
                dump_labeled_expression("options", opts, true, state);
            }
        }

        ExpressionKind::Yield {
            argument,
            is_yield_from,
        } => {
            let mut desc = color_node_name(state, "YieldExpression");
            if *is_yield_from {
                desc.push_str(&format!(" {}", color_flag(state, "yield*")));
            }
            desc.push_str(&format_position(state, &expression.range));
            print_node(state, &desc);
            if let Some(argument) = argument {
                dump_expression(argument, &child_state(state, true));
            }
        }

        ExpressionKind::Await(argument) => {
            dump_node!(state, "AwaitExpression", &expression.range);
            dump_expression(argument, &child_state(state, true));
        }

        ExpressionKind::Error => {
            dump_node!(state, "ErrorExpression", &expression.range);
        }
    }
}

// ============================================================================
// Identifier dumper
// ============================================================================

fn dump_identifier(ident: &Identifier, range: &SourceRange, state: &DumpState) {
    let mut desc = color_node_name(state, "Identifier");
    desc.push_str(&format!(" {}", color_string_utf16(state, &ident.name)));
    if ident.is_local() {
        let kind = if ident.local_type.get() == Some(LocalType::Argument) {
            "argument"
        } else {
            "variable"
        };
        desc.push_str(&format!(
            " {}",
            color_local(state, kind, ident.local_index.get())
        ));
    } else if ident.is_global.get() {
        desc.push_str(&format!(" {}", color_global(state)));
    }
    if let Some(declaration_kind) = ident.declaration_kind.get() {
        desc.push_str(&format!(
            " {}",
            color_op(state, declaration_kind_to_string(declaration_kind))
        ));
    }
    if ident.is_inside_scope_with_eval.get() {
        desc.push_str(&format!(" {}", color_flag(state, "in-eval-scope")));
    }
    desc.push_str(&format_position(state, range));
    print_node(state, &desc);
}

// ============================================================================
// Helper dumpers
// ============================================================================

fn dump_scope_node(class_name: &str, scope: &ScopeData, range: &SourceRange, state: &DumpState) {
    dump_node!(state, class_name, range);
    for (i, child) in scope.children.iter().enumerate() {
        dump_statement(child, &child_state(state, i == scope.children.len() - 1));
    }
}

fn dump_function(
    function_data: &FunctionData,
    class_name: &str,
    range: &SourceRange,
    state: &DumpState,
) {
    let mut desc = color_node_name(state, class_name);
    let is_async = function_data.kind == FunctionKind::Async
        || function_data.kind == FunctionKind::AsyncGenerator;
    let is_generator = function_data.kind == FunctionKind::Generator
        || function_data.kind == FunctionKind::AsyncGenerator;
    if is_async {
        desc.push_str(" async");
    }
    if is_generator {
        desc.push('*');
    }
    let name_str = match &function_data.name {
        Some(ident) => utf16_to_string(&ident.name),
        None => String::new(),
    };
    desc.push_str(&format!(" {}", color_string(state, &name_str)));
    if function_data.is_strict_mode {
        desc.push_str(&format!(" {}", color_flag(state, "strict")));
    }
    if function_data.is_arrow_function {
        desc.push_str(&format!(" {}", color_flag(state, "arrow")));
    }
    if function_data.parsing_insights.contains_direct_call_to_eval {
        desc.push_str(&format!(" {}", color_flag(state, "direct-eval")));
    }
    if function_data.parsing_insights.uses_this {
        desc.push_str(&format!(" {}", color_flag(state, "uses-this")));
    }
    if function_data.parsing_insights.uses_this_from_environment {
        desc.push_str(&format!(
            " {}",
            color_flag(state, "uses-this-from-environment")
        ));
    }
    if function_data.parsing_insights.might_need_arguments_object {
        desc.push_str(&format!(" {}", color_flag(state, "might-need-arguments")));
    }
    desc.push_str(&format_position(state, range));
    print_node(state, &desc);

    if !function_data.parameters.is_empty() {
        print_node(
            &child_state(state, false),
            &color_label(state, "parameters"),
        );
        let parameters_state = child_state(state, false);
        for (i, parameter) in function_data.parameters.iter().enumerate() {
            let parameter_state =
                child_state(&parameters_state, i == function_data.parameters.len() - 1);
            let has_default = parameter.default_value.is_some();
            if parameter.is_rest {
                print_node(&parameter_state, &color_label(state, "rest"));
                match &parameter.binding {
                    FunctionParameterBinding::Identifier(ident) => {
                        dump_identifier(
                            ident,
                            &ident.range,
                            &child_state(&parameter_state, !has_default),
                        );
                    }
                    FunctionParameterBinding::BindingPattern(pattern) => {
                        dump_binding_pattern(
                            pattern,
                            &child_state(&parameter_state, !has_default),
                            state,
                        );
                    }
                }
            } else {
                match &parameter.binding {
                    FunctionParameterBinding::Identifier(ident) => {
                        dump_identifier(
                            ident,
                            &ident.range,
                            &child_state(
                                &parameters_state,
                                i == function_data.parameters.len() - 1,
                            ),
                        );
                    }
                    FunctionParameterBinding::BindingPattern(pattern) => {
                        dump_binding_pattern(
                            pattern,
                            &child_state(
                                &parameters_state,
                                i == function_data.parameters.len() - 1,
                            ),
                            state,
                        );
                    }
                }
            }
            if has_default {
                print_node(
                    &child_state(&parameter_state, true),
                    &color_label(state, "default"),
                );
                dump_expression(
                    parameter
                        .default_value
                        .as_ref()
                        .expect("guarded by is_some check"),
                    &child_state(&child_state(&parameter_state, true), true),
                );
            }
        }
    }

    print_node(&child_state(state, true), &color_label(state, "body"));
    dump_statement(
        &function_data.body,
        &child_state(&child_state(state, true), true),
    );
}

fn dump_class(
    class_data: &ClassData,
    range: &SourceRange,
    state: &DumpState,
    root_state: &DumpState,
) {
    let name_str = match &class_data.name {
        Some(ident) => utf16_to_string(&ident.name),
        None => String::new(),
    };
    print_node(
        state,
        &format!(
            "{} {}{}",
            color_node_name(root_state, "ClassExpression"),
            color_string(root_state, &name_str),
            format_position(root_state, range),
        ),
    );
    let has_super = class_data.super_class.is_some();
    let has_elements = !class_data.elements.is_empty();

    if has_super {
        print_node(
            &child_state(state, false),
            &color_label(root_state, "super class"),
        );
        dump_expression(
            class_data
                .super_class
                .as_ref()
                .expect("guarded by has_super_class check"),
            &child_state(&child_state(state, false), true),
        );
    }

    if let Some(ref constructor) = class_data.constructor {
        print_node(
            &child_state(state, !has_elements),
            &color_label(root_state, "constructor"),
        );
        dump_expression(
            constructor,
            &child_state(&child_state(state, !has_elements), true),
        );
    }

    if has_elements {
        print_node(
            &child_state(state, true),
            &color_label(root_state, "elements"),
        );
        for (i, element) in class_data.elements.iter().enumerate() {
            dump_class_element(
                &element.inner,
                &element.range,
                &child_state(
                    &child_state(state, true),
                    i == class_data.elements.len() - 1,
                ),
                root_state,
            );
        }
    }
}

fn dump_class_element(
    element: &ClassElement,
    range: &SourceRange,
    state: &DumpState,
    root_state: &DumpState,
) {
    match element {
        ClassElement::Method {
            key,
            function,
            kind,
            is_static,
        } => {
            let mut desc = color_node_name(root_state, "ClassMethod");
            if *is_static {
                desc.push_str(" static");
            }
            if *kind != ClassMethodKind::Method {
                desc.push_str(&format!(
                    " {}",
                    color_op(root_state, class_method_kind_to_string(*kind))
                ));
            }
            desc.push_str(&format_position(root_state, range));
            print_node(state, &desc);
            dump_expression(key, &child_state(state, false));
            dump_expression(function, &child_state(state, true));
        }
        ClassElement::Field {
            key,
            initializer,
            is_static,
        } => {
            let mut desc = color_node_name(root_state, "ClassField");
            if *is_static {
                desc.push_str(" static");
            }
            desc.push_str(&format_position(root_state, range));
            print_node(state, &desc);
            dump_expression(key, &child_state(state, initializer.is_none()));
            if let Some(init) = initializer {
                print_node(
                    &child_state(state, true),
                    &color_label(root_state, "initializer"),
                );
                dump_expression(init, &child_state(&child_state(state, true), true));
            }
        }
        ClassElement::StaticInitializer { body } => {
            print_node(
                state,
                &format!(
                    "{}{}",
                    color_node_name(root_state, "StaticInitializer"),
                    format_position(root_state, range)
                ),
            );
            dump_statement(body, &child_state(state, true));
        }
    }
}

fn dump_binding_pattern(pattern: &BindingPattern, state: &DumpState, root_state: &DumpState) {
    let kind_str = match pattern.kind {
        BindingPatternKind::Array => "array",
        BindingPatternKind::Object => "object",
    };
    print_node(
        state,
        &format!(
            "{} {}",
            color_node_name(root_state, "BindingPattern"),
            color_op(root_state, kind_str)
        ),
    );

    for (i, entry) in pattern.entries.iter().enumerate() {
        let entry_state = child_state(state, i == pattern.entries.len() - 1);

        if pattern.kind == BindingPatternKind::Array && is_elision(entry) {
            print_node(&entry_state, &color_node_name(root_state, "Elision"));
            continue;
        }

        let mut label = "entry".to_string();
        if entry.is_rest {
            label.push_str(" (rest)");
        }
        print_node(&entry_state, &color_label(root_state, &label));

        let has_alias = entry.alias.is_some();
        let has_initializer = entry.initializer.is_some();

        if pattern.kind == BindingPatternKind::Object {
            match &entry.name {
                Some(BindingEntryName::Identifier(ident)) => {
                    print_node(
                        &child_state(&entry_state, !has_alias && !has_initializer),
                        &color_label(root_state, "name"),
                    );
                    dump_identifier(
                        ident,
                        &ident.range,
                        &child_state(
                            &child_state(&entry_state, !has_alias && !has_initializer),
                            true,
                        ),
                    );
                }
                Some(BindingEntryName::Expression(expression)) => {
                    print_node(
                        &child_state(&entry_state, !has_alias && !has_initializer),
                        &color_label(root_state, "name (computed)"),
                    );
                    dump_expression(
                        expression,
                        &child_state(
                            &child_state(&entry_state, !has_alias && !has_initializer),
                            true,
                        ),
                    );
                }
                None => {}
            }
        }

        if let Some(ref alias) = entry.alias {
            print_node(
                &child_state(&entry_state, !has_initializer),
                &color_label(root_state, "alias"),
            );
            match alias {
                BindingEntryAlias::Identifier(ident) => {
                    dump_identifier(
                        ident,
                        &ident.range,
                        &child_state(&child_state(&entry_state, !has_initializer), true),
                    );
                }
                BindingEntryAlias::BindingPattern(sub) => {
                    dump_binding_pattern(
                        sub,
                        &child_state(&child_state(&entry_state, !has_initializer), true),
                        root_state,
                    );
                }
                BindingEntryAlias::MemberExpression(expression) => {
                    dump_expression(
                        expression,
                        &child_state(&child_state(&entry_state, !has_initializer), true),
                    );
                }
            }
        }

        if has_initializer {
            print_node(
                &child_state(&entry_state, true),
                &color_label(root_state, "initializer"),
            );
            dump_expression(
                entry
                    .initializer
                    .as_ref()
                    .expect("guarded by is_some check"),
                &child_state(&child_state(&entry_state, true), true),
            );
        }
    }
}

fn is_elision(entry: &BindingEntry) -> bool {
    entry.name.is_none() && entry.alias.is_none() && entry.initializer.is_none() && !entry.is_rest
}

fn dump_variable_declarator(
    declaration: &VariableDeclarator,
    state: &DumpState,
    root_state: &DumpState,
) {
    print_node(
        state,
        &format!(
            "{}{}",
            color_node_name(root_state, "VariableDeclarator"),
            format_position(root_state, &declaration.range)
        ),
    );
    let has_init = declaration.init.is_some();
    match &declaration.target {
        VariableDeclaratorTarget::Identifier(ident) => {
            dump_identifier(ident, &ident.range, &child_state(state, !has_init));
        }
        VariableDeclaratorTarget::BindingPattern(pattern) => {
            dump_binding_pattern(pattern, &child_state(state, !has_init), root_state);
        }
    }
    if let Some(ref init) = declaration.init {
        dump_expression(init, &child_state(state, true));
    }
}

fn dump_object_property(property: &ObjectProperty, state: &DumpState, root_state: &DumpState) {
    if property.property_type == ObjectPropertyType::Spread {
        print_node(
            state,
            &format!(
                "{} {}{}",
                color_node_name(root_state, "ObjectProperty"),
                color_op(root_state, "spread"),
                format_position(root_state, &property.range)
            ),
        );
        dump_expression(&property.key, &child_state(state, true));
    } else {
        let mut desc = color_node_name(root_state, "ObjectProperty");
        if property.is_method {
            desc.push_str(&format!(" {}", color_op(root_state, "method")));
        } else if property.property_type == ObjectPropertyType::Getter {
            desc.push_str(&format!(" {}", color_op(root_state, "getter")));
        } else if property.property_type == ObjectPropertyType::Setter {
            desc.push_str(&format!(" {}", color_op(root_state, "setter")));
        }
        desc.push_str(&format_position(root_state, &property.range));
        print_node(state, &desc);
        dump_expression(&property.key, &child_state(state, false));
        if let Some(ref value) = property.value {
            dump_expression(value, &child_state(state, true));
        }
    }
}

fn dump_catch_clause(clause: &CatchClause, state: &DumpState, root_state: &DumpState) {
    print_node(
        state,
        &format!(
            "{}{}",
            color_node_name(root_state, "CatchClause"),
            format_position(root_state, &clause.range)
        ),
    );
    if let Some(parameter) = &clause.parameter {
        match parameter {
            CatchBinding::Identifier(ident) => {
                print_node(
                    &child_state(state, false),
                    &color_label(root_state, "parameter"),
                );
                dump_identifier(
                    ident,
                    &ident.range,
                    &child_state(&child_state(state, false), true),
                );
            }
            CatchBinding::BindingPattern(pattern) => {
                print_node(
                    &child_state(state, false),
                    &color_label(root_state, "parameter"),
                );
                dump_binding_pattern(
                    pattern,
                    &child_state(&child_state(state, false), true),
                    root_state,
                );
            }
        }
    }
    dump_statement(&clause.body, &child_state(state, true));
}

fn dump_switch_case(case: &SwitchCase, state: &DumpState, root_state: &DumpState) {
    if let Some(ref test) = case.test {
        print_node(
            state,
            &format!(
                "{}{}",
                color_node_name(root_state, "SwitchCase"),
                format_position(root_state, &case.range)
            ),
        );
        print_node(&child_state(state, false), &color_label(root_state, "test"));
        dump_expression(test, &child_state(&child_state(state, false), true));
    } else {
        print_node(
            state,
            &format!(
                "{} {}{}",
                color_node_name(root_state, "SwitchCase"),
                color_op(root_state, "default"),
                format_position(root_state, &case.range)
            ),
        );
    }
    print_node(
        &child_state(state, true),
        &color_label(root_state, "consequent"),
    );
    let consequent_state = child_state(&child_state(state, true), true);
    let scope = case.scope.borrow();
    let children = &scope.children;
    for (i, child) in children.iter().enumerate() {
        dump_statement(
            child,
            &child_state(&consequent_state, i == children.len() - 1),
        );
    }
}

fn dump_for_in_of_lhs(lhs: &ForInOfLhs, state: &DumpState) {
    match lhs {
        ForInOfLhs::Declaration(declaration) => dump_statement(declaration, state),
        ForInOfLhs::Expression(expression) => dump_expression(expression, state),
        ForInOfLhs::Pattern(pattern) => {
            dump_binding_pattern(pattern, state, state);
        }
    }
}

fn format_assert_clauses(request: &ModuleRequest) -> String {
    if request.attributes.is_empty() {
        return String::new();
    }
    let mut result = " [".to_string();
    for (i, attr) in request.attributes.iter().enumerate() {
        if i > 0 {
            result.push_str(", ");
        }
        result.push_str(&format!(
            "{}: {}",
            utf16_to_string(&attr.key),
            utf16_to_string(&attr.value)
        ));
    }
    result.push(']');
    result
}
