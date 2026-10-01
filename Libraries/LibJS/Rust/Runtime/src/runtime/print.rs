/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Pretty printing of values for js-rust, mirroring Libraries/LibJS/Print.cpp.

use super::value::number_to_string;
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::layout::value::Value;
use crate::utf16::Utf16View;

pub struct PrintContext {
    pub strip_ansi: bool,
    pub raw_strings: bool,
}

fn write_escape(output: &mut String, context: &PrintContext, escape: &str) {
    if !context.strip_ansi {
        output.push_str(escape);
    }
}

fn escape_for_string_literal(output: &mut String, string: &str) {
    for character in string.chars() {
        match character {
            '\r' => output.push_str("\\r"),
            '\u{b}' => output.push_str("\\v"),
            '\u{c}' => output.push_str("\\f"),
            '\u{8}' => output.push_str("\\b"),
            '\n' => output.push_str("\\n"),
            '\\' => output.push_str("\\\\"),
            _ => output.push(character),
        }
    }
}

fn primitive_to_string_without_side_effects(value: Value) -> String {
    if value.is_number() {
        return number_to_string(value.as_f64());
    }
    if value.is_boolean() {
        return if value.as_bool() { "true" } else { "false" }.to_string();
    }
    if value.is_null() {
        return "null".to_string();
    }
    if value.is_undefined() {
        return "undefined".to_string();
    }
    if value.is_string() {
        return value.as_string().to_utf8();
    }
    if value.is_bigint() {
        return Utf16View::of_string(&value.as_bigint().to_utf16_string()).to_utf8();
    }
    if value.is_symbol() {
        return Utf16View::of_string(&value.as_symbol().descriptive_string()).to_utf8();
    }
    unimplemented_runtime_function("printing cells", 0)
}

pub fn print_value(output: &mut String, context: &PrintContext, value: Value) {
    if value.is_empty() {
        write_escape(output, context, "\x1b[34;1m");
        output.push_str("<empty>");
        write_escape(output, context, "\x1b[0m");
        return;
    }

    if value.is_string() {
        write_escape(output, context, "\x1b[32;1m");
    } else if value.is_number() || value.is_bigint() {
        write_escape(output, context, "\x1b[35;1m");
    } else if value.is_boolean() || value.is_null() {
        write_escape(output, context, "\x1b[33;1m");
    } else if value.is_undefined() {
        write_escape(output, context, "\x1b[34;1m");
    }

    let quote_string = value.is_string() && !context.raw_strings;
    if quote_string {
        output.push('"');
    } else if value.is_number() && value.as_f64() == 0.0 && value.as_f64().is_sign_negative() {
        output.push('-');
    }

    let contents = primitive_to_string_without_side_effects(value);
    if quote_string {
        escape_for_string_literal(output, &contents);
        output.push('"');
    } else {
        output.push_str(&contents);
    }
    write_escape(output, context, "\x1b[0m");
}
