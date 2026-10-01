/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Pretty printing of values for js-rust, mirroring Libraries/LibJS/Print.cpp.

use super::value::number_to_string;
use crate::layout::value::Value;

pub struct PrintContext {
    pub strip_ansi: bool,
}

fn write_colored(output: &mut String, context: &PrintContext, color: &str, text: &str) {
    if !context.strip_ansi {
        output.push_str(color);
    }
    output.push_str(text);
    if !context.strip_ansi {
        output.push_str("\x1b[0m");
    }
}

pub fn print_value(output: &mut String, context: &PrintContext, value: Value) {
    if value.is_empty() {
        write_colored(output, context, "\x1b[34;1m", "<empty>");
        return;
    }
    if value.is_number() {
        let number = value.as_f64();
        let text = if number == 0.0 && number.is_sign_negative() {
            "-0".to_string()
        } else {
            number_to_string(number)
        };
        write_colored(output, context, "\x1b[35;1m", &text);
        return;
    }
    if value.is_boolean() {
        write_colored(
            output,
            context,
            "\x1b[33;1m",
            if value.as_bool() { "true" } else { "false" },
        );
        return;
    }
    if value.is_null() {
        write_colored(output, context, "\x1b[33;1m", "null");
        return;
    }
    if value.is_undefined() {
        write_colored(output, context, "\x1b[34;1m", "undefined");
        return;
    }
    crate::interpreter::runtime_functions::unimplemented_runtime_function("printing cells", 0);
}
